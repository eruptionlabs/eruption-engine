// Gravacao do frame principal: Engine::render(). Saiu do Engine.cpp em
// 2026-09-04 (segunda fatia da quebra do arquivo, depois do painel de debug).
// Sozinha eram ~1.760 linhas - e' a funcao que orquestra os passes na ordem
// (sombra, minimapa, G-buffer, iluminacao diferida, agua, ceu/nuvem/sprite,
// clima, pos-processamento, copia pro swapchain, ImGui) e marca as fases de
// CPU que a telemetria exporta.
//
// Continua a MESMA classe Engine, mesmo Engine.hpp - so' mudou a unidade de
// traducao. Nenhuma logica mudou: recorta-e-cola verificado por screenshot
// deterministico (RMS 0.0 contra o binario anterior).

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
#include <unordered_map>
#include <filesystem>
#include <cctype>

#include "utils/ImageUtils.hpp"
#include "utils/TextureCache.hpp"
#include <vk_mem_alloc.h>

namespace eruption {

void Engine::render() {
    static int frameCount = 0; frameCount++;

    // ERUPTION_TEST_DEMO (bancada de VIDEO): roteiro por tempo, uma execucao,
    // um take - ver Engine::runDemoTimeline abaixo de toggleWireframe.
    // NO TOPO do frame, de proposito: ficava junto dos outros hooks de camera
    // (fim do render) e as cascatas de sombra, o cull e o LOD ja' tinham
    // lido a camera de ANTES do keyframe - e sem free camera o
    // PlayerController ainda puxava o alvo pro sprite entre um frame e outro.
    // Resultado: sombra num lugar, imagem noutro, trocando a cada frame
    // ("a sombra ta' flickando de uma forma muito ruim" - autor).
    static const char* demoSpec = std::getenv("ERUPTION_TEST_DEMO");
    if (demoSpec) runDemoTimeline(demoSpec);

    // Headless screenshot/benchmark runs: disable all debug overlays so the
    // captured frame matches what a player sees in normal gameplay.
    if (m_enableAutoExit) {
        m_debugModelPivots = false;
        m_shadowRenderer.settings().debugLightRays = false;
        m_shadowRenderer.settings().debugMagentaShadow = false;
    }

    if (m_resized) {
        m_vulkan.recreateSwapchain();
        uint32_t w = m_vulkan.swapExtent().width, h = m_vulkan.swapExtent().height;
        if (m_renderW && m_renderH && m_vulkan.swapExtent().width) {
            const float rs = float(m_renderW) / float(m_vulkan.swapExtent().width);
            if (rs < 0.999f) { w = std::max(64u, uint32_t(w * rs)); h = std::max(64u, uint32_t(h * rs)); }
        }
        m_renderW = w; m_renderH = h;
        m_gbuffer.resize(w, h); m_deferredLighting.resize(w, h); m_skybox.resize(w, h);
        m_postProcessor.resize(std::max(64u, static_cast<uint32_t>(w * m_postScale)),
                               std::max(64u, static_cast<uint32_t>(h * m_postScale)));
        m_upscaleAA.resizeRenderTarget(m_postProcessor.width(), m_postProcessor.height(),
                                       postColorFormat(m_vulkan.physicalDevice()));
        // PostProcessor::resize() destroi e recria m_outputView (handle NOVO)
        // - o descriptor do FXAA precisa apontar pro handle atual.
        m_upscaleAA.bindSource(m_postProcessor.outputView());
        m_water.setSkyTexture(m_skybox.outputView(), m_skybox.outputSampler());
        m_camera.setPerspective(60.0f, (float)w / (float)h, 0.1f, 50000.0f);
        m_resized = false;
    }
    // Process deferred deletion of old map contexts
    for (auto it = m_deferredDeletes.begin(); it != m_deferredDeletes.end(); ) {
        it->framesRemaining--;
        if (it->framesRemaining <= 0) {
            if (it->context) {
                it->context->clearGpu(&m_vulkan, &m_bindless);
            }
            it = m_deferredDeletes.erase(it);
        } else {
            ++it;
        }
    }

    // Split the pre-render cost into "everything since the last present"
    // (update/sim) vs "blocked in beginFrame" (fence wait + image acquire =
    // GPU / vsync back-pressure). Without this the frame time looks unexplained
    // by the per-stage marks. Always measured: it is two clock reads a frame,
    // and the debug overlay (F3) shows the result.
    {
        static std::chrono::steady_clock::time_point s_lastPresentEnd{};
        auto t = std::chrono::steady_clock::now();
        if (s_lastPresentEnd.time_since_epoch().count() != 0) {
            m_cpuUpdateMs = static_cast<float>(
                std::chrono::duration<double, std::milli>(t - s_lastPresentEnd).count());
        }
        s_lastPresentEnd = t;
        if (!m_vulkan.beginFrame()) return;
        m_cpuBeginFrameMs = static_cast<float>(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t).count());
    }

    // Read back shimmer metric from the oldest completed frame slot
    if (m_shimmerMeasureEnabled && m_shimmerFrameCount >= 3) {
        uint32_t frameIdx = m_vulkan.currentFrame();
        void* mappedData = nullptr;
        if (vmaMapMemory(m_vulkan.allocator(), m_shimmerBufferAllocs[frameIdx], &mappedData) == VK_SUCCESS) {
            const uint8_t* pixels = reinterpret_cast<const uint8_t*>(mappedData);
            double sumDelta = 0.0;
            size_t pixelCount = 128 * 128;
            for (size_t i = 0; i < pixelCount; i++) {
                uint8_t r = pixels[i * 4 + 0];
                uint8_t g = pixels[i * 4 + 1];
                uint8_t b = pixels[i * 4 + 2];
                double luma = 0.299 * r + 0.587 * g + 0.114 * b;
                
                uint8_t prevR = m_shimmerPrevPixels[i * 4 + 0];
                uint8_t prevG = m_shimmerPrevPixels[i * 4 + 1];
                uint8_t prevB = m_shimmerPrevPixels[i * 4 + 2];
                double prevLuma = 0.299 * prevR + 0.587 * prevG + 0.114 * prevB;
                
                sumDelta += std::abs(luma - prevLuma);
            }
            m_shimmerCurrentValue = static_cast<float>(sumDelta / pixelCount);
            
            // Push to history. (Sim, e' O(n) - mas sao 120 floats = 480 B,
            // ~8 cache lines, e SO' roda com a metrica de shimmer ligada.
            // Trocar por ring buffer indexado nao mediu diferenca; deixado
            // simples de proposito. Varredura de hot paths 2026-09-02.)
            m_shimmerHistory.erase(m_shimmerHistory.begin());
            m_shimmerHistory.push_back(m_shimmerCurrentValue);
            
            // Save pixels
            std::memcpy(m_shimmerPrevPixels.data(), pixels, pixelCount * 4);
            
            vmaUnmapMemory(m_vulkan.allocator(), m_shimmerBufferAllocs[frameIdx]);
        }
    }

    // Start ImGui frame only after a successful beginFrame (skip during
    // automated benchmarks to remove UI overhead from measurements).
    if (!m_benchmarkThunderstorm) {
        ImGui_ImplVulkan_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        // Precisa rodar 1x por frame de ImGui, antes de qualquer
        // ImGuizmo::Manipulate() (Object Manager F1, Lava shape no Liquids) -
        // documentado assim no proprio ImGuizmo.h.
        //
        // FIX CONFIRMADO EM PIXEL (2026-09-04) da "linha cinza" relatada pelo
        // autor. ImGuizmo::BeginFrame() (third_party/ImGuizmo/ImGuizmo.cpp)
        // abre uma janela ImGui "gizmo" do tamanho da TELA INTEIRA todo
        // frame, mesmo sem gizmo nenhum ativo (guarda o DrawList que
        // Manipulate() usa depois). Ela zera o FUNDO
        // (PushStyleColor(WindowBg, 0)) mas NAO desliga a BORDA - no
        // imgui.cpp (RenderWindowOuterBorders), a borda so' e' pulada quando
        // a flag NoBackground esta' setada, e um push de cor com alfa 0 nao
        // conta. Sem StyleColorsDark() chamado em lugar nenhum do projeto
        // (ela roda de qualquer jeito, e' a ULTIMA linha do construtor de
        // ImGuiStyle), a cor de borda default (cinza, ~50% alfa) desenhava
        // ao redor da tela inteira, todo frame, ANTES de qualquer selecao.
        //
        // A janela nunca recebe SetNextWindowPos, e ImGuiWindowFlags_
        // NoSavedSettings tira ela do imgui.ini - entao a posicao e' o
        // fallback padrao do ImGui pra uma janela nunca vista, medido nesta
        // maquina em (60, 60) via ERUPTION_TEST_SWAP_SHOT (screenshot do
        // SWAPCHAIN, depois do ImGui compor por cima - o --screenshot normal
        // le' m_postProcessor.outputImage() e sai ANTES do passe de UI,
        // entao e' cego pra qualquer coisa desenhada pelo ImGui: nenhuma das
        // capturas anteriores nesta investigacao podia ter mostrado a
        // linha). Nessa posicao a borda cai exatamente perto de "Object
        // Manager" (linha 1 do painel Shortcuts) na horizontal e entre os
        // dois 'f' de "Effects / Post-Process" na vertical - bate com a
        // descricao do autor. Prova em pixel: linha 60 media 79,3 contra
        // ~38 nas vizinhas (58/59), coluna 60 media 77,5 contra ~26 nas
        // vizinhas - diff entre COM e SEM a correcao: 1713 pixels no total,
        // 848 na linha 60 e 866 na coluna 60 (praticamente 100% do efeito).
        //
        // Zera a espessura da borda so' pra esta janela em vez de mexer na
        // lib vendorizada. ERUPTION_TEST_GIZMO_BORDER=1 desliga a correcao
        // (reproduz o bug, pra comparacao). ERUPTION_DEBUG_GIZMOWIN=1
        // imprime a posicao/tamanho reais da janela "gizmo" uma vez.
        static const bool kDbgGizmoWin = std::getenv("ERUPTION_DEBUG_GIZMOWIN") != nullptr;
        static const bool kNoBorderFix = std::getenv("ERUPTION_TEST_GIZMO_BORDER") != nullptr;
        if (!kNoBorderFix) ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuizmo::BeginFrame();
        if (!kNoBorderFix) ImGui::PopStyleVar();
        if (kDbgGizmoWin) {
            static bool printed = false;
            if (!printed) {
                if (ImGuiWindow* gw = ImGui::FindWindowByName("gizmo")) {
                    printed = true;
                    ERUPTION_LOG_WARN("[GIZMOWIN] pos=%.1f,%.1f size=%.1f,%.1f borderFix=%d",
                                      gw->Pos.x, gw->Pos.y, gw->Size.x, gw->Size.y, !kNoBorderFix);
                }
            }
        }
    }

    VkCommandBuffer cmd = m_vulkan.currentCmdBuf();
    uint32_t imageIndex = m_vulkan.currentImageIndex();

    // ERUPTION_TEST_CPU_MARKS=1 (debug): collect the per-stage CPU timings in any
    // run, not just the benchmark modes, so a plain --screenshot capture can
    // report where the frame time goes on the CPU side.
    static const bool forceCpuMarks = std::getenv("ERUPTION_TEST_CPU_MARKS") != nullptr;
    auto cpuMark = [&](size_t idx) {
        // Telemetria ligada tambem liga as marcas: 12 leituras de relogio por
        // frame nao aparecem na medicao, e sem elas render_ms fica cego.
        if (forceCpuMarks || TelemetryExporter::enabled() || m_benchmarkThunderstormStarted || m_benchmarkHeavyRainPhase >= 1 ||
            !m_pendingCloudSpawns.empty() || m_showStatsMenu) {
            static std::chrono::steady_clock::time_point last;
            auto now = std::chrono::steady_clock::now();
            if (idx == 0) { last = now; m_cpuPhaseMs.fill(0.0); }
            else {
                const double ms = std::chrono::duration<double, std::milli>(now - last).count();
                m_benchmarkCpuAccum[idx] += ms;
                if (idx < m_cpuPhaseMs.size()) m_cpuPhaseMs[idx] = ms;
                if (idx < m_benchmarkCpuFrameMs.size()) m_benchmarkCpuFrameMs[idx] = ms;
            }
            last = now;
        }
    };
    cpuMark(0);

    // Incremental upload of staging map GPU resources
    uploadStagingMapGPU(cmd);
    // Fase propria: o streaming do mapa custava ate' 12ms e era atribuido a
    // "Shadow" (proxima fase) - gargalo fantasma na telemetria.
    cpuMark(1);

    // Generate cloud coverage/altitude textures on the GPU if any parameter changed.
    // This is done early in the frame before any cloud pass reads the texture.
    if (m_cloudLayersEnabled && m_cloudLayerRenderer.coverageArray().isDirty()) {
        m_cloudLayerRenderer.coverageArray().recordComputeGeneration(cmd);
    }
    // Independent cloud layers regenerate their own coverage textures.
    if (m_cloudLayersEnabled) {
        for (auto& icl : m_independentCloudLayers) {
            if (icl.array->isDirty()) icl.array->recordComputeGeneration(cmd);
        }
    }

    LightingEnvironment env;
    env.sun.direction = m_dayNightCycle.getSunDirection(); env.sun.color = m_dayNightCycle.getSunColor();
    env.sun.intensity = m_dayNightCycle.getSunIntensity(); env.ambientColor = m_dayNightCycle.getAmbientColor();
    env.ambientIntensity = m_dayNightCycle.getAmbientIntensity(); env.ambientSkyColor = m_dayNightCycle.getSkyTopColor();
    env.ambientGroundColor = m_mapGroundAlbedo;
    env.skyHorizonColor = m_dayNightCycle.getSkyHorizonColor();

    // Night fill: keep the ambient roughly at its daytime level all day/night
    // (the authored spline cranked it to ~4x at night, which flat-washed the
    // scene and killed the normal-map relief). Instead, when the sun is down,
    // drive the directional light with the MOON - a real directional keeps the
    // bump/relief and shadows alive at night, just dim and cool.
    {
        const float sunI = m_dayNightCycle.getSunIntensity();
        env.ambientIntensity = m_ambientIntensity; // constant fill; slider in Lighting panel
        const float FADE = 0.25f; // sun intensity below which the moon takes over
        if (m_moonFillEnabled && sunI < FADE) {
            float t = 1.0f - glm::clamp(sunI / FADE, 0.0f, 1.0f); // 0 at dusk -> 1 deep night
            Vec3 moonDir = -glm::normalize(m_dayNightCycle.getMoonVisualDirection()); // travel dir
            Vec3 moonCol = m_dayNightCycle.getMoonColor();
            float moonI  = m_dayNightCycle.getMoonIntensity();
            if (moonI <= 0.0f) moonI = 0.18f; // sensible default if the curve is empty
            moonI *= m_moonFillStrength;
            env.sun.direction = glm::normalize(glm::mix(env.sun.direction, moonDir, t));
            env.sun.color     = glm::mix(env.sun.color, moonCol, t);
            env.sun.intensity = glm::mix(sunI, moonI, t);
        }
    }

    // Cloud shadows: base strength = sun/(sun+ambient), so a dense cloud
    // darkens the ground like map geometry shadows (author feedback 2026-08-10).
    m_cloudLayerRenderer.setSceneAmbientIntensity(env.ambientIntensity);
    m_deferredLighting.setEnvironment(env);
    m_deferredLighting.setWeatherState(m_weatherSystem.current().rainIntensity,
                                       m_weatherSystem.current().snowIntensity,
                                       m_weatherSystem.temperatureC());

    // Sync render effects with systems every frame (was previously only done when F2 menu was open)
    m_postSettings.enableFog = m_renderEffects[0]->isEnabled();
    m_postSettings.enableDoF = m_renderEffects[3]->isEnabled();
    m_shadowRenderer.settings().enabled = m_renderEffects[5]->isEnabled();
    // [4] "Point Lights": a caixa do F2 e a chave "point_lights" do preset
    // eram escritas e nunca lidas - o `low` (930M) pagava o passe inteiro
    // mesmo pedindo pra' nao pagar. Mesmo padrao do [5] logo acima.
    if (m_renderEffects.size() > 4) m_deferredLighting.setPointLightsEnabled(m_renderEffects[4]->isEnabled());
    
    float giIntensity = m_renderEffects[1]->isEnabled() ? m_renderEffects[1]->getParam(0) : 0.0f;
    m_deferredLighting.setGlobalIllumination(env.ambientIntensity * giIntensity, 0.0f);
    m_deferredLighting.setIndirectParams(m_ssaoEnabled, m_ssaoStrength, m_ssaoRadius,
                                         m_envSpecEnabled, m_envSpecIntensity,
                                         m_nightAoContrast, m_ambientHemiFloor);
    m_deferredLighting.setBounceParams(m_sunBounceEnabled, m_sunBounceStrength);
    // ERUPTION_TEST_CONTACT_LENGTH=<unidades de mundo> (debug): varre o alcance
    // da marcha sem passar pelo slider do ImGui (que exige display e mao humana).
    static const float contactLenEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_CONTACT_LENGTH");
        return (e && *e) ? std::stof(e) : -1.0f;
    }();
    m_deferredLighting.setContactShadowParams(
        m_contactShadowEnabled,
        contactLenEnv > 0.0f ? contactLenEnv : m_contactShadowLength);

    float zoomPercent = 1.0f - (m_camera.orbitDistance() - 10.0f) / (1000.0f - 10.0f);

    // Prepare FrameUBO earlier for shadow and gbuffer passes
    FrameUBO frameUbo{};
    frameUbo.view = m_camera.viewMatrix();
    frameUbo.projection = m_camera.projectionMatrix();
    frameUbo.viewProjection = m_camera.viewProjectionMatrix();
    frameUbo.inverseView = glm::inverse(m_camera.viewMatrix());
    frameUbo.inverseProjection = glm::inverse(m_camera.projectionMatrix());
    frameUbo.cameraPos = m_camera.position();
    frameUbo.time = m_timer.elapsed();
    frameUbo.screenResolution = Vec2(static_cast<float>(renderExtent().width), static_cast<float>(renderExtent().height));
    frameUbo.nearPlane = m_camera.nearPlane();
    frameUbo.farPlane = m_camera.farPlane();
    frameUbo.frameIndex = static_cast<uint32_t>(m_framesCount);
    // MODO WIREFRAME ESTILIZADO (pedido do autor: "tudo wireframe + fundo
    // preto, wireframe verde"). debugMode 3 = wireframe: model.frag e
    // terrain.frag escrevem verde no EMISSIVE e zeram o albedo, entao o passe
    // de iluminacao entrega a linha verde sem sombra/PBR por cima. As
    // pipelines de modelo e terreno nascem em VK_POLYGON_MODE_LINE quando o
    // modo esta ligado (ERUPTION_WIREFRAME=1), e o ceu nao e' desenhado - o
    // fundo fica no preto do clear.
    frameUbo.debugMode = wireframeMode() ? 3u : 0u;
    // Adaptive billboard tilt: configured by the spline in the Sprites tab.
    // Spline stores X normalized over pitch 0..89°, so convert pitch to normalized X.
    constexpr float kTiltPitchMin = 0.0f;
    constexpr float kTiltPitchMax = 89.0f;
    float pitchDeg = glm::degrees(m_camera.orbitPitch());
    float pitchNorm = (pitchDeg - kTiltPitchMin) / (kTiltPitchMax - kTiltPitchMin);
    frameUbo.spriteTilt = m_charConfig.billboardTiltSpline.evaluate(pitchNorm);
    
    // diorama Tweaks
    frameUbo.shadowHeightScale = 0.35f; // Squash shadows on Y axis so they lay flat on terrain
    frameUbo.spriteNormalYMix = 0.85f;  // Heavily bias normals UP so sprites catch sun from above
    frameUbo.normalMapScale = m_normalMapScale;
    frameUbo.normalSmoothing = m_normalSmoothing;
    // LUT das curvas por distancia. Oito amostras uniformes em [0, max]: a
    // spline pode ter quantos pontos o autor arrastar no F2 (widget
    // SplineEditorUI), o shader so' ve' a tabela. Os sliders de Normal Map
    // Scale/Smoothing seguem como trim: a forca multiplica, o vies soma.
    auto sampleLut = [](const SplineCurve& c, Vec4& lo, Vec4& hi) {
        for (int i = 0; i < 8; ++i) {
            const float v = c.evaluate(static_cast<float>(i) / 7.0f);
            (i < 4 ? lo : hi)[i & 3] = v;
        }
    };
    const float distMax = std::max(m_normalDistMax, 1.0f);
    // z = modo de auditoria do deslocamento (ERUPTION_TEST_TESS_AUDIT, ver
    // model.frag): 1 = deslocamento x normal, 2 = luz x relevo, 3 = modulo.
    static const float kTessAudit = [] {
        const char* e = std::getenv("ERUPTION_TEST_TESS_AUDIT");
        return e ? static_cast<float>(std::atoi(e)) : 0.0f;
    }();
    frameUbo.normalDistParams = Vec4(distMax, m_normalDistCurve ? 1.0f : 0.0f, kTessAudit, m_tessHeightGain);
    sampleLut(m_normalScaleCurve, frameUbo.normalScaleLut0, frameUbo.normalScaleLut1);
    sampleLut(m_normalSmoothCurve, frameUbo.normalSmoothLut0, frameUbo.normalSmoothLut1);
    // ERUPTION_TEST_SHADOW_OPACITY=0 devolve a sombra cheia (A/B de bancada).
    static const bool kShadowOpacityEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_SHADOW_OPACITY");
        return !(e && std::atoi(e) == 0);
    }();
    // Tesselacao: mesma tabela de 8 amostras. O maximo da curva vai no .w
    // para o shader normalizar a atenuacao da amplitude.
    Vec4 tessLo(1.0f), tessHi(1.0f);
    sampleLut(m_tessCurve, tessLo, tessHi);
    // TETO DO DEVICE. A curva do F2 vai a 16 e a do JSON a qualquer coisa, mas
    // quem manda e' limits.maxTessellationGenerationLevel - todo o resto do
    // VulkanContext checa feature bit e limite, este era o unico lugar que
    // confiava. Num alvo onde o tessellator e' o estagio pior suportado
    // (930M, driver velho) confiar sai caro. Tefra 2026-09-06, G2.
    const float tessCap = static_cast<float>(std::max(m_vulkan.maxTessellationLevel(), 1u));
    for (int i = 0; i < 4; ++i) {
        tessLo[i] = glm::clamp(tessLo[i], 1.0f, tessCap);
        tessHi[i] = glm::clamp(tessHi[i], 1.0f, tessCap);
    }
    float tessMax = 1.0f;
    for (int i = 0; i < 4; ++i) tessMax = std::max({tessMax, tessLo[i], tessHi[i]});
    // ERUPTION_TEST_TESS=0|1 (bancada): A/B da tesselacao da banda perto.
    static const int kTessEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_TESS");
        return e ? std::atoi(e) : -1;
    }();
    const bool tessWanted = (kTessEnv < 0) ? m_tessEnabled : (kTessEnv != 0);
    const bool tessOn = tessWanted && m_vulkan.tessellationSupported() && tessMax > 1.001f;
    // ERUPTION_TEST_TESS_AMP=<u> (bancada): amplitude do deslocamento.
    static const float kTessAmpEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_TESS_AMP");
        return e ? static_cast<float>(std::atof(e)) : -1.0f;
    }();
    frameUbo.tessParams = Vec4(distMax, tessOn ? 1.0f : 0.0f,
                               kTessAmpEnv >= 0.0f ? kTessAmpEnv : m_tessAmplitude, tessMax);
    frameUbo.tessLut0 = tessLo;
    frameUbo.tessLut1 = tessHi;
    // .w = TETO DE FATOR PARA FOLHAGEM. A folha precisa de pouca subdivisao
    // para a lamina curvar (2-3 segmentos), mas a curva por distancia dava a
    // ela o mesmo fator de uma pedra - e' de onde saia 5,90x de triangulo por
    // 1,08x de fragmento. 0 = sem teto. Ver model.tesc, edgeFactor().
    static const float kFoliageTessCap = [] {
        const char* e = std::getenv("ERUPTION_TESS_FOLIAGE_CAP");
        return e ? static_cast<float>(std::atof(e)) : 0.0f;
    }();
    frameUbo.tessParams2 = Vec4(static_cast<float>(m_tessHeightSpace),
                                std::max(m_tessWorldScale, 1e-4f),
                                glm::radians(m_normalMaxSlopeDeg), kFoliageTessCap);
    m_modelRenderer.setTessellation(tessOn, m_tessCurve.evaluate(0.0f) > 1.001f ? distMax * 0.12f : 0.0f);
    Vec4 shOpLo(1.0f), shOpHi(1.0f);
    sampleLut(m_shadowOpacitySpline, shOpLo, shOpHi);
    m_deferredLighting.setShadowOpacityCurve(
        Vec4(distMax, (m_shadowOpacityCurve && kShadowOpacityEnv) ? 1.0f : 0.0f, 0.0f, 0.0f),
        shOpLo, shOpHi);
    frameUbo.normalMapInvertY = m_normalMapInvertY ? 1.0f : 0.0f;
    frameUbo.defaultRoughness = m_defaultRoughness;
    frameUbo.defaultMetallic = m_defaultMetallic;
    // POM (docs/displacement_design.md). w = wetness global do clima: tira o
    // dual-use do alpha do MRAH-W (agora exclusivamente altura).
    // .y: mascara de ABLACAO da micro-sombra do normal map (POM foi removido,
    // entao maxSteps nao tinha leitor no shader). ERUPTION_TEST_MICRO_SHADOW=
    // bits: 1=marcha do sol, 2=marcha fixa (ambiente), 4=cavidade AO. Padrao
    // 7 (tudo ligado). Serve pra isolar qual termo desloca a textura.
    static const float kMicroShadowMask = [] {
        const char* e = std::getenv("ERUPTION_TEST_MICRO_SHADOW");
        return e ? static_cast<float>(std::atoi(e)) : 7.0f;
    }();
    frameUbo.pomParams = Vec4(m_pomHeightScale,
                              kMicroShadowMask,
                              static_cast<float>(m_pomMinSteps),
                              m_weatherSystem.surfaceWetness());

    // VENTO: alimenta o balanco da vegetacao no vertex shader.
    //
    // O WindField existia desde a implementacao do vento e NINGUEM consumia
    // para geometria - o vento so' movia nuvem e inclinava chuva, entao a
    // vegetacao ficava congelada enquanto o resto da cena tinha vento. Este e'
    // o consumidor que faltava.
    //
    // Relogio proprio (m_windClock) e nao `time`: o balanco nao pode saltar
    // quando o tempo da cena reinicia numa troca de mapa, senao toda a
    // vegetacao teleporta de posicao num frame.
    m_windClock += m_timer.deltaTime();
    const Vec3 windVec = m_weatherSystem.wind().velocity();
    // .y carrega a escala de balanco (o vento e' horizontal, o y era sempre 0).
    frameUbo.windParams = Vec4(windVec.x, m_windSwayScale, windVec.z, m_windClock);
    // ERUPTION_TEST_WIND_DEBUG=1: o vetor que de fato chega ao vertex shader.
    static const bool kWindDbg = std::getenv("ERUPTION_TEST_WIND_DEBUG") != nullptr;
    if (kWindDbg) {
        static float acc = 0.0f;
        acc += m_timer.deltaTime();
        if (acc >= 1.0f) {
            acc = 0.0f;
            ERUPTION_LOG_WARN("[VENTO] vel=(%.3f,%.3f,%.3f) |v|=%.3f relogio=%.1f dir=(%.2f,%.2f,%.2f) forca=%.3f",
                              windVec.x, windVec.y, windVec.z, glm::length(windVec), m_windClock,
                              m_weatherSystem.wind().direction().x, m_weatherSystem.wind().direction().y,
                              m_weatherSystem.wind().direction().z, m_weatherSystem.wind().strength());
        }
    }

    // Fill Lighting info for Forward Sprites
    frameUbo.sunDir = Vec4(env.sun.direction, env.sun.intensity);
    frameUbo.sunColor = Vec4(env.sun.color, 1.0f); // Sun color multiplier
    frameUbo.ambientSky = Vec4(env.ambientSkyColor, env.ambientIntensity);
    frameUbo.ambientGround = Vec4(env.ambientGroundColor, 0.0f);
    
    // Sync GI parameters to match map rules
    frameUbo.giIntensity = env.ambientIntensity * giIntensity;
    frameUbo.giAmbientFloor = 0.0f;

    if (m_shadowRenderer.settings().enabled) {
        m_shadowRenderer.updateCascades(env.sun, m_camera, zoomPercent, m_dayNightCycle.timeOfDay());
        m_vulkan.writeTimestamp(cmd, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        m_shadowRenderer.renderCascades(cmd, &m_terrainRenderer, &m_modelRenderer, &m_spriteRenderer, frameUbo, m_spriteSystem.instanceBuffer(), m_spriteSystem.visibleCount());
        m_vulkan.writeTimestamp(cmd, 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        Profiler::addGpuTime("Shadows", m_vulkan.timestampDeltaMs(0, 1));
    }
    cpuMark(2);

    // Debug light rays (disable during headless screenshots/auto-exit)
    if (m_shadowRenderer.settings().debugLightRays && !m_enableAutoExit && m_screenshotOnLoad.empty()) {
        Vec3 sunDir = glm::normalize(env.sun.direction);
        Vec3 rayOrigin = m_camera.target() + sunDir * 5000.0f;
        Vec3 rayEnd = m_camera.target() - sunDir * 5000.0f;
        m_debugLineRenderer.addLine(rayOrigin, rayEnd, Vec3(1.0f, 1.0f, 0.0f));
        // Cross pattern for visibility
        Vec3 perp1 = glm::abs(sunDir.y) < 0.99f ? glm::normalize(glm::cross(sunDir, Vec3(0,1,0))) : Vec3(1,0,0);
        Vec3 perp2 = glm::cross(sunDir, perp1);
        float crossSize = 200.0f;
        m_debugLineRenderer.addLine(m_camera.target() - perp1 * crossSize, m_camera.target() + perp1 * crossSize, Vec3(1.0f, 1.0f, 0.0f));
        m_debugLineRenderer.addLine(m_camera.target() - perp2 * crossSize, m_camera.target() + perp2 * crossSize, Vec3(1.0f, 1.0f, 0.0f));
        m_debugLineRenderer.upload();
    }
    
    if (!m_benchmarkThunderstorm) {
        renderMinimap(cmd);
    cpuMark(3); // minimap fora do custo do G-buffer
    }

    m_vulkan.writeTimestamp(cmd, 2, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    m_gbuffer.beginPass(cmd);
    // PIPELINE STATISTICS em volta do passe de GEOMETRIA: e' aqui que se
    // responde quantos triangulos o rasterizador de fato viu (e nao quantos o
    // arquivo tinha, nem quantos a CPU submeteu). Comeca DEPOIS do beginPass e
    // termina ANTES do endPass - consulta de estatistica nao pode atravessar a
    // fronteira do render pass.
    m_vulkan.beginPipelineStats(cmd);
    // Update the shared FrameUBO before any geometry pass reads it. Terrain and
    // model pipelines bind the same UBO set as sprites and need current
    // camera/POM data; without this they read stale values from the previous frame.
    m_spriteRenderer.updateFrameUbo(frameUbo);
    // ERUPTION_TEST_SKYBOX_ONLY=1 (debug): skip terrain/model geometry so a
    // headless screenshot shows the skybox with nothing occluding it, for
    // isolating skybox-only gradient/seam bugs from foreground silhouettes.
    static const bool skyboxOnlyTest = std::getenv("ERUPTION_TEST_SKYBOX_ONLY") != nullptr;
    if (!skyboxOnlyTest) {
        if (m_currentMap) {
            m_terrainRenderer.render(cmd, m_camera.viewProjectionMatrix(), m_camera.frustum());
        }
        // Pixels por unidade de mundo a distancia 1 (descarte por tamanho na tela).
        m_modelRenderer.setScreenMetrics(
            static_cast<float>(m_vulkan.swapExtent().height) /
            (2.0f * std::tan(glm::radians(m_camera.fov()) * 0.5f)));
        // ERUPTION_TEST_FORCE_BASE_LOD=1 (debug): forca a malha CHEIA em toda
        // instancia visivel, mesmo longe - A/B contra o LOD normal para o
        // harness perf/test_lod_quality.py medir o quanto o LOD simplificado
        // se afasta da malha de referencia (RMS de silhueta na mesma pose).
        static const bool forceBaseLod = std::getenv("ERUPTION_TEST_FORCE_BASE_LOD") != nullptr;
        if (forceBaseLod || m_demoForceBaseLod) m_modelRenderer.setForceBaseLod(true);
        m_modelRenderer.render(cmd, m_camera.viewProjectionMatrix(), m_camera.frustum(), m_camera.position());
        if (forceBaseLod || m_demoForceBaseLod) m_modelRenderer.setForceBaseLod(false);
    }

    {
        uint32_t spriteCount = m_spriteSystem.visibleCount();
        if (spriteCount > 0) {
            m_spriteRenderer.renderSprites(cmd, frameUbo, m_spriteSystem.instanceBuffer(), spriteCount);
        }
    }
    m_vulkan.endPipelineStats(cmd);
    m_gbuffer.endPass(cmd);
    m_vulkan.writeTimestamp(cmd, 3, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("GBuffer", m_vulkan.timestampDeltaMs(2, 3));
    cpuMark(4);

    // Cloud shadow map placeholder: not implemented yet for Cloud Layers.
    m_vulkan.writeTimestamp(cmd, 20, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    m_vulkan.writeTimestamp(cmd, 21, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("Cloud Shadow Map", m_vulkan.timestampDeltaMs(20, 21));
    cpuMark(5);

    m_gbuffer.transitionToRead(cmd);
    m_vulkan.writeTimestamp(cmd, 4, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    {
        Vec3 worldMin(-3000.0f, 0.0f, -3000.0f);
        Vec3 worldMax(3000.0f, 2000.0f, 3000.0f);
        // LUZ PRESA AO SPRITE PRINCIPAL (F10). Vive na MESMA lista que as luzes
        // do mapa pra' passar pelo mesmo passe de point light, sem caminho
        // especial. Identificada por um sentinela em shadowCubemapIndex (o
        // campo nunca e' lido pelo shader - point light nao tem sombra) pra'
        // ser removida e reposta a cada frame: assim sobrevive a reload de mapa
        // (setPointLights() substitui a lista) e segue o jogador sem precisar
        // guardar indice.
        {
            constexpr uint32_t kPlayerLightTag = 0xFFFFFFFEu;
            auto& pls = m_deferredLighting.getPointLights();
            pls.erase(std::remove_if(pls.begin(), pls.end(),
                                     [](const PointLight& p) { return p.shadowCubemapIndex == kPlayerLightTag; }),
                      pls.end());
            if (m_playerLight.enabled) {
                const float h = m_playerController ? m_playerController->spriteHeight() : 40.0f;
                PointLight pl;
                pl.position = m_camera.target() + Vec3(0.0f, h * m_playerLight.heightFrac, 0.0f);
                pl.color = m_playerLight.color;
                pl.intensity = m_playerLight.intensity;
                pl.radius = m_playerLight.radius;
                pl.animType = LightAnimType::Static;
                pl.shadowCubemapIndex = kPlayerLightTag;
                pl.enabled = true;
                pl.nightOnly = false;
                pls.push_back(pl);
            }
        }
        m_deferredLighting.render(cmd, m_camera,
            &m_shadowRenderer,
            VK_NULL_HANDLE, VK_NULL_HANDLE,
            worldMin, worldMax,
            0.0f,
            VK_NULL_HANDLE, VK_NULL_HANDLE,
            m_timer.elapsed());
    }
    m_vulkan.writeTimestamp(cmd, 5, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("Deferred Lighting", m_vulkan.timestampDeltaMs(4, 5));
    cpuMark(6);

    VkRenderingAttachmentInfo colorAttachment{}; colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_deferredLighting.litImageView(); colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depthAttachment{}; depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = m_gbuffer.depthView(); depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    // Visibilidade da agua, calculada UMA vez e usada por tudo que so' existe
    // por causa dela (ceu-para-textura, espuma, refracao, o proprio passe).
    // A agua nao entra no shadow map, entao cortar pelo frustum da CAMERA
    // aqui nao apaga sombra de nada - a regra de "sombra e' pelo frustum da
    // LUZ" segue intocada no ShadowRenderer.
    const float waterYNow = m_mapBaseWaterLevel + m_waterMenu.config.waterLevel;
    const bool waterInView = m_renderEffects[7]->isEnabled() && m_water.hasWater() &&
        !m_waterFullyBuried &&
        (!m_waterBoundsValid ||
         m_camera.frustum().intersectsAABB(
             Vec3(m_waterAabbMin.x, waterYNow - 2.0f, m_waterAabbMin.z),
             Vec3(m_waterAabbMax.x, waterYNow + 2.0f, m_waterAabbMax.z)));

    // Prepare water refraction copies before entering the forward rendering pass,
    // because we cannot transition the active color attachment inside dynamic rendering.
    // O ceu-para-textura existe SO' para o reflexo da agua: sem agua na vista,
    // e' um passe inteiro de skybox jogado fora todo frame. Medido em cidade-A
    // com a agua fora do quadro: a fase inteira custava 2,045 ms de CPU.
    if (!m_benchmarkThunderstormStarted && waterInView) {
        if (!wireframeMode()) m_skybox.renderToTexture(cmd, m_dayNightCycle, m_camera.skyboxViewProjectionMatrix());
    }

    // SONDA DE CEU (G36): 6 faces de 32x32 + mips + SH9, a cada 8 frames (o
    // ceu muda com a hora e o clima, nao com a camera). Fora de render pass.
    // Ate' a primeira sonda ficar pronta o lighting fica no gradiente antigo.
    {
        static uint32_t s_skyProbeFrame = 0;
        const bool wantProbe = m_skyProbeEnabled && m_renderEffects.size() > 0;
        if (wantProbe && (!m_skyProbe.ready() || (s_skyProbeFrame % 8u) == 0u)) {
            m_skyProbe.update(cmd, m_skybox, m_dayNightCycle);
        }
        ++s_skyProbeFrame;
        m_deferredLighting.setSkyProbeParams(wantProbe && m_skyProbe.ready(), m_skyProbeIntensity);
        m_deferredLighting.setIrradianceProbeStrength(m_irradianceProbesEnabled ? m_irradianceProbeStrength : 0.0f);
    }

    // Mapa sem agua nao paga agua: a espuma e' simulacao de CPU e o
    // prepareRefraction copia a imagem lit INTEIRA - 1,5 ms de CPU por frame
    // medidos em parana_field, que nao tem um tile de agua. hasWater() ja
    // existia e ninguem consultava. O regen de malha logo abaixo continua
    // rodando, entao ligar forceWater reativa tudo no frame seguinte.
    if (waterInView) {
        m_water.updateContactFoam(m_timer.deltaTime(), m_waterMenu.config);
        m_water.prepareRefraction(cmd,
                                  m_deferredLighting.litImage(), m_gbuffer.depthImage(),
                                  m_gbuffer.extent().width, m_gbuffer.extent().height,
                                  m_camera.nearPlane(), m_camera.farPlane(),
                                  inverse(m_camera.viewProjectionMatrix()),
                                  m_waterMenu.config);
    }

    m_vulkan.cmdImageBarrier(cmd, m_gbuffer.depthImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    m_vulkan.cmdBeginRendering(cmd, {colorAttachment}, &depthAttachment, nullptr, renderExtent());

    // Water forward pass (transparent) — BEFORE skybox so depth buffer still has terrain
    // Regenerate water mesh if forceWater toggle changed
    if (m_currentMap && m_waterForceWaterPrev != m_waterMenu.config.forceWater) {
        m_waterForceWaterPrev = m_waterMenu.config.forceWater;
        float wl, wh;
        bool fromTerrain = getMapWaterParams(*m_currentMap, wl, wh, m_waterMenu.config.forceWater);
        auto wmesh = TerrainParser::generateWaterMesh(m_currentMap->terrain, 0, 0,
                                                  m_currentMap->terrain.width, m_currentMap->terrain.height,
                                                  wl, wh, m_waterMenu.config.forceWater, !fromTerrain);
        m_water.setWaterMesh(wmesh);
        cacheWaterBounds(wmesh);
        ERUPTION_LOG_INFO("Water mesh regenerated: %u tiles (force=%d)", wmesh.waterTileCount, m_waterMenu.config.forceWater);
    }

    m_water.setViewport((float)renderExtent().width, (float)renderExtent().height);
    m_vulkan.writeTimestamp(cmd, 10, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (m_renderEffects[7]->isEnabled()) {
        WaterRenderer::WaterSettings adjustedSettings = m_waterMenu.config;
        adjustedSettings.waterLevel = m_mapBaseWaterLevel + m_waterMenu.config.waterLevel;
        // O plano d'agua do mapa e' SEMPRE agua. Sem esta linha ele herdava
        // config.liquidKind, que WaterDebugMenu::applySelectedLiquid escreve
        // a partir do liquido SELECIONADO na lista - e a selecao comeca em 0,
        // o primeiro liquido do .env. Num mapa cuja lista comeca por lava
        // (parana_field: o lago da cratera vem primeiro), o plano d'agua do
        // mapa inteiro passava a ser sombreado como LAVA, sem ninguem ter
        // clicado em nada. E' a mesma classe de bug que applySelectedLiquid
        // ja' corrigiu pro waterLevel ("selecionar lava sujava o nivel de
        // agua global em silencio") - so' que num campo vizinho.
        //
        // A superficie de lava NAO depende disto: ela e' desenhada logo
        // abaixo, num segundo draw, com lavaSettings.liquidKind = 1 proprio.
        adjustedSettings.liquidKind = 0;

        // Weather affects water surface: agitation, opacity and reflection
        if (m_weatherAffectsWater) {
            const auto& w = m_weatherSystem.current();
            float rain = w.rainIntensity;
            float storm = w.stormTint;
            float fog = w.fogDensity;
            float wind = glm::abs(w.rainWind);

            // Agitation
            adjustedSettings.normalScale    *= 1.0f + rain * 0.5f + storm * 0.3f;
            adjustedSettings.normalStrength *= 1.0f + rain * 0.4f + storm * 0.3f;
            adjustedSettings.waveAmplitude  *= 1.0f + rain * 0.6f + storm * 0.8f;
            adjustedSettings.waveFrequency  *= 1.0f + rain * 0.2f + storm * 0.2f;
            adjustedSettings.waveSpeed      *= 1.0f + rain * 0.8f + storm * 1.0f + wind * 0.5f;

            // Roughness increases under rain/storm so highlights spread and dim
            adjustedSettings.roughness = glm::min(1.0f, adjustedSettings.roughness + rain * 0.25f + storm * 0.35f);

            // Reflectivity drops under rain and storm (less mirror-like, more diffuse)
            adjustedSettings.reflectivity = glm::max(0.1f, adjustedSettings.reflectivity * (1.0f - rain * 0.3f - storm * 0.5f));

            // Fog makes water more opaque and merges with the atmosphere
            adjustedSettings.transparency = glm::max(0.15f, adjustedSettings.transparency * (1.0f - fog * 0.5f));

            // Tint water slightly darker under storm so it doesn't look unnaturally bright
            float darken = 1.0f - storm * 0.15f;
            adjustedSettings.baseColorDeep *= darken;
            adjustedSettings.baseColorShallow *= darken;
        }

        // CULLING DA AGUA POR FRUSTUM DA CAMERA (feito AQUI, no chamador:
        // WaterRenderer e' de outro agente). O passe de agua nao tinha culling
        // nenhum - os unicos early-outs sao "efeito desligado" e "malha
        // vazia". Num mapa onde a agua existe mas esta' fora da vista
        // (cidade-A), isso custava 0.134 ms de GPU e 2.045 ms de CPU POR
        // FRAME sem desenhar um pixel visivel - mais que o passe de sombra
        // inteiro. Reportado pelo autor 2026-09-02.
        //
        // Seguro para SOMBRA: a agua nao entra no shadow map (renderCascades
        // desenha terreno, modelos e sprites - nao a agua), entao cortar pelo
        // frustum da CAMERA aqui nao pode apagar sombra de nada. A regra de
        // "culling de sombra e' por frustum da LUZ" continua valendo e
        // intocada no ShadowRenderer.
        if (waterInView)
        m_water.render(cmd, m_camera.viewProjectionMatrix(), m_camera.position(),
                       m_dayNightCycle, m_timer.elapsed(),
                       m_camera.nearPlane(), m_camera.farPlane(),
                       adjustedSettings, m_waterMenu.abTestMask,
                       m_camera.orbitPitch(),
                       m_weatherSystem.sunOcclusion(),
                       m_weatherSystem.moonOcclusion(),
                       m_weatherSystem.current().stormTint,
                       m_weatherSystem.current().rainIntensity,
                       m_weatherSystem.current().rainSplashIntensity);

        if (m_hasLavaLiquid && m_water.hasLava()) {
            WaterRenderer::WaterSettings lavaSettings = adjustedSettings;
            lavaSettings.liquidKind = 1;
            lavaSettings.waterLevel = m_lavaLiquid.level;
            lavaSettings.emissiveStrength = m_lavaLiquid.emissive;
            lavaSettings.flowSpeed = m_lavaLiquid.flowSpeed;
            lavaSettings.enableCaustics = false;
            lavaSettings.enableFoam = false;
            lavaSettings.enableSurfaceFoam = false;
            lavaSettings.transparency = 0.0f;
            lavaSettings.reflectivity = 0.05f;
            m_water.render(cmd, m_camera.viewProjectionMatrix(), m_camera.position(),
                           m_dayNightCycle, m_timer.elapsed(),
                           m_camera.nearPlane(), m_camera.farPlane(),
                           lavaSettings, m_waterMenu.abTestMask,
                           m_camera.orbitPitch(),
                           m_weatherSystem.sunOcclusion(),
                           m_weatherSystem.moonOcclusion(),
                           0.0f, 0.0f, 0.0f, true);
        }
    }
    m_vulkan.writeTimestamp(cmd, 11, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("Water", m_vulkan.timestampDeltaMs(10, 11));
    cpuMark(7);

    // Projected Shadow pass for billboards (drop shadows on ground)
    if (!m_benchmarkThunderstormStarted) {
        uint32_t spriteCount = m_spriteSystem.visibleCount();
        if (spriteCount > 0) {
            if (m_charConfig.shadowType == 1) {
                Vec2 circleParams(m_charConfig.circleShadowDilation, m_charConfig.circleShadowSoftness);
                m_spriteRenderer.renderBillboardCircleShadows(cmd, frameUbo, circleParams,
                                                              m_spriteSystem.instanceBuffer(), spriteCount);
            } else {
                Vec2 planarParams(m_charConfig.planarShadowDilation, m_charConfig.planarShadowSoftness);
                m_spriteRenderer.renderBillboardPlanarShadows(cmd, frameUbo, m_shadowRenderer.currentLightDir(),
                                                              planarParams, m_spriteSystem.instanceBuffer(), spriteCount);
            }
        }
    }

    m_vulkan.writeTimestamp(cmd, 6, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (!m_benchmarkThunderstormStarted) {
        if (!wireframeMode()) m_skybox.render(cmd, m_dayNightCycle, m_camera.skyboxViewProjectionMatrix());
    }
    m_vulkan.writeTimestamp(cmd, 7, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("Skybox IBL", m_vulkan.timestampDeltaMs(6, 7));

    // Cloud layers are rendered later in the frame before post-processing.
    m_vulkan.writeTimestamp(cmd, 22, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    m_vulkan.writeTimestamp(cmd, 23, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("CloudFluff Render", 0.0f);
    cpuMark(8);

    m_overlayLineRenderer.render(cmd, m_camera.viewProjectionMatrix());
    m_debugLineRenderer.render(cmd, m_camera.viewProjectionMatrix());
    m_vulkan.cmdEndRendering(cmd);

    // Make depth readable so the cloud-shadow fullscreen pass can reconstruct world positions.
    m_vulkan.cmdImageBarrier(cmd, m_gbuffer.depthImage(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Cloud shadows: project cloud coverage onto the lit scene using the depth buffer.
    if (m_cloudLayersEnabled) {
        // ERUPTION_TEST_NO_CLOUD_SHADOW=1 (debug): skip per-layer shadows to isolate
        // their GPU cost. ERUPTION_TEST_SHADOW_FULLRES=1 (debug): bypass the half-res
        // accumulation and draw straight into the lit image (A/B diff path).
        static const bool skipCloudShadows = std::getenv("ERUPTION_TEST_NO_CLOUD_SHADOW") != nullptr;
        static const bool shadowFullRes = std::getenv("ERUPTION_TEST_SHADOW_FULLRES") != nullptr;
        const uint32_t swapW = renderExtent().width;
        const uint32_t swapH = renderExtent().height;
        // Half-res accumulation (Frostbite-style): the 100 per-cloud shadow
        // passes render into a half-res offscreen that accumulates occlusion
        // in alpha; a single fullscreen apply pass then darkens the scene.
        const bool wantAccum = !skipCloudShadows && !shadowFullRes && m_useProceduralCloudField
                               && !m_independentCloudLayers.empty();
        bool accumBegun = false;
        if (wantAccum) {
            // Quarter-res: the shadow kernel already gaussian-blurs ~6 m+
            // world units, so the upsample is invisible (measured ~0 diff).
            accumBegun = m_cloudLayerRenderer.beginCloudShadowAccum(cmd, (swapW + 3) / 4, (swapH + 3) / 4);
        }
        const uint32_t shW = accumBegun ? (swapW + 3) / 4 : swapW;
        const uint32_t shH = accumBegun ? (swapH + 3) / 4 : swapH;
        if (!accumBegun) {
            VkRenderingAttachmentInfo shadowColorAttach{};
            shadowColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            shadowColorAttach.imageView = m_deferredLighting.litImageView();
            shadowColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            shadowColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            shadowColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo shadowInfo{};
            shadowInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            shadowInfo.renderArea = {{0, 0}, {swapW, swapH}};
            shadowInfo.layerCount = 1;
            shadowInfo.colorAttachmentCount = 1;
            shadowInfo.pColorAttachments = &shadowColorAttach;

            vkCmdBeginRendering(cmd, &shadowInfo);
        }
        // Global cloud shadow: dormant while the procedural field replaces the
        // global cloud layer (each field cloud casts its own shadow below).
        if (!m_useProceduralCloudField) {
            m_cloudLayerRenderer.renderCloudShadows(cmd, m_camera,
                                                    m_deferredLighting.litImageView(), m_gbuffer.depthView(),
                                                    swapW, swapH);
        }
        // Independent cloud layers: each casts its own ground shadow.
        const Vec3 cloudLightDir = m_shadowRenderer.currentLightDir();
        m_vulkan.writeTimestamp(cmd, 28, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        if (!skipCloudShadows) {
            for (const auto& icl : m_independentCloudLayers) {
                m_cloudLayerRenderer.renderCloudLayerShadow(cmd, icl.rendererId,
                                                            icl.planeY,
                                                            m_camera, cloudLightDir,
                                                            m_gbuffer.depthView(),
                                                            shW, shH);
            }
        }
        m_vulkan.writeTimestamp(cmd, 29, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        Profiler::addGpuTime("Cloud Shadows (field)", m_vulkan.timestampDeltaMs(28, 29));
        if (accumBegun) {
            m_cloudLayerRenderer.endCloudShadowAccumApply(cmd, m_deferredLighting.litImageView(), swapW, swapH);
        } else {
            vkCmdEndRendering(cmd);
        }
        cpuMark(9);
    }

    // Return depth buffer to a READ-ONLY attachment layout: the cloud planes
    // depth-test (no writes) and the volumetric puff shader samples the same
    // depth image via descriptor — legal only with a read-only attachment.
    m_vulkan.cmdImageBarrier(cmd, m_gbuffer.depthImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Render the cloud coverage plane after scene objects so it layers correctly.
    // cloudVolOccReady: set when the cloud volume accumulation texture holds
    // this frame's fluff occlusion (screen-space alpha) for the weather
    // overlay's crown/splash occlusion (author feedback 2026-08-10).
    bool cloudVolOccReady = false;
    if (m_cloudLayersEnabled) {
        VkRenderingAttachmentInfo planeColorAttach{};
        planeColorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        planeColorAttach.imageView = m_deferredLighting.litImageView();
        planeColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        planeColorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        planeColorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo planeDepthAttach{};
        planeDepthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        planeDepthAttach.imageView = m_gbuffer.depthView();
        planeDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        planeDepthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        planeDepthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo planeInfo{};
        planeInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        planeInfo.renderArea = {{0, 0}, {renderExtent().width, renderExtent().height}};
        planeInfo.layerCount = 1;
        planeInfo.colorAttachmentCount = 1;
        planeInfo.pColorAttachments = &planeColorAttach;
        planeInfo.pDepthAttachment = &planeDepthAttach;

        vkCmdBeginRendering(cmd, &planeInfo);
        m_vulkan.writeTimestamp(cmd, 36, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        // Global coverage plane: dormant while the procedural field replaces
        // the global cloud layer.
        if (!m_useProceduralCloudField) {
            m_cloudLayerRenderer.renderDebugPlane(cmd, m_camera,
                                                  m_deferredLighting.litImageView(), m_gbuffer.depthView(),
                                                  renderExtent().width, renderExtent().height);
        }
        // Independent cloud layers: one isolated plane per layer altitude,
        // drawn far-to-near (depth write is off, so painter's order applies).
        // ERUPTION_TEST_NO_INDEP_PLANE=1 (debug): skip the volumetric plane draws to
        // isolate their GPU cost from the rest of the per-layer work.
        // ERUPTION_TEST_NO_INDEP_BILLBOARD=1 (debug): skip the cross-billboard draws.
        static const bool skipIndepPlanes = std::getenv("ERUPTION_TEST_NO_INDEP_PLANE") != nullptr;
        static const bool skipIndepBillboards = std::getenv("ERUPTION_TEST_NO_INDEP_BILLBOARD") != nullptr;
        // ERUPTION_TEST_VOLUME_FULLRES=1 (debug): billboards draw straight into the
        // lit image at full resolution (A/B diff path) instead of the half-res
        // premultiplied accumulation + single composite.
        static const bool volFullRes = std::getenv("ERUPTION_TEST_VOLUME_FULLRES") != nullptr;
        const bool wantVolAccum = !skipIndepBillboards && !volFullRes && m_useProceduralCloudField;
        // Scratch estatico (varredura 2026-09-02): alocacao por frame.
        static std::vector<std::pair<const IndependentCloudLayer*, float>> pendingBillboards;
        pendingBillboards.clear();
        if (!skipIndepPlanes || !skipIndepBillboards) {
            // Painter's order BACK-TO-FRONT by camera distance (farthest
            // first, nearest last = on top). The old planeY key was stable but
            // only correct from above: from a low/horizontal view a farther
            // but slightly higher cloud drew OVER a nearer one ("background
            // clouds in front"). Raw distance re-sorts whenever two drifting
            // clouds cross equidistance (the blend flicker), so the previous
            // frame's order is reused and an adjacent pair only swaps when the
            // distance gap exceeds a hysteresis band — while the depths are
            // that close, either blend order looks the same.
            const Vec3 camPos = m_camera.position();
            auto layerCenter = [](const IndependentCloudLayer* icl) {
                const Vec3 wmin = icl->array->worldMin();
                const Vec3 wmax = icl->array->worldMax();
                const Vec2 windUv = icl->array->windOffset();
                const Vec2 drift(windUv.x * (wmax.x - wmin.x), windUv.y * (wmax.z - wmin.z));
                Vec2 c(0.0f);
                int n = 0;
                for (const LocalCloud& lc : icl->array->localClouds()) {
                    if (!lc.alive) continue;
                    c.x += lc.center.x + drift.x;
                    c.y += lc.center.y + drift.y;
                    ++n;
                }
                if (n == 0) c = icl->centerXZ + drift;
                else c /= static_cast<float>(n);
                return Vec3(c.x, icl->planeY, c.y);
            };
            // Reused scratch containers: this runs every frame with up to
            // ~100 layers, so no per-frame heap and no O(n^2) id lookups.
            auto& sorted = m_cloudSortScratch;
            auto& dist = m_cloudOrderScratch;
            auto& order = m_cloudOrderIds;
            sorted.clear();
            sorted.reserve(m_independentCloudLayers.size());
            for (const auto& icl : m_independentCloudLayers) sorted.push_back(&icl);
            dist.clear();
            for (const IndependentCloudLayer* icl : sorted)
                dist[icl->rendererId] = {glm::length(layerCenter(icl) - camPos), icl, false};

            // Reconcile the stored order with the live layers: drop stale ids,
            // append new ones, then one insertion pass with the swap margin.
            order.clear();
            order.reserve(sorted.size());
            for (uint32_t id : m_cloudLayerDrawOrder) {
                auto it = dist.find(id);
                if (it != dist.end() && !it->second.inOrder) {
                    it->second.inOrder = true;
                    order.push_back(id);
                }
            }
            for (const IndependentCloudLayer* icl : sorted) {
                auto& e = dist[icl->rendererId];
                if (!e.inOrder) {
                    e.inOrder = true;
                    order.push_back(icl->rendererId);
                }
            }
            // Hysteresis: a farther layer only overtakes (moves earlier than)
            // its neighbor when it is farther by more than max(30 m, 10% of
            // the nearer distance). Result: farthest first, nearest last.
            auto hyst = [](float nearDist) { return std::max(30.0f, 0.10f * nearDist); };
            for (size_t i = 1; i < order.size(); ++i) {
                size_t j = i;
                while (j > 0 && dist[order[j]].dist > dist[order[j - 1]].dist + hyst(dist[order[j - 1]].dist)) {
                    std::swap(order[j], order[j - 1]);
                    --j;
                }
            }
            m_cloudLayerDrawOrder = order;

            sorted.clear();
            for (uint32_t id : order) sorted.push_back(dist[id].layer);
            for (const IndependentCloudLayer* icl : sorted) {
                float tint = -1.0f;
                if (m_postSettings.debugRainRegions && icl->hasRain) {
                    tint = 999.0f;
                } else if (m_cloudTestColors) {
                    tint = icl->altitude;
                }
                // ERUPTION_TEST_ORDER_LOG=1 (debug): dump the painter order per frame
                // to correlate order swaps with cloud blend flicker.
                static const bool kOrderLog = std::getenv("ERUPTION_TEST_ORDER_LOG") != nullptr;
                if (kOrderLog) {
                    static std::string s_last;
                    std::string cur;
                    for (const IndependentCloudLayer* l : sorted) cur += std::to_string(l->rendererId) + ",";
                    if (cur != s_last) {
                        ERUPTION_LOG_WARN("[ORDER] frame %llu: %s", (unsigned long long)m_framesCount, cur.c_str());
                        s_last = cur;
                    }
                }
                // Layers with live local clouds render as ray-marched 3D
                // volumes only: the flat POM deck is skipped so the cloud is
                // not drawn twice (flat sprite + volume). Layers without
                // local clouds keep the deck.
                bool hasVolume = false;
                for (const LocalCloud& lc : icl->array->localClouds()) {
                    if (lc.alive) { hasVolume = true; break; }
                }
                bool isRainDebug = m_postSettings.debugRainRegions && icl->hasRain;
                float drawPlaneY = icl->planeY;
                if (isRainDebug) {
                    float puffRy = 6.0f;
                    for (const LocalCloud& lc : icl->array->localClouds()) {
                        if (!lc.alive) continue;
                        const float invF = 1.0f / std::max(lc.falloff, 0.01f);
                        puffRy = std::max(puffRy, glm::clamp(std::fmax(lc.radius.x, lc.radius.y) * invF * 0.6f, 6.0f, 100.0f));
                    }
                    drawPlaneY = icl->planeY + puffRy;
                }
                if (!skipIndepPlanes && (!hasVolume || isRainDebug)) {
                    // Classic/manual clouds render with the pre-field
                    // volumetric dome look; weather-field clouds use the
                    // flat POM deck.
                    m_cloudLayerRenderer.renderCloudLayerPlane(cmd, icl->rendererId,
                                                               drawPlaneY,
                                                               m_camera, m_gbuffer.depthView(), tint,
                                                               isRainDebug ? false : !icl->fromField);
                }
                // Volumetric puffs: each independent cloud ray-marches its own
                // 3D noise-eroded volume (replaces the old upright cross-cards
                // and the "deitadona" flat-only look).
                constexpr bool kDrawIndependentBillboards = true;
                if (!skipIndepBillboards && kDrawIndependentBillboards && !isRainDebug) {
                    if (wantVolAccum) {
                        pendingBillboards.emplace_back(icl, tint);
                    } else {
                        m_cloudLayerRenderer.renderCloudLayerBillboard(cmd, icl->rendererId,
                                                                       icl->planeY,
                                                                       m_camera, m_gbuffer.depthView(), tint);
                    }
                }
            }
        }
        vkCmdEndRendering(cmd);

        // Half-res volume pass: all collected puffs ray-march into the
        // half-res offscreen, then a single premultiplied composite applies
        // them over the lit scene (exact "over" associativity; the billboard
        // pipeline has no hardware depth test — occlusion is analytic).
        if (wantVolAccum && !pendingBillboards.empty()) {
            // Low-res volume accum: billboards are pure soft volumetrics,
            // so the bilinear premult upsample is visually lossless and cuts
            // the dominant GPU cost at zoom-out by ~6x per dimension.
            const uint32_t vhW = (renderExtent().width + 5) / 6;
            const uint32_t vhH = (renderExtent().height + 5) / 6;
            if (m_cloudLayerRenderer.beginCloudVolumeAccum(cmd, vhW, vhH)) {
                for (const auto& [icl, tint] : pendingBillboards) {
                    m_cloudLayerRenderer.renderCloudLayerBillboard(cmd, icl->rendererId,
                                                                   icl->planeY,
                                                                   m_camera, m_gbuffer.depthView(), tint);
                }
                m_cloudLayerRenderer.endCloudVolumeAccumApply(cmd, m_deferredLighting.litImageView(),
                                                              renderExtent().width, renderExtent().height);
                cloudVolOccReady = true;
            } else {
                // Fallback: redraw direct into the lit image at full res.
                vkCmdBeginRendering(cmd, &planeInfo);
                for (const auto& [icl, tint] : pendingBillboards) {
                    m_cloudLayerRenderer.renderCloudLayerBillboard(cmd, icl->rendererId,
                                                                   icl->planeY,
                                                                   m_camera, m_gbuffer.depthView(), tint);
                }
                vkCmdEndRendering(cmd);
            }
        }
        m_vulkan.writeTimestamp(cmd, 37, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        Profiler::addGpuTime("Cloud Volumes (field)", m_vulkan.timestampDeltaMs(36, 37));

        // Fumaça do mapa (emissores declarados no .env): uma coluna
        // ray-marchada por emissor, desenhada DEPOIS do composite das nuvens
        // (a fumaça é local e fica na frente do céu). Mapa sem "smoke" no
        // .env não paga nada: o vetor está vazio e o passe inteiro some.
        // ERUPTION_TEST_NO_SMOKE=1 (debug): isola o custo do passe.
        static const bool kNoSmoke = std::getenv("ERUPTION_TEST_NO_SMOKE") != nullptr;
        const bool drawSmoke = m_smokeEnabled && !kNoSmoke && m_currentMap &&
                               !m_currentMap->smokeEmitters.empty() &&
                               m_cloudLayerRenderer.hasSmokePipeline();
        if (drawSmoke) {
            m_vulkan.writeTimestamp(cmd, 38, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            // Vento do clima, convertido para m/s com a mesma constante que
            // move as nuvens independentes (uvFactor 20 / tamanho do mundo).
            const auto& ccfg = m_cloudLayerRenderer.config();
            const float wspd = (m_cloudWindOverride >= 0.0f) ? m_cloudWindOverride : ccfg.windSpeed;
            Vec2 wdir(ccfg.windDirection.x, ccfg.windDirection.z);
            if (glm::length(wdir) > 1e-4f) wdir = glm::normalize(wdir);
            CloudLayerRenderer::SmokeQuality sq;
            sq.maxSteps = m_smokeMaxSteps;
            sq.sunTap = m_smokeSunTap;
            sq.detail = m_smokeDetail;
            vkCmdBeginRendering(cmd, &planeInfo);
            {
                // Scratch estatico (varredura 2026-09-02): reserve evitava
                // realocacao, nao a alocacao+free por frame.
                static std::vector<const SmokeEmitter*> ordered;
                ordered.clear();
                ordered.reserve(m_currentMap->smokeEmitters.size());
                for (const SmokeEmitter& sm : m_currentMap->smokeEmitters) ordered.push_back(&sm);
                const Vec3 eye = m_camera.position();
                std::sort(ordered.begin(), ordered.end(),
                          [&eye](const SmokeEmitter* a, const SmokeEmitter* b) {
                              const Vec3 da = a->position - eye;
                              const Vec3 db = b->position - eye;
                              return glm::dot(da, da) > glm::dot(db, db);
                          });
                // CULLING POR FRUSTUM DA COLUNA DE FUMACA. Nao havia nenhum:
                // toda coluna do mapa era desenhada a cada frame, dentro ou
                // fora da tela, e cada uma e' um ray-march volumetrico. E' o
                // mesmo padrao que a agua tinha (passe inteiro sem culling) e
                // custava caro: "Map Smoke" era o SEGUNDO passe mais caro do
                // parana_field, 1,48 ms de media nos 13 climas de estresse -
                // 18% do orcamento de 8,33 ms dos 120 FPS - e presente ate' em
                // tempo limpo.
                // A caixa e' a propria coluna: base de raio `radius` na boca,
                // topo em `height` com raio `radius * (1 + spread)`, mais uma
                // folga lateral para a adveccao pelo vento nao sumir na borda.
                for (const SmokeEmitter* sm : ordered) {
                    const float rTopo = sm->radius * (1.0f + std::max(sm->spread, 0.0f));
                    const float folga = rTopo + glm::length(wdir * wspd * 20.0f * sm->windScale);
                    const Vec3 mn(sm->position.x - folga, sm->position.y,
                                  sm->position.z - folga);
                    const Vec3 mx(sm->position.x + folga, sm->position.y + sm->height,
                                  sm->position.z + folga);
                    if (!m_camera.frustum().intersectsAABB(mn, mx)) continue;
                    m_cloudLayerRenderer.renderSmokePlume(cmd, *sm, m_camera, m_gbuffer.depthView(),
                                                          wdir * wspd * 20.0f * sm->windScale, sq);
                }
            }
            vkCmdEndRendering(cmd);
            m_vulkan.writeTimestamp(cmd, 39, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            Profiler::addGpuTime("Map Smoke", m_vulkan.timestampDeltaMs(38, 39));
        }
        cpuMark(10);
    }

    m_vulkan.cmdImageBarrier(cmd, m_deferredLighting.litImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    m_vulkan.cmdImageBarrier(cmd, m_gbuffer.depthImage(), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    PostProcessor::PostSettings postSettings = m_postSettings;
    postSettings.fogColor = m_dayNightCycle.getFogColor();

    // Auto-exposure (eye adaptation). Primary source: HISTOGRAM measured from
    // the rendered HDR frame on the GPU (luminance_histogram/adaptation compute
    // in PostProcessor), which already applies temporal smoothing. Fallback for
    // the first frames (before a GPU result exists): analytic estimate from the
    // light budget.
    if (m_autoExposureEnabled) {
        postSettings.gpuAutoExposure = true;
        postSettings.aeTau = m_autoExposureSpeed;
        postSettings.aeDt = glm::clamp(m_timer.deltaTime(), 0.001f, 0.1f);

        bool gpuValid = false;
        float gpuExp = m_postProcessor.gpuAutoExposure(gpuValid);
        if (gpuValid) {
            // Key rescales the mid-grey target: 1.55 (default) ~= x1 at noon.
            m_autoExposureCurrent = glm::clamp(gpuExp * (m_autoExposureKey / 1.55f),
                                               m_autoExposureMin, m_autoExposureMax);
        } else {
            float sceneLevel = env.sun.intensity * 0.7f + m_ambientIntensity * 0.9f
                             + (m_moonFillEnabled ? 0.05f : 0.0f);
            sceneLevel = glm::max(sceneLevel, 0.05f);
            float targetExp = glm::clamp(m_autoExposureKey / sceneLevel,
                                         m_autoExposureMin, m_autoExposureMax);
            float rate = 1.0f - std::exp(-m_timer.deltaTime() * m_autoExposureSpeed);
            m_autoExposureCurrent = glm::mix(m_autoExposureCurrent, targetExp, glm::clamp(rate, 0.0f, 1.0f));
        }
        postSettings.exposure = m_postSettings.exposure * m_autoExposureCurrent;
    } else {
        postSettings.gpuAutoExposure = false;
    }

    {
        auto& w = m_weatherSystem.current();
        auto& skyCfg = m_skybox.editableConfig().procedural;
        skyCfg.stormTint = w.stormTint;
        skyCfg.cloudCoverage = w.cloudCoverage;
        skyCfg.cloudSpeed = w.cloudSpeed;
        postSettings.enableFog = w.fogDensity > 0.001f;
        postSettings.fogStart = w.fogStart;
        postSettings.fogEnd = w.fogEnd;
        postSettings.fogOpacity = w.fogDensity;
        postSettings.fogHeight = w.fogHeight;
        postSettings.fogHeightFalloff = w.fogHeightFalloff;

        // Coerência atmosférica: ajustar sol/lua conforme oclusão de nuvens
        auto& skyParams = m_skybox.editableConfig().procedural;
        skyParams.sunHaloIntensity *= m_weatherSystem.sunOcclusion();
        // A lua é mais sensível às nuvens
        float moonOcc = m_weatherSystem.moonOcclusion();
        skyParams.moonSize = glm::mix(0.0f, skyParams.moonSize, moonOcc);
        if (skyCfg.weatherType != m_weatherSystem.currentType()) {
            skyCfg.weatherType = m_weatherSystem.currentType();
        }
        m_skybox.commitConfig();
        m_water.setSkyTexture(m_skybox.outputView(), m_skybox.outputSampler());
    }

    // A autoria por zoom do ramo legado saiu junto com ele; so' o CoC
    // mapeia zoom -> focal/abertura agora.
    postSettings.cocZoomFactor = zoomPercent;
    if (postSettings.cocEnableZoomMapping) {
        float baseFocal = postSettings.cocFocalCurve.evaluate(zoomPercent);
        postSettings.cocFocalDistance = baseFocal;
        postSettings.cocAperture = postSettings.cocApertureCurve.evaluate(zoomPercent);

        if (postSettings.cocEnableAdaptiveFocal) {
            postSettings.cocFocalDistance = glm::clamp(baseFocal + m_playerController->adaptiveFocalOffset(), 1.0f, 1000.0f);
        }
    }
    // Overrides de bancada DEPOIS do mapeamento por zoom (senao a curva
    // sobrescreve o valor a cada frame): varredura de DoF sem editar json.
    {
        static const char* kCocFocal = std::getenv("ERUPTION_TEST_COC_FOCAL");
        static const char* kCocAperture = std::getenv("ERUPTION_TEST_COC_APERTURE");
        if (kCocFocal) postSettings.cocFocalDistance = std::strtof(kCocFocal, nullptr);
        if (kCocAperture) postSettings.cocAperture = std::strtof(kCocAperture, nullptr);
    }

    // Top-down orthographic depth pass for rain/snow heightmap generation.
    // Only render when there is actual precipitation (or wind-blown debris —
    // the sand/dust box uses the same heightmap for below-terrain kill and
    // the directional wind-shadow occlusion) to avoid wasting GPU time.
    m_vulkan.writeTimestamp(cmd, 24, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VkImageView topDownDepthView = VK_NULL_HANDLE;
    const WeatherParams& currentWeather = m_weatherSystem.current();
    bool needsTopDownDepth = postSettings.weatherTier != WeatherTier::Mobile &&
                             (currentWeather.rainIntensity > 0.001f ||
                              currentWeather.snowIntensity > 0.001f ||
                              currentWeather.windDebrisIntensity > 0.001f ||
                              currentWeather.weatherType == WeatherType::Hail ||
                              currentWeather.weatherType == WeatherType::FreezingRain);
    if (needsTopDownDepth) {
        // Center the rain heightmap on the character (orbit target), not the
        // camera: at gameplay zoom the camera sits hundreds of meters behind,
        // leaving the character's own ground outside the heightmap (no
        // splashes, no rain surface occlusion there).
        const Vec3 rainAnchor = m_camera.target();
        m_postProcessor.weatherRenderer().renderTopDownDepth(
            cmd, rainAnchor,
            m_terrainRenderer, m_modelRenderer, m_spriteRenderer,
            m_shadowRenderer.shadowPipeline(), m_shadowRenderer.shadowLayout(),
            &m_bindless, frameUbo,
            m_spriteSystem.instanceBuffer(), m_spriteSystem.visibleCount());
        cpuMark(11);
        topDownDepthView = m_postProcessor.weatherRenderer().topDownDepthView();
    }
    m_vulkan.writeTimestamp(cmd, 25, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("Rain TopDown Depth", m_vulkan.timestampDeltaMs(24, 25));

    // Render volumetric cloud layers before post-processing.
    // NOTE (perf, tentado e revertido): pular este raymarch no modo field
    // muda o visual da tempestade - o weather overlay consome cloudColor/
    // cloudDepth como véu de overcast. Otimizar = trocar a fonte do véu,
    // não desligar o passe (docs/perf_log.md, item 3.1).
    m_vulkan.writeTimestamp(cmd, 26, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (m_cloudLayersEnabled) {
        m_cloudLayerRenderer.render(cmd, m_camera, env.sun.direction,
                                    env.sun.intensity, m_timer.elapsed(),
                                    static_cast<uint32_t>(m_framesCount));
        cpuMark(12);
    }
    m_vulkan.writeTimestamp(cmd, 27, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("Cloud Layers", m_vulkan.timestampDeltaMs(26, 27));

    m_vulkan.writeTimestamp(cmd, 8, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Vec3 sunVisualDir = m_dayNightCycle.getSunVisualDirection();
    Vec3 cameraDir = m_camera.forward();
    VkImageView cloudColorView = m_cloudLayersEnabled ? m_cloudLayerRenderer.cloudColorView() : VK_NULL_HANDLE;
    VkImageView cloudDepthView = m_cloudLayersEnabled ? m_cloudLayerRenderer.cloudDepthView() : VK_NULL_HANDLE;
    float cloudBaseHeight = m_cloudLayerRenderer.coverageArray().cloudBottom();
    if (m_postSettings.debugRainRegions) {
        for (const auto& icl : m_independentCloudLayers) {
            if (icl.hasRain) {
                float puffRy = 6.0f;
                for (const LocalCloud& lc : icl.array->localClouds()) {
                    if (!lc.alive) continue;
                    const float invF = 1.0f / std::max(lc.falloff, 0.01f);
                    puffRy = std::max(puffRy, glm::clamp(std::fmax(lc.radius.x, lc.radius.y) * invF * 0.6f, 6.0f, 100.0f));
                }
                cloudBaseHeight = icl.planeY + puffRy;
                break;
            }
        }
    }
    // During the automated benchmark skip the screen-space weather overlay so
    // the measurement isolates cloud geometry and base frame cost.
    // A caixa "Bloom" do menu de efeitos era ESCRITA e NUNCA LIDA: nada ligava
    // m_renderEffects[2] a m_postSettings.enableBloom, entao marcar ou
    // desmarcar nao mudava um pixel (relatado pelo autor 2026-09-03). O post
    // le' o ajuste, entao e' aqui que a caixa tem que chegar.
    if (m_renderEffects.size() > 2) m_postSettings.enableBloom = m_renderEffects[2]->isEnabled();
    const WeatherParams* weatherForPost = m_benchmarkThunderstorm ? nullptr : &m_weatherSystem.current();
    Vec3 cloudWorldMin = m_cloudLayerRenderer.coverageArray().worldMin();
    Vec3 cloudWorldMax = m_cloudLayerRenderer.coverageArray().worldMax();
    auto& coverageArray = m_cloudLayerRenderer.coverageArray();
    auto& cloudCfg = m_cloudLayerRenderer.config();
    float cloudLayer = glm::clamp(cloudCfg.layerCount * 0.5f, 0.0f, static_cast<float>(cloudCfg.layerCount - 1));
    m_postProcessor.setRainAnchor(m_camera.target());
    // O G-buffer nao carrega mais WorldPos (reconstruido do depth). Este slot
    // alimenta so' o overlay de debug de cobertura de nuvem, cujo shader
    // DECLARA u_worldPos e nunca o le' - entao o depth serve.
    m_postProcessor.render(cmd, m_deferredLighting.litImageView(), m_gbuffer.depthView(), m_gbuffer.normalView(),
                           m_gbuffer.normalView(), m_shadowRenderer.shadowAtlasView(), m_camera.viewMatrix(),
                           m_camera.projectionMatrix(), glm::inverse(m_camera.viewProjectionMatrix()), m_prevViewProj,
                           m_camera.position(), m_camera.nearPlane(), m_camera.farPlane(), env.sun.direction, postSettings,
                           &m_lookConfig, VK_NULL_HANDLE, m_timer.elapsed(), weatherForPost, m_weatherSystem.sunOcclusion(),
                           m_weatherSystem.moonOcclusion(), topDownDepthView, sunVisualDir, cameraDir, cloudColorView,
                           cloudDepthView, cloudBaseHeight, cloudWorldMin, cloudWorldMax,
                           coverageArray.imageView(), coverageArray.sampler(),
                           cloudLayer, cloudCfg.cloudAmount, currentWeather.cloudCoverage, coverageArray.windOffset(),
                           coverageArray.altitudeView(), coverageArray.altitudeSampler(),
                           coverageArray.cloudBottom(), coverageArray.cloudTop(), cloudCfg.cloudScale,
                           cloudVolOccReady ? m_cloudLayerRenderer.cloudVolumeAccumView() : VK_NULL_HANDLE);
    m_vulkan.writeTimestamp(cmd, 9, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    // 8..9 envolve o post inteiro; o DoF real esta' em 40..41 (PostProcessor).
    Profiler::addGpuTime("Post Total", m_vulkan.timestampDeltaMs(8, 9));
    cpuMark(13);


    m_prevViewProj = m_camera.viewProjectionMatrix();

    uint32_t sw = m_vulkan.swapExtent().width, sh = m_vulkan.swapExtent().height;
    // Era um blit BILINEAR cru (VK_FILTER_LINEAR) daqui pro swapchain - nao
    // existia AA nenhum na engine, e bilinear e' o upscale mais cego que ha'.
    // Agora: FXAA (resolucao de render) -> upscale bicubico Catmull-Rom +
    // nitidez adaptativa por contraste local (escrevendo direto no
    // swapchain). Ver UpscaleAA.hpp/.cpp e os dois shaders novos pro porque
    // de nao ser SMAA/FSR1 byte-a-byte (LUT externa e pesos nao-verificaveis).
    m_vulkan.cmdImageBarrier(cmd, m_postProcessor.outputImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    m_upscaleAA.render(cmd, m_postProcessor.outputView(), m_postProcessor.outputImage(),
                       m_vulkan.swapImageView(imageIndex), m_vulkan.swapImage(imageIndex),
                       {sw, sh}, m_enableFXAA, m_aaSharpenAmount);
    // takeScreenshot() (Screenshot.cpp) e o bloco de shimmer logo
    // abaixo dependem de outputImage() estar em TRANSFER_SRC_OPTIMAL no resto
    // do frame - era a pos-condicao do blit antigo. UpscaleAA::render so' LEU
    // a imagem (ficou em SHADER_READ_ONLY_OPTIMAL); repoe a mesma garantia.
    m_vulkan.cmdImageBarrier(cmd, m_postProcessor.outputImage(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    cpuMark(14);

    if (m_shimmerMeasureEnabled) {
        // outputImage() ja' esta' em TRANSFER_SRC_OPTIMAL (barreira logo
        // acima, a mesma garantia que takeScreenshot() tambem depende).
        m_vulkan.cmdImageBarrier(cmd, m_shimmerImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 };
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {128, 128, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        
        vkCmdBlitImage(cmd, m_postProcessor.outputImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_shimmerImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        
        m_vulkan.cmdImageBarrier(cmd, m_shimmerImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        
        uint32_t curFrameIdx = m_vulkan.currentFrame();
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {128, 128, 1};
        
        vkCmdCopyImageToBuffer(cmd, m_shimmerImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_shimmerBuffers[curFrameIdx], 1, &region);
        
        m_shimmerFrameCount++;
    } else {
        m_shimmerFrameCount = 0;
    }
    // A barreira do swap (TRANSFER_DST->COLOR_ATTACHMENT) que existia aqui era
    // pos-condicao do blit antigo. UpscaleAA::render ja' deixa o swap nesse
    // MESMO layout final (COLOR_ATTACHMENT_OPTIMAL, pronto pro ImGui desenhar
    // em cima). outputImage() ja' foi devolvido a TRANSFER_SRC_OPTIMAL logo
    // apos a chamada, ver acima.

    VkRenderingAttachmentInfo swapAttachment{}; swapAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    swapAttachment.imageView = m_vulkan.swapImageView(imageIndex); swapAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; swapAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    m_vulkan.cmdBeginRendering(cmd, {swapAttachment}, nullptr, nullptr, m_vulkan.swapExtent());

    if (!m_benchmarkThunderstorm && !(m_benchmarkHeavyRain && m_benchmarkHideHud)) {
        renderImGui();

        // In-game loading progress overlay
        renderLoadingProgress();
    }

    if (!m_benchmarkThunderstorm && !(m_benchmarkHeavyRain && m_benchmarkHideHud) && m_packManager.dirCount() == 0) {
        ImVec2 ws = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(ws.x * 0.5f, ws.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("No Pack Overlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        ImGui::SetWindowFontScale(3.0f); ImGui::TextColored(ImVec4(1, 0, 0, 1), "No pack :("); ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
    }

    if (!m_benchmarkThunderstorm && !(m_benchmarkHeavyRain && m_benchmarkHideHud)) {
        ImGui::Render(); ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    } else {
        // Benchmarks that skip the UI still call ImGui::NewFrame above: close
        // the frame so the frame bookkeeping stays balanced (a stray mid-frame
        // NewFrame — e.g. a loading screen — otherwise trips the "Forgot to
        // call Render()" sanity assert).
        ImGui::EndFrame();
    }

    // Capture screenshot including UI overlay.
    if (m_screenshotPending && m_screenshotStaging == VK_NULL_HANDLE) {
        uint32_t ssW = m_vulkan.swapExtent().width, ssH = m_vulkan.swapExtent().height;
        m_screenshotStagingW = ssW;
        m_screenshotStagingH = ssH;
        if (m_vulkan.createBuffer(ssW * ssH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_GPU_TO_CPU,
                                  m_screenshotStaging, m_screenshotStagingAlloc)) {
            m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex),
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {ssW, ssH, 1};
            vkCmdCopyImageToBuffer(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   m_screenshotStaging, 1, &region);
            m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex),
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            m_screenshotSubmitFrame = m_vulkan.currentFrame();
        }
    }

    m_vulkan.cmdEndRendering(cmd);
    m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
    m_vulkan.endFrame();
    cpuMark(15);
    if (m_benchmarkThunderstormStarted) ++m_benchmarkCpuSamples;
    m_framesCount++;
    if (m_framesAfterSwap >= 0) m_framesAfterSwap++;
    // ERUPTION_TEST_CAM_ORBIT=degPerFrame (debug): spin the camera yaw every frame.
    static const float camOrbit = [] {
        const char* e = std::getenv("ERUPTION_TEST_CAM_ORBIT");
        return e ? static_cast<float>(std::atof(e)) : 0.0f;
    }();
    if (camOrbit != 0.0f) m_camera.orbitYaw(glm::radians(camOrbit));
    // ERUPTION_TEST_CAM_DIST=<dist> (debug): trava a DISTANCIA da orbita todo
    // frame, mantendo alvo/yaw/pitch do mapa. 0% de zoom = 1000. Serve pra
    // medir custo por zoom sem depender do alvo de spawn (que CAM_POSE exige).
    // Aceita tambem "dist,pitchGraus" para fixar a inclinacao.
    static float camDist = 0.0f, camPitchDeg = -1.0f;
    static const bool camDistParsed = [] {
        const char* e = std::getenv("ERUPTION_TEST_CAM_DIST");
        if (e) std::sscanf(e, "%f,%f", &camDist, &camPitchDeg);
        return true;
    }();
    (void)camDistParsed;
    if (camDist > 0.0f) {
        const float pitch = camPitchDeg > 0.0f ? glm::radians(camPitchDeg) : m_camera.orbitPitch();
        m_camera.setOrbit(m_camera.orbitYaw(), pitch, camDist);
    }
    // ERUPTION_TEST_CAM_ZOOM_RAMP="d0,d1,frames" (debug): rampa LINEAR de
    // distancia entre d0 e d1 ao longo de N frames a partir do frame 90 (e
    // fica em d1). Reproduz "tirar o zoom" em cidade-A: o pop de culling de
    // sombra que o autor ve' so' aparece na TRANSICAO, nao em prints parados.
    static const char* zoomRamp = std::getenv("ERUPTION_TEST_CAM_ZOOM_RAMP");
    if (zoomRamp) {
        static float r0 = 200.0f, r1 = 1000.0f, rf = 60.0f;
        static bool rParsed = [] { std::sscanf(zoomRamp, "%f,%f,%f", &r0, &r1, &rf); if (rf < 1.0f) rf = 1.0f; return true; }();
        (void)rParsed;
        const float t = glm::clamp((static_cast<float>(m_framesCount) - 90.0f) / rf, 0.0f, 1.0f);
        m_camera.setOrbit(m_camera.orbitYaw(), m_camera.orbitPitch(), glm::mix(r0, r1, t));
    }
    // ERUPTION_TEST_CAM_TUMBLE="yawDegPorFrame,pitchAmpGraus,dist" (debug):
    // orbita RAPIDA em yaw E pitch ao mesmo tempo (pitch em senoide de
    // amplitude dada, em torno do pitch atual), com distancia fixa. Reproduz
    // o gesto do autor 2026-09-02: "movo a camera rapidamente (pitch + yaw,
    // orbitando mesmo) com zoom 0 e vejo shimmering na sombra" - o orbit de
    // yaw sozinho nao muda a inclinacao da luz em relacao ao frustum, e e' a
    // variacao de pitch que mais mexe na esfera de cada cascata.
    static const char* camTumble = std::getenv("ERUPTION_TEST_CAM_TUMBLE");
    if (camTumble) {
        static float tYaw = 1.0f, tPitchAmp = 20.0f, tDist = 1000.0f;
        static float basePitch = -1.0f;
        static bool parsed = [] {
            std::sscanf(camTumble, "%f,%f,%f", &tYaw, &tPitchAmp, &tDist);
            return true;
        }();
        (void)parsed;
        if (basePitch < 0.0f) basePitch = m_camera.orbitPitch();
        const float t = m_timer.elapsed();
        const float pitch = glm::clamp(basePitch + glm::radians(tPitchAmp) * std::sin(t * 2.0f),
                                       glm::radians(10.0f), glm::radians(70.0f));
        m_camera.setOrbit(m_camera.orbitYaw() + glm::radians(tYaw), pitch, tDist);
    }
    // ERUPTION_TEST_CAM_SWEEP=1 (debug): varredura automatica do mapa para
    // medir CACHE HIT/MISS sob movimento real - orbita + ZOOM ciclico +
    // TRANSLACAO do alvo cobrindo o mapa inteiro. Cache hit/miss so' significa
    // algo sob movimento: parado, a lista visivel nao muda e a taxa fica
    // artificialmente perfeita. Perfil: zoom em senoide (perto<->longe, cobre
    // as trocas de LOD) e alvo percorrendo uma figura de Lissajous sobre a
    // extensao do terreno (cobre o mapa sem repetir trajeto).
    static const bool camSweep = std::getenv("ERUPTION_TEST_CAM_SWEEP") != nullptr;
    if (camSweep) {
        // ERUPTION_TEST_CAM_SWEEP_FRAMES=<fps>: pilota a varredura pelo NUMERO
        // DE FRAME em vez do relogio de parede, assumindo <fps> quadros por
        // segundo. Sem isto o mesmo teste visita paisagens DIFERENTES a cada
        // execucao: o percurso avanca com o tempo real, entao uma execucao
        // mais rapida cobre mais mapa nos mesmos N frames e mede outra cena.
        // Era a maior fonte de variancia entre rodadas do portao dos 120 FPS
        // (a mesma configuracao deu media 8,15, 8,58 e 8,87 ms em rodadas
        // diferentes). Com passo por frame, frame N e' SEMPRE a mesma pose e
        // duas execucoes viram comparaveis.
        static const float sweepFps = [] {
            const char* e = std::getenv("ERUPTION_TEST_CAM_SWEEP_FRAMES");
            return (e && std::atof(e) > 0.0) ? static_cast<float>(std::atof(e)) : 0.0f;
        }();
        const float t = (sweepFps > 0.0f)
            ? static_cast<float>(m_framesCount) / sweepFps
            : m_timer.elapsed();
        // Extensao do mapa pelas instancias (mesma fonte que o minimapa usa),
        // computada UMA vez: o alvo precisa cobrir o mapa inteiro.
        static Vec3 sweepMin(0.0f), sweepMax(0.0f);
        static bool sweepBoundsReady = false;
        if (!sweepBoundsReady) {
            Vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
            bool any = false;
            for (const auto& inst : m_modelRenderer.getInstances()) {
                if (!inst.enabled) continue;
                bmin = glm::min(bmin, inst.worldAabbMin);
                bmax = glm::max(bmax, inst.worldAabbMax);
                any = true;
            }
            if (any && bmax.x > bmin.x && bmax.z > bmin.z) {
                sweepMin = bmin; sweepMax = bmax; sweepBoundsReady = true;
            }
        }
        const Vec3 mn = sweepBoundsReady ? sweepMin : m_camera.target() - Vec3(500.0f);
        const Vec3 mx = sweepBoundsReady ? sweepMax : m_camera.target() + Vec3(500.0f);
        const Vec3 c = (mn + mx) * 0.5f;
        const float hx = std::max((mx.x - mn.x) * 0.42f, 1.0f);
        const float hz = std::max((mx.z - mn.z) * 0.42f, 1.0f);
        // Lissajous 3:2 - trajeto denso, sem repetir cedo.
        m_camera.setOrbitTarget(Vec3(c.x + hx * std::sin(t * 0.21f),
                                     c.y,
                                     c.z + hz * std::sin(t * 0.14f + 1.57f)));
        // Zoom entre 15% e 95% do alcance util (dist 10..1000).
        const float zt = 0.5f + 0.5f * std::sin(t * 0.37f);
        m_camera.setOrbit(m_camera.orbitYaw(), m_camera.orbitPitch(),
                          glm::mix(150.0f, 900.0f, zt));
    }
    // ERUPTION_TEST_CAM_POSE="tx,ty,tz,yawDeg,pitchDeg,dist" (debug): force the
    // orbit camera to an exact pose every frame — reproduces user-reported
    // views for screenshot comparison. 0% zoom = dist 1000.
    // Os hooks de camera abaixo emulam camera livre/tatica (varrem pitch ate'
    // 89), entao SOBEM o teto de pitch - depois do fix de limite unico, um
    // setOrbit(.., 89, ..) seria clampado pra 70 no modo de jogo normal.
    static const bool kCamHookActive =
        std::getenv("ERUPTION_TEST_CAM_POSE") || std::getenv("ERUPTION_TEST_CAM_DOLLY") ||
        std::getenv("ERUPTION_TEST_CAM_PITCH") || std::getenv("ERUPTION_TEST_CAM_OSC") ||
        std::getenv("ERUPTION_TEST_CAM_DRAG");
    // ERUPTION_TEST_KEEP_PITCH_LIMITS=1: NAO alarga - serve pra testar o
    // comportamento de jogo (limites 10..70) pelo caminho de entrada real.
    static const bool kKeepLimits = std::getenv("ERUPTION_TEST_KEEP_PITCH_LIMITS") != nullptr;
    if (kCamHookActive && !kKeepLimits) m_camera.setPitchLimits(glm::radians(-89.0f), glm::radians(89.0f));
    static const char* camPose = std::getenv("ERUPTION_TEST_CAM_POSE");
    if (camPose) {
        float pt[6] = {0};
        if (std::sscanf(camPose, "%f,%f,%f,%f,%f,%f",
                        &pt[0], &pt[1], &pt[2], &pt[3], &pt[4], &pt[5]) == 6) {
            m_camera.setOrbitTarget(Vec3(pt[0], pt[1], pt[2]));
            m_camera.setOrbit(glm::radians(pt[3]), glm::radians(pt[4]), pt[5]);
        }
    }
    // ERUPTION_TEST_CAM_DOLLY="tx,ty,tz,yawDeg,pitchDeg,distStart,distEnd,frames"
    // (debug): pose FIXA (alvo/yaw/pitch parados) com a distancia interpolada
    // linearmente ao longo de N frames - so' a distancia muda, entao qualquer
    // salto de RMS entre frames consecutivos e' o LOD trocando de nivel, nao
    // rotacao/paralaxe de camera. Usado por perf/test_lod_pop.py.
    static const char* camDolly = std::getenv("ERUPTION_TEST_CAM_DOLLY");
    if (camDolly) {
        float pt[8] = {0};
        if (std::sscanf(camDolly, "%f,%f,%f,%f,%f,%f,%f,%f",
                        &pt[0], &pt[1], &pt[2], &pt[3], &pt[4], &pt[5], &pt[6], &pt[7]) == 8) {
            const float frames = std::max(pt[7], 1.0f);
            const float frac = glm::clamp(static_cast<float>(m_framesCount) / frames, 0.0f, 1.0f);
            const float dist = glm::mix(pt[5], pt[6], frac);
            m_camera.setOrbitTarget(Vec3(pt[0], pt[1], pt[2]));
            m_camera.setOrbit(glm::radians(pt[3]), glm::radians(pt[4]), dist);
        }
    }
    // ERUPTION_TEST_CAM_PITCH="tx,ty,tz,yawDeg,pitchStart,pitchEnd,dist,frames"
    // (debug): igual ao DOLLY, mas quem interpola e' o PITCH (alvo/yaw/
    // distancia parados). E' o gesto que o autor reportou como "jitter
    // esquisito" (zoom 0, pitch de 89 pra 10, 2026-09-03): varrer o pitch
    // muda a distancia camera->instancia de MUITAS instancias ao mesmo tempo,
    // entao qualquer degrau na troca de LOD aparece de uma vez so'.
    static const char* camPitch = std::getenv("ERUPTION_TEST_CAM_PITCH");
    if (camPitch) {
        float pt[8] = {0};
        if (std::sscanf(camPitch, "%f,%f,%f,%f,%f,%f,%f,%f",
                        &pt[0], &pt[1], &pt[2], &pt[3], &pt[4], &pt[5], &pt[6], &pt[7]) == 8) {
            const float frames = std::max(pt[7], 1.0f);
            const float frac = glm::clamp(static_cast<float>(m_framesCount) / frames, 0.0f, 1.0f);
            const float pitch = glm::mix(pt[4], pt[5], frac);
            m_camera.setOrbitTarget(Vec3(pt[0], pt[1], pt[2]));
            m_camera.setOrbit(glm::radians(pt[3]), glm::radians(pitch), pt[6]);
        }
    }
    // ERUPTION_TEST_CAM_OSC="tx,ty,tz,yawDeg,pitchMin,pitchMax,dist,framesPorVarredura"
    // (debug): pitch em onda TRIANGULAR (sobe, desce, sobe...), que e' o gesto
    // que o autor usa pra reproduzir o "jump" da camera (2026-09-04: parana_demo,
    // yaw -134.1, pitch 10<->89 em ~1 s, varias vezes). Diferente do CAM_PITCH,
    // que faz uma passagem so'.
    static const char* camOsc = std::getenv("ERUPTION_TEST_CAM_OSC");
    if (camOsc) {
        float pt[8] = {0};
        if (std::sscanf(camOsc, "%f,%f,%f,%f,%f,%f,%f,%f",
                        &pt[0], &pt[1], &pt[2], &pt[3], &pt[4], &pt[5], &pt[6], &pt[7]) == 8) {
            const float sweep = std::max(pt[7], 1.0f);
            // Onda triangular: 0->1 na primeira varredura, 1->0 na segunda.
            const float phase = std::fmod(static_cast<float>(m_framesCount) / sweep, 2.0f);
            const float tri = (phase <= 1.0f) ? phase : (2.0f - phase);
            const float pitch = glm::mix(pt[4], pt[5], tri);
            m_camera.setOrbitTarget(Vec3(pt[0], pt[1], pt[2]));
            m_camera.setOrbit(glm::radians(pt[3]), glm::radians(pitch), pt[6]);
        }
    }
    // ERUPTION_TEST_CAM_DRAG="yawDeg,pitchInicial,pitchMin,pitchMax,dist,framesPorVarredura"
    // (debug): igual ao CAM_OSC, mas passando pelo CAMINHO DE ENTRADA REAL -
    // uma pose inicial com setOrbit() (o que a visao tatica do F7 faz) e
    // depois DELTAS por Camera::orbitPitch(), que e' o que o arrasto do mouse
    // chama. E' o unico jeito de exercitar os limites de pitch: setOrbit nao
    // clampa e orbitPitch clampa.
    // ERUPTION_TEST_FREE_CAM=0|1 (debug): forca o modo de camera livre (o que o
    // botao do meio do mouse liga). Os LIMITES DE PITCH dependem dele:
    // [10,70] no jogo normal, [-89,89] na livre - e e' exatamente ai' que
    // mora o salto investigado (setOrbit nao clampa, orbitPitch clampa).
    if (const char* fc = std::getenv("ERUPTION_TEST_FREE_CAM")) {
        m_camera.setDebugFreeCamera(std::atoi(fc) != 0);
    }
    static const char* camDrag = std::getenv("ERUPTION_TEST_CAM_DRAG");
    if (camDrag) {
        float pt[6] = {0};
        if (std::sscanf(camDrag, "%f,%f,%f,%f,%f,%f",
                        &pt[0], &pt[1], &pt[2], &pt[3], &pt[4], &pt[5]) == 6) {
            const float sweep = std::max(pt[5], 1.0f);
            if (m_framesCount <= 1) {
                m_camera.setOrbitTarget(Vec3(0.0f));
                m_camera.setOrbit(glm::radians(pt[0]), glm::radians(pt[1]), pt[4]);
            } else {
                // Passo FIXO por frame, como o mouse: nunca "teleporta" pro
                // valor desejado. Inverte o sentido nas pontas da faixa.
                static float s_dir = -1.0f;
                const float passo = (pt[3] - pt[2]) / sweep;
                const float atual = glm::degrees(m_camera.orbitPitch());
                if (atual <= pt[2] + passo) s_dir = 1.0f;
                else if (atual >= pt[3] - passo) s_dir = -1.0f;
                m_camera.orbitPitch(glm::radians(passo * s_dir));
            }
        }
    }
    // ERUPTION_DEBUG_CAMLOG=1 (debug): estado da camera POR FRAME, depois de
    // todos os hooks. E' o instrumento pra achar descontinuidade: numa
    // varredura suave a posicao anda pouco por frame, e um "jump" e' um passo
    // muito maior que os vizinhos.
    static const bool kCamLog = std::getenv("ERUPTION_DEBUG_CAMLOG") != nullptr;
    if (kCamLog) {
        const Vec3 p = m_camera.position(), t = m_camera.target();
        ERUPTION_LOG_WARN("[CAMLOG] frame=%u pos=%.4f,%.4f,%.4f alvo=%.4f,%.4f,%.4f "
                          "yaw=%.5f pitch=%.5f dist=%.4f livre=%d",
                          m_framesCount, p.x, p.y, p.z, t.x, t.y, t.z,
                          glm::degrees(m_camera.orbitYaw()), glm::degrees(m_camera.orbitPitch()),
                          m_camera.orbitDistance(), m_camera.debugFreeCamera() ? 1 : 0);
    }
    // ERUPTION_TEST_EXIT_FRAME=N (debug): encerra no frame N sem depender de
    // --screenshot/--auto-exit (que param no primeiro PNG). Usado com o dump
    // de todos os frames abaixo.
    static const int kExitFrame = [] {
        const char* e = std::getenv("ERUPTION_TEST_EXIT_FRAME");
        return (e && std::atoi(e) > 0) ? std::atoi(e) : 0;
    }();
    if (kExitFrame > 0 && m_framesCount >= kExitFrame) m_running = false;
    // ERUPTION_TEST_VMA_DUMP_FRAME=N + ERUPTION_TEST_VMA_DUMP=<arquivo>: o
    // inventario do VMA NO MEIO DO FRAME N (o do shutdown so' ve' o que vazou).
    // E' como se responde "quem e' dono da VRAM" sem instrumentar cada site
    // de alocacao - o JSON traz tipo, tamanho e uso de cada alocacao viva.
    static const int kVmaDumpFrame = [] {
        const char* e = std::getenv("ERUPTION_TEST_VMA_DUMP_FRAME");
        return (e && std::atoi(e) > 0) ? std::atoi(e) : 0;
    }();
    if (kVmaDumpFrame > 0 && static_cast<int>(m_framesCount) == kVmaDumpFrame) {
        if (const char* dump = std::getenv("ERUPTION_TEST_VMA_DUMP")) m_vulkan.dumpVmaStats(dump);
    }
    // ERUPTION_TEST_SWAP_SHOT="caminho,frame" (debug): captura o SWAPCHAIN
    // (scheduleScreenshotFromSwap), DEPOIS do ImGui compor por cima - ao
    // contrario de --screenshot/takeScreenshot(), que le' m_postProcessor.
    // outputImage() e sai ANTES do passe de UI rodar. Toda investigacao de UI
    // (linha cinza, HUD, gizmo) tem que usar este hook - o outro e' cego pra
    // qualquer coisa desenhada pelo ImGui. Usar com ERUPTION_TEST_EXIT_FRAME
    // (alguns frames depois, pra finishPendingScreenshot() ter tempo de
    // esvaziar a fence) em vez de --auto-exit, que pula este caminho de
    // proposito (vazaria o staging buffer - ver o comentario ao lado do
    // scheduleScreenshotFromSwap mais abaixo).
    static const char* swapShot = std::getenv("ERUPTION_TEST_SWAP_SHOT");
    if (swapShot) {
        static std::string s_path;
        static uint32_t s_frame = 0;
        static bool s_parsed = false, s_done = false;
        if (!s_parsed) {
            s_parsed = true;
            std::string spec(swapShot);
            size_t comma = spec.rfind(',');
            if (comma != std::string::npos) {
                s_path = spec.substr(0, comma);
                s_frame = static_cast<uint32_t>(std::atoi(spec.c_str() + comma + 1));
            }
        }
        if (!s_done && !s_path.empty() && static_cast<uint32_t>(m_framesCount) == s_frame) {
            s_done = true;
            scheduleScreenshotFromSwap(s_path);
        }
    }
    // ERUPTION_TEST_WEATHER_TIER=mobile|medium|high (debug): force the weather tier
    // in any mode — headless screenshot runs sit at the default Medium, which
    // skips the High-only 3D splash pass entirely.
    static const int forcedTier = [] {
        const char* e = std::getenv("ERUPTION_TEST_WEATHER_TIER");
        if (!e) return -1;
        const std::string s(e);
        if (s == "mobile") return 0;
        if (s == "medium") return 1;
        if (s == "high") return 2;
        return -1;
    }();
    if (forcedTier >= 0) m_postSettings.weatherTier = static_cast<WeatherTier>(forcedTier);
    // ERUPTION_TEST_WEATHER_SWITCH="frame,weather_name" (debug): switch the weather
    // once at the given frame — reproduces in-game weather-tab switches for
    // headless screenshot runs (e.g. sandstorm -> rain transition state).
    static const char* weatherSwitch = std::getenv("ERUPTION_TEST_WEATHER_SWITCH");
    static bool weatherSwitchDone = false;
    if (weatherSwitch && !weatherSwitchDone) {
        int swFrame = 0; char swName[64] = {0};
        if (std::sscanf(weatherSwitch, "%d,%63s", &swFrame, swName) == 2 &&
            m_framesCount >= swFrame) {
            weatherSwitchDone = true;
            setInitialWeatherType(swName);
            ERUPTION_LOG_WARN("[TEST] Weather switched to '%s' at frame %u", swName, m_framesCount);
        }
    }
    // ERUPTION_TEST_WEATHER_CYCLE="framesPorClima,nome1,nome2,..." (debug):
    // a partir do frame 90 troca o clima a cada N frames seguindo a lista.
    // A telemetria marca weather.type em cada frame, entao UMA execucao por
    // mapa cobre todos os climas (pedido do autor 2026-09-02: teste de todos
    // os climas, incluindo os extremos, em parana/cidade-A/instancia-A/campo-A).
    static const char* weatherCycle = std::getenv("ERUPTION_TEST_WEATHER_CYCLE");
    if (weatherCycle) {
        static int cyPeriod = 0;
        static std::vector<std::string> cyNames;
        static size_t cyNext = 0;
        static bool cyParsed = [] {
            std::string spec(weatherCycle);
            size_t pos = 0; int field = 0;
            while (pos <= spec.size()) {
                size_t e = spec.find(',', pos); if (e == std::string::npos) e = spec.size();
                std::string tok = spec.substr(pos, e - pos);
                if (field == 0) cyPeriod = std::atoi(tok.c_str());
                else if (!tok.empty()) cyNames.push_back(tok);
                ++field; pos = e + 1;
            }
            if (cyPeriod <= 0) cyPeriod = 60;
            return true;
        }();
        (void)cyParsed;
        if (cyNext < cyNames.size() &&
            m_framesCount >= 90 + cyPeriod * static_cast<int>(cyNext)) {
            setInitialWeatherType(cyNames[cyNext]);
            ERUPTION_LOG_WARN("[TEST] Weather cycle -> '%s' (frame %u)", cyNames[cyNext].c_str(), m_framesCount);
            ++cyNext;
        }
    }
       // ERUPTION_TEST_FRAME_DUMP=prefix (debug): screenshot every frame in
    // [90, ERUPTION_TEST_FRAME_DUMP_END or 190] into prefix_NNNN.png, to catch
    // camera-motion pops static shots miss.
    static const char* frameDump = std::getenv("ERUPTION_TEST_FRAME_DUMP");
    static const int frameDumpEnd = [] {
        const char* e = std::getenv("ERUPTION_TEST_FRAME_DUMP_END");
        return (e && std::atoi(e) > 0) ? std::atoi(e) : 190;
    }();
    if (frameDump && m_framesCount >= 90 && m_framesCount <= frameDumpEnd) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s_%04u.png", frameDump, m_framesCount);
        takeScreenshot(path);
    }

    // ERUPTION_TEST_TIME_SWEEP="startHour,endHour,stepHour" (debug): from ONE
    // engine launch, pin the clock to each hour in turn and screenshot it into
    // <--screenshot base>_hNN.png. Used to inspect light-direction-dependent
    // artefacts (shadow crawl, normal-map "breathing") without re-launching.
    static const char* timeSweep = std::getenv("ERUPTION_TEST_TIME_SWEEP");
    if (timeSweep && !m_screenshotOnLoad.empty()) {
        static float swStart = 0.0f, swEnd = 7.0f, swStep = 1.0f;
        static bool swParsed = [&] {
            std::sscanf(timeSweep, "%f,%f,%f", &swStart, &swEnd, &swStep);
            if (swStep <= 0.0f) swStep = 1.0f;
            return true;
        }();
        (void)swParsed;
        static const int kWarmup = 80;     // let assets/shadows settle first
        static const int kSettle = 10;     // frames to hold each hour before the shot
        static float swHour = swStart;
        static int swPhase = 0;            // 0 = set time, 1 = wait, then shoot
        static int swWaitUntil = 0;
        static bool swDone = false;
        if (!swDone && m_framesCount >= kWarmup) {
            if (swPhase == 0) {
                m_dayNightCycle.setTimeOfDay(swHour / 24.0f);
                m_dayNightCycle.setPaused(true);
                swWaitUntil = m_framesCount + kSettle;
                swPhase = 1;
            } else if (m_framesCount >= swWaitUntil) {
                std::string base = m_screenshotOnLoad;
                auto dot = base.find_last_of('.');
                std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
                std::string ext  = (dot == std::string::npos) ? ".png" : base.substr(dot);
                char path[600];
                std::snprintf(path, sizeof(path), "%s_h%02d%s", stem.c_str(),
                              (int)std::lround(swHour), ext.c_str());
                takeScreenshot(path);
                ERUPTION_LOG_WARN("[TEST] time-sweep shot %s (hour %.1f)", path, swHour);
                swHour += swStep;
                swPhase = 0;
                if (swHour > swEnd + 1e-3f) {
                    swDone = true;
                    if (m_enableAutoExit) m_running = false;
                }
            }
        }
    }

    if (!m_screenshotOnLoad.empty() && !timeSweep) {
        // Keep the user-requested camera distance so screenshots show the actual
        // camera frustum (especially important for sky/cloud visibility).
        // ERUPTION_TEST_SCREENSHOT_FRAME=N (debug): trigger the screenshot at frame N
        // instead of 600 (heavy cloud-field runs can take minutes to reach 600).
        static const int screenshotFrame = [] {
            const char* e = std::getenv("ERUPTION_TEST_SCREENSHOT_FRAME");
            return (e && std::atoi(e) > 0) ? std::atoi(e) : 600;
        }();
        bool shouldScreenshot = false;
        if (m_playerController->m_autoTest) {
            if (m_framesAfterSwap == screenshotFrame) shouldScreenshot = true;
        } else {
            if (m_framesCount == screenshotFrame) shouldScreenshot = true;
        }
        if (shouldScreenshot) {
            ERUPTION_LOG_WARN("[TEST] FPS at screenshot: %.1f (%.2f ms)", m_fps, m_frameTime);
            if (const char* mmPath = std::getenv("ERUPTION_TEST_DUMP_MINIMAP")) dumpMinimap(mmPath);
            for (const auto& [name, ms] : Profiler::getAllGpuTimes()) {
                ERUPTION_LOG_WARN("[TEST] GPU %-24s %.2f ms", name.c_str(), ms);
            }
            if (std::getenv("ERUPTION_TEST_CPU_MARKS")) {
                ERUPTION_LOG_WARN("[TEST] CPU %-24s %.2f ms", "advanceFrame (sim)", m_cpuAdvanceMs);
                ERUPTION_LOG_WARN("[TEST] CPU %-24s %.2f ms", "app onUpdate", m_cpuAppUpdateMs);
                ERUPTION_LOG_WARN("[TEST] CPU %-24s %.2f ms", "render (record)", m_cpuRenderMs);
                ERUPTION_LOG_WARN("[TEST] CPU %-24s %.2f ms", "tail", m_cpuTailMs);
                ERUPTION_LOG_WARN("[TEST] CPU %-24s %.2f ms", "beginFrame (fence+acq)", m_cpuBeginFrameMs);
                static const char* kCpuStage[] = {
                    "0 (start)", "1 upload+update", "2 shadows", "3 gbuffer-begin",
                    "4 gbuffer-draw", "5 lighting", "6 sprites/fwd", "7 cloud-field",
                    "8 cloud-volumes", "9 postprocess", "10 imgui", "11 submit/present"
                };
                for (size_t i = 1; i < m_benchmarkCpuFrameMs.size(); ++i) {
                    const char* nm = (i < sizeof(kCpuStage)/sizeof(kCpuStage[0])) ? kCpuStage[i] : "?";
                    ERUPTION_LOG_WARN("[TEST] CPU %-24s %.2f ms", nm, m_benchmarkCpuFrameMs[i]);
                }
            }
            takeScreenshot(m_screenshotOnLoad); if (m_enableAutoExit) m_running = false;
        }
        // Only schedule the swapchain screenshot path when we are staying alive:
        // in auto-exit mode the next frame never runs, so the pending staging
        // buffer would leak and trigger the VMA "allocations not freed" assert.
        if (shouldScreenshot && !m_enableAutoExit) scheduleScreenshotFromSwap(m_screenshotOnLoad);
    }

    // A/B automated capture: take two screenshots a few frames apart, one with
    // PBR on and one with PBR off, so they can be diffed externally. The swap
    // screenshot captures the same frame it is scheduled in, so the state change
    // must happen one frame before the screenshot is scheduled.
    if (m_abCaptureStep > 0 && m_framesCount >= m_abCaptureFrame) {
        switch (m_abCaptureStep) {
            case 1: // settle PBR on
                m_deferredLighting.setUsePbr(true);
                m_abCaptureFrame = m_framesCount + 2;
                m_abCaptureStep = 2;
                break;
            case 2: // capture PBR on
                scheduleScreenshotFromSwap("/tmp/ab_pbr/ab_pbr_on.png");
                m_abCaptureFrame = m_framesCount + 1;
                m_abCaptureStep = 3;
                break;
            case 3: // switch to PBR off
                m_deferredLighting.setUsePbr(false);
                m_abCaptureFrame = m_framesCount + 2;
                m_abCaptureStep = 4;
                break;
            case 4: // capture PBR off
                scheduleScreenshotFromSwap("/tmp/ab_pbr/ab_pbr_off.png");
                m_abCaptureFrame = m_framesCount + 1;
                m_abCaptureStep = 5;
                break;
            case 5: // restore PBR on
                m_deferredLighting.setUsePbr(true);
                ERUPTION_LOG_WARN("A/B capture done: /tmp/ab_pbr/ab_pbr_on.png + ab_pbr_off.png");
                m_abCaptureStep = 0;
                break;
        }
    }

    if (m_screenshotPending) finishPendingScreenshot();

    // ERUPTION_TEST_DUMP_FRAMES=<dir> (debug): grava TODO frame como PNG.
    // O caminho normal de screenshot difere a leitura (fence com timeout 0) e
    // pularia frames; aqui espera a fence de verdade, porque o objetivo e' a
    // sequencia COMPLETA - e' o unico jeito de ver um "jump" de um frame pro
    // outro. So' pra investigacao headless: e' lento (um PNG por frame).
    static const char* kDumpDir = std::getenv("ERUPTION_TEST_DUMP_FRAMES");
    if (kDumpDir && !m_screenshotPending) {
        static bool dirReady = false;
        if (!dirReady) {
            std::error_code ec;
            std::filesystem::create_directories(kDumpDir, ec);
            dirReady = true;
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/f%05u.png", kDumpDir, m_framesCount);
        scheduleScreenshotFromSwap(path);
    }
}

// ERUPTION_TEST_DEMO="t:chave=valor;t:chave=valor;..." (bancada de VIDEO).
// Roteiro por TEMPO (segundos desde o primeiro frame renderizado) numa UNICA
// execucao: o autor aperta gravar no OBS e sai um take sem edicao - nada de
// recarregar mapa entre cenas. Chaves:
//   cam=tx,ty,tz,yaw,pitch,dist  keyframe de camera; entre keyframes a pose e'
//                                interpolada com smoothstep (sem arranque)
//   wire=0|1      wireframe (mesmo caminho do F8: recria pipelines)
//   lights=0|1    passe de point lights (RenderEffect 4)
//   probes=0|1    sonda de ceu + probes de irradiancia
//   stats=0|1     painel F3 fixo a' direita (metricas por passe na tela)
//   overlay=0|1   0 = esconde atalhos/control panel/minimapa (F3 e FPS ficam)
//   dof=0|1       profundidade de campo
//   time=<0..1>   hora do dia;  timescale=<x>  velocidade do dia (0 = parado)
//   weather=<nome>  troca de clima (mesmo caminho do ciclo de teste)
//   baselod=0|1   forca a malha cheia (sem LOD) em tudo
//   exit=1        encerra a execucao
void Engine::runDemoTimeline(const char* spec) {
    struct Ev { float t; std::string key, val; bool done = false; };
    struct Cam { float t; float p[6]; };
    static std::vector<Ev> evs;
    static std::vector<Cam> cams;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        std::string s(spec);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t e = s.find(';', pos); if (e == std::string::npos) e = s.size();
            std::string tok = s.substr(pos, e - pos); pos = e + 1;
            const size_t c = tok.find(':'), q = tok.find('=');
            if (c == std::string::npos || q == std::string::npos || q < c) continue;
            Ev ev; ev.t = std::strtof(tok.c_str(), nullptr);
            ev.key = tok.substr(c + 1, q - c - 1); ev.val = tok.substr(q + 1);
            if (ev.key == "cam") {
                Cam k{ev.t, {0, 0, 0, 0, 0, 0}};
                if (std::sscanf(ev.val.c_str(), "%f,%f,%f,%f,%f,%f",
                                &k.p[0], &k.p[1], &k.p[2], &k.p[3], &k.p[4], &k.p[5]) == 6) cams.push_back(k);
            } else {
                evs.push_back(ev);
            }
        }
        std::sort(cams.begin(), cams.end(), [](const Cam& a, const Cam& b) { return a.t < b.t; });
        ERUPTION_LOG_WARN("[DEMO] roteiro: %zu acoes, %zu keyframes de camera", evs.size(), cams.size());
    }
    if (m_demoStartTime < 0.0f) m_demoStartTime = m_timer.elapsed();
    const float t = m_timer.elapsed() - m_demoStartTime;
    if (!cams.empty()) {
        const Cam* a = &cams.front();
        const Cam* b = a;
        for (size_t i = 0; i < cams.size(); ++i) {
            if (cams[i].t <= t) { a = &cams[i]; b = (i + 1 < cams.size()) ? &cams[i + 1] : a; }
        }
        float f = (b->t > a->t) ? glm::clamp((t - a->t) / (b->t - a->t), 0.0f, 1.0f) : 0.0f;
        f = f * f * (3.0f - 2.0f * f);
        float p[6];
        for (int i = 0; i < 6; ++i) p[i] = glm::mix(a->p[i], b->p[i], f);
        m_camera.setOrbitTarget(Vec3(p[0], p[1], p[2]));
        m_camera.setOrbit(glm::radians(p[3]), glm::radians(p[4]), p[5]);
    }
    for (auto& ev : evs) {
        if (ev.done || t < ev.t) continue;
        ev.done = true;
        const bool on = ev.val != "0";
        ERUPTION_LOG_WARN("[DEMO] t=%.1f %s=%s", t, ev.key.c_str(), ev.val.c_str());
        if (ev.key == "wire") { if (wireframeMode() != on) toggleWireframe(); }
        else if (ev.key == "lights") { if (m_renderEffects.size() > 4) m_renderEffects[4]->setEnabled(on); }
        else if (ev.key == "probes") { m_irradianceProbesEnabled = on; m_skyProbeEnabled = on; }
        else if (ev.key == "stats") { m_showStatsMenu = on; m_demoPinStats = on; }
        else if (ev.key == "overlay") { m_demoHidePanels = !on; m_showDebugOverlay = true; } // 0 = so' F3 + FPS
        else if (ev.key == "dof") m_postSettings.enableDoF = on;
        else if (ev.key == "time") m_dayNightCycle.setTimeOfDay(std::strtof(ev.val.c_str(), nullptr));
        else if (ev.key == "timescale") m_dayNightCycle.setTimeScale(std::strtof(ev.val.c_str(), nullptr));
        else if (ev.key == "weather") setInitialWeatherType(ev.val);
        // label=<texto>[|<segundos>]: rotulo grande no rodape, para o video
        // dizer o que esta' mudando na tela (pedido do autor 2026-09-06:
        // "queria um overlay explicando qual clima que ta on"). Underscore
        // vira espaco (o roteiro e' separado por ';' e ':'), '~' vira ':'.
        else if (ev.key == "label") {
            std::string txt = ev.val;
            float dur = 4.0f;
            const size_t bar = txt.find('|');
            if (bar != std::string::npos) {
                dur = std::strtof(txt.c_str() + bar + 1, nullptr);
                txt = txt.substr(0, bar);
            }
            for (char& c : txt) { if (c == '_') c = ' '; else if (c == '~') c = ':'; }
            m_demoLabel = txt;
            m_demoLabelUntil = t + std::max(dur, 0.5f);
        }
        else if (ev.key == "baselod") m_demoForceBaseLod = on;
        // autoexp=0 congela a exposicao automatica. No video isso importa: ao
        // apagar as luzes de lava a cena escurece, a auto-exposicao ABRE e o
        // corte quase some da tela ("a iluminacao dinamica nem ta fazendo
        // diferenca", autor 2026-09-06). Travada, apagar a luz apaga a cena.
        else if (ev.key == "autoexp") m_autoExposureEnabled = on;
        else if (ev.key == "exit") m_running = false;
    }
}

void Engine::toggleWireframe() {
    m_vulkan.waitIdle();
    wireframeModeFlag() = !wireframeModeFlag();
    m_modelRenderer.recreatePipeline();
    m_terrainRenderer.recreatePipeline();
    ERUPTION_LOG_INFO("Wireframe %s (F8)", wireframeMode() ? "ligado" : "desligado");
}

} // namespace eruption
