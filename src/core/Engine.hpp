#pragma once

#include <array>
#include "core/MinimapWalk.hpp"
#include "core/Window.hpp"
#include "core/Timer.hpp"
#include <chrono>
#include <limits>
#include <thread>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif
#include "renderer/VulkanContext.hpp"
#include "renderer/SpritePickerUI.hpp"
#include "renderer/GBuffer.hpp"
#include "utils/SplineCurve.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/TextureStreamer.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "utils/EmbeddedTextureBake.hpp"
#include "utils/ImageUtils.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/DeferredLighting.hpp"
#include "renderer/ShadowRenderer.hpp"
#include "renderer/PostProcessor.hpp"
#include "renderer/VolumetricLight.hpp"
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
#include "renderer/vfx/VfxSystem.hpp"
#include "core/Affects.hpp"
#include "core/GroundState.hpp"
#include "core/FootstepEffects.hpp"
#include "core/SpriteContact.hpp"
#include "renderer/vfx/AmbientParticles.hpp"
#include "formats/TextureRegions.hpp"
#include "renderer/VisionMap.hpp"
#include "core/Vision.hpp"
#include "formats/Ribbon.hpp"
#include "renderer/LocalCloud.hpp"
#include "renderer/WaterRenderer.hpp"
#include "renderer/WaterReflectionPass.hpp"
#include "renderer/DebugLineRenderer.hpp"
#include "renderer/OverlayLineRenderer.hpp"
#include "renderer/ImmediateDraw.hpp"
#include "renderer/vfx/CloudPrecip.hpp"
#include "renderer/vfx/VfxBolt.hpp"
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
#include "renderer/UiCanvas.hpp"
#include "renderer/UiDocument.hpp"
#include "core/InputConfig.hpp"
#include "formats/SpriteTypes.hpp"
#include "core/ConfigManager.hpp"
#include "core/Input.hpp"
#include "utils/Profiler.hpp"
#include "renderer/GpuTextures.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/EtexLoader.hpp"
#include "formats/MapLoader.hpp"
#include "formats/TerrainParser.hpp"
#include "formats/ModelFile.hpp"
#include "formats/PackManager.hpp"
#include <vector>
#include <iosfwd>
#include <string>
#include <imgui.h>
#include <memory>
#include <future>
#include "scene/SceneEdits.hpp"
#include "scene/SceneObjects.hpp"
#include "audio/Audio.hpp"
#include <deque>
#include <mutex>
#include <unordered_map>
#include <functional>
namespace eruption {
struct LoadedMap;
struct ImageData;
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
    // Altura "qualquer" nas consultas de chão (mapAt e quem chama): o primeiro
    // mapa que cobre x/z, como antes de existir mapa empilhado.
    static constexpr float kAnyHeight = std::numeric_limits<float>::quiet_NaN();
    Engine();
    ~Engine();
    bool init(int width, int height, const std::string& title);
    void shutdown();
    template<typename App>
    void run(App& app) {
        if (!m_initialized) return;
#if defined(__EMSCRIPTEN__)
        s_browserEngine = this;
#endif
        app.onInit(*this);
        ERUPTION_LOG_INFO("[ENGINE] Entering main loop (running={}, shouldClose={})", m_running, m_window.shouldClose());
#if defined(__EMSCRIPTEN__)
        // O navegador e' dono do laco: um quadro por requestAnimationFrame. O
        // simulate_infinite_loop=1 nao volta daqui (main nunca termina), como
        // o while dos outros alvos.
        emscripten_set_main_loop_arg(&Engine::browserFrame<App>, &app, 0, 1);
#else
        while (m_running && !m_window.shouldClose()) runFrame(app);
#endif
        app.onShutdown();
        m_vulkan.waitIdle();
    }
    // Um quadro do laco principal (run). Separado para o navegador chamar.
    template<typename App>
    void runFrame(App& app) {
        // Coarse per-phase CPU timing for the debug overlay (F3). The frame
        // marks inside render() only cover command recording, so without
        // these the overlay cannot tell "the sim is slow" from "the GPU is
        // slow" — the two look identical as a high frame time.
        auto _t0 = std::chrono::steady_clock::now();
        float dt = advanceFrame();
        m_frameDt = dt;
        auto _t1 = std::chrono::steady_clock::now();
        app.onUpdate(dt);
        // Objetos da cena: puxa o que mexeram nas tabelas, propaga pai ->
        // filho e empurra o que mudou, antes de desenhar.
        m_sceneObjects.sync(*this);
        // Sprites: corte, ordem e envio com a camera FINAL do quadro, depois
        // do jogo inteiro. Feito no PlayerController, o corte usava a camera
        // de antes dos scripts: um jogo que mira a camera por script
        // (camera.set_target num personagem longe do jogador) via o
        // personagem cortado - 2 sprites enviados, 0 visiveis - e o sprite
        // do jogador, enviado depois do corte, saia um quadro atrasado.
        m_spriteSystem.processSprites(m_camera.frustum(), m_camera.position(), m_camera.forward());
        auto _t2 = std::chrono::steady_clock::now();
        // Segura pelo script (loading.hold): nenhum quadro da cena antes do
        // release; no editor a espera nao vale (a interface sumiria).
        if ((m_loadingHeld || m_loadingFinishing) && !m_editorMode) presentHeldLoading();
        else render();
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
            m_capSleepMs = 0.0f;
            if (_next < _now) _next = _now;
            else {
                m_capSleepMs = _ms(_next - _now).count();
                std::this_thread::sleep_until(_next);
            }
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
#if defined(__EMSCRIPTEN__)
    template<typename App>
    static void browserFrame(void* app) { s_browserEngine->runFrame(*static_cast<App*>(app)); }
    static inline Engine* s_browserEngine = nullptr;
#endif
    void enableAutoExit(bool enable) { m_enableAutoExit = enable; }
    bool autoExitEnabled() const { return m_enableAutoExit; }
    void enableWaterBenchmark(bool enable) { m_enableWaterBenchmark = enable; }
    void setBenchmarkMap(const std::string& map) { m_benchmarkMap = map; }
    void setBenchmarkPosition(const Vec3& pos) { m_benchmarkPos = pos; }
    void setBenchmarkDuration(float sec) { m_benchmarkDuration = sec; }
    void enableBenchmarkThunderstorm(bool enable) { m_benchmarkThunderstorm = enable; }
    void enableBenchmarkHeavyRain(bool enable) { m_benchmarkHeavyRain = enable; }
    bool isRunning() const { return m_running; }
    void requestQuit() { m_running = false; }
    void logMemoryCaches() const; // [MEMCACHE]: tamanho dos caches que atravessam a troca de mapa
    // `preloaded`: mapa já lido do disco numa thread (troca pelo seletor de
    // mapas); o resto é o mesmo carregamento do mapa aberto direto.
    bool loadMap(const std::string& name, std::shared_ptr<LoadedMap> preloaded = nullptr);
    bool warpLoading() const { return m_warpJobActive; }
    // Le' o mapa numa thread (arquivos e conversao dos modelos) antes do warp
    // para ele; o warp seguinte so' monta. false = o mapa nao existe. Chamar
    // de novo com o mesmo nome nao refaz nada; outro nome espera a leitura em
    // curso acabar e entao toma o lugar.
    bool preloadMap(const std::string& name);
    // Acrescenta um .glb ao mapa aberto, com o chao do modelo em `position`.
    // Devolve o indice da primeira instancia (uma por no) ou -1.
    // `nodeName` não vazio: só esse nó do arquivo.
    int addModelFromFile(const std::string& glbPath, const Vec3& position, const std::string& nodeName = {});
    // Le, interpreta e decodifica as texturas embutidas de um .glb num job,
    // fora do quadro; o addModelFromFile seguinte so' envia para a GPU.
    void preloadModel(const std::string& glbPath);
    void prepareSceneModels(const std::vector<std::string>& files); // cena: malhas em paralelo
    // Espera o preload deste arquivo terminar (se houver um). Para quem vai
    // usar o modelo agora no mesmo mapa e disparou varios preloads antes
    // (cena aplicada): todos se preparam em paralelo, cada um espera so' o seu.
    void waitModelPreload(const std::string& glbPath);
    const std::string& currentMapName() const { return m_currentMapName; }
    // `y` (opcional): altura de quem pergunta, para escolher entre mapas
    // empilhados (ver mapAt).
    // withRelief = false: o chao sem o desnivel do relevo assado na tela
    // (ModelRenderer::reliefFloorDelta) - a inclinacao do anel no chao.
    float sampleTerrainHeight(float x, float z, float y = kAnyHeight, bool withRelief = true) const;
    // Altura do chao de UM mapa (sem vizinhos nem relevo): so' le', qualquer thread.
    static float sampleOpenMapHeight(const LoadedMap& map, float x, float z);
    // Raio contra a grade de altura do mapa (.ter); falso se o mapa não tem uma.
    bool raycastTerrain(const Vec3& origin, const Vec3& dir, float maxT, float& hitT) const;
    // Distancia da camera ao chao no centro da tela (marcador das curvas por distancia).
    float cameraGroundDistance() const;
    // Painel fixo da camera (posicao/rotacao) com botao que grava a pose no log.
    void drawCameraHud();
    static bool raycastGrid(const TerrainFile& ter, const Vec3& origin, const Vec3& dir, float maxT, float& hitT);
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
    // Nuvens de script (tabela weather do Luau). Cada campo de ScriptCloudSpec
    // tem padrao sensato; ScriptCloudSpec::Field diz quais campos um cloud_set
    // traz. Implementado em ScriptCloud.cpp.
    struct ScriptCloudSpec {
        enum Field : uint32_t {
            FX = 1u << 0, FZ = 1u << 1, FRadiusX = 1u << 2, FRadiusZ = 1u << 3, FDensity = 1u << 4,
            FCoverage = 1u << 5, FAltitude = 1u << 6, FFalloff = 1u << 7, FDarkness = 1u << 8,
            FTint = 1u << 9, FFadeIn = 1u << 10, FLife = 1u << 11, FFadeOut = 1u << 12, FDrift = 1u << 13,
            FThickness = 1u << 14, FFrame = 1u << 15, FArea = 1u << 16, FPrecip = 1u << 17, FClimate = 1u << 18, FFrameFit = 1u << 19,
        };
        float x = 0.0f, z = 0.0f;
        float radiusX = 80.0f, radiusZ = 60.0f;  // semi-eixos em metros
        float density = 0.9f;                    // 0..1
        float coverage = 0.7f;                   // 0..1, limiar do ruido interno
        // Metros acima do chao em (x, z): a altura das nuvens do clima (as de campo
        // ficam em 39 + 780 x altitude acima do alvo). Nada rente ao personagem.
        float altitude = 230.0f;
        float falloff = 1.2f;                    // suavidade da borda
        float darkness = 0.0f;                   // 0..1, base escurecida
        Vec3 tint = Vec3(1.0f);                  // cor (multiplica)
        float fadeIn = 0.7f;                     // s para condensar
        float life = 6.0f;                       // s formada; <= 0: ate' cloud_remove
        float fadeOut = 1.5f;                    // s para dispersar ao fim da vida
        bool drift = false;                      // o vento leva a nuvem
        // Espessura vertical do domo em metros; 0 = automatica (metade do maior raio,
        // entre 14 e 90). O topo fica em espessura / falloff acima da base.
        float thickness = 0.0f;
        // 0..1: quanto a massa e' deslocada para o lado da camera para aparecer
        // CENTRADA em cima do alvo (x, z) na tela. 0 = fica exatamente em cima do
        // ponto no mundo (aparece acima do alvo na tela).
        float frame = 0.0f;
        // Opcional (false por padrao: frame se comporta como sempre). true: a posicao
        // e' calculada para a BASE da nuvem aparecer no terco superior da tela, pelo
        // raio e pela altitude (nao pela altura media do domo). Nao usa `frame`.
        bool frameFit = false;
        // Raio da area da habilidade (m), 0 = nao usado. Quando vem sem radius_x/z, a
        // nuvem fica com a borda visivel em 1,3x essa area (raios = 1,3 * area * falloff).
        float areaRadius = 0.0f;
        // Clima local: o preset diz que clima a nuvem traz (1 = tempestade, 2 = neve,
        // 0 = nenhum, nuvem so' visual). `climate` liga a gradacao local (luz, cor,
        // neblina) e `precip` a chuva/neve; as duas valem so' com kind > 0.
        int kind = 0;
        bool climate = true;
        bool precip = true;
    };
    // Raios da nuvem a partir da area da habilidade e do falloff atual.
    static void deriveRadiiFromArea(ScriptCloudSpec& s);
    // Valores iniciais de um preset ("storm", "snow"); false se o nome nao existe.
    // Os campos que o script passar sobrescrevem o preset.
    static bool scriptCloudPreset(const std::string& name, ScriptCloudSpec& spec);
    // Ponto aleatorio na base da nuvem (para o script saber de onde o raio sai);
    // false se o id nao existe.
    bool scriptCloudPoint(uint32_t id, Vec3& out);
    // Relampago por dentro da massa: intensidade (0..8) e duracao em segundos.
    bool flashScriptCloud(uint32_t id, float intensity, float duration);
    // Precipitacao da nuvem: snow/rain na area; on = false a desliga.
    bool setScriptCloudPrecip(uint32_t id, bool on, bool snow, float rate, float radius);
    // Raio 3D (vfx.bolt) e fita de precipitacao: ver renderer/vfx/VfxBolt.hpp e CloudPrecip.hpp.
    uint32_t startBolt(BoltSpec spec);
    bool stopBolt(uint32_t id) { return m_bolts.stop(id); }
    // Por quadro: sorteia/avanca raios e precipitacao e entrega a geometria ao ImmediateDraw.
    void updateScriptFx(float dt);
    void submitScriptFx();
    // Clima local das nuvens de script no composite (gradacao pelo mundo em volta de cada uma).
    void fillLocalClimate(PostProcessor::PostSettings& out) const;
    VfxBolt& bolts() { return m_bolts; }
    CloudPrecip& cloudPrecip() { return m_precip; }
    // Devolve o id (> 0) ou 0 quando recusada (cota cheia, nuvens desligadas).
    uint32_t spawnScriptCloud(const ScriptCloudSpec& spec);
    bool moveScriptCloud(uint32_t id, float x, float z);
    bool setScriptCloud(uint32_t id, const ScriptCloudSpec& spec, uint32_t fields);
    // Dispersa a nuvem em `fadeOut` segundos (< 0: o fade_out dela) e a remove.
    bool removeScriptCloud(uint32_t id, float fadeOut);
    uint32_t scriptCloudCount() const { return m_scriptCloudCount; }
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
    // loading.hold()/release() do script: a tela de carga fica no lugar da
    // cena (o jogo roda por tras) ate' soltar ou passar maxSeconds.
    void holdLoading(bool on, float maxSeconds = 30.0f);
    bool loadingHeld() const { return m_loadingHeld; }
    // loading.game_progress: a fila do proprio jogo durante a espera.
    void setGameLoadProgress(float done, float total);
    // loading.progress: (feito, total, pronto) da carga em curso. feito/total
    // e' a fracao da barra (nunca volta; na primeira carga, 0.3 + 0.7 * p,
    // como a barra unica do boot); total conta o mapa e o trabalho pendente
    // visto nesta carga. Pronto so' com nada pendente.
    void loadProgress(float& done, float& total, bool& ready);
    // Trabalho de carga pendente agora: folhas de sprite em preparo, mapa e
    // vizinhos lendo, texturas na fila de envio, sons decodificando.
    int pendingLoadWork();
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
        // Primeira linha (de cima) com um pixel de alfa >= 128: a cabeça de
        // characters.head_screen, numa célula alta por causa de um efeito.
        uint32_t opaqueTop = 0;
        // Pe' (faixa de baixo do opaco): largura e centro em x, em 1/255 da
        // largura do quadro; largura 0 = sem medida. Vao nos bits 16-31 das
        // flags do sprite (SpriteFlags::footBits) para o disco de contato.
        uint8_t footWidth = 0, footCenter = 128;
        // As ultimas linhas opacas, em bits (SpriteContact.hpp): a silhueta de
        // contato dos passos. Nulo = quadro vazio ou textura de outra origem.
        std::shared_ptr<const SpriteContactRows> contact;
    };
    struct LoadedSprite {
        AnimFile act;
        std::vector<TextureResource> textures;
        // Normal map AUTORAL da folha (<folha>_normal.png ao lado do PNG, ou
        // <folha>_normal.spr com as mesmas imagens do .spr): mesmo indice e UV
        // de `textures`; vazio = sem mapa (o sprite fica so' com a normal
        // abaulada). Forca no preset: sprite_normal_strength.
        std::vector<TextureResource> normalTextures;
        // Varias personagens podem usar o mesmo arquivo de sprite. Quando isso
        // acontece elas DIVIDEM as texturas; esta ficha conta quantas ainda
        // usam, e so a ultima a soltar devolve os slots a GPU.
        std::shared_ptr<char> texturesShared;
        uint32_t indexedTextureCount = 0;
        bool loaded = false;
        bool isPngSprite = false;
        // Largura do pe' da folha (mediana do footWidth de todos os quadros,
        // em pixels) e quantas texturas ela viu: o disco de contato usa a
        // mesma pegada em todo quadro (ver footMedianPx em Characters.cpp).
        float footMedianPx = 0.0f;
        size_t footMedianOf = 0;
        // ERUPTSPR v2: o atraso de cada ação vem do arquivo (andar escala
        // com a velocidade a partir dele, não do valor fixo antigo).
        bool authoredTiming = false;
        // ERUPTSPR v3: animações extras com nome (8 ações cada, uma por direção).
        struct Named { std::string name; uint32_t first; bool loop; };
        std::vector<Named> named;
        const Named* findNamed(const std::string& n) const {
            for (const auto& a : named)
                if (a.name == n) return &a;
            return nullptr;
        }
        // Contato dos pes por acao (indice da acao), montado na primeira vez
        // que ela anda: a forma que toca o chao, lida dos pixels.
        std::unordered_map<int, std::shared_ptr<contact::Action>> contactActions;
    };
    // Personagem da cena (NPC): ver core/Characters.cpp.
    struct SceneCharacter {
        std::string name, sprite;
        Vec3 position{0.0f};
        // Passos (CharacterStep): ultima posicao medida, distancia andada desde
        // o ultimo passo e o pe' do proximo (0 = esquerdo, 1 = direito).
        Vec3 stepLastPos{0.0f};
        float stepOdometer = 0.0f;
        int stepFoot = 0;
        bool stepTracked = false;
        // Passo por pixel (trackContactSteps): medida do quadro (distancia
        // andada, direcao, velocidade), se o sprite manda nos passos, o ultimo
        // quadro mostrado (acao, quadro) e a distancia andada arrastando.
        float stepDist = 0.0f, stepSpeed = 0.0f;
        Vec2 stepDir{0.0f, 1.0f};
        bool contactDriven = false;
        int contactAction = -1, contactFrame = -1;
        float dragOdometer = 0.0f;
        // Direcao sem 2 pisadas por ciclo detectadas: passada, mas com a forma do sprite.
        std::string strideShape;
        float strideWidth = 0.0f;
        float facing = 0.0f; // graus; 0 = olhando para +Z
        bool visible = true;
        bool removed = false; // apagado no editor (desfazer traz de volta)
        bool fromNeighbor = false; // de um mapa vizinho (não se edita nem se salva aqui)
        bool parked = false;       // o mapa vizinho dele saiu de cena
        // Previa dos scripts de editor (ScriptHost::runEditorPreview): existe so'
        // em modo de edicao, nao entra na cena salva nem no Play.
        bool transient = false;
        // `ground = true` da cena: assenta no chão ao carregar (o editor
        // salva). O assentar espera o primeiro updateCharacters: o vizinho só
        // vira Shown (e entra em mapAt) depois de criar os personagens dele.
        bool ground = false;
        bool groundPending = false;
        // O relevo assa em segundo plano e muda o piso quando entra: enquanto
        // houver assado pendente o assentar se repete a partir do mesmo Y de
        // partida (groundStartY), e solta se o script mexeu na altura.
        float groundStartY = 0.0f;
        float groundSettledY = 0.0f;
        // Em cima de um lago congelado (ground_state ice_water): o motor mantem o
        // Y na superficie do gelo e afunda junto com ele no degelo. iceY = o Y que
        // o motor escreveu; se o script mexer na altura, a regra solta.
        bool iceRide = false;
        float iceY = 0.0f;
        bool groundSettled = false;
        // Y escrito na cena: o editor salva este, não o assentado, para abrir
        // e salvar uma cena não mudar o arquivo.
        float groundFromY = 0.0f;
        // Andar conduzido por set_position(..., true): distância horizontal
        // acumulada desde o último tick, folga que segura o andar entre
        // chamadas e a velocidade medida (ritmo dos passos).
        float scriptStep = 0.0f;
        float scriptWalkLeft = 0.0f;
        float scriptSpeed = 0.0f;
        // set_stride: velocidade suavizada que rege o ciclo (0 = parado) e o
        // ultimo atraso por quadro, para manter a fase ao trocar de atraso.
        float strideSpeed = 0.0f;
        float lastDelay = 0.0f;
        LoadedSprite look;
        int frame = 0;
        float timer = 0.0f;
        // Animacao em degraus (setSpriteStepFps): acao e quadro MOSTRADOS e
        // o ultimo degrau do relogio em que foram trocados.
        int shownAction = -1, shownFrame = 0;
        int64_t shownStep = -1;
        bool walking = false;
        // Caminho achado (desvia de objetos sólidos e do que não se anda).
        std::vector<Vec3> path;
        size_t pathIndex = 0;
        float speed = 60.0f; // unidades por segundo
        // Balão de fala.
        std::string say;
        float sayTime = 0.0f;
        // Animação extra tocando (do sprite v3); vazia = parado/andando.
        std::string action;
        bool actionLoop = false;
        float actionSeconds = 0.0f; // duracao pedida da animacao inteira; 0 = a da folha
        // Comportamento: seguir o jogador ou patrulhar pontos (só com o jogo rodando).
        // SUCO (juice) de apresentacao. Sem isso uma acao nao tem peso: o
        // boneco se mexe, o numero aparece e nada na tela reage. Todos decaem
        // sozinhos em updateCharacters e sao aplicados em drawCharacter.
        struct Fx {
            Vec3 flashColor{1.0f};      // cor somada no sprite (branco = clarao)
            float flashLeft = 0.0f, flashTotal = 0.0f;
            float shakeAmp = 0.0f;      // chacoalho do proprio sprite, em unidades
            float shakeLeft = 0.0f, shakeTotal = 0.0f, shakeTime = 0.0f;
            Vec3 lungeDir{0.0f};        // avanco do golpe: vai e volta
            float lungeDist = 0.0f, lungeLeft = 0.0f, lungeTotal = 0.0f;
            float popScale = 0.0f;      // estica/amassa (0 = nada)
            float scale = 1.0f;         // tamanho que fica (characters.set_scale)
            float popLeft = 0.0f, popTotal = 0.0f;
            float fade = 1.0f;          // opacidade (morte, entrada em cena)
            float fadeTo = 1.0f, fadeLeft = 0.0f, fadeTotal = 0.0f, fadeFrom = 1.0f;
        } fx;
        enum class Behavior { None, Follow, Patrol } behavior = Behavior::None;
        float followDistance = 30.0f;
        std::vector<Vec3> patrol;
        size_t patrolNext = 0;
        float patrolWait = 0.0f, waitLeft = 0.0f, repath = 0.0f;
        // Topo do quadro desenhado agora (drawCharacter), no espaco do
        // billboard: x = deslocamento lateral, y = altura acima do pe' ao
        // longo do "cima" inclinado; headAnchor = o pe' com o suco. headFrame
        // = quadro em que foi desenhado (velho = fora de cena).
        Vec2 head{0.0f};
        // Caixa do desenho no mesmo espaco: x = lado min/max, y = altura min/max
        // (characters.at_screen).
        Vec4 hitBox{0.0f};
        Vec3 headAnchor{0.0f};
        int headFrame = -10;
        // characters.ring: raio 0 = sem anel; cor com o alfa em a.
        float ringRadius = 0.0f, ringWidth = 0.0f;
        Vec4 ringColor{1.0f};
        Vec3 followTarget{0.0f};
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
    AudioSystem& audio() { return m_audio; }

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
    // Quadro ortografico do minimapa: x, z do centro em mundo e meio-lado
    // (zoom). E' o MESMO enquadramento que renderMinimap() calcula a partir
    // das AABBs de terreno/instancia - exposto para o jogo posicionar
    // marcadores sem reimplementar a formula por fora (ver ui.minimap_frame
    // em ScriptApi.cpp). z <= 0 antes do primeiro bake.
    Vec3 minimapFrame() const { return m_minimapFrame; }
    // Descritor da imagem da cena para ImGui::Image (VK_NULL_HANDLE sem editor).
    VkDescriptorSet viewportTexture() const { return m_viewportTexture; }
    InputConfig& inputConfig() { return m_inputConfig; }
    CharConfig& charConfig() { return m_charConfig; }
    auto* currentMap() const { return m_currentMap.get(); }
    const std::vector<std::string>& availableMaps() const { return m_availableMaps; }
    void setPendingMapWarp(const std::string& map) { m_pendingMapWarp = map; }
    // Troca de mapa pedida pelo jogo (porta, jogo salvo): quem roda o jogo
    // (aplicação ou editor) faz a troca fora do frame e põe o jogador em `spawn`.
    struct MapRequest {
        std::string map;
        bool hasSpawn = false;
        Vec3 spawn{0.0f};
    };
    void requestMap(const std::string& map, const Vec3* spawn) {
        m_mapRequest = {map, spawn != nullptr, spawn ? *spawn : Vec3(0.0f)};
    }
    bool takeMapRequest(MapRequest& out) {
        if (m_mapRequest.map.empty()) return false;
        out = std::move(m_mapRequest);
        m_mapRequest = {};
        return true;
    }
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
    // Live switch for the quality screen of a game (script `graphics.apply`):
    // the active preset becomes a copy of preset `base` with `overrides` on top
    // (the top-level key "present_mode" goes to the file root), then it is
    // applied again and the scene resolution follows its "render_scale".
    // False when `base` does not exist. Keys decided before the pipelines
    // exist (shader_lite, post_format, texture_layer_*, relief_bake modes of
    // meshes already loaded) only take effect on the next start.
    bool applyGraphicsLive(const std::string& base, const json& overrides);
    const json* activeGraphicsPreset();
    const std::string& graphicsPresetName();
    std::string m_graphicsPreset; // resolvido uma vez (graphicsPresetName)
    bool m_tessPresetAllowed = true; // chave "tessellation" do preset
    // chave "tessellation_max_level" = teto do fator de tesselacao na banda
    // (curva "Tessellation Factor by Distance"); o teto do device vale por cima.
    float m_tessLevelCap = 64.0f;
    float m_tessBandScale = 1.0f;    // chave "tessellation_band_scale"
    float m_tessNearDistance = 0.0f;   // chave "tessellation_near_distance" (0 = sem banda perto)
    float m_tessZoomFollow = 0.0f;     // chave "tessellation_zoom_follow" (Engine::tessZoomFollow)
    float m_tessAxisDist = 0.0f;       // chave "tessellation_max_distance" (u; 0 = normal_distance.max_distance x lente)
    float m_tessAxisMipOct = 0.0f;     // chave "tessellation_axis_mip_octaves" (0 = usa m_tessAxisDist; Engine::tessAxis)
    float m_tessNearSpacing = 0.15f;   // chave "tessellation_near_spacing"
    float m_tessHeightMinSpacing = 0.0f; // chave "tessellation_height_min_spacing" (0 = so' a grade)
    // Ilusao barata de relevo no chao (preset low; FrameUBO::reliefParams.yzw).
    float m_reliefCavity = 0.0f;         // chave "relief_cavity" (forca do escurecimento de fresta; 0 desliga)
    float m_reliefCavityContrast = 1.0f; // chave "relief_cavity_contrast" (estica o desvio da altura/AO)
    float m_presetNormalStrength = 1.0f; // chave "normal_strength" (multiplica o normal map; 1 = como sempre)
    float m_reliefSunEmboss = 0.0f;      // chave "relief_sun_emboss" (lado do sol da pedra mais claro, o oposto mais escuro; 0 desliga)
    // POM do chao por preset (medium; FrameUBO::reliefPom/reliefPom2, model_frag reliefPomPreset).
    float m_reliefPomDepth = 0.0f;       // chave "relief_pom" (espessura da camada em u; 0 desliga)
    float m_reliefPomMaxSteps = 12.0f;   // chave "relief_pom_steps"
    float m_reliefPomMinSteps = 4.0f;    // chave "relief_pom_min_steps"
    float m_reliefPomDistance = 150.0f;  // chave "relief_pom_distance" (u onde o POM some)
    float m_reliefPomContrast = 6.0f;    // chave "relief_pom_contrast"
    float m_tessHeightGridFilter = 0.0f; // chave "tessellation_height_grid_filter" (filtro da altura pelo espacamento real da grade; 0 = desliga)
    float m_reliefPomAo = 0.0f;          // chave "relief_pom_ao" (fresta pela profundidade do POM)
    // Relevo sem tesselacao, so' no fragmento (FrameUBO::reliefPom3/reliefPom4; 0 desliga).
    float m_reliefHeightNormal = 0.0f;   // chave "relief_height_normal" (espessura u da altura que vira normal)
    float m_reliefSelfShadow = 0.0f;     // chave "relief_self_shadow" (forca da auto-sombra ao sol)
    float m_reliefSelfShadowSteps = 6.0f;    // chave "relief_self_shadow_steps"
    float m_reliefFragmentDistance = 120.0f; // chave "relief_fragment_distance"
    float m_reliefSelfShadowHeight = 0.35f;  // chave "relief_self_shadow_height"
    float m_reliefSelfShadowLength = 1.0f;   // chave "relief_self_shadow_length"
    float m_reliefPomNearBoost = 0.0f;   // chave "relief_pom_near_boost"
    float m_reliefPomNearDistance = 60.0f;   // chave "relief_pom_near_distance"
    // Calibracao de albedo na escrita do G-buffer (gbuffer/albedo_calib.glsl;
    // FrameUBO::albedoCalib): chaves "albedo_knee" (luminancia linear onde a
    // compressao comeca, 0 desliga) e "albedo_ceiling" (teto da luminancia).
    float m_albedoKnee = 0.0f;
    float m_albedoCeiling = 0.40f;
    // Sprites (FrameUBO::spriteParams): chave "sprite_filter" (0 nearest, 1
    // bilinear nitido) e "sprite_normal_strength" (normal map autoral, 0 = sem).
    float m_spriteFilter = 0.0f;
    float m_spriteNormalStrength = 1.0f;
    // Distorcao de tela dos emissores "distortion" (preset "distortion":
    // multiplica o autorado; 0 desliga o passe do VFX e o do pos).
    float m_distortionStrength = 1.0f;
    float m_tessNearEdgePixels = 8.0f; // chave "tessellation_near_edge_pixels"
    uint32_t m_godRayLights = 4;     // chave "god_ray_lights"
    float m_godRaySun = 0.0f;        // chave "god_ray_sun" (forca dos raios do sol)
    float m_lensFlare = 0.0f;        // chave "lens_flare" (forca do flare do sol)
    int m_lensFlareRays = 6;         // chave "lens_flare_rays" (linhas da estrela)
    MapFog m_scriptFog;              // world.set_fog (set=false: nenhuma)
    WorldBorder m_scriptBorder;       // world.set_border (set=false: nenhuma)
    float m_scriptExposure = -1.0f;  // world.set_exposure (< 0: a do .env ou automatica)
    std::string m_graphicsPresetAuto; // o que a tabela auto_presets escolheu (vazio fora do auto)
    std::string m_presetChoiceNext;   // escolha do botao Qualidade, vale na proxima abertura
    static constexpr const char* kPresetOverrideFile = "preset_override.txt";
    // Escada do botao Qualidade. mobile_legacy so' entra onde a tabela a escolheu
    // (GPU de entrada): ela e' o degrau Baixa ali, e o ciclo para em mobile_low.
    static constexpr const char* kQualityPresets[4] = {"mobile_legacy", "mobile_low", "mobile_medium", "mobile_high"};
    size_t qualityBase() const { return m_graphicsPresetAuto == kQualityPresets[0] ? 0 : 1; }
    size_t qualityLevels() const {
        return (m_graphicsPresetAuto == kQualityPresets[0] || m_graphicsPresetAuto == kQualityPresets[1]) ? 2 : 3;
    }
    void cycleQualityPreset();
    // integer_scale: n de 1..kMaxIntegerDivisor tal que n divide a largura e a altura da tela, o
    // mais perto de 1/scale (render = tela / n, exato). 1 se nenhum outro serve.
    // Ate 1/8 da tela (os presets seguram o piso em render_scale_min; 1/2 e o piso dos mobile_*).
    static constexpr uint32_t kMaxIntegerDivisor = 16;
    // Copia a saida do pos para o swapchain por blit NEAREST (escala inteira exata).
    void blitSceneToSwap(VkCommandBuffer cmd, uint32_t imageIndex, uint32_t sw, uint32_t sh);
    static uint32_t integerScaleDivisor(float scale, uint32_t dispW, uint32_t dispH);
    bool m_integerScale = false;
    void syncUiFramebufferScale();
    bool m_presetOverridesApplied = false; // ERUPTION_TEST_PRESET_SET, uma vez
    void applyPresetEffects();
    DioramaLookConfig& lookConfig() { return m_lookConfig; }
    ConfigManager& dioramaConfig() { return m_dioramaConfig; }
    ConfigManager& skyboxConfig() { return m_skyboxConfig; }
    SkySystem& skyboxSystem() { return m_skybox; }
    VkSampler defaultSampler() const { return m_defaultSampler; }
    VulkanContext& vulkan() { return m_vulkan; }
    VfxSystem& vfx() { return m_vfx; }
    // InitInfo do sistema de particulas da cena; frameUboSet != 0 troca o
    // FrameUBO (outra camera, ex.: a previa de efeitos do editor).
    VfxSystem::InitInfo vfxInitInfo(VkDescriptorSet frameUboSet = VK_NULL_HANDLE);
    ModelRenderer& modelRenderer() { return m_modelRenderer; }
    TerrainRenderer& terrainRenderer() { return m_terrainRenderer; }
    SpriteSystem& spriteSystem() { return m_spriteSystem; }
    DebugLineRenderer& debugLineRenderer() { return m_debugLineRenderer; }
    OverlayLineRenderer& overlayLineRenderer() { return m_overlayLineRenderer; }
    // Desenho imediato de script (world.draw_strip / world.draw_sprite).
    ImmediateDraw& immediateDraw() { return m_immediateDraw; }
    bool& debugModelPivots() { return m_debugModelPivots; }
    PackManager& packManager() { return m_packManager; }
    float& fps() { return m_fps; }
    float& frameTime() { return m_frameTime; }
    int& visibleChunks() { return m_visibleChunks; }
    // AABB horizontal da malha de agua (Y vem do waterLevel no uso). Serve
    // para cortar o passe de agua pelo frustum da CAMERA no chamador - ver
    // Engine::render. Seguro: a agua nao entra no shadow map.
    void cacheWaterBounds(const struct WaterMesh& mesh);
    void buildShoreMask(const struct WaterMesh& mesh);
    void rebuildShoreMask();
    void rebuildShoreMaskAsync(); // na troca de mapa: monta em segundo plano
    void pollShoreMask();         // entrega a grade pronta (a cada quadro)
    void applyShoreMask(struct ShoreOut& r);
    Timer& timer() { return m_timer; }
    bool& showConsole() { return m_showConsole; }
    bool& showDebugOverlay() { return m_showDebugOverlay; }
    // Ferramentas de desenvolvimento no jogo (painéis F1..F12, console, câmera
    // livre): ligadas no build com editor; no jogo exportado só com ERUPTION_DEV=1.
    bool devTools() const { return m_devTools; }
    bool hasEditor() const { return m_editor != nullptr; }
    // HUD de exemplo (barras do personagem) e minimapa: o jogo exportado só
    // mostra se o projeto pedir (ui.show_hud / ui.show_minimap).
    void setHudBars(bool on) { m_showHudBars = on; m_showGameHud = m_showHudBars || m_showMinimapHud; }
    void setMinimapHud(bool on) { m_showMinimapHud = on; m_showGameHud = m_showHudBars || m_showMinimapHud; }
    // HUD do jogo numa área (Play do editor, dentro do painel Cena).
    void drawGameHudIn(ImDrawList* dl, const ImVec2& origin, const ImVec2& size) {
        if (m_showGameHud) drawGameHud(dl, origin, size);
    }
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
    // TEXTURAS EMBUTIDAS DOS ARQUIVOS DO MAPA (EmbeddedTextures.cpp): lista
    // por arquivo, chao do mapa primeiro; o clique abre a textura no cubo da
    // previa com o material que o mundo usa (cor, MRAH-W e normal dos slots
    // do mundo), os canais, e o relevo assado pela mesma rotina do mundo.
    // `tr` traduz a interface (nullptr = ingles); `file` = so' esse arquivo
    // (vazio = todos); `inlineCube` = desenha o cubo aqui (o editor) e nao so'
    // na janela "Texture Preview" da depuracao.
    using UiText = const char* (*)(const char*);
    void drawEmbeddedTextures(UiText tr, const std::string& file, bool inlineCube);
    void closeEmbeddedPreview();
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
    std::vector<Vec4> m_wetTrail; // chuva local: x,z,raio,forca (seca devagar)
    float m_groundWet = 0.0f;      // 0..1: encharca na chuva (~6 s), seca pelo clima (WeatherSystem::dryingRate)
    float m_wetForced = -1.0f;     // world.set_wetness: alvo 0..1; < 0 = segue o clima
    float m_wetForcedSeconds = 5.0f; // segundos de 0 a 1 ate' o alvo forcado
    struct WetArea { Vec2 xz; float radius, amount, dry, age; };
    std::vector<WetArea> m_wetAreas; // world.wet_area, secando
    const void* m_wetMap = nullptr;  // mapa das areas e da grade de cobertura
    uint64_t m_wetCoverSerial = 0;   // grade de cobertura so' a partir desta atualizacao (a do mapa de antes nao vale)
    void updateWetness(float dt);
    bool m_demoHidePanels = false; // esconde Shortcuts/Control Panel/minimapa (video)
    // Painel de toque (ERUPTION_TOUCH_HUD=<px>): hora e clima sem teclado, para
    // celular. Fonte propria no tamanho pedido; nullptr = desligado.
    ImFont* m_touchHudFont = nullptr;
    float m_touchHudDayScale = 1.0f; // velocidade do dia antes de pausar
    void drawTouchHud();
    void drawTouchPad();
    // input.touch_pad: o jogo mostra o controle de toque so' onde ele serve (a
    // exploracao); escondido, solta o analogico e as teclas virtuais.
    void setTouchPadVisible(bool on) { m_touchPadVisible = on; }
    bool m_touchPadVisible = true;
    bool m_touchHudTelemetry = false; // painel de numeros ligado pelo botao
    float m_controlPanelBottom = 0.0f; // base do "Control Panel" (dev tools) no quadro anterior
    bool m_touchHudMaps = false;      // lista de mapas aberta (ERUPTION_TOUCH_MAPS)
    // Modo classico: luz fixa do jeito antigo (so' difusa, sem relevo, sem
    // sombra, tesselacao, GI, bloom, DoF, luzes de ponto nem SSAO), meio-dia
    // e ceu limpo - para comparar com tudo ligado. Desligar restaura o estado
    // de antes.
    void setClassicMode(bool on);
    // CHAO MOLHADO. Automatico: encharca com a chuva (so' onde ela chega:
    // DeferredLighting::setRainCover) e seca pelo clima depois. target 0..1
    // fixa o molhado (seconds = de 0 a 1 ate' ele); target < 0 volta ao
    // automatico. No modo classico fica seco.
    void setWetness(float target, float seconds);
    float wetness() const { return m_groundWet; }
    // Molha um disco do chao (skill de agua, balde): amount 0..1, seca em
    // drySeconds. Coberto ou nao, com ou sem chuva; varios: vale o maior.
    void wetArea(const Vec3& pos, float radius, float amount, float drySeconds);
    // Luz do modo classico: a do proprio mapa (direcao e cor do sol, cor do
    // ambiente), cor x (ambiente + difusa x N.L).
    void applyClassicLight(LightingEnvironment& env) const;
    bool classicMode() const { return m_classic.on; }
    // Ceu (cupula do fundo) em tempo de execucao: o mesmo efeito de "sky": false
    // no .env. Vale ate trocar de mapa (a carga volta ao valor do .env).
    void setSkyEnabled(bool on) { m_mapHasSky = on; }
    bool skyEnabled() const { return m_mapHasSky; }
    // Modo classico ligado: o clima pedido (script ao carregar mapa) fica
    // guardado para a volta ao HD, e o classico continua limpo.
    void setClassicSavedWeather(WeatherType t) { m_classic.weatherType = static_cast<int>(t); }
    // Agua do modo classico: plano chato no nivel do mapa, quadros de uma
    // animacao trocados na velocidade do mapa e translucida, sem reflexo,
    // refracao, Fresnel nem espuma. `pattern` e' um caminho do jogo com dois
    // %d (tipo da agua do mapa, numero do quadro de 0 a count-1), p.ex.
    // "assets/external/agua/agua%d_%02d.png". Altura, velocidade e passo da
    // onda e a velocidade da animacao vem do plano d'agua do mapa. Sem
    // pedido (ou sem os arquivos) o modo classico mantem a agua normal.
    void setClassicWater(const std::string& pattern, int count, float alpha, float tile);
    struct ClassicSaved {
        bool on = false;
        bool usePbr = true, ssao = true;
        float normalMapScale = 1.0f, timeOfDay = 0.5f, timeScale = 1.0f;
        int weatherType = 0;
        bool tess = true, contact = true;
        bool effects[6] = {};
        bool cloudLayers = true, motionBlur = true, chromaticAb = true, waterReflection = false;
        int outlineMode = 0;
    } m_classic;
    struct ClassicWaterRequest {
        std::string pattern;
        int count = 0;
        float alpha = 0.6f, tile = 10.0f;
        std::string loadedKey; // padrao + tipo ja' carregados (vazio = nenhum)
    } m_classicWater;
    // Carrega os quadros da agua classica quando o pedido ou o tipo de agua
    // do mapa mudou (so' com o modo ligado). Chamado todo quadro.
    void syncClassicWater();
    std::string m_demoLabel;       // rotulo grande no rodape (ver runDemoTimeline: label=)
    float m_demoLabelUntil = -1.0f;// segundos do roteiro ate' quando o rotulo fica
    RenderEffect* renderEffect(size_t i) { return m_renderEffects[i].get(); }
    // Liga/desliga o DoF de vez: o render recopia enableDoF do efeito 3 a cada
    // quadro, entao escrever so' em postSettings() durava um quadro.
    void setDofEnabled(bool on);
    // game.restart() e' um boot: o que os scripts mudam fora da VM (visual,
    // foco, neblina, ver atraves, sombra dos sprites, mixagem) volta ao que
    // era quando os scripts nasceram. Captura no ScriptHost::init.
    void captureScriptDefaults();
    void restoreScriptDefaults();
    // Visao de equipe (API vision.*): fontes, zonas e a regra visible().
    Vision& vision() { return m_vision; }
    // Nevoa por script (world.set_fog): vence a do mapa (.env) e a do clima;
    // set=false devolve o comando para eles.
    void setScriptFog(const MapFog& fog) { m_scriptFog = fog; }
    // Borda de mundo por script (world.set_border): vence a "world_border" do
    // .env; set=false devolve o comando para ela (ou desliga, sem mapa).
    void setScriptBorder(const WorldBorder& b) { m_scriptBorder = b; }
    // Exposicao travada por script (world.set_exposure): vence o "exposure"
    // do .env; negativo devolve o comando a ele (ou a' automatica).
    void setScriptExposure(float value) { m_scriptExposure = value; }
    void setMinimapBottomRight(bool bottomRight) { m_minimapBottomRight = bottomRight; }
    bool minimapBottomRight() const { return m_minimapBottomRight; }
    // O minimapa (e o cursor do jogador nele) segue o #minimap/#minimap-info do
    // CSS: ligado por setMinimapBottomRight ou pela folha da HUD do projeto.
    bool minimapFollowsCss() const;
    // ui.minimap_label: um texto so' no #minimap-info, no lugar de
    // "mapa [x, z]" + relogio; vazio volta ao padrao.
    void setMinimapLabel(const std::string& text) { m_minimapLabel = text; }
    bool loadEruptSprV2(std::istream& f, const std::string& path, LoadedSprite& out, uint32_t version = 2);
    // `preCleaned`: os pixels ja' vem com corte de alfa e dilatacao (preload
    // de folha, characters.preload) - so' sobem.
    uint32_t createTextureFromSprImage(const SprImage& img, const uint8_t* palette, bool usePalette, std::vector<TextureResource>& outResources, bool swapRB = false, const std::string& sourcePath = "", bool preCleaned = false);
    // Envio em lote de uma folha de sprite: com m_spriteUploads posto, o
    // createTextureFromSprImage so' anota a copia, e flushSpriteUploads faz um
    // envio (uma espera da GPU) para a folha toda em vez de um por quadro.
    struct SpriteUpload {
        VkBuffer staging;
        VmaAllocation stagingAlloc;
        VkImage image;
        uint32_t width, height, mipLevels;
        VkFormat format;
    };
    std::vector<SpriteUpload>* m_spriteUploads = nullptr;
    void flushSpriteUploads(std::vector<SpriteUpload>& uploads);
    // characters.preload: le a folha .spr e prepara os quadros (corte de alfa
    // e dilatacao) num job; a carga seguinte dessa folha so' cria e envia.
    // Chave: o caminho do .spr resolvido no projeto. So' o fio principal mexe.
    struct PreparedSheet {
        std::vector<uint32_t> sizes;                // largura, altura por quadro
        std::vector<std::vector<uint8_t>> pixels;   // RGBA pronto por quadro
    };
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<PreparedSheet>>> m_spritePreloads;
    void preloadCharacterSprite(const std::string& png);
    bool characterSpritePreloaded(const std::string& png) const;
    bool loadSpriteFromPng(const std::string& path, LoadedSprite& out);
    // <folha>_normal.png / _normal.spr opcional ao lado (LoadedSprite::normalTextures).
    void loadSpriteNormals(const std::string& path, LoadedSprite& out);
    bool loadSpriteFromEruptSpr(const std::string& path, LoadedSprite& out);
    void cleanupSpriteTextures(LoadedSprite& ls);
    void calculateMaxCharHeight();
    void renderSpritePart(LoadedSprite& ls, uint32_t paletteSlot, int& frameIdx, float& timer, float dt, int& outCount, bool isBody = false);
    // Personagens da cena
    int addCharacter(const std::string& name, const std::string& sprite, const Vec3& position, float facingDeg);
    bool setCharacterSprite(int index, const std::string& sprite);
    int findCharacter(const std::string& name) const;
    // Tira o personagem da cena e devolve a folha de sprite dele se mais ninguem a usa
    // (GPU sem bindless: cada quadro ocupa uma camada de textura). Falso se nao existe.
    bool removeCharacter(const std::string& name);
    // Tira os personagens transitorios (previa do editor); devolve quantos.
    int removeTransientCharacters();
    // Solta uma folha da cache sem personagem usando; falso se ainda ha' quem a use
    // ou ela nao esta carregada.
    bool releaseCharacterSheet(const std::string& sprite);
    // Topo do sprite do personagem na tela, em pixels da interface (os de
    // ui.size), projetado com a camera e a inclinacao do billboard deste
    // quadro. Falso quando o personagem nao foi desenhado ou cai fora da tela.
    bool characterHeadScreen(int index, Vec2& out) const;
    // Raio da camera pelo pixel (x, y) da JANELA (o espaco de Input::mousePosition).
    // Falso sem camera/area valida. origin/dir em coordenadas de mundo, dir unitario.
    bool screenRay(float x, float y, Vec3& origin, Vec3& dir) const;
    // O chao (terreno ou topo de objeto solido, como floorHeight) sob o pixel;
    // falso se o raio nao o atinge em ate' `maxDist`. Marcha com passo que
    // cresce com a distancia e refina o cruzamento por bisseção.
    bool screenToGround(float x, float y, Vec3& out, float maxDist = 8000.0f) const;
    // Personagem desenhado cuja caixa na tela contem o pixel (janela); o mais
    // perto da camera em empate. -1 se nenhum.
    int characterAtScreen(float x, float y) const;
    // Elemento de UI preso ao topo de um personagem (ui.anchor); `dyPx` sobe.
    // Recalculado em drawGameFx2D, no mesmo quadro do render.
    void setUiAnchor(const std::string& selector, const std::string& character, float dyPx);
    void clearUiAnchor(const std::string& selector);
    // Retrato (ui.portrait): o elemento mostra o personagem de frente, no
    // recorte `crop` (x, y, w, h em fracao do primeiro quadro parado).
    void setUiPortrait(const std::string& selector, const std::string& character, const Vec4& crop);
    void clearUiPortrait(const std::string& selector);
    // Anda até `target` pelo caminho achado; `seconds` > 0 acerta a velocidade
    // para chegar nesse tempo (0: passo normal).
    void walkCharacter(int index, const Vec3& target, float seconds);
    // Põe o personagem em `position`. Com `walk`, o script move o boneco quadro
    // a quadro e o motor anima: o deslocamento liga o andar e dá o ritmo dos
    // passos; um salto de mais de 2 alturas de corpo é teleporte (não anima).
    void placeCharacter(int index, const Vec3& position, bool walk);
    // Caminho no chão de `from` a `to` (grade de caminhada + objetos sólidos),
    // já alisado; falso se não há caminho. `radius` cresce os sólidos;
    // `wallClearance` (> 0) mantém células e trechos a essa distância de
    // qualquer célula que não anda. Em camadas: o piso de cada célula é o
    // terreno ou o topo de um sólido logo abaixo de quem chega nela (ponte
    // sobre ravina: por cima entrando pela ponta, por baixo pela ravina).
    // `avoidLiquid` (kAvoidWater | kAvoidLava): célula com esse líquido acima
    // do piso não entra no caminho (o corpo ainda anda nela). As buscas de um
    // quadro dividem kPathFrameBudgetMs (Pathfinding.cpp): esgotado o tempo,
    // devolve o trecho até o ponto mais perto do alvo; sem tempo, falso.
    enum AvoidLiquid : uint8_t { kAvoidWater = 1, kAvoidLava = 2 };
    bool findPath(const Vec3& from, const Vec3& to, float radius, std::vector<Vec3>& out,
                  float wallClearance = 0.0f, uint8_t avoidLiquid = 0) const;
    mutable uint32_t m_pathBudgetFrame = ~0u;
    mutable double m_pathBudgetLeftMs = 0.0;
    // Um personagem (fora `ignore`) ocupa (x, z)?
    bool characterAt(const Vec3& p, float radius, int ignore = -1) const;
    // Balões de fala (personagens e jogador) sobre a área do jogo.
    void sayPlayer(const std::string& text, float seconds);
    // Toca uma animação extra do sprite (v3, "attack", "jump"...); vazio para.
    bool playPlayerAction(const std::string& name, bool loop);
    // seconds > 0: a animacao inteira dura isso (o atraso da folha estica ou
    // encolhe por igual); 0 = o ritmo da folha.
    bool playCharacterAction(int index, const std::string& name, bool loop, float seconds = 0.0f);
    const std::string& playerAction() const { return m_playerAction; }
    void followPlayer(int index, float distance);
    // Ver atraves (camera.see_through): o que fica entre a camera e o jogador,
    // num raio de `radius` unidades de mundo em volta dele na tela, sai em
    // dither ate' `opacity` no centro. radius 0 desliga. `atFocus`: o centro e'
    // o ponto de world.set_focus (o grupo), nao o jogador. `band` > 0: tambem
    // a faixa da tela abaixo do centro, ate' `band` u antes dele.
    // minHidden > 0: o corte so' liga quando essa fracao do sprite em foco esta'
    // escondida atras de modelos do mapa (liga em >= minHidden, desliga abaixo
    // de minHidden - 0,1) e entra/sai em ~0,2 s. 0 = sempre ligado.
    void setSeeThrough(float radius, float opacity, bool atFocus = false, float band = 0.0f, float minHidden = 0.0f);
    // Ponto de world.set_focus (nullptr: foco automatico de novo).
    void setFocusPoint(const Vec3* point);
    // Perto da camera (camera.near_fade): o que fica a menos de `start` da
    // camera some em dither e volta ate' `end` (padrao 1,5x start). 0 desliga.
    void setNearFade(float start, float end);
    // camera.collide: a orbita encurta quando o relevo (chao do mapa aberto e
    // dos vizinhos) fica entre o alvo e a camera, e volta devagar quando
    // libera. `margin` = folga em u entre a lente e o chao.
    void setCameraCollide(bool on, float margin);
    // camera.set_orbit_input: arrastar com o botao esquerdo/direito (o dedo
    // no mundo) gira a orbita do PlayerController. Desligado, o jogo faz o
    // proprio gesto com input.touches sem o motor girar no mesmo quadro.
    void setOrbitInput(bool on) { m_orbitInput = on; }
    bool orbitInput() const { return m_orbitInput; }
    void injectTestInput(); // ERUPTION_TEST_INPUT (Engine.cpp)
    // Recorte por volume (world.set_cutaway): malha do mapa e modelos dentro
    // do volume `slot` (0..FrameUBO::kCutaways-1) somem em dither ate'
    // `opacity`, com borda suave de `soft` unidades. shape 0 desliga, 1 caixa
    // (halfSize), 2 cilindro (halfSize.x = raio, .y = meia altura).
    void setCutaway(int slot, int shape, const Vec3& center, const Vec3& halfSize, float yawDeg, float soft,
                    float opacity);
    // Sombra dos sprites (characters.set_shadow): 0 = planar do sol, 1 =
    // disco, 2 = disco + planar das ate' 2 luzes pontuais dominantes; -1 = a
    // do data/char_config.json.
    void setSpriteSink(float pixels) { m_spriteSinkPixels = std::clamp(pixels, 0.0f, 64.0f); }
    // LUZ DE BORDA dos sprites do lado da luz (characters.set_rim): forca
    // (0 desliga, ~0,3-0,8 sutil) e tinta; o preset "sprite_rim" multiplica.
    void setSpriteRim(float strength, const Vec3& tint) {
        m_spriteRimStrength = std::clamp(strength, 0.0f, 2.0f);
        m_spriteRimTint = glm::clamp(tint, Vec3(0.0f), Vec3(1.0f));
    }
    // ANIMACAO EM DEGRAUS (characters.set_anim_fps): o quadro mostrado do
    // sprite so' muda N vezes por segundo, num relogio comum a todos (o
    // passo do mundo, da camera e das particulas segue liso). 0 = livre.
    // `key` vazio = global; senao o nome de um personagem ou o arquivo de
    // uma folha (.spr), que vencem o global; fps < 0 apaga a regra da chave.
    void setSpriteStepFps(const std::string& key, float fps);
    // PASSADA (characters.set_stride): distancia, em unidades, que um ciclo
    // completo de andar cobre. `key` vazio = padrao de todos; senao o nome de
    // um personagem (vence o padrao). units <= 0 volta ao ritmo por altura.
    void setWalkStride(const std::string& key, float units);
    // RITMO DE ANDAR (characters.set_walk_rate): cada quadro dura
    // action.delay x (ms por celula na velocidade atual) / refMs, com a celula
    // de `cellUnits` unidades. Vence a passada. cellUnits <= 0 desliga.
    void setWalkRate(const std::string& key, float cellUnits, float refMs);
    // PARTICULAS DE AMBIENTE (world.set_ambient): camadas presas ao foco.
    void setAmbientLayers(const std::vector<AmbientLayer>& layers) { m_ambient.setLayers(m_vfx, layers); }
    // Amplitude do relevo que o chao VISIVEL tem: o da tesselacao ou o da
    // malha assada no G-buffer (relief_bake "mesh"); 0 sem relevo na vista.
    float reliefInView() const {
        if (m_modelRenderer.tessellationActive()) return m_tessAmplitude;
        const ModelRenderer::ReliefBakeConfig& rb = m_modelRenderer.reliefBake();
        return rb.mode == 2 ? rb.amplitude : 0.0f;
    }
    // Folga de altura dos decais de chao (anel, vfx.area): o relevo tira o
    // chao visivel da altura do pe' em ate' a amplitude.
    float groundDecalSlack(float extent) const { return std::max(extent, 4.0f) + reliefInView(); }
    // Com relevo na vista, o decal e' lido no plano do chao SEM deslocamento
    // (inclinacao pela altura do piso na CPU): segue rampa e degrau do mapa
    // e ignora o relevo. Sem relevo, nada muda.
    void groundDecalPlane(GroundRing& decal) const;
    void setSpriteShadow(int kind, float contactSun = 0.0f) {
        m_spriteShadowKind = kind;
        m_spriteContactSun = std::clamp(contactSun, 0.0f, 1.0f);
    }
    // Juncao malha x terreno (world.set_ground_blend): faixa de altura acima do
    // chao em que a peca mistura com o material dele; 0 desliga. GroundBlend.cpp.
    // `fraction`: a faixa de cada peca e' no maximo essa fracao da altura dela.
    void setGroundBlend(float band, float fraction = 0.3f);
    // Luz pontual dominante de cada sprite visivel neste quadro, indice =
    // instancia de SpriteSystem::getSortedSprites() (a sombra por luz e a
    // contraluz leem daqui). So' luzes pontuais: o sol fica com quem o le' do FrameUBO.
    const std::vector<SpriteLight>& spriteDominantLights() const { return m_spriteLights[0]; }
    void patrolCharacter(int index, const std::vector<Vec3>& points, float waitSeconds);
    void stopCharacter(int index);
    void drawSpeechBubbles(float x, float y, float w, float h, float dt);
    // SUCO de apresentacao por personagem (SceneCharacter::Fx). Cada um decai
    // sozinho; chamar de novo reinicia. `index` do findCharacter.
    void flashCharacter(int index, const Vec3& color, float seconds);
    void shakeCharacter(int index, float amplitude, float seconds);
    // Investida: anda `distance` na direcao de `towards` e volta.
    void lungeCharacter(int index, const Vec3& towards, float distance, float seconds);
    void popCharacter(int index, float amount, float seconds);
    void scaleCharacter(int index, float scale);
    void fadeCharacter(int index, float to, float seconds);
    // Desenho 2D por cima da cena: numeros que sobem, clarao de
    // tela e a interface montada pelo script (ui2d). Vem DEPOIS dos baloes.
    void drawGameFx2D(float x, float y, float w, float h, float dt);
    // Interface 2D montada pelo jogo (renderer/UiCanvas.hpp).
    UiCanvas& uiCanvas() { return m_uiCanvas; }
    // Interface do jogo descrita em CSS (renderer/UiDocument.hpp).
    UiDocument& uiDocument() { return m_uiDocument; }
    // Tocado/clicado neste quadro (ui.clicked): um elemento do CSS que esta' na
    // tela, ou "minimap". Toque que comeca no controle de toque nao conta.
    bool uiClicked(const std::string& selector);
    // O mouse esta' sobre o retangulo desenhado de algum elemento que casa
    // com `selector` (ui.hovered); no celular so' com o dedo na tela.
    bool uiHovered(const std::string& selector);
    // Arrasto (ui.dragged): deslocamento do dedo/mouse neste quadro, em pixels
    // da interface, se o toque comecou dentro do elemento; 0,0 ate' passar da
    // folga, para um toque parado nao rolar nada.
    glm::vec2 uiDragged(const std::string& selector);
    // Segurar e soltar (ui.held / ui.released): segundos com o dedo (o mouse)
    // apertado num toque que comecou no elemento, e se soltou em cima dele
    // neste quadro. O "dedo" e' o botao que pousa: direito no celular (o
    // esquerdo de la' e' o toque curto sintetico), esquerdo no PC.
    float uiHeld(const std::string& selector);
    bool uiReleased(const std::string& selector);
    // Retangulo do elemento na tela, publicado como area tocavel (um dedo que
    // pousa nele e' da interface, nao do analogico). Falso se nao esta' na tela.
    bool uiTouchRect(const std::string& selector, CSSRect& r);
    // Ponto `p` toca o elemento: dentro de `r` e fora do selo dele, que tem
    // toque proprio ("#id:badge").
    bool uiHit(const std::string& selector, const CSSRect& r, const glm::vec2& p) const;
    // No do grafo do elemento tocado neste quadro (ui.graph_clicked), com
    // area de toque de pelo menos 48 dp por no; vazio se nenhum.
    std::string uiGraphClicked(const std::string& selector);
    // No do grafo onde comecou o toque que segue apertado ha' pelo menos
    // `seconds` (ui.graph_held); vazio se nenhum.
    std::string uiGraphHeld(const std::string& selector, float seconds);
    struct UiHold { glm::vec2 start{0.0f}, end{0.0f}; double t0 = 0.0, t1 = 0.0; bool active = false, released = false; int frame = -1; };
    UiHold m_uiHold;
    // Relogio da interface: soma o passo sem escala de tempo (o real com o
    // mundo em pausa), entao um toque longo dura o mesmo com ERUPTION_TEST_FIXED_DT
    // em qualquer maquina, e nao encolhe em camera lenta.
    double m_uiClock = 0.0;
    void updateUiHold();
    glm::vec2 uiCursor() const;
    struct UiDrag { glm::vec2 start{0.0f}, last{0.0f}, delta{0.0f}; bool active = false, moving = false; int frame = -1; };
    UiDrag m_uiDrag;
    Vec4 m_uiArea{0.0f};        // x, y, w, h da ultima interface desenhada (drawGameFx2D)
    struct UiAnchor { std::string selector, character; float dyPx = 0.0f; };
    std::vector<UiAnchor> m_uiAnchors;
    // Retrato: relogio proprio do parado; `ref` = caixa do primeiro quadro
    // parado em pixels (x0, y0, x1, y1, y para baixo), da folha `refSprite`.
    struct UiPortrait {
        std::string selector, character, refSprite;
        Vec4 crop{0.0f, 0.0f, 1.0f, 1.0f};
        Vec4 ref{0.0f};
        int frame = 0;
        float timer = 0.0f;
    };
    std::vector<UiPortrait> m_uiPortraits;
    // Tempo de jogo desde o ultimo desenho da interface (updateCharacters
    // soma): o parado do retrato anda no relogio dos personagens, nao no do
    // ImGui (tempo real, sem o passo fixo nem a pausa).
    float m_portraitDt = 0.0f;
    void updateUiPortraits();
    // Quadro de sprite (textura de personagem) como textura do ImGui, criada
    // na primeira vez que um retrato a mostra; sai com a folha
    // (cleanupSpriteTextures).
    std::unordered_map<VkImageView, VkDescriptorSet> m_uiSpriteSets;
    void* uiSpriteTexture(const TextureResource& tex);
    Vec4 m_minimapScreen{0.0f}; // x0, y0, x1, y1 do minimapa no ultimo quadro em que apareceu
    int m_minimapScreenFrame = -100;
    // Textura de um PNG para a UI em CSS, carregada na primeira vez e
    // guardada por caminho. Devolve o handle do ImGui, ou nullptr.
    void* uiImageTexture(const std::string& png);
    void clearCharacters(bool keepNeighbors = false);
    // Anda quem tem caminho (só com o jogo rodando) e envia os sprites.
    void updateCharacters(float dt, bool moveAlongPaths);
    // vfx.area: indicador no chao por id do script (nullptr tira).
    void setGroundArea(const std::string& id, const GroundRing* area) {
        for (auto it = m_groundAreas.begin(); it != m_groundAreas.end(); ++it) {
            if (it->first != id) continue;
            if (area) it->second = *area;
            else m_groundAreas.erase(it);
            return;
        }
        if (area) m_groundAreas.emplace_back(id, *area);
    }
    void clearGroundAreas() { m_groundAreas.clear(); }
    std::vector<SceneCharacter>& characters() { return m_characters; }
    // Objetos da cena (hierarquia e componentes sobre modelos e personagens).
    SceneObjects& sceneObjects() { return m_sceneObjects; }
    bool hideCharactersInView = false; // vista do editor
    // Visualizador de modelo do editor: malhas do .glb sem instância na cena.
    bool loadModelForPreview(const std::string& glbPath);
    // Desenho fora da tela do editor (visualizador), no frame, antes da interface.
    std::function<void(VkCommandBuffer)> offscreenHook;
    bool hideMapEffectsInView = false; // vista do editor isolada: sem fumaça do mapa
    // Vista de detalhe total (editor, Exibir > Detalhe total): toda malha na
    // forma original, sem LOD, na tela e na sombra. Para conferir o LOD.
    void setFullDetailView(bool on) {
        m_fullDetailView = on;
        m_modelRenderer.setShadowFullDetail(on);
    }
    bool fullDetailView() const { return m_fullDetailView; }
    bool m_fullDetailView = false;
    // Retoque de terreno (core/TerrainEditing.cpp): grade de altura do mapa
    // (.ter) + delta salvo em assets/scenes/<mapa>.height.bin.
    struct HeightEdit {
        bool active = false;
        uint32_t w = 0, d = 0;
        float scale = 8.0f, offX = 0.0f, offZ = 0.0f;
        std::vector<float> base, delta;
        struct Ref { uint32_t a, b; }; // GPU: (malha, vértice); CPU: (nó, vértice)
        std::vector<uint32_t> gpuStart, cpuStart;
        std::vector<Ref> gpuRefs, cpuRefs;
        std::string assetPath;
        std::string mapName; // mapa dono da grade (os arquivos de retoque vêm dele)
        std::unordered_map<uint32_t, Mat4> meshTransform;
        bool refsBuilt = false; // gpu/cpuRefs montados (ensureHeightEditRefs)
        float height(uint32_t g) const { return base[g] + delta[g]; }
    };
    enum class SculptMode { Raise, Lower, Smooth, Flatten };
    bool terrainEditable() const;
    const HeightEdit& heightEdit() const { return m_heightEdit; }
    // Um passo do pincel; `rect` recebe a região da grade tocada (x0, z0, x1, z1).
    bool sculptTerrain(SculptMode mode, const Vec2& center, float radius, float strength, float falloff,
                       float flattenTo, float dt, int rect[4]);
    std::vector<float> heightDeltaRegion(int x0, int z0, int x1, int z1) const;
    void setHeightDeltaRegion(int x0, int z0, int x1, int z1, const std::vector<float>& values);
    bool saveHeightDelta(std::string& error) const;

    // Pintura de camadas (core/TerrainPaint.cpp): 8 camadas em 2 máscaras RGBA,
    // um texel por ponto da grade.
    struct SplatEdit {
        bool active = false, fromMap = false, painted = false;
        uint32_t w = 0, d = 0;
        std::vector<uint8_t> mask[2];
        std::vector<float> weights; // 8 por ponto (não perde passos pequenos no arredondamento)
        TextureResource gpu[2];
        uint32_t slot[2] = {0, 0};
        bool ownSlot[2] = {false, false};
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t layerSlot[8] = {};
        std::string layerNames[8];
        std::string layerFiles[8]; // camadas escolhidas no editor (mapas sem splat próprio)
        TextureResource layerTex[8];
        TextureResource layerNormal[8]; // <camada>_normal (slot da cor + 1)
        TextureResource layerMrahw[8];  // <camada>_mrahw (slot da cor + 2)
        int dirty[2][4] = {{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN}, {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN}};
    };
    const SplatEdit& splatEdit() const { return m_splatEdit; }
    bool beginSplatEditing();
    int splatLayerCount() const;
    bool addSplatLayer(const std::string& png);
    // Decodifica em paralelo as imagens das camadas (cor, _normal, _mrahw) que
    // o addSplatLayer vai pedir em seguida (as da cena chegam juntas).
    void prefetchSplatLayers(const std::vector<std::string>& layers);
    // O mesmo preparo, sem esperar: um job por imagem (world.preload_map, com
    // as camadas da cena do mapa seguinte). A chegada pula o que ja' veio.
    void preloadSplatLayers(const std::vector<std::string>& layers);
    // layer -1 = base do material; `erase` tira a camada.
    bool paintSplat(int layer, const Vec2& center, float radius, float strength, float falloff, float dt, bool erase,
                    int rect[4]);
    std::vector<float> splatRegion(int x0, int z0, int x1, int z1) const;
    void setSplatRegion(int x0, int z0, int x1, int z1, const std::vector<float>& values);
    bool saveSplat(std::string& error) const;
    std::vector<std::string> splatLayerFiles() const;
    static bool hasSavedSplat(const std::string& map);

    // Nível da água do mapa (lagos: cavar o terreno abaixo dele). Refaz o plano
    // de água na hora (espera a GPU: ação de editor, não de todo frame).
    void setWaterLevel(bool enabled, float level);
    bool waterEnabled() const { return m_waterOn; }
    float waterLevel() const { return m_mapBaseWaterLevel; }

    // Grade de caminhada (core/WalkGrid.cpp).
    bool navFollowsTerrain() const;
    bool navCell(float x, float z, int& gx, int& gz) const;
    bool navWalkable(float x, float z, float y = kAnyHeight) const;
    // Regras de terreno (core/WalkGrid.cpp): ligadas, a grade pintada (.nav,
    // .walk.png) deixa de valer e cada célula dentro dos mapas em cena anda,
    // salvo a íngreme: mais que maxStep de desnível até meia célula, ou
    // inclinação acima de maxSlopeDeg. Líquido e objetos seguem nas consultas
    // do chão (perigo e água pelas capacidades). Vale para os mapas seguintes.
    struct NavRules {
        bool on = false;
        float maxStep = 4.0f;
        float maxSlopeDeg = 45.0f;
    };
    void setNavRules(const NavRules& rules);
    const NavRules& navRules() const { return m_navRules; }
    // Célula sob (x, z): 0 fora de todo mapa, 1 anda, 2 íngreme (regras), 3
    // bloqueada pela grade pintada ou pelo custo.
    int navCellKind(float x, float z, float y = kAnyHeight) const;
    // Passo e centro de uma celula da grade de caminhada de `map`, em mundo
    // (a busca de caminho anda nas mesmas celulas).
    float navCellFrame(const LoadedMap* map, Vec2& origin) const;
    bool paintWalk(const Vec2& center, float radius, bool block, int rect[4]);
    std::vector<uint8_t> walkRegion(int x0, int z0, int x1, int z1) const;
    void setWalkRegion(int x0, int z0, int x1, int z1, const std::vector<uint8_t>& blocked);
    int blockSteepSlopes(float maxDegrees);
    bool saveWalk(std::string& error) const;
    // Custo de andar nas células a até `radius` de (x, z) (0 = só a célula do
    // ponto), só nesta partida: 1 = normal, infinito = bloqueada. Vale no
    // mesmo quadro para o andar, navWalkable e findPath. Devolve quantas
    // células mudaram.
    int setWalkCost(float x, float z, float radius, float cost);
    float walkCost(float x, float z) const;

    // Áreas de gatilho e objetos sólidos (core/Areas.cpp).
    // Área: caixa nomeada (girada em Y) que os scripts e as regras observam
    // (on.enter / on.leave / player.inside).
    struct TriggerArea {
        std::string name;
        Vec3 center{0.0f};
        Vec3 size{40.0f, 30.0f, 40.0f};
        float yaw = 0.0f; // graus
        bool removed = false;      // apagada no editor (desfazer traz de volta)
        bool fromNeighbor = false; // de um mapa vizinho
        bool parked = false;       // o vizinho dela saiu de cena
        Mat4 matrix() const;       // posição * giro * tamanho (o gizmo do editor mexe nela)
        void setMatrix(const Mat4& m);
        bool contains(const Vec3& p) const;
    };
    std::vector<TriggerArea>& areas() { return m_areas; }
    // Faixas por spline (core/Ribbons.cpp): estrada, trilha, rio raso. As da
    // cena vêm do .scene.luau (ribbons = {...}); as do script, de world.ribbon.
    // Assentadas no chão quando entram; a consulta (Ribbon::query) é pura.
    struct RibbonDef {
        std::string name;
        std::vector<float> pointsXZ; // pares (x, z)
        std::vector<float> widths;   // um por ponto, ou um só
        bool closed = false;
        float step = 4.0f, uvLength = 32.0f, lift = 0.1f;
    };
    struct SceneRibbon {
        RibbonDef def;
        Ribbon ribbon;
        bool fromScript = false; // world.ribbon: não vai para a cena salva
    };
    bool addRibbon(const RibbonDef& def, bool fromScript, std::string& error);
    const Ribbon* findRibbon(const std::string& name) const;
    void clearRibbons();
    const std::vector<SceneRibbon>& ribbons() const { return m_ribbons; }
    // Emissor de som: um som em laço num ponto (cachoeira, fogueira, taverna).
    // Toca com o jogo rodando, só perto do ouvinte (alcance).
    struct SoundEmitter {
        std::string name, file;
        Vec3 position{0.0f};
        float volume = 1.0f, range = 200.0f;
        bool removed = false, fromNeighbor = false, parked = false;
        uint32_t playing = 0; // som tocando agora (AudioSystem)
    };
    std::vector<SoundEmitter>& soundEmitters() { return m_soundEmitters; }
    int addSoundEmitter(const std::string& name, const std::string& file, const Vec3& position, float volume, float range);
    int findSoundEmitter(const std::string& name) const;
    void clearSoundEmitters(bool keepNeighbors = false);
    // Ligado no jogo e no Play do editor; desligado, os emissores param.
    void setSoundEmittersActive(bool on) { m_soundEmittersActive = on; }
    int addArea(const std::string& name, const Vec3& center, const Vec3& size, float yawDeg);
    int findArea(const std::string& name) const;
    void clearAreas(bool keepNeighbors = false);
    // Objeto sólido: o jogador não atravessa o pé dele (a parte de baixo da malha).
    void setSolid(uint32_t instance, bool solid);
    bool isSolid(uint32_t instance) const;
    // Tipo de superfície de um objeto (core/Areas.cpp). Floor = o sólido de
    // sempre: o topo é piso e o que passa de um degrau acima do piso é parede.
    // Hazard: piso que só quem tem kMoveProtected pisa (ou o empurrão, que o
    // jogo trata). Blocked e Wall: o pé inteiro não se pisa, em qualquer altura
    // (Wall ainda diz ao jogo que barra projétil).
    enum SurfaceKind : uint8_t { kSurfaceNone = 0, kSurfaceFloor, kSurfaceHazard, kSurfaceBlocked, kSurfaceWall };
    void setSurface(uint32_t instance, SurfaceKind kind);
    SurfaceKind surfaceOf(uint32_t instance) const;
    static const char* surfaceName(SurfaceKind kind);
    static bool parseSurface(const std::string& name, SurfaceKind& out);
    // Capacidades de quem anda: entrar em água funda (swim) e em perigo
    // (lava, superfície Hazard: protected).
    enum MoveCaps : uint8_t { kMoveSwim = 1, kMoveProtected = 2 };
    // Sólido pelo ARQUIVO do modelo (o tipo: toda ponte feita do mesmo .glb):
    // vale para as instâncias do mapa aberto, dos vizinhos que entram e dos
    // mapas carregados depois, até ser desligado.
    void setSolidFile(const std::string& file, bool solid);
    void setSurfaceFile(const std::string& file, SurfaceKind kind);
    // Reaplica os arquivos sólidos às instâncias de agora (mapa novo, vizinho).
    void applySolidFiles();
    // Algum objeto sólido ocupa (x, z) na altura de p (raio do corpo em volta)?
    bool blockedAt(const Vec3& p, float radius) const;
    // (x, z) dentro do pé do objeto (core/Areas.cpp), crescido pelo raio.
    bool footprintContains(uint32_t instance, const Vec2& p, float radius) const;

    // Consultas do chão para o jogo (core/GroundQuery.cpp). O piso é o
    // terreno ou o topo de um objeto sólido (passarela, ponte, laje de kit):
    // floorHeight devolve o mais alto abaixo de `fromY`.
    float floorHeight(float x, float z, float fromY, bool* onModel = nullptr, int* instance = nullptr,
                      bool withRelief = true, bool withIce = true) const;
    // Dá para ficar de pé em (x, z) sobre o piso: grade de caminhada (só no
    // terreno; sobre um objeto sólido vale a peça), nenhum outro sólido
    // ocupando o corpo acima do piso, no raio, e no máximo um degrau de
    // líquido por cima. Só conta objeto marcado sólido (scene.luau `solid`,
    // models.set_solid): peça do .env sem a marca não é piso.
    bool floorWalkable(float x, float z, float radius, float fromY, uint8_t caps = 0) const;
    struct LiquidAt {
        int index = -1;  // em map->liquids (-1 = o plano de água global)
        int kind = 0;    // 0 água, 1 lava
        float level = 0.0f;
        const LoadedMap* map = nullptr; // o mapa aberto ou o vizinho dono da lava
    };
    // Líquido aparente em (x, z): dentro da margem e acima do piso.
    bool liquidAt(float x, float z, LiquidAt& out) const;
    // Nível de uma lava do mapa (índice em currentMap()->liquids, ou nos de
    // `map`, um vizinho em cena, com o nível no mundo) durante o jogo: a
    // superfície sobe no shader (a malha fica) e liquidAt/sampleGround usam o
    // nível novo no mesmo quadro. Vale para lava e para água de contorno
    // (polygon); falso para o plano de água do mapa ou índice inválido.
    bool setLiquidLevel(int index, float level, LoadedMap* map = nullptr);
    // Wall: célula íngreme (regras de terreno) ou corpo barrado por sólido;
    // Blocked: fora da grade, célula pintada ou objeto Blocked; Hazard: piso
    // num objeto Hazard.
    enum GroundFlag : uint8_t {
        kGroundWalkable = 1, kGroundWater = 2, kGroundLava = 4, kGroundOnModel = 8,
        kGroundWall = 16, kGroundBlocked = 32, kGroundHazard = 64
    };
    // Grade nx x nz a partir de (x0, z0) com passo `step`, linha a linha (x
    // varia mais rápido): altura do piso e GroundFlag por ponto. Uma chamada
    // no lugar de milhares de floorHeight/floorWalkable/liquidAt.
    void sampleGround(float x0, float z0, float step, int nx, int nz, float radius,
                      float* heights, uint8_t* flags) const;
    bool bodyBlocked(const std::vector<uint32_t>& cand, int floorInstance, float floorY, float x, float z,
                     float radius) const;
    // O sólido `instance` tem, em todo ponto dele sob o corpo, o topo a no
    // máximo um degrau acima do piso (a peça vizinha da mesma ponte ou laje)?
    bool solidLowUnderBody(uint32_t instance, float floorY, float x, float z, float radius) const;
    // Piso em (x, z) abaixo de fromY entre os sólidos `cand` (o terreno é
    // terrainY); `instance` = o sólido do piso ou -1.
    float floorAmong(const std::vector<uint32_t>& cand, float terrainY, float x, float z, float fromY,
                     int& instance, bool withRelief = true) const;
    // O liquido sem o gelo (o lago congelado deixa de ser liquido em liquidOver).
    bool liquidOverRaw(float x, float z, float floorY, LiquidAt& out) const;
    bool liquidOver(float x, float z, float floorY, LiquidAt& out) const;
    bool navWalkableBase(float x, float z, float y) const;
    bool iceWalkable(float x, float z) const;
    // Agua de contorno (kind water com polygon) do mapa aberto ou de um vizinho
    // em cena em (x, z) com o piso em floorY abaixo da superficie: a de
    // superficie mais alta. `surface` = altura dela ali, com o nivel vivo.
    bool contourWaterAt(float x, float z, float floorY, LiquidAt& out) const;
    bool walkableOver(const std::vector<uint32_t>& cand, int floorInstance, float floorY, float x, float z,
                      float radius, uint8_t caps = 0) const;

    // Mundo contínuo (core/World.cpp): mapas emendados ao aberto, declarados no
    // scene.luau (neighbors = {{map = "b", offset = {x, y, z}}}), entram em
    // segundo plano quando o jogador (ou a câmera do editor) chega perto e saem
    // de cena quando ele se afasta. Sem tela de carregamento.
    using WorldNeighbor = SceneEdits::Neighbor;
    // Vizinhos declarados pelo mapa aberto (o editor grava no scene.luau).
    const std::vector<WorldNeighbor>& worldNeighbors() const { return m_worldNeighbors; }
    void setWorldNeighbors(std::vector<WorldNeighbor> neighbors);
    static void warmWorldGraphCache(); // vizinhos de todas as cenas, para o warp (World.cpp)
    // Monta o mundo a partir do mapa aberto (depois de a cena dele estar aplicada).
    void startWorld();
    // Esconde os vizinhos e monta de novo (o arquivo de cena mudou).
    void restartWorld();
    struct WorldRegionInfo {
        std::string map;
        Vec3 offset{0.0f};
        Vec2 boundsMin{0.0f}, boundsMax{0.0f};
        const char* state = "";
    };
    std::vector<WorldRegionInfo> worldRegions() const;
    bool isRegionInstance(uint32_t index) const;
    // Líquidos dos vizinhos em cena, já no lugar do mundo (centro, nível e
    // margem girada): map->liquids[index].
    struct WorldLiquid { const LoadedMap* map; uint32_t index; };
    std::vector<WorldLiquid> worldLiquids() const;
    // Mapa (aberto ou vizinho em cena) que tem chão em (x, z). Com `y`, entre
    // mapas empilhados (um andar embaixo de outro mapa) vale o chão mais alto
    // até um degrau acima de y; sem chão abaixo, o mais perto acima. Sem `y`,
    // o mapa aberto primeiro.
    const LoadedMap* mapAt(float x, float z, float y = kAnyHeight) const;
    // Todos os mapas em cena com chão em (x, z), até `max` (emendas e
    // empilhados); devolve quantos.
    int mapsAt(float x, float z, const LoadedMap** out, int max) const;
    bool seamWalkable(const LoadedMap* map, float x, float z) const;
    // Área com chão de todos os mapas em cena (limite da câmera).
    bool worldBounds(Vec2& outMin, Vec2& outMax) const;
    // Lado e área de um mapa pela grade de altura (.ter); falso se não tem.
    static bool mapGroundArea(const std::string& map, Vec2& outMin, Vec2& outMax);
    static bool readMapGroundArea(const std::string& map, Vec2& outMin, Vec2& outMax); // sem cache
    static void setWorldCacheTrusted(bool trusted); // chegada de warp: cache do mundo sem conferir o disco
    float worldLoadDistance = 300.0f; // distância da borda em que o vizinho entra
    float worldReleaseSeconds = 30.0f; // fora de cena por tanto tempo: memória devolvida
    uint32_t worldVersion() const { return m_worldVersion; } // muda quando um vizinho entra/sai

    // Objetos espalhados pelo pincel (core/Scatter.cpp), por modelo.
    struct ScatterGroup {
        std::string file, node, prefab;
        uint32_t templateFirst = 0;  // molde desligado de onde saem as cópias
        std::vector<Mat4> local;     // cada nó do modelo relativo à raiz
        struct Item { uint32_t first; bool removed; };
        std::vector<Item> items;
    };
    std::vector<ScatterGroup>& scatterGroups() { return m_scatter; }
    int scatterGroup(const std::string& file, const std::string& node, const std::string& prefab);
    std::vector<uint32_t> scatterAdd(int group, const std::vector<Mat4>& roots);
    void scatterSetRemoved(int group, const std::vector<uint32_t>& items, bool removed);
    Mat4 scatterRoot(int group, uint32_t item) const;
    bool isScatterInstance(uint32_t index) const;
    // `place`: deslocamento do mapa no mundo (vizinhos do mundo contínuo).
    int loadScatter(const std::string& map, const Mat4& place = Mat4(1.0f));
    // Arquivos .glb (sem repetir, sem prefab) que o scatter do mapa espalha: o
    // preload do destino os prepara fora do fio principal. Nao cria instancia.
    std::vector<std::string> scatterFiles(const std::string& map) const;
    bool saveScatter(std::string& error) const;
    void clearScatter();

    // Início do jogador vindo do scene.luau: leva o jogador (e a câmera) até lá.
    void setPlayerStart(const Vec3& p);
    bool hasPlayerStart() const { return m_hasPlayerStart; }
    const Vec3& playerStart() const { return m_playerStart; }
    void clearPlayerStart() { m_hasPlayerStart = false; }
    // Só muda o ponto (editor arrastando), sem levar jogador nem câmera.
    void movePlayerStart(const Vec3& p) { m_hasPlayerStart = true; m_playerStart = p; }
    float computeObstruction() const;
    float computeObstructionUncached() const;
private:
    void generateMipmaps(VkCommandBuffer cmd, VkImage image, int w, int h, uint32_t mipLevels, VkFormat format, uint32_t alphaMode);
    float advanceFrame();
    void populateMaps();
    void flushPendingUploads();
    // GPU sem bindless: libera as imagens de origem das texturas de modelo que o
    // array de camadas ja' copiou (Textures.cpp). Contadores na telemetria.
    void releaseCopiedTextureSources();
    // `contour` != nullptr: os ajustes de uma agua de contorno (MapLiquid::look e a
    // beira `contourShore`, 0..2) por cima dos do menu; o nivel o chamador poe.
    WaterRenderer::WaterSettings waterDrawSettings(const LoadedMap::MapLiquid* contour = nullptr,
                                                   int contourShore = 0) const;
    uint32_t m_uploadGuardFlushes = 0; // envios adiantados por setBeforeCopies (ver Engine::init)
    uint32_t m_texSourcesReleased = 0;
    double m_texSourcesReleasedMb = 0.0;
    // Quadros de sprite (so' GPU sem bindless): a imagem de origem de cada slot,
    // ate' a copia para a camada. A folha decide quais quadros o retrato
    // (ui.portrait) ainda le' direto (decideSpriteSources); os outros perdem a
    // origem depois da copia. m_releasedSpriteSlots: slots cuja imagem ja' foi
    // destruida (a destruicao da folha nao pode destruir de novo).
    struct SpriteSource {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool decided = false, keep = true, copied = false;
    };
    std::unordered_map<uint32_t, SpriteSource> m_spriteSources;
    std::unordered_set<uint32_t> m_releasedSpriteSlots;
    void decideSpriteSources(const LoadedSprite& ls);
    void destroySpriteSource(uint32_t slot, const SpriteSource& s);
    void render();
    // Calibra a escala da cena para target_fps (graphics.json) nos primeiros
    // segundos de cada mapa: uns poucos ajustes, sem oscilar durante o jogo.
    void settleRenderScale();
    float tessZoomFollow() const; // escala da faixa de tesselacao pela lente (Render.cpp)
    float tessAxis() const;       // eixo X da curva de tesselacao em u de mundo (Render.cpp)
    void renderImGui();
    void drawLightingControls();
    uint32_t resolveModelTexture(const std::string& path);
    uint32_t resolveTerrainTexture(const std::string& path);
    ModelMeshGPU resolveModelMesh(const std::string& legacyModelPath, uint32_t nodeIndex,
                                  const std::vector<TerrainVertex>& vertices,
                                  const std::vector<uint32_t>& indices);
    void initImGui();
#if defined(__EMSCRIPTEN__)
    // Caminho WebGL2 (EngineWeb.cpp): sem Vulkan no navegador.
    void renderWeb();
    void initImGuiWebBackend();
    void* webUiTexture(const std::string& path); // GLuint como ImTextureID; nulo se nao abriu
    void renderWebScene();                         // EngineWebScene.cpp
    void renderWebPost(struct WebSceneGl& s, const Mat4& viewProj, const LightingEnvironment& env, unsigned screenFb,
                       bool skyInHdr);
    void uploadWebLights(unsigned program, const Vec3& eye);
    void updateWebSkyProbe(struct WebSceneGl& s);
    void bakeWebProbes(struct WebSceneGl& s);
    void drawWebWater(struct WebSceneGl& s, const Mat4& view, int width, int height);
    bool drawWebShadows(struct WebSceneGl& s, const struct LightingEnvironment& env, const int (&viewport)[4], unsigned hdrFbo);
    struct WebSceneGl* m_webScene = nullptr;       // estado GL da cena (vive com o motor estatico)
    bool m_webReady = false; // Module.onReady() ja foi chamado (1o quadro desenhado)
#endif
    void shutdownImGui();
    void onWindowResize(int width, int height);
    void renderMinimap(VkCommandBuffer cmd);
    // Reflexo espelhado da agua HD-2D (src/core/WaterReflection.cpp): a cena
    // pela camera espelhada no plano d'agua, antes do G-buffer principal.
    void renderWaterReflection(VkCommandBuffer cmd);
    void drawWaterReflectionSprites(VkCommandBuffer cmd, const Mat4& drawVp);
    // As nuvens volumetricas pela mesma camera espelhada (WaterReflection.cpp).
    VkImageView renderWaterReflectionClouds(VkCommandBuffer cmd, const Mat4& depthVp, const Mat4& rasterVp,
                                            const Mat4& cullVp, const Vec3& mirroredCam);
    bool waterInViewThisFrame() const;
    // Raster da visao na janela da camera e upload; fora de passe de render.
    void updateVision(VkCommandBuffer cmd);
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
    // A barra anda enquanto o driver compila shaders (vkc::PipelineHookScope):
    // cada pipeline conta, e a tela é redesenhada a cada 200 ms.
    static void onPipelineCompiled(void* engine);
    void showShaderProgress();
    std::string m_loadingMap;
    std::string m_loadingTask;
    float m_loadingProgress = 0.0f;
    bool m_loadingClear = false;
    bool m_drawingShaderProgress = false;
    uint32_t m_shadersCompiled = 0;
    std::chrono::steady_clock::time_point m_lastShaderProgress{};
    void renderResourceManager();
public:
    bool createTextureFromFile(const char* path, TextureResource& out);
    bool createTextureFromPixels(const unsigned char* pixels, int w, int h, TextureResource& out);

    // REGIOES POR TEXTURA (formats/TextureRegions.hpp, docs/regioes_de_textura.md).
    // O motor monta a emissiva das imagens do mapa a partir das regioes do
    // arquivo da cena, por cima do que veio do pacote: sem regioes o mapa carrega
    // exatamente como antes. RegionEmissive.cpp.
    std::vector<texregion::ImageRegions>& textureRegions() { return m_texRegions; }
    std::vector<std::string>& emissiveOffNodes() { return m_emissiveOffNodes; }
    // Le as regioes da cena do mapa e troca, nos nos ja convertidos, o nome da
    // textura emissiva das imagens com regiao (antes de subir para a GPU).
    void loadRegionsForMap(const std::string& map);
    void patchRegionEmissive(ModelAsset& model);
    // Imagens de cor usadas pelo mapa e nos com malha (para a lista do editor).
    const std::vector<std::string>& regionImages() const { return m_regionImageNames; }
    const std::vector<std::string>& regionNodes() const { return m_regionNodeNames; }
    // A emissiva da imagem passa por uma textura do motor (da' para refazer
    // na hora)? Falso: o mapa precisa ser recarregado para a regiao valer.
    bool regionLive(const std::string& image) const;
    // Pixels do albedo da imagem (RGBA/RGB), com cache ate' trocar de mapa.
    bool regionAlbedo(const std::string& image, const uint8_t*& px, int& w, int& h, int& ch);
    // Refaz a mascara da imagem e sobe so' essa textura (sem recarregar o mapa).
    void refreshRegionEmissive(const std::string& image);
    // Hora do dia (0..24) que a mascara usa para os horarios das regioes.
    float regionHour() const { return m_dayNightCycle.timeOfDay() * 24.0f; }
    // Tipo de superficie ("sand", "snow", ...) da regiao sob o ponto (UV 0..1)
    // da imagem; "" fora de qualquer regiao de superficie. E' o dado que o passo
    // (characters.on_step, ground.surface) entrega ao jogo.
    std::string regionSurfaceAt(const std::string& image, const Vec2& uv);
    // Fim da carga das regioes (depois de patchRegionEmissive em todas as
    // malhas): acha os postes e, fora do editor, larga o que so' a carga usava.
    void finishRegionLoad();
    // Liga/desliga a emissiva de um no' (editor: vale na hora, sem recarregar).
    void setRegionNodeEmissive(const std::string& node, bool on);
    // Cintilacao da emissiva e brilho dos objetos no UBO do quadro.
    void fillEmissiveFrame(FrameUBO& ubo);
private:
    struct RegionLampSite { Vec3 pos{0.0f}; std::string image; int region = 0; };
    // Triangulo de uma superficie com a imagem: centro do UV (0..1) e do mundo.
    // O editor guarda os de todas as imagens (o ModelAsset e' liberado depois
    // da carga), para a regiao "lamp" nova achar os postes na hora.
    struct RegionTri { Vec2 uv; Vec3 pos; };
    void gatherRegionTris(const ModelNode& node);
    void rebuildRegionLampSites(const std::string& onlyImage);
    void syncRegionLamps();
    void updateEmissiveFlicker();
    void rebindRegionAliases(const std::string& image);
    VkImageView regionBlackView();
    std::unordered_map<std::string, std::vector<RegionTri>> m_regionTris;
    std::unordered_map<std::string, uint64_t> m_regionLampSig;
    size_t m_regionTriCount = 0;
    // Editor: cada (imagem, no') tem um slot proprio que aponta para a mesma
    // imagem emissiva ou para uma preta - ligar/desligar o no' e' so' trocar o
    // descritor, sem memoria de imagem nova nem recarregar a malha.
    struct RegionAlias { std::string image, node; uint32_t slot = 0; };
    std::unordered_map<std::string, RegionAlias> m_regionAliases;
    std::unordered_map<std::string, std::string> m_regionPack; // imagem -> nome da emissiva que veio no pacote
    VkImageView regionAliasView(const std::string& image, const std::string& node);
    std::unordered_map<std::string, std::vector<uint32_t>> m_regionSlots; // slots de cada imagem (cintilacao)
    std::vector<std::pair<uint32_t, float>> m_emiFlicker;                  // (slot, fator) do quadro
    struct RegionSurfaceMap { int w = 0, h = 0; std::vector<uint8_t> id; std::vector<std::string> types; };
    std::unordered_map<std::string, RegionSurfaceMap> m_regionSurface;
    std::vector<RegionLampSite> m_regionLampSites;
    std::vector<PointLight> m_regionLampLights;
    texregion::ImageRegions* regionEntry(const std::string& image);
    uint32_t resolveRegionEmissive(const std::string& key);
    uint32_t uploadRegionEmissive(const std::string& key, const uint8_t* rgba, int w, int h);
    void updateRegionSchedule();
    std::vector<texregion::ImageRegions> m_texRegions;
    std::vector<std::string> m_emissiveOffNodes;
    std::vector<std::string> m_regionImageNames, m_regionNodeNames;
    std::unordered_set<std::string> m_regionLive;              // imagens roteadas por textura do motor
    std::unordered_map<std::string, uint64_t> m_regionSig;     // horarios ativos na ultima mascara
    struct RegionAlbedo { std::vector<uint8_t> px; int w = 0, h = 0, ch = 4; bool ok = false; };
    std::unordered_map<std::string, RegionAlbedo> m_regionAlbedo;
    // Decodes a GLB-embedded albedo by name so PBR synthesis can run on
    // textures that have no file on disk. Returns false when not found.
    // Hash da textura embutida como esta' no arquivo (cor: os bytes do PNG/JPEG;
    // surface: o MRAH-W ja' comprimido), sem decodificar - a chave rapida do LOD.
    bool embeddedTextureIdentity(const std::string& name, bool surface, uint64_t& id) const;
    std::string modelTextureCacheKey(const std::string& sanitized) const; // nome + conteudo (Textures.cpp)
    bool findEmbeddedAlbedoPixels(const std::string& name, std::vector<uint8_t>& outPixels,
                                  int& outW, int& outH, int& outChannels);
    // Altura do relevo assado sintetizada do albedo embutido, para a textura
    // sem MRAH-W pronto em m_embeddedBaked (Textures.cpp).
    bool reliefAlphaFromAlbedo(const std::string& name, bool pixels, std::vector<uint8_t>& alpha, int& w, int& h,
                               uint64_t& id);
    void recordTelemetry();
    // Plano proximo da camera (ERUPTION_TEST_NEAR para bancada).
    static float cameraNear() {
        static const float n = [] {
            const char* e = std::getenv("ERUPTION_TEST_NEAR");
            return e ? std::max(0.01f, static_cast<float>(std::atof(e))) : 0.1f;
        }();
        return n;
    }
    // ERUPTION_TEST_TOUR=<s por mapa> (teste): percorre ERUPTION_TOUCH_MAPS,
    // girando a camera uma volta e indo de longe a perto em cada mapa, e loga
    // o FPS por mapa e por fase. Bancada de troca de mapa e de queda de FPS.
    void updateTestTour(float dt);
    // VRAM do processo: alocacoes VMA nas heaps locais da placa (MB).
    double deviceMemoryMb() const;
    void renderLoadingProgress();
    void presentHeldLoading();
    void takeScreenshot(const std::string& filename);
    void readPostOutput(std::vector<float>& rgb, uint32_t& w, uint32_t& h);
    void takeScreenshotLit(const std::string& filename);
    void scheduleScreenshotFromSwap(const std::string& filename);
    void finishPendingScreenshot();
    // Captura pedida por script (game.screenshot): atendida no fim do quadro.
    // Com ui, le o swapchain (assincrono, sem esperar a GPU); sem ui, le a
    // saida do pos-processamento, antes do ImGui (uma leitura sincrona).
    void requestScriptScreenshot(const std::string& path, bool ui) {
        m_scriptShotPath = path;
        m_scriptShotUi = ui;
        m_scriptShotRequested = true;
    }
    void serviceScriptScreenshot();
    float raycastScene(const Vec3& rayOrigin, const Vec3& rayDir, float maxDist, const std::vector<struct VisibleObject>& objects) const;
    float computeObstructionSingleRay() const;
    float computeObstructionMultiRay() const;
    float computeObstructionScreenSpace() const;
    float computeObstructionAngular() const;
    void drawWeatherTab();
    void writeMinimapLiquids();
    bool bakeWalkMinimapImage(VkCommandBuffer cmd, const Mat4& vp);
    Mat4 computeCloudShadowMatrix(const Vec3& lightDir, const Vec3& target) const;
    Window m_window;
    VulkanContext m_vulkan;
    Timer m_timer;
    BindlessDescriptor m_bindless;
    TextureStreamer m_textureStreamer;
    GBuffer m_gbuffer;
    TerrainRenderer m_terrainRenderer;
    ModelRenderer m_modelRenderer;
    SpriteRenderer m_spriteRenderer;
    ShadowRenderer m_shadowRenderer;
    DeferredLighting m_deferredLighting;
    PostProcessor m_postProcessor;
    // Luz volumetrica do sol (preset volumetric_light/_steps/_max_distance).
    VolumetricLight m_volumetric;
    int m_volumetricMode = 0;
    int m_volumetricSteps = 24;
    float m_volumetricMaxDist = 600.0f;
    Vision m_vision;
    // Aneis dos personagens desenhados neste quadro (drawCharacter), para o
    // decal depois da luz.
    std::vector<GroundRing> m_groundRings;
    std::vector<std::pair<std::string, GroundRing>> m_groundAreas; // vfx.area, na ordem de criacao
    VisionMap m_visionMap;
    // Minimapa: escuro da visao no mapa inteiro, camada preta por cima da
    // imagem (ImGui). Quadro do minimapa: x, z do centro e meio lado; e o
    // retangulo do mapa (xmin, zmin, xmax, zmax), fora dele a imagem e' vazia.
    VisionMap m_visionMinimap;
    VkDescriptorSet m_visionMinimapSet = VK_NULL_HANDLE;
    std::vector<uint8_t> m_visionShade;
    Vec3 m_minimapFrame = Vec3(0.0f);
    Vec4 m_minimapKeep = Vec4(0.0f);
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
    // Mistura da normal do billboard com +Y (sprite.frag, FrameUBO) e a do
    // cartao no reflexo espelhado da agua: o mesmo valor, para a luz bater.
    static constexpr float kSpriteNormalYMix = 0.85f;
    bool m_fsrReset = true;
    Vec4 m_prevWindParams = Vec4(0.0f);
    bool m_hasPrevWindParams = false;
    float m_fsrSharpness = 0.0f;   // 0 = sem RCAS
    bool fsrActive() const { return m_upscalerMode != UpscalerMode::Fxaa; }
    void bindFsrInputs(bool write = true);
    bool m_enableFXAA = true;
    float m_aaSharpenAmount = 0.35f;
    SkySystem m_skybox;
    // SONDA DE CEU (G36): cubemap do skybox.frag -> SH9 (difuso) + mips
    // (especular). Preset "sky_probe" / "sky_probe_intensity";
    // ERUPTION_TEST_NO_SKY_PROBE=1 desliga (A/B).
    SkyProbe m_skyProbe;
    // Preset "compute_free": nenhum passe de compute por quadro (sondas de ceu e de
    // irradiancia, histograma de exposicao, vetores de movimento sem uso, efeitos
    // de particula). Para GPUs de celular cujo driver trava em compute.
    bool m_computeFree = false;
    // Preset "camera_near_scale" (0 = desligado, padrao do motor; o jogo liga: plano proximo fixo de
    // cameraNear()): plano proximo = escala x distancia da orbita, limitado a
    // [cameraNear(), 60]. Profundidade D32 padrao com near 0,1 e far 50000 tem
    // passo de ~0,1 u a 400 u do olho; a posicao reconstruida do pixel pula nesse
    // passo e o fator de sombra vira binario a cada deslocamento sub-pixel da
    // camera (tremida). Recorta geometria mais perto da lente que o near.
    float m_cameraNearScale = 0.0f;
    bool m_skyProbeEnabled = true;
    float m_skyProbeIntensity = 1.0f;
    // PROBES DE IRRADIANCIA (G38): visibilidade do ceu por probe, bakeada ao
    // carregar o mapa. Preset "irradiance_probes" / "irradiance_probe_strength";
    // ERUPTION_TEST_NO_PROBES=1 desliga (A/B).
    IrradianceProbes m_irradianceProbes;
    bool m_irradianceProbesEnabled = true;
    // false: a grade nunca e' cozida aqui, so' lida do <mapa>.glb.probes que o
    // PC grava (preset "irradiance_probe_bake"; celular).
    bool m_irradianceProbeBake = true;
    // Instancias que a grade cozida viu (SIZE_MAX: grade nao foi cozida aqui,
    // nao recoze). Ver refreshIrradianceProbes.
    size_t m_probeBakedInstances = SIZE_MAX;
    size_t m_probeSeenInstances = 0;
    int m_probeStableFrames = 0;
    // Assinatura da geometria do mapa carregado (mapGeometryKey): valida os
    // caches derivados dela, como o <mapa>.glb.probes.
    uint64_t m_mapGeometryKey = 0;
    // Cargas de mapa que uma textura sem uso sobrevive no cache (preset
    // "asset_cache_maps"). Celular fraco: 1, senao as texturas de tres mapas
    // somadas passam de 512 camadas e o crescimento da grade trava a GPU.
    uint32_t m_assetCacheMaps = 3;
    // Troca de mapa com fade (MapLoad.cpp, beginWarpFade/endWarpFade).
    bool m_warpFading = false;
    void beginWarpFade();
    void endWarpFade();
    float m_irradianceProbeStrength = 1.0f;
    CloudLayerRenderer m_cloudLayerRenderer;
    WeatherSystem m_weatherSystem;
    WaterRenderer m_water;
    // Passe espelhado do reflexo (preset "water_reflection_pass"); ver
    // WaterReflectionPass.hpp. m_waterReflectionDiv = divisor da resolucao.
    WaterReflectionPass m_waterReflection;
    bool m_waterReflectionOn = false;
    // "water_contour_planar" (preset): a maior agua de contorno pode ser o plano
    // do reflexo planar (SSPR + passe espelhado, um so' para o mapa todo). Falso:
    // as aguas de contorno refletem so' pela marcha de raio, com o ceu de fundo.
    bool m_waterContourPlanar = true;
    uint32_t m_waterReflectionDiv = 2;
    DebugLineRenderer m_debugLineRenderer;
    OverlayLineRenderer m_overlayLineRenderer;
    ImmediateDraw m_immediateDraw;
    VfxBolt m_bolts;       // vfx.bolt: raios 3D
    CloudPrecip m_precip;  // weather.cloud_precip: chuva e neve de nuvem de script
    SpriteSystem m_spriteSystem;
    TextureAtlas m_textureAtlas;
    ChunkManager m_chunkManager;
    ParallaxSystem m_parallaxSystem;
    DayNightCycle m_dayNightCycle;
    DioramaLookConfig m_lookConfig;
    DioramaLookConfig m_bootLook; // captureScriptDefaults
    bool m_bootDof = false;
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
    // Altura do minimapa (R8_UINT): sem filtro linear em NADA, nem entre mips
    // nem anisotropia - o m_nearestSampler mistura mips (VUID-04770).
    VkSampler m_minimapUintSampler = VK_NULL_HANDLE;
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
    // Sono do limitador de FPS no ultimo quadro: a calibracao da escala mede o
    // trabalho, nao o intervalo travado pelo limite.
    float m_capSleepMs = 0.0f;
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
    float m_mapGeometryMinY = 3.4e38f; // chao mais baixo do .glb do mapa (teste de agua soterrada)
    // Escala do balanco da vegetacao (graphics.json wind_sway_scale).
    float m_windSwayScale = 0.15f;
    // Mapa de sombra refeito a cada N quadros (graphics.json shadow_update_interval).
    int m_shadowUpdateInterval = 1;
    // Escala automatica da cena (settleRenderScale). target_fps 0 desliga.
    struct RenderScaleTuning {
        float targetFps = 0.0f;
        float minScale = 1.0f;
        float maxScale = 1.0f;
        int frames = 0;
        int steps = 0;
        bool integer = false; // preset integer_scale: so' 1/n da tela, n divisor exato
        std::array<float, 90> frameMs{}; // janela medida
    } m_scaleTuning;
    bool m_showDebugOverlay = true;
    bool m_devTools = true;
    bool m_showGameHud = true, m_showHudBars = true, m_showMinimapHud = true;
    void drawGameHud(ImDrawList* target = nullptr, ImVec2 origin = ImVec2(0.0f, 0.0f), ImVec2 size = ImVec2(0.0f, 0.0f));
    bool m_showEffectsMenu = false; 
    bool m_showObjectMenu = false;
    bool m_showStatsMenu = false;
    bool m_showCameraHud = true; // painel "Camera" (ImGui.cpp, drawCameraHud)
    bool m_showPostProcessMenu = false;
    bool m_showSpritePicker = false; 
    bool m_showResourceManager = false; 
    bool m_showABTestMenu = false;
    SpherePreview m_spherePreview;
    struct EmbeddedPreview {
        const void* map = nullptr; // m_currentMap quando abriu
        std::string key, image, label;
        uint32_t slot = 0;
        int groundRole = 0;
        float amplitude = 0.0f, worldAmplitude = 0.0f, spacing = 0.0f, edge = 0.0f;
        float gridMin = 0.0f, gridMax = 0.0f, cubeSpacing = 0.0f; // malhas do arquivo; a que o cubo usou
        uint32_t gridMeshes = 0;
        bool baked = true, bakeFailed = false;
        bool inlineCube = false; // o editor desenha o cubo no painel: sem a janela da depuracao
        size_t triangles = 0;
        TextureResource tiles[6]; // cor, normal, M, R, A(O), altura
        VkDescriptorSet sets[6] = {};
    } m_embPreview;
    void openEmbeddedPreview(const std::string& key, const std::string& image, const std::string& label,
                             uint32_t slot, int groundRole, const std::string& file);
    void rebuildEmbeddedCube();
    void releaseEmbeddedTiles();
    bool embeddedMaterialImages(const std::string& image, std::vector<uint8_t> rgba[3], int w[3], int h[3]);
    // Particulas e efeitos (GPU). Texturas das folhas de quadros: do motor,
    // liberadas no shutdown.
    VfxSystem m_vfx;
    // Efeitos do mapa com "nightOnly": nascem ao anoitecer e param de manha
    // junto com as luzes (updateNightLights). id 0 = apagado.
    struct NightEffect {
        std::string name;
        Mat4 transform;
        uint32_t id = 0;
        bool failed = false;
    };
    std::vector<NightEffect> m_nightEffects;
    // Metro -> unidade do mundo para os efeitos quando o mapa nao diz (o
    // personagem anda ~60 u/s e a arvore grande tem ~250 u: ~16 u por metro).
    static constexpr float kVfxUnitsPerMeter = 16.0f;
    std::vector<TextureResource> m_vfxTextures;
    std::unordered_map<std::string, uint32_t> m_vfxTextureSlots;
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
        // Nuvem de script (weather.cloud_spawn): o clima nao a apaga, e ela
        // tem uma vida propria (ScriptCloud.cpp).
        bool fromScript = false;
        uint32_t scriptId = 0;
        ScriptCloudSpec spec;       // parametros atuais (cloud_set muda ao vivo)
        float age = 0.0f;           // s desde o nascimento
        float dispersal = 1.0f;     // 1 = nao formada / dispersa .. 0 = formada
        bool removing = false;
        float removeT = 0.0f, removeDur = 1.0f, removeFrom = 0.0f;
        float flashPeak = 0.0f, flashDur = 0.0f, flashT = 0.0f; // relampago em curso (flashDur > 0)
        bool precipOn = false, precipSnow = true;   // weather.cloud_precip
        bool precipExplicit = false; // cloud_precip foi chamado: vence a precipitacao automatica do preset
        float precipRate = 1.0f, precipRadius = 0.0f; // 0 = deriva da nuvem
    };
    // Camadas retiradas (remocao de script, nuvem de campo cedida): a GPU ainda
    // pode ler os recursos dos quadros em voo, entao o destruir espera alguns
    // quadros em vez de um waitIdle.
    struct RetiredCloudLayer {
        std::unique_ptr<CloudCoverageArray> array;
        uint32_t rendererId = UINT32_MAX;
        int framesLeft = 0;
    };
    std::vector<RetiredCloudLayer> m_retiredCloudLayers;
    uint32_t m_scriptCloudCount = 0;   // nuvens de script vivas (fora de m_independentCloudCount)
    uint32_t m_nextScriptCloudId = 1;
    // Arrays de cobertura ja' criados e ociosos para o proximo cloud_spawn. Criar
    // um (imagens, buffer, pipeline de compute) custa de 0,3 a 4 ms (alocacao de
    // memoria da GPU), mais do que um spawn pode gastar; o spawn pega um daqui e
    // quem repoe e' o quadro (um por 0,1 s), so' depois do primeiro cloud_spawn.
    std::vector<std::unique_ptr<CloudCoverageArray>> m_spareCloudArrays;
    bool m_scriptCloudsUsed = false;
    uint32_t m_scriptCloudRng = 0x9E3779B9u; // sorteio de cloud_point (xorshift)
    Vec2 scriptCloudCenter(const ScriptCloudSpec& s) const; // (x, z) + deslocamento da camera (frame)
    float scriptCloudThickness(const ScriptCloudSpec& s) const;
    float m_spareRefillCooldown = 0.0f;
    std::unique_ptr<CloudCoverageArray> makeScriptCloudArray(float planeY);
    void retireCloudLayer(size_t index);
    void updateScriptClouds(float dt);
    void applyScriptCloudLook(IndependentCloudLayer& layer);
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
    uint32_t m_independentCloudCount = 0; // clima + manuais (as de script contam em m_scriptCloudCount; juntas <= LocalCloud::MAX_COUNT)
    // Spawns an independent cloud at the player position (plus an optional XZ
    // jitter) with the given altitude. Always adds; never replaces existing
    // clouds. density < 0 uses the current Local Density slider value.
    // radiusScale multiplies the baked blob size (used by the weather field).
    // fromField marks weather-field clouds (they respawn upwind off the map).
    void spawnIndependentCloud(float altitude, float density = -1.0f,
                               Vec2 jitterXZ = Vec2(0.0f), bool hasRain = false,
                               float radiusScale = 1.0f, bool fromField = false);
    // keepScript: o clima trocou, as nuvens de script ficam (so' o fim do mapa as leva).
    void clearIndependentClouds(bool keepScript = false);
    float independentCloudPlaneY(float altitude, bool rain = false) const;
    bool cloudBoxFor(bool rain) const;

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
    // Caixa antes do 1o mapa (linha de comando ou nenhuma): o .env "cloud_box"
    // vale so' no mapa dele (MapLoad).
    struct CloudBoxBase { bool on; float bottom; float thickness; bool valid; };
    CloudBoxBase m_cloudBoxBase{false, 5000.0f, 640.0f, false};
    bool m_cloudBoxBaseSaved = false;
    bool m_cloudShadowsBase = true;
    int m_mapCloudMinCount = 0;       // "count" do cloud_box do .env: nuvens em qualquer clima
    float m_mapCloudMinDensity = 0.6f;
    float m_mapCloudMinScale = 0.0f;
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
    float m_initialDist = camera_default::kDistance;
    std::string m_screenshotOnLoad;
    std::string m_screenshotAfterWarpPath;
    bool m_screenshotAfterWarpPending = false;
    int m_framesCount = 0;
    int m_framesAfterSwap = -1;
    // Troca de mapa pelo seletor (warp): o .glb é lido numa thread e o mapa
    // entra pelo mesmo loadMap do carregamento direto (antes era um segundo
    // caminho de montagem, sem LOD, sem soldar vértices, sem GI nem chão).
    // Mapa lido em segundo plano, com os modelos ja' convertidos (MapLoad.cpp).
    struct PreparedMap;
    static std::shared_ptr<PreparedMap> readPreparedMap(const std::string& name, bool deferPbr);
    std::future<std::shared_ptr<PreparedMap>> m_warpJob;
    std::string m_warpJobMap;
    bool m_warpJobActive = false;
    void updateWarp();
    // preloadMap: leitura em curso, ultimo pedido e o mapa pronto para o warp.
    std::future<std::shared_ptr<PreparedMap>> m_preloadJob;
    std::string m_preloadJobMap; // mapa que m_preloadJob realmente le' (m_preloadWanted pode ja' ser outro)
    // Arquivos que a cena e o scatter do mapa pedido acrescentam (modelos e
    // camadas de terreno), lidos fora do quadro: interpretar a cena de um mapa
    // de 12,7 mil pecas custava ~150 ms dentro do world.preload_map.
    struct PreloadLists {
        std::vector<std::string> models, layers;
    };
    std::future<PreloadLists> m_preloadListJob;
    // Mapa da lista acima e os modelos da cena dele: o warp so' troca quando
    // esses modelos ja' foram lidos (updateWarp), com a tela escura e os
    // quadros andando, em vez de parar o fio principal na chegada lendo do HD.
    std::string m_preloadListMap;
    std::vector<std::string> m_preloadListModels;
    // Leitura dos PBR soltos desses modelos (so' para o cache do sistema),
    // antes da troca: na chegada eram 2,4 s de PBR lido do HD no fio principal.
    std::future<void> m_warpPbrWarm;
    // Futuros de std::async bloqueiam quem os destroi ate o job acabar. Job de um
    // warp abandonado (outro pedido chegou) vai para ca e e' solto quando pronto,
    // sem segurar o fio principal nem a carga do warp mais novo.
    std::vector<std::function<bool()>> m_abandonedJobs;
    template <class T> void parkJob(std::future<T>& f) {
        if (!f.valid()) return;
        auto held = std::make_shared<std::future<T>>(std::move(f));
        m_abandonedJobs.emplace_back([held] { return held->wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    }
    void reapAbandonedJobs();
    void abandonWarp();
    std::string m_warpPbrWarmMap;
    PreloadLists readPreloadLists(const std::string& name) const;
    std::string m_preloadWanted;
    std::shared_ptr<PreparedMap> m_preloaded;
    void updatePreload();
    // Modelos convertidos na thread do warp; o loadMap seguinte os usa no
    // lugar de converter de novo.
    std::vector<ModelAsset> m_convertedModels;
    // Nós montados na mesma thread (slots provisórios) e a assinatura da
    // geometria que eles viram: só valem se o retoque de relevo não mudou nada.
    std::unordered_map<std::string, ModelRenderer::PreparedAsset> m_convertedPrepared;
    uint64_t m_convertedKey = 0;
    // Texturas embutidas lidas do cache na mesma thread, por arquivo.
    std::unordered_map<std::string, EmbeddedBakeResult> m_convertedBakes;
    // UI-inclusive screenshot capture state.
    bool m_screenshotPending = false;
    std::string m_scriptShotPath;
    bool m_scriptShotUi = true;
    bool m_scriptShotRequested = false;
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
        std::unordered_map<uint32_t, std::string> slotKey; // slot -> chave em modelTextures (detachCopiedSource)
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
            if (!tex.isExternal) {
                if (tex.bindlessSlot && tex.image != VK_NULL_HANDLE) slotKey[tex.bindlessSlot] = path;
                if (tex.pbrSlot && tex.pbrImage != VK_NULL_HANDLE) slotKey[tex.pbrSlot] = path;
                if (tex.normalSlot && tex.normalImage != VK_NULL_HANDLE) slotKey[tex.normalSlot] = path;
            }
        }
        // Sem o array de camadas (GPU sem bindless) a textura vive duas vezes: a
        // imagem de origem e a camada. Depois da copia tira do cache a imagem de
        // origem do `slot` (a entrada fica, so' sem ela: o slot segue valendo);
        // quem chama a destroi. false se o slot nao e' de uma textura de modelo
        // do cache ou ela ja' foi liberada.
        bool detachCopiedSource(uint32_t slot, VkImage& image, VmaAllocation& alloc, VkImageView& view) {
            std::lock_guard<std::mutex> lock(mutex);
            const auto key = slotKey.find(slot);
            if (key == slotKey.end()) return false;
            const auto it = modelTextures.find(key->second);
            slotKey.erase(key);
            if (it == modelTextures.end()) return false;
            ModelTextureGPU& t = it->second.tex;
            if (t.bindlessSlot == slot && t.image != VK_NULL_HANDLE) {
                image = t.image; alloc = t.alloc; view = t.view;
                t.image = VK_NULL_HANDLE; t.alloc = VK_NULL_HANDLE; t.view = VK_NULL_HANDLE;
                return true;
            }
            if (t.pbrSlot == slot && t.pbrImage != VK_NULL_HANDLE) {
                image = t.pbrImage; alloc = t.pbrAlloc; view = t.pbrView;
                t.pbrImage = VK_NULL_HANDLE; t.pbrAlloc = VK_NULL_HANDLE; t.pbrView = VK_NULL_HANDLE;
                return true;
            }
            if (t.normalSlot == slot && t.normalImage != VK_NULL_HANDLE) {
                image = t.normalImage; alloc = t.normalAlloc; view = t.normalView;
                t.normalImage = VK_NULL_HANDLE; t.normalAlloc = VK_NULL_HANDLE; t.normalView = VK_NULL_HANDLE;
                return true;
            }
            return false;
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
        // Tira do cache as texturas cujo nome começa com `prefix` (as embutidas
        // de um mapa vizinho que saiu: "#<mapa>|..."); quem chama as destrói.
        std::vector<ModelTextureGPU> takeModelTextures(const std::string& prefix) {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<ModelTextureGPU> out;
            for (auto it = modelTextures.begin(); it != modelTextures.end();) {
                if (it->first.rfind(prefix, 0) == 0) {
                    out.push_back(it->second.tex);
                    it = modelTextures.erase(it);
                } else {
                    ++it;
                }
            }
            return out;
        }
        // Tira do cache as malhas de um nó ("arquivo#nó"; quem chama libera os
        // buffers). A chave guardada leva ainda "#<impressão dos slots>", uma
        // por versão de texturas.
        std::vector<ModelMeshGPU> takeModelMeshes(const std::string& nodeKey) {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<ModelMeshGPU> out;
            const std::string prefix = nodeKey + "#";
            for (auto it = modelMeshes.begin(); it != modelMeshes.end();) {
                if (it->first.rfind(prefix, 0) == 0) {
                    out.push_back(it->second.mesh);
                    it = modelMeshes.erase(it);
                } else {
                    ++it;
                }
            }
            return out;
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
    bool m_isInitialBoot = true;
    bool m_inFrame = false;
    bool m_playerSpawnedOnActiveMap = false;
    bool m_loadingHeld = false;
    float m_loadingHeldLeft = 0.0f;     // segundos ate' soltar sozinho
    float m_loadingHeldProgress = 0.0f; // barra durante a espera, ate' 0.99
    bool m_loadingFinishing = false;    // release: um quadro a 100% antes da cena
    bool m_presentingHeld = false;      // quadro desenhado pela espera (barra ja' pronta)
    float m_gameLoadDone = 0.0f, m_gameLoadTotal = 0.0f; // loading.game_progress
    float m_gameLoadShown = 0.0f;       // fracao do jogo na barra, andando dentro do passo
    float m_gameStepAge = 0.0f;         // segundos desde o ultimo passo do jogo
    float m_gameStepSecs = 2.0f;        // segundos entre avisos (media; comeca lento)
    float m_gameStepFrac = -1.0f;       // fracao ganha por aviso (media; < 0 = sem aviso)
    // loading.progress (LoadProgress.cpp): a carga em curso comeca num mapa
    // novo ou numa troca em segundo plano.
    std::string m_lpMap;
    bool m_lpWarp = false;
    int m_lpSessions = 0;
    int m_lpPendingLast = 0, m_lpSeen = 0;
    float m_lpRaw = 0.0f;   // fracao da carga, a da barra antes do 0.3 + 0.7 * p
    bool m_lpMapped = false; // carga que comecou no boot: o script ve' 0.3 + 0.7 * p
    bool m_lpReady = false;  // esta carga ja' acabou (fica ate' a proxima)
    bool m_firstLoadOver = false; // primeiro quadro normal depois do boot: a barra deixa o 0.3 + 0.7 * p
    inline static const Vec3 kSafePreWarpPlayerPos = Vec3(0.0f, -1000.0f, 0.0f);
    std::string m_pendingMapWarp;
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
    // Lavas desenhadas (até WaterRenderer::kMaxLavaSurfaces), com o ajuste
    // do menu aplicado; a ordem é a das faixas da malha de lava.
    std::vector<LoadedMap::MapLiquid> m_lavaLiquids;
    // Por lava desenhada: o líquido do mapa de origem e o nível com que a
    // malha foi montada (o nível vivo vira deslocamento no shader).
    struct LavaSource {
        const LoadedMap* map = nullptr; // o aberto ou um vizinho em cena
        uint32_t index = 0;
        float builtLevel = 0.0f;
    };
    std::vector<LavaSource> m_lavaSources;
    // As superficies desenhadas pelo caminho de contorno sao lavas (kind 1) e
    // aguas de contorno (kind 0, polygon no .env). Estas ultimas: caixa (no
    // nivel da malha) e area em XZ de cada uma, para o culling e para a
    // escolha do plano do reflexo planar (WaterRenderer::setContourPlane).
    struct ContourWater {
        uint32_t drawn = 0;     // indice em m_lavaLiquids
        Vec3 mn{0.0f}, mx{0.0f};
        float area = 0.0f;
    };
    std::vector<ContourWater> m_contourWater;
    // Quanto o nivel vivo (setLiquidLevel) andou desde que a malha da superficie
    // desenhada `drawn` foi montada; vira deslocamento em Y no shader.
    float contourLift(uint32_t drawn) const;
    // Plano do reflexo e visibilidade das aguas de contorno; a cada quadro,
    // antes do passe espelhado.
    void updateContourWater();
    bool contourWaterInView() const;
    bool waterPlaneInView() const;
    // Fontes da grade da beira (buildShoreMask): a agua do mapa e as lavas,
    // uma faixa de indices por lava (mesma ordem de m_lavaLiquids). A grade
    // guarda, por celula, a proximidade e QUAL liquido esta' mais perto - a
    // beira (onda, lava subindo, contato) e' propriedade de cada plano.
    WaterMesh m_shoreWaterSrc;
    WaterMesh m_shoreLavaSrc;
    // Durante o loadMap a grade da beira so' e' montada uma vez, no fim, com a
    // config final (antes eram tres montagens, uma de ~75 ms cada num mapa grande).
    bool m_shoreBuildDeferred = false;
    bool m_shoreAsyncOnce = false;    // a proxima montagem sai em segundo plano (fim do loadMap)
    uint32_t m_shoreGen = 0;          // sobe a cada montagem; descarta resultado velho
    uint32_t m_shoreAsyncGen = 0;
    std::future<std::shared_ptr<struct ShoreOut>> m_shoreJob;
    std::vector<uint32_t> m_shoreLavaCounts;
    WaterBenchmark m_waterBenchmark;
    bool m_waterForceWaterPrev = false;
    float m_mapBaseWaterLevel = 0.0f;
    bool m_waterOn = false; // o mapa tem plano de água agora
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
    float m_smokeScale = 1.0f;   // chave "smoke_scale": < 1 desenha a coluna em baixa resolucao e compoe
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
    // Preenchimento ambiente escrito pelo .env do mapa (bloco light.ambient).
    // Vazio = o ciclo dia/noite manda, que e' o certo a ceu aberto. Um mapa de
    // interior escreve o seu e passa a mandar: la' dentro nao ha hora do dia.
    Vec3  m_mapAmbientColor = Vec3(0.0f);
    bool  m_mapAmbientAuthored = false;
    bool  m_mapLightAuthored = false;
    // "sky": false no .env — interior fechado, sem cupula de ceu no fundo.
    bool  m_mapHasSky = true;
    float m_mapAmbientIntensity = 0.95f;
    // Sol escrito pelo .env do mapa. Um INTERIOR precisa poder desligar o sol
    // (opacity 0): nao existe sol dentro de uma caverna fechada, e o ciclo
    // dia/noite insistia em por um, com reflexo em agua e tudo.
    Vec3  m_mapSunDirection = Vec3(0.0f, -1.0f, 0.0f);
    Vec3  m_mapSunColor = Vec3(1.0f);
    float m_mapSunIntensity = 1.0f;
    // Texturas embutidas do GLB já comprimidas em bloco (BC1/BC3 albedo, BC7
    // normal/mrahw) com mips prontos, indexadas pelo nome da imagem. Vazio
    // quando a GPU não suporta BC ou o bake falhou: o caminho RGBA8 continua
    // valendo, é só mais VRAM.
    std::unordered_map<std::string, BakedEmbeddedTexture> m_embeddedBaked;
    // Albedo copiado na thread principal para reliefAlphaFromAlbedo sintetizar
    // a altura nas threads do assado (o mapa aberto muda enquanto elas rodam).
    struct ReliefAlbedo {
        std::vector<uint8_t> bytes; // PNG/JPG embutido, ou pixels ja' decodificados
        int w = 0, h = 0, channels = 0; // > 0: `bytes` sao pixels
    };
    std::mutex m_reliefAlbedoMutex;
    std::unordered_map<std::string, std::shared_ptr<const ReliefAlbedo>> m_reliefAlbedo;
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
    // Piso 1,0: a exposicao nao escurece abaixo do neutro. Em cena clara
    // (neve, ceu) ela descia a 0,75 e afundava as sombras (autor 2026-10-11).
    float m_autoExposureMin = 1.0f;
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
    float m_tessAmplitude = 3.0f;   // unidades de mundo, pico do deslocamento (autor: default 3)
    // Ganho da altura em passa-alta. A altura cozida vem da LUMINANCIA do
    // albedo (PbrMapGen), entao a banda baixa dela e' a sombra pintada da
    // textura; o shader usa mip2-mip5 e este ganho devolve a faixa util. Alto
    // demais e o chao vira papel amassado.
    float m_tessHeightGain = 1.5f;   // autor: default 1.5
    // Espacamento alvo entre vertices da tesselacao (u). A curva sozinha tem
    // teto 8 por aresta, e o triangulo do chao tem ~10-12 u: ~1,3 u entre
    // vertices, grosso demais pra desenhar paralelepipedo. 0 = so' a curva.
    float m_tessVertexSpacing = 0.25f;
    // Teto de inclinacao da normal do normal map, em GRAUS (0 = sem teto).
    // Acima de ~65 graus a normal deita e a superficie le' como papel
    // amassado - ver o bloco em model.frag.
    float m_normalMaxSlopeDeg = 62.0f;
    // "Tessellation Factor by Distance" (chave normal_distance.tessellation_curve).
    // Y = fator de tesselacao (triangulos por aresta) pela distancia, 1..teto.
    SplineCurve m_tessCurve{{0.0f, 0.14528f, 0.23606f, 0.47995f, 0.84906f, 1.0f}, {50.0f, 20.0f, 10.0f, 5.0f, 1.0f, 1.0f}, InterpolationMode::Linear};
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
    float m_frameDt = 0.0f; // dt do frame (animação por esqueleto no render)
    float m_cpuAppUpdateMs = 0.0f;  // app.onUpdate(): gameplay/controller
    float m_cpuRenderMs = 0.0f;     // render(): command recording + submit
    float m_cpuTailMs = 0.0f;       // onRender + input + profiler commit
    // Ligado de fabrica: o TBN do motor (cotangente em model/terrain.frag,
    // gbTBN no chao misturado) tem B = dP/dv, e os normal maps gerados pela
    // rede saem com o verde no sentido oposto. Medido contra a normal tirada
    // da propria altura do MRAH-W: x correlaciona +0,61..+0,74 e y -0,65..-0,77
    // em pedra, rocha e camada de chao - fenda iluminada como saliencia.
    bool m_normalMapInvertY = true;    // true = flip normal-map green channel (Y down)
    float m_defaultRoughness = 0.8f;   // Fallback roughness when no PBR map is bound
    float m_defaultMetallic = 0.0f;    // Fallback metallic when no PBR map is bound
    struct CharConfig m_charConfig;
    ResourceConfig m_resourceConfig;
    UiCanvas m_uiCanvas;
    UiDocument m_uiDocument;
    // Retrato, icone, moldura: um PNG por caminho, carregado sob demanda.
    // Guardado para sempre porque sao poucos e pequenos, e recarregar a cada
    // quadro custaria um upload por quadro.
    struct UiImage { TextureResource tex; VkDescriptorSet set = VK_NULL_HANDLE; };
    std::unordered_map<std::string, UiImage> m_uiImages;
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
    std::string m_minimapLabel;
    // Minimap composite pass (IGNIS G10/G11): relights the minimap G-buffer and
    // paints water from terrain height, instead of blitting raw albedo. See
    // renderMinimap() and shaders/postprocess/minimap_composite.frag.
    VkDescriptorSetLayout m_minimapCompositeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_minimapCompositeLayout = VK_NULL_HANDLE;
    VkPipeline m_minimapCompositePipeline = VK_NULL_HANDLE;
    VkDescriptorSet m_minimapCompositeDS = VK_NULL_HANDLE;
    static constexpr uint32_t MINIMAP_RES = 256;
    // Liquidos (lava, sobretudo) do mapa, para o composite pintar o lago de
    // lava com brilho proprio em vez de deixar so' a rocha por baixo - ver
    // initMinimapComposite()/renderMinimap() e minimap_composite.frag. Buffer
    // host-visivel, mapeado para sempre; os bytes sao reescritos a cada rebake
    // (cadencia ja' rara, gated por `dirty`), o handle nunca muda, entao o
    // descriptor write so' acontece uma vez.
    static constexpr uint32_t kMinimapMaxLiquids = 64;
    VkBuffer m_minimapLiquidsBuffer = VK_NULL_HANDLE;
    VmaAllocation m_minimapLiquidsAlloc = VK_NULL_HANDLE;
    void* m_minimapLiquidsMapped = nullptr;
    // Minimapa por grade de andar (core/MinimapWalk.hpp, data/hud/minimap.json):
    // os pixels saem da CPU uma vez por bake, por um buffer host-visivel
    // persistente e uma copia para a textura que ja' existe (sem alvo nem passe).
    MinimapWalkConfig m_minimapWalkCfg;
    const void* m_minimapWalkCfgKey = nullptr; // a config e' relida a cada mapa
    VkBuffer m_minimapWalkBuffer = VK_NULL_HANDLE;
    VmaAllocation m_minimapWalkAlloc = VK_NULL_HANDLE;
    void* m_minimapWalkMapped = nullptr;
    PlayerController* m_playerController = nullptr;
    float m_seeThroughRadius = 0.0f;
    float m_seeThroughOpacity = 0.25f;
    bool m_seeThroughAtFocus = false;
    float m_seeThroughBand = 0.0f;
    float m_seeThroughMinHidden = 0.0f;
    bool m_seeThroughOn = false;       // histerese do min_hidden
    float m_seeThroughGate = 1.0f;     // forca do corte, 0..1 (fade de ~0,2 s)
    float m_seeThroughHidden = 0.0f;   // ultima fracao medida
    void updateSeeThroughGate(float dt);
    float measureSpriteHidden() const;
    bool m_cameraCollide = false;
    float m_cameraCollideMargin = 6.0f;
    float m_cameraCollideDist = -1.0f; // distancia atual (< 0 = sem colisao)
    float m_cameraCollideLift = 0.0f;  // radianos somados ao pitch (lente por cima da pedra)
    void updateCameraCollision(float dt);
    // Raio contra as malhas de chao sem balanco (CameraCollision.cpp): os
    // cruzamentos em ordem, (t, true) entrando pela face da frente.
    void raycastGroundMeshes(const Vec3& origin, const Vec3& dir, float maxT, std::vector<std::pair<float, bool>>& hits);
    void warmCameraCollision();
    std::shared_ptr<struct GroundRayCache> m_groundRays;
    bool m_focusPointSet = false;
    Vec3 m_focusPoint{0.0f};
    float m_nearFadeStart = 0.0f, m_nearFadeEnd = 0.0f;
    // Chaves de preset alpha_hash (0..1, alfa-teste por hash) e shadow_soft (0/1/2, PCF da variante leve).
    float m_alphaHash = 0.0f;
    int m_shadowSoft = 0;
    bool m_orbitInput = true;
    std::array<Vec4, 3 * FrameUBO::kCutaways> m_cutaway{};
    int m_spriteShadowKind = -1;
    float m_spriteContactSun = 0.0f; // "map_contact": forca do disco ao sol (0..1)
    float m_spriteSinkPixels = 0.0f; // characters.set_sink: pixels do quadro abaixo do chao
    float m_spriteRimStrength = 0.0f;  // characters.set_rim (0 = sem luz de borda)
    Vec3 m_spriteRimTint{1.0f};
    float m_spriteRimPreset = 1.0f;    // chave "sprite_rim" do preset (multiplica; 0 desliga)
    float m_spriteStepFps = 0.0f;      // characters.set_anim_fps global (0 = livre)
    std::unordered_map<std::string, float> m_spriteStepByKey; // personagem ou folha -> fps
    bool m_spriteStepAllowed = true;   // chave "sprite_step" do preset (false desliga)
    double m_spriteStepClock = 0.0;    // s; relogio comum dos degraus
    float spriteStepFpsFor(const SceneCharacter& c) const;
    float m_walkStride = 0.0f;         // characters.set_stride global (0 = desligado)
    std::unordered_map<std::string, float> m_walkStrideByName;
    float walkStrideFor(const SceneCharacter& c) const;
public:
    // PASSOS. updateCharacters registra aqui cada passo dado no quadro (meia
    // passada andada no chao; teleporte nao conta). Quem consome (ondinha na
    // agua, efeito de pisar no chao) le' depois do update; a lista e' refeita
    // todo quadro.
    struct CharacterStep {
        std::string name;
        Vec3 pos{0.0f};  // ponto do pe' no chao (ancora deslocada para o lado do pe')
        Vec2 dir{0.0f, 1.0f}; // direcao horizontal do andar (unitaria)
        int foot = 0;    // 0 = esquerdo, 1 = direito
        float speed = 0.0f; // u/s no passo
        // Origem do passo: 0 = passada (sem sprite: modelo 3D), 1 = pe' que
        // pisou (strike), 2 = corpo arrastando (drag). Com 1 e 2 `shape` e' a
        // textura gerada da forma que toca o chao e `width` a largura dela.
        int contact = 0;
        std::string shape;
        float width = 0.0f;
    };
    const std::vector<CharacterStep>& characterSteps() const { return m_characterSteps; }
    // Agua de superficie no ponto (x, z): true com o nivel em *level quando ha'
    // agua ali acima de p.y - kFolga (o pe' na agua ou na lamina dela). A agua
    // vence o chao embaixo: efeito de pisar no chao nao sai onde isto e' true.
    bool waterSurfaceAt(const Vec3& p, float* level) const;
    // Passo com o chao sob o pe' (FootstepEffects.cpp), para o on_step do
    // script. Sem tabela de tipos no motor: quem decide o que cada material faz
    // e' o jogo, que chama world.footprint / world.dust.
    struct FootstepEvent {
        std::string name;
        Vec3 pos{0.0f};
        Vec2 dir{0.0f, 1.0f};
        int foot = 0;
        float speed = 0.0f;
        int contact = 0;       // CharacterStep::contact
        std::string shape;     // textura da forma de contato ("" = passada)
        float width = 0.0f;    // largura da forma, em metros (o `size` do world.footprint)
        // Triangulo de chao embaixo do pe': material do glTF (o caminho original
        // da textura nos mapas em ladrilhos), imagem de cor, no', e TEXCOORD_0
        // no ponto (com o wrap em 0..1).
        std::string material, texture, node;
        // Tipo de superficie da regiao desenhada na textura sob o pe' (areia,
        // neve, ...; editor, aba Regions). "" = nenhuma regiao de superficie.
        std::string surface;
        Vec2 uv{0.0f};
        bool hasGround = false;
        bool inWater = false; // waterSurfaceAt no pe'
        bool onIce = false;   // o pe' pisa no gelo de um lago congelado (ground_state ice_water)
    };
    // Calculado na primeira chamada do quadro (sem script que leia, nada roda).
    const std::vector<FootstepEvent>& footstepEvents();
    // Efeitos genericos com os parametros na mao (world.footprint / world.dust).
    // Tamanhos em metros do VFX; a cor e' linear.
    struct FootprintParams {
        std::string texture;   // vazio = a marca embutida (builtin:foot)
        Vec3 color{0.12f, 0.09f, 0.06f};
        float size = 0.16f;    // m, comprimento da marca
        float life = 10.0f;    // s ate' sumir (suave nos ultimos 30%)
        float alpha = 0.6f;
        bool mirror = false;   // textura espelhada (pe' direito)
        // Forma de contato (CharacterStep::shape): vale no lugar de `texture`,
        // com a ponta para a direcao do andar. `size`, se dado, e' a largura
        // da forma; sem ele, a estimada das 8 direcoes do sprite. Sem `color`,
        // preta: a marca so' escurece o chao, que guarda o proprio tom (cor
        // somada ganha a luz de ceu das particulas e sai cinza-azulada).
        std::string shape;
        bool sizeGiven = false;
        bool colorGiven = false;
        std::string owner;     // de quem e' a marca (o rastro tem teto por dono)
    };
    struct DustParams {
        Vec3 color{0.6f, 0.52f, 0.4f};
        float size = 0.35f;    // m, diametro do grao
        float count = 6.0f;    // particulas
        float life = 0.8f;     // s
        float rise = 0.25f;    // m/s2 para cima
        float alpha = 0.5f;
    };
    void spawnFootprint(const Vec3& pos, const Vec2& dir, const FootprintParams& p);
    void spawnDust(const Vec3& pos, const DustParams& p);

    // AFECCOES DO LUGAR (core/AffectsEngine.cpp, logica em core/Affects.hpp):
    // um elemento (kind) com forca 0..1, area (radius, unidades do mundo) e
    // duracao (s) marca o chao e o entorno; a reacao sai da tabela
    // data/affects.json pela superficie do lugar. Devolve um id (0 = pedido
    // recusado: elemento desconhecido ou sem reacao na tabela).
    struct AffectRequest {
        std::string kind;
        float x = 0.0f, z = 0.0f;
        float radius = 0.0f; // 0 = padrao: 3 m (3 x unidades por metro do mapa)
        float strength = 1.0f;
        float duration = 20.0f;
        Vec2 dir{0.0f, 1.0f};
        bool hasDir = false;
    };
    uint32_t affect(const AffectRequest& r);
    // Para a afeccao (e as particulas dela) agora; false se o id nao vive mais.
    // Vale tambem para o id de uma rajada.
    bool affectStop(uint32_t id);
    // Rajada radial local de vento: a vegetacao se curva para fora, a frente
    // anda e a forca decai com a distancia e o tempo. Teto fixo de rajadas.
    // `shape`: largura da frente, o que sobra atras dela, tremor da copa e sopro
    // das particulas (os padroes sao a rajada de vento comum).
    uint32_t windBurst(float x, float z, float radius, float strength, float duration,
                       const affects::GustShape& shape = affects::GustShape());

    // ESTADO DO CHAO POR CELULA (core/GroundStateEngine.cpp, logica em
    // core/GroundState.hpp): queimado, geada, molhado, neve ou gelo sobre a agua
    // numa area, com forca, duracao e um tempo de sumir. Modula o material do
    // chao (albedo, rugosidade, normal) no passe de luz. Devolve um id (0 =
    // tipo desconhecido).
    struct GroundStateRequest {
        std::string kind;       // scorch, frost, wet, snow, ice_water
        float x = 0.0f, z = 0.0f;
        float radius = 0.0f;    // 0 = 3 m; meio lado se `square`
        float strength = 1.0f;  // 0..1
        float duration = 0.0f;  // s ate' sumir; 0 = o padrao do tipo
        float fade = -1.0f;     // s do fim gastos sumindo; < 0 = o padrao do tipo
        bool square = false;    // uma celula quadrada no lugar do disco
        // ice_water: o gelo aguenta quem anda (fica na superficie da agua, a
        // celula vira andavel). false = so' o visual. Ignorado nos outros tipos.
        bool walkable = true;
    };
    uint32_t groundState(const GroundStateRequest& r);
    bool groundStateStop(uint32_t id);
    size_t groundStatesAlive() const { return m_groundField.size(); }
    // Gelo sobre a agua que aguenta peso (ice_water com walkable): 0..1, 1 =
    // congelado em (x, z), entre 0 e 1 = afundando no degelo. Custo zero sem gelo.
    float iceLift(float x, float z) const { return m_groundField.iceLift(x, z); }
    // `floorY` (o leito: terreno ou topo de solido) levantado ate' a superficie
    // da agua onde ha' gelo, na proporcao de iceLift; sem gelo devolve `floorY`.
    // E' a altura de quem anda no lago congelado (jogador, characters.*, caminho).
    float iceRaise(float x, float z, float floorY) const;
    void settleOnIce(SceneCharacter& c, float dt);
    // Superficie do lugar (regiao de textura, material do chao, agua) pela tabela.
    std::string affectSurfaceAt(const Vec3& pos);
    const affects::Table& affectTable();
    size_t affectsAlive() const { return m_affectRing.size(); }
    size_t gustsAlive() const { return m_gustRing.size(); }
    void updateAffects(float dt);                   // 1x por quadro, Render.cpp
    void affectsOnMapLoaded();                      // o VFX foi limpo: as afeccoes e as rajadas caducam
    void fillGusts(FrameUBO& ubo) const;            // rajadas do quadro para o UBO
    void updateGroundState(float dt);               // 1x por quadro (updateAffects chama)
    void groundStateApplyPreset(const json& preset);
    void groundStateOnMapLoaded();

private:
    affects::Table m_affectTable;
    bool m_affectTableLoaded = false;
    affects::AffectRing m_affectRing;
    affects::GustRing m_gustRing;
    affects::Quality m_affectQuality;
    uint32_t m_affectNextId = 0;
    double m_affectClock = 0.0;
    std::unordered_map<std::string, std::string> m_affectEffects; // chave da variante -> efeito do VFX
    ground::Field m_groundField;
    uint32_t m_groundTexels = 512;   // preset ground_state_texels
    float m_groundTexelM = 0.5f;     // preset ground_state_texel_m (metros por celula)
    uint32_t m_groundCap = 400;      // preset ground_state_max
    double m_groundNextRaster = 0.0;
    bool m_groundDirty = true;
    void loadAffectTable();
    void affectApplyPreset(const json& preset);
    std::vector<CharacterStep> m_characterSteps;
    std::vector<FootstepEvent> m_footstepEvents;
    bool m_footstepEventsReady = false;
    FootstepEffects m_footsteps;
    bool footSurfaceAt(const Vec3& pos, ModelRenderer::SurfaceHit& hit);
    void trackCharacterStep(SceneCharacter& c, float dt);
    // Passos pelos pixels do sprite (FootstepEffects.cpp): chamado por
    // drawCharacter com a acao e o quadro mostrados.
    Vec3 groundUnderSpritePoint(const Vec3& anchor, float sx, float up) const;
    void trackContactSteps(SceneCharacter& c, int actionIdx, int shownFrame, bool moving, const Vec3& anchor);
    void pushCharacterStep(const SceneCharacter& c, const Vec3& pos, int contact, const std::string& shape, float width);
    struct WalkRate { float cell = 0.0f, refMs = 150.0f; };
    WalkRate m_walkRate;
    std::unordered_map<std::string, WalkRate> m_walkRateByName;
    WalkRate walkRateFor(const SceneCharacter& c) const;
    AmbientParticles m_ambient;
    float m_ambientPreset = 1.0f;      // chave "ambient_particles" (densidade; 0 desliga)
    int spriteShadowKind() const;
    float m_groundBlendBand = 0.0f;
    float m_groundBlendFraction = 0.3f;
    struct GroundHeightTexture {
        uint32_t slot = 0;
        Vec4 map{0.0f};      // uv = (x * map.x + map.y, z * map.z + map.w)
        float minY = 0.0f, range = 1.0f, cell = 10.0f;
    } m_groundHeight;
    uint32_t m_groundHeightEpoch = 0;
    bool buildGroundHeightTexture();
    void fillGroundBlend(FrameUBO& ubo);
    // [0] = luz dominante, [1] = a segunda (a sombra por luz usa as duas).
    std::vector<SpriteLight> m_spriteLights[2];
    void updateSpriteDominantLights();
    void collectSpritePointShadows(std::vector<SpritePointShadow>& out) const;
    void fillSeeThrough(FrameUBO& ubo) const;
    std::vector<SceneCharacter> m_characters;
    SceneObjects m_sceneObjects;
    // Sprite de personagem ja lido, por caminho de arquivo. Sem isto, cada
    // personagem relia e reenviava a folha inteira para a GPU.
    std::unordered_map<std::string, LoadedSprite> m_characterSpriteCache;
    std::unordered_map<std::string, uint32_t> m_characterSpriteUse; // folha -> troca de mapa em que foi usada
    uint32_t m_characterSpriteGen = 0;
    // Solta as folhas sem uso ha' `keepGenerations` trocas de mapa (0 = todas).
    void releaseCharacterSpriteCache(uint32_t keepGenerations);
    // Modelos pedidos por preloadModel: arquivo -> modelo pronto (ou a
    // caminho) e a troca de mapa em que foi pedido. So' o fio principal mexe.
    struct ModelPreload {
        std::shared_future<std::shared_ptr<ModelFile>> model;
        uint32_t gen = 0;
    };
    std::unordered_map<std::string, ModelPreload> m_modelPreloads;
    uint32_t m_modelPreloadGen = 0;
    bool m_hasPlayerStart = false;
    HeightEdit m_heightEdit;
    std::vector<ScatterGroup> m_scatter;
    SplatEdit m_splatEdit;
    void endSplatEditing();
    void flushSplatUploads(VkCommandBuffer cmd);
    std::string textureNameForSlot(uint32_t slot) const;
    void prepareHeightEdits(LoadedMap& map, std::vector<ModelAsset>& models);
    // Delta de altura salvo -> grade .ter e vértices do chão (sem estado do motor:
    // também roda fora da thread principal, para os mapas vizinhos).
    static void applyHeightDelta(LoadedMap& map, std::vector<ModelAsset>& models, HeightEdit& h);
    void bindHeightEditToMeshes();
    // Monta o indice ponto da grade -> vertices do chao no primeiro uso do
    // pincel de relevo (ou de uma edicao de relevo), nao na carga.
    void ensureHeightEditRefs();
    static void applySavedWalk(LoadedMap& map);
    // Mundo contínuo
    struct WorldRegionLoad;
    // Modelo que a cena de um vizinho acrescenta, lido, convertido e montado
    // fora da thread principal (World.cpp); o addModelFromFile seguinte do
    // arquivo usa o pronto. Montar ali custava 2,3 s de quadro para 900
    // instâncias de 24 arquivos (converter e simplificar, não ler).
    struct ReadyModel {
        std::string file;
        std::shared_ptr<ModelFile> model; // imagens embutidas (vão para o mapa aberto)
        EmbeddedBakeResult bake;
        ModelAsset asset;
        std::vector<ModelRenderer::NodeSlotUse> slotUse;
        std::vector<ModelRenderer::NodeSlots> slots;
        ModelRenderer::PreparedAsset prepared;
    };
    static std::shared_ptr<ReadyModel> readModelForWorld(const std::string& glbPath); // qualquer thread
    bool modelTextureOnGpu(const std::string& name);
    std::shared_ptr<ModelFile> takePreparedModelFile(const std::string& glbPath);
    void applyModelBake(ModelFile& model, const std::string& glbPath);
    std::unordered_map<std::string, std::shared_ptr<ReadyModel>> m_readyModels;
    struct WorldRegion {
        std::string map;
        Vec3 offset{0.0f};
        float yaw = 0.0f; // graus em Y, em volta de offset
        Vec2 boundsMin{0.0f}, boundsMax{0.0f}; // caixa no mundo (com giro, a que contém o mapa girado)
        enum class State { Idle, Loading, Shown, Hidden, Failed } state = State::Idle;
        std::future<std::shared_ptr<WorldRegionLoad>> job;
        bool slotsResolved = false; // Loading: lido e com texturas resolvidas; a malha está sendo montada
        // Loading: modelos do mapa no lugar; a lista added entra aos poucos
        // (attachRegionAdded) e só depois o vizinho vira chão (mapAt).
        std::shared_ptr<WorldRegionLoad> attaching;
        size_t addedNext = 0;
        int attachFrames = 0;
        // Modelos do mapa no lugar, envio para a GPU no quadro seguinte
        // (attachRegion); as instancias ligadas esperam apagadas.
        bool uploadPending = false;
        int uploadDelayLeft = -1; // ERUPTION_TEST_UPLOAD_DELAY
        std::vector<uint32_t> uploadHidden;
        double slotsMs = 0.0, attachMs = 0.0, attachMaxMs = 0.0;
        std::shared_ptr<LoadedMap> data; // grade e caminhada já no lugar do mundo
        uint32_t firstInstance = 0, endInstance = 0;
        Mat4 place() const; // mapa -> mundo (deslocamento e giro)
        std::vector<uint8_t> enabled; // instâncias ligadas quando o vizinho saiu de cena
        std::vector<PointLight> lights;
        std::vector<std::string> characters;
        std::vector<std::string> areas;
        std::vector<std::string> sounds;
        bool hasWater = false;
        float waterLevel = 0.0f;
        TextureResource splatMask[2];
        TextureResource splatLayer[8];
        TextureResource splatLayerNormal[8]; // <camada>_normal (slot = cor + 1)
        TextureResource splatLayerMrahw[8];  // <camada>_mrahw (slot = cor + 2)
        VkSampler splatSampler = VK_NULL_HANDLE;
        std::chrono::steady_clock::time_point hiddenAt{};
        std::vector<std::string> modelFiles; // .glb do próprio mapa (malhas só dele)
        double vramAtLoad = 0.0, rssAtLoad = 0.0, heapAtLoad = 0.0; // MB no início da leitura (ERUPTION_TEST_CHUNK_STATS)
    };
    ImageData splatLayerImage(const std::filesystem::path& path);
    std::mutex m_splatDecodeMutex;
    std::unordered_map<std::string, ImageData> m_splatDecoded; // prefetchSplatLayers -> addSplatLayer
    // Camada do splat com os mapas ao lado (normal, MRAH-W): TerrainPaint.cpp.
    uint32_t uploadSplatLayer(TextureResource& colorTex, TextureResource& normalTex, TextureResource& mrahwTex,
                              const std::filesystem::path& colorPath, const ImageData& color);
    std::vector<WorldRegion> m_regions;
    // Leituras de vizinho de um mundo que saiu (restartWorld): terminam sozinhas
    // e o resultado e' descartado em updateWorld. Esperar por elas na troca de
    // mapa segurava o visor 3,6 s (de um mapa de porto para um mapa de floresta, pela tecla); o futuro de
    // std::async bloqueia no destrutor, por isso fica guardado ate' ficar pronto.
    std::vector<std::future<std::shared_ptr<WorldRegionLoad>>> m_retiredRegionJobs;
    void logChunkStats(const WorldRegion& r, const WorldRegionLoad& load); // ERUPTION_TEST_CHUNK_STATS (World.cpp)
    std::vector<TriggerArea> m_areas;
    std::vector<SceneRibbon> m_ribbons;
    std::vector<SoundEmitter> m_soundEmitters;
    bool m_soundEmittersActive = false;
    void updateSoundEmitters();
    AudioSystem m_audio;
    std::string m_playerSay;
    float m_playerSayTime = 0.0f;
    std::string m_playerAction; // animação extra do jogador (sprite v3)
    bool m_playerActionLoop = false;
    bool m_playerActionStarted = false;
    MapRequest m_mapRequest;
    std::vector<uint8_t> m_solid; // por instância: SurfaceKind (0 = não sólido)
    std::vector<std::pair<std::string, uint8_t>> m_solidFiles; // setSurfaceFile
    NavRules m_navRules;
    uint32_t m_navRulesVersion = 0; // muda a cada setNavRules: a grade de cada mapa refaz na hora
    void bakeNavRules(const LoadedMap& map) const;
    bool ruleCellWalkable(const LoadedMap& map, int gx, int gz) const;
    static const char* worldStateName(WorldRegion::State s);
    std::vector<WorldNeighbor> m_worldNeighbors;
    bool m_worldStarted = false;
    uint32_t m_worldVersion = 0;
    void resetWorld();
    void buildWorldGraph();
    void updateWorld();
    void refreshIrradianceProbes(); // ProbeRefresh.cpp
    void resolveRegionSlots(WorldRegionLoad& load);
    void uploadVfxGround(); // grade de altura do mapa aberto para as particulas
    bool m_vfxGroundDirty = false; // pincel de relevo mexeu: reenviar (no maximo a cada 30 frames)
    void attachRegion(WorldRegion& r, WorldRegionLoad& load);
    bool attachRegionAdded(WorldRegion& r, double budgetMs);
    void finishRegion(WorldRegion& r);
    void showRegion(WorldRegion& r, bool show);
    void rebuildWorldWater();
    void stitchGroundEdges();
    // Liga/desliga cada lado do avental da borda pelo vizinho em cena (World.cpp).
    void updateEdgeAprons();
    bool groundOfOtherMap(const LoadedMap* owner, float x, float z) const;
    void rebuildWorldLights();
    bool buildRegionSplat(WorldRegion& r, const std::vector<std::string>& layers);
    void releaseRegionSplat(WorldRegion& r, bool now = false); // now: GPU já parada
    void releaseRegion(WorldRegion& r);
    void refreshHeightRegion(int x0, int z0, int x1, int z1);
    float gridHeightAt(float wx, float wz, int x0, int z0, int x1, int z1, const std::vector<float>* old) const;
    void reprojectOnTerrain(int x0, int z0, int x1, int z1, const std::vector<float>& oldHeights);
    Vec3 m_playerStart{0.0f};
    bool loadCharacterSprite(const std::string& png, LoadedSprite& out);
    void drawCharacter(SceneCharacter& c, float dt);
    void tickCharacterFx(SceneCharacter& c, float dt);

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
