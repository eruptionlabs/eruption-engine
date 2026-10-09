#pragma once

#include <array>
#include "core/Window.hpp"
#include "core/Timer.hpp"
#include <chrono>
#include <thread>
#include "renderer/VulkanContext.hpp"
#include "renderer/SpritePickerUI.hpp"
#include "renderer/GBuffer.hpp"
#include "utils/SplineCurve.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/DeferredLighting.hpp"
#include "renderer/ShadowRenderer.hpp"
#include "renderer/PostProcessor.hpp"
#include "renderer/UpscaleAA.hpp"
#include "renderer/CameraMotion.hpp"
#include "renderer/Fsr3Upscaler.hpp"
#include "renderer/SpriteLayer.hpp"
#include "renderer/SkyProbe.hpp"
#include "renderer/IrradianceProbes.hpp"
#include "renderer/WeatherSystem.hpp"
#include "renderer/skymap/SkySystem.hpp"
#include "renderer/skymap/SkyConfig.hpp"
#include "renderer/CloudLayerRenderer.hpp"
#include "renderer/LocalCloud.hpp"
#include "renderer/WaterRenderer.hpp"
#include "renderer/DebugLineRenderer.hpp"
#include "renderer/OverlayLineRenderer.hpp"
#include "renderer/SpriteSystem.hpp"
#include "renderer/TextureAtlas.hpp"
#include "renderer/ChunkManager.hpp"
#include "renderer/ParallaxSystem.hpp"
#include "renderer/RenderEffect.hpp"
#include "renderer/MipmapGenerator.hpp"
#include "debug/MipmapDebugMenu.hpp"
#include "debug/WaterDebugMenu.hpp"
#include "debug/WaterBenchmark.hpp"
#include "debug/SpherePreview.hpp"
#include "utils/DayNightCycle.hpp"
#include "utils/EmbeddedTextureBake.hpp"
#include "utils/SystemMonitor.hpp"
#include "renderer/DioramaLookConfig.hpp"
#include "math/Camera.hpp"
#include "formats/PackManager.hpp"
#include "formats/ResourceConfig.hpp"
#include "core/InputConfig.hpp"
#include "formats/SpriteTypes.hpp"
#include "core/ConfigManager.hpp"
#include "core/BackgroundMapLoader.hpp"
#include "core/Input.hpp"
#include "utils/Profiler.hpp"
#include "renderer/MapContext.hpp"
#include <vector>
#include <string>
#include <memory>
#include <deque>
#include <functional>
namespace eruption {
struct LoadedMap;
class MapLoader;
class PlayerController;
class Editor;
struct ScriptBridge;
class ScriptHost;
class Engine {
    friend class Editor;
    friend struct ScriptBridge;
    friend class ScriptHost;
public:
    Engine();
    ~Engine();
    bool init(int width, int height, const std::string& title);
    void shutdown();
    template<typename App>
    void run(App& app) {
        if (!m_initialized) return;
        app.onInit(*this);
        ERUPTION_LOG_INFO("[ENGINE] Entering main loop (running={}, shouldClose={})", m_running, m_window.shouldClose());
        while (m_running && !m_window.shouldClose()) {
            // Coarse per-phase CPU timing for the debug overlay (F3). The frame
            // marks inside render() only cover command recording, so without
            // these the overlay cannot tell "the sim is slow" from "the GPU is
            // slow" — the two look identical as a high frame time.
            auto _t0 = std::chrono::steady_clock::now();
            float dt = advanceFrame();
            auto _t1 = std::chrono::steady_clock::now();
            app.onUpdate(dt);
            auto _t2 = std::chrono::steady_clock::now();
            render();
            auto _t3 = std::chrono::steady_clock::now();
            app.onRender(*this);
            Input::update();
            Profiler::commitFrame();
            auto _t4 = std::chrono::steady_clock::now();
            using _ms = std::chrono::duration<float, std::milli>;
            m_cpuAdvanceMs  = _ms(_t1 - _t0).count();
            m_cpuAppUpdateMs = _ms(_t2 - _t1).count();
            m_cpuRenderMs   = _ms(_t3 - _t2).count();
            m_cpuTailMs     = _ms(_t4 - _t3).count();
            recordTelemetry();
            // PERIODO REAL DESTE LACO, medido no mesmo frame que as fases acima.
            // Antes o painel dividia as fases por m_frameTime, que e' uma MEDIA
            // de janela escrita la' no PlayerController (fpsElapsed/fpsFrames).
            // Numerador instantaneo sobre denominador medio dava porcentagem
            // acima de 100% quando um frame estourava a media - o autor viu
            // "render 110%" em 2026-09-03. Aqui as fases somam exatamente
            // _t4 - _t0, entao dividir por (fim do laco - _t0) nunca passa de
            // 100% e a conta fecha. O valor e' fechado depois do limitador de
            // FPS, senao o sono do limitador sumiria da conta.
            if (m_fpsCap > 0) {
                // Battery frame limiter: sleep to the cap period; when a frame
                // overruns, resync instead of bursting to catch up.
                using _clk = std::chrono::steady_clock;
                static _clk::time_point _next = _clk::now();
                _next += std::chrono::duration_cast<_clk::duration>(
                    std::chrono::duration<double>(1.0 / m_fpsCap));
                const auto _now = _clk::now();
                if (_next < _now) _next = _now;
                else std::this_thread::sleep_until(_next);
            }
            m_cpuFrameWallMs = _ms(std::chrono::steady_clock::now() - _t0).count();

            // CONTADOR DE FPS. Ficava dentro de PlayerController::updateUI, que
            // e' chamado por update(), que comeca com `if (dt <= 0.0f) return;`.
            // Resultado: todo frame com dt zero - pausa, carregamento, passo
            // travado - era DESENHADO mas nao CONTADO, enquanto o relogio de
            // parede seguia correndo. O FPS saia baixo na proporcao dos quadros
            // perdidos (o autor viu 22 FPS que nao batiam com a sensacao nem
            // com o tempo de GPU medido). Contar frame renderizado e' trabalho
            // do laco, nao da logica de jogo: aqui roda SEMPRE, uma vez por
            // iteracao, sem depender de nada a montante.
            {
                ++m_fpsFrames;
                const auto agora = std::chrono::steady_clock::now();
                const float decorrido = std::chrono::duration<float>(agora - m_fpsWindowStart).count();
                if (decorrido >= 0.5f) {
                    m_fps = static_cast<float>(m_fpsFrames) / decorrido;
                    m_frameTime = (decorrido / static_cast<float>(m_fpsFrames)) * 1000.0f;
                    m_fpsWindowStart = agora;
                    m_fpsFrames = 0;
                }
            }
        }
        app.onShutdown();
        m_vulkan.waitIdle();
    }
    void enableAutoExit(bool enable) { m_enableAutoExit = enable; }
    bool autoExitEnabled() const { return m_enableAutoExit; }
    void enableWaterBenchmark(bool enable) { m_enableWaterBenchmark = enable; }
    void setBenchmarkMap(const std::string& map) { m_benchmarkMap = map; }
    void setBenchmarkPosition(const Vec3& pos) { m_benchmarkPos = pos; }
    void setBenchmarkDuration(float sec) { m_benchmarkDuration = sec; }
    void enableBenchmarkThunderstorm(bool enable) { m_benchmarkThunderstorm = enable; }
    void enableBenchmarkHeavyRain(bool enable) { m_benchmarkHeavyRain = enable; }
    bool isRunning() const { return m_running; }
    bool loadMap(const std::string& name);
    const std::string& currentMapName() const { return m_currentMapName; }
    float sampleTerrainHeight(float x, float z) const;
    // CLI/test hooks: spawn independent clouds (altitudes) at the player after
    // map load, tint clouds by altitude for automated tests, debug auto-spawn.
    struct PendingCloudSpawn {
        float altitude;
        float density = -1.0f; // < 0: use the current Local Density slider value
        float jitterX = 0.0f;  // XZ offset from the player so stacked clouds spread out
        float jitterZ = 0.0f;
        bool hasRain = false;  // cloud carries its own rain that follows it
        float radiusScale = 1.0f; // baked blob size multiplier (weather field)
        bool fromField = false;   // spawned by the weather field (auto upwind respawn)
    };
    void setPendingCloudSpawns(const std::vector<float>& alts, bool hasRain = false) {
        m_pendingCloudSpawns.clear();
        for (float a : alts) m_pendingCloudSpawns.push_back({a, -1.0f, 0.0f, 0.0f, hasRain, 1.0f, false});
    }
    void setCloudTestColors(bool on) { m_cloudTestColors = on; }
    // Test hook: override the local cloud spawn params (mimics the UI sliders).
    void setLocalCloudParams(float radiusX, float radiusZ, float falloff) {
        m_localCloudRadiusX = radiusX;
        m_localCloudRadiusZ = radiusZ;
        m_localCloudFalloff = falloff;
    }
    // Test hook: force the weather type by name (e.g. "stormy"); the cloud
    // field respawns for it on the next update tick.
    void setInitialWeatherType(const std::string& name);
    // Full weather-type application: syncs WeatherSystem params AND the
    // CloudLayerRenderer look (scale/lightness/shade/softness/thickness/wind)
    // from the type's template, with the same coverage transition the UI uses.
    // Both the --weather CLI flag and the Weather tab combo/category buttons
    // must go through this — applying only WeatherSystem::applyType() (as the
    // CLI path used to) leaves the cloud layer at its default look (thin/wrong
    // shade), which reads as a broken/mismatched fog layer on load.
    void applyWeatherTypeFull(WeatherType type, float intensity);
    void setAutoCloud(bool on) { m_autoCloud = on; }
    // Test-only override of the cloud box for independent clouds (does NOT
    // touch the renderer config, so it is never persisted to the json).
    void setCloudBoxOverride(float bottom, float thickness) {
        m_cloudBoxOverride = true;
        m_cloudBoxBottom = bottom;
        m_cloudBoxThickness = thickness;
    }
    // Test hook: force the cloud wind speed (e.g. 0 for deterministic runs).
    void setCloudWindOverride(float speed) { m_cloudWindOverride = speed; }
    void setScreenshotOnLoad(const std::string& filename) { m_screenshotOnLoad = filename; }
    // Test hook: capture a screenshot on the next completed map swap instead of
    // on the initial load. Used to validate warp behavior (e.g. PBR textures).
    void setScreenshotAfterWarp(const std::string& filename) {
        m_screenshotAfterWarpPath = filename;
        m_screenshotAfterWarpPending = true;
    }
    void setInitialPosition(const Vec3& pos) { m_initialPos = pos; m_hasInitialPos = true; }
    void setInitialCamera(float yawDeg, float pitchDeg, float distance) {
        m_initialYaw = glm::radians(yawDeg);
        m_initialPitch = glm::radians(pitchDeg);
        m_initialDist = distance;
        m_hasInitialCamera = true;
        // Allow very large camera distances so distant cloud planes remain visible.
        if (distance > m_camera.maxOrbitDistance()) {
            m_camera.setMaxOrbitDistance(distance);
        }
        if (distance > m_camera.farPlane() * 0.5f) {
            m_camera.setFarPlane(distance * 2.0f);
        }
    }
    void setPlayerController(PlayerController* pc);
    bool isPlayerSpawnedOnActiveMap() const { return m_playerSpawnedOnActiveMap; }
    struct TextureResource {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t slot = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool isIndexed = false;
        uint32_t mipLevels = 1;
        // Content bounds after erosion/dilation (for class sprites with ghost pixels).
        int contentMinX = -1, contentMinY = -1, contentMaxX = -1, contentMaxY = -1;
    };
    struct LoadedSprite {
        AnimFile act;
        std::vector<TextureResource> textures;
        uint32_t indexedTextureCount = 0;
        bool loaded = false;
        bool isPngSprite = false;
    };
    struct TextureStats {
        size_t totalTextures = 0;
        size_t texturesWithMipmaps = 0;
        uint64_t totalVRAMBytes = 0;
        uint64_t baseVRAMBytes = 0;
        size_t modelTexCount = 0;
        size_t terrainTexCount = 0;
        size_t spriteTexCount = 0;
    };
    TextureStats getTextureStats() const;
    Camera& camera() { return m_camera; }
    DeferredLighting& deferredLighting() { return m_deferredLighting; }

    // Editor (src/editor/). Com o editor ligado a cena e' desenhada num alvo
    // proprio, do tamanho do painel Cena, e o swapchain recebe so' a UI.
    // setEditorMode vale antes de init() (fonte e janela da UI).
    void setEditorMode(bool on) { m_editorMode = on; }
    bool editorMode() const { return m_editorMode; }
    void setEditor(Editor* editor) { m_editor = editor; }
    // Desenho por cima do jogo (sem editor), dentro do passe de ImGui.
    void setGameOverlay(std::function<void()> fn) { m_gameOverlay = std::move(fn); }
    // Tamanho da imagem final da cena: o painel Cena no editor, a janela no jogo.
    VkExtent2D displayExtent() const {
        return (m_editor && m_viewportW && m_viewportH) ? VkExtent2D{m_viewportW, m_viewportH}
                                                        : m_vulkan.swapExtent();
    }
    // Novo tamanho do painel Cena, aplicado no comeco do proximo frame.
    void setViewportSize(uint32_t w, uint32_t h);
    // Descritor da imagem da cena para ImGui::Image (VK_NULL_HANDLE sem editor).
    VkDescriptorSet viewportTexture() const { return m_viewportTexture; }
    InputConfig& inputConfig() { return m_inputConfig; }
    CharConfig& charConfig() { return m_charConfig; }
    auto* currentMap() const { return m_currentMap.get(); }
    const std::vector<std::string>& availableMaps() const { return m_availableMaps; }
    void setPendingMapWarp(const std::string& map) { m_pendingMapWarp = map; }
    WaterRenderer& water() { return m_water; }
    WaterDebugMenu& waterMenu() { return m_waterMenu; }
    float mapBaseWaterLevel() const { return m_mapBaseWaterLevel; }
    SpriteRenderer& spriteRenderer() { return m_spriteRenderer; }
    bool& shimmerMeasureEnabled() { return m_shimmerMeasureEnabled; }
    const std::vector<float>& getShimmerHistory() const { return m_shimmerHistory; }
    float getShimmerCurrentValue() const { return m_shimmerCurrentValue; }
    BindlessDescriptor& bindless() { return m_bindless; }
    PostProcessor::PostSettings& postSettings() { return m_postSettings; }
    PostProcessor& postProcessor() { return m_postProcessor; }
    WeatherSystem& weatherSystem() { return m_weatherSystem; }
    ShadowRenderer& shadowRenderer() { return m_shadowRenderer; }
    ConfigManager& shadowConfig() { return m_shadowConfig; }
    DayNightCycle& dayNightCycle() { return m_dayNightCycle; }
    // Toggles nightOnly point lights (lamp/lantern props) on/off by hour.
    void updateNightLights();
    ConfigManager& dayNightConfig() { return m_dayNightConfig; }
    ConfigManager& postConfig() { return m_postConfig; }
    ConfigManager& graphicsConfig() { return m_graphicsConfig; }
    // Applies the active preset from data/graphics.json (weather tier, shadow
    // atlas, SSAO, post toggles, cloud/rain budgets). Only keys present in
    // the preset are applied; safe to call at init and on F5 hot-reload.
    void applyGraphicsPreset();
    DioramaLookConfig& lookConfig() { return m_lookConfig; }
    ConfigManager& dioramaConfig() { return m_dioramaConfig; }
    ConfigManager& skyboxConfig() { return m_skyboxConfig; }
    SkySystem& skyboxSystem() { return m_skybox; }
    VkSampler defaultSampler() const { return m_defaultSampler; }
    VulkanContext& vulkan() { return m_vulkan; }
    ModelRenderer& modelRenderer() { return m_modelRenderer; }
    TerrainRenderer& terrainRenderer() { return m_terrainRenderer; }
    SpriteSystem& spriteSystem() { return m_spriteSystem; }
    DebugLineRenderer& debugLineRenderer() { return m_debugLineRenderer; }
    OverlayLineRenderer& overlayLineRenderer() { return m_overlayLineRenderer; }
    bool& debugModelPivots() { return m_debugModelPivots; }
    PackManager& packManager() { return m_packManager; }
    float& fps() { return m_fps; }
    float& frameTime() { return m_frameTime; }
    int& visibleChunks() { return m_visibleChunks; }
    // AABB horizontal da malha de agua (Y vem do waterLevel no uso). Serve
    // para cortar o passe de agua pelo frustum da CAMERA no chamador - ver
    // Engine::render. Seguro: a agua nao entra no shadow map.
    void cacheWaterBounds(const struct WaterMesh& mesh);
    Timer& timer() { return m_timer; }
    bool& showConsole() { return m_showConsole; }
    bool& showDebugOverlay() { return m_showDebugOverlay; }
    bool& showStatsMenu() { return m_showStatsMenu; }
    bool& showSpritePicker() { return m_showSpritePicker; }
    bool& showResourceManager() { return m_showResourceManager; }
    bool& showObjectMenu() { return m_showObjectMenu; }
    bool& showEffectsMenu() { return m_showEffectsMenu; }
    bool& showABTestMenu() { return m_showABTestMenu; }
    // Esfera de teste (F1 -> Object Manager -> Textures, clicar num item da
    // lista - ver debug/SpherePreview.hpp). Popup INSTANTANEO: renderiza num
    // alvo offscreen proprio, nunca entra no mapa/instancia nenhuma, nao
    // recarrega nada (a 1a versao recarregava o mapa pra' injetar a esfera
    // como objeto do mundo - autor: "a esfera tem que aparecer num pop do
    // mapa" / "atualmente eu clico e ele muda a textura do chao (errado)").
    SpherePreview& spherePreview() { return m_spherePreview; }
    bool& showPlayerLightMenu() { return m_showPlayerLightMenu; }
    // Luz de teste presa ao sprite principal (F10): pra' ver a interacao da
    // point light com o mundo (PBR, PCSS, molhado) sem depender de prop com
    // luz autoral por perto. Nao e' feature de jogo, e' bancada.
    struct PlayerLight {
        bool enabled = false;
        Vec3 color = Vec3(1.0f, 0.85f, 0.6f);
        float intensity = 6.0f;
        float radius = 220.0f;
        float heightFrac = 0.6f; // fracao da altura do sprite (0.6 = peito)
    };
    PlayerLight& playerLight() { return m_playerLight; }
    // Alterna o modo wireframe estilizado em runtime (F8). Espera a GPU
    // ficar ociosa e recria as pipelines de modelo/terreno com a nova
    // polygon mode - ver renderer/GBuffer.hpp:wireframeMode().
    void toggleWireframe();
    // ERUPTION_TEST_DEMO (bancada de VIDEO): roteiro por tempo numa execucao
    // so' - ver Render.cpp. m_demoForceBaseLod e' OR'd com o env FORCE_BASE_LOD.
    void runDemoTimeline(const char* spec);
    bool m_demoForceBaseLod = false;
    float m_demoStartTime = -1.0f;
    bool m_demoPinStats = false;   // painel F3 fixo no canto direito (video)
    bool m_demoHidePanels = false; // esconde Shortcuts/Control Panel/minimapa (video)
    std::string m_demoLabel;       // rotulo grande no rodape (ver runDemoTimeline: label=)
    float m_demoLabelUntil = -1.0f;// segundos do roteiro ate' quando o rotulo fica
    RenderEffect* renderEffect(size_t i) { return m_renderEffects[i].get(); }
    void setMinimapBottomRight(bool bottomRight) { m_minimapBottomRight = bottomRight; }
    bool minimapBottomRight() const { return m_minimapBottomRight; }
    uint32_t createTextureFromSprImage(const SprImage& img, const uint8_t* palette, bool usePalette, std::vector<TextureResource>& outResources, bool swapRB = false, const std::string& sourcePath = "");
    bool loadSpriteFromPng(const std::string& path, LoadedSprite& out);
    bool loadSpriteFromEruptSpr(const std::string& path, LoadedSprite& out);
    void cleanupSpriteTextures(LoadedSprite& ls);
    void calculateMaxCharHeight();
    void renderSpritePart(LoadedSprite& ls, uint32_t paletteSlot, int& frameIdx, float& timer, float dt, int& outCount, bool isBody = false);
    float computeObstruction() const;
    float computeObstructionUncached() const;
private:
    void generateMipmaps(VkCommandBuffer cmd, VkImage image, int w, int h, uint32_t mipLevels, VkFormat format, uint32_t alphaMode);
    float advanceFrame();
    void populateMaps();
    void render();
    void renderImGui();
    void drawLightingControls();
    uint32_t resolveModelTexture(const std::string& path);
    uint32_t resolveTerrainTexture(const std::string& path);
    ModelMeshGPU resolveModelMesh(const std::string& legacyModelPath, uint32_t nodeIndex,
                                  const std::vector<TerrainVertex>& vertices,
                                  const std::vector<uint32_t>& indices);
    void initImGui();
    void shutdownImGui();
    void onWindowResize(int width, int height);
    void renderMinimap(VkCommandBuffer cmd);
    void setupLavaLiquid(const LoadedMap* map);
    // Preenche m_waterMenu.liquids com o que o mapa de fato autorou (nome,
    // tipo) - so' roda em MAP LOAD, nao em toda rebuild de malha, senao
    // qualquer nudge de posicao/rotacao feito na janela seria zerado no
    // frame seguinte junto com ela.
    void syncLiquidMenuFromMap(const LoadedMap* map);
    void initMinimapComposite();
    void dumpMinimap(const std::string& filename);
    void loadMapClimate(const std::string& mapName);
    void initMinimap();
    void shutdownMinimap();
    void initSplashLogo();
    void shutdownSplashLogo();
    void renderLoadingUI(const std::string& mapName, const std::string& task, float progress);
    void presentLoadingScreen(const std::string& mapName, const std::string& task, float progress, bool clear = false);
    void renderResourceManager();
public:
    bool createTextureFromFile(const char* path, TextureResource& out);
    bool createTextureFromPixels(const unsigned char* pixels, int w, int h, TextureResource& out);
private:
    // Decodes a GLB-embedded albedo by name so PBR synthesis can run on
    // textures that have no file on disk. Returns false when not found.
    bool findEmbeddedAlbedoPixels(const std::string& name, std::vector<uint8_t>& outPixels,
                                  int& outW, int& outH, int& outChannels);
    void recordTelemetry();
    void processBackgroundLoading();
    void uploadStagingMapGPU(VkCommandBuffer cmd);
    void performMapSwap();
    void renderLoadingProgress();
    void takeScreenshot(const std::string& filename);
    void readPostOutput(std::vector<float>& rgb, uint32_t& w, uint32_t& h);
    void takeScreenshotLit(const std::string& filename);
    void scheduleScreenshotFromSwap(const std::string& filename);
    void finishPendingScreenshot();
    float raycastScene(const Vec3& rayOrigin, const Vec3& rayDir, float maxDist, const std::vector<struct VisibleObject>& objects) const;
    float computeObstructionSingleRay() const;
    float computeObstructionMultiRay() const;
    float computeObstructionScreenSpace() const;
    float computeObstructionAngular() const;
    void drawWeatherTab();
    Mat4 computeCloudShadowMatrix(const Vec3& lightDir, const Vec3& target) const;
    Window m_window;
    VulkanContext m_vulkan;
    Timer m_timer;
    BindlessDescriptor m_bindless;
    GBuffer m_gbuffer;
    TerrainRenderer m_terrainRenderer;
    ModelRenderer m_modelRenderer;
    SpriteRenderer m_spriteRenderer;
    ShadowRenderer m_shadowRenderer;
    DeferredLighting m_deferredLighting;
    PostProcessor m_postProcessor;
    // FXAA + upscale bicubico/nitidez, no lugar do blit bilinear cru que
    // levava a imagem de RENDER pro swapchain de DISPLAY. Ver UpscaleAA.hpp.
    UpscaleAA m_upscaleAA;
    // Motion vectors de camera (resolucao de render), entrada do FSR.
    CameraMotion m_cameraMotion;
    // UPSCALER. Fxaa = caminho antigo (FXAA + Catmull-Rom). Com FSR o jitter
    // liga sozinho e o UpscaleAA vira so' a copia final pro swapchain:
    //   FsrBeforePost: FSR na imagem HDR linear (antes de DoF/bloom/tonemap);
    //                  o pos passa a rodar na resolucao de DISPLAY.
    //   FsrAfterPost : FSR na saida do pos (ja' tonemapeada); o pos fica na
    //                  resolucao de render - mais barato, mas DoF, motion blur
    //                  e particulas entram no historico.
    // ERUPTION_UPSCALER=fxaa|fsr|fsr_post (ou "upscaler" no preset).
    enum class UpscalerMode { Fxaa, FsrBeforePost, FsrAfterPost };
    UpscalerMode m_upscalerMode = UpscalerMode::Fxaa;
    Fsr3Upscaler m_fsr;
    // Sprites em camada sobre o FSR (pixel art nitido, sem rastro). A mascara
    // reativa vale nos dois modos; o redesenho em resolucao de display so'
    // no FsrBeforePost (depois do pos a imagem ja' esta' tonemapeada).
    // ERUPTION_SPRITE_LAYER=0 desliga (A/B).
    SpriteLayer m_spriteLayer;
    bool m_spriteLayerActive = false;
    bool m_fsrReset = true;
    Vec4 m_prevWindParams = Vec4(0.0f);
    bool m_hasPrevWindParams = false;
    float m_fsrSharpness = 0.0f;   // 0 = sem RCAS
    bool fsrActive() const { return m_upscalerMode != UpscalerMode::Fxaa; }
    void bindFsrInputs();
    bool m_enableFXAA = true;
    float m_aaSharpenAmount = 0.35f;
    SkySystem m_skybox;
    // SONDA DE CEU (G36): cubemap do skybox.frag -> SH9 (difuso) + mips
    // (especular). Preset "sky_probe" / "sky_probe_intensity";
    // ERUPTION_TEST_NO_SKY_PROBE=1 desliga (A/B).
    SkyProbe m_skyProbe;
    bool m_skyProbeEnabled = true;
    float m_skyProbeIntensity = 1.0f;
    // PROBES DE IRRADIANCIA (G38): visibilidade do ceu por probe, bakeada ao
    // carregar o mapa. Preset "irradiance_probes" / "irradiance_probe_strength";
    // ERUPTION_TEST_NO_PROBES=1 desliga (A/B).
    IrradianceProbes m_irradianceProbes;
    bool m_irradianceProbesEnabled = true;
    float m_irradianceProbeStrength = 1.0f;
    CloudLayerRenderer m_cloudLayerRenderer;
    WeatherSystem m_weatherSystem;
    WaterRenderer m_water;
    DebugLineRenderer m_debugLineRenderer;
    OverlayLineRenderer m_overlayLineRenderer;
    SpriteSystem m_spriteSystem;
    TextureAtlas m_textureAtlas;
    ChunkManager m_chunkManager;
    ParallaxSystem m_parallaxSystem;
    DayNightCycle m_dayNightCycle;
    DioramaLookConfig m_lookConfig;
    SpritePickerUI m_spritePickerUI;
    Camera m_camera;
    Mat4 m_prevViewProjNoJitter = Mat4(1.0f);
    // Jitter sub-pixel da rasterizacao (base do FSR). DESLIGADO por padrao:
    // sem acumulador temporal o jitter so' ADICIONA cintilacao.
    // ERUPTION_JITTER=1 liga (teste). Ver updateTemporalJitter() em Render.cpp.
    bool m_temporalJitter = false;
    uint32_t m_jitterIndex = 0;
    Vec2 m_jitterPx = Vec2(0.0f);     // pixels de render, [-0.5, 0.5), y para baixo
    Vec2 m_prevJitterPx = Vec2(0.0f);
    void updateTemporalJitter();
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    // Samplers padrao SEM vies de mip, guardados quando o FSR troca os de cena
    // (a UI criada antes continua apontando para eles).
    VkSampler m_uiDefaultSampler = VK_NULL_HANDLE;
    VkSampler m_uiNearestSampler = VK_NULL_HANDLE;
    VkDescriptorPool m_imguiPool = VK_NULL_HANDLE;
    std::vector<std::string> m_availableMaps;
    std::string m_currentMapName;
    PackManager m_packManager;
    std::unique_ptr<MapLoader> m_mapLoader;
    std::shared_ptr<LoadedMap> m_currentMap;
    float m_centerX = 0.0f;
    float m_centerZ = 0.0f;
    float m_fps = 0.0f;
    float m_frameTime = 0.0f;
    // Periodo do laco medido NO MESMO frame que as fases de CPU (ver run()).
    // m_frameTime e' media de janela e nao serve de denominador para elas.
    float m_cpuFrameWallMs = 0.0f;
    // Janela do contador de FPS (ver run()). Fica aqui, no laco, porque um
    // frame renderizado tem que ser contado mesmo quando a logica de jogo nao
    // roda naquele frame.
    std::chrono::steady_clock::time_point m_fpsWindowStart = std::chrono::steady_clock::now();
    int m_fpsFrames = 0;
    int m_visibleChunks = 0;
    Vec3 m_waterAabbMin = Vec3(0.0f);
    Vec3 m_waterAabbMax = Vec3(0.0f);
    bool m_waterBoundsValid = false;
    // true quando o nivel da agua esta' abaixo de TODO o terreno (plano
    // global soterrado, caso de cidade-A) - a agua nunca pode ser vista.
    bool m_waterFullyBuried = false;
    // Escala do balanco da vegetacao (graphics.json wind_sway_scale).
    float m_windSwayScale = 0.15f;
    bool m_showDebugOverlay = true;
    bool m_showEffectsMenu = false; 
    bool m_showObjectMenu = false;
    bool m_showStatsMenu = false;
    bool m_showPostProcessMenu = false;
    bool m_showSpritePicker = false; 
    bool m_showResourceManager = false; 
    bool m_showABTestMenu = false;
    SpherePreview m_spherePreview;
    bool m_showPlayerLightMenu = false;
    PlayerLight m_playerLight;
    bool m_debugModelPivots = false;
    bool m_showConsole = false;
    bool m_cloudLayersEnabled = true;
    bool m_showCloudCoverageDebug = false;

    // Smooth cloud amount transition when weather type changes.
    float m_cloudAmountTransitionStart = 0.0f;
    float m_cloudAmountTransitionEnd = 0.0f;
    bool m_cloudAmountInTransition = false;

    // Local cloud spawn parameters (used by the debug UI).
    float m_localCloudRadiusX = 60.0f;
    float m_localCloudRadiusZ = 40.0f;
    float m_localCloudDensity = 0.8f;
    float m_localCloudFalloff = 1.0f;
    float m_localCloudAltitude = 0.5f;
    float m_localCloudRotation = 0.0f;
    float m_localCloudCoverage = 0.5f;
    uint32_t m_localCloudLayerMask = 1;
    bool m_localCloudRain = true; // spawned cloud carries rain that follows its shadow

    // Independent clouds: each spawned cloud lives in its OWN coverage layer
    // (same class as the global layer, isolated, no tiling). Clouds with the
    // same altitude share one layer; different altitudes get separate layers.
    struct IndependentCloudLayer {
        std::unique_ptr<CloudCoverageArray> array;
        float altitude = 0.5f;      // slider value that created the layer
        float planeY = 0.0f;        // absolute world Y, fixed at spawn time
        Vec2 centerXZ = Vec2(0.0f); // XZ of the first cloud in this layer
        float density = 0.8f;       // local cloud density at spawn
        float weight = 1.0f;        // density + occupied area + rain darkness; slows wind
        bool hasRain = false;       // cloud carries its own rain that follows it
        bool fromField = false;     // weather-field cloud: respawns upwind when it leaves the map
        uint32_t rendererId = UINT32_MAX;
    };
    std::vector<IndependentCloudLayer> m_independentCloudLayers;
    // Last frame's painter order (rendererIds, back-to-front). Kept across
    // frames so the distance sort can apply hysteresis (see the render loop).
    std::vector<uint32_t> m_cloudLayerDrawOrder;
    // Per-frame scratch for the painter-order pass (reused to avoid heap
    // churn and the old O(n^2) id lookups with ~100 layers).
    struct CloudOrderEntry { float dist; const IndependentCloudLayer* layer; bool inOrder; };
    std::unordered_map<uint32_t, CloudOrderEntry> m_cloudOrderScratch;
    std::vector<const IndependentCloudLayer*> m_cloudSortScratch;
    std::vector<uint32_t> m_cloudOrderIds;
    uint32_t m_independentCloudCount = 0; // total clouds across all layers (max MAX_CLOUD_LAYERS)
    // Spawns an independent cloud at the player position (plus an optional XZ
    // jitter) with the given altitude. Always adds; never replaces existing
    // clouds. density < 0 uses the current Local Density slider value.
    // radiusScale multiplies the baked blob size (used by the weather field).
    // fromField marks weather-field clouds (they respawn upwind off the map).
    void spawnIndependentCloud(float altitude, float density = -1.0f,
                               Vec2 jitterXZ = Vec2(0.0f), bool hasRain = false,
                               float radiusScale = 1.0f, bool fromField = false);
    void clearIndependentClouds();
    float independentCloudPlaneY(float altitude) const;

    // Procedural cloud field: each weather type spawns its own set of
    // independent clouds (varying count/size/density/rain) instead of
    // rendering the global cloud layer. The global layer code stays intact
    // but dormant while this flag is on (default).
    bool m_useProceduralCloudField = true;
    int m_weatherFieldType = -1; // WeatherType the field was last spawned for
    void spawnWeatherCloudField(WeatherType type);
    void setUseProceduralCloudField(bool on) { m_useProceduralCloudField = on; m_weatherFieldType = -1; }

    std::vector<PendingCloudSpawn> m_pendingCloudSpawns;
    bool m_cloudTestColors = false;
    bool m_autoCloud = true;
    bool m_cloudBoxOverride = false;
    float m_cloudBoxBottom = 5000.0f;
    float m_cloudBoxThickness = 640.0f;
    float m_cloudWindOverride = -1.0f; // >= 0: overrides cfg.windSpeed
    float m_cloudDebugLogTimer = 0.0f; // periodic position/UV log accumulator


    bool m_shortcutsExpanded = true;
    char m_consoleInput[256] = {0};
    int m_selectedLightIndex = -1;
    // Selecionado na lista "Models" do Object Manager (F1) - pedido do autor
    // pra ter setinhas de gizmo (estilo editor 3D) em cima de
    // QUALQUER objeto da cena, nao so' a lava. -1 = nada selecionado.
    int m_selectedModelInstance = -1;
    PostProcessor::PostSettings m_postSettings;
    int m_lookPreset = 2;
    bool m_showMapSelector = false;
    bool m_enableAutoExit = false;
    bool m_enableWaterBenchmark = false;
    bool m_benchmarkThunderstorm = false;
    bool m_weatherAffectsWater = true;
    std::string m_benchmarkMap = "parana_field";
    Vec3 m_benchmarkPos = Vec3(1220.0f, 0.0f, 270.0f);
    float m_benchmarkDuration = 60.0f;
    bool m_benchmarkThunderstormStarted = false;
    float m_benchmarkThunderstormTimer = 0.0f;
    float m_benchmarkThunderstormZoomPhase = 0.0f;
    int m_benchmarkThunderstormFrames = 0;
    float m_benchmarkThunderstormFpsSum = 0.0f;
    float m_benchmarkThunderstormFpsMin = 9999.0f;
    float m_benchmarkThunderstormStable60Time = 0.0f;
    // Heavy-rain benchmark (--benchmark-heavy-rain): Thunderstorm weather with
    // the user's real render settings (High tier, per-cloud rain, lightning).
    // Phases: warmup (cloud field drains) -> 5s idle -> 5s camera orbit.
    bool m_benchmarkHeavyRain = false;
    int m_benchmarkHeavyRainPhase = 0; // 0=warmup, 1=idle, 2=orbit
    float m_benchmarkHeavyRainTimer = 0.0f;   // wall time in current phase
    float m_benchmarkHeavyRainWarmup = 0.0f;  // grace time after spawns drain
    float m_benchmarkHeavyRainOrbitYaw = 0.0f;
    int m_benchmarkHeavyRainFrames[2] = {0, 0};
    float m_benchmarkHeavyRainFpsSum[2] = {0.0f, 0.0f};
    float m_benchmarkHeavyRainFpsMin[2] = {9999.0f, 9999.0f};
    // ERUPTION_TEST_BENCH_NO_HUD=1: hide the HUD during the benchmark (clean frames
    // for automated screenshot diffs; the FPS spec is measured with HUD on).
    bool m_benchmarkHideHud = false;
    // Frames to exclude from benchmark stats (the takeScreenshot readback +
    // PNG encode stalls the next frame by ~450ms and would poison min/spikes).
    int m_benchmarkSkipStatFrames = 0;
    bool m_benchmarkCloseShot = false;
    bool m_benchmarkCloseShotTaken = false;
    // ERUPTION_TEST_BENCH_SHOTS=<dir>: save <weather>_idle.png at the end of the
    // idle phase and <weather>_dyn.png at the end of the dynamic phase.
    std::string m_benchmarkShotsDir;
    std::string m_benchmarkWeatherName = "thunderstorm";
    static constexpr float BENCHMARK_HEAVY_RAIN_PHASE_SEC = 5.0f;
    static constexpr float BENCHMARK_HEAVY_RAIN_WARMUP_SEC = 2.0f;
    void logBenchmarkBreakdown();
    std::chrono::steady_clock::time_point m_benchmarkCpuFrameStart;
    std::array<double, 16> m_benchmarkCpuAccum{};
    std::array<double, 16> m_benchmarkCpuFrameMs{}; // per-frame section times (spike diagnosis)
    int m_benchmarkSpikeCount[2] = {0, 0};          // frames with dt > 16.7ms per phase
    int m_benchmarkSpikeLogs = 0;
    int m_benchmarkCpuSamples = 0;
    bool m_hasInitialPos = false;
    Vec3 m_initialPos = Vec3(0.0f);
    bool m_hasInitialCamera = false;
    float m_initialYaw = 0.0f;
    float m_initialPitch = 0.0f;
    float m_initialDist = 158.5f;
    std::string m_screenshotOnLoad;
    std::string m_screenshotAfterWarpPath;
    bool m_screenshotAfterWarpPending = false;
    int m_framesCount = 0;
    int m_framesAfterSwap = -1;
    // UI-inclusive screenshot capture state.
    bool m_screenshotPending = false;
    uint32_t m_screenshotSubmitFrame = 0;
    VkBuffer m_screenshotStaging = VK_NULL_HANDLE;
    VmaAllocation m_screenshotStagingAlloc = VK_NULL_HANDLE;
    uint32_t m_screenshotStagingW = 0;
    uint32_t m_screenshotStagingH = 0;
    std::string m_screenshotPendingPath;
    // A/B capture state: 0=idle, 1=settling PBR on, 2=capture PBR on,
    // 3=settling PBR off, 4=capture PBR off.
    int m_abCaptureStep = 0;
    int m_abCaptureFrame = 0;
    std::unique_ptr<BackgroundMapLoader> m_backgroundLoader;
    std::unique_ptr<MapContext> m_activeMapContext;
    std::unique_ptr<MapContext> m_stagingMapContext;
    struct GlobalAssetCache {
        mutable std::mutex mutex;
        struct CacheEntry {
            ModelTextureGPU tex;
            uint32_t lastSeenWarpId;
        };
        struct TerrainCacheEntry {
            TerrainTextureGPU tex;
            uint32_t lastSeenWarpId;
        };
        struct MeshCacheEntry {
            ModelMeshGPU mesh;
            uint32_t lastSeenWarpId;
        };
        std::unordered_map<std::string, CacheEntry> modelTextures;
        std::unordered_map<std::string, TerrainCacheEntry> terrainTextures;
        std::unordered_map<std::string, MeshCacheEntry> modelMeshes;
        uint32_t currentWarpId = 0;
        void incrementWarp() {
            std::lock_guard<std::mutex> lock(mutex);
            currentWarpId++;
        }
        bool hasModelTexture(const std::string& path) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = modelTextures.find(path);
            if (it != modelTextures.end()) {
                it->second.lastSeenWarpId = currentWarpId;
                return true;
            }
            return false;
        }
        bool getModelTexture(const std::string& path, ModelTextureGPU& outTex) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = modelTextures.find(path);
            if (it != modelTextures.end()) {
                it->second.lastSeenWarpId = currentWarpId;
                outTex = it->second.tex;
                return true;
            }
            return false;
        }
        void addModelTexture(const std::string& path, const ModelTextureGPU& tex) {
            std::lock_guard<std::mutex> lock(mutex);
            modelTextures[path] = {tex, currentWarpId};
        }
        bool hasTerrainTexture(const std::string& path) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = terrainTextures.find(path);
            if (it != terrainTextures.end()) {
                it->second.lastSeenWarpId = currentWarpId;
                return true;
            }
            return false;
        }
        bool getTerrainTexture(const std::string& path, TerrainTextureGPU& outTex) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = terrainTextures.find(path);
            if (it != terrainTextures.end()) {
                it->second.lastSeenWarpId = currentWarpId;
                outTex = it->second.tex;
                return true;
            }
            return false;
        }
        void addTerrainTexture(const std::string& path, const TerrainTextureGPU& tex) {
            std::lock_guard<std::mutex> lock(mutex);
            terrainTextures[path] = {tex, currentWarpId};
        }
        bool getModelMesh(const std::string& key, ModelMeshGPU& outMesh) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = modelMeshes.find(key);
            if (it != modelMeshes.end()) {
                it->second.lastSeenWarpId = currentWarpId;
                outMesh = it->second.mesh;
                return true;
            }
            return false;
        }
        void addModelMesh(const std::string& key, const ModelMeshGPU& mesh) {
            std::lock_guard<std::mutex> lock(mutex);
            modelMeshes[key] = {mesh, currentWarpId};
        }
        struct Stats {
            size_t modelTextureCount = 0;
            size_t terrainTextureCount = 0;
            size_t meshCount = 0;
        };
        Stats getStats() const {
            std::lock_guard<std::mutex> lock(mutex);
            return {modelTextures.size(), terrainTextures.size(), modelMeshes.size()};
        }
        void cleanupOldTextures(VulkanContext* ctx, BindlessDescriptor* bindless, uint32_t maxAge = 3);
        void shutdown(VulkanContext* ctx, BindlessDescriptor* bindless) {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& pair : modelTextures) {
                destroyTexture(pair.second.tex, ctx, bindless);
            }
            modelTextures.clear();
            for (auto& pair : terrainTextures) {
                destroyTexture(pair.second.tex, ctx, bindless);
            }
            terrainTextures.clear();
            // UMA lista de buffers, a da propria malha. A copia a mao que
            // vivia aqui tinha 4 dos 9 (faltavam lo/far/sh, adicionados
            // depois) e vazava os de LOD e sombra no fechamento - 48 index
            // buffers vivos, VMA abortando em build de debug. Quem adicionar
            // buffer novo em ModelMeshGPU so' precisa mexer em destroyBuffers.
            for (auto& pair : modelMeshes) pair.second.mesh.destroyBuffers(ctx);
            modelMeshes.clear();
        }

    private:
        static void destroyTexture(ModelTextureGPU& tex, VulkanContext* ctx, BindlessDescriptor* bindless) {
            if (tex.bindlessSlot != 0 && bindless) bindless->freeSlotSafe(tex.bindlessSlot);
            if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(ctx->device(), tex.view, nullptr);
            if (tex.image != VK_NULL_HANDLE) vmaDestroyImage(ctx->allocator(), tex.image, tex.alloc);
        }
        static void destroyTexture(TerrainTextureGPU& tex, VulkanContext* ctx, BindlessDescriptor* bindless) {
            if (tex.bindlessSlot != 0 && bindless) bindless->freeSlotSafe(tex.bindlessSlot);
            if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(ctx->device(), tex.view, nullptr);
            if (tex.image != VK_NULL_HANDLE) vmaDestroyImage(ctx->allocator(), tex.image, tex.alloc);
            if (tex.pbrSlot != 0 && bindless) bindless->freeSlotSafe(tex.pbrSlot);
            if (tex.pbrView != VK_NULL_HANDLE) vkDestroyImageView(ctx->device(), tex.pbrView, nullptr);
            if (tex.pbrImage != VK_NULL_HANDLE) vmaDestroyImage(ctx->allocator(), tex.pbrImage, tex.pbrAlloc);
            if (tex.normalSlot != 0 && bindless) bindless->freeSlotSafe(tex.normalSlot);
            if (tex.normalView != VK_NULL_HANDLE) vkDestroyImageView(ctx->device(), tex.normalView, nullptr);
            if (tex.normalImage != VK_NULL_HANDLE) vmaDestroyImage(ctx->allocator(), tex.normalImage, tex.normalAlloc);
        }
    } m_assetCache;
    struct DeferredMapDelete {
        std::unique_ptr<MapContext> context;
        int framesRemaining;
    };
    std::deque<DeferredMapDelete> m_deferredDeletes;
    bool m_mapSwapPending = false;
    bool m_isInitialBoot = true;
    bool m_inFrame = false;
    bool m_playerSpawnedOnActiveMap = false;
    inline static const Vec3 kSafePreWarpPlayerPos = Vec3(0.0f, -1000.0f, 0.0f);
    std::string m_pendingMapWarp;
    std::string m_warpRestartTarget;   
    bool m_warpRestartPending = false;
    TextureResource m_splashLogo;
    VkDescriptorSet m_splashLogoDescriptorSet = VK_NULL_HANDLE;
    TextureResource m_splashSilhouette;
    VkDescriptorSet m_splashSilhouetteDescriptorSet = VK_NULL_HANDLE;
    TextureResource m_splashGlow;
    VkDescriptorSet m_splashGlowDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_splashDescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_splashPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_splashDS = VK_NULL_HANDLE;
    VkPipeline m_splashPipeline = VK_NULL_HANDLE;
    float m_splashTime = 0.0f;
    float m_splashTaskProgress = 0.0f;   // smooth progress for the current boot task text
    float m_splashBarProgress = 0.0f;    // smooth progress for the bottom loading bar
    float m_splashReveal = 0.0f;
    TextureResource m_loadingIcons[2];
    VkDescriptorSet m_loadingIconDS[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool m_showShadowSettings = true;
    float m_timeOfDaySlider = 360.0f; 
    int m_timeScaleIndex = 1; 
    std::vector<std::unique_ptr<RenderEffect>> m_renderEffects;
    MipmapDebugMenu m_mipmapMenu;
    WaterDebugMenu m_waterMenu;
    LoadedMap::MapLiquid m_lavaLiquid{};
    bool m_hasLavaLiquid = false;
    WaterBenchmark m_waterBenchmark;
    bool m_waterForceWaterPrev = false;
    float m_mapBaseWaterLevel = 0.0f;
    ConfigManager m_shadowConfig;
    ConfigManager m_postConfig;
    ConfigManager m_graphicsConfig;
    // Budgets set by the graphics preset (defaults = current hardcoded caps).
    int m_maxRainFollowers = 100;
    int m_maxFieldClouds = 100;
    // Fumaça do mapa (emissores vindos do .env). Orçamento por preset:
    // smoke=false desliga o passe, smoke_steps limita o ray-march.
    bool m_smokeEnabled = true;
    int m_smokeMaxSteps = 32;
    bool m_smokeSunTap = true;   // 2a amostra de densidade (auto-sombra)
    bool m_smokeDetail = true;   // 3a oitava do fbm
    // Battery: frame limiter (0 = uncapped) applied in run().
    // Cache da oclusao para o foco adaptativo do DoF - ver computeObstruction().
    mutable float m_obstructionCached = 0.0f;
    mutable int m_obstructionAge = 0;
    mutable bool m_obstructionValid = false;
    // Estado do minimapa estatico - ver renderMinimap().
    const void* m_minimapKey = nullptr;
    size_t m_minimapInstances = 0;
    size_t m_minimapChunks = 0;
    bool m_minimapForce = true;   // primeira renderizacao sempre acontece
    uint32_t m_minimapTriangles = 0; // triangulos do ultimo bake: sinal de que
                                     // a geometria ainda esta' subindo pra GPU
    std::array<double, 16> m_cpuPhaseMs{};  // ms por fase do render, deste frame
    int m_fpsCap = 0;
    // LOD geométrico (mipmap para malha), configurado por preset.
    size_t m_geoLodBudgetBytes = 0;   // 0 = desligado
    size_t m_geoLodBytesUsed = 0;
    float  m_geoLodTargetEdge = 8.0f;
    float  m_geoLodLowPass = 0.5f;
    float  m_geoLodDistance = 0.0f;

    // POM (banda média do displacement híbrido). heightScale 0 = desligado;
    // habilitado por preset em data/graphics.json ("pom_height_scale").
    float m_postScale = 1.0f;

    // Extensao em que a CENA e' desenhada. Igual a' do swapchain quando
    // ERUPTION_RENDER_SCALE = 1; menor quando a escala esta ativa. Todo passe
    // que desenha num alvo offscreen tem que usar ESTA, nao swapExtent() - um
    // renderArea maior que o attachment e' erro de Vulkan que nao retorna
    // codigo (VUID-VkRenderingInfo-pNext-06079/06080).
    uint32_t m_renderW = 0, m_renderH = 0;
    // Largura de display do ultimo resize: a escala de render (m_renderW /
    // isto) se mantem quando a janela ou o painel Cena mudam de tamanho.
    uint32_t m_displayW = 0;
    VkExtent2D renderExtent() const {
        return (m_renderW && m_renderH) ? VkExtent2D{m_renderW, m_renderH}
                                        : m_vulkan.swapExtent();
    }

    // Relogio proprio do vento: nao reinicia com o mapa, senao a vegetacao
    // inteira teleporta de posicao num frame.
    float m_windClock = 0.0f;
    float m_pomHeightScale = 0.0f;
    int m_pomMaxSteps = 12;
    int m_pomMinSteps = 4;
    ConfigManager m_dayNightConfig;
    ConfigManager m_dioramaConfig;
    ConfigManager m_skyboxConfig;
    ConfigManager m_cloudLayerConfig;

    // Runtime tweakables exposed in the Lighting debug tab.
    float m_normalMapScale = 1.0f;
    // Flat hemispheric fill light. Constant across the day/night cycle; the old
    // per-time spline (which cranked to ~4x at night and flat-washed the scene)
    // is gone - a moonlight directional fill covers night visibility instead.
    float m_ambientIntensity = 0.95f;
    // Indirect lighting (Lighting tab).
    bool  m_ssaoEnabled = true;
    float m_ssaoStrength = 0.8f;
    float m_ssaoRadius = 12.0f;  // unidades de MUNDO (celula de terreno = 10u)
    bool  m_envSpecEnabled = true;
    float m_envSpecIntensity = 1.0f;
    float m_nightAoContrast = 1.6f;   // AO exponent at night (1 = off)
    float m_ambientHemiFloor = 0.35f; // min hemispheric factor for down-facing surfaces
    bool  m_moonFillEnabled = true;
    float m_moonFillStrength = 1.0f;  // multiplier on the night moonlight directional
    // One-bounce GI: sun bounced off the ground back onto walls/undersides,
    // tinted by the map's average ground albedo (computed at map load).
    bool  m_sunBounceEnabled = true;
    float m_sunBounceStrength = 0.6f;
    // Average albedo of the loaded map's textures (computed at load, luminance-
    // normalised). Tints the hemispheric ground half AND the sun bounce, so the
    // indirect light carries this map's real palette (green for vila-A grass,
    // red laterite for parana) instead of a hardcoded brown.
    Vec3  m_mapGroundAlbedo = Vec3(0.08f, 0.06f, 0.04f);
    // Texturas embutidas do GLB já comprimidas em bloco (BC1/BC3 albedo, BC7
    // normal/mrahw) com mips prontos, indexadas pelo nome da imagem. Vazio
    // quando a GPU não suporta BC ou o bake falhou: o caminho RGBA8 continua
    // valendo, é só mais VRAM.
    std::unordered_map<std::string, BakedEmbeddedTexture> m_embeddedBaked;
    // Teto de resolução de textura do preset (data/graphics.json).
    int   m_textureMaxSize = 1024;
    // Screen-space contact shadows: short march toward the sun in the lighting
    // pass, catches near-field occluders the shadow map misses.
    bool  m_contactShadowEnabled = true;
    float m_contactShadowLength = 1.2f; // metres
    // Auto-exposure (eye adaptation). Histogram-measured when available,
    // analytic light-budget estimate as fallback.
    bool  m_autoExposureEnabled = true;
    float m_autoExposureKey = 1.55f;  // ~= noon light budget -> x1.0 at noon
    float m_autoExposureSpeed = 2.0f; // adaptation rate (1/s)
    float m_autoExposureMin = 0.75f;
    float m_autoExposureMax = 2.0f;
    float m_autoExposureCurrent = 1.0f;
    float m_normalSmoothing = 0.0f;   // mip bias on normal-map reads; 0 keeps normal aligned 1:1 with albedo
    // CURVAS POR DISTANCIA DA CAMERA (editaveis no F2 com o mesmo widget de
    // spline do DoF e das sombras - SplineEditorUI). X e' guardado normalizado
    // 0..1 e vale 0..m_normalDistMax unidades de mundo; os nos padrao saem do
    // mapeamento focal do DoF (35 / 100 / 180 / 1000 u).
    //   m_normalScaleCurve  - forca do normal map
    //   m_normalSmoothCurve - vies de mip na leitura do normal map
    //   m_shadowOpacitySpline - opacidade da sombra do sol (1 = preto cheio)
    // Vao para o shader como LUT de 8 amostras (ver Render.cpp), entao a curva
    // pode ter quantos pontos o autor quiser sem mexer no UBO.
    bool  m_normalDistCurve = true;
    bool  m_shadowOpacityCurve = true;
    float m_normalDistMax = 1000.0f;
    SplineCurve m_normalScaleCurve{{0.035f, 0.10f, 0.18f, 1.0f}, {0.33f, 0.60f, 1.0f, 1.0f}, InterpolationMode::Smoothstep};
    SplineCurve m_normalSmoothCurve{{0.035f, 0.10f, 0.18f, 1.0f}, {0.0f, 1.0f, 1.0f, 1.0f}, InterpolationMode::Smoothstep};
    SplineCurve m_shadowOpacitySpline{{0.035f, 0.10f, 0.18f, 1.0f}, {0.72f, 0.80f, 0.88f, 0.95f}, InterpolationMode::Smoothstep};
    // TESSELACAO DE HARDWARE NA BANDA PERTO (model.tesc/model.tese). O fator
    // vem de spline, como todo o resto: 8 colado na camera, caindo a 1 em 10%
    // da distancia maxima (zoom 90-100%), e 1 dali pra frente - onde vale 1 o
    // patch sai identico ao triangulo original e a malha atual continua
    // valendo. So' o CHAO entra na banda (ver ModelRenderer::render).
    bool  m_tessEnabled = true;
    float m_tessAmplitude = 2.5f;   // unidades de mundo, pico do deslocamento (autor: default 2.5)
    // Ganho da altura em passa-alta. A altura cozida vem da LUMINANCIA do
    // albedo (PbrMapGen), entao a banda baixa dela e' a sombra pintada da
    // textura; o shader usa mip2-mip5 e este ganho devolve a faixa util. Alto
    // demais e o chao vira papel amassado.
    float m_tessHeightGain = 1.5f;   // autor: default 1.5
    // Filtro passa-baixa (blur) sobre o height map - soma em mip no
    // heightUv() (model.vert/model.tese). 0 = sem blur extra, so' os mips
    // 2/5 originais. Slider pedido pelo autor pra' tirar ruido de alta
    // frequencia do relevo sem mexer no ganho/contraste. Default 0.15 (autor).
    float m_tessHeightBlur = 0.15f;
    // Espacamento alvo entre vertices da tesselacao (u). A curva sozinha tem
    // teto 8 por aresta, e o triangulo do chao tem ~10-12 u: ~1,3 u entre
    // vertices, grosso demais pra desenhar paralelepipedo. 0 = so' a curva.
    float m_tessVertexSpacing = 0.25f;
    // Teto de inclinacao da normal do normal map, em GRAUS (0 = sem teto).
    // Acima de ~65 graus a normal deita e a superficie le' como papel
    // amassado - ver o bloco em model.frag.
    float m_normalMaxSlopeDeg = 62.0f;
    SplineCurve m_tessCurve{{0.0f, 0.05f, 0.10f, 1.0f}, {8.0f, 4.0f, 1.0f, 1.0f}, InterpolationMode::Smoothstep};
    // MODO DE AUDITORIA DO DESLOCAMENTO (model.frag, bloco "AUDITORIA DO
    // DESLOCAMENTO"): 0 desliga, 1 deslocamento x normal, 2 luz x relevo,
    // 4 origem da altura - so' alcancaveis por ERUPTION_TEST_TESS_AUDIT (env
    // var, precisa reiniciar), sao ferramentas de bancada. O F2 (ImGui.cpp,
    // checkbox "Debug: height map") so' mexe entre 0 e 3: 3 e' o height map
    // em cinza por pixel (compara direto contra a textura, pedido do autor:
    // "o tesselation nao ta batendo com a textura" / "isso tem que ser um
    // checkbox"). Era ERUPTION_TEST_TESS_AUDIT so' de leitura no load; agora
    // vive aqui pra trocar ao vivo, e o env var so' fixa o valor inicial.
    int   m_tessDebugMode = 0;

    // Per-phase CPU frame breakdown, surfaced in the F3 debug overlay so a
    // bottleneck is attributable instead of just "the frame is slow".
    float m_cpuUpdateMs = 0.0f;     // whole loop iteration (present -> present)
    float m_cpuBeginFrameMs = 0.0f; // blocked in beginFrame (fence + acquire)
    float m_cpuAdvanceMs = 0.0f;    // advanceFrame(): sim, weather, clouds, streaming
    float m_cpuAppUpdateMs = 0.0f;  // app.onUpdate(): gameplay/controller
    float m_cpuRenderMs = 0.0f;     // render(): command recording + submit
    float m_cpuTailMs = 0.0f;       // onRender + input + profiler commit
    bool m_normalMapInvertY = false;   // true = flip normal-map green channel (Y down)
    float m_defaultRoughness = 0.8f;   // Fallback roughness when no PBR map is bound
    float m_defaultMetallic = 0.0f;    // Fallback metallic when no PBR map is bound
    struct CharConfig m_charConfig;
    ResourceConfig m_resourceConfig;
    InputConfig m_inputConfig;
    VkImage m_minimapImage = VK_NULL_HANDLE;
    VmaAllocation m_minimapAlloc = VK_NULL_HANDLE;
    VkImageView m_minimapView = VK_NULL_HANDLE;
    VkImage m_minimapDepthImage = VK_NULL_HANDLE;
    VmaAllocation m_minimapDepthAlloc = VK_NULL_HANDLE;
    VkImageView m_minimapDepthView = VK_NULL_HANDLE;
    // View com que m_minimapCompositeDS foi escrito (rewrites so' em resize).
    VkImageView m_minimapCompositeDSAlbedoView = VK_NULL_HANDLE;
    VkDescriptorSet m_minimapDescriptorSet = VK_NULL_HANDLE;
    bool m_minimapBottomRight = false;
    // Minimap composite pass (IGNIS G10/G11): relights the minimap G-buffer and
    // paints water from terrain height, instead of blitting raw albedo. See
    // renderMinimap() and shaders/postprocess/minimap_composite.frag.
    VkDescriptorSetLayout m_minimapCompositeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_minimapCompositeLayout = VK_NULL_HANDLE;
    VkPipeline m_minimapCompositePipeline = VK_NULL_HANDLE;
    VkDescriptorSet m_minimapCompositeDS = VK_NULL_HANDLE;
    static constexpr uint32_t MINIMAP_RES = 256;
    PlayerController* m_playerController = nullptr;

    bool m_editorMode = false;
    Editor* m_editor = nullptr;
    std::function<void()> m_gameOverlay;
    uint32_t m_viewportW = 0, m_viewportH = 0;
    VkImage m_viewportImage = VK_NULL_HANDLE;
    VmaAllocation m_viewportAlloc = VK_NULL_HANDLE;
    VkImageView m_viewportView = VK_NULL_HANDLE;
    VkDescriptorSet m_viewportTexture = VK_NULL_HANDLE;
    void recreateViewportTarget();
    void destroyViewportTarget();
    bool m_running = false;
    bool m_initialized = false;
    bool m_resized = false;

    // Shimmering metric validation resources
    bool initShimmerMetric();
    void shutdownShimmerMetric();
    VkImage m_shimmerImage = VK_NULL_HANDLE;
    VmaAllocation m_shimmerAlloc = VK_NULL_HANDLE;
    VkBuffer m_shimmerBuffers[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation m_shimmerBufferAllocs[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::vector<uint8_t> m_shimmerPrevPixels;
    std::vector<float> m_shimmerHistory;
    float m_shimmerCurrentValue = 0.0f;
    uint32_t m_shimmerFrameCount = 0;
    bool m_shimmerMeasureEnabled = false;
};
} 
