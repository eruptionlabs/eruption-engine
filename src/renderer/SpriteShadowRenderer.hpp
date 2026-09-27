#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/GBuffer.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "math/Types.hpp"
#include "math/Frustum.hpp"
#include <vector>
#include <cstdint>

namespace eruption {

// Forward declarations
struct Sprite;
struct SpriteInstanceData;
struct FrameUBO;
struct TerrainFile;

// ============================================================================
// Shadow Technique Enum
// ============================================================================
enum class ShadowTechnique : uint32_t {
    None  = 0,
    Planar = 1,
    Blob   = 2,
    Proxy  = 3  // Future: simple geometry in shadow map
};

// ============================================================================
// Per-Sprite Shadow Data (SSBO layout, std430 aligned)
// ============================================================================
struct SpriteShadowData {
    alignas(16) Vec4 worldPosAndScale;      // xyz = worldPos, w = shadowScale
    alignas(16) Vec4 groundNormalAndAlpha;  // xyz = groundNormal, w = blobAlpha
    alignas(16) Vec4 spriteRightAndSizeX;   // xyz = spriteRight, w = spriteSize.x
    alignas(16) Vec4 spriteUpAndSizeY;      // xyz = spriteUp, w = spriteSize.y
    alignas(4)  uint32_t technique;
    alignas(4)  float groundHeight;
    alignas(4)  float fadeStart;
    alignas(4)  float fadeEnd;
};
static_assert(sizeof(SpriteShadowData) == 80, "SpriteShadowData must be 80 bytes");

// ============================================================================
// Sprite Shadow Configuration
// ============================================================================
struct SpriteShadowConfig {
    float shadowIntensity = 0.7f;
    float maxPlanarDistance = 60.0f;
    float maxBlobDistance = 250.0f;
    float blobScale = 50.0f;
    bool alignToTerrain = true;
    float depthBias = 1.0f;
    bool enabled = true;
    bool forceBlob = false;
    bool showProxies = false;
    float globalIntensity = 1.0f;
    bool debugShowBlob = true; // Renders blobs as bright red for visibility testing
};

// ============================================================================
// Presets for different object types
// ============================================================================
namespace ShadowPresets {
    inline SpriteShadowConfig Character() {
        SpriteShadowConfig cfg;
        cfg.shadowIntensity = 0.6f;
        cfg.maxPlanarDistance = 25.0f;
        cfg.maxBlobDistance = 60.0f;
        cfg.blobScale = 0.8f;
        cfg.alignToTerrain = true;
        cfg.depthBias = 2.0f;
        return cfg;
    }
    inline SpriteShadowConfig Vegetation() {
        SpriteShadowConfig cfg;
        cfg.shadowIntensity = 0.3f;
        cfg.maxPlanarDistance = 40.0f;
        cfg.maxBlobDistance = 80.0f;
        cfg.blobScale = 1.2f;
        cfg.alignToTerrain = true;
        cfg.depthBias = 1.0f;
        return cfg;
    }
    inline SpriteShadowConfig Particle() {
        SpriteShadowConfig cfg;
        cfg.shadowIntensity = 0.2f;
        cfg.maxPlanarDistance = 0.0f;
        cfg.maxBlobDistance = 20.0f;
        cfg.blobScale = 0.5f;
        cfg.alignToTerrain = false;
        cfg.depthBias = 0.5f;
        return cfg;
    }
}

// ============================================================================
// Sprite Shadow Renderer
// ============================================================================
class SpriteShadowRenderer {
public:
    SpriteShadowRenderer();
    ~SpriteShadowRenderer();

    bool init(VulkanContext* ctx, GBuffer* gbuffer, BindlessDescriptor* bindless);
    void shutdown();

    // Update shadow data for all visible sprites (CPU-side)
    void update(const std::vector<Sprite>& visibleSprites,
                const Vec3& cameraPos,
                const TerrainFile* terrain,
                uint32_t frameCounter);

    // Render planar shadows (after terrain, before billboards)
    void renderPlanar(VkCommandBuffer cmd, const FrameUBO& frameUbo,
                      VkBuffer spriteInstanceBuffer, uint32_t spriteCount);

    // Render blob shadows (after terrain, before billboards)
    void renderBlob(VkCommandBuffer cmd, const FrameUBO& frameUbo);

    // Accessors
    uint32_t activeCount() const { return m_activeCount; }
    uint32_t planarCount() const { return m_planarCount; }
    uint32_t blobCount() const { return m_blobCount; }
    uint32_t proxyCount() const { return m_proxyCount; }
    float gpuTimeMs() const { return m_gpuTimeMs; }

    SpriteShadowConfig& config() { return m_config; }
    const SpriteShadowConfig& config() const { return m_config; }

    // Debug UI
    void debugUI();

    // Hot reload shaders (development only)
    void reloadShaders();

    // Generate simple proxy geometry (AABB) for shadow map rendering
    static void generateProxyGeometry(const Sprite& sprite,
                                       std::vector<Vec3>& outVertices,
                                       std::vector<uint32_t>& outIndices);

    // Render proxy geometries into the provided shadow pass
    void renderProxyShadows(VkCommandBuffer cmd, VkPipelineLayout shadowLayout,
                            const Mat4& lightSpaceMatrix);

    // Proxy instance buffer access (used by ShadowRenderer)
    VkBuffer proxyInstanceBuffer() const { return m_proxyInstanceBuffer; }

private:
    VulkanContext* m_ctx = nullptr;
    GBuffer* m_gbuffer = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    // Pipelines
    VkPipeline m_blobPipeline = VK_NULL_HANDLE;
    VkPipeline m_planarPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    // SSBO for shadow data
    VkBuffer m_shadowSSBO = VK_NULL_HANDLE;
    VmaAllocation m_shadowSSBOAlloc = VK_NULL_HANDLE;
    void* m_mappedShadowSSBO = nullptr;

    // Proxy geometry buffers
    VkBuffer m_proxyVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_proxyVertexAlloc = VK_NULL_HANDLE;
    VkBuffer m_proxyIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_proxyIndexAlloc = VK_NULL_HANDLE;
    VkBuffer m_proxyInstanceBuffer = VK_NULL_HANDLE;
    VmaAllocation m_proxyInstanceAlloc = VK_NULL_HANDLE;
    void* m_mappedProxyInstances = nullptr;

    // Quad buffer for blob (generated procedurally, no vertex data needed)
    // We use gl_VertexIndex in shader

    // UBO layout for FrameUBO (shared with SpriteRenderer)
    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    // CPU-side shadow data
    std::vector<SpriteShadowData> m_shadowData;
    std::vector<uint32_t> m_planarIndices;  // Indices of sprites using planar
    std::vector<uint32_t> m_blobIndices;    // Indices of sprites using blob

    // Stats
    uint32_t m_activeCount = 0;
    uint32_t m_planarCount = 0;
    uint32_t m_blobCount = 0;
    uint32_t m_proxyCount = 0;
    float m_gpuTimeMs = 0.0f;

    SpriteShadowConfig m_config;
    uint32_t m_maxSprites = 10000;

    bool createBlobPipeline();
    bool createPlanarPipeline();
    bool createPipelineLayout();
    bool createShadowSSBO();
    bool createProxyBuffers();
    bool createDescriptors();

    // LOD selection
    ShadowTechnique selectTechnique(float distance) const;
};

} // namespace eruption
