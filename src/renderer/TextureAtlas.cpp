#include "renderer/TextureAtlas.hpp"
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include <cstring>
#include <algorithm>

namespace eruption {

void TextureAtlas::init(VulkanContext* ctx, uint32_t maxSize, VkFormat format) {
    m_ctx = ctx;
    m_maxSize = maxSize;
    m_format = format;
    m_atlasWidth = maxSize;
    m_atlasHeight = maxSize;
    m_atlasPixels.resize(maxSize * maxSize * 4, 0);
    m_shelves.reserve(64);
    m_entries.reserve(256);
    m_finalized = false;
}

void TextureAtlas::shutdown() {
    if (m_atlasView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_atlasView, nullptr);
        m_atlasView = VK_NULL_HANDLE;
    }
    if (m_atlasImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_atlasImage, m_atlasAlloc);
        m_atlasImage = VK_NULL_HANDLE;
    }
    m_atlasPixels.clear();
    m_shelves.clear();
    m_entries.clear();
}

AtlasEntry TextureAtlas::addTexture(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t channels) {
    if (m_finalized) {
        ERUPTION_LOG_WARN("TextureAtlas already finalized");
        return AtlasEntry{};
    }
    if (w > m_maxSize || h > m_maxSize) {
        ERUPTION_LOG_WARN("Texture too large for atlas: %ux%u", w, h);
        return AtlasEntry{};
    }

    AtlasEntry entry = packTexture(w, h);
    if (entry.w == 0) {
        ERUPTION_LOG_WARN("Failed to pack texture into atlas");
        return entry;
    }

    // Copy pixels into atlas
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t atlasIdx = ((entry.y + y) * m_atlasWidth + (entry.x + x)) * 4;
            uint32_t texIdx = (y * w + x) * channels;

            uint8_t r = pixels[texIdx + 0];
            uint8_t g = (channels > 1) ? pixels[texIdx + 1] : pixels[texIdx + 0];
            uint8_t b = (channels > 2) ? pixels[texIdx + 2] : pixels[texIdx + 0];
            uint8_t a = (channels > 3) ? pixels[texIdx + 3] : 255;

            ImageUtils::applyMagentaTransparencyToPixel(r, g, b, a);

            m_atlasPixels[atlasIdx + 0] = r;
            m_atlasPixels[atlasIdx + 1] = g;
            m_atlasPixels[atlasIdx + 2] = b;
            m_atlasPixels[atlasIdx + 3] = a;
        }
    }

    entry.bindlessIndex = static_cast<uint32_t>(m_entries.size());
    m_entries.push_back(entry);
    return entry;
}

AtlasEntry TextureAtlas::packTexture(uint32_t w, uint32_t h) {
    // Try to fit in existing shelf
    for (auto& shelf : m_shelves) {
        if (shelf.height >= h && shelf.currentX + w <= m_atlasWidth) {
            AtlasEntry entry;
            entry.x = shelf.currentX;
            entry.y = shelf.y;
            entry.w = w;
            entry.h = h;
            entry.uvMin = Vec2(static_cast<float>(entry.x) / m_atlasWidth,
                               static_cast<float>(entry.y) / m_atlasHeight);
            entry.uvMax = Vec2(static_cast<float>(entry.x + w) / m_atlasWidth,
                               static_cast<float>(entry.y + h) / m_atlasHeight);
            shelf.currentX += w;
            return entry;
        }
    }

    // Create new shelf
    uint32_t shelfY = m_shelves.empty() ? 0 : m_shelves.back().y + m_shelves.back().height;
    if (shelfY + h > m_atlasHeight) {
        return AtlasEntry{}; // Out of space
    }

    Shelf newShelf;
    newShelf.y = shelfY;
    newShelf.height = h;
    newShelf.currentX = w;
    m_shelves.push_back(newShelf);

    AtlasEntry entry;
    entry.x = 0;
    entry.y = shelfY;
    entry.w = w;
    entry.h = h;
    entry.uvMin = Vec2(0.0f, static_cast<float>(shelfY) / m_atlasHeight);
    entry.uvMax = Vec2(static_cast<float>(w) / m_atlasWidth,
                       static_cast<float>(shelfY + h) / m_atlasHeight);
    return entry;
}

void TextureAtlas::finalize() {
    if (m_finalized) return;
    m_finalized = true;

    // Trim atlas height
    if (!m_shelves.empty()) {
        m_atlasHeight = m_shelves.back().y + m_shelves.back().height;
    }

    uploadToGpu();
    ERUPTION_LOG_INFO("TextureAtlas finalized: %ux%u, %zu entries",
                    m_atlasWidth, m_atlasHeight, m_entries.size());
}

void TextureAtlas::uploadToGpu() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_format;
    imageInfo.extent = {m_atlasWidth, m_atlasHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_atlasImage, &m_atlasAlloc, nullptr);

    // Create staging buffer
    VkDeviceSize size = m_atlasWidth * m_atlasHeight * 4;
    VkBuffer staging;
    VmaAllocation stagingAlloc;
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    vmaCreateBuffer(m_ctx->allocator(), &bufInfo, &stagingAllocInfo, &staging, &stagingAlloc, nullptr);

    void* data;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &data);
    std::memcpy(data, m_atlasPixels.data(), static_cast<size_t>(size));
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_atlasImage;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_atlasWidth, m_atlasHeight, 1};
        vkCmdCopyBufferToImage(cmd, staging, m_atlasImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });

    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_atlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_atlasView);
}

} // namespace eruption
