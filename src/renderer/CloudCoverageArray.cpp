#include "renderer/CloudCoverageArray.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include <glm/glm.hpp>
#include <cstring>
#include <algorithm>

namespace eruption {

CloudCoverageArray::~CloudCoverageArray() {
    shutdown();
}

bool CloudCoverageArray::init(VulkanContext* ctx,
                              uint32_t layerCount,
                              uint32_t size,
                              const Vec3& worldMin,
                              const Vec3& worldMax,
                              uint32_t octaves,
                              float coverage,
                              uint32_t seed,
                              float cloudBottom,
                              float layerSpacing,
                              VkSamplerAddressMode addressMode) {
    shutdown();

    m_ctx = ctx;
    m_layerCount = glm::clamp(layerCount, MIN_LAYERS, MAX_LAYERS);
    m_size = glm::clamp(size, MIN_SIZE, MAX_SIZE);
    m_layerSpacing = layerSpacing;
    m_cloudBottom = cloudBottom;
    m_addressMode = addressMode;
    m_worldMin = worldMin;
    m_worldMax = worldMax;
    m_octaves = glm::clamp(octaves, 0u, MAX_OCTAVES);
    m_coverage = glm::clamp(coverage, 0.0f, 1.0f);
    m_seed = seed;
    m_windOffset = Vec2(0.0f);
    m_lastWindOffset = Vec2(0.0f);
    m_coverageDirty = true;
    m_paramsDirty = true;

    if (!m_ctx) {
        Logger::error("CloudCoverageArray::init called without VulkanContext");
        return false;
    }

    if (!createImage()) return false;
    if (!createSampler()) return false;
    if (!createAltitudeImage()) return false;
    if (!createAltitudeSampler()) return false;
    if (!createLocalCloudBuffer()) return false;
    if (!createComputePipeline()) {
        Logger::error("CloudCoverageArray: failed to create compute pipeline; continuing without GPU generation");
        return false;
    }

    // Initial generation is done synchronously via immediateSubmit because the
    // engine needs a valid texture before the first frame. Subsequent changes
    // are recorded into the frame command buffer and do not stall.
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        recordComputeGeneration(cmd);
    });

    return true;
}

void CloudCoverageArray::shutdown() {
    if (!m_ctx) return;

    destroyComputePipeline();
    destroyLocalCloudBuffer();
    destroyAltitudeSampler();
    destroyAltitudeImage();
    destroySampler();
    destroyImage();

    m_ctx = nullptr;
}

void CloudCoverageArray::recordComputeGeneration(VkCommandBuffer cmd) {
    if (!m_ctx || m_computePipeline == VK_NULL_HANDLE) return;

    // On the very first generation the images are in UNDEFINED layout.
    VkImageLayout oldCoverageLayout = m_firstGeneration ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout oldAltitudeLayout = m_firstGeneration ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkPipelineStageFlags srcStage = m_firstGeneration ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkAccessFlags srcAccess = m_firstGeneration ? 0 : VK_ACCESS_SHADER_READ_BIT;

    // Transition images to GENERAL so the compute shader can write.
    m_ctx->cmdImageBarrier(cmd, m_image,
                           oldCoverageLayout, VK_IMAGE_LAYOUT_GENERAL,
                           srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           srcAccess, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_ctx->cmdImageBarrier(cmd, m_altitudeImage,
                           oldAltitudeLayout, VK_IMAGE_LAYOUT_GENERAL,
                           srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           srcAccess, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    updateLocalCloudBuffer();
    dispatchCompute(cmd);

    // Transition back to shader-read for the fragment passes in the same frame.
    m_ctx->cmdImageBarrier(cmd, m_image,
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_ctx->cmdImageBarrier(cmd, m_altitudeImage,
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    m_coverageDirty = false;
    m_paramsDirty = false;
    m_firstGeneration = false;
}

void CloudCoverageArray::update(float dt, const Vec3& windDirection, float windSpeed) {
    updateWind(dt, windDirection, windSpeed);
    updateLocalClouds(dt, windDirection, windSpeed);
}

void CloudCoverageArray::updateWind(float dt, const Vec3& windDirection, float windSpeed, float uvFactor) {
    // Guard on initialization, NOT on enabled() (which is m_octaves > 0):
    // independent cloud layers are created with octaves=0 and still need wind.
    if (!m_ctx || m_image == VK_NULL_HANDLE) return;

    // Animate the wind offset; the shader applies it as a UV shift, so we no
    // longer regenerate the coverage texture when the wind changes.
    Vec2 windXZ = glm::normalize(Vec2(windDirection.x, windDirection.z));
    if (glm::length(windXZ) < 0.001f) windXZ = Vec2(1.0f, 0.0f);
    m_windOffset += windXZ * windSpeed * dt * uvFactor;
    // Wrap only for tiling (REPEAT) layers: there fract keeps the same texels
    // and bounds float precision. Independent no-repeat layers sample with
    // CLAMP_TO_BORDER, so wrapping a small negative offset to ~+1.0 would
    // throw the cloud outside the texture (invisible cloud AND shadow).
    if (m_addressMode == VK_SAMPLER_ADDRESS_MODE_REPEAT) {
        m_windOffset.x = glm::fract(m_windOffset.x);
        m_windOffset.y = glm::fract(m_windOffset.y);
    }
}

void CloudCoverageArray::addLocalCloud(const LocalCloud& cloud) {
    if (m_localClouds.size() < LocalCloud::MAX_COUNT) {
        m_localClouds.push_back(cloud);
        m_localCloudsDirty = true;
        m_coverageDirty = true;
    }
}

void CloudCoverageArray::clearLocalClouds() {
    if (!m_localClouds.empty()) {
        m_localClouds.clear();
        m_localCloudsDirty = true;
        m_coverageDirty = true;
    }
}

void CloudCoverageArray::updateLocalClouds(float dt, const Vec3& windDirection, float windSpeed) {
    if (m_localClouds.empty()) return;
    Vec2 windXZ = glm::normalize(Vec2(windDirection.x, windDirection.z));
    if (glm::length(windXZ) < 0.001f) windXZ = Vec2(1.0f, 0.0f);
    Vec2 offset = windXZ * windSpeed * dt * 10.0f;
    if (glm::dot(offset, offset) > 1e-12f) {
        for (auto& c : m_localClouds) c.center += offset;
        // Clouds moved: regenerate the coverage texture so they actually drift
        // (and eventually leave the map) instead of staying frozen.
        m_coverageDirty = true;
    }
}

void CloudCoverageArray::setLayerCount(uint32_t layers) {
    layers = glm::clamp(layers, MIN_LAYERS, MAX_LAYERS);
    if (m_layerCount == layers) return;
    VulkanContext* ctx = m_ctx;
    m_layerCount = layers;
    shutdown();
    init(ctx, m_layerCount, m_size, m_worldMin, m_worldMax, m_octaves, m_coverage, m_seed,
         m_cloudBottom, m_layerSpacing, m_addressMode);
}

void CloudCoverageArray::setSize(uint32_t size) {
    size = glm::clamp(size, MIN_SIZE, MAX_SIZE);
    if (m_size == size) return;
    VulkanContext* ctx = m_ctx;
    m_size = size;
    shutdown();
    init(ctx, m_layerCount, m_size, m_worldMin, m_worldMax, m_octaves, m_coverage, m_seed,
         m_cloudBottom, m_layerSpacing, m_addressMode);
}

void CloudCoverageArray::setOctaves(uint32_t octaves) {
    octaves = glm::clamp(octaves, 0u, MAX_OCTAVES);
    if (m_octaves == octaves) return;
    CloudCoverageNoise::setNoiseParams(octaves, m_coverage, m_seed);
    m_coverageDirty = true;
}

void CloudCoverageArray::setCoverage(float coverage) {
    coverage = glm::clamp(coverage, 0.0f, 1.0f);
    if (m_coverage == coverage) return;
    CloudCoverageNoise::setNoiseParams(m_octaves, coverage, m_seed);
    m_coverageDirty = true;
}

void CloudCoverageArray::setSeed(uint32_t seed) {
    if (m_seed == seed) return;
    CloudCoverageNoise::setNoiseParams(m_octaves, m_coverage, seed);
    m_coverageDirty = true;
}

void CloudCoverageArray::setWorldBounds(const Vec3& worldMin, const Vec3& worldMax) {
    if (glm::all(glm::equal(m_worldMin, worldMin)) &&
        glm::all(glm::equal(m_worldMax, worldMax))) {
        return;
    }
    CloudCoverageNoise::setWorldBounds(worldMin, worldMax);
    m_coverageDirty = true;
    m_paramsDirty = true;
}

void CloudCoverageArray::setLayerSpacing(float spacing) {
    if (m_layerSpacing == spacing) return;
    m_layerSpacing = spacing;
    m_paramsDirty = true;
}

void CloudCoverageArray::setCloudBottom(float bottom) {
    if (m_cloudBottom == bottom) return;
    m_cloudBottom = bottom;
    m_paramsDirty = true;
}

bool CloudCoverageArray::createImage() {
    if (!m_ctx) return false;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_size;
    imageInfo.extent.height = m_size;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = m_layerCount;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkResult result = vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_image, &m_alloc, nullptr);
    if (result != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, m_layerCount};
    if (vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_imageView) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create image view");
        return false;
    }

    return true;
}

void CloudCoverageArray::destroyImage() {
    if (!m_ctx) return;
    if (m_imageView) {
        vkDestroyImageView(m_ctx->device(), m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    if (m_image) {
        vmaDestroyImage(m_ctx->allocator(), m_image, m_alloc);
        m_image = VK_NULL_HANDLE;
        m_alloc = VK_NULL_HANDLE;
    }
}

bool CloudCoverageArray::createSampler() {
    if (!m_ctx) return false;

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = m_addressMode;
    info.addressModeV = m_addressMode;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    // CLAMP_TO_BORDER layers read 0 coverage outside the texture (no cloud).
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;

    if (vkCreateSampler(m_ctx->device(), &info, nullptr, &m_sampler) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create sampler");
        return false;
    }
    return true;
}

void CloudCoverageArray::destroySampler() {
    if (!m_ctx) return;
    if (m_sampler) {
        vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
}

bool CloudCoverageArray::createAltitudeImage() {
    if (!m_ctx) return false;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_size;
    imageInfo.extent.height = m_size;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_altitudeImage, &m_altitudeAlloc, nullptr) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create altitude image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_altitudeImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_altitudeView) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create altitude view");
        return false;
    }
    return true;
}

void CloudCoverageArray::destroyAltitudeImage() {
    if (!m_ctx) return;
    if (m_altitudeView) {
        vkDestroyImageView(m_ctx->device(), m_altitudeView, nullptr);
        m_altitudeView = VK_NULL_HANDLE;
    }
    if (m_altitudeImage) {
        vmaDestroyImage(m_ctx->allocator(), m_altitudeImage, m_altitudeAlloc);
        m_altitudeImage = VK_NULL_HANDLE;
        m_altitudeAlloc = VK_NULL_HANDLE;
    }
}

bool CloudCoverageArray::createAltitudeSampler() {
    if (!m_ctx) return false;

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = m_addressMode;
    info.addressModeV = m_addressMode;
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;

    if (vkCreateSampler(m_ctx->device(), &info, nullptr, &m_altitudeSampler) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create altitude sampler");
        return false;
    }
    return true;
}

void CloudCoverageArray::destroyAltitudeSampler() {
    if (!m_ctx) return;
    if (m_altitudeSampler) {
        vkDestroySampler(m_ctx->device(), m_altitudeSampler, nullptr);
        m_altitudeSampler = VK_NULL_HANDLE;
    }
}

bool CloudCoverageArray::createLocalCloudBuffer() {
    if (!m_ctx) return false;
    VkDeviceSize size = LocalCloud::MAX_COUNT * sizeof(LocalCloud);
    if (!m_ctx->createBuffer(size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY,
                             m_localCloudBuffer, m_localCloudAlloc)) {
        Logger::error("CloudCoverageArray: failed to create local cloud buffer");
        return false;
    }
    return true;
}

void CloudCoverageArray::destroyLocalCloudBuffer() {
    if (!m_ctx) return;
    if (m_localCloudBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_localCloudBuffer, m_localCloudAlloc);
        m_localCloudBuffer = VK_NULL_HANDLE;
        m_localCloudAlloc = VK_NULL_HANDLE;
    }
}

void CloudCoverageArray::updateLocalCloudBuffer() {
    if (!m_ctx || !m_localCloudsDirty) return;
    m_localCloudsDirty = false;

    LocalCloud buffer[LocalCloud::MAX_COUNT] = {};
    uint32_t count = static_cast<uint32_t>(glm::min(m_localClouds.size(), size_t(LocalCloud::MAX_COUNT)));
    for (uint32_t i = 0; i < count; ++i) buffer[i] = m_localClouds[i];

    VkDeviceSize size = LocalCloud::MAX_COUNT * sizeof(LocalCloud);
    VkBuffer staging;
    VmaAllocation stagingAlloc;
    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, staging, stagingAlloc)) return;

    void* data;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &data);
    memcpy(data, buffer, size);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging, m_localCloudBuffer, 1, &copy);
    });

    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
}

bool CloudCoverageArray::createComputePipeline() {
    if (!m_ctx) return false;

    auto code = ShaderCompiler::loadSPIRV("clouds/coverage_gen.comp.spv");
    if (code.empty()) {
        Logger::error("CloudCoverageArray: failed to load compute shader");
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = code.size() * sizeof(uint32_t);
    smInfo.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &module) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create compute shader module");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_computeDescLayout) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create compute descriptor layout");
        vkDestroyShaderModule(m_ctx->device(), module, nullptr);
        return false;
    }

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = 128;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_computeDescLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &plInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create compute pipeline layout");
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_computeDescLayout, nullptr);
        vkDestroyShaderModule(m_ctx->device(), module, nullptr);
        m_computeDescLayout = VK_NULL_HANDLE;
        return false;
    }

    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeInfo.stage.module = module;
    pipeInfo.stage.pName = "main";
    pipeInfo.layout = m_computePipelineLayout;

    VkResult result = vkCreateComputePipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &m_computePipeline);

    vkDestroyShaderModule(m_ctx->device(), module, nullptr);

    if (result != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create compute pipeline");
        vkDestroyPipelineLayout(m_ctx->device(), m_computePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_computeDescLayout, nullptr);
        m_computePipelineLayout = VK_NULL_HANDLE;
        m_computeDescLayout = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_computeDescPool) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to create compute descriptor pool");
        destroyComputePipeline();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_computeDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_computeDescLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_computeDescSet) != VK_SUCCESS) {
        Logger::error("CloudCoverageArray: failed to allocate compute descriptor set");
        destroyComputePipeline();
        return false;
    }

    VkDescriptorImageInfo coverageInfo{};
    coverageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    coverageInfo.imageView = m_imageView;

    VkDescriptorImageInfo altitudeInfo{};
    altitudeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    altitudeInfo.imageView = m_altitudeView;

    VkDescriptorBufferInfo localCloudInfo{};
    localCloudInfo.buffer = m_localCloudBuffer;
    localCloudInfo.offset = 0;
    localCloudInfo.range = LocalCloud::MAX_COUNT * sizeof(LocalCloud);

    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_computeDescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &coverageInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_computeDescSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &altitudeInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_computeDescSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &localCloudInfo;

    vkUpdateDescriptorSets(m_ctx->device(), 3, writes, 0, nullptr);

    return true;
}

void CloudCoverageArray::destroyComputePipeline() {
    if (!m_ctx) return;
    if (m_computeDescPool) {
        vkDestroyDescriptorPool(m_ctx->device(), m_computeDescPool, nullptr);
        m_computeDescPool = VK_NULL_HANDLE;
    }
    if (m_computePipeline) {
        vkDestroyPipeline(m_ctx->device(), m_computePipeline, nullptr);
        m_computePipeline = VK_NULL_HANDLE;
    }
    if (m_computePipelineLayout) {
        vkDestroyPipelineLayout(m_ctx->device(), m_computePipelineLayout, nullptr);
        m_computePipelineLayout = VK_NULL_HANDLE;
    }
    if (m_computeDescLayout) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_computeDescLayout, nullptr);
        m_computeDescLayout = VK_NULL_HANDLE;
    }
    m_computeDescSet = VK_NULL_HANDLE;
}

void CloudCoverageArray::dispatchCompute(VkCommandBuffer cmd) {
    struct Push {
        Vec2 worldMin;
        Vec2 worldMax;
        Vec2 windOffset;
        float coverage;
        int octaves;
        uint32_t seed;
        float cloudBottom;
        float layerSpacing;
        int layerCount;
        int imageSize;
        int localCloudCount;
    } pc;
    pc.worldMin = Vec2(m_worldMin.x, m_worldMin.z);
    pc.worldMax = Vec2(m_worldMax.x, m_worldMax.z);
    pc.windOffset = m_windOffset;
    pc.coverage = m_coverage;
    pc.octaves = static_cast<int>(m_octaves);
    pc.seed = m_seed;
    pc.cloudBottom = m_cloudBottom;
    pc.layerSpacing = m_layerSpacing;
    pc.layerCount = static_cast<int>(m_layerCount);
    pc.imageSize = static_cast<int>(m_size);
    pc.localCloudCount = static_cast<int>(glm::min(m_localClouds.size(), size_t(LocalCloud::MAX_COUNT)));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout, 0, 1, &m_computeDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t groups = (m_size + 15) / 16;
    vkCmdDispatch(cmd, groups, groups, m_layerCount);
}

float CloudCoverageArray::layerProfile(float t) {
    float x = 1.0f - std::abs(t - 0.5f) * 2.0f;
    return glm::clamp(x * x, 0.0f, 1.0f);
}

} // namespace eruption
