#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/CloudCoverageArray.hpp"
#include "formats/SmokeEmitter.hpp"
#include "math/Camera.hpp"
#include "math/Types.hpp"

namespace eruption {

// Cloud layer volumetric renderer.  Renders clouds as a fullscreen ray-march
// pass into a half-resolution off-screen target, then composites the result
// over the scene using depth-aware blending.
class CloudLayerRenderer {
public:
    enum class GodRayMode {
        Off = 0,
        PostProcess = 1,
        Physical = 2
    };

    struct Config {
        bool enabled = true;
        uint32_t layerCount = 5;
        uint32_t coverageSize = 1024;
        float cloudBottom = 500.0f;
        float cloudThickness = 800.0f;
        float coverageScale = 0.001f;
        float noiseScale = 0.005f;
        float detailStrength = 0.35f;
        float windSpeed = 0.5f;
        Vec3 windDirection = Vec3(1.0f, 0.0f, -0.5f);
        GodRayMode godRayMode = GodRayMode::PostProcess;
        uint32_t godRaySamples = 64;
        float godRayIntensity = 0.5f;
        bool castShadows = true;
        uint32_t shadowMapSize = 1024;
        bool temporalReprojection = false; // reserved for future
        uint32_t maxSteps = 96;
        float stepSize = 40.0f;
        // 0 = normal ray-march, 1 = visualize coverage array (layer 2) on the sky,
        // 2 = render a 3D horizontal coverage plane above the world.
        uint32_t debugMode = 0;
        bool debugPlane = true;
        bool debugShowMissColor = false;
        float debugPlaneY = 400.0f; // 0 = auto (cloudBottom + thickness/2)
        bool showCloudShadows = true;
        float cloudShadowOpacity = 1.0f;  // trim do usuario; a forca real vem de cloudShadowBaseStrength()
        float cloudShadowDistanceFade = 2000.0f;
        float cloudCoverage = 0.5f;         // global coverage amount; drives CloudCoverageNoise.
        float cloudAmount = 0.5f;           // user-facing "amount of clouds" slider (boosts coverage density).
        float cloudEdgeSoftness = 0.15f;

        // Visual tuning exposed by the skybox/weather UI.
        float cloudScale = 1.5f;       // multiplies noiseScale
        float cloudLightness = 1.0f;   // brightens cloud luminance
        float cloudShade = 0.55f;      // darkens cloud base/shadowed parts
        float cloudSoftness = 0.5f;    // softens coverage edges

        // Hybrid ray-march parameters (used at low pitch angles).
        float cloudRayMarchSteps = 250.0f;
        float cloudDebugThickness = 50.0f;
        float cloudSpikeHeight = 200.0f;
        float cloudBaseDepth = 50.0f;
        float cloudSpikeWidth = 300.0f;
    };

    CloudLayerRenderer() = default;
    ~CloudLayerRenderer();

    bool init(VulkanContext* ctx, uint32_t width, uint32_t height, const Config& cfg);
    void shutdown();
    void resize(uint32_t width, uint32_t height);

    void setConfig(const Config& cfg);
    const Config& config() const { return m_cfg; }

    // Weather-driven modulation (coverage amount + rain darkening).
    void setWeather(float cloudCoverage, float rainIntensity, float stormTint);

    // Update wind/coverage animation.
    void update(float dt);

    // Render clouds to internal half-res targets.
    void render(VkCommandBuffer cmd,
                const Camera& camera,
                const Vec3& sunDir,
                float sunIntensity,
                float time,
                uint32_t frameIndex);

    // Render debug coverage plane directly into the scene (uses scene depth).
    void renderDebugPlane(VkCommandBuffer cmd,
                          const Camera& camera,
                          VkImageView colorView,
                          VkImageView depthView,
                          uint32_t width,
                          uint32_t height);

    // Render cloud shadows projected onto the ground.
    void renderCloudShadows(VkCommandBuffer cmd,
                            const Camera& camera,
                            VkImageView colorView,
                            VkImageView depthView,
                            uint32_t width,
                            uint32_t height);

    // Composite cloud result over scene.  If godRayMode == PostProcess it is
    // applied here as well.
    void composite(VkCommandBuffer cmd,
                   VkImageView sceneColorView,
                   VkImageView sceneDepthView,
                   VkImageView outputView,
                   const Vec3& sunVisualDir);

    // Accessors for integration.
    VkImageView cloudColorView() const { return m_cloudColorView; }
    VkImageView cloudDepthView() const { return m_cloudDepthView; }
    CloudCoverageArray& coverageArray() { return m_coverageArray; }
    VulkanContext* context() const { return m_ctx; }

    // Independent cloud layers. Each layer is its own CloudCoverageArray (the
    // exact same class as the global layer) spawned isolated on screen, with
    // its own altitude and no tiling. The arrays are owned by the caller
    // (Engine); the renderer keeps per-layer UBO + descriptor sets.
    static constexpr uint32_t MAX_CLOUD_LAYERS = 128;
    uint32_t registerCloudLayer(CloudCoverageArray* array); // returns id, UINT32_MAX on failure
    void unregisterCloudLayer(uint32_t id);
    void updateCloudLayerUBO(uint32_t id); // refresh bounds/wind (per frame)
    uint32_t cloudLayerCount() const;
    // Per-layer rain look (push constants only; negative = use global weather).
    void setCloudLayerWeather(uint32_t id, float rainIntensity, float stormTint);

    // Render one independent layer as its own plane at planeY, and its ground
    // shadow. tintAltitude >= 0 enables the debug test tint (blue=low, red=high).
    // legacyVolume = the pre-cloud-field look (commit 0b48093): full ray-march
    // dome with the config spike values and the configured pitch threshold,
    // instead of the flat POM deck used by the weather field.
    void renderCloudLayerPlane(VkCommandBuffer cmd, uint32_t id, float planeY,
                               const Camera& camera, VkImageView depthView,
                               float tintAltitude, bool legacyVolume);
    // Volumetric puff: each local cloud draws its bounding box and the
    // fragment shader ray-marches a 3D ellipsoid eroded by the same 3D fbm
    // the coverage compute bakes (per-cloud fixed noiseZ). The plane stays
    // for the top-down footprint.
    void renderCloudLayerBillboard(VkCommandBuffer cmd, uint32_t id, float planeY,
                                   const Camera& camera, VkImageView depthView,
                                   float tintAltitude);
    void renderCloudLayerShadow(VkCommandBuffer cmd, uint32_t id, float planeY,
                                const Camera& camera, const Vec3& lightDir,
                                VkImageView depthView, uint32_t width, uint32_t height);

    // Coluna de fumaça (vulcão ativo). Mesmo caminho da nuvem local
    // volumétrica — cubo unitário + ray-march no fragment, mesmo pipeline
    // layout e mesmo VBO — mas com densidade de pluma analítica: sobe, é
    // advectada pelo vento, alarga com a altura e dissipa longe da fonte.
    // Não usa CloudCoverageArray: zero bake de compute, zero SSBO por emissor.
    // windXZ = velocidade do vento em m/s (o mesmo vento do clima).
    // maxSteps/sunTap/detail vêm do preset gráfico (custo controlado).
    struct SmokeQuality {
        int maxSteps = 32;   // teto de passos do ray-march
        bool sunTap = true;  // segunda amostra de densidade na direção do sol
        bool detail = true;  // 3a oitava do fbm
    };
    void renderSmokePlume(VkCommandBuffer cmd, const SmokeEmitter& emitter,
                          const Camera& camera, VkImageView depthView,
                          const Vec2& windXZ, const SmokeQuality& quality);
    bool hasSmokePipeline() const { return m_smokePipeline != VK_NULL_HANDLE; }

    // Half-res shadow accumulation (Frostbite-style): the per-cloud shadow
    // passes render into a half-resolution offscreen that accumulates the
    // occlusion in alpha (A = 1 - prod(1 - a_i), same blend math as the
    // direct passes), then a single fullscreen apply pass multiplies the lit
    // scene by (1 - A). begin returns false when unavailable -> caller falls
    // back to the direct full-res path. width/height are the ACCUM target
    // dims (half of the swap extent).
    bool beginCloudShadowAccum(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    void endCloudShadowAccumApply(VkCommandBuffer cmd, VkImageView litView,
                                  uint32_t width, uint32_t height);

    // Same half-res accumulation for the volumetric puffs (ray-marched
    // billboards): they render into a half-res offscreen with premultiplied
    // "over" accumulation (exact: no hardware depth test exists in the
    // billboard pipeline, occlusion is analytic in the fragment shader) and a
    // single composite pass blends the result over the scene.
    bool beginCloudVolumeAccum(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    void endCloudVolumeAccumApply(VkCommandBuffer cmd, VkImageView litView,
                                  uint32_t width, uint32_t height);

    bool hasShadowMap() const { return m_cfg.castShadows && m_shadowMapView != VK_NULL_HANDLE; }
    VkImageView shadowMapView() const { return m_shadowMapView; }
    VkSampler shadowMapSampler() const { return m_shadowMapSampler; }
    // Scene ambient (daynight) for the cloud-shadow base strength: base =
    // sun/(sun+ambient), so dense clouds darken like map geometry shadows.
    void setSceneAmbientIntensity(float ambient) { m_sceneAmbientIntensity = ambient; }
    // Half-res screen-space fluff occlusion (alpha) accumulated this frame;
    // valid only right after endCloudVolumeAccumApply (shader-read layout).
    VkImageView cloudVolumeAccumView() const { return m_volumeAccumView; }

private:
    VulkanContext* m_ctx = nullptr;
    Config m_cfg;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    float m_weatherCoverage = 1.0f;
    float m_rainIntensity = 0.0f;
    float m_stormTint = 0.0f;

    CloudCoverageArray m_coverageArray;

    // Independent cloud layers (see registerCloudLayer).
    struct CloudLayerSlot {
        CloudCoverageArray* array = nullptr; // owned by the caller
        // Per frame-in-flight CloudUBOs (same race fix as the global UBOs).
        VkBuffer ubo[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
        VmaAllocation uboAlloc[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
        void* uboMapped[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
        // Own plane vertex buffer: uploads happen at record time but the GPU
        // reads at execution time, so a shared buffer would make every layer
        // draw with the last layer's mesh.
        VkBuffer planeVbo = VK_NULL_HANDLE;
        VmaAllocation planeVboAlloc = VK_NULL_HANDLE;
        VkDescriptorSet set0[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {}; // global UBO (shared) + own CloudUBO, per frame
        VkDescriptorSet set1 = VK_NULL_HANDLE; // own coverage + altitude textures
        VkDescriptorSet set3 = VK_NULL_HANDLE; // own local-cloud SSBO (volumetric puff)
        float rainOverride = -1.0f; // >= 0: per-layer rain look instead of global weather
        float stormOverride = -1.0f;
        bool inUse = false;
    };
    std::vector<CloudLayerSlot> m_cloudLayers;

    // Half-res targets.
    VkImage m_cloudColorImage = VK_NULL_HANDLE;
    VmaAllocation m_cloudColorAlloc = VK_NULL_HANDLE;
    VkImageView m_cloudColorView = VK_NULL_HANDLE;

    VkImage m_cloudDepthImage = VK_NULL_HANDLE;
    VmaAllocation m_cloudDepthAlloc = VK_NULL_HANDLE;
    VkImageView m_cloudDepthView = VK_NULL_HANDLE;

    // God ray post-process target (full-res).
    VkImage m_godRayImage = VK_NULL_HANDLE;
    VmaAllocation m_godRayAlloc = VK_NULL_HANDLE;
    VkImageView m_godRayView = VK_NULL_HANDLE;

    // Cloud shadow map.
    VkImage m_shadowMapImage = VK_NULL_HANDLE;
    VmaAllocation m_shadowMapAlloc = VK_NULL_HANDLE;
    VkImageView m_shadowMapView = VK_NULL_HANDLE;
    VkSampler m_shadowMapSampler = VK_NULL_HANDLE;

    // Fullscreen quad geometry.
    VkBuffer m_quadBuffer = VK_NULL_HANDLE;
    VmaAllocation m_quadAlloc = VK_NULL_HANDLE;

    // Debug coverage plane geometry and pipeline.
    VkBuffer m_debugPlaneBuffer = VK_NULL_HANDLE;
    VmaAllocation m_debugPlaneAlloc = VK_NULL_HANDLE;
    VkPipeline m_debugPlanePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_debugPlanePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_smokePipeline = VK_NULL_HANDLE; // coluna de fumaça (reusa m_billboardPipelineLayout)
    VkPipeline m_billboardPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_billboardPipelineLayout = VK_NULL_HANDLE; // same 3 set layouts as debug plane + SSBO set, bigger push block
    // Set-3 layout/pool for the per-layer local-cloud SSBO the volumetric
    // puff shader reads (dedicated pool: the shared one has no STORAGE_BUFFER
    // budget).
    VkDescriptorSetLayout m_billboardCloudsLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_billboardCloudsPool = VK_NULL_HANDLE;
    // Static unit cube [0,1]^3 (36 verts, vec3) scaled per cloud in the vertex
    // shader via push constants; shared by all volumetric puff draws.
    VkBuffer m_billboardCubeVbo = VK_NULL_HANDLE;
    VmaAllocation m_billboardCubeVboAlloc = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_debugPlaneDepthLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_debugPlaneDepthPool = VK_NULL_HANDLE;
    VkDescriptorSet m_debugPlaneDepthSet = VK_NULL_HANDLE;
    // Last depth view written into the sets above: the update is skipped when
    // unchanged (it was re-written once per cloud per frame for 100+ clouds).
    VkImageView m_debugPlaneDepthViewCached = VK_NULL_HANDLE;

    // Cloud shadows plane geometry and pipeline (reuses debug plane geometry).
    VkPipeline m_cloudShadowsPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_cloudShadowsPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_cloudShadowsDepthLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_cloudShadowsDepthPool = VK_NULL_HANDLE;
    VkDescriptorSet m_cloudShadowsDepthSet = VK_NULL_HANDLE;
    VkImageView m_cloudShadowsDepthViewCached = VK_NULL_HANDLE;

    // Half-res shadow accumulation target + pipeline variant whose alpha
    // blend accumulates occlusion (ONE / ONE_MINUS_SRC_ALPHA) instead of
    // overwriting dst alpha. The apply pass re-applies it to the lit scene.
    VkImage m_shadowAccumImage = VK_NULL_HANDLE;
    VmaAllocation m_shadowAccumAlloc = VK_NULL_HANDLE;
    VkImageView m_shadowAccumView = VK_NULL_HANDLE;
    VkSampler m_shadowAccumSampler = VK_NULL_HANDLE;
    uint32_t m_shadowAccumWidth = 0;
    uint32_t m_shadowAccumHeight = 0;
    bool m_shadowAccumFirstUse = true;
    bool m_shadowAccumMode = false;
    VkPipeline m_cloudShadowAccumPipeline = VK_NULL_HANDLE;
    VkPipeline m_cloudShadowApplyPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_cloudShadowApplyPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_cloudShadowApplyLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_cloudShadowApplyPool = VK_NULL_HANDLE;
    VkDescriptorSet m_cloudShadowApplySet = VK_NULL_HANDLE;

    // Half-res volume accumulation target + billboard pipeline variant +
    // composite pipeline (see beginCloudVolumeAccum).
    VkImage m_volumeAccumImage = VK_NULL_HANDLE;
    VmaAllocation m_volumeAccumAlloc = VK_NULL_HANDLE;
    VkImageView m_volumeAccumView = VK_NULL_HANDLE;
    uint32_t m_volumeAccumWidth = 0;
    uint32_t m_volumeAccumHeight = 0;
    bool m_volumeAccumFirstUse = true;
    bool m_volumeAccumMode = false;
    VkPipeline m_billboardAccumPipeline = VK_NULL_HANDLE;
    VkPipeline m_volumeApplyPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_volumeApplyPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_volumeApplyLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_volumeApplyPool = VK_NULL_HANDLE;
    VkDescriptorSet m_volumeApplySet = VK_NULL_HANDLE;

    // Ray-march pipeline.
    VkPipeline m_raymarchPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // Per frame-in-flight: a single shared UBO raced with in-flight frames
    // (the cloud layer rendered with a newer camera matrix than the rest of
    // the scene -> clouds popped during camera motion).
    VkDescriptorSet m_descriptorSets[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};

    // Composite pipeline.
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compositeDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_compositeDescriptorSet = VK_NULL_HANDLE;

    VkSampler m_linearSampler = VK_NULL_HANDLE;

    struct UBO {
        Mat4 viewProj;
        Mat4 invViewProj;
        Vec3 cameraPos;
        float time;
        Vec3 sunDir;
        float sunIntensity;
        Vec2 screenSize;
        uint32_t frameIndex;
    };

    struct CloudUBO {
        Vec3 worldMin;
        float cloudBottom;
        Vec3 worldMax;
        float cloudTop;
        float layerSpacing;
        uint32_t layerCount;
        float noiseScale;
        float detailStrength;
        Vec3 windOffset;
        float extinctionScale;
        float scatteringAlbedo;
        float phaseG1;
        float phaseG2;
        float phaseAlpha;
        float powderStrength;
        float ambientIntensity;
        uint32_t godRayMode;
        uint32_t debugMode;
        float weatherCoverage;
        float rainIntensity;
        float stormTint;
        float cloudCoverageThreshold;
        float cloudScale;
        float cloudLightness;
        float cloudShade;
        float cloudSoftness;
        // Orçamento de passos do ray-march de nuvem. Ocupa o antigo pad1 (o
        // layout do UBO não muda). Motivo medido: "Cloud Volumes (field)" é o
        // passe de GPU mais caro em mapa típico - 1,15 ms em cidade-A e 1,60 ms
        // em vila-A na RTX 4060, contra 0,38 ms do GBuffer. Na 930M isso escala
        // para dezenas de ms. 0 = usa o máximo do shader.
        float marchStepBudget;
        float pad2;
    };

public:
    // Orçamento de passos do ray-march, vindo do preset gráfico
    // (cloud_march_steps em data/graphics.json). 0 = máximo do shader.
    void setMarchStepBudget(int steps) { m_marchStepBudget = steps; }
private:
    int m_marchStepBudget = 0;

    // Per frame-in-flight UBOs (see m_descriptorSets above).
    VkBuffer m_uboBuffers[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VmaAllocation m_uboAllocs[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    void* m_uboMapped[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};

    VkBuffer m_cloudUboBuffers[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VmaAllocation m_cloudUboAllocs[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    void* m_cloudUboMapped[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};

    bool createTargets();
    void destroyTargets();
    bool createShadowMap();
    void destroyShadowMap();
    bool createQuad();
    void destroyQuad();
    bool createDebugPlane();
    void destroyDebugPlane();
    bool createDebugPlanePipeline();
    void destroyDebugPlanePipeline();
    bool createBillboardPipeline();
    void destroyBillboardPipeline();
    bool createSmokePipeline();
    bool createDebugPlaneDepthDescriptor();
    void destroyDebugPlaneDepthDescriptor();
    void updateDebugPlaneDepthDescriptor(VkImageView depthView);
    bool createCloudShadowsPipeline();
    void destroyCloudShadowsPipeline();
    bool createCloudShadowsDepthDescriptor();
    void destroyCloudShadowsDepthDescriptor();
    void updateCloudShadowsDepthDescriptor(VkImageView depthView);
    bool createShadowAccumTarget(uint32_t width, uint32_t height);
    void destroyShadowAccumTarget();
    bool createCloudShadowApplyPipeline();
    void destroyCloudShadowApplyPipeline();
    bool createVolumeAccumTarget(uint32_t width, uint32_t height);
    void destroyVolumeAccumTarget();
    bool createVolumeApplyPipeline();
    void destroyVolumeApplyPipeline();
    bool createDescriptors();
    void destroyDescriptors();
    bool createPipelines();
    void destroyPipelines();
    bool createBuffers();
    void destroyBuffers();

    void updateUBOs(const Camera& camera, const Vec3& sunDir, float sunIntensity,
                    float time, uint32_t frameIndex);

    static VkSampler createLinearSampler(VulkanContext* ctx);

    // Scene light levels for the cloud-shadow base strength (see
    // setSceneAmbientIntensity); sun comes from updateUBOs every frame.
    float m_sceneSunIntensity = 1.0f;
    float m_sceneAmbientIntensity = 0.5f;
    float cloudShadowBaseStrength() const {
        float total = m_sceneSunIntensity + m_sceneAmbientIntensity;
        const float base = total > 1.0e-4f ? glm::clamp(m_sceneSunIntensity / total, 0.0f, 1.0f) : 0.0f;
        // Author feedback 2026-08-10: full base×density made rainy weathers too
        // dark — the cloud-shadow contribution is scaled to 30% of the base
        // (cloudShadowOpacity stays as the user-facing trim on top).
        return base * 0.3f;
    }
};

} // namespace eruption
