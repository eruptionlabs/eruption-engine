#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/GBuffer.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "math/Types.hpp"

#include <vector>
#include <cstdint>

namespace eruption {

class ShadowRenderer;

struct SpriteInstanceData {
    Mat4 modelMatrix;
    Vec4 anchorPoint; // xyz = world anchor, w = padding/unused
    Vec4 texRect;
    uint32_t texIndex;
    uint32_t normalTexIndex;
    uint32_t mrahwTexIndex;
    uint32_t flags; // bit 0: flipX, bit 1: flipY, bit 2: billboard, bit 3-7: matId, bit 8: usePalette
    uint32_t paletteIndex;
    Vec4 tintColor;
};

struct FrameUBO {
    Mat4 view;
    Mat4 projection;
    Mat4 viewProjection;
    Mat4 inverseView;
    Mat4 inverseProjection;
    Vec3 cameraPos;
    float time;
    Vec2 screenResolution;
    float nearPlane;
    float farPlane;
    uint32_t frameIndex;
    uint32_t debugMode;
    float spriteExposure; 
    float giIntensity;     // Added to match map lighting
    float giAmbientFloor;  // Added to match map lighting
    float spriteTilt;      // isometric tilt in degrees
    float shadowHeightScale; // Compress shadow height
    float spriteNormalYMix;  // Mix for Y-up normal
    float normalMapScale;    // Strength of the normal map detail (0 = flat, 1 = full)
    float normalMapInvertY;  // 0 = OpenGL/Vulkan convention (Y up), 1 = flip green channel (Y down)
    float normalSmoothing;   // LOD bias for normal maps to smooth compression artifacts
    float defaultRoughness;  // Fallback roughness when no PBR map is bound
    float defaultMetallic;   // Fallback metallic when no PBR map is bound

    // Lighting for Forward Sprites
    alignas(16) Vec4 sunDir;          // xyz=dir, w=intensity
    alignas(16) Vec4 sunColor;        // xyz=color, w=unused
    alignas(16) Vec4 ambientSky;      // xyz=skyColor, w=ambientIntensity
    alignas(16) Vec4 ambientGround;   // xyz=groundColor, w=unused

    // Appended fields: only the shaders that use them declare this tail
    // (model.frag/terrain.frag); shorter shader-side blocks stay valid.
    // POM (banda média do displacement híbrido - docs/displacement_design.md):
    // x=heightScale (uv units), y=maxSteps, z=minSteps, w=wetness global
    // (tira o dual-use do alpha do MRAH-W, que agora é SÓ altura).
    alignas(16) Vec4 pomParams;

    // VENTO (IGNIS). xyz = vetor de vento em mundo (direcao * forca, ja' com a
    // rajada), w = tempo do vento em segundos - proprio, e nao `time`, porque
    // o vento precisa de um relogio que nao reinicie com o mapa.
    //
    // Anexado no fim, como os anteriores: os offsets de tudo acima ficam
    // intactos e um shader que declare um bloco mais curto continua valido.
    alignas(16) Vec4 windParams;

    // RELEVO POR DISTANCIA (ideia do autor, 2026-09-06). Duas curvas editadas
    // no F2 com o widget de spline da engine (SplineEditorUI, o mesmo do DoF e
    // das sombras) e enviadas como LUT de 8 AMOSTRAS uniformes em
    // [0, normalDistParams.x] unidades de mundo. LUT em vez dos pontos da
    // curva: assim o autor pode por quantos pontos quiser sem mexer no UBO nem
    // no shader, e o shader faz uma interpolacao linear de 8 valores.
    //   normalDistParams: x = distancia maxima (u), y = 1 liga / 0 desliga
    //   normalScaleLut0/1  = forca do normal map      (8 amostras)
    //   normalSmoothLut0/1 = vies de mip do normal map (8 amostras)
    alignas(16) Vec4 normalDistParams;
    alignas(16) Vec4 normalScaleLut0;
    alignas(16) Vec4 normalScaleLut1;
    alignas(16) Vec4 normalSmoothLut0;
    alignas(16) Vec4 normalSmoothLut1;

    // TESSELACAO DE HARDWARE NA BANDA PERTO (model.tesc/model.tese).
    //   x = distancia maxima da curva (u)
    //   y = 1 liga / 0 desliga
    //   z = amplitude do deslocamento em unidades de mundo
    //   w = fator maximo da curva (normaliza a atenuacao da amplitude)
    alignas(16) Vec4 tessParams;
    alignas(16) Vec4 tessLut0;
    alignas(16) Vec4 tessLut1;
    // x = fonte da altura: 0 = UV da malha (o relevo bate com a textura que se
    //     ve, mas abre costura onde a UV quebra), 1 = mundo/triplanar (junta
    //     fechada sempre, mas o padrao deixa de coincidir com a textura).
    // y = escala do mundo no modo triplanar (1/u por ladrilho).
    alignas(16) Vec4 tessParams2;
};

class SpriteRenderer {
public:
    bool init(VulkanContext* ctx, GBuffer* gbuffer, BindlessDescriptor* bindless);
    void shutdown();

    void addSprite(const SpriteInstanceData& sprite);
    void clearSprites();
    void flushUploads(VkCommandBuffer cmd);

    // Palette management
    uint32_t addPalette(const uint8_t* rgba256);
    void updatePalette(uint32_t slot, const uint8_t* rgba256);
    void removePalette(uint32_t slot);

    // Update the shared FrameUBO buffer without drawing. Call this before
    // terrain/model geometry passes so they read current camera/POM data.
    void updateFrameUbo(const FrameUBO& frameUbo);

    void renderSprites(VkCommandBuffer cmd, const FrameUBO& frameUbo);
    void renderSprites(VkCommandBuffer cmd, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count);

    void renderSpritesForward(VkCommandBuffer cmd, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count, const ShadowRenderer* shadows);

    void renderShadow(VkCommandBuffer cmd, VkPipelineLayout shadowLayout, const Mat4& lightSpaceMatrix, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count);
    
    // Projected Planar Shadow for billboards — renders a flattened sprite silhouette on the ground.
    void renderBillboardPlanarShadows(VkCommandBuffer cmd, const FrameUBO& frameUbo, const Vec3& shadowLightDir,
                                      const Vec2& planarParams, VkBuffer instanceBuffer, uint32_t count);

    // Projected Circle/Blob Shadow for billboards — renders a soft procedural circle flat on the ground.
    void renderBillboardCircleShadows(VkCommandBuffer cmd, const FrameUBO& frameUbo,
                                      const Vec2& circleParams, VkBuffer instanceBuffer, uint32_t count);

    uint32_t spriteCount() const { return m_spriteCount; }

    void setSpriteExposure(float exposure) { m_spriteExposure = exposure; }
    float spriteExposure() const { return m_spriteExposure; }

    // Expose the shared FrameUBO descriptor so other G-Buffer pipelines
    // (model, terrain) can bind the same set 1 and access camera/time/POM data.
    VkDescriptorSetLayout frameUboLayout() const { return m_frameUboLayout; }
    VkDescriptorSet frameUboSet() const { return m_frameUboSet; }

private:
    VulkanContext* m_ctx = nullptr;

    struct PendingPaletteUpload {
        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc;
        VkImage targetImage;
    };
    std::vector<PendingPaletteUpload> m_pendingPaletteUploads;
    
    // G-Buffer
    GBuffer* m_gbuffer = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_forwardPipeline = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_planarShadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_circleShadowPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    VkBuffer m_quadVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_quadVertexAlloc = VK_NULL_HANDLE;

    VkBuffer m_instanceBuffer = VK_NULL_HANDLE;
    VmaAllocation m_instanceAlloc = VK_NULL_HANDLE;
    void* m_mappedInstances = nullptr;

    VkBuffer m_frameUboBuffer = VK_NULL_HANDLE;
    VmaAllocation m_frameUboAlloc = VK_NULL_HANDLE;
    void* m_mappedFrameUbo = nullptr;

    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_shadowLayoutSet = VK_NULL_HANDLE;
    VkDescriptorSet m_shadowSet = VK_NULL_HANDLE;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkSampler m_paletteSampler = VK_NULL_HANDLE;

    struct PaletteTexture {
        VkImage image;
        VmaAllocation alloc;
        VkImageView view;
        uint32_t bindlessSlot;
    };
    std::vector<PaletteTexture> m_palettes;

    float m_spriteExposure = 1.0f;
    uint32_t m_maxInstances = 10000;
    uint32_t m_spriteCount = 0;

    bool createQuadBuffer();
    bool createBuffers();
    bool createDescriptors();
    bool createPipeline();
    bool createForwardPipeline();
    bool createShadowPipeline();
    bool createPlanarShadowPipeline();
    bool createCircleShadowPipeline();
};

} // namespace eruption
