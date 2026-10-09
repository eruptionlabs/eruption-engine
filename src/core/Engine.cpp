#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "renderer/PostFormat.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "core/Input.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TelemetryExporter.hpp"
#include "utils/EtexLoader.hpp"
#include "utils/MeshSubdivide.hpp"
#include "utils/ImGuiEx.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "utils/PbrTextureLoader.hpp"
#include <stb_image.h>
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/skymap/SkySystem.hpp"
#include "renderer/skymap/SkyConfig.hpp"
#include "renderer/WeatherTypes.hpp"
#include <cfloat>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numeric>
#include "renderer/SplineEditorUI.hpp"
#include "renderer/SpritePickerUI.hpp"
#include "formats/MapLoader.hpp"
#include "formats/ModelDataConverter.hpp"
#include "formats/SpriteTypes.hpp"

#include <imgui.h>
// So' pra ERUPTION_DEBUG_GIZMOWIN (diagnostico pontual da janela "gizmo" do
// ImGuizmo::BeginFrame - ver o comentario la'). API interna do ImGui, usada
// so' de leitura (ImGui::FindWindowByName + ImGuiWindow::Pos/Size).
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <unordered_map>
#include <filesystem>
#include <cctype>

#include "utils/ImageUtils.hpp"
#include "utils/TextureCache.hpp"
#include <vk_mem_alloc.h>

// Nomes das 12 fases de CPU marcadas dentro de Engine::render(). Eram locais do
// relatorio de benchmark; viraram constante porque a telemetria por frame passou
// a exportar a mesma quebra - sem ela, render_ms e' um numero cego, e foi
// exatamente isso que escondeu que o gargalo real e' CPU, nao GPU.
// (movido para core/EngineInternal.hpp - o painel F3 tambem usa)

namespace eruption {

// (PendingMesh/PendingUpload e as filas foram para core/EngineInternal.hpp)

// (appendArtificialTestLights foi para core/EngineInternal.hpp - o swap de mapa tambem usa)

// Helper: get water level/height from modern terrain water planes or source map descriptor.
// Returns true if the values came from modern terrain water planes.
// Note: terrain water type==0 is valid (texture "water0"), not "no water".
// When force=true and map has no water info, returns a default level (0) so
// the "Force Water" debug toggle works even on maps without water planes.
// (movido para core/EngineInternal.hpp - Render.cpp tambem usa)

// When the sampled terrain is below water level, spawn the player/camera on the
// water surface so the sprite doesn't disappear under water (e.g. instancia-A center).
// (movido para core/EngineInternal.hpp - o swap de mapa tambem usa)

// 1234567 -> "1.234.567". So' para leitura humana no F3: contagem de triangulo
// sem separador e' facil de ler errado por uma ordem de grandeza.

void Engine::GlobalAssetCache::cleanupOldTextures(VulkanContext* ctx, BindlessDescriptor* bindless, uint32_t maxAge) {
    std::lock_guard<std::mutex> lock(mutex);
    size_t initialModelSize = modelTextures.size();
    size_t initialTerrainSize = terrainTextures.size();
    size_t initialMeshSize = modelMeshes.size();
    size_t bytesFreed = 0;
    
    for (auto it = modelTextures.begin(); it != modelTextures.end(); ) {
        if (currentWarpId >= it->second.lastSeenWarpId + maxAge) {
            auto& tex = it->second.tex;
            if (tex.image != VK_NULL_HANDLE) {
                VmaAllocationInfo tInfo;
                vmaGetAllocationInfo(ctx->allocator(), tex.alloc, &tInfo);
                bytesFreed += tInfo.size;
                PROFILE_VRAM_FREE(ProfilerCategory::Textures, tInfo.size);
            }
            destroyTexture(tex, ctx, bindless);
            it = modelTextures.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = terrainTextures.begin(); it != terrainTextures.end(); ) {
        if (currentWarpId >= it->second.lastSeenWarpId + maxAge) {
            auto& tex = it->second.tex;
            if (tex.image != VK_NULL_HANDLE) {
                VmaAllocationInfo tInfo;
                vmaGetAllocationInfo(ctx->allocator(), tex.alloc, &tInfo);
                bytesFreed += tInfo.size;
                PROFILE_VRAM_FREE(ProfilerCategory::Textures, tInfo.size);
            }
            if (tex.pbrImage != VK_NULL_HANDLE) {
                VmaAllocationInfo tInfo;
                vmaGetAllocationInfo(ctx->allocator(), tex.pbrAlloc, &tInfo);
                bytesFreed += tInfo.size;
                PROFILE_VRAM_FREE(ProfilerCategory::Textures, tInfo.size);
            }
            if (tex.normalImage != VK_NULL_HANDLE) {
                VmaAllocationInfo tInfo;
                vmaGetAllocationInfo(ctx->allocator(), tex.normalAlloc, &tInfo);
                bytesFreed += tInfo.size;
                PROFILE_VRAM_FREE(ProfilerCategory::Textures, tInfo.size);
            }
            destroyTexture(tex, ctx, bindless);
            it = terrainTextures.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = modelMeshes.begin(); it != modelMeshes.end(); ) {
        if (currentWarpId >= it->second.lastSeenWarpId + maxAge) {
            auto& mesh = it->second.mesh;
            // Mesma lista unica do shutdown do cache - ver Engine.hpp. A copia
            // a mao que ficava aqui liberava 4 dos 9 buffers: os de LOD e
            // sombra vazavam a CADA WARP, nao so' no fechamento.
            bytesFreed += mesh.destroyBuffers(ctx);
            it = modelMeshes.erase(it);
        } else {
            ++it;
        }
    }
    
    if (initialModelSize != modelTextures.size() || initialTerrainSize != terrainTextures.size() || initialMeshSize != modelMeshes.size()) {
        ERUPTION_LOG_INFO("Engine: LRU Cache cleaned old resources (Freed %.2f MB). ModelTex: %zu -> %zu | TerrainTex: %zu -> %zu | Meshes: %zu -> %zu",
            bytesFreed / (1024.0f * 1024.0f),
            initialModelSize, modelTextures.size(),
            initialTerrainSize, terrainTextures.size(),
            initialMeshSize, modelMeshes.size());
    }
}

Engine::Engine() {
    m_postSettings.showCloudCoverageDebug = false;
    if (const char* e = std::getenv("ERUPTION_TEST_NORMAL_SCALE")) m_normalMapScale = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_NORMAL_INVERT_Y")) m_normalMapInvertY = std::stof(e) != 0.0f;
    if (const char* e = std::getenv("ERUPTION_TEST_DEFAULT_ROUGHNESS")) m_defaultRoughness = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_DEFAULT_METALLIC")) m_defaultMetallic = std::stof(e);
    // Headless overrides for the Lighting-tab controls: the same members the
    // sliders edit, so an A/B screenshot pair proves the sliders' data path.
    if (const char* e = std::getenv("ERUPTION_TEST_SSAO")) m_ssaoEnabled = std::stof(e) != 0.0f;
    if (const char* e = std::getenv("ERUPTION_TEST_SSAO_STRENGTH")) m_ssaoStrength = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_SSAO_RADIUS")) m_ssaoRadius = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_ENV_SPEC")) m_envSpecEnabled = std::stof(e) != 0.0f;
    if (const char* e = std::getenv("ERUPTION_TEST_SUN_BOUNCE")) m_sunBounceEnabled = std::stof(e) != 0.0f;
    if (const char* e = std::getenv("ERUPTION_TEST_CONTACT_SHADOW")) m_contactShadowEnabled = std::stof(e) != 0.0f;
    if (const char* e = std::getenv("ERUPTION_TEST_MOON_FILL")) m_moonFillEnabled = std::stof(e) != 0.0f;
    if (const char* e = std::getenv("ERUPTION_TEST_AUTO_EXPOSURE")) m_autoExposureEnabled = std::stof(e) != 0.0f;
    if (const char* e = std::getenv("ERUPTION_TEST_AMBIENT_INTENSITY")) m_ambientIntensity = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_NIGHT_AO")) m_nightAoContrast = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_TESS_AUDIT")) m_tessDebugMode = std::atoi(e);
}
Engine::~Engine() { shutdown(); }

bool Engine::init(int width, int height, const std::string& title) {
    Logger::init();
    
    if (!m_window.init(width, height, title)) return false;
    if (m_editorMode) m_window.maximize();

    width = m_window.width();
    height = m_window.height();

    m_window.setResizeCallback([this](int w, int h) { onWindowResize(w, h); });
    if (std::getenv("ERUPTION_PREFER_IMMEDIATE") != nullptr) {
        m_vulkan.preferImmediatePresentMode(true);
    }
    if (!m_vulkan.init(m_window.handle())) return false;

    // Load PBR material profiles if the agy-generated file exists.
    // This is done early so every renderer can query profiles during map load.
    PbrMaterialProfileManager::instance().loadFromFile("assets/data/pbr_materials.json");

    Input::init(m_window.handle());

    // Create samplers EARLY because ImGui and splash logos need them
    VkSamplerCreateInfo samplerInfo{}; samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(m_vulkan.device(), &samplerInfo, nullptr, &m_defaultSampler);

    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(m_vulkan.device(), &samplerInfo, nullptr, &m_nearestSampler);

    initImGui();
    initSplashLogo();

    // Force progress to 0 initially
    presentLoadingScreen("", "Initializing Engine", 0.0f, true);

    if (!m_bindless.init(&m_vulkan)) return false;
    ERUPTION_LOG_WARN("Resolucao de RENDER (G-buffer): %dx%d (%.2f Mpx)", width, height,
                      width * height / 1.0e6);
    {
        // RENDER SCALE: a cena inteira (G-buffer, iluminacao, post) e' desenhada
        // nesta fracao da resolucao da janela e o blit final sobe a imagem com
        // filtro linear. Alcanca TODOS os passes que escalam com pixels -
        // inclusive Deferred Lighting e Cloud Shadows, que o post_scale nao
        // pegava. Custa nitidez, entao so' o preset low (alvo 930M) usa.
        float rs = 1.0f;
        if (const char* e = std::getenv("ERUPTION_RENDER_SCALE")) {
            rs = std::clamp(std::strtof(e, nullptr), 0.25f, 1.0f);
        } else {
            try {
                std::ifstream rf("data/graphics.json");
                if (rf) {
                    nlohmann::json gj; rf >> gj;
                    std::string pr = gj.value("preset", "high");
                    if (const char* pe = std::getenv("ERUPTION_TEST_GRAPHICS_PRESET")) pr = pe;
                    if (gj.contains("presets") && gj["presets"].contains(pr)) {
                        rs = std::clamp<float>(gj["presets"][pr].value("render_scale", 1.0f), 0.25f, 1.0f);
                        // Formato dos alvos de post: "packed" = B10G11R11 (metade
                        // da banda, ver PostFormat.hpp). Lido aqui porque tem que
                        // valer ANTES de PostProcessor::init.
                        postPackedPreference() =
                            (gj["presets"][pr].value("post_format", std::string("rgba16f")) == "packed");
                    }
                }
            } catch (...) { rs = 1.0f; }
        }
        if (rs < 0.999f) {
            width  = std::max(64, static_cast<int>(width * rs));
            height = std::max(64, static_cast<int>(height * rs));
            ERUPTION_LOG_WARN("Render scale %.2f: cena em %dx%d", rs, width, height);
        }
        m_renderW = static_cast<uint32_t>(width);
        m_renderH = static_cast<uint32_t>(height);
        m_displayW = m_vulkan.swapExtent().width;
    }
    // Upscaler: env > preset > FXAA. Decidido ANTES do G-buffer: a velocidade de
    // objeto (6o alvo) depende dele. Com FSR o post_scale e' ignorado (abaixo).
    {
        std::string mode = "fxaa";
        bool objectVelocity = true;
        try {
            std::ifstream pf("data/graphics.json");
            if (pf) {
                nlohmann::json gj; pf >> gj;
                std::string pr = gj.value("preset", "high");
                if (const char* pe = std::getenv("ERUPTION_TEST_GRAPHICS_PRESET")) pr = pe;
                if (gj.contains("presets") && gj["presets"].contains(pr)) {
                    mode = gj["presets"][pr].value("upscaler", mode);
                    m_fsrSharpness = gj["presets"][pr].value("fsr_sharpness", m_fsrSharpness);
                    // Velocidade de objeto (vegetacao com vento): ligada por
                    // padrao, o preset pode desligar (low/930M: custa banda).
                    objectVelocity = gj["presets"][pr].value("fsr_object_velocity", objectVelocity);
                }
            }
        } catch (...) {}
        if (const char* e = std::getenv("ERUPTION_UPSCALER")) mode = e;
        if (const char* e = std::getenv("ERUPTION_FSR_SHARPNESS")) m_fsrSharpness = std::strtof(e, nullptr);
        m_upscalerMode = mode == "fsr" ? UpscalerMode::FsrBeforePost
                       : mode == "fsr_post" ? UpscalerMode::FsrAfterPost
                       : UpscalerMode::Fxaa;
        if (fsrActive() && !Fsr3Upscaler::supported(&m_vulkan)) {
            ERUPTION_LOG_WARN("FSR pedido mas o device nao suporta: voltando ao FXAA");
            m_upscalerMode = UpscalerMode::Fxaa;
        }
        if (const char* e = std::getenv("ERUPTION_FSR_OBJECT_VELOCITY")) objectVelocity = e[0] == '1';
        GBuffer::setVelocityEnabled(fsrActive() && objectVelocity);
    }
    if (!m_gbuffer.init(&m_vulkan, width, height)) return false;
    if (!m_cameraMotion.init(&m_vulkan, width, height)) return false;
    m_cameraMotion.bindDepth(m_gbuffer.depthView());
    m_cameraMotion.bindObjectVelocity(m_gbuffer.velocityView());
    if (!m_deferredLighting.init(&m_vulkan, &m_gbuffer, &m_bindless, width, height)) return false;
    if (std::getenv("ERUPTION_TEST_FORCE_LEGACY") != nullptr) {
        m_deferredLighting.setUsePbr(false);
    }
    // ERUPTION_TEST_PLAYER_LIGHT=1 (debug): liga a luz presa ao sprite (o
    // mesmo que apertar F10) sem display - e' como o teste headless prova que
    // a point light do jogador ilumina o mundo.
    // Aceita "1" ou "intensidade,raio" (ex.: "6,60") pra' variar a esfera da
    // luz sem recompilar - e' como se testa a camera DENTRO vs FORA do volume.
    if (const char* e = std::getenv("ERUPTION_TEST_PLAYER_LIGHT")) {
        m_playerLight.enabled = true;
        float inten = 0.0f, rad = 0.0f;
        if (std::sscanf(e, "%f,%f", &inten, &rad) == 2) {
            m_playerLight.intensity = inten;
            m_playerLight.radius = rad;
        }
    }
    {
        const char* pbrDebug = std::getenv("ERUPTION_TEST_PBR_DEBUG");
        if (pbrDebug) {
            std::string mode(pbrDebug);
            uint32_t mask = 0;
            auto addIfContains = [&](const std::string& token, uint32_t bit) {
                if (mode.find(token) != std::string::npos) mask |= bit;
            };
            if (mode == "all") {
                mask = 1u | 2u | 4u | 8u | 16u;
            } else {
                addIfContains("roughness", 1u);
                addIfContains("metallic", 2u);
                addIfContains("normal", 4u);
                addIfContains("wetness", 8u);
                addIfContains("source", 16u);
            }
            m_deferredLighting.setPbrDebugMode(mask);
        }
    }

    if (std::getenv("ERUPTION_TEST_AB_CAPTURE") != nullptr) {
        m_abCaptureStep = 1;
        m_abCaptureFrame = 2;
    }
    // POST SCALE: so' a cadeia de post-process roda nesta fracao da resolucao da
    // janela; G-buffer e iluminacao ficam em resolucao cheia, e o blit final
    // sobe a imagem com filtro linear. Os passes fullscreen sao ~70% do frame
    // num mapa classico e o custo deles e' proporcional a pixels, entao este e'
    // o knob que decide hardware fraco. Custa NITIDEZ - por isso 1.0 (desligado)
    // nos presets high/medium, e so' o low, que mira a 930M, paga o preco.
    m_postScale = 1.0f;
    if (const char* e = std::getenv("ERUPTION_POST_SCALE")) {
        m_postScale = std::clamp(std::strtof(e, nullptr), 0.25f, 1.0f);
    } else {
        try {
            std::ifstream pf("data/graphics.json");
            if (pf) {
                nlohmann::json gj; pf >> gj;
                // Respeita o override de preset dos testes: sem isto o
                // post_scale sempre vinha do preset gravado no JSON, e
                // ERUPTION_TEST_GRAPHICS_PRESET=low nao mudava nada.
                std::string pr = gj.value("preset", "high");
                if (const char* pe = std::getenv("ERUPTION_TEST_GRAPHICS_PRESET")) pr = pe;
                if (gj.contains("presets") && gj["presets"].contains(pr))
                    m_postScale = std::clamp<float>(gj["presets"][pr].value("post_scale", 1.0f), 0.25f, 1.0f);
            }
        } catch (...) { m_postScale = 1.0f; }
    }
    if (fsrActive()) m_postScale = 1.0f; // o FSR resolve a escala sozinho
    const VkExtent2D displayExt = m_vulkan.swapExtent();
    uint32_t postW = std::max(64u, static_cast<uint32_t>(width * m_postScale));
    uint32_t postH = std::max(64u, static_cast<uint32_t>(height * m_postScale));
    if (m_upscalerMode == UpscalerMode::FsrBeforePost) { postW = displayExt.width; postH = displayExt.height; }
    if (!m_postProcessor.init(&m_vulkan, postW, postH)) return false;
    m_postProcessor.setWeatherSystem(&m_weatherSystem);
    if (fsrActive()) {
        if (!m_fsr.init(&m_vulkan, m_upscalerMode == UpscalerMode::FsrBeforePost,
                        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                        displayExt.width, displayExt.height)) {
            ERUPTION_LOG_WARN("FSR falhou ao iniciar: voltando ao FXAA");
            m_upscalerMode = UpscalerMode::Fxaa;
            m_postProcessor.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        } else {
            // Vies de mip (recomendacao da AMD: log2(render/display) - 1). O
            // "- 1" e' configuravel: mais negativo = textura mais nitida e mais
            // alias para o FSR resolver - o trade-off que estamos medindo.
            float extra = -1.0f;
            if (const char* e = std::getenv("ERUPTION_FSR_MIP_BIAS_EXTRA")) extra = std::strtof(e, nullptr);
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(m_vulkan.physicalDevice(), &props);
            const float maxBias = props.limits.maxSamplerLodBias;
            const float bias = std::clamp(std::log2(float(width) / float(displayExt.width)) + extra, -maxBias, maxBias);
            m_vulkan.setTextureLodBias(bias);
            // Os samplers padrao ja' foram criados (ImGui/splash os usam e
            // seguem com os antigos, vivos ate' o shutdown); as texturas de cena
            // registradas daqui em diante pegam os novos, com vies.
            m_uiDefaultSampler = m_defaultSampler;
            m_uiNearestSampler = m_nearestSampler;
            VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter = si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = 16.0f;
            si.mipLodBias = bias;
            si.minLod = 0.0f;
            si.maxLod = VK_LOD_CLAMP_NONE;
            vkCreateSampler(m_vulkan.device(), &si, nullptr, &m_defaultSampler);
            si.magFilter = si.minFilter = VK_FILTER_NEAREST;
            vkCreateSampler(m_vulkan.device(), &si, nullptr, &m_nearestSampler);
            ERUPTION_LOG_WARN("FSR 3.1 %s: %dx%d -> %ux%u, vies de mip %.2f",
                              m_upscalerMode == UpscalerMode::FsrBeforePost ? "antes do pos (HDR)" : "depois do pos (LDR)",
                              width, height, displayExt.width, displayExt.height, bias);
        }
    }
    // FXAA + upscale bicubico/nitidez: substitui o blit bilinear que levava a
    // saida do PostProcessor (resolucao de RENDER) pro swapchain (resolucao
    // de DISPLAY). Mesma resolucao/formato que o PostProcessor usa, pra ler
    // a saida dele sem conversao.
    if (m_upscalerMode == UpscalerMode::FsrAfterPost) {
        // A entrada do UpscaleAA vira a saida do FSR (display, RGBA16F).
        if (!m_upscaleAA.init(&m_vulkan, m_fsr.displayWidth(), m_fsr.displayHeight(),
                              Fsr3Upscaler::kOutputFormat)) return false;
        m_upscaleAA.bindSource(m_fsr.outputView());
    } else {
        if (!m_upscaleAA.init(&m_vulkan, m_postProcessor.width(), m_postProcessor.height(),
                              postColorFormat(m_vulkan.physicalDevice()))) return false;
        m_upscaleAA.bindSource(m_postProcessor.outputView());
    }
    if (fsrActive()) bindFsrInputs();
    if (!initShimmerMetric()) return false;

    presentLoadingScreen("", "Loading Renderers", 0.15f, true);
    m_mipmapMenu.loadConfig();
    m_waterMenu.loadConfig();
    if (!m_terrainRenderer.init(&m_vulkan, &m_bindless)) return false;
    m_terrainRenderer.setMipmapsEnabled(m_mipmapMenu.config.enabled, m_mipmapMenu.config.mode);
    if (!m_modelRenderer.init(&m_vulkan, &m_bindless)) return false;
    m_modelRenderer.setMipmapsEnabled(m_mipmapMenu.config.enabled);
    m_modelRenderer.setTextureResolver([this](const std::string& path) {
        return this->resolveModelTexture(path);
    });
    // Lets the PBR synthesis reach albedos that exist only inside a GLB.
    // Without it, every embedded-texture material renders with flat fallback
    // constants (parana_field: ~90% of its materials).
    setEmbeddedAlbedoProvider([this](const std::string& name, std::vector<uint8_t>& px,
                                     int& w, int& h, int& ch) {
        return this->findEmbeddedAlbedoPixels(name, px, w, h, ch);
    });
    // Par PBR já sintetizado E comprimido em bloco durante o bake do GLB.
    // Entra exatamente no lugar onde a síntese em runtime entraria - depois do
    // PBR cozido em disco - para não substituir mapa autoral por derivado.
    setBakedPbrProvider([this](const std::string& key, PbrTextureData& mrahw,
                               PbrTextureData& normal) {
        if (m_embeddedBaked.empty()) return false;
        auto it = m_embeddedBaked.find(key);
        if (it == m_embeddedBaked.end()) {
            // O renderer normaliza '/'->'\\' e tira a barra inicial; o nome
            // guardado é o da imagem dentro do GLB.
            std::string alt = key;
            std::replace(alt.begin(), alt.end(), '\\', '/');
            it = m_embeddedBaked.find(alt);
            if (it == m_embeddedBaked.end()) return false;
        }
        if (!it->second.mrahw.valid() || !it->second.normal.valid()) return false;
        mrahw.etex = it->second.mrahw;
        mrahw.width = static_cast<int>(mrahw.etex.width);
        mrahw.height = static_cast<int>(mrahw.etex.height);
        mrahw.channels = 4;
        normal.etex = it->second.normal;
        normal.width = static_cast<int>(normal.etex.width);
        normal.height = static_cast<int>(normal.etex.height);
        normal.channels = 4;
        return true;
    });
    m_modelRenderer.setMeshResolver([this](const std::string& key,
                                            const std::vector<TerrainVertex>& vertices,
                                            const std::vector<uint32_t>& indices) {
        size_t hashPos = key.find_last_of('#');
        std::string legacyModelPath = key.substr(0, hashPos);
        uint32_t nodeIdx = 0;
        if (hashPos != std::string::npos) {
            nodeIdx = static_cast<uint32_t>(std::stoul(key.substr(hashPos + 1)));
        }
        return this->resolveModelMesh(legacyModelPath, nodeIdx, vertices, indices);
    });
    if (!m_spriteRenderer.init(&m_vulkan, &m_gbuffer, &m_bindless)) return false;
    if (fsrActive()) {
        const char* sl = std::getenv("ERUPTION_SPRITE_LAYER");
        if (!(sl && sl[0] == '0')) {
            m_spriteLayerActive = m_spriteLayer.init(&m_vulkan, &m_spriteRenderer,
                                                     static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            if (!m_spriteLayerActive) ERUPTION_LOG_WARN("Camada de sprites indisponivel: sprites ficam so' no G-buffer");
        }
        bindFsrInputs(); // agora com a mascara reativa
    }
    // Share SpriteRenderer's per-frame UBO with model/terrain G-Buffer pipelines
    // so their fragment shaders can access camera position, time and POM params.
    m_modelRenderer.setFrameUboSet(m_spriteRenderer.frameUboLayout(), m_spriteRenderer.frameUboSet());
    m_terrainRenderer.setFrameUboSet(m_spriteRenderer.frameUboLayout(), m_spriteRenderer.frameUboSet());
    m_spriteSystem.init(&m_vulkan, &m_spriteRenderer);
    // Popup de preview de textura em esfera (F1 -> Object Manager ->
    // Textures) - ver debug/SpherePreview.hpp. Mesmo FrameUBO compartilhado
    // acima, so' que aqui e' pra' pegar sol/tempo pro fragment PROPRIO do
    // popup (nao model.frag, que escreve no G-buffer da cena principal).
    if (!m_spherePreview.init(&m_vulkan, &m_bindless, m_spriteRenderer.frameUboLayout(), m_spriteRenderer.frameUboSet())) {
        ERUPTION_LOG_WARN("[SPHEREPREVIEW] falhou ao inicializar - popup de preview ficara indisponivel.");
    }

    presentLoadingScreen("", "Loading Configuration", 0.3f, true);
    // Shadow settings load BEFORE ShadowRenderer::init so atlas_size from
    // data/shadows.json decides the atlas allocation.
    m_shadowConfig.load("data/shadows.json");
    m_shadowRenderer.settings().loadFromJson(m_shadowConfig.root());
    if (!m_shadowRenderer.init(&m_vulkan, &m_bindless)) return false;

    m_postConfig.load("data/postprocess.json");
    m_postSettings.loadFromJson(m_postConfig.root());

    m_graphicsConfig.load("data/graphics.json");
    // Quais formatos de bloco esta GPU sabe AMOSTRAR. Maxwell (930M, o alvo)
    // tem BC1/BC3/BC5/BC7; Android/ARM não tem nenhum. Sem isso a engine
    // criaria uma imagem que o driver rejeita - o fallback RGBA8 depende
    // desta sondagem estar correta.
    {
        BcFormatSupport sup;
        auto probe = [&](VkFormat f) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(m_vulkan.physicalDevice(), f, &props);
            return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        };
        sup.bc1 = probe(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        sup.bc3 = probe(VK_FORMAT_BC3_UNORM_BLOCK);
        sup.bc5 = probe(VK_FORMAT_BC5_UNORM_BLOCK);
        sup.bc7 = probe(VK_FORMAT_BC7_UNORM_BLOCK);
        // Escape hatch de A/B e de suporte: ERUPTION_NO_TEXTURE_BC=1 desliga a
        // compressao de bloco e exercita o caminho RGBA8 puro (o mesmo que uma
        // GPU sem BC pega). Sem isso nao da' pra medir a regressao visual do
        // codec no mesmo binario.
        if (const char* e = std::getenv("ERUPTION_NO_TEXTURE_BC")) {
            if (e[0] == '1') sup = BcFormatSupport{};
        }
        setBcFormatSupport(sup);
        ERUPTION_LOG_WARN("[GRAPHICS] compressao de bloco: BC1=%d BC3=%d BC5=%d BC7=%d",
                          sup.bc1, sup.bc3, sup.bc5, sup.bc7);
    }
    applyGraphicsPreset();
    // Respect postprocess.json for DoF/Tilt-Shift; users can toggle it via the
    // effects UI or config file instead of having it forced on at startup.
    // m_postSettings.enableDoF = true;

    m_dayNightConfig.load("data/daynight.json");
    m_dayNightCycle.init(600.0f);
    m_dayNightCycle.loadFromJson(m_dayNightConfig.root());

    // ERUPTION_TEST_TIME=<0..1> (debug): pin the time of day (and freeze it) for
    // reproducible captures. 14:50 = (14 + 50/60) / 24 ~= 0.618.
    if (const char* e = std::getenv("ERUPTION_TEST_TIME")) {
        m_dayNightCycle.setTimeOfDay(std::stof(e));
        m_dayNightCycle.setTimeScale(0.0f);
    }

    m_skyboxConfig.load("data/skybox_config.json");
    SkyConfig skyConfig;
    skyConfig.loadFromJson(m_skyboxConfig.root());
    {
        float yawSnaps[8];
        float timeSnaps[8];
        for (uint32_t i = 0; i < m_dayNightCycle.getKeyframeCount(); i++) {
            timeSnaps[i] = m_dayNightCycle.getKeyframes()[i].timeOfDay;
            yawSnaps[i] = DayNightCycle::getSunYaw(timeSnaps[i]);
        }
        m_shadowRenderer.setShadowSnaps(yawSnaps, timeSnaps, m_dayNightCycle.getKeyframeCount());
    }

    m_dioramaConfig.load("data/diorama.json");

    // CLIMA. Dois arquivos com responsabilidades separadas:
    //   climate_types.json  ONDE cada clima vive no espaco atmosferico
    //                       (temperatura x umidade x estabilidade x vento).
    //                       Ao carregar, valida e AVISA no log se dois climas
    //                       tem nucleo sobreposto - dois climas 100% certos no
    //                       mesmo estado seriam um empate arbitrario.
    //   weather_types.json  COMO cada clima se parece. So' sobrepoe o que
    //                       declara; tipo ausente mantem o template de C++.
    // Os dois releem sozinhos ~1x/s (hot-reload).
    m_weatherSystem.climateTypes().loadFromFile("data/climate_types.json");
    m_weatherSystem.loadTypeOverrides("data/weather_types.json");
    if (m_dioramaConfig.root().contains("default_preset") && m_dioramaConfig.root().contains("presets")) {
        std::string presetName = m_dioramaConfig.root()["default_preset"];
        const auto& presets = m_dioramaConfig.root()["presets"];
        if (presets.contains(presetName)) {
            m_lookConfig.loadFromJson(presets[presetName]);
            m_lookConfig.name = presetName;
        } else {
            m_lookConfig = DioramaLookConfig::fieldLook();
        }
    } else {
        m_lookConfig = DioramaLookConfig::fieldLook();
    }

    m_charConfig.load("data/char_config.json");
    m_inputConfig.load("data/input_config.json");

    presentLoadingScreen("", "Initializing Additional Effects", 0.6f, true);
    if (!m_skybox.init(&m_vulkan, width, height)) return false;
    m_skybox.loadConfig(skyConfig);
    // SONDA DE CEU (G36): cubemap do ceu -> SH9 + mips para o ambiente.
    if (!m_skyProbe.init(&m_vulkan, 32)) return false;
    m_deferredLighting.setSkyProbe(m_skyProbe.cubeView(), m_skyProbe.sampler(), m_skyProbe.shBuffer(),
                                   m_skyProbe.shBufferSize(), m_skyProbe.mipCount());
    // PROBES DE IRRADIANCIA (G38): fallback 1x1x1 ate' o primeiro mapa bakear.
    if (!m_irradianceProbes.init(&m_vulkan)) return false;
    m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                           Vec3(0.0f), Vec3(0.0f), false);

    // Cloud Layers (replaces CloudFluff).
    {
        // O arquivo so' existe depois que a UI salva uma vez: num clone novo
        // a raiz vem nula e value() estourava excecao. Sem arquivo = padroes.
        m_cloudLayerConfig.load("data/cloud_layer_config.json");
        static const nlohmann::json kNoCloudConfig = nlohmann::json::object();
        const auto& j = m_cloudLayerConfig.root().is_object() ? m_cloudLayerConfig.root() : kNoCloudConfig;

        CloudLayerRenderer::Config cloudCfg{};
        cloudCfg.enabled = j.value("enabled", true);
        cloudCfg.layerCount = j.value("layerCount", 1u);
        cloudCfg.coverageSize = j.value("coverageSize", 1024u);
        cloudCfg.cloudBottom = j.value("cloudBottom", 5000.0f);
        cloudCfg.cloudThickness = j.value("cloudThickness", 600.0f);
        cloudCfg.coverageScale = j.value("coverageScale", 0.001f);
        cloudCfg.noiseScale = j.value("noiseScale", 0.004f);
        cloudCfg.detailStrength = j.value("detailStrength", 0.5f);
        cloudCfg.windSpeed = j.value("windSpeed", 0.25f);
        if (j.contains("windDirection") && j["windDirection"].is_array() && j["windDirection"].size() >= 3) {
            cloudCfg.windDirection = Vec3(j["windDirection"][0].get<float>(),
                                          j["windDirection"][1].get<float>(),
                                          j["windDirection"][2].get<float>());
        } else {
            cloudCfg.windDirection = Vec3(1.0f, 0.0f, -0.5f);
        }
        cloudCfg.godRayMode = static_cast<CloudLayerRenderer::GodRayMode>(j.value("godRayMode", 2u));
        cloudCfg.godRaySamples = j.value("godRaySamples", 48u);
        cloudCfg.godRayIntensity = j.value("godRayIntensity", 0.4f);
        cloudCfg.castShadows = j.value("castShadows", false);
        cloudCfg.shadowMapSize = j.value("shadowMapSize", 1024u);
        cloudCfg.temporalReprojection = j.value("temporalReprojection", false);
        cloudCfg.maxSteps = j.value("maxSteps", 48u);
        cloudCfg.stepSize = j.value("stepSize", 60.0f);
        cloudCfg.debugMode = j.value("debugMode", 2u);
        cloudCfg.debugShowMissColor = j.value("debugShowMissColor", false);
        cloudCfg.showCloudShadows = j.value("showCloudShadows", true);
        cloudCfg.cloudShadowOpacity = j.value("cloudShadowOpacity", 1.0f); // trim; base strength = sun/(sun+ambient)
        cloudCfg.cloudShadowDistanceFade = j.value("cloudShadowDistanceFade", 2000.0f);
        cloudCfg.debugPlaneY = j.value("debugPlaneY", 800.0f);
        cloudCfg.cloudCoverage = j.value("cloudCoverage", 0.5f);
        cloudCfg.cloudAmount = j.value("cloudAmount", 0.5f);
        cloudCfg.cloudEdgeSoftness = j.value("cloudEdgeSoftness", 0.15f);
        cloudCfg.cloudRayMarchSteps = j.value("cloudRayMarchSteps", 250.0f);
        cloudCfg.cloudDebugThickness = j.value("cloudDebugThickness", 50.0f);
        cloudCfg.cloudSpikeHeight = j.value("cloudSpikeHeight", 200.0f);
        cloudCfg.cloudBaseDepth = j.value("cloudBaseDepth", 50.0f);
        cloudCfg.cloudSpikeWidth = j.value("cloudSpikeWidth", 300.0f);

        m_cloudLayersEnabled = cloudCfg.enabled;
        if (!m_cloudLayerRenderer.init(&m_vulkan, width, height, cloudCfg)) return false;
        // cloudAmount thresholds the coverage texture on the CPU; the shaders read
        // the thresholded result directly. Regeneration is triggered from the UI
        // when the slider is released to avoid per-frame stalls.
        m_postProcessor.weatherRenderer().setCloudCoverageMap(&m_cloudLayerRenderer.coverageArray());
        m_postProcessor.weatherRenderer().setCloudCoverageTexture(m_cloudLayerRenderer.coverageArray().imageView(),
                                                                   m_cloudLayerRenderer.coverageArray().sampler());
        m_postProcessor.weatherRenderer().setRainBoxMoveDirection(cloudCfg.windDirection);
        // The authored cloud heading is the PREVAILING wind. Handing it to the
        // WindField makes it the single source of truth: clouds, rain slant and
        // vegetation then all agree on which way the weather is moving, instead
        // of each carrying its own unrelated "wind" (G6).
        m_weatherSystem.setPrevailingWindDirection(cloudCfg.windDirection);
    }

    // Default weather is Clear; local clouds can be spawned manually for rain tests.
    m_weatherSystem.applyType(WeatherType::Clear, 1.0f);
    m_weatherSystem.snapToTarget();
    {
        // Drive the cloud-layer look from the Clear template at boot (same as
        // the weather UI does on every weather change). Without this, values
        // persisted in cloud_layer_config.json by a previous UI session
        // (cloudAmount, thickness, windSpeed) silently override the Clear look.
        const auto& w = m_weatherSystem.current();
        CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
        cloudCfg.cloudAmount = glm::clamp(w.cloudCoverage, 0.0f, 1.0f);
        cloudCfg.cloudScale = w.cloudScale;
        cloudCfg.cloudLightness = w.cloudLightness;
        cloudCfg.cloudShade = w.cloudShade;
        cloudCfg.cloudSoftness = w.cloudSoftness;
        cloudCfg.cloudThickness = 400.0f + w.cloudThickness * 1200.0f;
        cloudCfg.windSpeed = w.cloudSpeed * 10.0f;
        m_cloudLayerRenderer.setConfig(cloudCfg);
        m_cloudLayerRenderer.coverageArray().setCoverage(cloudCfg.cloudAmount);
        m_cloudLayerRenderer.coverageArray().setLayerSpacing(
            cloudCfg.cloudThickness / glm::max(1u, cloudCfg.layerCount - 1));
    }
    if (!m_water.init(&m_vulkan, &m_bindless)) return false;
    m_water.setSkyTexture(m_skybox.outputView(), m_skybox.outputSampler());
    if (!m_debugLineRenderer.init(&m_vulkan)) return false;
    if (!m_overlayLineRenderer.init(&m_vulkan)) return false;

    presentLoadingScreen("", "Scanning Game Assets", 0.8f, true);
    // Load resource config (resources.ini) - try multiple locations
    auto findDataIni = [&]() -> std::string {
        const char* candidates[] = {"resources.ini", "../resources.ini"};
        for (const char* path : candidates) {
            if (std::filesystem::exists(path)) return path;
        }
        return "";
    };
    std::string iniPath = findDataIni();
    if (iniPath.empty()) {
        m_resourceConfig.createDefault("resources.ini");
        iniPath = "resources.ini";
    }
    m_resourceConfig.load(iniPath);
    for (const auto& entry : m_resourceConfig.entries) {
        m_packManager.addDirectory(entry.path);
    }

    m_mapLoader = std::make_unique<MapLoader>();
    populateMaps();

    // Init background loading subsystem
    JobSystem::instance().init();
    
    BackgroundMapLoader::AssetCacheInterface cacheInterface;
    cacheInterface.hasModelTexture = [this](const std::string& path) {
        return m_assetCache.hasModelTexture(path);
    };
    cacheInterface.hasTerrainTexture = [this](const std::string& path) {
        return m_assetCache.hasTerrainTexture(path);
    };

    m_backgroundLoader = std::make_unique<BackgroundMapLoader>(cacheInterface);
    initMinimap();

    m_camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 50000.0f);
    // Default camera: 85% zoom, 50% pitch
    // zoomPercent=0.85 => distance = 10 + (1-0.85)*990 = 158.5
    // pitchPercent=0.50 => 10 + 0.50*(90-10) = 50 degrees
    m_camera.setOrbit(glm::radians(-90.0f), glm::radians(50.0f), 158.5f);
    m_camera.setDefaultOrbit(glm::radians(-90.0f), glm::radians(50.0f), 158.5f);

    {
        auto fog = std::make_unique<RenderEffect>("Fog", m_postSettings.enableFog);
        fog->setTooltip("Atmospheric depth fog that fades distant geometry into the sky color.");
        m_renderEffects.push_back(std::move(fog));
    }
    {
        auto gi = std::make_unique<GlobalIlluminationEffect>(0.25f);
        m_renderEffects.push_back(std::move(gi));
    }
    {
        // Nao e' mais placeholder: o bloom tem passe proprio, cronometrado
        // no F3 (Bloom Down / Bloom Up) desde 2026-09-03.
        // Nasce com o valor REAL do ajuste (data/postprocess.json), nao com
        // false fixo - a caixa aparecia desmarcada mesmo com o bloom ligado.
        auto bloom = std::make_unique<RenderEffect>("Bloom", m_postSettings.enableBloom);
        bloom->setTooltip("Simulates bright areas bleeding into the image (currently a placeholder).");
        m_renderEffects.push_back(std::move(bloom));
    }
    {
        auto dof = std::make_unique<RenderEffect>("Depth of Field", m_postSettings.enableDoF);
        dof->setTooltip("Depth-of-field or tilt-shift miniature effect on the final image.");
        // Test hook: ERUPTION_TEST_DOF=1 forces DoF on for headless benchmarks
        // (startup forces it off and the effects UI is the only other path).
        if (std::getenv("ERUPTION_TEST_DOF")) {
            dof->setEnabled(true);
            m_postSettings.enableDoF = true;
        }
        // ERUPTION_TEST_NO_DOF=1: forca DoF DESLIGADO independente de preset.
        // Regra dos testes de caminhada/cintilacao de sombra (pedido do autor
        // 2026-09-02): o desfoque do tilt-shift contamina o RMS entre frames
        // adjacentes e mascara exatamente o artefato que se quer medir.
        if (std::getenv("ERUPTION_TEST_NO_DOF")) {
            dof->setEnabled(false);
            m_postSettings.enableDoF = false;
        }
        m_renderEffects.push_back(std::move(dof));
    }
    {
        auto lights = std::make_unique<RenderEffect>("Point Lights", true);
        lights->setTooltip("Per-pixel point/spot lights in the deferred shading pass.");
        m_renderEffects.push_back(std::move(lights));
    }
    {
        bool shadowsEnabled = m_shadowRenderer.settings().enabled;
        auto shadows = std::make_unique<RenderEffect>("Shadows", shadowsEnabled);
        shadows->setTooltip("Cascaded shadow maps for the sun/moon and sprite shadows.");
        m_renderEffects.push_back(std::move(shadows));
    }
    {
        auto skybox = std::make_unique<RenderEffect>("Skybox IBL", true);
        skybox->setTooltip("Sky background and image-based lighting reflections.");
        m_renderEffects.push_back(std::move(skybox));
    }
    {
        auto water = std::make_unique<RenderEffect>("Water", true);
        water->setTooltip("Reflective/refractive water surfaces with specular highlights.");
        m_renderEffects.push_back(std::move(water));
    }
    {
        auto sprites = std::make_unique<RenderEffect>("Sprites", true);
        sprites->setTooltip("2D sprite rendering for characters, NPCs and billboards.");
        m_renderEffects.push_back(std::move(sprites));
    }

    // ERUPTION_TEST_NO_EFFECTS=1 (debug): desliga fog, DoF, bloom, ceu/IBL e
    // agua, mas MANTEM sombras e luzes. Para dumps de frame que isolam a
    // sombra (pedido do autor 2026-09-02: "desative os efeitos" ao validar
    // shimmering) - qualquer efeito temporal ou de tela cheia contaminaria a
    // diferenca entre frames adjacentes.
    // ERUPTION_TEST_NO_SHADOWS=1 (debug): so' as sombras. Par do NO_EFFECTS
    // pra isolar num dump de frames quanto do ruido temporal e' da sombra.
    if (std::getenv("ERUPTION_TEST_NO_SHADOWS")) {
        if (m_renderEffects.size() > 5) m_renderEffects[5]->setEnabled(false);
        m_shadowRenderer.settings().enabled = false;
    }
    if (std::getenv("ERUPTION_TEST_NO_EFFECTS")) {
        m_postSettings.enableDoF = false;
        m_postSettings.enableFog = false;
        m_postSettings.enableBloom = false;
        m_postSettings.enableChromaticAberration = false;
        m_postSettings.enableMotionBlur = false;
        if (m_renderEffects.size() > 0) m_renderEffects[0]->setEnabled(false); // Fog
        if (m_renderEffects.size() > 2) m_renderEffects[2]->setEnabled(false); // Bloom
        if (m_renderEffects.size() > 3) m_renderEffects[3]->setEnabled(false); // DoF
        if (m_renderEffects.size() > 6) m_renderEffects[6]->setEnabled(false); // Skybox IBL
        if (m_renderEffects.size() > 7) m_renderEffects[7]->setEnabled(false); // Water
    }

    m_initialized = true;
    m_running = true;

    // Active map context starts empty
    m_activeMapContext = std::make_unique<MapContext>();

    m_debugModelPivots = false;
    m_showDebugOverlay = true;

    return true;
}

void Engine::populateMaps() {
    namespace fs = std::filesystem;
    m_availableMaps.clear();

    auto scanDir = [&](const std::string& root) {
        if (!fs::exists(root) || !fs::is_directory(root)) return;
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory()) {
                // assets/external/<map>/<map>.glb convention
                std::string glb = (entry.path() / (entry.path().filename().string() + ".glb")).string();
                if (fs::exists(glb)) {
                    m_availableMaps.push_back(entry.path().filename().string());
                }
            } else if (entry.is_regular_file()) {
                std::string name = entry.path().filename().string();
                if (name.size() >= 4) {
                    std::string ext = name.substr(name.size() - 4);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".glb") {
                        m_availableMaps.push_back(name.substr(0, name.size() - 4));
                    }
                }
            }
        }
    };

    scanDir("assets/external");
    scanDir("assets/data");

    std::sort(m_availableMaps.begin(), m_availableMaps.end());
    m_availableMaps.erase(std::unique(m_availableMaps.begin(), m_availableMaps.end()), m_availableMaps.end());
}

void Engine::shutdown() {
    TelemetryExporter::shutdown();
    // Persist cloud layer settings before teardown.
    {
        auto& cfg = m_cloudLayerRenderer.config();
        json j;
        j["enabled"] = cfg.enabled;
        j["layerCount"] = cfg.layerCount;
        j["coverageSize"] = cfg.coverageSize;
        j["cloudBottom"] = cfg.cloudBottom;
        j["cloudThickness"] = cfg.cloudThickness;
        j["coverageScale"] = cfg.coverageScale;
        j["noiseScale"] = cfg.noiseScale;
        j["detailStrength"] = cfg.detailStrength;
        j["windSpeed"] = cfg.windSpeed;
        j["windDirection"] = { cfg.windDirection.x, cfg.windDirection.y, cfg.windDirection.z };
        j["godRayMode"] = static_cast<uint32_t>(cfg.godRayMode);
        j["godRaySamples"] = cfg.godRaySamples;
        j["godRayIntensity"] = cfg.godRayIntensity;
        j["castShadows"] = cfg.castShadows;
        j["shadowMapSize"] = cfg.shadowMapSize;
        j["temporalReprojection"] = cfg.temporalReprojection;
        j["maxSteps"] = cfg.maxSteps;
        j["stepSize"] = cfg.stepSize;
        j["debugMode"] = cfg.debugMode;
        j["debugShowMissColor"] = cfg.debugShowMissColor;
        j["showCloudShadows"] = cfg.showCloudShadows;
        j["cloudShadowOpacity"] = cfg.cloudShadowOpacity;
        j["cloudShadowDistanceFade"] = cfg.cloudShadowDistanceFade;
        j["debugPlaneY"] = cfg.debugPlaneY;
        j["cloudCoverage"] = cfg.cloudCoverage;
        j["cloudAmount"] = cfg.cloudAmount;
        j["cloudEdgeSoftness"] = cfg.cloudEdgeSoftness;
        j["cloudRayMarchSteps"] = cfg.cloudRayMarchSteps;
        j["cloudDebugThickness"] = cfg.cloudDebugThickness;
        j["cloudSpikeHeight"] = cfg.cloudSpikeHeight;
        j["cloudBaseDepth"] = cfg.cloudBaseDepth;
        j["cloudSpikeWidth"] = cfg.cloudSpikeWidth;
        try {
            std::ofstream ofs("data/cloud_layer_config.json");
            if (ofs) ofs << j.dump(2);
        } catch (...) {}
    }

    if (m_backgroundLoader) {
        m_backgroundLoader->cancel();
    }
    JobSystem::instance().shutdown();

    m_vulkan.waitIdle();

    // SCREENSHOT PENDENTE. m_screenshotStaging e' alocado no loop de render
    // (Render.cpp, "Capture screenshot including UI overlay") e so' e'
    // liberado em finishPendingScreenshot(), quando a fence do frame que
    // gravou a copia esta' pronta - ou seja, num frame SEGUINTE. Se a
    // aplicacao encerra com ele vivo (--auto-exit, ERUPTION_TEST_EXIT_FRAME
    // logo apos um dump, SWAP_SHOT no ultimo frame), o buffer chega ao
    // vmaDestroyAllocator ainda alocado e o VMA aborta em build de debug:
    //   VmaDeviceMemoryBlock::Destroy: "Some allocations were not freed
    //   before destruction of this memory block!"
    // Em release NDEBUG cala o assert e o vazamento fica silencioso.
    //
    // Aqui a GPU ja' esta' ociosa, entao a copia terminou: em vez de descartar
    // a captura, TERMINA ela (grava o PNG e libera o staging). O segundo if e'
    // rede de seguranca para o caso de o staging existir sem o flag.
    finishPendingScreenshot();
    if (m_screenshotStaging != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_vulkan.allocator(), m_screenshotStaging, m_screenshotStagingAlloc);
        m_screenshotStaging = VK_NULL_HANDLE;
        m_screenshotStagingAlloc = VK_NULL_HANDLE;
    }

    // Flush any pending PBR texture uploads so their staging buffers are freed
    // before VMA is torn down.
    flushPbrTextureUploads(&m_vulkan);

    // Destroy any pending mesh/upload staging buffers before VMA teardown.
    // Pending meshes also own their destination vertex/index buffers because the
    // upload was never submitted; destroy them too to avoid leaking VMA memory.
    for (auto& pm : s_pendingMeshes) {
        vmaDestroyBuffer(m_vulkan.allocator(), pm.staging, pm.alloc);
        if (pm.vb != VK_NULL_HANDLE) vmaDestroyBuffer(m_vulkan.allocator(), pm.vb, pm.vbAlloc);
        if (pm.ib != VK_NULL_HANDLE) vmaDestroyBuffer(m_vulkan.allocator(), pm.ib, pm.ibAlloc);
    }
    s_pendingMeshes.clear();
    // Pending texture uploads own both staging and the destination image until
    // the upload is flushed. Destroy both; the corresponding AssetCache entry will
    // hold null handles and be skipped during later cache shutdown.
    for (auto& pu : s_pendingUploads) {
        vmaDestroyBuffer(m_vulkan.allocator(), pu.staging, pu.stagingAlloc);
        if (pu.image != VK_NULL_HANDLE) vmaDestroyImage(m_vulkan.allocator(), pu.image, pu.imageAlloc);
    }
    s_pendingUploads.clear();

    // Clear all map contexts properly
    if (m_activeMapContext) m_activeMapContext->clearGpu(&m_vulkan, &m_bindless);
    if (m_stagingMapContext) m_stagingMapContext->clearGpu(&m_vulkan, &m_bindless);
    for (auto& dd : m_deferredDeletes) {
        if (dd.context) dd.context->clearGpu(&m_vulkan, &m_bindless);
    }
    m_activeMapContext.reset();
    m_stagingMapContext.reset();
    m_deferredDeletes.clear();

    m_assetCache.shutdown(&m_vulkan, &m_bindless);

    m_spherePreview.shutdown();
    m_modelRenderer.shutdown(); m_terrainRenderer.shutdown(); m_spriteSystem.shutdown();
    m_spriteRenderer.shutdown(); m_shadowRenderer.shutdown(); m_deferredLighting.shutdown();
    m_upscaleAA.shutdown();
    m_cameraMotion.shutdown();
    m_spriteLayer.shutdown();
    m_fsr.shutdown();
    m_postProcessor.shutdown(); shutdownShimmerMetric(); MipmapGenerator::shutdownCompute(&m_vulkan);
    // Independent cloud layers must be released while the Vulkan context and
    // the cloud renderer are still alive (they own VMA allocations and hold
    // renderer slots).
    clearIndependentClouds();
    m_cloudLayerRenderer.shutdown(); m_skyProbe.shutdown(); m_irradianceProbes.shutdown(); m_skybox.shutdown();
    // LAVA: WaterRenderer::shutdown() chama clearWaterMesh() mas nao
    // clearLavaMesh(), e o par vertex+index da superficie de lava (30.740 +
    // 36.096 bytes em parana_demo) chegava vivo ao vmaDestroyAllocator - o
    // ultimo vazamento que sobrou depois de fechar os de LOD/sombra. A
    // correcao propria e' UMA linha dentro de WaterRenderer::shutdown(), que
    // e' arquivo de outro agente; ate' ela entrar, fecha-se daqui pelo metodo
    // publico que ja' existe. Quando o WaterRenderer passar a limpar sozinho,
    // esta chamada vira no-op (clearLavaMesh checa os handles) e pode sair.
    m_water.clearLavaMesh();
    m_water.shutdown();
    m_debugLineRenderer.shutdown(); m_overlayLineRenderer.shutdown(); m_gbuffer.shutdown(); m_bindless.shutdown();

    // Cleanup player sprites
    if (m_playerController) {
        cleanupSpriteTextures(m_playerController->bodySpriteRef());
        cleanupSpriteTextures(m_playerController->hairSpriteRef());
    }

    if (m_defaultSampler != VK_NULL_HANDLE) vkDestroySampler(m_vulkan.device(), m_defaultSampler, nullptr);
    if (m_nearestSampler != VK_NULL_HANDLE) vkDestroySampler(m_vulkan.device(), m_nearestSampler, nullptr);
    if (m_uiDefaultSampler != VK_NULL_HANDLE) vkDestroySampler(m_vulkan.device(), m_uiDefaultSampler, nullptr);
    if (m_uiNearestSampler != VK_NULL_HANDLE) vkDestroySampler(m_vulkan.device(), m_uiNearestSampler, nullptr);

    shutdownSplashLogo();
    shutdownMinimap();
    destroyViewportTarget();
    shutdownImGui();

    m_vulkan.shutdown();
 m_window.shutdown();
    Input::shutdown(); Logger::shutdown();
    m_initialized = false;
}

float Engine::advanceFrame() {
    Profiler::resetCpuTime();
    Profiler::resetGpuTime();
    Profiler::resetGpuTimeByName();
    SystemMonitor::update();

    if (!m_pendingMapWarp.empty()) {
        if (m_backgroundLoader->isLoading()) {
            if (m_backgroundLoader->currentMapName() != m_pendingMapWarp) {
                m_backgroundLoader->cancel();
                m_warpRestartTarget = m_pendingMapWarp;
                m_warpRestartPending = true;
            }
            m_pendingMapWarp.clear();
        } else if (m_warpRestartPending) {
            if (m_warpRestartTarget != m_pendingMapWarp) {
                m_warpRestartTarget = m_pendingMapWarp;
            }
            m_pendingMapWarp.clear();
        } else {
            TelemetryExporter::recordLoadStart(m_pendingMapWarp);
            m_backgroundLoader->startLoad(m_pendingMapWarp);
            m_mapSwapPending = true;
            m_pendingMapWarp.clear();
        }
    }

    if (m_warpRestartPending && !m_backgroundLoader->isLoading()) {
        TelemetryExporter::recordLoadStart(m_warpRestartTarget);
        m_backgroundLoader->startLoad(m_warpRestartTarget);
        m_mapSwapPending = true;
        m_warpRestartPending = false;
        m_warpRestartTarget.clear();
    }

    processBackgroundLoading();

    m_timer.tick();
    m_window.pollEvents();

    // Auto heat-shimmer ground height from terrain under the player.
    if (m_currentMap) {
        Vec3 refPos = m_playerController ? m_playerController->pos() : m_camera.position();
        float groundY = TerrainParser::getTerrainHeightAt(m_currentMap->terrain, refPos.x, refPos.z);
        m_weatherSystem.setAutoHeatGroundHeight(groundY);
        // Regional climate (G9): the field is sampled where the player is, so
        // walking toward the volcano actually walks into its weather. Same
        // reference position as the heat shimmer, deliberately -- the two would
        // disagree at the edge of a hot region otherwise.
        m_weatherSystem.setClimateSamplePosition(Vec3(refPos.x, groundY, refPos.z));
    } else {
        m_weatherSystem.setAutoHeatGroundHeight(0.0f);
    }

    m_weatherSystem.update(m_timer.deltaTime());

    // Update Cloud Layers.
    // With the procedural cloud field active, precipitation comes from the
    // rainy field clouds: suppress the camera-centered global particle draw.
    m_postProcessor.weatherRenderer().setSuppressGlobalPrecip(
        m_useProceduralCloudField && m_autoCloud && m_cloudLayersEnabled);
    if (m_cloudLayersEnabled) {
        auto cfg = m_cloudLayerRenderer.config();
        Vec3 windDir = cfg.windDirection;
        float windSpeed = (m_cloudWindOverride >= 0.0f) ? m_cloudWindOverride : cfg.windSpeed;
        // Clouds ride the same wind as everything else (G7). The field wanders
        // around the authored heading and pulses with gusts, so the cloud deck
        // now surges and eases instead of translating at a dead-constant rate.
        // Guarded by the override so the deterministic perf runs (which force a
        // fixed cloud wind) are unaffected.
        if (m_cloudWindOverride < 0.0f) {
            const WindField& wf = m_weatherSystem.wind();
            if (wf.strength() > 0.001f) {
                windDir = wf.direction();
                // Gust envelope around 1.0; the authored speed stays the mean.
                windSpeed *= glm::clamp(wf.gust(), 0.35f, 2.0f);
            }
        }

        // Drain pending independent-cloud spawns (CLI + UI button + weather
        // field) outside of command-buffer recording; layer creation submits
        // GPU work. Budget-based, ON DEMAND: every spawn bakes a coverage
        // array and blocking-submits GPU work, so spawn only while this
        // frame's spawn budget lasts (at least one per frame, for progress).
        // The field grows as fast as the machine allows WITHOUT hitching,
        // taking as long as it needs — no fixed schedule, no slider. The
        // budget is larger while the loading overlay is up: spawn warm-up
        // belongs behind the loading screen, not as an in-game hitch.
        if (!m_pendingCloudSpawns.empty()) {
            const bool loadingUp = (m_backgroundLoader && m_backgroundLoader->isLoading()) ||
                                   (m_stagingMapContext && (!m_stagingMapContext->isGpuReady() || m_mapSwapPending));
            const float budgetMs = loadingUp ? 8.0f : 2.0f;
            const auto spawnT0 = std::chrono::steady_clock::now();
            size_t n = 0;
            while (n < m_pendingCloudSpawns.size()) {
                const auto& s = m_pendingCloudSpawns[n];
                spawnIndependentCloud(s.altitude, s.density, Vec2(s.jitterX, s.jitterZ),
                                      s.hasRain, s.radiusScale, s.fromField);
                ++n;
                const float spentMs = std::chrono::duration<float, std::milli>(
                                          std::chrono::steady_clock::now() - spawnT0).count();
                if (spentMs >= budgetMs) break;
            }
            m_pendingCloudSpawns.erase(m_pendingCloudSpawns.begin(),
                                       m_pendingCloudSpawns.begin() + static_cast<long>(n));
            // Perf investigation: frame cost while the field drains.
            static float s_drainLogAccum = 0.0f;
            s_drainLogAccum += m_timer.deltaTime();
            if (s_drainLogAccum >= 1.0f) {
                s_drainLogAccum = 0.0f;
                ERUPTION_LOG_WARN("[DRAIN] pending=%zu frameDt=%.1fms lastSpawnBatch=%zu batchMs=%.1f | shad=%.1f gbuf=%.1f csm=%.1f def=%.1f water=%.1f cfsh=%.1f cfvol=%.1f volfield=%.1f wpost=%.1f copy=%.1f endf=%.1f",
                                m_pendingCloudSpawns.size(), m_timer.deltaTime() * 1000.0f, n,
                                std::chrono::duration<float, std::milli>(
                                    std::chrono::steady_clock::now() - spawnT0).count(),
                                m_benchmarkCpuFrameMs[1], m_benchmarkCpuFrameMs[2], m_benchmarkCpuFrameMs[3],
                                m_benchmarkCpuFrameMs[4], m_benchmarkCpuFrameMs[5], m_benchmarkCpuFrameMs[6],
                                m_benchmarkCpuFrameMs[7], m_benchmarkCpuFrameMs[8], m_benchmarkCpuFrameMs[9],
                                m_benchmarkCpuFrameMs[10], m_benchmarkCpuFrameMs[11]);
            }
        }

        // Procedural cloud field: (re)spawn the per-weather independent cloud
        // set when the weather type changes (including the first frame after
        // boot/map load). The global cloud layer stays dormant while enabled.
        if (m_useProceduralCloudField && m_autoCloud) {
            const int wt = static_cast<int>(m_weatherSystem.currentType());
            if (wt != m_weatherFieldType) {
                m_weatherFieldType = wt;
                spawnWeatherCloudField(m_weatherSystem.currentType());
            }
        }

        // Sync rain box drift direction with the cloud layer wind direction.
        m_postProcessor.weatherRenderer().setRainBoxMoveDirection(windDir);

        // Smoothly interpolate cloud amount when the weather type changed.
        if (m_cloudAmountInTransition) {
            float t = m_weatherSystem.inTransition() ? m_weatherSystem.transitionProgress() : 1.0f;
            cfg.cloudAmount = glm::mix(m_cloudAmountTransitionStart, m_cloudAmountTransitionEnd, t);
            m_cloudLayerRenderer.setConfig(cfg);
            m_cloudLayerRenderer.coverageArray().setCoverage(cfg.cloudAmount);
            if (t >= 1.0f) m_cloudAmountInTransition = false;
        }

        // Animate the cloud coverage array by wind. The global layer is
        // dormant while the procedural cloud field replaces it.
        {
            PROFILE_CPU_SCOPE(ProfilerCategory::RenderDispatch);
            if (!m_useProceduralCloudField) {
                m_cloudLayerRenderer.coverageArray().update(m_timer.deltaTime(), windDir, windSpeed);
            }
            m_cloudLayerRenderer.update(m_timer.deltaTime());

            const auto& w = m_weatherSystem.current();
            m_cloudLayerRenderer.setWeather(w.cloudCoverage, w.rainIntensity, w.stormTint);
            m_postProcessor.weatherRenderer().setPoolTargetRainIntensity(m_weatherSystem.target().rainIntensity);

            auto& coverageArray = m_cloudLayerRenderer.coverageArray();
            float layer = glm::clamp(cfg.layerCount * 0.5f, 0.0f, static_cast<float>(cfg.layerCount - 1));
            // cloudAmount is now baked into the coverage texture on CPU; the threshold
            // field is kept for push-constant compatibility but is no longer used.
            m_postProcessor.weatherRenderer().setCloudCoverageParams(
                layer, 0.0f, w.cloudCoverage, coverageArray.windOffset());

            // Independent cloud layers: drift by wind (UV shift, no regen) and
            // refresh their per-layer UBOs. Heavier clouds (weight = density +
            // occupied area + rain darkness) react slower to the wind. The UV
            // factor is divided by the world size so the drift speed is in
            // meters/second and feels the same on small and large maps:
            // v [m/s] ~= windSpeed * 20 / weight.
            m_cloudDebugLogTimer += m_timer.deltaTime();
            bool logCloudsNow = m_cloudDebugLogTimer >= 2.0f;
            if (logCloudsNow && !m_independentCloudLayers.empty()) m_cloudDebugLogTimer = 0.0f;
            for (auto& icl : m_independentCloudLayers) {
                Vec3 wsz = icl.array->worldMax() - icl.array->worldMin();
                float worldSize = glm::max(1.0f, std::min(std::abs(wsz.x), std::abs(wsz.z)));
                icl.array->updateWind(m_timer.deltaTime(), windDir, windSpeed,
                                      20.0f / (worldSize * glm::max(icl.weight, 0.25f)));
                m_cloudLayerRenderer.updateCloudLayerUBO(icl.rendererId);
                // Weather-field clouds that drifted off the map respawn at the
                // upwind edge (relative to the player, with perpendicular
                // jitter) so the field keeps crossing the map with the wind.
                if (icl.fromField) {
                    Vec2 woff = icl.array->windOffset();
                    Vec3 wmn = icl.array->worldMin();
                    const float cloudX = icl.centerXZ.x + woff.x * wsz.x;
                    const float cloudZ = icl.centerXZ.y + woff.y * wsz.z;
                    const float margin = 200.0f;
                    if (cloudX < wmn.x - margin || cloudX > wmn.x + wsz.x + margin ||
                        cloudZ < wmn.z - margin || cloudZ > wmn.z + wsz.z + margin) {
                        Vec2 wdir(windDir.x, windDir.z);
                        if (glm::length(wdir) > 1e-4f) {
                            const Vec2 wn = glm::normalize(wdir);
                            const Vec2 perp(-wn.y, wn.x);
                            const float spawnDist = 0.55f * worldSize;
                            const float jitter = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 1.2f * spawnDist;
                            const Vec2 target = Vec2(m_camera.target().x, m_camera.target().z)
                                              - wn * spawnDist + perp * jitter;
                            icl.array->setWindOffset(Vec2((target.x - icl.centerXZ.x) / wsz.x,
                                                          (target.y - icl.centerXZ.y) / wsz.z));
                        }
                    }
                }
                // Cloud debug logging disabled to avoid synchronous fprintf overhead
                // in weather-heavy scenes (e.g. thunderstorm).
                (void)icl; (void)wsz; (void)worldSize; (void)logCloudsNow;
            }

            // Rain followers: each rainy cloud gets a rain box that tracks its
            // GROUND SHADOW, i.e. the cloud's drifted XZ projected along the
            // sun direction onto the ground (same projection the cloud-shadow
            // pass uses, so the rain lands exactly under the shadow). The
            // occluder is the cloud blob itself (drifted center + blob
            // radius), so the cloud only hides the rain directly behind it.
            {
                // Scratch estatico: era malloc+realloc+free por frame p/
                // ate' ~100 followers de 88B (varredura 2026-09-02).
                static std::vector<WeatherRenderer::RainFollower> rainFollowers;
                rainFollowers.clear();
                Vec3 L = glm::normalize(m_shadowRenderer.currentLightDir());
                for (const auto& icl : m_independentCloudLayers) {
                    if (!icl.hasRain) continue;
                    Vec2 woff = icl.array->windOffset();
                    Vec3 wsz = icl.array->worldMax() - icl.array->worldMin();
                    const float cloudCX = icl.centerXZ.x + woff.x * wsz.x;
                    const float cloudCZ = icl.centerXZ.y + woff.y * wsz.z;
                    float cx = cloudCX;
                    float cz = cloudCZ;
                    float groundY = m_camera.target().y;
                    if (m_currentMap)
                        groundY = TerrainParser::getTerrainHeightAt(m_currentMap->terrain, cx, cz);
                    float h = glm::max(icl.planeY - groundY, 0.0f);
                    if (L.y > 0.01f && h > 0.0f) {
                        float tanAngle = glm::min(glm::length(Vec2(L.x, L.z)) / L.y, 4.0f);
                        Vec2 n = glm::normalize(Vec2(L.x, L.z) + Vec2(0.0001f));
                        cx += n.x * h * tanAngle;
                        cz += n.y * h * tanAngle;
                    }
                    // Occluder radius: baked blob extent (radius/falloff, same
                    // math as coverage_gen) plus a margin for the ray-marched
                    // volume that rises above the plane. Used only as a cheap
                    // early-out; the real occlusion test samples the cloud's
                    // own coverage array (exact silhouette).
                    float occRadius = 0.0f;
                    // Per-axis footprint for the rain box: the box must cover
                    // the whole cloud so drops spread under it instead of
                    // clustering (and additively blowing out) in the legacy
                    // 80x80 m patch. Floor of 20 m so a degenerate cloud never
                    // collapses the vertex-shader wrap. Composite clouds have
                    // sub-blobs offset from the layer center, so include the
                    // offset in both the radius and the per-axis extents.
                    Vec2 boxHalf(20.0f);
                    float puffRy = 6.0f;
                    for (const LocalCloud& lc : icl.array->localClouds()) {
                        if (!lc.alive) continue;
                        const float invF = 1.0f / std::max(lc.falloff, 0.01f);
                        const Vec2 off = lc.center - icl.centerXZ;
                        occRadius = std::max(occRadius, glm::length(off) + std::max(lc.radius.x, lc.radius.y) * invF);
                        boxHalf.x = std::max(boxHalf.x, std::abs(off.x) + lc.radius.x * invF);
                        boxHalf.y = std::max(boxHalf.y, std::abs(off.y) + lc.radius.y * invF);
                        // Vertical semi-axis of the volumetric puff (same ry law
                        // as cloud_billboard.frag's blobDensity): the rain box
                        // drops from the cloud's MIDDLE slice now that clouds
                        // are 3D volumes, not from the bottom plane (2D era).
                        puffRy = std::max(puffRy, glm::clamp(std::fmax(lc.radius.x, lc.radius.y) * invF * 0.6f, 6.0f, 100.0f));
                    }
                    const float cloudMidY = icl.planeY + puffRy;
                    Vec3 wmn = icl.array->worldMin();
                    // Rain rate from loadedness: density already drives the
                    // cloud's rain/storm darkness (see spawnIndependentCloud),
                    // so reuse it as the "how heavy is this cloud" signal.
                    // density 0 -> 0.8x, density 1 -> 1.5x drop intensity.
                    const float rainRate = glm::clamp(0.8f + 0.7f * icl.density, 0.6f, 1.8f);
                    rainFollowers.push_back({Vec3(cx, cloudMidY, cz),
                                             Vec4(cloudCX, cloudCZ, occRadius + 30.0f, 0.0f),
                                             Vec4(wmn.x, wmn.z, wsz.x, wsz.z),
                                             woff,
                                             boxHalf * 2.0f,
                                             icl.array->imageView(),
                                             icl.array->sampler(),
                                             rainRate});
                }
                // Each follower redraws the full rain particle pool, so cap
                // the draws to the nearest clouds: far rain is barely visible
                // and 100 rainy clouds would otherwise melt the GPU. In the
                // Rain/Extreme group EVERY spawned rainy cloud rains (the
                // renderer's occluder array supports 100; the pool density
                // scales with the follower count to keep drops/m2 per cloud).
                // Non-rain weathers with manual rainy clouds keep the legacy
                // 12-follower budget.
                const WeatherCategory catNow = weatherTypeCategory(m_weatherSystem.currentType());
                const bool rainGroup = (catNow == WeatherCategory::Rain ||
                                        catNow == WeatherCategory::Extreme);
                const size_t MAX_RAIN_FOLLOWERS =
                    rainGroup ? static_cast<size_t>(m_maxRainFollowers) : 12;
                if (rainFollowers.size() > MAX_RAIN_FOLLOWERS) {
                    const Vec3 camPos = m_camera.position();
                    std::sort(rainFollowers.begin(), rainFollowers.end(),
                              [&](const WeatherRenderer::RainFollower& a, const WeatherRenderer::RainFollower& b) {
                                  float da = glm::distance(Vec2(camPos.x, camPos.z), Vec2(a.boxCenter.x, a.boxCenter.z));
                                  float db = glm::distance(Vec2(camPos.x, camPos.z), Vec2(b.boxCenter.x, b.boxCenter.z));
                                  return da < db;
                              });
                    rainFollowers.resize(MAX_RAIN_FOLLOWERS);
                }
                // Pool sizing hint: while the field drains in (1 cloud/frame)
                // the follower count grows per frame — size the rain pool for
                // the FINAL count (current + pending rainy spawns) so the pool
                // rebuilds once at transition start, not every 4 followers.
                uint32_t pendingRainy = 0;
                for (const auto& s : m_pendingCloudSpawns)
                    if (s.hasRain) ++pendingRainy;
                m_postProcessor.weatherRenderer().setPoolTargetFollowerCount(
                    static_cast<uint32_t>(rainFollowers.size()) + pendingRainy);
                m_postProcessor.weatherRenderer().setRainFollowers(rainFollowers);
            }
        }

        m_postProcessor.weatherRenderer().setCloudBaseHeight(cfg.cloudBottom);
    }

    // Thunderstorm benchmark: force worst-case weather and oscillate zoom to stress
    // both cloud fill-rate and rain particle systems.
    if (m_benchmarkThunderstorm) {
        static bool s_benchmarkFlagLogged = false;
        if (!s_benchmarkFlagLogged) {
            s_benchmarkFlagLogged = true;
            ERUPTION_LOG_WARN("[BENCHMARK] Thunderstorm benchmark enabled (duration=%.1f pos=%.1f,%.1f,%.1f)",
                            m_benchmarkDuration, m_benchmarkPos.x, m_benchmarkPos.y, m_benchmarkPos.z);
        }
        if (!m_currentMap) {
            static float s_waitLog = 0.0f;
            s_waitLog += m_timer.deltaTime();
            if (s_waitLog >= 2.0f) {
                ERUPTION_LOG_WARN("[BENCHMARK] Waiting for map load...");
                s_waitLog = 0.0f;
            }
        } else if (!m_benchmarkThunderstormStarted) {
            m_benchmarkThunderstormStarted = true;
            m_benchmarkThunderstormTimer = 0.0f;
            m_benchmarkThunderstormZoomPhase = 0.0f;
            m_benchmarkThunderstormFrames = 0;
            m_benchmarkThunderstormFpsSum = 0.0f;
            m_benchmarkThunderstormFpsMin = 9999.0f;
            m_benchmarkThunderstormStable60Time = 0.0f;
            m_benchmarkCpuAccum.fill(0.0);
            m_benchmarkCpuSamples = 0;
            m_vulkan.preferImmediatePresentMode(true);
            m_vulkan.recreateSwapchain();
            // Render the benchmark at a lower resolution to leave headroom for
            // vsync and mobile-tier fill-rate targets.
            if (m_vulkan.window()) {
                glfwSetWindowSize(m_vulkan.window(), 960, 540);
                m_resized = true;
            }
            applyWeatherTypeFull(WeatherType::Thunderstorm, 1.0f);
            m_weatherSystem.snapToTarget();
            m_camera.setOrbitTarget(m_benchmarkPos);
            m_camera.setFarPlane(400.0f);
            // Mobile-oriented benchmark settings: disable expensive post-process,
            // water, cascaded shadows and fog so the target is a stable 60 FPS on
            // modest hardware.
            m_postSettings.enableDoF = false;
            m_postSettings.enableFog = false;
            m_postSettings.enableBloom = false;
            m_postSettings.enableChromaticAberration = false;
            m_postSettings.enableMotionBlur = false;
            if (m_renderEffects.size() > 0) m_renderEffects[0]->setEnabled(false); // Fog
            // Bloom faltava aqui: quem manda no bloom e' o RenderEffect (o
            // render() reatribui m_postSettings.enableBloom a partir dele todo
            // frame), entao o `enableBloom = false` logo acima era desfeito no
            // primeiro frame e o benchmark "sem post" media COM bloom.
            if (m_renderEffects.size() > 2) m_renderEffects[2]->setEnabled(false); // Bloom
            if (m_renderEffects.size() > 3) m_renderEffects[3]->setEnabled(false); // DoF / Tilt-Shift
            if (m_renderEffects.size() > 5) m_renderEffects[5]->setEnabled(false); // Shadows
            if (m_renderEffects.size() > 6) m_renderEffects[6]->setEnabled(false); // Skybox IBL
            if (m_renderEffects.size() > 7) m_renderEffects[7]->setEnabled(false); // Water
            m_shadowRenderer.settings().enabled = false;
            m_postSettings.weatherTier = WeatherTier::Mobile;

            ERUPTION_LOG_WARN("[BENCHMARK] Thunderstorm started for %.1fs", m_benchmarkDuration);
        }

        if (!m_benchmarkThunderstormStarted) {
            return m_timer.deltaTime();
        }

        float dt = m_timer.deltaTime();
        m_benchmarkThunderstormTimer += dt;
        m_benchmarkThunderstormZoomPhase += dt * 0.5f; // one zoom cycle every ~12s

        // Oscillate distance between close-up (80) and far (500) to stress fill-rate
        // while keeping cloud overdraw within the 16.6ms budget.
        float t = (glm::sin(m_benchmarkThunderstormZoomPhase) + 1.0f) * 0.5f;
        float dist = 80.0f + t * 420.0f;
        m_camera.setOrbit(m_camera.orbitYaw(), m_camera.orbitPitch(), dist);

        // FPS do benchmark vem do RELOGIO REAL, nao do delta de simulacao:
        // com ERUPTION_TEST_FIXED_DT (necessario para A/B deterministico) o
        // 1/dt daria o passo fixo cravado - media de 60,0 em todo mapa,
        // escondendo o numero de verdade. Mesmo bug que ja' existia no
        // contador do HUD (commit 312cb7e).
        const float realDt = glm::max(m_timer.realDeltaTime(), 0.0001f);
        float fps = 1.0f / realDt;
        m_benchmarkThunderstormFpsSum += fps;
        m_benchmarkThunderstormFpsMin = glm::min(m_benchmarkThunderstormFpsMin, fps);
        m_benchmarkThunderstormFrames++;

        if (fps >= 59.0f) {
            m_benchmarkThunderstormStable60Time += realDt;
        } else {
            m_benchmarkThunderstormStable60Time = 0.0f;
        }

        if (m_benchmarkThunderstormTimer >= m_benchmarkDuration) {
            float avgFps = (m_benchmarkThunderstormFrames > 0) ? m_benchmarkThunderstormFpsSum / m_benchmarkThunderstormFrames : 0.0f;
            (void)avgFps;
            ERUPTION_LOG_WARN("[BENCHMARK] Thunderstorm finished: avg=%.1f min=%.1f stable60=%.2fs / %.1fs",
                            (m_benchmarkThunderstormFrames > 0) ? m_benchmarkThunderstormFpsSum / m_benchmarkThunderstormFrames : 0.0f,
                            m_benchmarkThunderstormFpsMin,
                            m_benchmarkThunderstormStable60Time, m_benchmarkDuration);
            logBenchmarkBreakdown();
            m_running = false;
        }
    }

    // Heavy-rain benchmark (--benchmark-heavy-rain): Thunderstorm weather with
    // the user's real render settings (no mobile tier, no effect stripping).
    // 100-cloud storm field + per-cloud rain + lightning, HUD rendered. Measures
    // FPS in two phases: 5s with the character idle, then 5s with the camera
    // sweeping yaw, pitch and zoom dynamically around the character.
    // ERUPTION_TEST_BENCH_WEATHER=<name> swaps the storm for any other weather type.
    if (m_benchmarkHeavyRain) {
        if (!m_currentMap) {
            static float s_heavyWaitLog = 0.0f;
            s_heavyWaitLog += m_timer.deltaTime();
            if (s_heavyWaitLog >= 2.0f) {
                ERUPTION_LOG_WARN("[BENCHMARK] HeavyRain waiting for map load...");
                s_heavyWaitLog = 0.0f;
            }
            return m_timer.deltaTime();
        }

        float dt = m_timer.deltaTime();
        float fps = 1.0f / glm::max(dt, 0.0001f);

        if (m_benchmarkHeavyRainPhase == 0) {
            // Warmup: force the storm and wait for the 100-cloud spawn queue to
            // drain, plus a grace period for FPS to stabilize.
            if (m_benchmarkHeavyRainWarmup == 0.0f && m_benchmarkHeavyRainTimer == 0.0f) {
                // ERUPTION_TEST_BENCH_WEATHER=name (debug): benchmark a different
                // weather type (e.g. "clear") as a baseline instead of the storm.
                const char* benchWeather = std::getenv("ERUPTION_TEST_BENCH_WEATHER");
                const WeatherType benchType = (benchWeather && *benchWeather)
                                                  ? weatherTypeFromName(benchWeather)
                                                  : WeatherType::Thunderstorm;
                applyWeatherTypeFull(benchType, 1.0f);
                m_weatherSystem.snapToTarget();
                // ERUPTION_TEST_BENCH_TIER=high (debug): force the High weather tier
                // so the 3D rain splash pass (High-only) is exercised.
                if (const char* tierEnv = std::getenv("ERUPTION_TEST_BENCH_TIER")) {
                    if (std::string(tierEnv) == "high")
                        m_postSettings.weatherTier = WeatherTier::High;
                }
                m_camera.setOrbitTarget(m_benchmarkPos);
                m_benchmarkWeatherName = (benchWeather && *benchWeather) ? benchWeather : "thunderstorm";
                // ERUPTION_TEST_BENCH_SHOTS=<dir>: per-phase screenshots (idle shot
                // doubles as the deterministic frame for visual diffs).
                if (const char* shotsDir = std::getenv("ERUPTION_TEST_BENCH_SHOTS")) {
                    if (*shotsDir) {
                        m_benchmarkShotsDir = shotsDir;
                        std::filesystem::create_directories(m_benchmarkShotsDir);
                    }
                }
                // ERUPTION_TEST_BENCH_NO_HUD=1 (debug): hide the HUD so screenshots
                // are diffable (the FPS counter text would never match).
                m_benchmarkHideHud = std::getenv("ERUPTION_TEST_BENCH_NO_HUD") != nullptr;
                // ERUPTION_TEST_BENCH_CLOSESHOT=1 (debug): extra close-up screenshot
                // at the minimum-zoom moment of the dynamic phase.
                m_benchmarkCloseShot = std::getenv("ERUPTION_TEST_BENCH_CLOSESHOT") != nullptr;
                m_benchmarkCloseShotTaken = false;
                ERUPTION_LOG_WARN("[BENCHMARK] HeavyRain warmup (%s): cloud field spawning...",
                                benchWeather && *benchWeather ? benchWeather : "thunderstorm");
            }
            m_benchmarkHeavyRainTimer += dt;
            if (m_pendingCloudSpawns.empty()) {
                m_benchmarkHeavyRainWarmup += dt;
                if (m_benchmarkHeavyRainWarmup >= BENCHMARK_HEAVY_RAIN_WARMUP_SEC) {
                    m_benchmarkHeavyRainPhase = 1;
                    m_benchmarkHeavyRainTimer = 0.0f;
                    m_benchmarkHeavyRainOrbitYaw = m_camera.orbitYaw();
                    ERUPTION_LOG_WARN("[BENCHMARK] HeavyRain phase 1/2: idle %.1fs", BENCHMARK_HEAVY_RAIN_PHASE_SEC);
                }
            }
        } else if (m_benchmarkHeavyRainPhase == 1 || m_benchmarkHeavyRainPhase == 2) {
            const int idx = m_benchmarkHeavyRainPhase - 1;
            if (m_benchmarkSkipStatFrames > 0) {
                // Frame stalled by the screenshot readback/encode: exclude it
                // from timer, fps min/avg and spike stats.
                --m_benchmarkSkipStatFrames;
            } else {
            m_benchmarkHeavyRainTimer += dt;
            m_benchmarkHeavyRainFpsSum[idx] += fps;
            m_benchmarkHeavyRainFpsMin[idx] = glm::min(m_benchmarkHeavyRainFpsMin[idx], fps);
            m_benchmarkHeavyRainFrames[idx]++;
            ++m_benchmarkCpuSamples;

            // Spike diagnosis: count frames above the 60 FPS budget and log
            // the CPU section breakdown of the first few (the min-FPS number
            // alone does not say what stalled).
            if (dt > 0.0167f) {
                ++m_benchmarkSpikeCount[idx];
                if (m_benchmarkSpikeLogs < 40) {
                    ++m_benchmarkSpikeLogs;
                    ERUPTION_LOG_WARN("[BENCHMARK] spike dt=%.1fms | shad=%.2f gbuf=%.2f csm=%.2f def=%.2f water=%.2f sky=%.2f cfsh=%.2f cfvol=%.2f wpost=%.2f copy=%.2f endf=%.2f",
                                    dt * 1000.0f,
                                    m_benchmarkCpuFrameMs[1], m_benchmarkCpuFrameMs[2], m_benchmarkCpuFrameMs[3],
                                    m_benchmarkCpuFrameMs[4], m_benchmarkCpuFrameMs[5], m_benchmarkCpuFrameMs[6],
                                    m_benchmarkCpuFrameMs[7], m_benchmarkCpuFrameMs[8], m_benchmarkCpuFrameMs[9],
                                    m_benchmarkCpuFrameMs[10], m_benchmarkCpuFrameMs[11]);
                }
            }

            if (m_benchmarkHeavyRainPhase == 2) {
                // Dynamic camera: full yaw circle plus pitch and zoom sweeps
                // at incommensurate rates, so different yaw/pitch/zoom
                // combinations are exercised within the phase.
                const float t = m_benchmarkHeavyRainTimer / BENCHMARK_HEAVY_RAIN_PHASE_SEC;
                m_benchmarkHeavyRainOrbitYaw += dt * (glm::two_pi<float>() / BENCHMARK_HEAVY_RAIN_PHASE_SEC);
                const float pitch = glm::radians(40.0f + 28.0f * std::sin(t * glm::two_pi<float>() * 2.0f));
                const float dist = 430.0f + 370.0f * std::sin(t * glm::two_pi<float>() * 3.0f + 1.3f);
                m_camera.setOrbit(m_benchmarkHeavyRainOrbitYaw, pitch, dist);
                // ERUPTION_TEST_BENCH_CLOSESHOT=1 (debug): extra screenshot at the
                // minimum-zoom moment of the sweep (dist=60m), for close-up
                // visual checks (rain splash, streak shape).
                if (m_benchmarkCloseShot && !m_benchmarkCloseShotTaken &&
                    m_benchmarkHeavyRainTimer >= 0.905f && !m_benchmarkShotsDir.empty()) {
                    m_benchmarkCloseShotTaken = true;
                    takeScreenshot(m_benchmarkShotsDir + "/" + m_benchmarkWeatherName + "_close.png");
                    m_benchmarkSkipStatFrames = 1;
                }
            }

            if (m_benchmarkHeavyRainTimer >= BENCHMARK_HEAVY_RAIN_PHASE_SEC) {
                const char* names[2] = {"idle", "dynamic"};
                // Honest throughput: frames / wall time (avg of 1/dt inflates
                // when a few frames have tiny dt between long stalls).
                const float realAvg = static_cast<float>(m_benchmarkHeavyRainFrames[idx]) /
                                      glm::max(m_benchmarkHeavyRainTimer, 0.001f);
                ERUPTION_LOG_WARN("[BENCHMARK] HeavyRain %s: avg=%.1f fps min=%.1f fps (%d frames / %.1fs) spikes(>16.7ms)=%d",
                                names[idx], realAvg,
                                m_benchmarkHeavyRainFpsMin[idx],
                                m_benchmarkHeavyRainFrames[idx], m_benchmarkHeavyRainTimer,
                                m_benchmarkSpikeCount[idx]);
                if (!m_benchmarkShotsDir.empty()) {
                    takeScreenshot(m_benchmarkShotsDir + "/" + m_benchmarkWeatherName +
                                   (m_benchmarkHeavyRainPhase == 1 ? "_idle.png" : "_dyn.png"));
                    m_benchmarkSkipStatFrames = 1;
                }
                if (m_benchmarkHeavyRainPhase == 1) {
                    m_benchmarkHeavyRainPhase = 2;
                    m_benchmarkHeavyRainTimer = 0.0f;
                    ERUPTION_LOG_WARN("[BENCHMARK] HeavyRain phase 2/2: dynamic camera (yaw+pitch+zoom) %.1fs", BENCHMARK_HEAVY_RAIN_PHASE_SEC);
                } else {
                    logBenchmarkBreakdown();
                    m_running = false;
                }
            }
            } // !m_benchmarkSkipStatFrames
        }
    }

    return m_timer.deltaTime();
}

void Engine::logBenchmarkBreakdown() {
    const char* gpuNames[] = {
        "Shadows", "GBuffer", "Deferred Lighting", "Water", "Skybox IBL",
        "Cloud Shadow Map", "CloudFluff Render", "Cloud Shadows (field)",
        "Cloud Volumes (field)", "Rain TopDown Depth",
        "Rain Heightmap", "Weather Overlay", "Weather Particles", "DoF / Tilt-Shift"
    };
    for (const char* name : gpuNames) {
        ERUPTION_LOG_WARN("[BENCHMARK] GPU %-22s %.3f ms", name, Profiler::getGpuTime(name));
    }
    if (m_benchmarkCpuSamples > 0) {
        const char* const* cpuNames = kCpuPhaseNames;
        ERUPTION_LOG_WARN("[BENCHMARK] CPU section avg ms (samples=%d):", m_benchmarkCpuSamples);
        for (size_t i = 1; i < m_benchmarkCpuAccum.size(); ++i) {
            ERUPTION_LOG_WARN("[BENCHMARK] CPU %-22s %.3f ms", cpuNames[i], m_benchmarkCpuAccum[i] / double(m_benchmarkCpuSamples));
        }
    }
}

bool Engine::initShimmerMetric() {
    m_vulkan.createImage(128, 128, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VMA_MEMORY_USAGE_GPU_ONLY,
                         m_shimmerImage, m_shimmerAlloc, 1);
    
    for (uint32_t i = 0; i < 3; i++) {
        m_vulkan.createBuffer(128 * 128 * 4,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_CPU_ONLY,
                             m_shimmerBuffers[i], m_shimmerBufferAllocs[i]);
    }
    
    m_shimmerHistory.assign(120, 0.0f);
    m_shimmerPrevPixels.assign(128 * 128 * 4, 0);
    m_shimmerFrameCount = 0;
    return true;
}

void Engine::shutdownShimmerMetric() {
    if (m_shimmerImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_vulkan.allocator(), m_shimmerImage, m_shimmerAlloc);
        m_shimmerImage = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < 3; i++) {
        if (m_shimmerBuffers[i] != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_vulkan.allocator(), m_shimmerBuffers[i], m_shimmerBufferAllocs[i]);
            m_shimmerBuffers[i] = VK_NULL_HANDLE;
        }
    }
}

void Engine::generateMipmaps(VkCommandBuffer cmd, VkImage image, int w, int h, uint32_t mipLevels, VkFormat format, uint32_t alphaMode) {
    if (mipLevels <= 1) return;
    if (m_mipmapMenu.config.mode == MipGenerationMode::BLIT_LINEAR || format != VK_FORMAT_R8G8B8A8_UNORM) {
        MipmapGenerator::generateMipmapsBlit(cmd, image, w, h, mipLevels);
    } else {
        MipmapGenerator::generateMipmapsCompute(cmd, &m_vulkan, image, w, h, mipLevels, true, alphaMode);
    }
}


struct VisibleObject {
    AABB aabb;
    Vec3 center;
    float dist;
    float boundingRadius;
};

static std::vector<VisibleObject>& gatherVisibleObjects(const Camera& camera, const TerrainRenderer& terrain, const ModelRenderer& models, const SpriteSystem& sprites) {
    // Scratch estatico (main thread only): 4 chamadas por update montavam e
    // destruiam um vector de ~5000 elementos cada = malloc+realloc em cascata
    // + free, por frame. (Varredura de hot paths 2026-09-02.)
    static std::vector<VisibleObject> out;
    out.clear();
    const Frustum& frustum = camera.frustum();
    // Terrain chunks
    for (const auto& chunk : terrain.chunks()) {
        if (!frustum.intersectsAABB(chunk.aabbMin, chunk.aabbMax)) continue;
        Vec3 center = (chunk.aabbMin + chunk.aabbMax) * 0.5f;
        float dist = glm::length(center - camera.position());
        out.push_back({{chunk.aabbMin, chunk.aabbMax}, center, dist, glm::length(chunk.aabbMax - chunk.aabbMin) * 0.5f});
    }
    // Model instances. O AABB de mundo JA' EXISTE na instancia (mantido pelo
    // load/animacao e usado pelo culling de sombra) - a versao anterior
    // re-transformava 8 cantos por instancia POR CHAMADA (4 chamadas por
    // update = 32 transforms por instancia), o grosso dos ~7ms que o
    // comentario de computeObstruction registra. (Varredura 2026-09-02.)
    for (const auto& inst : models.getInstances()) {
        if (!inst.enabled) continue;
        if (!frustum.intersectsAABB(inst.worldAabbMin, inst.worldAabbMax)) continue;
        Vec3 center = (inst.worldAabbMin + inst.worldAabbMax) * 0.5f;
        float dist = glm::length(center - camera.position());
        out.push_back({{inst.worldAabbMin, inst.worldAabbMax}, center, dist, inst.boundingRadius});
    }
    // Sprites
    for (const auto& sprite : sprites.getVisibleSprites()) {
        AABB aabb = sprite.getAABB();
        Vec3 center = (aabb.min + aabb.max) * 0.5f;
        float dist = glm::length(center - camera.position());
        float radius = glm::length(sprite.size) * 0.5f;
        out.push_back({aabb, center, dist, radius});
    }
    return out;
}

float Engine::raycastScene(const Vec3& rayOrigin, const Vec3& rayDir, float maxDist, const std::vector<VisibleObject>& objects) const {
    float closest = -1.0f;
    float minDist = m_camera.nearPlane() * 2.0f;
    for (const auto& obj : objects) {
        float t = Frustum::rayIntersectAABB(rayOrigin, rayDir, obj.aabb.min, obj.aabb.max);
        if (t >= minDist && t < maxDist) {
            if (closest < 0.0f || t < closest) closest = t;
        }
    }
    return closest;
}

float Engine::computeObstruction() const {
    // Isto custava ~7 ms POR FRAME em parana_field - o maior item do app_update
    // inteiro, maior que todos os passes de GPU somados no preset low. O motivo:
    // gatherVisibleObjects monta um vetor com TODOS os objetos visiveis (5357
    // instancias) e raycasta na CPU, do zero, a cada frame.
    //
    // O resultado alimenta m_adaptiveFocalOffset, que e' um seguidor AMORTECIDO
    // (lerp por dt). Reamostrar a 60 Hz nao muda nada que o olho pegue: o proprio
    // amortecimento ja atrasa a resposta muito mais que o intervalo daqui.
    //
    // ERUPTION_OBSTRUCTION_INTERVAL sobrescreve (1 = comportamento antigo).
    static const int kInterval = [] {
        if (const char* e = std::getenv("ERUPTION_OBSTRUCTION_INTERVAL"))
            return std::clamp(std::atoi(e), 1, 60);
        return 4;
    }();
    if (m_obstructionValid && m_obstructionAge + 1 < kInterval) {
        ++m_obstructionAge;
        return m_obstructionCached;
    }
    m_obstructionAge = 0;
    m_obstructionValid = true;
    m_obstructionCached = computeObstructionUncached();
    return m_obstructionCached;
}

float Engine::computeObstructionUncached() const {
    switch (m_postSettings.cocAdaptiveFocalMethod) {
        case PostProcessor::PostSettings::AdaptiveFocalMethod::SingleRay:
            return computeObstructionSingleRay();
        case PostProcessor::PostSettings::AdaptiveFocalMethod::MultiRayGrid:
            return computeObstructionMultiRay();
        case PostProcessor::PostSettings::AdaptiveFocalMethod::ScreenSpace:
            return computeObstructionScreenSpace();
        case PostProcessor::PostSettings::AdaptiveFocalMethod::AngularRadius:
            return computeObstructionAngular();
    }
    return 0.0f;
}

float Engine::computeObstructionSingleRay() const {
    auto& objs = gatherVisibleObjects(m_camera, m_terrainRenderer, m_modelRenderer, m_spriteSystem);
    float hit = raycastScene(m_camera.position(), m_camera.forward(), m_camera.orbitDistance(), objs);
    if (hit > 0.0f && hit < m_camera.orbitDistance()) {
        return 1.0f - (hit / m_camera.orbitDistance());
    }
    return 0.0f;
}

float Engine::computeObstructionMultiRay() const {
    float orbitDist = m_camera.orbitDistance();
    Mat4 invVP = glm::inverse(m_camera.viewProjNoJitter());
    int hits = 0;
    float sumDist = 0.0f;
    const float uv[3] = {0.25f, 0.5f, 0.75f};

    auto& objs = gatherVisibleObjects(m_camera, m_terrainRenderer, m_modelRenderer, m_spriteSystem);

    for (int yi = 0; yi < 3; ++yi) {
        for (int xi = 0; xi < 3; ++xi) {
            float u = uv[xi];
            float v = uv[yi];
            Vec4 nearWorld = invVP * Vec4(u * 2.0f - 1.0f, v * 2.0f - 1.0f, 0.0f, 1.0f);
            Vec4 farWorld  = invVP * Vec4(u * 2.0f - 1.0f, v * 2.0f - 1.0f, 1.0f, 1.0f);
            nearWorld /= nearWorld.w;
            farWorld  /= farWorld.w;
            Vec3 rayDir = glm::normalize(Vec3(farWorld) - Vec3(nearWorld));
            float t = raycastScene(m_camera.position(), rayDir, orbitDist, objs);
            if (t > 0.0f && t < orbitDist) {
                hits++;
                sumDist += t;
            }
        }
    }
    if (hits == 0) return 0.0f;
    float screenCoverage = static_cast<float>(hits) / 9.0f;
    float depthFactor = 1.0f - ((sumDist / hits) / orbitDist);
    return screenCoverage * depthFactor;
}

float Engine::computeObstructionScreenSpace() const {
    float orbitDist = m_camera.orbitDistance();
    Mat4 vp = m_camera.viewProjNoJitter();
    float weightedArea = 0.0f;

    auto projectAndAccumulate = [&](const Vec3& minP, const Vec3& maxP, float dist, float extraWeight) {
        Vec3 corners[8] = {
            minP, {maxP.x, minP.y, minP.z}, {minP.x, maxP.y, minP.z}, {maxP.x, maxP.y, minP.z},
            {minP.x, minP.y, maxP.z}, {maxP.x, minP.y, maxP.z}, {minP.x, maxP.y, maxP.z}, maxP
        };
        float minX = 1.0f, maxX = 0.0f, minY = 1.0f, maxY = 0.0f;
        for (int i = 0; i < 8; ++i) {
            Vec4 clip = vp * Vec4(corners[i], 1.0f);
            if (clip.w > 0.0001f) {
                float x = (clip.x / clip.w) * 0.5f + 0.5f;
                float y = (clip.y / clip.w) * 0.5f + 0.5f;
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
        minX = glm::clamp(minX, 0.0f, 1.0f);
        maxX = glm::clamp(maxX, 0.0f, 1.0f);
        minY = glm::clamp(minY, 0.0f, 1.0f);
        maxY = glm::clamp(maxY, 0.0f, 1.0f);
        float area = (maxX - minX) * (maxY - minY);
        if (dist < orbitDist) {
            weightedArea += area * (1.0f - dist / orbitDist) * extraWeight;
        }
    };

    auto& objs = gatherVisibleObjects(m_camera, m_terrainRenderer, m_modelRenderer, m_spriteSystem);
    for (const auto& obj : objs) {
        projectAndAccumulate(obj.aabb.min, obj.aabb.max, obj.dist, obj.boundingRadius > 0.0f ? 1.5f : 1.0f);
    }

    return glm::clamp(weightedArea, 0.0f, 1.0f);
}

float Engine::computeObstructionAngular() const {
    float orbitDist = m_camera.orbitDistance();
    float weightedAngular = 0.0f;

    auto& objs = gatherVisibleObjects(m_camera, m_terrainRenderer, m_modelRenderer, m_spriteSystem);
    for (const auto& obj : objs) {
        if (obj.dist < orbitDist && obj.dist > 0.0001f) {
            float angularRadius = obj.boundingRadius / obj.dist;
            float angularArea = glm::pi<float>() * angularRadius * angularRadius;
            float weight = (obj.boundingRadius > 0.0f) ? 1.5f : 1.0f;
            weightedAngular += angularArea * (1.0f - obj.dist / orbitDist) * weight;
        }
    }

    return glm::clamp(weightedAngular * 2.0f, 0.0f, 1.0f);
}


Mat4 Engine::computeCloudShadowMatrix(const Vec3& lightDir, const Vec3& target) const {
    Vec3 l = glm::normalize(lightDir);
    if (glm::length(l) < 0.001f) l = Vec3(0.0f, -1.0f, 0.0f);

    Vec3 up = std::abs(l.y) < 0.99f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);

    // Orthographic frustum large enough to cover the cloud layer around the camera.
    float halfSize = 1200.0f;
    float lightPitchFactor = 1.0f / std::max(std::abs(l.y), 0.15f);
    float eyeOffset = 3000.0f * lightPitchFactor;
    float depthRange = 6000.0f * lightPitchFactor;

    Vec3 center = Vec3(target.x, 0.0f, target.z);
    Mat4 view = glm::lookAt(center - l * eyeOffset, center, up);
    Mat4 proj = glm::ortho(-halfSize, halfSize, -halfSize, halfSize, 0.1f, depthRange);
    proj[1][1] *= -1.0f; // Vulkan Y-flip

    return proj * view;
}

static void envOverrideBool(const char* name, bool& out) {
    if (const char* e = std::getenv(name)) out = std::stof(e) != 0.0f;
}

void Engine::applyGraphicsPreset() {
    const json& root = m_graphicsConfig.root();
    if (!root.is_object()) return;
    // ERUPTION_TEST_GRAPHICS_PRESET=low|medium|high (debug): força o preset
    // sem editar data/graphics.json (medição A/B de custo por preset).
    const char* presetEnv = std::getenv("ERUPTION_TEST_GRAPHICS_PRESET");
    const std::string active = (presetEnv && *presetEnv) ? std::string(presetEnv)
                                                         : root.value("preset", "high");
    json presets = root.value("presets", json::object());
    if (!presets.contains(active)) {
        ERUPTION_LOG_WARN("[GRAPHICS] preset '%s' not found in data/graphics.json", active.c_str());
        return;
    }
    const json& p = presets[active];

    // RELEVO POR DISTANCIA (bloco de topo, comum a todos os presets). Duas
    // curvas de quatro nos: forca do normal map e vies de mip, avaliadas por
    // fragmento com a distancia ate' a camera (ver FrameUBO::normalDistKnots
    // e o curve4 do model.frag). Os nos padrao saem do mapeamento focal do
    // DoF em data/postprocess.json - focal_offset 35, focal_distance 100,
    // focal_distance+focal_range 180 - para o relevo mudar junto com o que a
    // lente trata como perto, em foco e longe.
    if (root.contains("normal_distance")) {
        const json& nd = root["normal_distance"];
        m_normalDistCurve = nd.value("enabled", m_normalDistCurve);
        m_shadowOpacityCurve = nd.value("shadow_opacity_enabled", m_shadowOpacityCurve);
        m_normalDistMax = std::max(nd.value("max_distance", m_normalDistMax), 1.0f);
        // Cada curva e' uma lista de pontos no MESMO formato das outras curvas
        // da engine ([{x,y,interpolation}], X normalizado 0..1 = 0..max), entao
        // o editor de spline do F2 salva e carrega sem conversao nenhuma.
        auto readCurve = [&](const char* key, SplineCurve& dst) {
            if (nd.contains(key) && nd[key].is_array() && nd[key].size() >= 2)
                dst = SplineCurve::fromJson(nd[key]);
        };
        readCurve("scale_curve", m_normalScaleCurve);
        readCurve("smoothing_curve", m_normalSmoothCurve);
        readCurve("shadow_opacity_curve", m_shadowOpacitySpline);
        readCurve("tessellation_curve", m_tessCurve);
        m_tessEnabled = nd.value("tessellation_enabled", m_tessEnabled);
        m_tessAmplitude = std::clamp(nd.value("tessellation_amplitude", m_tessAmplitude), 0.0f, 10.0f);
        m_tessHeightGain = std::clamp(nd.value("tessellation_height_gain", m_tessHeightGain), 0.1f, 8.0f);
        m_tessHeightBlur = std::clamp(nd.value("tessellation_height_blur", m_tessHeightBlur), 0.0f, 4.0f);
        m_tessVertexSpacing = std::clamp(nd.value("tessellation_vertex_spacing", m_tessVertexSpacing), 0.0f, 4.0f);
    }
    envOverrideBool("ERUPTION_TEST_NORMAL_DIST", m_normalDistCurve);
    // Overrides de bancada da tesselacao DEPOIS do graphics.json - no
    // construtor eles eram sobrescritos pelo JSON e o teste nao valia nada.
    if (const char* e = std::getenv("ERUPTION_TEST_TESS_HEIGHT_GAIN")) m_tessHeightGain = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_TESS_HEIGHT_BLUR")) m_tessHeightBlur = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_TESS_SPACING")) m_tessVertexSpacing = std::stof(e);

    if (p.contains("weather_tier")) {
        const std::string tier = p["weather_tier"];
        m_postSettings.weatherTier = (tier == "mobile") ? WeatherTier::Mobile
                                   : (tier == "medium") ? WeatherTier::Medium
                                                        : WeatherTier::High;
    }
    if (p.contains("shadow_atlas")) {
        int atlas = std::clamp<int>(p["shadow_atlas"], 512, 8192);
        // BANCADA: ERUPTION_SHADOW_ATLAS=<lado>. Tem que ser AQUI. O preset
        // grafico e' o ultimo a falar sobre o atlas - tentei antes no
        // loadFromJson e no proprio ShadowRenderer::init, e nos dois casos
        // este bloco desfazia o override em seguida. A assinatura de que o
        // knob nao esta' pegando e' medida identica em todos os tamanhos COM
        // imagem pixel a pixel igual: se o atlas mudasse de verdade, a
        // qualidade da sombra mudaria junto.
        if (const char* e = std::getenv("ERUPTION_SHADOW_ATLAS")) {
            const int v = std::atoi(e);
            if (v > 0) atlas = std::clamp(v, 512, 8192);
        }
        m_shadowRenderer.settings().atlasSize = atlas;
        m_shadowRenderer.resizeAtlas(static_cast<uint32_t>(atlas));
    }
    if (p.contains("ssao")) m_ssaoEnabled = p["ssao"];
    envOverrideBool("ERUPTION_TEST_SSAO", m_ssaoEnabled);
    // FXAA no fim do post + upscale bicubico/nitidez, no lugar do bilinear
    // cru. `aa_sharpen` 0 = so' o upscale, sem nitidez (equivalente visual ao
    // upscale antigo, so' que menos borrado por ser bicubico em vez de
    // bilinear). ERUPTION_TEST_NO_AA=1 desliga o FXAA pra A/B.
    if (p.contains("fxaa")) m_enableFXAA = p["fxaa"];
    if (p.contains("aa_sharpen")) m_aaSharpenAmount = std::clamp<float>(p["aa_sharpen"], 0.0f, 1.0f);
    if (std::getenv("ERUPTION_TEST_NO_AA")) m_enableFXAA = false;
    if (p.contains("motion_blur")) m_postSettings.enableMotionBlur = p["motion_blur"];
    if (p.contains("chromatic_aberration")) m_postSettings.enableChromaticAberration = p["chromatic_aberration"];
    if (p.contains("tilt_shift")) m_postSettings.enableDoF = p["tilt_shift"];
    // RenderEffect toggles: 0 Fog, 1 GI, 2 Bloom, 3 DoF, 4 Point Lights, 7 Water.
    // Shadows/skybox/sprites are never preset-disabled ("se tem sombra, mantenha").
    auto setEffect = [&](size_t idx, const char* key) {
        if (p.contains(key) && m_renderEffects.size() > idx)
            m_renderEffects[idx]->setEnabled(p[key].get<bool>());
    };
    setEffect(0, "fog");
    setEffect(1, "gi");
    setEffect(2, "bloom");
    setEffect(3, "dof");
    // O passe le' m_postSettings.enableDoF, e ate' agora esse campo so'
    // era sincronizado com o toggle DENTRO do bloco da UI do F2 - que nunca
    // roda headless. Resultado: o knob "dof" do preset nao desligava nada.
    // Medido em cidade-A 1920x1080: DoF custava 3,87 ms com dof=false.
    if (p.contains("dof")) m_postSettings.enableDoF = p["dof"].get<bool>();
    setEffect(4, "point_lights");
    setEffect(7, "water");
    m_maxRainFollowers = std::clamp<int>(
        p.value("max_rain_followers", m_maxRainFollowers), 1,
        static_cast<int>(WeatherRenderer::MAX_RAIN_OCCLUDERS));
    m_maxFieldClouds = std::clamp<int>(p.value("max_field_clouds", m_maxFieldClouds), 4, 100);

    // Fumaça de mapa (emissores do .env). No preset low ela pode ser
    // desligada por completo ("smoke": false) ou só barateada: menos passos
    // de ray-march, sem a amostra extra na direção do sol e sem a 3a oitava.
    m_smokeEnabled = p.value("smoke", true);
    m_smokeMaxSteps = std::clamp<int>(p.value("smoke_steps", 32), 6, 96);
    m_smokeSunTap = p.value("smoke_sun_tap", true);
    m_smokeDetail = p.value("smoke_detail", true);

    if (p.contains("pom_height_scale")) m_pomHeightScale = std::clamp<float>(p["pom_height_scale"], 0.0f, 0.1f);
    if (p.contains("pom_max_steps")) m_pomMaxSteps = std::clamp<int>(p["pom_max_steps"], 2, 32);
    if (p.contains("pom_min_steps")) m_pomMinSteps = std::clamp<int>(p["pom_min_steps"], 1, 16);
    // Fill light (fake 3D na sombra): relevo do normal map continua legível
    // onde o sol não bate. 0 = desligado.
    m_deferredLighting.setFillLight(
        std::clamp<float>(p.value("fill_light", 0.25f), 0.0f, 1.5f));

    // Contact shadows: marcha em espaço de tela que pode gerar padrão
    // triangular/moiré em superfície quase plana com normal map. Preset pode
    // desligar (ganho de custo + some o artefato).
    if (p.contains("contact_shadows")) m_contactShadowEnabled = p["contact_shadows"];
    envOverrideBool("ERUPTION_TEST_CONTACT_SHADOW", m_contactShadowEnabled);
    // SONDA DE CEU (G36). ERUPTION_TEST_NO_SKY_PROBE=1 desliga (A/B).
    if (p.contains("sky_probe")) m_skyProbeEnabled = p["sky_probe"];
    if (std::getenv("ERUPTION_TEST_NO_SKY_PROBE")) m_skyProbeEnabled = false;
    m_skyProbeIntensity = std::clamp<float>(p.value("sky_probe_intensity", 1.0f), 0.0f, 4.0f);
    // PROBES DE IRRADIANCIA (G38). ERUPTION_TEST_NO_PROBES=1 desliga (A/B).
    if (p.contains("irradiance_probes")) m_irradianceProbesEnabled = p["irradiance_probes"];
    if (std::getenv("ERUPTION_TEST_NO_PROBES")) m_irradianceProbesEnabled = false;
    m_irradianceProbeStrength = std::clamp<float>(p.value("irradiance_probe_strength", 1.0f), 0.0f, 1.0f);

    // Síntese de PBR (relevo derivado onde não há PBR autoral): custa ~1,9 ms
    // de GPU por cobrir todos os materiais. Preset low abre mão.
    setPbrSynthesisEnabled(p.value("pbr_synthesis", true));

    // Teto de resolução de textura. Os 20 sheets 2048² do parana são a maior
    // parte dos pixels do mapa; o teto é o botão mais direto de VRAM. Vale
    // para as texturas EMBUTIDAS do GLB (é onde estão os sheets grandes).
    m_textureMaxSize = std::clamp<int>(p.value("texture_max_size", 1024), 128, 4096);
    setEmbeddedTextureMaxSize(static_cast<uint32_t>(m_textureMaxSize));

    // LOD geométrico (mipmap para malha). Orçamento em MB limita a memória
    // total gasta com malhas finas; 0 desliga.
    m_geoLodBudgetBytes = static_cast<size_t>(
        std::clamp<int>(p.value("geo_lod_budget_mb", 0), 0, 512)) * 1024u * 1024u;
    m_geoLodTargetEdge = std::clamp<float>(p.value("geo_lod_edge", 8.0f), 2.0f, 64.0f);
    m_geoLodLowPass = std::clamp<float>(p.value("geo_lod_lowpass", 0.5f), 0.0f, 1.0f);
    m_geoLodDistance = std::clamp<float>(p.value("geo_lod_distance", 120.0f), 0.0f, 2000.0f);
    m_modelRenderer.setGeoLodDistance(m_geoLodDistance);

    // Banda perto do displacement (vértice): amplitude em unidades de mundo
    // + alcance da banda. 0 = off (preset low).
    m_modelRenderer.setGeoDisplacement(
        std::clamp<float>(p.value("geo_disp_amplitude", 0.0f), 0.0f, 10.0f),
        std::clamp<float>(p.value("geo_disp_distance", 0.0f), 0.0f, 500.0f));

    // LOD grosseiro de malha (meshoptimizer): instância longe desenha um index
    // buffer simplificado sobre o mesmo vertex buffer. mesh_lod_ratio = 0
    // desliga. Medido no parana_field: 713 instâncias visíveis submetiam
    // 3.902.788 triângulos por frame - é o custo dominante de GPU e o que
    // decide se o mapa roda na 930M.
    // Override de bancada: com o LOD ligado a contagem de triangulos da cena
    // nao e' a da fonte, e comparar com benchmark publicado fica enganoso.
    // ERUPTION_TEST_MESH_LOD_RATIO=0 mede a mesma cena sem LOD nenhum.
    float meshLodRatio = std::clamp<float>(p.value("mesh_lod_ratio", 0.0f), 0.0f, 0.95f);
    if (const char* lr = std::getenv("ERUPTION_TEST_MESH_LOD_RATIO")) {
        meshLodRatio = std::clamp(std::strtof(lr, nullptr), 0.0f, 0.95f);
    }
    m_modelRenderer.setMeshLod(
        meshLodRatio,
        std::clamp<float>(p.value("mesh_lod_error", 0.05f), 0.0f, 1.0f),
        std::clamp<float>(p.value("mesh_lod_distance_factor", 8.0f), 0.5f, 200.0f),
        static_cast<uint32_t>(std::clamp<int>(p.value("mesh_lod_max_tris", 0), 0, 1000000)),
        static_cast<uint32_t>(std::clamp<int>(p.value("shadow_lod_max_tris", 0), 0, 1000000)));
    // LOD distante + descarte por tamanho na tela (zoom 0 do parana_field:
    // 14,7 M tris/frame -> ruido sub-pixel, 40 FPS). far_max_tris = 0 desliga
    // o LOD distante; small_object_cull_px = 0 desliga o descarte.
    // O LOD distante e' a mesma familia: se a bancada pediu LOD 0, ele some
    // junto, senao a contagem "sem LOD" ainda viria simplificada de longe.
    int farMaxTris = std::clamp<int>(p.value("mesh_lod_far_max_tris", 0), 0, 1000000);
    if (meshLodRatio <= 0.0f) farMaxTris = 0;
    m_modelRenderer.setMeshLodFar(
        static_cast<uint32_t>(farMaxTris),
        std::clamp<float>(p.value("mesh_lod_far_distance_mul", 3.0f), 1.2f, 20.0f));
    // Orcamento de triangulos por pixel projetado (ModelRenderer::lodSwitchFor):
    // antecipa a troca de nivel onde a malha ja' e' subpixel. 0 desliga.
    // ERUPTION_TEST_LOD_TRI_PER_PX=<f> sobrepoe o preset (A/B de bancada).
    float lodTrisPerPx = std::clamp<float>(p.value("mesh_lod_tris_per_px", 0.0f), 0.0f, 16.0f);
    if (const char* tp = std::getenv("ERUPTION_TEST_LOD_TRI_PER_PX")) {
        lodTrisPerPx = std::clamp(std::strtof(tp, nullptr), 0.0f, 16.0f);
    }
    if (meshLodRatio <= 0.0f) lodTrisPerPx = 0.0f;
    m_modelRenderer.setMeshLodTrisPerPx(lodTrisPerPx);
    m_modelRenderer.setScreenCull(std::clamp<float>(p.value("small_object_cull_px", 0.0f), 0.0f, 16.0f));
    // Escala do balanco da vegetacao ao vento (0 desliga). Chega ao vertex
    // shader em windParams.y.
    m_windSwayScale = std::clamp<float>(p.value("wind_sway_scale", 0.15f), 0.0f, 2.0f);
    m_modelRenderer.setShadowMinPx(std::clamp<float>(p.value("shadow_caster_min_px", 0.0f), 0.0f, 16.0f));
    // Pisos do LOD: malha pequena nao gera LOD e nada troca perto da camera.
    // ERUPTION_TEST_LOD_MIN_DIST="lo[,far]" (bancada de video): pisos em
    // unidades de mundo a partir do OLHO. A camera de diorama fica a 400-800 u
    // do alvo, e com os pisos do preset (300 u) tudo que interessa ja' cai no
    // lo/far ("o LOD ta' destruindo a geometria" - autor). Sobrepoe o preset.
    float lodMinDist = p.value("mesh_lod_min_distance", 0.0f);
    float lodFarMinDist = 0.0f;
    if (const char* md = std::getenv("ERUPTION_TEST_LOD_MIN_DIST")) {
        float lo = 0.0f, fr = 0.0f;
        const int n = std::sscanf(md, "%f,%f", &lo, &fr);
        if (n >= 1) lodMinDist = std::max(lo, 0.0f);
        if (n >= 2) lodFarMinDist = std::max(fr, 0.0f);
    }
    m_modelRenderer.setMeshLodFloors(
        static_cast<uint32_t>(std::max(0, p.value("mesh_lod_min_tris", 800))),
        lodMinDist, lodFarMinDist);

    // Orçamento de passos do ray-march de nuvem. "Cloud Volumes (field)" é o
    // passe de GPU mais caro num mapa típico (1,15 ms em cidade-A, 1,60 ms em
    // vila-A na 4060, contra 0,38 ms do GBuffer) - é a maior alavanca isolada
    // para a 930M. 0 = máximo do shader (32).
    m_cloudLayerRenderer.setMarchStepBudget(
        std::clamp<int>(p.value("cloud_march_steps", 0), 0, 32));

    // Top-level battery/present controls (independent of the preset).
    m_fpsCap = std::clamp<int>(root.value("fps_cap", m_fpsCap), 0, 1000);
    const std::string pm = root.value("present_mode", "auto");
    const bool wantFifo = (pm == "fifo" || pm == "vsync");
    const bool wantImmediate = (pm == "immediate");
    if (m_vulkan.setPresentPreference(wantFifo, wantImmediate)) {
        m_vulkan.recreateSwapchain();
    }
    ERUPTION_LOG_INFO("[GRAPHICS] preset '%s' applied (tier=%d atlas=%d ssao=%d followers=%d clouds=%d)",
                      active.c_str(), static_cast<int>(m_postSettings.weatherTier),
                      m_shadowRenderer.settings().atlasSize, m_ssaoEnabled ? 1 : 0,
                      m_maxRainFollowers, m_maxFieldClouds);
}

void Engine::recordTelemetry() {
    if (!TelemetryExporter::enabled()) return;
    TelemetryExporter::FrameStats st;
    st.frame = m_framesCount;
    st.fps = m_fps;
    st.frameMs = m_frameTime;
    // Delta REAL deste frame (relogio de parede, imune ao passo fixo). fps e
    // frameMs acima vem da janela de 0,5 s e nao servem pra achar engasgo -
    // ver o comentario em FrameStats.
    st.frameDtMs = m_timer.realDeltaTime() * 1000.0f;
    using PS = VulkanContext::PipeStat;
    st.pipeInputPrimitives    = m_vulkan.pipelineStat(PS::InputPrimitives);
    st.pipeClippedPrimitives  = m_vulkan.pipelineStat(PS::ClippedPrimitives);
    st.pipeVertexInvocations  = m_vulkan.pipelineStat(PS::VertexInvocations);
    st.pipeFragInvocations    = m_vulkan.pipelineStat(PS::FragmentInvocations);
    st.cpuAdvanceMs = m_cpuAdvanceMs;
    st.cpuAppUpdateMs = m_cpuAppUpdateMs;
    st.cpuRenderMs = m_cpuRenderMs;
    st.cpuBeginWaitMs = m_cpuBeginFrameMs;
    st.cpuTailMs = m_cpuTailMs;
    st.presentMs = m_vulkan.lastPresentMs();
    st.instances = static_cast<uint32_t>(m_modelRenderer.getInstances().size());
    st.meshes = static_cast<uint32_t>(m_modelRenderer.getMeshes().size());
    st.visibleChunks = m_visibleChunks;
    st.triModels = m_modelRenderer.lastDrawnTriangles();
    st.triTerrain = m_terrainRenderer.lastDrawnTriangles();
    st.drawCalls = m_modelRenderer.lastDrawCalls();
    st.batchReuse = m_modelRenderer.lastCacheHits();
    st.batchRuns  = m_modelRenderer.lastCacheMisses();
    for (size_t i = 1; i < m_cpuPhaseMs.size(); ++i)
        st.cpuPhases.emplace_back(kCpuPhaseNames[i], static_cast<float>(m_cpuPhaseMs[i]));
    // "loading" covers the whole pipeline: BG thread parse + the per-frame
    // GPU upload window (staging context alive until the swap).
    st.loading = (m_backgroundLoader && m_backgroundLoader->isLoading()) ||
                 (m_stagingMapContext != nullptr);
    st.loadProgress = m_backgroundLoader ? m_backgroundLoader->progress() : 1.0f;
    st.framesAfterSwap = m_framesAfterSwap;
    st.weatherType = weatherTypeName(m_weatherSystem.currentType());
    st.rainIntensity = m_weatherSystem.effectiveRainIntensity();
    st.snowIntensity = m_weatherSystem.effectiveSnowIntensity();
    // VRAM do PROCESSO: soma das ALOCACOES VMA nas heaps device-local. O NVML
    // (sys.vram_mb) mede o dispositivo inteiro e não isola a engine.
    // allocationBytes e nao blockBytes: o bloco do VMA e' de 256 MB, entao
    // blockBytes so' se move de 256 em 256 MB e esconde a economia real.
    if (m_vulkan.allocator()) {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(m_vulkan.physicalDevice(), &memProps);
        std::vector<VmaBudget> budgets(VK_MAX_MEMORY_HEAPS);
        vmaGetHeapBudgets(m_vulkan.allocator(), budgets.data());
        uint64_t deviceBytes = 0;
        for (uint32_t h = 0; h < memProps.memoryHeapCount; ++h) {
            if (memProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                deviceBytes += budgets[h].statistics.allocationBytes;
        }
        st.vmaDeviceMb = static_cast<double>(deviceBytes) / (1024.0 * 1024.0);
    }
    TelemetryExporter::recordFrame(st);
}

void Engine::initImGui() {
    VkDescriptorPoolSize poolSizes[] = { {VK_DESCRIPTOR_TYPE_SAMPLER, 1000}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000}, {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000} };
    VkDescriptorPoolCreateInfo poolInfo{}; poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)); poolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(m_vulkan.device(), &poolInfo, nullptr, &m_imguiPool);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Layout dos painéis do editor, por usuário (fora do git).
    if (m_editorMode) io.IniFilename = "editor_layout.ini";
    io.Fonts->Clear();
    // O editor usa uma fonte proporcional do sistema quando existe; o jogo
    // mantem a fonte embutida do ImGui.
    bool uiFont = false;
    if (m_editorMode) {
        static const char* kUiFonts[] = {
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
        };
        for (const char* f : kUiFonts) {
            if (!std::filesystem::exists(f)) continue;
            static const ImWchar kLatin[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0 };
            ImFontConfig uiCfg; uiCfg.OversampleH = 2; uiCfg.OversampleV = 2;
            uiFont = io.Fonts->AddFontFromFileTTF(f, 16.0f, &uiCfg, kLatin) != nullptr;
            if (uiFont) break;
        }
    }
    ImFontConfig fontCfg; fontCfg.SizePixels = 14.0f;
    if (!uiFont) io.Fonts->AddFontDefault(&fontCfg);
    if (std::filesystem::exists("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")) {
        static const ImWchar koreanRanges[] = { 0xAC00, 0xD7A3, 0x3131, 0x318E, 0x4E00, 0x9FFF, 0xFF00, 0xFFEF, 0 };
        ImFontConfig cjkCfg; cjkCfg.MergeMode = true; cjkCfg.SizePixels = 14.0f; cjkCfg.OversampleH = 1; cjkCfg.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 14.0f, &cjkCfg, koreanRanges);
    }
    io.Fonts->Build();
    ImGui_ImplGlfw_InitForVulkan(m_window.handle(), false);
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = m_vulkan.instance(); initInfo.PhysicalDevice = m_vulkan.physicalDevice(); initInfo.Device = m_vulkan.device();
    initInfo.QueueFamily = m_vulkan.queueFamilies().graphicsFamily; initInfo.Queue = m_vulkan.graphicsQueue();
    initInfo.DescriptorPool = m_imguiPool; initInfo.MinImageCount = 3; initInfo.ImageCount = 3;
    // Render pass compatível com o swapchain (Vulkan 1.1: sem dynamic rendering).
    initInfo.UseDynamicRendering = false;
    initInfo.RenderPass = vkc::compatibleRenderPass({m_vulkan.swapFormat()}, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED);
    initInfo.Subpass = 0;
    ImGui_ImplVulkan_Init(&initInfo);
}

void Engine::setViewportSize(uint32_t w, uint32_t h) {
    w = std::max(64u, w);
    h = std::max(64u, h);
    if (w == m_viewportW && h == m_viewportH) return;
    m_viewportW = w;
    m_viewportH = h;
    m_resized = true;
}

void Engine::destroyViewportTarget() {
    if (m_viewportTexture != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(m_viewportTexture);
    if (m_viewportView != VK_NULL_HANDLE) vkDestroyImageView(m_vulkan.device(), m_viewportView, nullptr);
    if (m_viewportImage != VK_NULL_HANDLE) vmaDestroyImage(m_vulkan.allocator(), m_viewportImage, m_viewportAlloc);
    m_viewportTexture = VK_NULL_HANDLE;
    m_viewportView = VK_NULL_HANDLE;
    m_viewportImage = VK_NULL_HANDLE;
    m_viewportAlloc = VK_NULL_HANDLE;
}

// Chamado so' dentro do bloco de resize do render(), depois do waitIdle do
// swapchain: a imagem antiga nao esta' mais em uso por nenhum frame.
void Engine::recreateViewportTarget() {
    destroyViewportTarget();
    if (!m_editor || !m_viewportW || !m_viewportH) return;
    const VkFormat fmt = m_vulkan.swapFormat();
    if (!m_vulkan.createImage(m_viewportW, m_viewportH, fmt,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VMA_MEMORY_USAGE_GPU_ONLY, m_viewportImage, m_viewportAlloc)) {
        ERUPTION_LOG_ERROR("Editor: falha ao criar a imagem da cena %ux%u", m_viewportW, m_viewportH);
        m_viewportImage = VK_NULL_HANDLE;
        m_viewportAlloc = VK_NULL_HANDLE;
        return;
    }
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_viewportImage; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_vulkan.device(), &vi, nullptr, &m_viewportView);
    const VkSampler sampler = m_uiDefaultSampler != VK_NULL_HANDLE ? m_uiDefaultSampler : m_defaultSampler;
    m_viewportTexture = ImGui_ImplVulkan_AddTexture(sampler, m_viewportView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Engine::shutdownImGui() {
    ImGui_ImplVulkan_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    if (m_imguiPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_vulkan.device(), m_imguiPool, nullptr);
}

void Engine::onWindowResize(int width, int height) {
    if (width == 0 || height == 0) return;
    m_resized = true;
}

} // namespace eruption
