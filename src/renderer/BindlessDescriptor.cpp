#include "renderer/BindlessDescriptor.hpp"
#include "core/Logger.hpp"
#include <cstring>
#include <algorithm>

namespace eruption {

bool BindlessDescriptor::init(VulkanContext* ctx) {
    m_ctx = ctx;
    if (!createLayout()) return false;
    if (!createPool()) return false;
    if (!allocateSet()) return false;

    // Create a default 2x2 neutral gray for slot 0 (avoids magenta artifacts)
    uint8_t checkerboard[] = {
        128, 128, 128, 255,   128, 128, 128, 255,
        128, 128, 128, 255,   128, 128, 128, 255
    };
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {2, 2, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_defaultImage, &m_defaultAlloc, nullptr);

    VkBuffer staging;
    VmaAllocation stagingAlloc;
    m_ctx->createBuffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, checkerboard, 16);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, m_defaultImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            0, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {2, 2, 1};
        vkCmdCopyBufferToImage(cmd, staging, m_defaultImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        m_ctx->cmdImageBarrier(cmd, m_defaultImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    });

    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_defaultImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_defaultView);

    VkSamplerCreateInfo samplerInfo{}; samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_defaultSampler);

    updateTexture(0, m_defaultView, m_defaultSampler);
    flushUpdates();

    // Pre-allocate free list backwards for better cache locality
    m_freeSlots.reserve(MAX_BINDLESS_TEXTURES);
    for (uint32_t i = MAX_BINDLESS_TEXTURES - 1; i >= 1; --i) {
        m_freeSlots.push_back(i);
    }

    ERUPTION_LOG_INFO("Bindless descriptor system initialized (%d slots, default at slot 0)", MAX_BINDLESS_TEXTURES);
    return true;
}

void BindlessDescriptor::shutdown() {
    if (m_defaultView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_defaultView, nullptr);
        m_defaultView = VK_NULL_HANDLE;
    }
    if (m_defaultSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_defaultSampler, nullptr);
        m_defaultSampler = VK_NULL_HANDLE;
    }
    if (m_defaultImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_defaultImage, m_defaultAlloc);
        m_defaultImage = VK_NULL_HANDLE;
        m_defaultAlloc = VK_NULL_HANDLE;
    }

    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    m_set = VK_NULL_HANDLE;
    m_pendingUpdates.clear();
    m_freeSlots.clear();
}

uint32_t BindlessDescriptor::allocateSlot() {
    if (!m_freeSlots.empty()) {
        uint32_t slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        ++m_allocated; m_highWater = std::max(m_highWater, m_allocated);
        return slot;
    }
    if (m_nextSlot < MAX_BINDLESS_TEXTURES) {
        ++m_allocated; m_highWater = std::max(m_highWater, m_allocated);
        return m_nextSlot++;
    }
    ERUPTION_LOG_ERROR("Bindless texture array full! (max=%d)", MAX_BINDLESS_TEXTURES);
    return 0; // slot 0 = default/null
}

void BindlessDescriptor::freeSlot(uint32_t slot) {
    if (slot == 0 || slot >= MAX_BINDLESS_TEXTURES) return;
    if (m_allocated > 0) --m_allocated;
    m_freeSlots.push_back(slot);
    // Reset slot to default texture so shader doesn't access destroyed image
    m_pendingUpdates.push_back({slot, m_defaultView, m_defaultSampler});
}

void BindlessDescriptor::updateTexture(uint32_t slot, VkImageView view, VkSampler sampler) {
    if (slot >= MAX_BINDLESS_TEXTURES) return;
    m_pendingUpdates.push_back({slot, view, sampler});
}

void BindlessDescriptor::flushUpdates() {
    if (m_pendingUpdates.empty()) return;

    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(m_pendingUpdates.size());
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(m_pendingUpdates.size());

    for (const auto& upd : m_pendingUpdates) {
        imageInfos.push_back({upd.sampler, upd.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    for (size_t i = 0; i < m_pendingUpdates.size(); ++i) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = 0;
        write.dstArrayElement = m_pendingUpdates[i].slot;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfos[i];
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(m_ctx->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    m_pendingUpdates.clear();
}

uint32_t BindlessDescriptor::allocateSlotSafe() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return allocateSlot();
}

void BindlessDescriptor::freeSlotSafe(uint32_t slot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    freeSlot(slot);
}

void BindlessDescriptor::updateTextureSafe(uint32_t slot, VkImageView view, VkSampler sampler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    updateTexture(slot, view, sampler);
}

void BindlessDescriptor::flushUpdatesSafe() {
    std::lock_guard<std::mutex> lock(m_mutex);
    flushUpdates();
}

bool BindlessDescriptor::createLayout() {
    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = MAX_BINDLESS_TEXTURES;
    textureBinding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    textureBinding.pImmutableSamplers = nullptr;

    VkDescriptorBindingFlags bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 1;
    flagsInfo.pBindingFlags = &bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &textureBinding;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.pNext = &flagsInfo;

    VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_layout));
    return true;
}

bool BindlessDescriptor::createPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_BINDLESS_TEXTURES;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    VK_CHECK(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_pool));
    return true;
}

bool BindlessDescriptor::allocateSet() {
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
    variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    uint32_t maxCount = MAX_BINDLESS_TEXTURES;
    variableInfo.descriptorSetCount = 1;
    variableInfo.pDescriptorCounts = &maxCount;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_layout;
    allocInfo.pNext = &variableInfo;

    VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_set));
    return true;
}

} // namespace eruption
