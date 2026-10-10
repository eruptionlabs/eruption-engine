#pragma once

// Texturas de chão e de modelo na GPU (imagem, view, slot bindless e os
// mapas PBR que vão junto), como o cache de assets do Engine as guarda.
#include "renderer/VulkanContext.hpp"

#include <cstdint>

namespace eruption {

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
    bool isExternal = false; // true: quem é dono da vida é outro (cache de assets)
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

} // namespace eruption
