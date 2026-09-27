#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/MipmapGenerator.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "math/Frustum.hpp"
#include "math/CullingSIMD.hpp"
#include "utils/AlignedAllocator.hpp"
#include "formats/TerrainParser.hpp"
#include "formats/PackManager.hpp"

#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>

namespace eruption {

struct TerrainChunkGPU {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    // PBR material scales resolved at load time to avoid CPU bottleneck.
    float metallicScale = 0.0f;
    float roughnessScale = 1.0f;

    // AABB for culling
    Vec3 aabbMin;
    Vec3 aabbMax;

    void shutdown(VulkanContext* ctx);
};

// SoA layout of terrain chunk bounds used by the SIMD culling kernel.
struct alignas(32) TerrainChunkSoA {
    std::vector<float, AlignedAllocator<float, 32>> minX;
    std::vector<float, AlignedAllocator<float, 32>> minY;
    std::vector<float, AlignedAllocator<float, 32>> minZ;
    std::vector<float, AlignedAllocator<float, 32>> maxX;
    std::vector<float, AlignedAllocator<float, 32>> maxY;
    std::vector<float, AlignedAllocator<float, 32>> maxZ;

    void resize(uint32_t count) {
        minX.resize(count); minY.resize(count); minZ.resize(count);
        maxX.resize(count); maxY.resize(count); maxZ.resize(count);
    }
    void clear() {
        minX.clear(); minY.clear(); minZ.clear();
        maxX.clear(); maxY.clear(); maxZ.clear();
    }
};

class TerrainRenderer {
    friend class Engine;
public:
    // Triangulos e chunks que o terreno submeteu no ultimo frame (pos-culling).
    uint32_t lastDrawnTriangles() const { return m_lastDrawnTriangles; }
    uint32_t lastDrawnChunks() const { return m_lastDrawnChunks; }

    bool init(VulkanContext* ctx, BindlessDescriptor* bindless);
    void shutdown();
    void clear();

    // Build chunks from parsed terrain data and load terrain textures
    void buildChunks(const TerrainFile& terrain, PackManager* assets = nullptr, uint32_t chunkSize = 32);
    
    // Render visible chunks to G-Buffer
    void render(VkCommandBuffer cmd, const Mat4& viewProj, const Frustum& frustum);

    // Share the SpriteRenderer's FrameUBO descriptor (set 1) so the fragment
    // shader can read camera position, time and POM parameters. Must be called
    // after the SpriteRenderer has been initialized.
    void setFrameUboSet(VkDescriptorSetLayout layout, VkDescriptorSet set);

    // Render visible chunks to shadow map
    void renderShadow(VkCommandBuffer cmd, VkPipeline shadowPipeline, VkPipelineLayout shadowLayout,
                      const Mat4& cascadeMatrix, const Frustum& shadowFrustum, bool enableCulling);

    const std::vector<TerrainChunkGPU>& chunks() const { return m_chunks; }
    // Indices dos chunks que passaram o culling AVX2 do ULTIMO passe de
    // terreno (1 frame atras para quem le' no update). Serve p/ estatistica
    // sem re-varrer o AoS inteiro com teste escalar.
    const std::vector<uint32_t>& visibleChunkIndices() const { return m_visibleChunkIndices; }
    
    VkPipeline pipeline() const { return m_pipeline; }
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }
    bool isInitialized() const { return m_initialized; }
    void setMipmapsEnabled(bool enabled, MipGenerationMode mode = MipGenerationMode::COMPUTE_SHARED_LDS) {
        m_mipmapsEnabled = enabled;
        m_mipmapMode = mode;
    }

private:
    // Hot-reload pipeline (for shader development)
    void recreatePipeline();

private:
    struct TerrainTexture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t bindlessSlot = 0;
        PbrTextureSlot pbrSlot;
        PbrTextureSlot normalSlot;
        bool isExternal = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
    };

    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    std::vector<TerrainChunkGPU> m_chunks;
    TerrainChunkSoA m_chunkSoA;
    std::vector<uint32_t> m_visibleChunkIndices;
    uint32_t m_paddedChunkCount = 0;

    std::vector<TerrainTexture> m_textures;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;

    bool m_mipmapsEnabled = true;
    MipGenerationMode m_mipmapMode = MipGenerationMode::BLIT_LINEAR;
    bool m_initialized = false;

    // Maps terrain texture index -> bindless slot
    std::unordered_map<uint16_t, uint32_t> m_textureSlots;
    std::unordered_map<uint16_t, PbrTextureSlots> m_pbrTextureSlots;

    void createPipeline();
    void uploadChunk(const TerrainMesh& mesh, TerrainChunkGPU& chunk);
    void loadTerrainTextures(const TerrainFile& terrain, PackManager* assets);
    uint32_t loadTerrainTexture(PackManager* assets, const std::string& path);
    void rebuildChunkSoA();
    // AABB de chunk sao estaticos pos-build: o SoA so' e' reconstruido quando
    // isto e' marcado (ver cullChunks). Comeca sujo para o primeiro frame.
    bool m_chunkSoADirty = true;
    std::vector<uint8_t> m_visibleMaskScratch;
    void cullChunks(const Frustum& frustum);

    uint32_t m_lastDrawnTriangles = 0;
    uint32_t m_lastDrawnChunks = 0;
};

} // namespace eruption
