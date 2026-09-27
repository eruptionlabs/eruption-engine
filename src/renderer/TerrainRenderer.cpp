#include "renderer/GBuffer.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/Profiler.hpp"

#include <cstring>
#include <algorithm>

namespace eruption {

void TerrainChunkGPU::shutdown(VulkanContext* ctx) {
    if (vertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx->allocator(), vertexBuffer, vertexAlloc);
        vertexBuffer = VK_NULL_HANDLE;
    }
    if (indexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx->allocator(), indexBuffer, indexAlloc);
        indexBuffer = VK_NULL_HANDLE;
    }
}

bool TerrainRenderer::init(VulkanContext* ctx, BindlessDescriptor* bindless) {
    m_ctx = ctx;
    m_bindless = bindless;

    // Create default linear sampler for terrain textures
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler);

    createPipeline();
    m_initialized = (m_pipeline != VK_NULL_HANDLE);
    return m_initialized;
}

void TerrainRenderer::shutdown() {
    clear(); // already destroys all terrain textures and their PBR slots

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }

    // m_pbrTextureSlots only mirrors the PBR slots owned by m_textures; clearing
    // m_textures in clear() already destroyed them, so just drop the map here.
    m_pbrTextureSlots.clear();

    m_initialized = false;
}

void TerrainRenderer::clear() {
    for (auto& chunk : m_chunks) {
        chunk.shutdown(m_ctx);
    }
    m_chunks.clear();
    m_chunkSoADirty = true; // lista de chunks mudou: SoA precisa ser refeito

    for (auto& tex : m_textures) {
        if (tex.isExternal) continue;
        if (tex.bindlessSlot != 0) {
            m_bindless->freeSlot(tex.bindlessSlot);
        }
        if (tex.view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_ctx->device(), tex.view, nullptr);
        }
        if (tex.image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_ctx->allocator(), tex.image, tex.alloc);
        }
        tex.pbrSlot.reset(m_ctx, m_bindless);
        tex.normalSlot.reset(m_ctx, m_bindless);
    }
    m_textures.clear();
    m_textureSlots.clear();
}

void TerrainRenderer::setFrameUboSet(VkDescriptorSetLayout layout, VkDescriptorSet set) {
    if (m_frameUboLayout == layout && m_frameUboSet == set) return;
    m_frameUboLayout = layout;
    m_frameUboSet = set;
    recreatePipeline();
}

void TerrainRenderer::recreatePipeline() {
    ERUPTION_LOG_INFO("TerrainRenderer::recreatePipeline() called");
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    createPipeline();
    ERUPTION_LOG_INFO("TerrainRenderer::recreatePipeline() done, new pipeline = %p", (void*)m_pipeline);
}

void TerrainRenderer::createPipeline() {
    ERUPTION_LOG_INFO("TerrainRenderer::createPipeline() - loading shaders...");
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/terrain.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/terrain.frag.spv");
    ERUPTION_LOG_INFO("TerrainRenderer::createPipeline() - vert=%zu words, frag=%zu words", vertCode.size(), fragCode.size());
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load terrain shaders");
        return;
    }

    VkShaderModule vertModule, fragModule;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = {};
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input: TerrainVertex layout
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(TerrainVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attribs(12);
    attribs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, position)};
    attribs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, texCoord)};
    attribs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, normal)};
    attribs[3] = {3, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, texIndex)};
    attribs[4] = {4, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, matId)};
    attribs[5] = {5, 0, VK_FORMAT_R32_UINT, offsetof(TerrainVertex, color)};
    attribs[6] = {6, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, pbrIndex)};
    attribs[7] = {7, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, normalIndex)};
    attribs[8] = {8, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendTexIndex)};
    attribs[9] = {9, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendPbrIndex)};
    attribs[10] = {10, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendNormalIndex)};
    attribs[11] = {11, 0, VK_FORMAT_R32_SFLOAT, offsetof(TerrainVertex, blendWeight)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertexInput.pVertexAttributeDescriptions = attribs.data();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    // viewProjection (mat4) + metallicScale + roughnessScale + padding (vec2)
    pcRange.size = sizeof(Mat4) + sizeof(Vec4);

    VkDescriptorSetLayout bindlessLayout = m_bindless->layout();
    VkDescriptorSetLayout layouts[2] = { bindlessLayout, m_frameUboLayout };

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = (m_frameUboLayout != VK_NULL_HANDLE) ? 2u : 1u;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout);

    std::vector<VkPipelineColorBlendAttachmentState> blends(5);
    for (auto& b : blends) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        b.blendEnable = VK_FALSE;
    }

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, 1280, 720)
        .setScissor(0, 0, 1280, 720)
        .setPolygonMode(wireframeMode() ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setBlendState(blends)
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats(GBuffer::colorAttachmentFormats())
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    m_pipeline = pipeline;
}

void TerrainRenderer::buildChunks(const TerrainFile& terrain, PackManager* assets, uint32_t chunkSize) {
    clear();

    if (terrain.width == 0 || terrain.height == 0) {
        ERUPTION_LOG_WARN("TerrainRenderer: empty terrain data");
        return;
    }

    // Load terrain textures
    if (assets) {
        loadTerrainTextures(terrain, assets);
    }

    uint32_t numChunksX = (terrain.width + chunkSize - 1) / chunkSize;
    uint32_t numChunksZ = (terrain.height + chunkSize - 1) / chunkSize;
    m_chunks.reserve(numChunksX * numChunksZ);

    size_t totalVerts = 0;
    size_t totalIndices = 0;
    for (uint32_t cz = 0; cz < numChunksZ; cz++) {
        for (uint32_t cx = 0; cx < numChunksX; cx++) {
            uint32_t startX = cx * chunkSize;
            uint32_t startZ = cz * chunkSize;
            uint32_t cw = std::min(chunkSize, terrain.width - startX);
            uint32_t ch = std::min(chunkSize, terrain.height - startZ);

            TerrainMesh mesh = TerrainParser::generateMesh(terrain, startX, startZ, cw, ch);
            if (mesh.vertices.empty()) continue;

            // Determine the dominant albedo texture in this chunk so we can
            // apply a single PBR material profile scale per draw call. This is
            // an approximation for chunks that mix several terrain textures,
            // but it keeps the draw call count unchanged.
            std::unordered_map<uint16_t, uint32_t> texFrequency;
            for (const auto& v : mesh.vertices) {
                texFrequency[static_cast<uint16_t>(v.texIndex)]++;
            }
            uint16_t dominantIdx = 0;
            uint32_t dominantCount = 0;
            for (const auto& [idx, count] : texFrequency) {
                if (count > dominantCount) {
                    dominantCount = count;
                    dominantIdx = idx;
                }
            }

            // Remap texture indices to bindless slots
            for (auto& v : mesh.vertices) {
                uint16_t origIdx = static_cast<uint16_t>(v.texIndex);
                auto it = m_textureSlots.find(origIdx);
                if (it != m_textureSlots.end()) {
                    v.texIndex = it->second;
                } else {
                    v.texIndex = 0; // fallback to slot 0 (undefined, will be magenta or black)
                }

                auto pit = m_pbrTextureSlots.find(origIdx);
                if (pit != m_pbrTextureSlots.end()) {
                    v.pbrIndex = pit->second.mrahw.index;
                    v.normalIndex = pit->second.normal.index;
                } else {
                    v.pbrIndex = 0;
                    v.normalIndex = 0;
                }
            }

            totalVerts += mesh.vertices.size();
            totalIndices += mesh.indices.size();

            TerrainChunkGPU chunk;
            std::string domName = (dominantIdx < terrain.textures.size()) ? terrain.textures[dominantIdx] : std::string{};
            const auto& profile = getPbrProfile(domName);
            chunk.metallicScale = profile.metallic;
            chunk.roughnessScale = profile.roughness;
            uploadChunk(mesh, chunk);
            ERUPTION_LOG_INFO("Chunk[%u,%u]: verts=%zu indices=%zu", cx, cz, mesh.vertices.size(), mesh.indices.size());

            // Compute AABB
            chunk.aabbMin = Vec3(FLT_MAX);
            chunk.aabbMax = Vec3(-FLT_MAX);
            for (const auto& v : mesh.vertices) {
                chunk.aabbMin = glm::min(chunk.aabbMin, v.position);
                chunk.aabbMax = glm::max(chunk.aabbMax, v.position);
            }

            m_chunks.push_back(chunk);
        }
    }

    ERUPTION_LOG_INFO("TerrainRenderer: built %zu chunks, total verts=%zu indices=%zu", m_chunks.size(), totalVerts, totalIndices);
    rebuildChunkSoA();
    m_chunkSoADirty = false; // acabou de ser construido, esta' coerente
}

void TerrainRenderer::loadTerrainTextures(const TerrainFile& terrain, PackManager* assets) {
    for (uint16_t i = 0; i < static_cast<uint16_t>(terrain.textures.size()); ++i) {
        uint32_t slot = loadTerrainTexture(assets, terrain.textures[i]);
        if (slot != 0) {
            m_textureSlots[i] = slot;
            m_pbrTextureSlots[i] = m_textures.back().pbrSlot.valid() || m_textures.back().normalSlot.valid()
                ? PbrTextureSlots{ m_textures.back().pbrSlot, m_textures.back().normalSlot }
                : PbrTextureSlots{};
            ERUPTION_LOG_INFO("Terrain texture %u -> slot %u: %s", i, slot, terrain.textures[i].c_str());
        } else {
            ERUPTION_LOG_WARN("Failed to load terrain texture %u: %s", i, terrain.textures[i].c_str());
        }
    }
    m_bindless->flushUpdates();
}

uint32_t TerrainRenderer::loadTerrainTexture(PackManager* assets, const std::string& path) {
    if (!assets || path.empty()) return 0;

    std::string sanitizedPath = path;
    // We only replace slashes here, PackArchive will handle case-insensitive matching for ASCII
    // and preserve multibyte characters correctly.
    std::replace(sanitizedPath.begin(), sanitizedPath.end(), '/', '\\');

    std::vector<uint8_t> data = assets->extract(sanitizedPath);
    if (data.empty()) {
        // Fallback 1: terrain texture paths may be relative to data\texture
        data = assets->extract("data\\texture\\" + sanitizedPath);
        if (data.empty()) {
            // Fallback 2: try just the bare filename under data\texture
            size_t lastSlash = sanitizedPath.find_last_of('\\');
            if (lastSlash != std::string::npos) {
                std::string filename = sanitizedPath.substr(lastSlash + 1);
                data = assets->extract("data\\texture\\" + filename);
                if (data.empty()) {
                    data = assets->extract(filename);
                }
            }
        }
    }
    
    if (data.empty()) {
        ERUPTION_LOG_WARN("Failed to extract terrain texture: %s", sanitizedPath.c_str());
        return 0;
    }

    ImageData img = ImageUtils::loadFromMemory(data.data(), data.size());
    if (!img.isValid()) {
        ERUPTION_LOG_WARN("Failed to decode terrain texture: %s", sanitizedPath.c_str());
        return 0;
    }

    static int texCount = 0;
    if (texCount < 10) {
        std::string debugPath = "logs/debug_terrain_tex_" + std::to_string(texCount) + ".png";
        ImageUtils::writePNG(debugPath, img.width, img.height, 4, img.pixels.data());
        texCount++;
    }

    uint32_t mipLevels = m_mipmapsEnabled ? MipmapGenerator::calculateMipLevels(img.width, img.height) : 1;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = { static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), 1 };
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

    VkImage image;
    VmaAllocation alloc;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &image, &alloc, nullptr) != VK_SUCCESS) {
        return 0;
    }

    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    m_ctx->createBuffer(img.pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer, stagingAlloc);

    VmaAllocationInfo vmaInfo;
    vmaGetAllocationInfo(m_ctx->allocator(), stagingAlloc, &vmaInfo);
    if (vmaInfo.pMappedData) {
        std::memcpy(vmaInfo.pMappedData, img.pixels.data(), img.pixels.size());
    } else {
        void* mapped;
        vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
        std::memcpy(mapped, img.pixels.data(), img.pixels.size());
        vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);
    }

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            0, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = imageInfo.extent;
        vkCmdCopyBufferToImage(cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        m_ctx->cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        if (mipLevels > 1) {
            if (m_mipmapMode == MipGenerationMode::BLIT_LINEAR) {
                MipmapGenerator::generateMipmapsBlit(cmd, image, img.width, img.height, mipLevels);
            } else {
                MipmapGenerator::generateMipmapsCompute(cmd, m_ctx, image, img.width, img.height, mipLevels, true, 0);
            }
        } else {
            m_ctx->cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }
    });

    vmaDestroyBuffer(m_ctx->allocator(), stagingBuffer, stagingAlloc);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view;
    vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &view);

    uint32_t slot = m_bindless->allocateSlot();
    if (slot == 0) {
        vkDestroyImageView(m_ctx->device(), view, nullptr);
        vmaDestroyImage(m_ctx->allocator(), image, alloc);
        return 0;
    }

    m_bindless->updateTexture(slot, view, m_sampler);

    PbrTextureSlots pbrSlots = loadPbrTexturesForAlbedo(m_ctx, m_bindless, m_sampler, sanitizedPath);

    TerrainTexture tex;
    tex.image = image;
    tex.alloc = alloc;
    tex.view = view;
    tex.bindlessSlot = slot;
    tex.pbrSlot = pbrSlots.mrahw;
    tex.normalSlot = pbrSlots.normal;
    tex.width = img.width;
    tex.height = img.height;
    tex.mipLevels = mipLevels;
    m_textures.push_back(tex);

    return slot;
}

void TerrainRenderer::uploadChunk(const TerrainMesh& mesh, TerrainChunkGPU& chunk) {
    VkDeviceSize vertexSize = mesh.vertices.size() * sizeof(TerrainVertex);
    VkDeviceSize indexSize = mesh.indices.size() * sizeof(uint32_t);
    chunk.indexCount = static_cast<uint32_t>(mesh.indices.size());

    // Vertex buffer
    m_ctx->createBuffer(vertexSize,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, chunk.vertexBuffer, chunk.vertexAlloc);

    // Index buffer
    m_ctx->createBuffer(indexSize,
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, chunk.indexBuffer, chunk.indexAlloc);

    // Staging and upload
    VkBuffer stagingBuf;
    VmaAllocation stagingAlloc;
    VkDeviceSize totalSize = vertexSize + indexSize;
    m_ctx->createBuffer(totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_CPU_ONLY, stagingBuf, stagingAlloc);

    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, mesh.vertices.data(), vertexSize);
    std::memcpy(static_cast<uint8_t*>(mapped) + vertexSize, mesh.indices.data(), indexSize);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy vCopy{};
        vCopy.size = vertexSize;
        vkCmdCopyBuffer(cmd, stagingBuf, chunk.vertexBuffer, 1, &vCopy);

        VkBufferCopy iCopy{};
        iCopy.srcOffset = vertexSize;
        iCopy.size = indexSize;
        vkCmdCopyBuffer(cmd, stagingBuf, chunk.indexBuffer, 1, &iCopy);
    });

    vmaDestroyBuffer(m_ctx->allocator(), stagingBuf, stagingAlloc);
}

void TerrainRenderer::render(VkCommandBuffer cmd, const Mat4& viewProj, const Frustum& frustum) {
    uint32_t dbgIdx = 0;
    PROFILE_CPU_SCOPE(ProfilerCategory::Culling);
    if (!m_initialized) {
        ERUPTION_LOG_WARN("TerrainRenderer::render skipped: not initialized");
        return;
    }
    if (m_chunks.empty()) {
        return;
    }

    // SIMD/SoA frustum cull; fills m_visibleChunkIndices.
    cullChunks(frustum);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    VkDescriptorSet bindlessSet = m_bindless->set();
    if (m_frameUboSet != VK_NULL_HANDLE) {
        VkDescriptorSet sets[2] = { bindlessSet, m_frameUboSet };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                0, 2, sets, 0, nullptr);
    } else {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                0, 1, &bindlessSet, 0, nullptr);
    }

    int rendered = 0;
    for (uint32_t idx : m_visibleChunkIndices) {
        const TerrainChunkGPU& chunk = m_chunks[idx];

        // Per-chunk PBR material profile scale. A single scale per chunk is an
        // approximation for mixed-texture chunks, but it avoids splitting the
        // chunk into multiple draw calls.
        struct {
            Mat4 vp;
            float metallicScale;
            float roughnessScale;
            float __pad[2];
        } push;
        push.vp = viewProj;
        push.metallicScale = chunk.metallicScale;
        push.roughnessScale = chunk.roughnessScale;

        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);

        VkBuffer vb = chunk.vertexBuffer;
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, chunk.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, chunk.indexCount, 1, 0, 0, 0);
        dbgIdx += chunk.indexCount;
        rendered++;
    }
    // O terreno desenha no MESMO passe G-Buffer que os modelos; a telemetria
    // soma os dois para dar o total de triangulos na tela.
    m_lastDrawnTriangles = dbgIdx / 3;
    m_lastDrawnChunks = static_cast<uint32_t>(rendered);

    // ERUPTION_DEBUG_DRAWSTATS=1: o terreno desenha no MESMO passe G-Buffer que
    // os modelos, mas nunca foi contado - o [DRAWSTATS] do ModelRenderer so' ve
    // modelo. Sem este numero nao da' para saber qual metade do passe otimizar.
    static const bool kStats = std::getenv("ERUPTION_DEBUG_DRAWSTATS") != nullptr;
    if (kStats) {
        static int f = 0;
        if (++f % 120 == 0) {
            ERUPTION_LOG_WARN("[TERRENO] chunks=%zu desenhados=%d triangulos=%u",
                              m_chunks.size(), rendered, dbgIdx / 3);
        }
    }
    if (rendered == 0) {
        // Throttle: isto disparava POR FRAME sempre que a camera olhava para
        // fora do terreno - time+localtime+3 fprintf sem buffer por frame
        // (varredura de hot paths 2026-09-02, item 5). Uma vez a cada ~5 s.
        static int s_zeroChunkCooldown = 0;
        if (s_zeroChunkCooldown <= 0) {
            ERUPTION_LOG_WARN("TerrainRenderer::render: no chunks passed frustum culling (total=%zu)", m_chunks.size());
            s_zeroChunkCooldown = 300;
        } else {
            --s_zeroChunkCooldown;
        }
    }
}

void TerrainRenderer::renderShadow(VkCommandBuffer cmd, VkPipeline shadowPipeline, VkPipelineLayout shadowLayout,
                                   const Mat4& cascadeMatrix, const Frustum& shadowFrustum, bool enableCulling) {
    if (!m_initialized || m_chunks.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);

    for (const auto& chunk : m_chunks) {
        if (enableCulling && !shadowFrustum.intersectsAABB(chunk.aabbMin, chunk.aabbMax)) {
            continue;
        }

        vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(Mat4), &cascadeMatrix);

        VkBuffer vb = chunk.vertexBuffer;
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, chunk.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, chunk.indexCount, 1, 0, 0, 0);
    }
}

void TerrainRenderer::rebuildChunkSoA() {
    const uint32_t count = static_cast<uint32_t>(m_chunks.size());
    m_paddedChunkCount = (count + 7u) & ~7u;
    m_chunkSoA.resize(m_paddedChunkCount);

    for (uint32_t i = 0; i < count; ++i) {
        const TerrainChunkGPU& chunk = m_chunks[i];
        m_chunkSoA.minX[i] = chunk.aabbMin.x;
        m_chunkSoA.minY[i] = chunk.aabbMin.y;
        m_chunkSoA.minZ[i] = chunk.aabbMin.z;
        m_chunkSoA.maxX[i] = chunk.aabbMax.x;
        m_chunkSoA.maxY[i] = chunk.aabbMax.y;
        m_chunkSoA.maxZ[i] = chunk.aabbMax.z;
    }

    // Pad trailing entries so the AVX2 kernel does not read garbage.
    for (uint32_t i = count; i < m_paddedChunkCount; ++i) {
        m_chunkSoA.minX[i] = 0.0f;
        m_chunkSoA.minY[i] = 0.0f;
        m_chunkSoA.minZ[i] = 0.0f;
        m_chunkSoA.maxX[i] = 0.0f;
        m_chunkSoA.maxY[i] = 0.0f;
        m_chunkSoA.maxZ[i] = 0.0f;
    }
}

void TerrainRenderer::cullChunks(const Frustum& frustum) {
    const uint32_t count = static_cast<uint32_t>(m_chunks.size());
    m_visibleChunkIndices.clear();
    if (count == 0) return;

    // O SoA e' reconstruido SO' quando os chunks mudam (load/rebuild): os AABB
    // de terreno sao ESTATICOS depois do build. Antes isto rodava por frame
    // (varredura completa do AoS 72B lendo 24B + 6 escritas por chunk) para
    // produzir dados identicos. (Varredura de hot paths 2026-09-02.)
    if (m_chunkSoADirty || m_chunkSoA.minX.size() < m_paddedChunkCount) {
        rebuildChunkSoA();
        m_chunkSoADirty = false;
    }

    const uint32_t maskBytes = (count + 7) / 8;
    // Membro reusado: era alocado e liberado por frame (o ModelRenderer ja'
    // fazia certo com m_visibleMaskScratch; espelhado aqui).
    m_visibleMaskScratch.assign(maskBytes, 0);
    auto& visibleMask = m_visibleMaskScratch;
    FrustumPlanesSoA planes = convertFrustumToSoA(frustum);

    cullAABBSoA(
        m_chunkSoA.minX.data(),
        m_chunkSoA.minY.data(),
        m_chunkSoA.minZ.data(),
        m_chunkSoA.maxX.data(),
        m_chunkSoA.maxY.data(),
        m_chunkSoA.maxZ.data(),
        m_paddedChunkCount,
        planes,
        visibleMask.data());

    m_visibleChunkIndices.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t byte = visibleMask[i / 8];
        if ((byte & (1u << (i & 7))) != 0) {
            m_visibleChunkIndices.push_back(i);
        }
    }
}

} // namespace eruption
