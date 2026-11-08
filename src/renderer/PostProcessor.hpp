#pragma once
#include "renderer/PushRing.hpp"
#include <map>

#include "renderer/VulkanContext.hpp"
#include "renderer/WeatherSystem.hpp"
#include "renderer/WeatherRenderer.hpp"
#include "renderer/DioramaLookConfig.hpp"
#include "math/Types.hpp"
#include "utils/SplineCurve.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>

namespace eruption {

struct PointLight;

class PostProcessor {
public:
    // Existia um `enum class DoFMode { DepthForeground, CircleOfConfusion }`
    // aqui. Eram DOIS caminhos de desfoque vivos ao mesmo tempo, e o legado
    // (tilt-shift por profundidade) era a versao pobre da mesma ideia: sem
    // curva de focal, sem curva de abertura, sem foco adaptativo. O CoC ganhou
    // e o legado foi apagado - um caminho so'.
    struct PostSettings {
        float exposure = 1.0f;
        // GPU histogram auto-exposure (luminance_histogram/adaptation compute).
        bool gpuAutoExposure = false;
        float aeTau = 2.0f;   // adaptation speed (1/s)
        float aeDt = 0.016f;  // frame delta time
        bool enableFog = true;
        float fogStart = 50.0f;
        float fogEnd = 1000.0f;
        float fogOpacity = 0.2f;
        Vec3 fogColor = Vec3(0.6f, 0.7f, 0.9f);
        float fogHeight = 0.0f;
        float fogHeightFalloff = 0.01f;
        // Escurecer so' o cenario (fx.dim): rgb = cor, a = forca. A UI vem depois.
        Vec4 sceneDim = Vec4(0.0f);
        // Fundo abaixo do horizonte onde nao ha' geometria: rgb = cor (antes
        // da exposicao), a = 1 liga (e tira esses pixels da autoexposicao).
        Vec4 belowHorizon = Vec4(0.0f);
        // BORDA-VAZIO (world_border do .env, ou world.set_border): fora do
        // retangulo [x0,z0]-[x1,z1] a tela funde para worldBorderParams.yzw
        // (cor linear) ao longo de worldBorderParams.x unidades.
        // worldBorderParams.x < 0 = desligada (padrao).
        Vec4 worldBorderRect = Vec4(0.0f);
        Vec4 worldBorderParams = Vec4(-1.0f, 0.0f, 0.0f, 0.0f);
        // INTERRUPTOR MESTRE DO DESFOQUE. Chamava-se `enableTiltShift`, o que
        // fez dois relatorios diferentes concluirem que "o tilt-shift esta'
        // desligado, por isso falta a pista de maquete" - errado nos dois: o
        // campo liga o DoF INTEIRO, e o preset o alimenta pela chave "dof".
        //
        // O que este DoF faz, em linguagem de diorama: um gradiente de
        // desfoque ancorado no PLANO DO CHAO, que e' a pista dominante de
        // "isto e' uma maquete fotografada" (Meese, Baker & Summers, PLOS ONE
        // 18(5) 2023 - o que importa e' a ORIENTACAO do gradiente em relacao
        // ao chao, mais que a qualidade do borrao). Os botoes:
        //   cocFocalDistance   - onde a "mesa da maquete" esta' em foco
        //   cocAperture        - tamanho da abertura; MAIOR = mais miniatura
        //   cocEnableForeground- borra o primeiro plano tambem; Meese et al.
        //                        mostram que essa metade do gradiente pesa
        //                        tanto quanto o fundo
        //   cocFocalCurve / cocApertureCurve - autoram os dois acima por zoom
        bool enableDoF = false;

        // Foco/alcance herdados: hoje so' alimentam a autoria por zoom.
        float focalOffset = 35.0f;
        float focalDistance = 100.0f;
        float focalRange = 80.0f;
        float dofAmount = 1.5f;
        float foregroundBlurAmount = 25.0f;
        float foregroundBleed = 1.5f;
        float radialStretch = 0.5f;

        // Circle of Confusion (New)
        float cocFocalDistance = 230.0f;
        float cocAperture = 0.05f;
        float cocMaxBlur = 3.0f;
        // Faixa em foco (unidades de mundo de cada lado do plano de foco, CoC
        // zero dentro dela) - coc_map.frag. Teto acima de 3 px liga o borrao
        // grande em 1/4 da resolucao (dof_far_*.frag).
        float cocFocusBand = 0.0f;
        float cocZoomFactor = 1.0f;
        // coc_model "thin_lens": CoC de lente fina, A*f*|z-zf|/(z*(zf-f)).
        // Com f e zf fixos isso e' K*|z-zf|/z: K e' o borrao no infinito
        // (coc_lens_blur), longe satura e perto cresce ate' o teto proprio.
        // O linear (|z-zf|*abertura) com a camera inclinada vira uma faixa
        // na tela: o borrao segue a altura da linha, nao a distancia.
        bool cocThinLens = false;
        float cocLensBlur = 2.0f;
        float cocMaxBlurNear = -1.0f; // < 0: coc_max_blur
        float cocMaxBlurFar = -1.0f;  // < 0: coc_max_blur
        float cocEdge = 0.0f;         // u de mundo: rampa suave depois da faixa em foco
        bool cocEnableForeground = true;
        bool visualizeCoC = false;

        // FOCO POR ASSUNTO: a lente aponta para ALGUMA COISA da cena, e nao
        // para o lugar onde a curva de zoom acha que ela deveria apontar.
        //
        // O mapeamento por zoom acima responde "o jogador aproximou quanto?".
        // E' a resposta certa para quem anda pelo mapa e nao tem assunto: a
        // maquete fica em foco por inteiro e o resto desmancha. Mas quando a
        // cena TEM um assunto — dois times de combatentes num palco, uma
        // mesa de dialogo, uma peca que o editor esta' inspecionando — quem
        // sabe onde esta' o assunto e' quem montou a cena, nao a curva.
        //
        // `focusDistance` e' profundidade de verdade (unidades de mundo ao
        // longo do eixo da camera), entao NAO passa pelo fator de lente: o
        // mapeamento existe para converter autoria feita numa abertura de
        // referencia, e aqui nao ha' autoria nenhuma para converter.
        bool focusOverride = false;
        float focusDistance = 0.0f;
        float focusAperture = -1.0f;   // < 0: mantem a abertura do mapeamento
        bool focusForeground = true;   // borra tambem o que esta' na FRENTE
        float focusBand = -1.0f;       // < 0: mantem cocFocusBand
        float focusMaxBlur = -1.0f;    // < 0: mantem cocMaxBlur
        // Visualizacao crua do G-buffer ligada (checkbox "Show Normal (RGB)" e
        // irmaos). Faz o composite pular fog/tonemap/bloom para a leitura nao
        // mudar com a hora do dia - ver post_composite.frag.
        bool pbrDebugActive = false;

        // Gotas e flocos por cima do DoF (antes do bloom). Antes do DoF eles
        // herdavam o desfoque do que esta' ATRAS (nao gravam profundidade):
        // com ceu ou fundo atras, neve de 4 px sumia.
        bool weatherAfterDoF = true;

        // Clima parado (nenhum efeito de tela do overlay e nenhuma gota ou
        // floco para desenhar): pula o "Weather Overlay", que so' copiaria a
        // cena para m_weatherImage (1 leitura + 1 escrita de tela cheia).
        // Preset "weather_skip_idle" / ERUPTION_WEATHER_SKIP_IDLE (=0 volta a
        // copiar sempre). Ligado: a imagem sai identica.
        bool weatherSkipIdle = true;

        // Weather debug: color rain particles by occlusion fate (DoF-style view).
        bool debugRainOcclusion = false;
        bool debugRainRegions = false;
        // Weather debug: render splash particles as big opaque red quads to
        // visualize the splash spawn area/density around the character.
        bool debugSplashArea = false;

        // A autoria por zoom (curvas abaixo) vive no espaco da abertura de
        // REFERENCIA de 60 graus: Camera::zoomPercent() converte distancia +
        // abertura numa distancia equivalente nessa lente, de proposito, para
        // que o mesmo enquadramento de' o mesmo numero. O shader de CoC, ao
        // contrario, compara com a profundidade REAL da cena. Com lente longa
        // (20 graus a 1000 unidades = 305 equivalentes) a curva pede foco a
        // 298 e a cena inteira fica 700 unidades atras do plano focal.
        // A conversao de volta e' feita no PostProcessor::render, por lente:
        // focal x tan(30)/tan(fov/2). Nos 60 graus do padrao o fator e' 1 e
        // nada muda. A ABERTURA nao e' convertida: medido na caverna, a lente
        // longa mostra so' ~200 unidades de profundidade de uma ponta a outra
        // do quadro, entao dividir a abertura pelo mesmo fator apaga o
        // gradiente (CoC abaixo de meio pixel). A forca do borrao e' autoria,
        // nao conversao - fica em coc_aperture_curve.
        bool cocEnableZoomMapping = true;
        SplineCurve cocFocalCurve = SplineCurve({0.0f, 0.25f, 0.5f, 0.75f, 1.0f}, {1000.0f, 750.0f, 500.0f, 250.0f, 1.0f});
        SplineCurve cocApertureCurve = SplineCurve({0.0f, 0.25f, 0.5f, 0.75f, 1.0f}, {0.05f, 0.162f, 0.275f, 0.388f, 0.5f});

        bool cocEnableAdaptiveFocal = true;
        enum class AdaptiveFocalMethod { SingleRay = 0, MultiRayGrid = 1, ScreenSpace = 2, AngularRadius = 3 };
        AdaptiveFocalMethod cocAdaptiveFocalMethod = AdaptiveFocalMethod::SingleRay;
        float cocAdaptiveFocalDamping = 0.05f; // 0..1 interpolation factor per frame at 60fps

        float bokehThreshold = 0.6f;
        float bokehIntensity = 2.0f;

        // Tone mapping: 0=legado (so' gamma), 1=AgX, 2=ACES, 3=Khronos Neutral.
        // Antes disto a engine nao tinha operador nenhum - ver post_composite.frag.
        int toneMapMode = 1;
        bool enableBloom = true;
        float bloomIntensity = 1.0f;
        float bloomThreshold = 1.0f;

        bool enableMotionBlur = true;
        float motionBlurAmount = 1.0f;
        // Tempo do quadro (s), escrito pelo Engine a cada quadro. O comprimento do
        // borrao de movimento vem do deslocamento ENTRE quadros, que cresce com o
        // tempo do quadro: com a banda/GPU reduzida (fps cai) o rastro ficava
        // maior e parecia imagem fantasma. Acima de motionBlurRefDt o borrao e'
        // escalado por refDt/dt (comprimento no tempo constante); ate' ali, igual.
        // 0 desliga a escala (ERUPTION_TEST_MB_DT_REF=0 para A/B).
        float frameDt = 1.0f / 60.0f;
        float motionBlurRefDt = 0.02f;

        bool enableChromaticAberration = true;
        float chromaticAberrationAmount = 0.5f;

        // RAIO LIMPO do borrao de movimento e da aberracao cromatica: um
        // circulo em volta do centro da tela (onde a camera poe o assunto)
        // fica sem os dois efeitos, e eles crescem suaves da borda dele para
        // fora. Raio e largura da transicao em fracao da ALTURA da tela;
        // raio 0 = sem mascara (o efeito vale na tela inteira, como antes).
        float effectClearRadius = 0.0f;
        float effectClearFeather = 0.25f;

        // Weather rendering tier
        WeatherTier weatherTier = WeatherTier::Medium;

        // Debug overlay that projects the cloud coverage map onto the scene.
        bool showCloudCoverageDebug = false;

        // Contorno fino por borda de profundidade (no Composite, sem passe extra). O modo e' chave de preset
        // (`outline`: 0 desliga, 1 lite com 4 taps, 2 completo com 8; `outline_sprites` 1 inclui os sprites,
        // padrao 0); cor, espessura e limiar vem de data/postprocess.json (`outline_color` RGBA em gamma de
        // tela, `outline_thickness` em px a 1080p, `outline_threshold` relativo a' distancia).
        int outlineMode = 0;
        bool outlineSprites = false;
        Vec4 outlineColor = Vec4(0.10f, 0.10f, 0.10f, 0.60f);
        float outlineThickness = 1.0f;
        float outlineThreshold = 0.02f;

        void loadFromJson(const nlohmann::json& j);
    };

    bool init(VulkanContext* ctx, uint32_t width, uint32_t height);
    void shutdown();

    WeatherRenderer& weatherRenderer() { return m_weatherRenderer; }
    void setWeatherSystem(const WeatherSystem* ws) { m_weatherSystem = ws; }
    // World point the rain heightmap + camera-relative splash group anchor
    // to (the orbit target / character). Sentinel y < -1e8 = use cameraPos.
    void setRainAnchor(const Vec3& p) { m_rainAnchor = p; }
    // Raios de luz das luzes pontuais (god_rays.frag): a lista do quadro e
    // quantas luzes no maximo (chave "god_ray_lights" do preset; 0 desliga).
    // A forca e o alcance vem do look (god_ray_*). Precisa do bloom ligado.
    void setGodRayLights(const std::vector<PointLight>* lights, uint32_t maxLights) {
        m_godRaySource = lights;
        m_godRayMaxLights = std::min(maxLights, 4u);
    }
    // Raios do sol no mesmo passe (chave "god_ray_sun" do preset: forca, 0 desliga).
    void setGodRaySun(float strength) { m_godRaySun = std::max(strength, 0.0f); }
    // Lens flare do sol no passe dos raios: forca (0 = desligado) e linhas
    // da estrela (o look pode trocar as duas).
    void setLensFlare(float strength, int rays) {
        m_lensFlare = std::max(strength, 0.0f);
        m_lensFlareRays = rays;
    }
    // Visao de equipe (core/Vision): textura R8 da janela e x=canto x, y=canto
    // z, z=1/lado, w=dim (< 0 = desligada, o composite nem amostra).
    void setVisionMap(VkImageView view, const Vec4& params) { m_visionView = view; m_visionParams = params; }
    // pbr do G-buffer: a marca de sprite que o borrao de movimento respeita.
    void setSpriteMark(VkImageView view) { m_spriteMarkView = view; }
    void resize(uint32_t width, uint32_t height);
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

    void render(VkCommandBuffer cmd, VkImageView hdrInput, VkImageView depthInput,
                VkImageView gbufferWorldPos, VkImageView gbufferNormal, VkImageView shadowInput,
                const Mat4& view, const Mat4& proj, const Mat4& invViewProj,
                const Mat4& prevViewProj,
                const Vec3& cameraPos, float nearPlane, float farPlane,
                const Vec3& lightDir, const PostSettings& settings,
                const DioramaLookConfig* lookConfig = nullptr,
                VkImageView targetOutput = VK_NULL_HANDLE,
                float time = 0.0f,
                const WeatherParams* weather = nullptr,
                float sunOcclusion = 1.0f,
                float moonOcclusion = 1.0f,
                VkImageView topDownDepthView = VK_NULL_HANDLE,
                const Vec3& sunVisualDir = Vec3(0.0f, 1.0f, 0.0f),
                const Vec3& cameraDir = Vec3(0.0f, 0.0f, 1.0f),
                VkImageView cloudColorView = VK_NULL_HANDLE,
                VkImageView cloudDepthView = VK_NULL_HANDLE,
                float cloudBaseHeight = 200.0f,
                const Vec3& cloudWorldMin = Vec3(-3000.0f, 0.0f, -3000.0f),
                const Vec3& cloudWorldMax = Vec3(3000.0f, 2000.0f, 3000.0f),
                VkImageView cloudCoverageView = VK_NULL_HANDLE,
                VkSampler cloudCoverageSampler = VK_NULL_HANDLE,
                float cloudLayer = 0.0f,
                float cloudAmount = 0.5f,
                float weatherCoverage = 1.0f,
                const Vec2& cloudWindOffset = Vec2(0.0f, 0.0f),
                VkImageView cloudAltitudeView = VK_NULL_HANDLE,
                VkSampler cloudAltitudeSampler = VK_NULL_HANDLE,
                float cloudBottom = 500.0f,
                float cloudTop = 1300.0f,
                float cloudScale = 1.5f,
                VkImageView cloudVolumeOccView = VK_NULL_HANDLE);

    void updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1 = VK_NULL_HANDLE, VkImageView view2 = VK_NULL_HANDLE, VkImageView view3 = VK_NULL_HANDLE);
    void updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1, VkImageView view2, VkImageView view3, VkImageView view5, VkImageView view4 = VK_NULL_HANDLE);
    void updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1, VkBuffer storageBuffer, VkDeviceSize storageRange, VkImageView view2 = VK_NULL_HANDLE, VkImageView view3 = VK_NULL_HANDLE);
    void updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1, VkImageView view2, VkImageView view3, VkSampler view3Sampler, VkBuffer storageBuffer, VkDeviceSize storageRange);

    VkImageView outputView() const { return m_outputView; }
    VkImage outputImage() const { return m_outputImage; }

private:
    VulkanContext* m_ctx = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

public:
    // Histogram auto-exposure result measured on the GPU last frame.
    // valid=false until the first adaptation dispatch has completed.
    float gpuAutoExposure(bool& valid) const { valid = m_gpuExposureValid; return m_gpuExposure; }
private:
    // Passe dos raios de luz (depois do upsample do bloom): devolve a imagem
    // bloom + raios, ou VK_NULL_HANDLE quando nao ha' luz para desenhar.
    VkImageView renderGodRays(VkCommandBuffer cmd, const Mat4& viewProj, const Mat4& proj,
                              const PostSettings& settings, const DioramaLookConfig& look,
                              VkImageView depthInput, VkImageView hdrInput, const Vec3& lightDir);
    static constexpr uint32_t AE_FRAMES = 3;
    VkPipeline m_lumHistPipeline = VK_NULL_HANDLE;
    VkPipeline m_lumAdaptPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_lumHistLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_lumAdaptLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lumHistDescLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lumAdaptDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_lumDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_lumHistSets[AE_FRAMES] = {};
    VkDescriptorSet m_lumAdaptSet = VK_NULL_HANDLE;
    VkBuffer m_lumHistBuffer = VK_NULL_HANDLE;
    VmaAllocation m_lumHistAlloc = VK_NULL_HANDLE;
    VkBuffer m_lumExpBuffer = VK_NULL_HANDLE;
    VmaAllocation m_lumExpAlloc = VK_NULL_HANDLE;
    void* m_lumExpMapped = nullptr;
    VkSampler m_lumSampler = VK_NULL_HANDLE;
    VkSampler m_lumDepthSampler = VK_NULL_HANDLE; // NEAREST: D32 sem filtro linear no celular
    float m_gpuExposure = 1.0f;
    bool m_gpuExposureValid = false;
    void createAutoExposure();
    void destroyAutoExposure();
    void dispatchAutoExposure(VkCommandBuffer cmd, VkImageView hdrView, VkImageView depthView,
                              const Mat4& invViewProj, const Vec3& cameraPos, bool skipBelowHorizon,
                              float dt, float tau);

    VkImage m_outputImage = VK_NULL_HANDLE;
    VmaAllocation m_outputAlloc = VK_NULL_HANDLE;
    VkImageView m_outputView = VK_NULL_HANDLE;

    VkImage m_dofImage = VK_NULL_HANDLE;
    VmaAllocation m_dofAlloc = VK_NULL_HANDLE;
    VkImageView m_dofView = VK_NULL_HANDLE;

    // Bloom Pyramid (5 levels)
    static constexpr uint32_t BLOOM_MIPS = 5;
    VkImage m_bloomDownImages[BLOOM_MIPS] = {};
    VmaAllocation m_bloomDownAllocs[BLOOM_MIPS] = {};
    VkImageView m_bloomDownViews[BLOOM_MIPS] = {};
    
    VkImage m_bloomUpImages[BLOOM_MIPS] = {};
    VmaAllocation m_bloomUpAllocs[BLOOM_MIPS] = {};
    VkImageView m_bloomUpViews[BLOOM_MIPS] = {};

    // CoC Pipeline Resources
    VkImage m_cocImage = VK_NULL_HANDLE;
    VmaAllocation m_cocAlloc = VK_NULL_HANDLE;
    VkImageView m_cocView = VK_NULL_HANDLE;

    VkImage m_blurHImage = VK_NULL_HANDLE;
    VmaAllocation m_blurHAlloc = VK_NULL_HANDLE;
    VkImageView m_blurHView = VK_NULL_HANDLE;

    VkImage m_blurVImage = VK_NULL_HANDLE;
    VmaAllocation m_blurVAlloc = VK_NULL_HANDLE;
    VkImageView m_blurVView = VK_NULL_HANDLE;

    VkImage m_weatherImage = VK_NULL_HANDLE;
    VmaAllocation m_weatherAlloc = VK_NULL_HANDLE;
    VkImageView m_weatherView = VK_NULL_HANDLE;

    VkPipeline m_cocPipeline = VK_NULL_HANDLE;
    // Borrao grande do DoF em 1/4 da resolucao: [0] cena pesada pelo CoC,
    // [1] horizontal, [2] vertical (dof_far_*.frag).
    static constexpr uint32_t kDofFarImages = 3;
    VkImage m_dofFarImages[kDofFarImages] = {};
    VmaAllocation m_dofFarAllocs[kDofFarImages] = {};
    VkImageView m_dofFarViews[kDofFarImages] = {};
    VkPipeline m_dofFarDownPipeline = VK_NULL_HANDLE;
    VkPipeline m_dofFarBlurPipeline = VK_NULL_HANDLE;
    VkPipeline m_blurHPipeline = VK_NULL_HANDLE;
    VkPipeline m_blurVPipeline = VK_NULL_HANDLE;
    VkPipeline m_cocCompositePipeline = VK_NULL_HANDLE;
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;
    VkPipeline m_copyPipeline = VK_NULL_HANDLE;

    VkPipeline m_brightPassPipeline = VK_NULL_HANDLE;
    VkPipeline m_godRayPipeline = VK_NULL_HANDLE;
    VkImage m_godRayImage = VK_NULL_HANDLE; // bloom + raios, meia resolucao
    VmaAllocation m_godRayAlloc = VK_NULL_HANDLE;
    VkImageView m_godRayView = VK_NULL_HANDLE;
    const std::vector<PointLight>* m_godRaySource = nullptr;
    uint32_t m_godRayMaxLights = 0;
    float m_godRaySun = 0.0f;
    float m_lensFlare = 0.0f;
    int m_lensFlareRays = 6;
    VkImageView m_visionView = VK_NULL_HANDLE;
    Vec4 m_visionParams = Vec4(0.0f, 0.0f, 0.0f, -1.0f);
    VkImageView m_spriteMarkView = VK_NULL_HANDLE;
    VkPipeline m_bloomDownPipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomUpPipeline = VK_NULL_HANDLE;
    VkPipeline m_weatherPipeline = VK_NULL_HANDLE;
    VkPipeline m_cloudCoverageDebugPipeline = VK_NULL_HANDLE;
    WeatherRenderer m_weatherRenderer;
    const WeatherSystem* m_weatherSystem = nullptr;
    Vec3 m_rainAnchor = Vec3(0.0f, -1.0e9f, 0.0f); // see setRainAnchor

    // Small UBO for data that does not fit into the fragment push-constant limit.
    // Layout must match WeatherUBO in weather_overlay.frag. Beyond the matrix it
    // carries the rain-cloud footprints (rain followers) so the overlay can gate
    // splash/wetness to pixels actually under a rain cloud — the global coverage
    // array does not contain the independent rain clouds (author feedback
    // 2026-08-09).
    static constexpr uint32_t SPLASH_CLOUD_MAX = 32;
    struct WeatherOverlayUBO {
        Mat4 invViewProj;
        Vec4 splashClouds[SPLASH_CLOUD_MAX];  // xy = box center XZ, zw = half extents (zw ~ 0 = empty)
        Vec4 splashCloudsB[SPLASH_CLOUD_MAX]; // x = cloud plane Y (boxCenter.y) for view-ray occlusion
        Vec4 splashCloudInfo;                 // x = active count
        Vec4 splashOccParams;                 // x = 1: binding 7 (cloud volume occlusion) is live this frame
        // TORNADO/TROMBA/DOWNBURST, ancorado no MUNDO. Vai no UBO e nao no push
        // porque o push da overlay ja' usa 256 bytes, que e' EXATAMENTE o
        // maxPushConstantsSize desta GPU - nao cabe mais um vec4. (Vale notar:
        // no iGPU Intel desta mesma maquina o limite e' 128, entao esse push
        // ja' nao caberia la'.)
        Vec4 tornadoA;   // xyz = base do funil em mundo, w = altura total
        Vec4 tornadoB;   // x = raio na base, y = raio no topo, z = giro, w = livre
    };
    VkBuffer m_weatherUbo = VK_NULL_HANDLE;
    VmaAllocation m_weatherUboAlloc = VK_NULL_HANDLE;
    void* m_weatherUboMapped = nullptr;
    // Base do funil (tornado/tromba/rajada) em coordenadas de MUNDO. Nasce
    // perto do jogador na primeira vez e deriva sozinha - ver PostProcessor.cpp.
    Vec3 m_tornadoBase = Vec3(0.0f);
    bool m_tornadoPlaced = false;

    // Lens-drop SSBO (CPU upload or GPU compute target)
    static constexpr uint32_t LENS_DROP_MAX = 16384;
    // Bindings de buffer do overlay (4/6) escritos uma vez (VUID-03047).
    bool m_weatherBufferDSWritten[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    // Ultima escrita por (set, binding): ver updateSets.
    struct WrittenDescriptor {
        VkImageView view; VkSampler sampler; VkImageLayout layout;
        VkBuffer buffer; VkDeviceSize offset, range;
        bool operator==(const WrittenDescriptor& o) const {
            return view == o.view && sampler == o.sampler && layout == o.layout && buffer == o.buffer &&
                   offset == o.offset && range == o.range;
        }
    };
    std::map<std::pair<VkDescriptorSet, uint32_t>, WrittenDescriptor> m_writeCache;
    void updateSets(const VkWriteDescriptorSet* writes, uint32_t count);
    VkBuffer m_lensDropBuffer = VK_NULL_HANDLE;
    VmaAllocation m_lensDropAlloc = VK_NULL_HANDLE;
    void* m_lensDropMapped = nullptr;

    // GPU compute update pipeline
    VkPipeline m_lensDropComputePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_lensDropComputeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lensDropComputeLayoutDesc = VK_NULL_HANDLE;
    VkDescriptorPool m_lensDropComputePool = VK_NULL_HANDLE;
    VkDescriptorSet m_lensDropComputeSet = VK_NULL_HANDLE;

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    // Blocos grandes (composicao final e overlay do clima, 256 B): set 1 via
    // PushRing. O resto dos passes segue em push constant de ate' 128 B.
    PushRing m_pushRing;

    // Temporal smoothing for heat shimmer time to reduce sub-pixel flicker.
    float m_heatTimeSmooth = 0.0f;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets; // kSetsPerFrame por frame em voo
    static constexpr uint32_t kSetsPerFrame = 32;
    VkDescriptorSet& dset(uint32_t i) { return m_descriptorSets[m_ctx->currentFrame() * kSetsPerFrame + i]; }
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkSampler m_pointSampler = VK_NULL_HANDLE;
    VkSampler samplerFor(VkImageView view) const;

    void createImages();
    void createPipelines();
    void createDescriptors();
    void createWeatherUbo();
    void destroyWeatherUbo();
    void createLensDropBuffer();
    void destroyLensDropBuffer();
    void createLensDropCompute();
    void destroyLensDropCompute();
    void dispatchLensDropCompute(VkCommandBuffer cmd, const WeatherParams& weather, float time, float dt);
    VkPipeline createFullscreenPipeline(const std::vector<uint32_t>& fragCode,
                                         uint32_t w, uint32_t h,
                                         bool needDepth = false,
                                         VkFormat colorFormat = VK_FORMAT_UNDEFINED,
                                         bool enableBlend = false);
};

} // namespace eruption
