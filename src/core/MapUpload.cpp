// Upload do mapa para a GPU e troca de mapa: as rotinas de staging (textura,
// .etex, malha) que gravam no command buffer DO FRAME, o consumo da fila de
// upload por orcamento de milissegundos, e o swap de mapa em si (incluindo
// o nudge de lava e a sincronia do menu de liquidos). Saiu do Engine.cpp em
// 2026-09-04, na quebra do arquivo.
//
// Por que o upload passa pelo CB do frame e nao por immediateSubmit: o
// immediateSubmit serializa CPU e GPU e era o que fazia o warp travar; aqui o
// trabalho entra no frame que ja' vai ser submetido, com teto de tempo por
// frame pra nao estourar o orcamento.

#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TelemetryExporter.hpp"
#include "utils/EtexLoader.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/MeshSubdivide.hpp"
#include "formats/MapLoader.hpp"
#include "formats/ModelDataConverter.hpp"
#include "renderer/WeatherTypes.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

static constexpr VkDeviceSize STAGING_BUFFER_SIZE = 64ull * 1024 * 1024;

static void allocateStagingBuffer(VulkanContext* ctx, VkDeviceSize size,
                                  VkBuffer& outBuffer, VmaAllocation& outAlloc,
                                  std::vector<std::pair<VkBuffer, VmaAllocation>>& stagingBuffers) {
    ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, outBuffer, outAlloc);
    stagingBuffers.emplace_back(outBuffer, outAlloc);
}

static void uploadTextureViaFrameCB(VkCommandBuffer cmd, VulkanContext* ctx, BindlessDescriptor* bindless,
                                    VkSampler sampler, const std::vector<uint8_t>& pixels, int w, int h,
                                    bool generateMips, MipGenerationMode mode, uint32_t alphaMode,
                                    VkImage& outImage, VmaAllocation& outAlloc,
                                    VkImageView& outView, uint32_t& outSlot,
                                    uint32_t& outWidth, uint32_t& outHeight, uint32_t& outMipLevels,
                                    std::vector<std::pair<VkBuffer, VmaAllocation>>& stagingBuffers,
                                    ProfilerCategory category = ProfilerCategory::Textures) {
    if (pixels.empty() || w <= 0 || h <= 0) {
        outSlot = 0; return;
    }

    uint32_t mipLevels = generateMips ? MipmapGenerator::calculateMipLevels(w, h) : 1;
    outWidth = w;
    outHeight = h;
    outMipLevels = mipLevels;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1) {
        imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx->allocator(), &imageInfo, &allocInfo, &outImage, &outAlloc, nullptr) != VK_SUCCESS) {
        outSlot = 0; return;
    }
    PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, ctx->allocationSize(outAlloc));

    VmaAllocationInfo vmaInfo;
    vmaGetAllocationInfo(ctx->allocator(), outAlloc, &vmaInfo);
    PROFILE_VRAM_ALLOC(category, vmaInfo.size);

    ctx->cmdImageBarrier(cmd, outImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        0, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkDeviceSize rowSize = static_cast<VkDeviceSize>(w) * 4;
    const int rowsPerChunk = static_cast<int>(std::max<VkDeviceSize>(1, STAGING_BUFFER_SIZE / rowSize));
    int remainingRows = h;
    int currentRow = 0;

    while (remainingRows > 0) {
        int rowsThisChunk = std::min(remainingRows, rowsPerChunk);
        VkDeviceSize chunkSize = static_cast<VkDeviceSize>(rowsThisChunk) * rowSize;

        VkBuffer staging;
        VmaAllocation stagingAlloc;
        allocateStagingBuffer(ctx, chunkSize, staging, stagingAlloc, stagingBuffers);

        void* mapped;
        vmaMapMemory(ctx->allocator(), stagingAlloc, &mapped);
        std::memcpy(mapped, pixels.data() + static_cast<size_t>(currentRow) * rowSize, chunkSize);
        vmaUnmapMemory(ctx->allocator(), stagingAlloc);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = static_cast<uint32_t>(w);
        region.bufferImageHeight = static_cast<uint32_t>(h);
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, currentRow, 0};
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(rowsThisChunk), 1};
        vkCmdCopyBufferToImage(cmd, staging, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        currentRow += rowsThisChunk;
        remainingRows -= rowsThisChunk;
    }

    if (mipLevels > 1) {
        if (mode == MipGenerationMode::BLIT_LINEAR) {
            MipmapGenerator::generateMipmapsBlit(cmd, outImage, w, h, mipLevels);
        } else {
            MipmapGenerator::generateMipmapsCompute(cmd, ctx, outImage, w, h, mipLevels, true, alphaMode);
        }
    } else {
        ctx->cmdImageBarrier(cmd, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(ctx->device(), &viewInfo, nullptr, &outView);

    outSlot = bindless->allocateSlotSafe();
    if (outSlot != 0) {
        bindless->updateTextureSafe(outSlot, outView, sampler);
    }
}

// Upload of a baked BC1/BC3 texture (.etex): pre-generated mips are copied
// straight from one staging buffer - no decode, no blit chain, ~4-8x less
// upload bandwidth and VRAM than RGBA8.
static void uploadEtexViaFrameCB(VkCommandBuffer cmd, VulkanContext* ctx, BindlessDescriptor* bindless,
                                 VkSampler sampler, const EtexData& etex,
                                 VkImage& outImage, VmaAllocation& outAlloc,
                                 VkImageView& outView, uint32_t& outSlot,
                                 uint32_t& outWidth, uint32_t& outHeight, uint32_t& outMipLevels,
                                 std::vector<std::pair<VkBuffer, VmaAllocation>>& stagingBuffers,
                                 ProfilerCategory category = ProfilerCategory::Textures) {
    if (!etex.valid()) { outSlot = 0; return; }
    outWidth = etex.width;
    outHeight = etex.height;
    outMipLevels = etex.mipCount;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = static_cast<VkFormat>(etex.vkFormat);
    imageInfo.extent = {etex.width, etex.height, 1};
    imageInfo.mipLevels = etex.mipCount;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(ctx->allocator(), &imageInfo, &allocInfo, &outImage, &outAlloc, nullptr) != VK_SUCCESS) {
        outSlot = 0; return;
    }
    PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, ctx->allocationSize(outAlloc));
    VmaAllocationInfo vmaInfo;
    vmaGetAllocationInfo(ctx->allocator(), outAlloc, &vmaInfo);
    PROFILE_VRAM_ALLOC(category, vmaInfo.size);

    ctx->cmdImageBarrier(cmd, outImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        0, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBuffer staging;
    VmaAllocation stagingAlloc;
    allocateStagingBuffer(ctx, etex.payload.size(), staging, stagingAlloc, stagingBuffers);
    void* mapped;
    vmaMapMemory(ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, etex.payload.data(), etex.payload.size());
    vmaUnmapMemory(ctx->allocator(), stagingAlloc);

    std::vector<VkBufferImageCopy> regions(etex.mipCount);
    VkDeviceSize offset = 0;
    for (uint32_t i = 0; i < etex.mipCount; ++i) {
        VkBufferImageCopy& r = regions[i];
        r = {};
        r.bufferOffset = offset;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel = i;
        r.imageSubresource.layerCount = 1;
        r.imageExtent = {std::max(1u, etex.width >> i), std::max(1u, etex.height >> i), 1};
        offset += etex.mipSizes[i];
    }
    vkCmdCopyBufferToImage(cmd, staging, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           etex.mipCount, regions.data());

    ctx->cmdImageBarrier(cmd, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = static_cast<VkFormat>(etex.vkFormat);
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = etex.mipCount;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(ctx->device(), &viewInfo, nullptr, &outView);

    outSlot = bindless->allocateSlotSafe();
    if (outSlot != 0) {
        bindless->updateTextureSafe(outSlot, outView, sampler);
    }
}

static void copyBufferChunked(VkCommandBuffer cmd, VulkanContext* ctx,
                              const void* srcData, VkDeviceSize srcSize,
                              VkBuffer dstBuffer,
                              std::vector<std::pair<VkBuffer, VmaAllocation>>& stagingBuffers) {
    VkDeviceSize offset = 0;
    while (offset < srcSize) {
        VkDeviceSize chunkSize = std::min(srcSize - offset, STAGING_BUFFER_SIZE);
        VkBuffer staging;
        VmaAllocation stagingAlloc;
        allocateStagingBuffer(ctx, chunkSize, staging, stagingAlloc, stagingBuffers);

        void* mapped;
        vmaMapMemory(ctx->allocator(), stagingAlloc, &mapped);
        std::memcpy(mapped, static_cast<const uint8_t*>(srcData) + offset, chunkSize);
        vmaUnmapMemory(ctx->allocator(), stagingAlloc);

        VkBufferCopy copy{};
        copy.srcOffset = 0;
        copy.dstOffset = offset;
        copy.size = chunkSize;
        vkCmdCopyBuffer(cmd, staging, dstBuffer, 1, &copy);

        offset += chunkSize;
    }
}

static void uploadMeshViaFrameCB(VkCommandBuffer cmd, VulkanContext* ctx,
                                 const std::vector<TerrainVertex>& vertices, const std::vector<uint32_t>& indices,
                                 VkBuffer& outVBuf, VmaAllocation& outVAlloc,
                                 VkBuffer& outIBuf, VmaAllocation& outIAlloc,
                                 std::vector<std::pair<VkBuffer, VmaAllocation>>& stagingBuffers,
                                 ProfilerCategory category = ProfilerCategory::Meshes) {
    VkDeviceSize vSize = vertices.size() * sizeof(TerrainVertex);
    VkDeviceSize iSize = indices.size() * sizeof(uint32_t);

    ctx->createBuffer(vSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY, outVBuf, outVAlloc);
    ctx->createBuffer(iSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_GPU_ONLY, outIBuf, outIAlloc);
    PROFILE_VRAM_ALLOC(ProfilerCategory::Meshes, vSize + iSize);

    VmaAllocationInfo vInfo, iInfo;
    vmaGetAllocationInfo(ctx->allocator(), outVAlloc, &vInfo);
    vmaGetAllocationInfo(ctx->allocator(), outIAlloc, &iInfo);
    PROFILE_VRAM_ALLOC(category, vInfo.size + iInfo.size);

    if (vSize > 0) {
        copyBufferChunked(cmd, ctx, vertices.data(), vSize, outVBuf, stagingBuffers);
    }
    if (iSize > 0) {
        copyBufferChunked(cmd, ctx, indices.data(), iSize, outIBuf, stagingBuffers);
    }
}

// ------------------------------------------------------------------
// Background loading pipeline
// ------------------------------------------------------------------

// Override de teste vence o preset. Os ERUPTION_TEST_* sao lidos no construtor
// (Engine.cpp:~264), mas applyGraphicsPreset() roda DEPOIS e sobrescreve o
// mesmo campo a partir do JSON - entao a variavel de debug nao fazia nada e o
// experimento media silenciosamente a coisa errada. Descoberto medindo contact
// shadow: ERUPTION_TEST_CONTACT_SHADOW=1 dava screenshot BIT A BIT identica ao
// =0, porque o preset high poe "contact_shadows": false logo em seguida.

void Engine::processBackgroundLoading() {
    if (!m_backgroundLoader) return;

    // Check if background CPU loading finished
    if (!m_stagingMapContext) {
        auto result = m_backgroundLoader->pollResult();
        if (result) {
            if (m_warpRestartPending) {
                // This result belongs to the cancelled load; discard it.
                ERUPTION_LOG_INFO("Engine: discarding cancelled load result for '%s'",
                                result->mapName.c_str());
                result.reset();
            } else {
                m_stagingMapContext = std::move(result);
                ERUPTION_LOG_INFO("Engine: staging map CPU data ready (%s), beginning GPU upload", m_stagingMapContext->mapName.c_str());
            }
        }
    }

    // If GPU upload is complete and swap pending, perform swap at frame boundary
    if (m_mapSwapPending && m_stagingMapContext) {
        if (m_stagingMapContext->isGpuReady()) {
            ERUPTION_LOG_INFO("Engine: performing map swap to %s", m_stagingMapContext->mapName.c_str());
            const std::string swapMapName = m_stagingMapContext->mapName;
            auto swapT0 = std::chrono::steady_clock::now();
            performMapSwap();
            double swapMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - swapT0).count();
            TelemetryExporter::recordMapSwap(swapMapName, swapMs);
            TelemetryExporter::recordLoadEnd(swapMapName);
            m_mapSwapPending = false;
        }
    }
}

void Engine::uploadStagingMapGPU(VkCommandBuffer cmd) {
    if (!m_stagingMapContext || !m_stagingMapContext->isCpuReady() || m_stagingMapContext->isGpuReady())
        return;

    auto* ctx = m_stagingMapContext.get();
    // This path only runs during background warps, where the world is still
    // being rendered underneath (renderLoadingProgress is just an overlay
    // bar) - so the budget must fit INSIDE a playable frame, not replace it.
    // The budget is checked between items; a single item can still overrun,
    // which is why the warp path clamps embedded textures to 1024^2.
    const float UPLOAD_TIME_BUDGET_MS = 6.0f;
    auto startTime = std::chrono::high_resolution_clock::now();
    auto elapsedMs = [&]() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<float, std::milli>(now - startTime).count();
    };
    auto hasBudget = [&]() { return elapsedMs() < UPLOAD_TIME_BUDGET_MS; };

    bool workDone = false;

    // ---- 1. Upload terrain textures ----
    if (ctx->uploadTerrainTexIdx < ctx->cpuTerrainTextures.size()) {
        size_t startIdx = ctx->uploadTerrainTexIdx;
        ERUPTION_LOG_INFO("Engine: Uploading terrain textures %zu/%zu (adaptive budget %.1f ms)", startIdx, ctx->cpuTerrainTextures.size(), UPLOAD_TIME_BUDGET_MS);
        for (size_t i = startIdx; i < ctx->cpuTerrainTextures.size() && hasBudget(); ++i) {
            const auto& cpuTex = ctx->cpuTerrainTextures[i];
            
            TerrainTextureGPU gpuTex{};
            bool foundInCache = m_assetCache.getTerrainTexture(cpuTex.path, gpuTex);
            
            if (foundInCache) {
                gpuTex.isExternal = true;
                ERUPTION_LOG_INFO("Engine: reuse cached terrain texture: %s (slot %u)", cpuTex.path.c_str(), gpuTex.bindlessSlot);
            } else if (cpuTex.etex.valid()) {
                uploadEtexViaFrameCB(cmd, &m_vulkan, &m_bindless, m_defaultSampler,
                                     cpuTex.etex,
                                     gpuTex.image, gpuTex.alloc, gpuTex.view, gpuTex.bindlessSlot,
                                     gpuTex.width, gpuTex.height, gpuTex.mipLevels,
                                     ctx->stagingBuffers, ProfilerCategory::Textures);
                gpuTex.isExternal = true;

                TerrainTextureGPU cacheEntry = gpuTex;
                cacheEntry.pbrSlot = 0;
                cacheEntry.normalSlot = 0;
                cacheEntry.pbrImage = VK_NULL_HANDLE; cacheEntry.pbrAlloc = VK_NULL_HANDLE; cacheEntry.pbrView = VK_NULL_HANDLE;
                cacheEntry.normalImage = VK_NULL_HANDLE; cacheEntry.normalAlloc = VK_NULL_HANDLE; cacheEntry.normalView = VK_NULL_HANDLE;
                if (gpuTex.bindlessSlot != 0) {
                    m_assetCache.addTerrainTexture(cpuTex.path, cacheEntry);
                }
            } else if (!cpuTex.pixels.empty()) {
                uploadTextureViaFrameCB(cmd, &m_vulkan, &m_bindless, m_defaultSampler,
                                        cpuTex.pixels, cpuTex.width, cpuTex.height, true,
                                        m_mipmapMenu.config.mode, 0,
                                        gpuTex.image, gpuTex.alloc, gpuTex.view, gpuTex.bindlessSlot,
                                        gpuTex.width, gpuTex.height, gpuTex.mipLevels,
                                        ctx->stagingBuffers, ProfilerCategory::Textures);
                gpuTex.isExternal = true; // MUST be true because the GlobalAssetCache owns it now

                // Cache only owns the albedo; PBR maps are owned by the terrain texture entry.
                TerrainTextureGPU cacheEntry = gpuTex;
                cacheEntry.pbrSlot = 0;
                cacheEntry.normalSlot = 0;
                cacheEntry.pbrImage = VK_NULL_HANDLE; cacheEntry.pbrAlloc = VK_NULL_HANDLE; cacheEntry.pbrView = VK_NULL_HANDLE;
                cacheEntry.normalImage = VK_NULL_HANDLE; cacheEntry.normalAlloc = VK_NULL_HANDLE; cacheEntry.normalView = VK_NULL_HANDLE;
                if (gpuTex.bindlessSlot != 0) {
                    m_assetCache.addTerrainTexture(cpuTex.path, cacheEntry);
                }
            }

            if (gpuTex.bindlessSlot != 0) {
                ctx->gpuTerrainSlotMap[cpuTex.terrainIndex] = gpuTex.bindlessSlot;
            }

            // Load pre-cooked PBR maps for this terrain texture so the deferred
            // pass can use normal/roughness/metallic data instead of constants.
            // Prefer data decoded on the background thread to avoid main-thread I/O stalls.
            // The global cache only owns the albedo; PBR maps are per-MapContext, so they
            // must be loaded even when the albedo was reused from cache (e.g. after warp).
            {
                PbrTextureSlots pbrSlots;
                bool usedPredecoded = cpuTex.pbrMrahw.valid() && cpuTex.pbrNormal.valid();
                if (usedPredecoded) {
                    pbrSlots = uploadPbrTexturesFromData(&m_vulkan, &m_bindless, m_defaultSampler,
                                                         cpuTex.pbrMrahw, cpuTex.pbrNormal);
                } else {
                    pbrSlots = loadPbrTexturesForAlbedo(&m_vulkan, &m_bindless, m_defaultSampler, cpuTex.path);
                }
                if (pbrSlots.valid()) {
                    gpuTex.pbrSlot = pbrSlots.mrahw.index;
                    gpuTex.normalSlot = pbrSlots.normal.index;
                    gpuTex.pbrImage = pbrSlots.mrahw.image;
                    gpuTex.pbrAlloc = pbrSlots.mrahw.alloc;
                    gpuTex.pbrView = pbrSlots.mrahw.view;
                    gpuTex.normalImage = pbrSlots.normal.image;
                    gpuTex.normalAlloc = pbrSlots.normal.alloc;
                    gpuTex.normalView = pbrSlots.normal.view;
                    // Ownership has been transferred to the terrain texture entry.
                    pbrSlots.mrahw.image = VK_NULL_HANDLE;
                    pbrSlots.mrahw.alloc = VK_NULL_HANDLE;
                    pbrSlots.mrahw.view = VK_NULL_HANDLE;
                    pbrSlots.normal.image = VK_NULL_HANDLE;
                    pbrSlots.normal.alloc = VK_NULL_HANDLE;
                    pbrSlots.normal.view = VK_NULL_HANDLE;
                }
            }
            if (gpuTex.pbrSlot != 0) {
                ctx->gpuTerrainPbrSlotMap[cpuTex.terrainIndex] = gpuTex.pbrSlot;
            }
            if (gpuTex.normalSlot != 0) {
                ctx->gpuTerrainNormalSlotMap[cpuTex.terrainIndex] = gpuTex.normalSlot;
            }

            ctx->gpuTerrainTextures.push_back(std::move(gpuTex));
            ctx->uploadTerrainTexIdx = i + 1;
            workDone = true;
        }
        // Don't proceed to chunks until all textures are uploaded to ensure correct remapping
    } 
    // ---- 2. Upload terrain chunks ----
    else if (ctx->uploadTerrainChunkIdx < ctx->cpuTerrainMeshes.size()) {
        size_t startIdx = ctx->uploadTerrainChunkIdx;
        ERUPTION_LOG_INFO("Engine: Uploading terrain chunks %zu/%zu (adaptive budget %.1f ms)", startIdx, ctx->cpuTerrainMeshes.size(), UPLOAD_TIME_BUDGET_MS);
        for (size_t i = startIdx; i < ctx->cpuTerrainMeshes.size() && hasBudget(); ++i) {
            TerrainMesh mesh = ctx->cpuTerrainMeshes[i]; // copy so we can remap indices
            for (auto& v : mesh.vertices) {
                uint16_t terrainTexIdx = static_cast<uint16_t>(v.texIndex);
                auto it = ctx->gpuTerrainSlotMap.find(terrainTexIdx);
                if (it != ctx->gpuTerrainSlotMap.end()) {
                    v.texIndex = it->second;
                } else {
                    v.texIndex = 0;
                }
                auto pit = ctx->gpuTerrainPbrSlotMap.find(terrainTexIdx);
                v.pbrIndex = (pit != ctx->gpuTerrainPbrSlotMap.end()) ? pit->second : 0u;
                auto nit = ctx->gpuTerrainNormalSlotMap.find(terrainTexIdx);
                v.normalIndex = (nit != ctx->gpuTerrainNormalSlotMap.end()) ? nit->second : 0u;
            }
            TerrainChunkGPU chunk{};
            uploadMeshViaFrameCB(cmd, &m_vulkan, mesh.vertices, mesh.indices,
                                 chunk.vertexBuffer, chunk.vertexAlloc,
                                 chunk.indexBuffer, chunk.indexAlloc,
                                 ctx->stagingBuffers);
            chunk.indexCount = static_cast<uint32_t>(mesh.indices.size());
            chunk.aabbMin = Vec3(FLT_MAX);
            chunk.aabbMax = Vec3(-FLT_MAX);
            for (const auto& v : mesh.vertices) {
                chunk.aabbMin = glm::min(chunk.aabbMin, v.position);
                chunk.aabbMax = glm::max(chunk.aabbMax, v.position);
            }
            ctx->gpuTerrainChunks.push_back(std::move(chunk));
            ctx->uploadTerrainChunkIdx = i + 1;
            workDone = true;
        }
    }
    // ---- 3. Upload model textures ----
    else if (ctx->uploadModelTexIdx < ctx->cpuModelTextures.size()) {
        size_t startIdx = ctx->uploadModelTexIdx;
        ERUPTION_LOG_INFO("Engine: Uploading model textures %zu/%zu (adaptive budget %.1f ms)", startIdx, ctx->cpuModelTextures.size(), UPLOAD_TIME_BUDGET_MS);
        for (size_t i = startIdx; i < ctx->cpuModelTextures.size() && hasBudget(); ++i) {
            const auto& cpuTex = ctx->cpuModelTextures[i];
            
            ModelTextureGPU gpuTex{};
            bool foundInCache = m_assetCache.getModelTexture(cpuTex.path, gpuTex);
            
            if (foundInCache) {
                gpuTex.isExternal = true;
                ERUPTION_LOG_INFO("Engine: reuse cached model texture: %s (slot %u)", cpuTex.path.c_str(), gpuTex.bindlessSlot);
            } else if (cpuTex.etex.valid()) {
                uploadEtexViaFrameCB(cmd, &m_vulkan, &m_bindless, m_defaultSampler,
                                     cpuTex.etex,
                                     gpuTex.image, gpuTex.alloc, gpuTex.view, gpuTex.bindlessSlot,
                                     gpuTex.width, gpuTex.height, gpuTex.mipLevels,
                                     ctx->stagingBuffers);
                gpuTex.isExternal = true;
                if (gpuTex.bindlessSlot != 0) {
                    m_assetCache.addModelTexture(cpuTex.path, gpuTex);
                }
            } else if (!cpuTex.pixels.empty()) {
                uploadTextureViaFrameCB(cmd, &m_vulkan, &m_bindless, m_defaultSampler,
                                        cpuTex.pixels, cpuTex.width, cpuTex.height, true,
                                        m_mipmapMenu.config.mode, 1,
                                        gpuTex.image, gpuTex.alloc, gpuTex.view, gpuTex.bindlessSlot,
                                        gpuTex.width, gpuTex.height, gpuTex.mipLevels,
                                        ctx->stagingBuffers);
                gpuTex.isExternal = true; // MUST be true because the GlobalAssetCache owns it now

                // Add to global cache
                if (gpuTex.bindlessSlot != 0) {
                    m_assetCache.addModelTexture(cpuTex.path, gpuTex);
                }
            }

            // Load pre-cooked PBR maps for this model texture so the deferred
            // pass can use normal/roughness/metallic data instead of constants.
            // Prefer data decoded on the background thread to avoid main-thread I/O stalls.
            {
                PbrTextureSlots pbrSlots;
                bool usedPredecoded = cpuTex.pbrMrahw.valid() && cpuTex.pbrNormal.valid();
                if (usedPredecoded) {
                    pbrSlots = uploadPbrTexturesFromData(&m_vulkan, &m_bindless, m_defaultSampler,
                                                         cpuTex.pbrMrahw, cpuTex.pbrNormal);
                } else {
                    const std::string& pbrLookup = !cpuTex.pbrName.empty() ? cpuTex.pbrName : cpuTex.path;
                    pbrSlots = loadPbrTexturesForAlbedo(&m_vulkan, &m_bindless, m_defaultSampler, pbrLookup);
                }
                if (pbrSlots.valid()) {
                    gpuTex.pbrSlot = pbrSlots.mrahw.index;
                    gpuTex.normalSlot = pbrSlots.normal.index;
                    gpuTex.pbrImage = pbrSlots.mrahw.image;
                    gpuTex.pbrAlloc = pbrSlots.mrahw.alloc;
                    gpuTex.pbrView = pbrSlots.mrahw.view;
                    gpuTex.normalImage = pbrSlots.normal.image;
                    gpuTex.normalAlloc = pbrSlots.normal.alloc;
                    gpuTex.normalView = pbrSlots.normal.view;
                    // Ownership has been transferred to the model texture entry.
                    pbrSlots.mrahw.image = VK_NULL_HANDLE;
                    pbrSlots.mrahw.alloc = VK_NULL_HANDLE;
                    pbrSlots.mrahw.view = VK_NULL_HANDLE;
                    pbrSlots.normal.image = VK_NULL_HANDLE;
                    pbrSlots.normal.alloc = VK_NULL_HANDLE;
                    pbrSlots.normal.view = VK_NULL_HANDLE;
                }
            }

            ctx->gpuModelTextureCache[cpuTex.path] = gpuTex.bindlessSlot;
            ctx->gpuModelTextures.push_back(std::move(gpuTex));
            ctx->uploadModelTexIdx = i + 1;
            workDone = true;
        }
        // Don't proceed to meshes until all model textures are uploaded
    }
    // ---- 4. Upload model meshes ----
    else if (!ctx->modelMeshesHandled) {
        ERUPTION_LOG_INFO("Engine: Phase 4 model meshes (CPU: %zu/%zu, GPU: %zu, Cache entries: %zu)",
            ctx->uploadModelMeshIdx, ctx->cpuModelMeshes.size(), ctx->gpuModelMeshes.size(), ctx->gpuModelMeshCache.size());

        // 4b. Normal uploads with adaptive time budget
        if (ctx->uploadModelMeshIdx < ctx->cpuModelMeshes.size()) {
            size_t startIdx = ctx->uploadModelMeshIdx;
            ERUPTION_LOG_INFO("Engine: Phase 4b uploading meshes %zu/%zu (adaptive budget %.1f ms)", startIdx, ctx->cpuModelMeshes.size(), UPLOAD_TIME_BUDGET_MS);
            for (size_t i = startIdx; i < ctx->cpuModelMeshes.size() && hasBudget(); ++i) {
                // IMPORTANT: Work on a local copy to avoid corrupting shared CPU meshes
                MapContext::CpuModelMesh cpuMeshCopy = ctx->cpuModelMeshes[i];
                
                // Resolve texture slots per vertex
                uint32_t firstSlot = 0;
                for (auto& v : cpuMeshCopy.vertices) {
                    uint32_t cpuIdx = v.texIndex;
                    if (cpuIdx != UINT32_MAX && cpuIdx < ctx->gpuModelTextures.size()) {
                        const auto& gpuTex = ctx->gpuModelTextures[cpuIdx];
                        v.texIndex = gpuTex.bindlessSlot;
                        v.pbrIndex = gpuTex.pbrSlot;
                        v.normalIndex = gpuTex.normalSlot;
                        if (firstSlot == 0) firstSlot = v.texIndex;
                    } else {
                        v.texIndex = 0;
                        v.pbrIndex = 0;
                        v.normalIndex = 0;
                    }

                    // GLB-authored crossfade: prepareModelData stored the CPU
                    // texture index in the blend slots; remap to GPU slots.
                    uint32_t cpuBlendIdx = v.blendTexIndex;
                    if (v.blendWeight > 0.0f && cpuBlendIdx != UINT32_MAX &&
                        cpuBlendIdx < ctx->gpuModelTextures.size()) {
                        const auto& gpuBlend = ctx->gpuModelTextures[cpuBlendIdx];
                        v.blendTexIndex = gpuBlend.bindlessSlot;
                        v.blendPbrIndex = gpuBlend.pbrSlot;
                        v.blendNormalIndex = gpuBlend.normalSlot;
                        if (gpuBlend.bindlessSlot == 0) v.blendWeight = 0.0f;
                    } else {
                        v.blendTexIndex = 0;
                        v.blendPbrIndex = 0;
                        v.blendNormalIndex = 0;
                        v.blendWeight = 0.0f;
                    }
                }

                std::string meshKey = cpuMeshCopy.legacyModelPath + "#" + std::to_string(cpuMeshCopy.nodeIndex);
                ModelMeshGPU gpuMesh{};
                bool foundInMeshCache = m_assetCache.getModelMesh(meshKey, gpuMesh);
                
                if (foundInMeshCache) {
                    ERUPTION_LOG_INFO("Engine: reuse cached mesh: %s (node %u)", cpuMeshCopy.legacyModelPath.c_str(), cpuMeshCopy.nodeIndex);
                    gpuMesh.isExternal = true;
                } else {
                    uploadMeshViaFrameCB(cmd, &m_vulkan, cpuMeshCopy.vertices, cpuMeshCopy.indices,
                                         gpuMesh.vertexBuffer, gpuMesh.vertexAlloc,
                                         gpuMesh.indexBuffer, gpuMesh.indexAlloc,
                                         ctx->stagingBuffers);
                    gpuMesh.indexCount = static_cast<uint32_t>(cpuMeshCopy.indices.size());
                    gpuMesh.bindlessTexSlot = firstSlot;
                    gpuMesh.aabbMin = cpuMeshCopy.aabbMin;
                    gpuMesh.aabbMax = cpuMeshCopy.aabbMax;
                    gpuMesh.isExternal = true; // owned by global cache

                    // LOD geométrico (mipmap para malha): a versão subdividida
                    // + passa-baixa é gerada AQUI, no load, e fica residente.
                    // Trocar de nível em runtime é só escolher outro buffer,
                    // então zoom in/out não regenera nada (era o hitch que o
                    // usuário previu). Só malhas com quad grosso qualificam -
                    // as densas não ganhariam nada e só custariam memória.
                    if (m_geoLodBudgetBytes > 0) {
                        const float medEdge = medianEdgeLength(cpuMeshCopy.vertices,
                                                               cpuMeshCopy.indices);
                        if (medEdge > m_geoLodTargetEdge * 1.5f) {
                            SubdivideParams sp;
                            sp.targetEdge = m_geoLodTargetEdge;
                            sp.lowPassStrength = m_geoLodLowPass;
                            SubdivideResult sub = subdivideMesh(cpuMeshCopy.vertices,
                                                                cpuMeshCopy.indices, sp);
                            const size_t bytes = sub.valid
                                ? sub.vertices.size() * sizeof(TerrainVertex) +
                                  sub.indices.size() * sizeof(uint32_t)
                                : 0;
                            if (sub.valid && bytes <= m_geoLodBudgetBytes) {
                                uploadMeshViaFrameCB(cmd, &m_vulkan, sub.vertices, sub.indices,
                                                     gpuMesh.hiVertexBuffer, gpuMesh.hiVertexAlloc,
                                                     gpuMesh.hiIndexBuffer, gpuMesh.hiIndexAlloc,
                                                     ctx->stagingBuffers);
                                gpuMesh.hiIndexCount = static_cast<uint32_t>(sub.indices.size());
                                gpuMesh.hiSwitchDistance = m_geoLodDistance;
                                m_geoLodBudgetBytes -= bytes;
                                m_geoLodBytesUsed += bytes;
                            }
                        }
                    }
                    m_assetCache.addModelMesh(meshKey, gpuMesh);
                }
                
                ctx->gpuModelMeshes.push_back(std::move(gpuMesh));
                uint32_t gpuIdx = static_cast<uint32_t>(ctx->gpuModelMeshes.size() - 1);
                
                // Update mapping in gpuModelMeshCache: source model path -> nodeIndex -> GPU Index
                auto it_cache = ctx->gpuModelMeshCache.find(cpuMeshCopy.legacyModelPath);
                if (it_cache != ctx->gpuModelMeshCache.end()) {
                    if (cpuMeshCopy.nodeIndex < it_cache->second.size()) {
                        it_cache->second[cpuMeshCopy.nodeIndex] = gpuIdx;
                    }
                }
                
                ctx->uploadModelMeshIdx = i + 1;
                workDone = true;
            }
        }

        // Check if finished
        if (ctx->uploadModelMeshIdx >= ctx->cpuModelMeshes.size()) {
            ERUPTION_LOG_INFO("Engine: Model meshes phase 4 complete");
            ctx->modelMeshesHandled = true;
            workDone = true;
        }
    }
    // ---- 5. Copy model instances (CPU only) ----
    if (ctx->modelMeshesHandled && ctx->uploadModelInstanceIdx < ctx->cpuModelInstances.size()) {
        ERUPTION_LOG_INFO("Engine: Phase 5 starting instance copy (%zu items)...", ctx->cpuModelInstances.size());
        
        ctx->gpuModelInstances = ctx->cpuModelInstances;
        
        // Remap ALL mesh indices using gpuModelMeshCache
        size_t remappedCount = 0;
        for (auto& inst : ctx->gpuModelInstances) {
            auto it = ctx->gpuModelMeshCache.find(inst.assetPath);
            if (it != ctx->gpuModelMeshCache.end()) {
                const auto& indices = it->second;
                if (!indices.empty() && inst.nodeIndex < indices.size()) {
                    uint32_t idx = indices[inst.nodeIndex];
                    if (idx != UINT32_MAX && idx < ctx->gpuModelMeshes.size()) {
                        inst.meshIndex = idx;
                        remappedCount++;
                    } else {
                        inst.meshIndex = 0;
                    }
                }
            }
        }
        
        ctx->uploadModelInstanceIdx = ctx->cpuModelInstances.size();
        ERUPTION_LOG_INFO("Engine: Phase 5 complete (remapped %zu/%zu instances)", remappedCount, ctx->gpuModelInstances.size());
        workDone = true;
    }

    // ---- 6. Drain queued PBR map uploads through the frame CB ----
    // Batches recorded into THIS frame's command buffer: no immediateSubmit,
    // no fence wait, so a warp never freezes the render thread. Staging
    // buffers join ctx->stagingBuffers (freed by the deferred-delete path).
    if (pendingPbrUploadCount() > 0) {
        recordPendingPbrUploads(&m_vulkan, cmd, 64, ctx->stagingBuffers);
        workDone = true;
    }

    // Update progress
    size_t totalItems = ctx->cpuTerrainTextures.size() + ctx->cpuTerrainMeshes.size()
                      + ctx->cpuModelTextures.size() + ctx->cpuModelMeshes.size() + 1;
    size_t doneItems = ctx->uploadTerrainTexIdx + ctx->uploadTerrainChunkIdx
                     + ctx->uploadModelTexIdx + ctx->uploadModelMeshIdx + ctx->uploadModelInstanceIdx;
    float progress = totalItems > 0 ? static_cast<float>(doneItems) / static_cast<float>(totalItems) : 1.0f;
    ctx->uploadProgress.store(progress);

    // Check completion
    if (ctx->uploadTerrainTexIdx >= ctx->cpuTerrainTextures.size() &&
        ctx->uploadTerrainChunkIdx >= ctx->cpuTerrainMeshes.size() &&
        ctx->uploadModelTexIdx >= ctx->cpuModelTextures.size() &&
        ctx->modelMeshesHandled &&
        ctx->uploadModelInstanceIdx >= ctx->cpuModelInstances.size() &&
        pendingPbrUploadCount() == 0) {
        ctx->gpuReady.store(true);
        ERUPTION_LOG_INFO("Engine: uploadStagingMapGPU fully complete (100%%) in %.1f ms", elapsedMs());
    } else {
        ERUPTION_LOG_INFO("Engine: upload not complete: TT:%zu/%zu TC:%zu/%zu MT:%zu/%zu MM:%s MI:%zu/%zu",
            ctx->uploadTerrainTexIdx, ctx->cpuTerrainTextures.size(),
            ctx->uploadTerrainChunkIdx, ctx->cpuTerrainMeshes.size(),
            ctx->uploadModelTexIdx, ctx->cpuModelTextures.size(),
            ctx->modelMeshesHandled ? "Y" : "N",
            ctx->uploadModelInstanceIdx, ctx->cpuModelInstances.size());
    }

    // Flush bindless updates so they're visible this frame
    if (workDone) {
        m_bindless.flushUpdatesSafe();
    }
}

void Engine::syncLiquidMenuFromMap(const LoadedMap* map) {
    m_waterMenu.liquids.clear();
    if (map) {
        for (const auto& lq : map->liquids) {
            WaterDebugMenu::LiquidEntry e;
            e.name = lq.name.empty()
                   ? (lq.kind == 1 ? "Lava" : "Water")
                   : lq.name;
            e.kind = lq.kind;
            e.level = lq.level;
            e.centerX = lq.centerX;
            e.centerZ = lq.centerZ;
            // offsetX/Z/Y e rotationDeg ficam em 0: sao nudge de sessao, nao
            // fazem sentido persistir entre mapas diferentes.
            m_waterMenu.liquids.push_back(e);
        }
    }
    m_waterMenu.ensureDefaultLiquid();
    m_waterMenu.applySelectedLiquid();
}

void Engine::setupLavaLiquid(const LoadedMap* map) {
    m_water.clearLavaMesh();
    m_lavaLiquid = LoadedMap::MapLiquid{};
    m_hasLavaLiquid = false;
    if (!map) return;

        for (size_t idx = 0; idx < map->liquids.size(); ++idx) {
            const auto& lq = map->liquids[idx];
            if (lq.kind != 1 || lq.radius <= 0.0f) continue;

            // Nudge autorado no WaterDebugMenu (posicao/rotacao), por cima
            // do que o .env autorou - nunca escreve no arquivo. `idx` casa
            // 1:1 com m_waterMenu.liquids porque os dois vem da MESMA
            // ordem de map->liquids (syncLiquidMenuFromMap, chamado so' em
            // map load, nao em toda rebuild - senao um nudge em andamento
            // seria zerado no frame seguinte).
            float offX = 0.0f, offZ = 0.0f, offY = 0.0f, rotDeg = 0.0f;
            if (idx < m_waterMenu.liquids.size()) {
                const auto& ui = m_waterMenu.liquids[idx];
                offX = ui.offsetX; offZ = ui.offsetZ; offY = ui.offsetY; rotDeg = ui.rotationDeg;
            }
            const float centerX = lq.centerX + offX;
            const float centerZ = lq.centerZ + offZ;
            const float level = lq.level + offY;
            const float rotRad = rotDeg * 0.017453292f;

            WaterMesh lmesh;
            const int rings = 24;
            const int segs = 64;
            // Margem LOBADA: se o mapa autorou shoreRadius, o raio MAXIMO
            // pra cada segmento angular vem de la' (interpolado, wraparound
            // incluido) em vez do escalar `radius` constante. Um circulo nao
            // acompanha uma caldeira lobada - sobra vazio nos lobos largos ou
            // vaza pra fora nos estreitos, nao tem raio unico que resolva os
            // dois ao mesmo tempo. shoreRadius vazio (todo mapa anterior a
            // essa feature) cai exatamente no caminho de antes.
            const size_t shoreN = lq.shoreRadius.size();
            auto radiusAt = [&](int a) -> float {
                if (shoreN == 0) return lq.radius;
                const float f = (float(a) / float(segs)) * float(shoreN);
                const size_t i0 = static_cast<size_t>(f) % shoreN;
                const size_t i1 = (i0 + 1) % shoreN;
                const float t = f - std::floor(f);
                return lq.shoreRadius[i0] * (1.0f - t) + lq.shoreRadius[i1] * t;
            };
            WaterVertex wv{};
            wv.position = Vec3(centerX, level, centerZ);
            wv.texCoord = Vec2(0.5f, 0.5f);
            lmesh.vertices.push_back(wv);
            for (int r = 1; r <= rings; ++r) {
                for (int a = 0; a < segs; ++a) {
                    // rotRad gira o PERFIL (a rotula qual amostra de
                    // shoreRadius fica em qual angulo do mundo), nao a
                    // amostragem em si - assim o lobo gira em volta do
                    // centro em vez de so' deslocar a fase da textura.
                    const float th = 6.2831853f * float(a) / float(segs) + rotRad;
                    const float rr = radiusAt(a) * (float(r) / float(rings));
                    const float px = centerX + rr * std::cos(th);
                    const float pz = centerZ + rr * std::sin(th);
                    WaterVertex rv{};
                    rv.position = Vec3(px, level, pz);
                    rv.texCoord = Vec2(px / 90.0f, pz / 90.0f);
                    lmesh.vertices.push_back(rv);
                }
            }
            auto ringIdx = [&](int r, int a) {
                return uint32_t(1 + (r - 1) * segs + (a % segs));
            };
            for (int a = 0; a < segs; ++a) {
                lmesh.indices.push_back(0);
                lmesh.indices.push_back(ringIdx(1, a));
                lmesh.indices.push_back(ringIdx(1, a + 1));
            }
            for (int r = 1; r < rings; ++r) {
                for (int a = 0; a < segs; ++a) {
                    lmesh.indices.push_back(ringIdx(r, a));
                    lmesh.indices.push_back(ringIdx(r + 1, a));
                    lmesh.indices.push_back(ringIdx(r + 1, a + 1));
                    lmesh.indices.push_back(ringIdx(r, a));
                    lmesh.indices.push_back(ringIdx(r + 1, a + 1));
                    lmesh.indices.push_back(ringIdx(r, a + 1));
                }
            }
            lmesh.waterTileCount = uint32_t(rings * segs);
            m_water.setLavaMesh(lmesh);
            m_lavaLiquid = lq;
            m_lavaLiquid.centerX = centerX;
            m_lavaLiquid.centerZ = centerZ;
            m_lavaLiquid.level = level;
            m_hasLavaLiquid = true;
            ERUPTION_LOG_WARN("Lava surface: level=%.1f center=(%.0f,%.0f) r=%.0f",
                              level, centerX, centerZ, lq.radius);
            break;
        }
}

void Engine::performMapSwap() {
    if (!m_stagingMapContext) return;

    m_assetCache.incrementWarp();
    m_assetCache.cleanupOldTextures(&m_vulkan, &m_bindless, 3); // Keep textures for up to 3 warps

    ERUPTION_LOG_INFO("Engine: performing map swap to %s", m_stagingMapContext->mapName.c_str());

    // Move old active context to deferred deletion (keep GPU resources alive for 8 more frames for safety)
    if (m_activeMapContext) {
        m_deferredDeletes.push_back({std::move(m_activeMapContext), 8});
    }

    // Swap
    m_activeMapContext = std::move(m_stagingMapContext);

    // Point legacy shared_ptr for systems that still use it
    m_currentMap = m_activeMapContext->loadedMap;
    m_currentMapName = m_activeMapContext->mapName;
    m_centerX = m_activeMapContext->centerX;
    m_centerZ = m_activeMapContext->centerZ;

    // Independent clouds belong to the previous map (same rule as the
    // synchronous loadMap): they sit at the OLD map's world coordinates after
    // a warp. Drop them and force the weather field to respawn around the
    // player on the new map.
    clearIndependentClouds();
    m_weatherFieldType = -1;

    // Fit cloud coverage bounds to the actual map size so clouds drift
    // across the playable area instead of clustering in a corner.
    if (m_currentMap) {
        Vec3 worldMin(0.0f, 0.0f, 0.0f);
        Vec3 worldMax(m_centerX * 2.0f, 2000.0f, m_centerZ * 2.0f);
        m_cloudLayerRenderer.coverageArray().setWorldBounds(worldMin, worldMax);
    }

    // Configure water mesh from map data
    m_water.clearWaterMesh();
    if (m_currentMap) {
        float wl, wh;
        bool fromTerrain = getMapWaterParams(*m_currentMap, wl, wh, m_waterMenu.config.forceWater);
        auto wmesh = TerrainParser::generateWaterMesh(m_currentMap->terrain, 0, 0,
                                                  m_currentMap->terrain.width, m_currentMap->terrain.height,
                                                  wl, wh, m_waterMenu.config.forceWater, !fromTerrain);
        m_mapBaseWaterLevel = wl;
        m_water.setWaterMesh(wmesh);
        cacheWaterBounds(wmesh);

        syncLiquidMenuFromMap(m_currentMap.get());
        setupLavaLiquid(m_currentMap.get());
        ERUPTION_LOG_INFO("Water mesh: %u tiles (force=%d, baseLevel=%.2f, offset=%.2f, src=%s)",
                         wmesh.waterTileCount, m_waterMenu.config.forceWater, m_mapBaseWaterLevel, m_waterMenu.config.waterLevel, fromTerrain ? "terrain data" : "world data");
    }

    ERUPTION_LOG_INFO("Engine: swap begin -> %s, terrTex=%zu terrChunks=%zu modelTex=%zu modelMeshes=%zu modelInst=%zu",
        m_currentMapName.c_str(),
        m_activeMapContext->gpuTerrainTextures.size(),
        m_activeMapContext->gpuTerrainChunks.size(),
        m_activeMapContext->gpuModelTextures.size(),
        m_activeMapContext->gpuModelMeshes.size(),
        m_activeMapContext->gpuModelInstances.size());

    // Configure renderers to use the new active context
    // TerrainRenderer
    m_terrainRenderer.clear();
    for (auto& tex : m_activeMapContext->gpuTerrainTextures) {
        TerrainRenderer::TerrainTexture tt{};
        tt.image = tex.image;
        tt.alloc = tex.alloc;
        tt.view = tex.view;
        tt.bindlessSlot = tex.bindlessSlot;
        tt.pbrSlot = PbrTextureSlot{tex.pbrSlot, tex.pbrImage, tex.pbrAlloc, tex.pbrView};
        tt.normalSlot = PbrTextureSlot{tex.normalSlot, tex.normalImage, tex.normalAlloc, tex.normalView};
        tt.isExternal = tex.isExternal;
        m_terrainRenderer.m_textures.push_back(tt);
        tex.image = VK_NULL_HANDLE; tex.alloc = VK_NULL_HANDLE; tex.view = VK_NULL_HANDLE; tex.bindlessSlot = 0;
        tex.pbrSlot = 0; tex.normalSlot = 0;
        tex.pbrImage = VK_NULL_HANDLE; tex.pbrAlloc = VK_NULL_HANDLE; tex.pbrView = VK_NULL_HANDLE;
        tex.normalImage = VK_NULL_HANDLE; tex.normalAlloc = VK_NULL_HANDLE; tex.normalView = VK_NULL_HANDLE;
    }
    m_terrainRenderer.m_textureSlots = std::move(m_activeMapContext->gpuTerrainSlotMap);
    m_terrainRenderer.m_chunks = std::move(m_activeMapContext->gpuTerrainChunks);

    // ModelRenderer
    m_modelRenderer.clear();
    m_modelRenderer.m_lastModels = std::move(m_activeMapContext->modelAssets);
    m_modelRenderer.m_textureCache = std::move(m_activeMapContext->gpuModelTextureCache);
    m_modelRenderer.m_modelMeshCache = std::move(m_activeMapContext->gpuModelMeshCache);
    m_modelRenderer.m_meshes = std::move(m_activeMapContext->gpuModelMeshes);
    m_modelRenderer.m_instances = std::move(m_activeMapContext->gpuModelInstances);
    m_modelRenderer.m_animatedNodes = std::move(m_activeMapContext->cpuAnimatedNodes);

    ERUPTION_LOG_INFO("Engine: swap complete -> %s", m_currentMapName.c_str());

    // Test hook: schedule a screenshot after the first swap following a warp
    // request. We cannot take it here because performMapSwap runs before the
    // frame is rendered, so the swap image would still show the previous map.
    if (m_screenshotAfterWarpPending && !m_screenshotAfterWarpPath.empty()) {
        m_screenshotOnLoad = m_screenshotAfterWarpPath;
        m_screenshotAfterWarpPath.clear();
        m_screenshotAfterWarpPending = false;
    }

    // Flush any queued PBR GPU uploads now that all terrain/model textures for
    // the new map have been resolved. This turns N synchronous submits into one.
    flushPbrTextureUploads(&m_vulkan);
    
    if (!s_pendingMeshes.empty()) {
        m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
            for (auto& pm : s_pendingMeshes) {
                VkBufferCopy cv{0, 0, pm.vSize};
                vkCmdCopyBuffer(cmd, pm.staging, pm.vb, 1, &cv);
                VkBufferCopy ci{pm.vSize, 0, pm.iSize};
                vkCmdCopyBuffer(cmd, pm.staging, pm.ib, 1, &ci);
            }
        });
        for (auto& pm : s_pendingMeshes) {
            vmaDestroyBuffer(m_vulkan.allocator(), pm.staging, pm.alloc);
        }
        s_pendingMeshes.clear();
    }


    // Lighting / camera
    if (m_currentMap) {
        // OFFSET DO CENTRO SO' EM MAPA LEGADO. As luzes de um mapa legado vem em
        // coordenada RELATIVA ao centro do mapa; as de um GLB (o .env ao lado)
        // vem em coordenada de MUNDO ja' pronta. Somar o centro nas duas fazia
        // as luzes do GLB irem parar FORA do mapa: no parana_field, as 12 luzes
        // de lava nasciam em (2807,2739) e eram jogadas para (4807,4739), num
        // mapa de 4000x4000 - nenhuma delas iluminava nada, e o que restava era
        // a luz presa ao jogador (autor 2026-09-06: "a luz dinamica ta muito
        // fraca"). Medido: intensidade de 0 a 27 nao mudava UM pixel.
        const Vec3 lightOrigin = m_currentMap->isGlbMap ? Vec3(0.0f)
                                                 : Vec3(m_centerX, 0.0f, m_centerZ);
        std::vector<PointLight> lights;
        lights.reserve(m_currentMap->lights.size());
        for (const auto& ml : m_currentMap->lights) {
            PointLight pl;
            pl.position = ml.position + lightOrigin;
            pl.color = ml.color;
            pl.intensity = ml.intensity;
            pl.radius = ml.range;
            pl.animType = ml.animType;
            pl.nightOnly = ml.nightOnly;
            pl.enabled = !ml.nightOnly;
            if (std::getenv("ERUPTION_DEBUG_LAVA_LIGHT"))
                ERUPTION_LOG_WARN("[PLIGHT] fonte (%.0f,%.0f,%.0f) -> mundo (%.0f,%.0f,%.0f) centro (%.0f,%.0f) r=%.0f i=%.2f",
                                  ml.position.x, ml.position.y, ml.position.z,
                                  pl.position.x, pl.position.y, pl.position.z,
                                  m_centerX, m_centerZ, pl.radius, pl.intensity);
            lights.push_back(pl);
        }
        appendArtificialTestLights(m_currentMapName, m_centerX, m_centerZ, lights);
        m_deferredLighting.setPointLights(lights);
    } else {
        m_deferredLighting.setPointLights({});
    }
    
    // Position player on the now-active map. Terrain/NavGrid are ready here.
    Vec3 targetPos;
    if (m_hasInitialPos) {
        targetPos = m_initialPos;
        if (m_currentMap) {
            targetPos.y = TerrainParser::getTerrainHeightAt(m_currentMap->terrain, targetPos.x, targetPos.z);
        }
        ERUPTION_LOG_INFO("[Engine] Applying initial position (%.2f, %.2f, %.2f) on active map -> terrain height %.2f",
                          m_initialPos.x, m_initialPos.y, m_initialPos.z, targetPos.y);
        m_hasInitialPos = false; // apply only once, on first swap after boot
    } else {
        // Default: map center snapped to a walkable NavGrid cell.
        targetPos = Vec3(m_centerX, 0.0f, m_centerZ);
        if (m_currentMap) {
            float navCellSize = 5.0f;
            int gx = (int)(targetPos.x / navCellSize);
            int gz = (int)(targetPos.z / navCellSize);
            
            if (gx < 0 || gz < 0 || gx >= (int)m_currentMap->navGrid.width || gz >= (int)m_currentMap->navGrid.height || 
                !m_currentMap->navGrid.at(gx, gz).isWalkable()) {
                
                bool found = false;
                for (int radius = 1; radius < 50 && !found; ++radius) {
                    for (int i = -radius; i <= radius; ++i) {
                        for (int j = -radius; j <= radius; ++j) {
                            int nx = gx + i;
                            int nz = gz + j;
                            if (nx >= 0 && nz >= 0 && nx < (int)m_currentMap->navGrid.width && nz < (int)m_currentMap->navGrid.height) {
                                if (m_currentMap->navGrid.at(nx, nz).isWalkable()) {
                                    targetPos.x = (float)nx * navCellSize + 2.5f;
                                    targetPos.z = (float)nz * navCellSize + 2.5f;
                                    found = true;
                                    break;
                                }
                            }
                        }
                        if (found) break;
                    }
                }
            }
            targetPos.y = clampSpawnYToWater(*m_currentMap,
                                              TerrainParser::getTerrainHeightAt(m_currentMap->terrain, targetPos.x, targetPos.z));
        }
    }
    
    m_camera.setOrbitTarget(targetPos);
    m_playerController->setPos(m_camera.target());
    m_playerSpawnedOnActiveMap = true;
    m_framesAfterSwap = 0;

    // Default spawn orientation: look toward the south.
    // Camera sits north of the player and looks toward +Z (south).
    if (m_hasInitialCamera) {
        m_camera.setOrbit(m_initialYaw, m_initialPitch, m_initialDist);
        m_camera.setDefaultOrbit(m_initialYaw, m_initialPitch, m_initialDist);
        m_hasInitialCamera = false;
    } else {
        m_camera.setOrbit(glm::radians(-90.0f), m_camera.orbitPitch(), m_camera.orbitDistance());
    }
    m_playerController->setYaw(glm::pi<float>());
    if (m_currentMap) {
        LightingEnvironment env;
        env.sun.direction = m_currentMap->env.sunDirection;
        if (glm::length(env.sun.direction) < 0.001f) env.sun.direction = glm::normalize(Vec3(-0.5f, -1.0f, -0.3f));
        env.sun.color = Vec3(1.0f, 1.0f, 1.0f);
        env.sun.intensity = 1.0f;
        env.ambientIntensity = 0.0f;
        m_deferredLighting.setEnvironment(env);
    }

    // Free CPU data as it's no longer needed
    m_activeMapContext->clearCpu();

    ERUPTION_LOG_INFO("Engine: map swap complete -> %s", m_currentMapName.c_str());
    ERUPTION_LOG_INFO("  - Terrain: %zu textures, %zu chunks", m_terrainRenderer.m_textures.size(), m_terrainRenderer.m_chunks.size());
    ERUPTION_LOG_INFO("  - Models: %zu cached textures, %zu meshes, %zu instances", m_modelRenderer.m_textureCache.size(), m_modelRenderer.m_meshes.size(), m_modelRenderer.m_instances.size());

    ERUPTION_LOG_WARN("Player spawned at (%.2f, %.2f, %.2f)",
                    m_playerController->pos().x,
                    m_playerController->pos().y,
                    m_playerController->pos().z);
}

} // namespace eruption
