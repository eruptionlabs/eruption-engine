#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"
#include <vector>

namespace eruption {

struct AtlasEntry {
    Vec2 uvMin;
    Vec2 uvMax;
    uint32_t x, y, w, h;
    uint32_t bindlessIndex;
};

class TextureAtlas {
public:
    void init(VulkanContext* ctx, uint32_t maxSize = 4096,
              VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
    void shutdown();

    AtlasEntry addTexture(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t channels);
    void finalize();

    VkImageView atlasView() const { return m_atlasView; }
    uint32_t atlasWidth() const { return m_atlasWidth; }
    uint32_t atlasHeight() const { return m_atlasHeight; }
    VkImage atlasImage() const { return m_atlasImage; }

private:
    VulkanContext* m_ctx = nullptr;

    std::vector<uint8_t> m_atlasPixels;
    uint32_t m_atlasWidth = 0;
    uint32_t m_atlasHeight = 0;
    uint32_t m_maxSize;
    VkFormat m_format;

    VkImage m_atlasImage = VK_NULL_HANDLE;
    VmaAllocation m_atlasAlloc = VK_NULL_HANDLE;
    VkImageView m_atlasView = VK_NULL_HANDLE;

    std::vector<AtlasEntry> m_entries;

    struct Shelf {
        uint32_t y;
        uint32_t height;
        uint32_t currentX;
    };
    std::vector<Shelf> m_shelves;

    bool m_finalized = false;

    AtlasEntry packTexture(uint32_t w, uint32_t h);
    void uploadToGpu();
};

} // namespace eruption
