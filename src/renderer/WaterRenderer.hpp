#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "math/Types.hpp"
#include "utils/DayNightCycle.hpp"
#include "formats/TerrainParser.hpp"
#include <vector>
#include <array>
#include <cstdint>

namespace eruption {

class WaterRenderer {
public:
    static constexpr int MAX_WATER_LAYERS = 5;

    enum class LiquidKind : int {
        Water = 0,
        Lava  = 1,
    };
    static constexpr int kLiquidKindCount = 2;
    static const char* liquidKindName(int kind) {
        switch (kind) {
            case 1:  return "Lava";
            default: return "Water";
        }
    }

    struct WaterSettings {
        bool enabled = true;

        int liquidKind = 0;
        Vec3 emissiveColor = Vec3(1.0f, 0.32f, 0.07f);
        float emissiveStrength = 0.0f;
        float flowSpeed = 1.0f;

        // Water level
        float waterLevel = 0.0f;

        // Colors (deep = profundo, shallow = raso) — sky-matched grey-blue
        Vec3 baseColorDeep    = Vec3(0.05f, 0.08f, 0.12f);
        Vec3 baseColorShallow = Vec3(0.18f, 0.25f, 0.28f);

        // PBR surface
        float transparency       = 0.75f;  // 0=opaco, 1=transparente
        float refractionStrength = 0.02f;  // força da distorção de refração
        float reflectivity       = 0.6f;   // força da reflection do céu
        float roughness          = 0.15f;  // 0=espelho, 1=difuso (afeta specular)

        // Normal map (procedural ou textura)
        float normalScale   = 1.0f;
        float normalStrength = 0.8f;
        int   normalOctaves = 3;
        bool useProceduralTexture = true;
        std::string waterTexturePath = "";

        // Gerstner waves
        float waveAmplitude  = 0.3f;
        float waveFrequency  = 1.2f;
        float waveSpeed      = 0.8f;
        std::array<Vec2, 3> waveDirections = {
            Vec2(1.0f, 0.0f),
            Vec2(0.7f, 0.7f),
            Vec2(-0.3f, 0.9f)
        };
        std::array<float, 3> waveSteepness = { 0.5f, 0.4f, 0.3f };

        // Caustics
        bool enableCaustics = true;
        float causticsIntensity = 0.25f;
        float causticsDepthAttenuation = 0.35f;

        // Surface Foam (Worley-based)
        bool enableSurfaceFoam = true;
        float foamScale = 1.0f;
        float foamSpeed = 0.5f;
        float foamRoughness = 0.85f;

        // Edge / Contact Foam
        bool enableFoam = true;
        float foamEdgeDepth = 0.5f;
        float contactFoamStrength = 2.0f;
        Vec2 foamMaskScale = Vec2(256.0f, 256.0f);
        float contactFoamDecay = 0.04f;
        float contactFoamSpread = 0.0f;

        // Debug / map overrides
        bool forceWater = false;
    };

    bool init(VulkanContext* ctx, BindlessDescriptor* bindless);
    void shutdown();

    void setWaterMesh(const WaterMesh& mesh);
    void clearWaterMesh();

    void setLavaMesh(const WaterMesh& mesh);
    void clearLavaMesh();
    bool hasLava() const { return m_lavaIndexCount > 0; }

    // Call BEFORE the forward rendering pass begins.
    // Recria as texturas de refracao no tamanho novo. Chamar no resize do
    // motor (GPU ociosa); recriar dentro do frame destruiria views em uso.
    void resizeRefraction(uint32_t width, uint32_t height);
    void prepareRefraction(VkCommandBuffer cmd,
                           VkImage sceneColorImage, VkImage sceneDepthImage,
                           uint32_t sceneWidth, uint32_t sceneHeight,
                           float nearPlane, float farPlane,
                           const Mat4& invViewProj,
                           const WaterSettings& settings);

    void render(VkCommandBuffer cmd, const Mat4& viewProj, const Vec3& cameraPos,
                const DayNightCycle& cycle, float time,
                float nearPlane, float farPlane,
                const WaterSettings& settings,
                uint32_t abTestMask = 0xFFFFFFFFu,
                float cameraPitch = 0.0f,
                float sunOcclusion = 1.0f,
                float moonOcclusion = 1.0f,
                float stormDarken = 0.0f,
                float rainIntensity = 0.0f,
                float rainSplashIntensity = 0.0f,
                bool drawLavaSurface = false);

    void setViewport(float width, float height);

    // Update the dynamic contact-foam mask. Call once per frame before prepareRefraction().
    void updateContactFoam(float deltaTime, const WaterSettings& settings);

    // Paint a contact-foam circle at a world position. World Y is ignored (projected to water surface).
    void addContactFoam(const Vec3& worldPos, float radius, float intensity = 1.0f);

    // Convenience: paint contact foam from a world-space AABB (e.g. character foot splash).
    void addContactFoamAABB(const Vec3& minPos, const Vec3& maxPos, float intensity = 1.0f);

    // Paint a trail of foam between two world positions (e.g. moving through water)
    void addContactFoamTrail(const Vec3& from, const Vec3& to, float radius, float intensity = 1.0f);

    bool hasWater() const { return m_indexCount > 0; }

    VkBuffer m_lavaVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_lavaVertexAlloc = nullptr;
    VkBuffer m_lavaIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_lavaIndexAlloc = nullptr;
    uint32_t m_lavaIndexCount = 0;

    // Shader hot-reload
    void reloadShaders();

    // Sky texture for reflection
    void setSkyTexture(VkImageView view, VkSampler sampler);
    void clearSkyTexture();

    // Water noise texture helpers
    void generateBlueNoiseTexture(int size);
    void loadWaterTextureFromFile(const char* path);
    void clearWaterTexture() { destroyWaterTextureResources(); }

private:
    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;

    static constexpr uint32_t MAX_FRAMES = 3;
    // Dois BANCOS de UBO por frame. A agua e a lava sao dois draws no MESMO
    // frame com conteudo de UBO diferente; com um banco so' o memcpy da lava
    // sobrescrevia o da agua ANTES de a GPU executar o draw da agua, e a
    // agua inteira do mapa passava a ser desenhada com o waterLevel e o
    // emissivo da lava - era isso que inundava a tela (brilho medio do frame
    // 53 -> 160, medido isolando so' esta chamada).
    static constexpr uint32_t UBO_BANKS = 2;   // 0 = agua, 1 = lava
    VkDescriptorSetLayout m_uboLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_uboPool = VK_NULL_HANDLE;
    VkDescriptorSet m_uboSets[MAX_FRAMES * UBO_BANKS] = {};

    struct WaterUBO {
        Vec4 baseColorDeep;       // xyz=cor profunda
        Vec4 baseColorShallow;    // xyz=cor rasa
        Vec4 waterParams;         // x=transparency, y=refractionStrength, z=reflectivity, w=roughness
        Vec4 normalParams;        // x=scale, y=waveSpeed, z=strength, w=unused
        Vec4 waveParams;          // x=amplitude, y=frequency, z=speed, w=unused
        Vec4 foamParams;          // x=edgeDepth, y=contactStrength, z=enable, w=unused
        Vec4 skyTop;
        Vec4 skyHorizon;
        Vec4 sunDirIntensity;     // xyz=direção sol, w=intensidade
        Vec4 sunColor;            // xyz=cor do sol, w=intensidade
        Vec4 ambientColor;        // xyz=cor ambiente, w=intensidade
        Vec4 cameraPos;           // xyz=posição câmera
        Vec4 waterLevelAndPlanes; // x=waterLevel, y=nearPlane, z=farPlane, w=time
        Vec4 screenSize;          // x=viewportW, y=viewportH, z=cameraPitch, w=unused
        Vec4 waveDirAmp[3];       // xy=direção, z=amp, w=freq
        Vec4 waveSpeedSteep[3];   // x=speed, y=steepness, zw=unused
        IVec4 textureSlots;       // x=screenTex, y=depthTex, z=foamMask, w=waterTex
        IVec4 skyboxSlot;         // x=skyboxTex, yzw=unused
        Mat4 invViewProj;         // reconstrução de posição mundo a partir de depth
        Vec4 extraParams;         // x=absorptionCoeff, y=maxPxScale, z=enableCaustics, w=unused
        Vec4 foamSurfaceParams;   // x=foamScale, y=foamSpeed, z=foamRoughness, w=enableSurfaceFoam
        Vec4 normalAdvanced;      // x=normalOctaves, yzw=unused
        Vec4 causticsParams;      // x=intensity, y=depthAttenuation, zw=unused
        Vec4 weatherParams;       // x=sunOcclusion, y=moonOcclusion, z=stormDarken, w=unused
    };
    static_assert(sizeof(WaterUBO) <= 1024, "WaterUBO too large");

    VkBuffer m_uboBuffers[MAX_FRAMES * UBO_BANKS] = {};
    VmaAllocation m_uboAllocs[MAX_FRAMES * UBO_BANKS] = {};
    void* m_uboMappeds[MAX_FRAMES * UBO_BANKS] = {};

    // Refraction copy of the lit scene (one per frame in flight)
    std::array<VkImage, MAX_FRAMES> m_refractionImage = {};
    std::array<VmaAllocation, MAX_FRAMES> m_refractionAlloc = {};
    std::array<VkImageView, MAX_FRAMES> m_refractionView = {};
    VkSampler m_refractionSampler = VK_NULL_HANDLE;
    std::array<uint32_t, MAX_FRAMES> m_refractionSlot = {};

    std::array<VkImage, MAX_FRAMES> m_depthCopyImage = {};
    std::array<VmaAllocation, MAX_FRAMES> m_depthCopyAlloc = {};
    std::array<VkImageView, MAX_FRAMES> m_depthCopyView = {};
    VkSampler m_depthCopySampler = VK_NULL_HANDLE;
    std::array<uint32_t, MAX_FRAMES> m_depthCopySlot = {};

    uint32_t m_refractionWidth = 0;
    uint32_t m_refractionHeight = 0;

    // Edge foam persistence accumulation (screen-space ping-pong)
    static constexpr uint32_t FOAM_ACCUM_COUNT = 2;
    std::array<VkImage, FOAM_ACCUM_COUNT> m_foamAccumImage = {};
    std::array<VmaAllocation, FOAM_ACCUM_COUNT> m_foamAccumAlloc = {};
    std::array<VkImageView, FOAM_ACCUM_COUNT> m_foamAccumView = {};
    VkSampler m_foamAccumSampler = VK_NULL_HANDLE;
    std::array<uint32_t, FOAM_ACCUM_COUNT> m_foamAccumSlot = {};
    uint32_t m_currentFoamAccumIndex = 0;
    uint32_t m_foamAccumWidth = 0;
    uint32_t m_foamAccumHeight = 0;

    VkPipeline m_foamAccumPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_foamAccumLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_foamAccumDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_foamAccumDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_foamAccumDescSet = VK_NULL_HANDLE;

    // Dynamic contact-foam mask
    // Reduced from 512 to keep contact-foam CPU cost inside the 16.6 ms budget
    // during heavy weather (thunderstorm + DoF + shadows).
    static constexpr uint32_t FOAM_MASK_SIZE = 256;
    VkImage m_foamMaskImage = VK_NULL_HANDLE;
    VmaAllocation m_foamMaskAlloc = VK_NULL_HANDLE;
    VkImageView m_foamMaskView = VK_NULL_HANDLE;
    VkSampler m_foamMaskSampler = VK_NULL_HANDLE;
    uint32_t m_foamMaskSlot = 0;
    uint32_t m_skyboxSlot = 0xFFFFFFFFu;

    std::vector<uint8_t> m_foamMaskData;
    std::vector<float> m_foamBlurTmp;
    std::vector<float> m_foamBlurOut;
    bool m_foamMaskDirty = false;

    VkBuffer m_foamMaskStagingBuffer = VK_NULL_HANDLE;
    VmaAllocation m_foamMaskStagingAlloc = VK_NULL_HANDLE;

    Vec2 m_foamMaskScale = Vec2(64.0f, 64.0f);
    float m_contactFoamSpread = 0.0f;
    Mat4 m_invViewProj = Mat4(1.0f);

    // Optional water noise/normal texture (procedural blue noise or loaded from file)
    VkImage m_waterTexture = VK_NULL_HANDLE;
    VmaAllocation m_waterTextureAlloc = VK_NULL_HANDLE;
    VkImageView m_waterTextureView = VK_NULL_HANDLE;
    VkSampler m_waterTextureSampler = VK_NULL_HANDLE;
    uint32_t m_waterTextureSlot = 0xFFFFFFFFu;

    // Underwater RTT resources (prepared for future projective texture use)
    std::array<VkImage, MAX_FRAMES> m_underwaterImage = {};
    std::array<VmaAllocation, MAX_FRAMES> m_underwaterAlloc = {};
    std::array<VkImageView, MAX_FRAMES> m_underwaterView = {};
    VkSampler m_underwaterSampler = VK_NULL_HANDLE;
    std::array<uint32_t, MAX_FRAMES> m_underwaterSlot = {};
    uint32_t m_underwaterWidth = 0;
    uint32_t m_underwaterHeight = 0;

    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_vertexAlloc = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_indexAlloc = VK_NULL_HANDLE;
    uint32_t m_indexCount = 0;

    float m_viewportW = 1920.0f;
    float m_viewportH = 1080.0f;

    struct WaterPushConstants {
        Mat4 viewProj;
        IVec4 debugMask; // x=abTestMask, yzw=unused
    };

    void createPipeline();
    void destroyPipeline();
    void createUBO();
    void destroyUBO();
    void createRefractionResources(uint32_t width, uint32_t height);
    void destroyRefractionResources();
    void createFoamAccumResources(uint32_t width, uint32_t height);
    void destroyFoamAccumResources();
    void createFoamAccumPipeline();
    void destroyFoamAccumPipeline();
    void dispatchFoamAccum(VkCommandBuffer cmd, VkImage depthImage, uint32_t depthSlot,
                           float nearPlane, float farPlane, float foamEdgeDepth,
                           float waterLevel, const Mat4& invViewProj,
                           const WaterSettings& settings);
    void createFoamMaskResources();
    void destroyFoamMaskResources();
    void createWaterTextureResources();
    void destroyWaterTextureResources();
    void createUnderwaterResources(uint32_t width, uint32_t height);
    void destroyUnderwaterResources();
    // frameIndex indexa os recursos POR FRAME (refracao, copia de depth);
    // uboSlot indexa o banco de UBO, que tem duas vias (agua e lava).
    void updateUBO(uint32_t frameIndex, uint32_t uboSlot, const WaterSettings& settings, float nearPlane, float farPlane,
                   const DayNightCycle& cycle, float time, const Vec3& cameraPos, float cameraPitch = 0.0f,
                   float sunOcclusion = 1.0f, float moonOcclusion = 1.0f, float stormDarken = 0.0f,
                   float rainIntensity = 0.0f, float rainSplashIntensity = 0.0f);
    void copySceneToRefraction(VkCommandBuffer cmd,
                               VkImage sceneColorImage, VkImage sceneDepthImage,
                               uint32_t width, uint32_t height,
                               uint32_t frameIndex);
    void uploadFoamMask(VkCommandBuffer cmd);

    bool createTextureFromPixelsInternal(const uint8_t* pixels, int w, int h, int channels,
                                          VkFormat format, VkImage& image, VmaAllocation& alloc,
                                          VkImageView& view, VkSampler& sampler, uint32_t& slot);
};

} // namespace eruption
