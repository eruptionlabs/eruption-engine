#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/EtexLoader.hpp"
#include "formats/MapLoader.hpp"
#include "formats/TerrainParser.hpp"
#include "formats/ModelFile.hpp"
#include "formats/PackManager.hpp"

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace eruption {

// Forward declarations
class TerrainRenderer;
class ModelRenderer;

// TerrainChunkGPU, ModelMeshGPU and ModelInstance are defined in the renderer headers above

struct TerrainTextureGPU {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t bindlessSlot = 0;
    bool isExternal = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint32_t pbrSlot = 0;    // MRAH-W map bindless index
    uint32_t normalSlot = 0; // normal map bindless index

    // GPU ownership for the PBR maps (must be destroyed with the terrain texture).
    VkImage pbrImage = VK_NULL_HANDLE;
    VmaAllocation pbrAlloc = VK_NULL_HANDLE;
    VkImageView pbrView = VK_NULL_HANDLE;
    VkImage normalImage = VK_NULL_HANDLE;
    VmaAllocation normalAlloc = VK_NULL_HANDLE;
    VkImageView normalView = VK_NULL_HANDLE;
};

struct ModelTextureGPU {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t bindlessSlot = 0;
    bool isExternal = false; // If true, MapContext doesn't own the lifecycle
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint32_t pbrSlot = 0;    // MRAH-W map bindless index
    uint32_t normalSlot = 0; // normal map bindless index

    // GPU ownership for the PBR maps (must be destroyed with the model texture).
    VkImage pbrImage = VK_NULL_HANDLE;
    VmaAllocation pbrAlloc = VK_NULL_HANDLE;
    VkImageView pbrView = VK_NULL_HANDLE;
    VkImage normalImage = VK_NULL_HANDLE;
    VmaAllocation normalAlloc = VK_NULL_HANDLE;
    VkImageView normalView = VK_NULL_HANDLE;
};

// ------------------------------------------------------------------
// MapContext encapsulates everything that defines a loaded map.
// It is populated in three stages:
//   1) Background thread: CPU data (parsing, mesh gen, image decode)
//   2) Main thread:       incremental GPU upload via frame CB
//   3) Swap:              atomically becomes the active map
// ------------------------------------------------------------------
struct MapContext {
    // ------------------------------------------------------------------
    // Stage 1: CPU data (filled by BackgroundMapLoader)
    // ------------------------------------------------------------------
    std::string mapName;
    std::shared_ptr<LoadedMap> loadedMap;

    struct CpuTerrainTex {
        std::string path;
        std::vector<uint8_t> pixels;
        EtexData etex; // baked BC1/BC3 (preferred over pixels when valid)
        int width = 0;
        int height = 0;
        int channels = 0;
        uint16_t terrainIndex = 0;
        PbrTextureData pbrMrahw;
        PbrTextureData pbrNormal;
    };
    std::vector<CpuTerrainTex> cpuTerrainTextures;
    std::vector<TerrainMesh>   cpuTerrainMeshes;

    struct CpuModelTex {
        std::string path;
        std::string pbrName; // original material name used to look up cooked PBR maps
        std::vector<uint8_t> pixels;
        EtexData etex; // baked BC1/BC3 (preferred over pixels when valid)
        int width = 0;
        int height = 0;
        int channels = 0;
        PbrTextureData pbrMrahw;
        PbrTextureData pbrNormal;
    };
    std::vector<CpuModelTex> cpuModelTextures;

    struct CpuModelMesh {
        std::vector<TerrainVertex> vertices;
        std::vector<uint32_t> indices;
        std::string texturePath;
        Vec3 aabbMin;
        Vec3 aabbMax;
        std::string legacyModelPath; // Track ownership
        uint32_t nodeIndex = 0;
    };
    std::vector<CpuModelMesh> cpuModelMeshes;
    std::vector<ModelInstance> cpuModelInstances;
    std::vector<ModelRenderer::AnimatedNodeData> cpuAnimatedNodes;
    std::vector<ModelAsset> modelAssets; // Generic assets for the renderer (converted from legacy model)

    float centerX = 0.0f;
    float centerZ = 0.0f;

    // ------------------------------------------------------------------
    // Stage 2: GPU data (filled incrementally by Engine on main thread)
    // ------------------------------------------------------------------
    std::vector<TerrainChunkGPU> gpuTerrainChunks;
    std::vector<TerrainTextureGPU> gpuTerrainTextures;
    std::unordered_map<uint16_t, uint32_t> gpuTerrainSlotMap;
    std::unordered_map<uint16_t, uint32_t> gpuTerrainPbrSlotMap;
    std::unordered_map<uint16_t, uint32_t> gpuTerrainNormalSlotMap;

    std::vector<ModelMeshGPU> gpuModelMeshes;
    std::vector<ModelInstance> gpuModelInstances;
    std::vector<ModelTextureGPU> gpuModelTextures;
    std::unordered_map<std::string, uint32_t> gpuModelTextureCache;
    std::unordered_map<std::string, std::vector<uint32_t>> gpuModelMeshCache;

    // Upload cursors
    size_t uploadTerrainTexIdx   = 0;
    size_t uploadTerrainChunkIdx = 0;
    size_t uploadModelTexIdx     = 0;
    size_t uploadModelMeshIdx    = 0;
    size_t uploadModelInstanceIdx = 0;
    bool modelMeshesHandled = false;

    // Staging buffers retained until GPU upload is complete (destroyed in clearGpu)
    std::vector<std::pair<VkBuffer, VmaAllocation>> stagingBuffers;

    // Thread-safe status
    std::atomic<bool> cpuReady{false};
    std::atomic<bool> gpuReady{false};
    std::atomic<float> uploadProgress{0.0f};

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------
    bool isCpuReady() const { return cpuReady.load(); }
    bool isGpuReady() const { return gpuReady.load(); }

    float totalProgress() const {
        float p = 0.0f;
        if (!cpuReady.load()) p = uploadProgress.load() * 0.5f;
        else if (!gpuReady.load()) p = 0.5f + uploadProgress.load() * 0.5f;
        else p = 1.0f;
        return glm::clamp(p, 0.0f, 1.0f);
    }

    void clearCpu();
    void clearGpu(VulkanContext* ctx, BindlessDescriptor* bindless);
};

} // namespace eruption
