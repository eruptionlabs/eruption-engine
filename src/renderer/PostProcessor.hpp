#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/WeatherSystem.hpp"
#include "renderer/WeatherRenderer.hpp"
#include "renderer/DioramaLookConfig.hpp"
#include "math/Types.hpp"
#include "utils/SplineCurve.hpp"
#include <nlohmann/json.hpp>

namespace eruption {

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
        float cocZoomFactor = 1.0f;
        bool cocEnableForeground = true;
        bool visualizeCoC = false;
        // Visualizacao crua do G-buffer ligada (checkbox "Show Normal (RGB)" e
        // irmaos). Faz o composite pular fog/tonemap/bloom para a leitura nao
        // mudar com a hora do dia - ver post_composite.frag.
        bool pbrDebugActive = false;

        // Weather debug: color rain particles by occlusion fate (DoF-style view).
        bool debugRainOcclusion = false;
        bool debugRainRegions = false;
        // Weather debug: render splash particles as big opaque red quads to
        // visualize the splash spawn area/density around the character.
        bool debugSplashArea = false;

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

        bool enableChromaticAberration = true;
        float chromaticAberrationAmount = 0.5f;

        // Weather rendering tier
        WeatherTier weatherTier = WeatherTier::Medium;

        // Debug overlay that projects the cloud coverage map onto the scene.
        bool showCloudCoverageDebug = false;

        void loadFromJson(const nlohmann::json& j);
    };

    bool init(VulkanContext* ctx, uint32_t width, uint32_t height);
    void shutdown();

    WeatherRenderer& weatherRenderer() { return m_weatherRenderer; }
    void setWeatherSystem(const WeatherSystem* ws) { m_weatherSystem = ws; }
    // World point the rain heightmap + camera-relative splash group anchor
    // to (the orbit target / character). Sentinel y < -1e8 = use cameraPos.
    void setRainAnchor(const Vec3& p) { m_rainAnchor = p; }
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
    float m_gpuExposure = 1.0f;
    bool m_gpuExposureValid = false;
    void createAutoExposure();
    void destroyAutoExposure();
    void dispatchAutoExposure(VkCommandBuffer cmd, VkImageView hdrView, float dt, float tau);

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
    VkPipeline m_blurHPipeline = VK_NULL_HANDLE;
    VkPipeline m_blurVPipeline = VK_NULL_HANDLE;
    VkPipeline m_cocCompositePipeline = VK_NULL_HANDLE;
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;
    VkPipeline m_copyPipeline = VK_NULL_HANDLE;

    VkPipeline m_brightPassPipeline = VK_NULL_HANDLE;
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
    bool m_weatherBufferDSWritten = false;
    VkBuffer m_lensDropBuffer = VK_NULL_HANDLE;
    VmaAllocation m_lensDropAlloc = VK_NULL_HANDLE;
    void* m_lensDropMapped = nullptr;

    // GPU compute update pipeline
    VkPipeline m_lensDropComputePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_lensDropComputeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_lensDropComputeLayoutDesc = VK_NULL_HANDLE;
    VkDescriptorPool m_lensDropComputePool = VK_NULL_HANDLE;
    VkDescriptorSet m_lensDropComputeSet = VK_NULL_HANDLE;
    VkBuffer m_lensDropParamBuffer = VK_NULL_HANDLE;
    VmaAllocation m_lensDropParamAlloc = VK_NULL_HANDLE;

    VkPipelineLayout m_layout = VK_NULL_HANDLE;

    // Temporal smoothing for heat shimmer time to reduce sub-pixel flicker.
    float m_heatTimeSmooth = 0.0f;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
    VkSampler m_linearSampler = VK_NULL_HANDLE;

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
