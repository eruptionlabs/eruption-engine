#include <cstdlib>
#include "WaterRenderer.hpp"
#include "core/Logger.hpp"
#include "renderer/ShaderCompiler.hpp"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include <stb_image.h>

namespace eruption {

struct FoamAccumPushConstants {
    Vec4 params;      // x=decay, y=foamEdgeDepth, z=nearPlane, w=farPlane
    Vec4 moreParams;  // x=waterLevel, yzw=unused
    Vec2 texelSize;
    float enable;
    float _pad;
    Mat4 invViewProj;
};

bool WaterRenderer::init(VulkanContext* ctx, BindlessDescriptor* bindless) {
    m_ctx = ctx;
    m_bindless = bindless;
    createUBO();
    createFoamMaskResources();
    createPipeline();
    return m_pipeline != VK_NULL_HANDLE;
}

void WaterRenderer::shutdown() {
    clearWaterMesh();
    destroyRefractionResources();
    destroyFoamAccumResources();
    destroyFoamAccumPipeline();
    destroyFoamMaskResources();
    destroyWaterTextureResources();
    destroyUBO();
    destroyPipeline();
    m_ctx = nullptr;
    m_bindless = nullptr;
}

void WaterRenderer::clearWaterMesh() {
    if (m_vertexBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_vertexBuffer, m_vertexAlloc);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexAlloc = VK_NULL_HANDLE;
    }
    if (m_indexBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_indexBuffer, m_indexAlloc);
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexAlloc = VK_NULL_HANDLE;
    }
    m_indexCount = 0;
}

void WaterRenderer::clearLavaMesh() {
    if (m_lavaVertexBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_lavaVertexBuffer, m_lavaVertexAlloc);
        m_lavaVertexBuffer = VK_NULL_HANDLE;
        m_lavaVertexAlloc = VK_NULL_HANDLE;
    }
    if (m_lavaIndexBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_lavaIndexBuffer, m_lavaIndexAlloc);
        m_lavaIndexBuffer = VK_NULL_HANDLE;
        m_lavaIndexAlloc = VK_NULL_HANDLE;
    }
    m_lavaIndexCount = 0;
}

void WaterRenderer::setLavaMesh(const WaterMesh& mesh) {
    clearLavaMesh();
    if (mesh.vertices.empty() || mesh.indices.empty()) return;

    VkDeviceSize vertexSize = mesh.vertices.size() * sizeof(WaterVertex);
    VkDeviceSize indexSize = mesh.indices.size() * sizeof(uint32_t);
    m_lavaIndexCount = static_cast<uint32_t>(mesh.indices.size());

    if (!m_ctx->createBuffer(vertexSize,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_lavaVertexBuffer, m_lavaVertexAlloc)) {
        clearLavaMesh();
        return;
    }
    if (!m_ctx->createBuffer(indexSize,
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_lavaIndexBuffer, m_lavaIndexAlloc)) {
        clearLavaMesh();
        return;
    }

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
        vkCmdCopyBuffer(cmd, stagingBuf, m_lavaVertexBuffer, 1, &vCopy);
        VkBufferCopy iCopy{};
        iCopy.srcOffset = vertexSize;
        iCopy.size = indexSize;
        vkCmdCopyBuffer(cmd, stagingBuf, m_lavaIndexBuffer, 1, &iCopy);
    });

    vmaDestroyBuffer(m_ctx->allocator(), stagingBuf, stagingAlloc);
}

void WaterRenderer::setWaterMesh(const WaterMesh& mesh) {
    clearWaterMesh();
    if (mesh.vertices.empty() || mesh.indices.empty()) return;

    VkDeviceSize vertexSize = mesh.vertices.size() * sizeof(WaterVertex);
    VkDeviceSize indexSize = mesh.indices.size() * sizeof(uint32_t);
    m_indexCount = static_cast<uint32_t>(mesh.indices.size());

    if (!m_ctx->createBuffer(vertexSize,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_vertexBuffer, m_vertexAlloc)) {
        ERUPTION_LOG_ERROR("WaterRenderer: failed to create vertex buffer");
        clearWaterMesh();
        return;
    }

    if (!m_ctx->createBuffer(indexSize,
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_indexBuffer, m_indexAlloc)) {
        ERUPTION_LOG_ERROR("WaterRenderer: failed to create index buffer");
        clearWaterMesh();
        return;
    }

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
        vkCmdCopyBuffer(cmd, stagingBuf, m_vertexBuffer, 1, &vCopy);

        VkBufferCopy iCopy{};
        iCopy.srcOffset = vertexSize;
        iCopy.size = indexSize;
        vkCmdCopyBuffer(cmd, stagingBuf, m_indexBuffer, 1, &iCopy);
    });

    vmaDestroyBuffer(m_ctx->allocator(), stagingBuf, stagingAlloc);
}

void WaterRenderer::setViewport(float width, float height) {
    m_viewportW = width;
    m_viewportH = height;
}

// ============================================================================
// UBO
// ============================================================================
void WaterRenderer::createUBO() {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboBinding;
    VK_CHECK_VOID(vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_uboLayout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_FRAMES * UBO_BANKS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES * UBO_BANKS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK_VOID(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_uboPool));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_uboPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_uboLayout;
    for (uint32_t i = 0; i < MAX_FRAMES * UBO_BANKS; i++) {
        VK_CHECK_VOID(vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_uboSets[i]));
    }

    for (uint32_t i = 0; i < MAX_FRAMES * UBO_BANKS; i++) {
        m_ctx->createBuffer(sizeof(WaterUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VMA_MEMORY_USAGE_CPU_TO_GPU, m_uboBuffers[i], m_uboAllocs[i]);
        vmaMapMemory(m_ctx->allocator(), m_uboAllocs[i], &m_uboMappeds[i]);

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_uboBuffers[i];
        bufInfo.offset = 0;
        bufInfo.range = sizeof(WaterUBO);

        VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_uboSets[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    }
}

void WaterRenderer::destroyUBO() {
    for (uint32_t i = 0; i < MAX_FRAMES * UBO_BANKS; i++) {
        if (m_uboBuffers[i]) {
            vmaUnmapMemory(m_ctx->allocator(), m_uboAllocs[i]);
            vmaDestroyBuffer(m_ctx->allocator(), m_uboBuffers[i], m_uboAllocs[i]);
            m_uboBuffers[i] = VK_NULL_HANDLE;
            m_uboAllocs[i] = VK_NULL_HANDLE;
            m_uboMappeds[i] = nullptr;
        }
    }
    if (m_uboPool) {
        vkDestroyDescriptorPool(m_ctx->device(), m_uboPool, nullptr);
        m_uboPool = VK_NULL_HANDLE;
    }
    if (m_uboLayout) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_uboLayout, nullptr);
        m_uboLayout = VK_NULL_HANDLE;
    }
}

// ============================================================================
// Refraction targets
// ============================================================================
void WaterRenderer::resizeRefraction(uint32_t width, uint32_t height) {
    if (m_refractionImage[0] != VK_NULL_HANDLE) createRefractionResources(width, height);
}

void WaterRenderer::createRefractionResources(uint32_t width, uint32_t height) {
    if (m_refractionImage[0] != VK_NULL_HANDLE && m_refractionWidth == width && m_refractionHeight == height)
        return;

    destroyRefractionResources();
    m_refractionWidth = width;
    m_refractionHeight = height;

    // Color copy (one per frame in flight)
    {
        VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;
        vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_refractionSampler);

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_refractionImage[i], &m_refractionAlloc[i], nullptr);
            viewInfo.image = m_refractionImage[i];
            vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_refractionView[i]);
            m_refractionSlot[i] = m_bindless->allocateSlot();
            m_bindless->updateTexture(m_refractionSlot[i], m_refractionView[i], m_refractionSampler);
        }
        m_bindless->flushUpdates();
    }

    // Depth copy (one per frame in flight)
    {
        VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

        VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;
        vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_depthCopySampler);

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_depthCopyImage[i], &m_depthCopyAlloc[i], nullptr);
            viewInfo.image = m_depthCopyImage[i];
            vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_depthCopyView[i]);
            m_depthCopySlot[i] = m_bindless->allocateSlot();
            m_bindless->updateTexture(m_depthCopySlot[i], m_depthCopyView[i], m_depthCopySampler);
        }
        m_bindless->flushUpdates();
    }
}

void WaterRenderer::destroyRefractionResources() {
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        if (m_refractionSlot[i] != 0) { m_bindless->freeSlot(m_refractionSlot[i]); m_refractionSlot[i] = 0; }
        if (m_depthCopySlot[i] != 0) { m_bindless->freeSlot(m_depthCopySlot[i]); m_depthCopySlot[i] = 0; }
        if (m_refractionView[i]) { vkDestroyImageView(m_ctx->device(), m_refractionView[i], nullptr); m_refractionView[i] = VK_NULL_HANDLE; }
        if (m_depthCopyView[i]) { vkDestroyImageView(m_ctx->device(), m_depthCopyView[i], nullptr); m_depthCopyView[i] = VK_NULL_HANDLE; }
        if (m_refractionImage[i]) { vmaDestroyImage(m_ctx->allocator(), m_refractionImage[i], m_refractionAlloc[i]); m_refractionImage[i] = VK_NULL_HANDLE; m_refractionAlloc[i] = VK_NULL_HANDLE; }
        if (m_depthCopyImage[i]) { vmaDestroyImage(m_ctx->allocator(), m_depthCopyImage[i], m_depthCopyAlloc[i]); m_depthCopyImage[i] = VK_NULL_HANDLE; m_depthCopyAlloc[i] = VK_NULL_HANDLE; }
    }

    if (m_refractionSampler) { vkDestroySampler(m_ctx->device(), m_refractionSampler, nullptr); m_refractionSampler = VK_NULL_HANDLE; }
    if (m_depthCopySampler) { vkDestroySampler(m_ctx->device(), m_depthCopySampler, nullptr); m_depthCopySampler = VK_NULL_HANDLE; }

    m_refractionWidth = 0;
    m_refractionHeight = 0;
}

// ============================================================================
// Foam accumulation (edge foam persistence)
// ============================================================================

void WaterRenderer::createFoamAccumResources(uint32_t width, uint32_t height) {
    uint32_t w = width / 2;
    uint32_t h = height / 2;
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    if (m_foamAccumImage[0] != VK_NULL_HANDLE && m_foamAccumWidth == w && m_foamAccumHeight == h)
        return;

    destroyFoamAccumResources();
    m_foamAccumWidth = w;
    m_foamAccumHeight = h;
    m_currentFoamAccumIndex = 0;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = w;
    imageInfo.extent.height = h;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_foamAccumSampler);

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    for (uint32_t i = 0; i < FOAM_ACCUM_COUNT; ++i) {
        vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_foamAccumImage[i], &m_foamAccumAlloc[i], nullptr);
        viewInfo.image = m_foamAccumImage[i];
        vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_foamAccumView[i]);
        m_foamAccumSlot[i] = m_bindless->allocateSlot();
        m_bindless->updateTexture(m_foamAccumSlot[i], m_foamAccumView[i], m_foamAccumSampler);
    }
    m_bindless->flushUpdates();

    // Clear to black
    VkClearColorValue clearColor{};
    clearColor.float32[0] = 0.0f;

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        for (uint32_t i = 0; i < FOAM_ACCUM_COUNT; ++i) {
            m_ctx->cmdImageBarrier(cmd, m_foamAccumImage[i],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, m_foamAccumImage[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

            m_ctx->cmdImageBarrier(cmd, m_foamAccumImage[i],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    });
}

void WaterRenderer::destroyFoamAccumResources() {
    for (uint32_t i = 0; i < FOAM_ACCUM_COUNT; ++i) {
        if (m_foamAccumSlot[i] != 0) { m_bindless->freeSlot(m_foamAccumSlot[i]); m_foamAccumSlot[i] = 0; }
        if (m_foamAccumView[i]) { vkDestroyImageView(m_ctx->device(), m_foamAccumView[i], nullptr); m_foamAccumView[i] = VK_NULL_HANDLE; }
        if (m_foamAccumImage[i]) { vmaDestroyImage(m_ctx->allocator(), m_foamAccumImage[i], m_foamAccumAlloc[i]); m_foamAccumImage[i] = VK_NULL_HANDLE; m_foamAccumAlloc[i] = VK_NULL_HANDLE; }
    }
    if (m_foamAccumSampler) { vkDestroySampler(m_ctx->device(), m_foamAccumSampler, nullptr); m_foamAccumSampler = VK_NULL_HANDLE; }
    m_foamAccumWidth = 0;
    m_foamAccumHeight = 0;
    m_currentFoamAccumIndex = 0;
}

void WaterRenderer::createFoamAccumPipeline() {
    if (m_foamAccumPipeline != VK_NULL_HANDLE) return;

    auto compCode = ShaderCompiler::loadSPIRV("shaders/water/foam_accum.comp.spv");

    VkShaderModule compModule = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = compCode.size() * sizeof(uint32_t);
    smInfo.pCode = compCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &compModule);

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    VK_CHECK_VOID(vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_foamAccumDescLayout));

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(FoamAccumPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_foamAccumDescLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pcRange;
    VK_CHECK_VOID(vkCreatePipelineLayout(m_ctx->device(), &pipelineLayoutInfo, nullptr, &m_foamAccumLayout));

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = compModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = m_foamAccumLayout;
    VK_CHECK_VOID(vkCreateComputePipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipelineInfo, nullptr, &m_foamAccumPipeline));

    vkDestroyShaderModule(m_ctx->device(), compModule, nullptr);

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK_VOID(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_foamAccumDescPool));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_foamAccumDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_foamAccumDescLayout;
    VK_CHECK_VOID(vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_foamAccumDescSet));
}

void WaterRenderer::destroyFoamAccumPipeline() {
    if (m_foamAccumDescPool) {
        vkDestroyDescriptorPool(m_ctx->device(), m_foamAccumDescPool, nullptr);
        m_foamAccumDescPool = VK_NULL_HANDLE;
    }
    if (m_foamAccumPipeline) {
        vkDestroyPipeline(m_ctx->device(), m_foamAccumPipeline, nullptr);
        m_foamAccumPipeline = VK_NULL_HANDLE;
    }
    if (m_foamAccumLayout) {
        vkDestroyPipelineLayout(m_ctx->device(), m_foamAccumLayout, nullptr);
        m_foamAccumLayout = VK_NULL_HANDLE;
    }
    if (m_foamAccumDescLayout) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_foamAccumDescLayout, nullptr);
        m_foamAccumDescLayout = VK_NULL_HANDLE;
    }
    m_foamAccumDescSet = VK_NULL_HANDLE;
}

void WaterRenderer::dispatchFoamAccum(VkCommandBuffer cmd, VkImage depthImage, uint32_t depthSlot,
                                      float nearPlane, float farPlane, float foamEdgeDepth,
                                      float waterLevel, const Mat4& invViewProj,
                                      const WaterSettings& settings) {
    (void)depthImage;
    (void)depthSlot;

    (void)settings;
    (void)nearPlane;
    (void)farPlane;
    (void)foamEdgeDepth;
    (void)waterLevel;
    (void)invViewProj;
    return; // foam accum disabled

    uint32_t targetW = m_refractionWidth > 0 ? m_refractionWidth : static_cast<uint32_t>(m_viewportW);
    uint32_t targetH = m_refractionHeight > 0 ? m_refractionHeight : static_cast<uint32_t>(m_viewportH);
    if (targetW == 0) targetW = 1;
    if (targetH == 0) targetH = 1;

    createFoamAccumResources(targetW, targetH);
    createFoamAccumPipeline();

    uint32_t prevIndex = m_currentFoamAccumIndex;
    uint32_t outputIndex = 1 - prevIndex;
    uint32_t frameIndex = m_ctx->currentFrame() % MAX_FRAMES;

    // Transition prev accum to shader read
    m_ctx->cmdImageBarrier(cmd, m_foamAccumImage[prevIndex],
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    // Transition output accum to general for writing
    m_ctx->cmdImageBarrier(cmd, m_foamAccumImage[outputIndex],
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    // Update descriptor set
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthInfo.imageView = m_depthCopyView[frameIndex];
    depthInfo.sampler = m_depthCopySampler;

    VkDescriptorImageInfo prevFoamInfo{};
    prevFoamInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    prevFoamInfo.imageView = m_foamAccumView[prevIndex];
    prevFoamInfo.sampler = m_foamAccumSampler;

    VkDescriptorImageInfo outputFoamInfo{};
    outputFoamInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputFoamInfo.imageView = m_foamAccumView[outputIndex];

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_foamAccumDescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &depthInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_foamAccumDescSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &prevFoamInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_foamAccumDescSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &outputFoamInfo;

    vkUpdateDescriptorSets(m_ctx->device(), 3, writes, 0, nullptr);

    // Push constants
    // foam accum disabled — do nothing
    (void)outputIndex;
}

// ============================================================================
// Contact foam mask
// ============================================================================
void WaterRenderer::createFoamMaskResources() {
    m_foamMaskData.assign(FOAM_MASK_SIZE * FOAM_MASK_SIZE, 0);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = FOAM_MASK_SIZE;
    imageInfo.extent.height = FOAM_MASK_SIZE;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_foamMaskImage, &m_foamMaskAlloc, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_foamMaskImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_foamMaskView);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_foamMaskSampler);

    m_foamMaskSlot = m_bindless->allocateSlot();
    m_bindless->updateTexture(m_foamMaskSlot, m_foamMaskView, m_foamMaskSampler);
    m_bindless->flushUpdates();

    // Staging buffer for CPU uploads
    m_ctx->createBuffer(FOAM_MASK_SIZE * FOAM_MASK_SIZE,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_CPU_TO_GPU,
                        m_foamMaskStagingBuffer, m_foamMaskStagingAlloc);

    // Zero-initialize staging buffer and GPU image
    void* mapped;
    vmaMapMemory(m_ctx->allocator(), m_foamMaskStagingAlloc, &mapped);
    std::memset(mapped, 0, FOAM_MASK_SIZE * FOAM_MASK_SIZE);
    vmaUnmapMemory(m_ctx->allocator(), m_foamMaskStagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, m_foamMaskImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {FOAM_MASK_SIZE, FOAM_MASK_SIZE, 1};
        vkCmdCopyBufferToImage(cmd, m_foamMaskStagingBuffer, m_foamMaskImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        m_ctx->cmdImageBarrier(cmd, m_foamMaskImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    });
}

void WaterRenderer::destroyFoamMaskResources() {
    if (m_foamMaskSlot != 0) { m_bindless->freeSlot(m_foamMaskSlot); m_foamMaskSlot = 0; }
    if (m_foamMaskSampler) { vkDestroySampler(m_ctx->device(), m_foamMaskSampler, nullptr); m_foamMaskSampler = VK_NULL_HANDLE; }
    if (m_foamMaskView) { vkDestroyImageView(m_ctx->device(), m_foamMaskView, nullptr); m_foamMaskView = VK_NULL_HANDLE; }
    if (m_foamMaskImage) { vmaDestroyImage(m_ctx->allocator(), m_foamMaskImage, m_foamMaskAlloc); m_foamMaskImage = VK_NULL_HANDLE; m_foamMaskAlloc = VK_NULL_HANDLE; }
    if (m_foamMaskStagingBuffer) { vmaDestroyBuffer(m_ctx->allocator(), m_foamMaskStagingBuffer, m_foamMaskStagingAlloc); m_foamMaskStagingBuffer = VK_NULL_HANDLE; m_foamMaskStagingAlloc = VK_NULL_HANDLE; }
    m_foamMaskData.clear();
}

void WaterRenderer::updateContactFoam(float deltaTime, const WaterSettings& settings) {
    if (m_foamMaskData.empty()) return;

    m_foamMaskScale = settings.foamMaskScale;
    m_contactFoamSpread = settings.contactFoamSpread;

    const int S = static_cast<int>(FOAM_MASK_SIZE);
    const int N = S * S;

    // --- Diffusion blur (separable 3-tap) ---
    // Reuse persistent buffers to avoid 2 MB of heap allocations every frame.
    if (static_cast<int>(m_foamBlurTmp.size()) != N) m_foamBlurTmp.assign(N, 0.0f);
    if (static_cast<int>(m_foamBlurOut.size()) != N) m_foamBlurOut.assign(N, 0.0f);
    std::fill(m_foamBlurTmp.begin(), m_foamBlurTmp.end(), 0.0f);
    std::fill(m_foamBlurOut.begin(), m_foamBlurOut.end(), 0.0f);

    // Horizontal pass: kernel [1, 2, 1] / 4
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            float c = static_cast<float>(m_foamMaskData[y * S + x]) / 255.0f;
            float l = static_cast<float>(m_foamMaskData[y * S + std::max(0, x - 1)]) / 255.0f;
            float r = static_cast<float>(m_foamMaskData[y * S + std::min(S - 1, x + 1)]) / 255.0f;
            m_foamBlurTmp[y * S + x] = (l + c * 2.0f + r) * 0.25f;
        }
    }
    // Vertical pass
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            float c = m_foamBlurTmp[y * S + x];
            float t = m_foamBlurTmp[std::max(0, y - 1) * S + x];
            float b = m_foamBlurTmp[std::min(S - 1, y + 1) * S + x];
            m_foamBlurOut[y * S + x] = (t + c * 2.0f + b) * 0.25f;
        }
    }

    // --- Fade ---
    // Exponential decay so the slider actually feels responsive
    float fade = std::exp(-deltaTime * settings.contactFoamDecay * 10.0f);
    bool anyNonZero = false;
    for (int i = 0; i < N; ++i) {
        float f = m_foamBlurOut[i] * fade;
        if (f < 1.0f / 255.0f) f = 0.0f;
        uint8_t v = static_cast<uint8_t>(f * 255.0f);
        m_foamMaskData[i] = v;
        if (v != 0) anyNonZero = true;
    }

    m_foamMaskDirty = true;
    (void)anyNonZero;
}

void WaterRenderer::addContactFoam(const Vec3& worldPos, float radius, float intensity) {
    if (m_foamMaskData.empty()) return;

    float u = worldPos.x / m_foamMaskScale.x + 0.5f;
    float v = worldPos.z / m_foamMaskScale.y + 0.5f;

    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return;

    int cx = static_cast<int>(u * FOAM_MASK_SIZE);
    int cy = static_cast<int>(v * FOAM_MASK_SIZE);
    float worldRadius = std::max(radius, 0.01f);
    // Spread increases radius up to 4x; fade profile stays the same (sharp gaussian)
    float spread = m_contactFoamSpread;
    float effectiveRadius = worldRadius * (1.0f + spread * 3.0f);
    int r = static_cast<int>((effectiveRadius / std::max(m_foamMaskScale.x, m_foamMaskScale.y)) * FOAM_MASK_SIZE);
    if (r < 1) r = 1;

    float sharpness = 3.0f;

    for (int y = -r; y <= r; ++y) {
        int py = cy + y;
        if (py < 0 || py >= static_cast<int>(FOAM_MASK_SIZE)) continue;
        for (int x = -r; x <= r; ++x) {
            int px = cx + x;
            if (px < 0 || px >= static_cast<int>(FOAM_MASK_SIZE)) continue;
            float dist = std::sqrt(static_cast<float>(x*x + y*y)) / static_cast<float>(r);
            if (dist > 1.0f) continue;
            // Gaussian falloff: strong in center, soft edges
            float gauss = std::exp(-dist * dist * sharpness);
            int value = static_cast<int>(glm::clamp(intensity, 0.0f, 1.0f) * gauss * 255.0f);
            if (value <= 0) continue;
            size_t idx = static_cast<size_t>(py) * FOAM_MASK_SIZE + px;
            int cur = m_foamMaskData[idx];
            int merged = std::min(255, cur + value);
            m_foamMaskData[idx] = static_cast<uint8_t>(merged);
        }
    }

    m_foamMaskDirty = true;
}

void WaterRenderer::addContactFoamAABB(const Vec3& minPos, const Vec3& maxPos, float intensity) {
    if (m_foamMaskData.empty()) return;

    Vec3 center = (minPos + maxPos) * 0.5f;
    float radius = std::max(maxPos.x - minPos.x, maxPos.z - minPos.z) * 0.5f;

    float u = center.x / m_foamMaskScale.x + 0.5f;
    float v = center.z / m_foamMaskScale.y + 0.5f;

    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return;

    int cx = static_cast<int>(u * FOAM_MASK_SIZE);
    int cy = static_cast<int>(v * FOAM_MASK_SIZE);
    float spread = m_contactFoamSpread;
    float effectiveRadius = radius * (1.0f + spread * 3.0f);
    int r = static_cast<int>((effectiveRadius / std::max(m_foamMaskScale.x, m_foamMaskScale.y)) * FOAM_MASK_SIZE);
    if (r < 1) r = 1;

    float sharpness = 3.0f;

    for (int y = -r; y <= r; ++y) {
        int py = cy + y;
        if (py < 0 || py >= static_cast<int>(FOAM_MASK_SIZE)) continue;
        for (int x = -r; x <= r; ++x) {
            int px = cx + x;
            if (px < 0 || px >= static_cast<int>(FOAM_MASK_SIZE)) continue;
            float dist = std::sqrt(static_cast<float>(x*x + y*y)) / static_cast<float>(r);
            if (dist > 1.0f) continue;
            float gauss = std::exp(-dist * dist * sharpness);
            int value = static_cast<int>(glm::clamp(intensity, 0.0f, 1.0f) * gauss * 255.0f);
            if (value <= 0) continue;

            size_t idx = static_cast<size_t>(py) * FOAM_MASK_SIZE + px;
            int cur = m_foamMaskData[idx];
            int merged = std::min(255, cur + value);
            m_foamMaskData[idx] = static_cast<uint8_t>(merged);
        }
    }

    m_foamMaskDirty = true;
}

void WaterRenderer::addContactFoamTrail(const Vec3& from, const Vec3& to, float radius, float intensity) {
    if (m_foamMaskData.empty()) return;

    Vec2 from2(from.x, from.z);
    Vec2 to2(to.x, to.z);
    Vec2 delta = to2 - from2;
    float dist = glm::length(delta);
    if (dist < 0.01f) {
        addContactFoam(from, radius, intensity);
        return;
    }

    float step = radius * 0.5f;
    int steps = static_cast<int>(dist / step) + 1;
    Vec2 dir = delta / dist;

    for (int i = 0; i <= steps; ++i) {
        float t = float(i) / float(steps);
        // Elliptical fade: stronger in middle, weaker at ends
        float fade = 1.0f - std::abs(t - 0.5f) * 2.0f;
        fade = fade * fade;
        Vec2 pos2 = from2 + dir * (t * dist);
        Vec3 pos3(pos2.x, 0.0f, pos2.y);
        addContactFoam(pos3, radius, intensity * fade);
    }
}

void WaterRenderer::uploadFoamMask(VkCommandBuffer cmd) {
    if (!m_foamMaskDirty || m_foamMaskData.empty()) return;

    void* mapped;
    vmaMapMemory(m_ctx->allocator(), m_foamMaskStagingAlloc, &mapped);
    std::memcpy(mapped, m_foamMaskData.data(), m_foamMaskData.size());
    vmaUnmapMemory(m_ctx->allocator(), m_foamMaskStagingAlloc);

    m_ctx->cmdImageBarrier(cmd, m_foamMaskImage,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {FOAM_MASK_SIZE, FOAM_MASK_SIZE, 1};
    vkCmdCopyBufferToImage(cmd, m_foamMaskStagingBuffer, m_foamMaskImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    m_ctx->cmdImageBarrier(cmd, m_foamMaskImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    m_foamMaskDirty = false;
}

// ============================================================================
// Scene copy for refraction
// ============================================================================
void WaterRenderer::copySceneToRefraction(VkCommandBuffer cmd,
                                          VkImage sceneColorImage, VkImage sceneDepthImage,
                                          uint32_t width, uint32_t height,
                                          uint32_t frameIndex) {
    // Color
    m_ctx->cmdImageBarrier(cmd, sceneColorImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    m_ctx->cmdImageBarrier(cmd, m_refractionImage[frameIndex],
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkImageCopy copyRegionColor{};
    copyRegionColor.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegionColor.srcSubresource.layerCount = 1;
    copyRegionColor.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegionColor.dstSubresource.layerCount = 1;
    copyRegionColor.extent = {width, height, 1};
    vkCmdCopyImage(cmd, sceneColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_refractionImage[frameIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegionColor);

    m_ctx->cmdImageBarrier(cmd, sceneColorImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    m_ctx->cmdImageBarrier(cmd, m_refractionImage[frameIndex],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // Depth
    m_ctx->cmdImageBarrier(cmd, sceneDepthImage,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    m_ctx->cmdImageBarrier(cmd, m_depthCopyImage[frameIndex],
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkImageCopy copyRegion{};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.extent = {width, height, 1};
    vkCmdCopyImage(cmd, sceneDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_depthCopyImage[frameIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    m_ctx->cmdImageBarrier(cmd, sceneDepthImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    m_ctx->cmdImageBarrier(cmd, m_depthCopyImage[frameIndex],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
}

// ============================================================================
// UBO update
// ============================================================================
void WaterRenderer::updateUBO(uint32_t frameIndex, uint32_t uboSlot, const WaterSettings& settings,
                               float nearPlane, float farPlane,
                               const DayNightCycle& cycle, float time, const Vec3& cameraPos,
                               float cameraPitch,
                               float sunOcclusion, float moonOcclusion, float stormDarken,
                               float rainIntensity, float rainSplashIntensity) {
    WaterUBO ubo{};
    ubo.baseColorDeep    = Vec4(settings.baseColorDeep, 1.0f);
    ubo.baseColorShallow = Vec4(settings.baseColorShallow, 1.0f);
    ubo.waterParams      = Vec4(settings.transparency, settings.refractionStrength, settings.reflectivity, settings.roughness);
    ubo.normalParams     = Vec4(settings.normalScale, settings.waveSpeed, settings.normalStrength, 0.0f);
    ubo.waveParams       = Vec4(settings.waveAmplitude, settings.waveFrequency, settings.waveSpeed, 0.0f);
    ubo.foamParams       = Vec4(settings.foamEdgeDepth, settings.contactFoamStrength,
                                settings.enableFoam ? 1.0f : 0.0f, rainSplashIntensity);
    Vec3 skyTop = cycle.getSkyTopColor();
    Vec3 skyHor = cycle.getSkyHorizonColor();
    Vec3 sunDir = cycle.getSunDirection();
    Vec3 sunCol = cycle.getSunColor();
    ubo.skyTop       = Vec4(skyTop.x, skyTop.y, skyTop.z, 0.0f);
    ubo.skyHorizon   = Vec4(skyHor.x, skyHor.y, skyHor.z, 0.0f);
    float effectiveSunIntensity = cycle.getSunIntensity() * sunOcclusion * (1.0f - stormDarken * 0.7f);
    ubo.sunDirIntensity = Vec4(sunDir.x, sunDir.y, sunDir.z, effectiveSunIntensity);
    ubo.sunColor = Vec4(sunCol.x * sunOcclusion, sunCol.y * sunOcclusion, sunCol.z * sunOcclusion, effectiveSunIntensity);
    Vec3 amb = cycle.getAmbientColor();
    ubo.ambientColor = Vec4(amb.x, amb.y, amb.z, cycle.getAmbientIntensity());
    ubo.cameraPos    = Vec4(cameraPos, 1.0f);
    ubo.waterLevelAndPlanes = Vec4(settings.waterLevel, nearPlane, farPlane, time);
    ubo.screenSize   = Vec4(m_viewportW, m_viewportH, cameraPitch, 0.0f);
    for (int i = 0; i < 3; ++i) {
        Vec2 d = settings.waveDirections[i];
        float len = glm::length(d);
        if (len > 0.0001f) d /= len;
        ubo.waveDirAmp[i] = Vec4(d.x, d.y, settings.waveAmplitude, settings.waveFrequency);
        ubo.waveSpeedSteep[i] = Vec4(settings.waveSpeed, settings.waveSteepness[i], 0.0f, 0.0f);
    }
    int waterTex = settings.useProceduralTexture ? -1 : static_cast<int32_t>(m_waterTextureSlot);
    ubo.textureSlots = IVec4(static_cast<int32_t>(m_refractionSlot[frameIndex]),
                             static_cast<int32_t>(m_depthCopySlot[frameIndex]),
                             static_cast<int32_t>(m_foamMaskSlot),
                             waterTex);
    int skySlot = (m_skyboxSlot != 0xFFFFFFFFu) ? static_cast<int32_t>(m_skyboxSlot) : -1;
    ubo.skyboxSlot = IVec4(skySlot, 0, 0, 0);
    ubo.invViewProj = m_invViewProj;
    ubo.extraParams = Vec4(0.15f, 10.0f, settings.enableCaustics ? 1.0f : 0.0f,
                           static_cast<float>(settings.liquidKind));
    ubo.foamSurfaceParams = Vec4(settings.foamScale, settings.foamSpeed, settings.foamRoughness, settings.enableSurfaceFoam ? 1.0f : 0.0f);
    ubo.normalAdvanced = Vec4(static_cast<float>(settings.normalOctaves), 0.0f, 0.0f, 0.0f);
    ubo.causticsParams = Vec4(settings.causticsIntensity, settings.causticsDepthAttenuation, 0.0f, 0.0f);
    ubo.weatherParams = Vec4(sunOcclusion, moonOcclusion, stormDarken, rainIntensity);
    std::memcpy(m_uboMappeds[uboSlot], &ubo, sizeof(ubo));
}

// ============================================================================
// Render entry points
// ============================================================================
void WaterRenderer::prepareRefraction(VkCommandBuffer cmd,
                                      VkImage sceneColorImage, VkImage sceneDepthImage,
                                      uint32_t sceneWidth, uint32_t sceneHeight,
                                      float nearPlane, float farPlane,
                                      const Mat4& invViewProj,
                                      const WaterSettings& settings) {
    if (!settings.enabled || (m_indexCount == 0 && m_lavaIndexCount == 0)) return;

    createRefractionResources(sceneWidth, sceneHeight);

    // Upload dynamic contact-foam mask if dirty
    uploadFoamMask(cmd);

    uint32_t frameIndex = m_ctx->currentFrame() % MAX_FRAMES;
    copySceneToRefraction(cmd, sceneColorImage, sceneDepthImage, sceneWidth, sceneHeight, frameIndex);

    m_invViewProj = invViewProj;
}

void WaterRenderer::render(VkCommandBuffer cmd, const Mat4& viewProj, const Vec3& cameraPos,
                           const DayNightCycle& cycle, float time,
                           float nearPlane, float farPlane,
                           const WaterSettings& settings,
                           uint32_t abTestMask,
                           float cameraPitch,
                           float sunOcclusion,
                           float moonOcclusion,
                           float stormDarken,
                           float rainIntensity,
                           float rainSplashIntensity,
                           bool drawLavaSurface) {
    if (!settings.enabled) return;
    if (!drawLavaSurface && m_indexCount == 0) return;
    if (drawLavaSurface && m_lavaIndexCount == 0) return;

    // Banco proprio pra lava: os dois draws do mesmo frame nao podem
    // compartilhar UBO, senao o segundo memcpy vale pros dois.
    uint32_t frameIndex = m_ctx->currentFrame() % MAX_FRAMES;
    // Banco proprio pra lava: os dois draws do mesmo frame nao podem
    // compartilhar UBO, senao o segundo memcpy vale pros dois.
    uint32_t uboSlot = frameIndex + (drawLavaSurface ? MAX_FRAMES : 0);
    updateUBO(frameIndex, uboSlot, settings, nearPlane, farPlane, cycle, time, cameraPos, cameraPitch,
              sunOcclusion, moonOcclusion, stormDarken, rainIntensity, rainSplashIntensity);

    WaterPushConstants pc{};
    pc.viewProj = viewProj;
    // ERUPTION_LIQUID_DEBUG=1 pinta a superficie liquida de ciano chapado,
    // pra medir num print de cima quanto da caldeira o disco cobre. Ciano e
    // nao magenta porque magenta e' chroma-key de descarte no model.frag.
    static const int s_liquidDebug = []() {
        const char* e = std::getenv("ERUPTION_LIQUID_DEBUG");
        return (e && *e == '1') ? 1 : 0;
    }();
    // Campo PROPRIO, nao um bit dentro do abTestMask: o mask do menu ja' vem
    // com bits altos ligados e o diagnostico acendia sozinho.
    pc.debugMask = IVec4(static_cast<int32_t>(abTestMask), s_liquidDebug, 0, 0);

    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WaterPushConstants), &pc);

    VkDescriptorSet sets[2] = { m_bindless->set(), m_uboSets[uboSlot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 2, sets, 0, nullptr);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = m_viewportW;
    viewport.height = m_viewportH;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(m_viewportW), static_cast<uint32_t>(m_viewportH)};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDeviceSize offset = 0;
    VkBuffer bindVB = drawLavaSurface ? m_lavaVertexBuffer : m_vertexBuffer;
    VkBuffer bindIB = drawLavaSurface ? m_lavaIndexBuffer : m_indexBuffer;
    const uint32_t bindCount = drawLavaSurface ? m_lavaIndexCount : m_indexCount;
    if (bindVB == VK_NULL_HANDLE || bindCount == 0) return;
    vkCmdBindVertexBuffers(cmd, 0, 1, &bindVB, &offset);
    vkCmdBindIndexBuffer(cmd, bindIB, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdDrawIndexed(cmd, bindCount, 1, 0, 0, 0);
}

// ============================================================================
// Water noise texture
// ============================================================================
bool WaterRenderer::createTextureFromPixelsInternal(const uint8_t* pixels, int w, int h, int channels,
                                                     VkFormat format, VkImage& image, VmaAllocation& alloc,
                                                     VkImageView& view, VkSampler& sampler, uint32_t& slot) {
    VkDeviceSize imageSize = (VkDeviceSize)w * h * channels;
    if (!pixels || imageSize == 0) return false;

    VkBuffer staging; VmaAllocation stagingAlloc;
    m_ctx->createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* data; vmaMapMemory(m_ctx->allocator(), stagingAlloc, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->createImage(w, h, format,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY, image, alloc, 1);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        m_ctx->cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    });
    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &view);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &sampler);

    slot = m_bindless->allocateSlot();
    m_bindless->updateTexture(slot, view, sampler);
    m_bindless->flushUpdates();
    return true;
}

void WaterRenderer::setSkyTexture(VkImageView view, VkSampler sampler) {
    clearSkyTexture();
    if (!m_bindless) return;
    m_skyboxSlot = m_bindless->allocateSlot();
    m_bindless->updateTexture(m_skyboxSlot, view, sampler);
    m_bindless->flushUpdates();
}

void WaterRenderer::clearSkyTexture() {
    if (m_skyboxSlot != 0xFFFFFFFFu && m_bindless) {
        m_bindless->freeSlot(m_skyboxSlot);
        m_skyboxSlot = 0xFFFFFFFFu;
    }
}

void WaterRenderer::destroyWaterTextureResources() {
    if (m_waterTextureSlot != 0xFFFFFFFFu) { m_bindless->freeSlot(m_waterTextureSlot); m_waterTextureSlot = 0xFFFFFFFFu; }
    if (m_waterTextureSampler) { vkDestroySampler(m_ctx->device(), m_waterTextureSampler, nullptr); m_waterTextureSampler = VK_NULL_HANDLE; }
    if (m_waterTextureView) { vkDestroyImageView(m_ctx->device(), m_waterTextureView, nullptr); m_waterTextureView = VK_NULL_HANDLE; }
    if (m_waterTexture) { vmaDestroyImage(m_ctx->allocator(), m_waterTexture, m_waterTextureAlloc); m_waterTexture = VK_NULL_HANDLE; m_waterTextureAlloc = VK_NULL_HANDLE; }
}

// ============================================================================
// Underwater RTT resources (projective texture preparation)
// ============================================================================
void WaterRenderer::createUnderwaterResources(uint32_t width, uint32_t height) {
    if (m_underwaterImage[0] != VK_NULL_HANDLE && m_underwaterWidth == width && m_underwaterHeight == height)
        return;

    destroyUnderwaterResources();
    m_underwaterWidth = width;
    m_underwaterHeight = height;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_underwaterSampler);

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_underwaterImage[i], &m_underwaterAlloc[i], nullptr);
        viewInfo.image = m_underwaterImage[i];
        vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_underwaterView[i]);
        m_underwaterSlot[i] = m_bindless->allocateSlot();
        m_bindless->updateTexture(m_underwaterSlot[i], m_underwaterView[i], m_underwaterSampler);
    }
    m_bindless->flushUpdates();
}

void WaterRenderer::destroyUnderwaterResources() {
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        if (m_underwaterSlot[i] != 0) { m_bindless->freeSlot(m_underwaterSlot[i]); m_underwaterSlot[i] = 0; }
        if (m_underwaterView[i]) { vkDestroyImageView(m_ctx->device(), m_underwaterView[i], nullptr); m_underwaterView[i] = VK_NULL_HANDLE; }
        if (m_underwaterImage[i]) { vmaDestroyImage(m_ctx->allocator(), m_underwaterImage[i], m_underwaterAlloc[i]); m_underwaterImage[i] = VK_NULL_HANDLE; m_underwaterAlloc[i] = VK_NULL_HANDLE; }
    }
    if (m_underwaterSampler) { vkDestroySampler(m_ctx->device(), m_underwaterSampler, nullptr); m_underwaterSampler = VK_NULL_HANDLE; }
    m_underwaterWidth = 0;
    m_underwaterHeight = 0;
}

void WaterRenderer::generateBlueNoiseTexture(int size) {
    destroyWaterTextureResources();

    // Fast approximate blue noise via high-pass filtered white noise.
    // Much faster than void-and-cluster while still removing low-frequency patterns.
    std::vector<float> values(size * size);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (auto& v : values) v = dist(rng);

    // Simple 3x3 box blur (two passes for speed)
    auto blur = [&](const std::vector<float>& src) {
        std::vector<float> tmp(size * size);
        std::vector<float> dst(size * size);
        // Horizontal pass
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = (x + dx + size) % size;
                    sum += src[y * size + nx];
                }
                tmp[y * size + x] = sum / 3.0f;
            }
        }
        // Vertical pass
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = (y + dy + size) % size;
                    sum += tmp[ny * size + x];
                }
                dst[y * size + x] = sum / 3.0f;
            }
        }
        return dst;
    };

    auto blurred = blur(values);
    // High-pass: original - blurred (removes low frequencies)
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = values[i] - blurred[i] + 0.5f;
    }

    // Rank ordering -> final pixel values (ensures uniform histogram = good dithering)
    std::vector<std::pair<float, int>> sorted;
    sorted.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) sorted.emplace_back(values[i], (int)i);
    std::sort(sorted.begin(), sorted.end());

    std::vector<uint8_t> pixels(size * size);
    for (size_t i = 0; i < sorted.size(); ++i) {
        pixels[sorted[i].second] = static_cast<uint8_t>((float(i) / float(sorted.size() - 1)) * 255.0f);
    }

    bool ok = createTextureFromPixelsInternal(pixels.data(), size, size, 1,
                                               VK_FORMAT_R8_UNORM,
                                               m_waterTexture, m_waterTextureAlloc,
                                               m_waterTextureView, m_waterTextureSampler,
                                               m_waterTextureSlot);
    if (ok) {
        ERUPTION_LOG_INFO("Generated blue noise texture %dx%d", size, size);
    } else {
        ERUPTION_LOG_ERROR("Failed to create blue noise texture");
        m_waterTextureSlot = 0xFFFFFFFFu;
    }
}

void WaterRenderer::loadWaterTextureFromFile(const char* path) {
    destroyWaterTextureResources();

    int w, h, channels;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        ERUPTION_LOG_WARN("Failed to load water texture: %s", path);
        return;
    }
    bool ok = createTextureFromPixelsInternal(pixels, w, h, 4,
                                               VK_FORMAT_R8G8B8A8_UNORM,
                                               m_waterTexture, m_waterTextureAlloc,
                                               m_waterTextureView, m_waterTextureSampler,
                                               m_waterTextureSlot);
    stbi_image_free(pixels);
    if (ok) {
        ERUPTION_LOG_INFO("Loaded water texture: %s (%dx%d)", path, w, h);
    } else {
        ERUPTION_LOG_WARN("Failed to create GPU texture for: %s", path);
        m_waterTextureSlot = 0xFFFFFFFFu;
    }
}

// ============================================================================
// Pipeline
// ============================================================================
void WaterRenderer::destroyPipeline() {
    if (m_pipeline) {
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout) {
        vkDestroyPipelineLayout(m_ctx->device(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}

void WaterRenderer::reloadShaders() {
    if (!m_ctx) return;
    ERUPTION_LOG_INFO("Reloading water shaders...");
    vkDeviceWaitIdle(m_ctx->device());
    destroyPipeline();
    createPipeline();
    ERUPTION_LOG_INFO("Water shaders reloaded.");
}

void WaterRenderer::createPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/water/liquid.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/water/liquid.frag.spv");

    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(WaterVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(WaterVertex, position);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(WaterVertex, texCoord);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 3;
    dyn.pDynamicStates = dynStates;

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(WaterPushConstants);

    VkDescriptorSetLayout setLayouts[2] = { m_bindless->layout(), m_uboLayout };

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 2;
    layoutCI.pSetLayouts = setLayouts;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;

    VK_CHECK_VOID(vkCreatePipelineLayout(m_ctx->device(), &layoutCI, nullptr, &m_layout));

    VkPipelineRenderingCreateInfo renderingCI{};
    renderingCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCI.colorAttachmentCount = 1;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    renderingCI.pColorAttachmentFormats = &colorFormat;
    renderingCI.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.pNext = &renderingCI;
    pipelineCI.stageCount = 2;
    pipelineCI.pStages = stages;
    pipelineCI.pVertexInputState = &vi;
    pipelineCI.pInputAssemblyState = &ia;
    pipelineCI.pViewportState = &vp;
    pipelineCI.pRasterizationState = &rs;
    pipelineCI.pMultisampleState = &ms;
    pipelineCI.pDepthStencilState = &ds;
    pipelineCI.pColorBlendState = &cb;
    pipelineCI.pDynamicState = &dyn;
    pipelineCI.layout = m_layout;

    VK_CHECK_VOID(vkc::createGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipelineCI, nullptr, &m_pipeline));

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);
}

} // namespace eruption
