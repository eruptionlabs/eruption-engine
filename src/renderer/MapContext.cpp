#include "renderer/MapContext.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "utils/Profiler.hpp"

namespace eruption {

void MapContext::clearCpu() {
    // Calculate RAM usage before clearing so we can free it from the profiler
    size_t terrainTexRam = 0;
    for (const auto& t : cpuTerrainTextures) {
        terrainTexRam += t.pixels.size() + t.pbrMrahw.pixels.size() + t.pbrNormal.pixels.size();
    }
    size_t terrainMeshRam = 0;
    for (const auto& m : cpuTerrainMeshes) {
        terrainMeshRam += m.vertices.size() * sizeof(TerrainVertex) + m.indices.size() * sizeof(uint32_t);
    }
    size_t modelTexRam = 0;
    for (const auto& t : cpuModelTextures) {
        modelTexRam += t.pixels.size() + t.pbrMrahw.pixels.size() + t.pbrNormal.pixels.size();
    }
    size_t modelMeshRam = 0;
    for (const auto& m : cpuModelMeshes) {
        modelMeshRam += m.vertices.size() * sizeof(TerrainVertex) + m.indices.size() * sizeof(uint32_t);
    }
    if (terrainTexRam > 0) Profiler::freeRam(ProfilerCategory::Textures, terrainTexRam);
    if (terrainMeshRam > 0) Profiler::freeRam(ProfilerCategory::Meshes, terrainMeshRam);
    if (modelTexRam > 0) Profiler::freeRam(ProfilerCategory::Textures, modelTexRam);
    if (modelMeshRam > 0) Profiler::freeRam(ProfilerCategory::Meshes, modelMeshRam);

    cpuTerrainTextures.clear();
    cpuTerrainTextures.shrink_to_fit();
    cpuTerrainMeshes.clear();
    cpuTerrainMeshes.shrink_to_fit();
    cpuModelTextures.clear();
    cpuModelTextures.shrink_to_fit();
    cpuModelMeshes.clear();
    cpuModelMeshes.shrink_to_fit();
    cpuModelInstances.clear();
    cpuModelInstances.shrink_to_fit();
    if (loadedMap) {
        size_t rawSize = loadedMap->rawDataSize;
        if (rawSize > 0) {
            Profiler::freeRam(ProfilerCategory::FileCache, rawSize);
            size_t parsedSize = rawSize * 2;
            if (parsedSize > rawSize) {
                Profiler::freeRam(ProfilerCategory::LoadingIO, parsedSize - rawSize);
            }
        }
        loadedMap.reset();
    }
}

void MapContext::clearGpu(VulkanContext* ctx, BindlessDescriptor* bindless) {
    for (auto& chunk : gpuTerrainChunks) {
        chunk.shutdown(ctx);
    }
    gpuTerrainChunks.clear();

    for (auto& tex : gpuTerrainTextures) {
        if (tex.isExternal) continue;
        if (tex.bindlessSlot != 0 && bindless) {
            bindless->freeSlotSafe(tex.bindlessSlot);
        }
        if (tex.view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device(), tex.view, nullptr);
        }
        if (tex.image != VK_NULL_HANDLE) {
            vmaDestroyImage(ctx->allocator(), tex.image, tex.alloc);
        }
        if (tex.pbrSlot != 0 && bindless) {
            bindless->freeSlotSafe(tex.pbrSlot);
        }
        if (tex.pbrView != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device(), tex.pbrView, nullptr);
        }
        if (tex.pbrImage != VK_NULL_HANDLE) {
            vmaDestroyImage(ctx->allocator(), tex.pbrImage, tex.pbrAlloc);
        }
        if (tex.normalSlot != 0 && bindless) {
            bindless->freeSlotSafe(tex.normalSlot);
        }
        if (tex.normalView != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device(), tex.normalView, nullptr);
        }
        if (tex.normalImage != VK_NULL_HANDLE) {
            vmaDestroyImage(ctx->allocator(), tex.normalImage, tex.normalAlloc);
        }
    }
    gpuTerrainTextures.clear();
    gpuTerrainSlotMap.clear();

    for (auto& mesh : gpuModelMeshes) {
        mesh.shutdown(ctx);
    }
    gpuModelMeshes.clear();
    gpuModelInstances.clear();

    for (auto& tex : gpuModelTextures) {
        // PBR maps are always owned by this MapContext even when the albedo is
        // shared through the global asset cache, so they must be freed regardless
        // of the external flag.
        if (tex.pbrSlot != 0 && bindless) {
            bindless->freeSlotSafe(tex.pbrSlot);
        }
        if (tex.pbrView != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device(), tex.pbrView, nullptr);
        }
        if (tex.pbrImage != VK_NULL_HANDLE) {
            vmaDestroyImage(ctx->allocator(), tex.pbrImage, tex.pbrAlloc);
        }
        if (tex.normalSlot != 0 && bindless) {
            bindless->freeSlotSafe(tex.normalSlot);
        }
        if (tex.normalView != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device(), tex.normalView, nullptr);
        }
        if (tex.normalImage != VK_NULL_HANDLE) {
            vmaDestroyImage(ctx->allocator(), tex.normalImage, tex.normalAlloc);
        }

        if (tex.isExternal) continue; // Albedo lifecycle is managed elsewhere
        if (tex.bindlessSlot != 0 && bindless) {
            bindless->freeSlotSafe(tex.bindlessSlot);
        }
        if (tex.view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device(), tex.view, nullptr);
        }
        if (tex.image != VK_NULL_HANDLE) {
            vmaDestroyImage(ctx->allocator(), tex.image, tex.alloc);
        }
    }
    gpuModelTextures.clear();
    gpuModelTextureCache.clear();
    gpuModelMeshCache.clear();

    for (auto& sb : stagingBuffers) {
        vmaDestroyBuffer(ctx->allocator(), sb.first, sb.second);
    }
    stagingBuffers.clear();

    uploadTerrainTexIdx   = 0;
    uploadTerrainChunkIdx = 0;
    uploadModelTexIdx     = 0;
    uploadModelMeshIdx    = 0;
    uploadModelInstanceIdx = 0;
    gpuReady.store(false);
}

} // namespace eruption
