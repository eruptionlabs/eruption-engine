#include "renderer/VisionMap.hpp"

#include "core/Logger.hpp"
#include "renderer/VulkanContext.hpp"

#include <cstring>

namespace eruption {

static_assert(VulkanContext::MAX_FRAMES_IN_FLIGHT <= 3, "um staging da visao por quadro em voo");

bool VisionMap::init(VulkanContext* ctx, uint32_t texels, bool alphaMask) {
    m_ctx = ctx;
    m_alphaMask = alphaMask;
    return create(texels);
}

void VisionMap::shutdown() {
    destroy(false);
    m_ctx = nullptr;
}

bool VisionMap::ensureSize(uint32_t texels) {
    if (texels == m_side && m_image) return true;
    destroy(true);
    return create(texels);
}

bool VisionMap::create(uint32_t texels) {
    if (!m_ctx) return false;
    if (!m_ctx->createImage(texels, texels, VK_FORMAT_R8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_image, m_alloc)) {
        ERUPTION_LOG_ERROR("VisionMap: falha ao criar a imagem %ux%u", texels, texels);
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (m_alphaMask)
        viewInfo.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                               VK_COMPONENT_SWIZZLE_R};
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_view);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(texels) * texels;
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (!m_ctx->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
                                 m_staging[f], m_stagingAlloc[f]) ||
            vmaMapMemory(m_ctx->allocator(), m_stagingAlloc[f], &m_mapped[f]) != VK_SUCCESS) {
            ERUPTION_LOG_ERROR("VisionMap: falha ao criar o staging");
            return false;
        }
    }
    m_side = texels;

    // Comeca em zero (sem visao) e ja' no layout de leitura: a composicao
    // amostra a imagem mesmo antes do primeiro upload.
    std::memset(m_mapped[0], 0, static_cast<size_t>(bytes));
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) { upload(cmd, nullptr, texels); });
    return true;
}

void VisionMap::destroy(bool deferred) {
    if (!m_ctx) return;
    VmaAllocator allocator = m_ctx->allocator();
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (!m_staging[f]) continue;
        if (m_mapped[f]) vmaUnmapMemory(allocator, m_stagingAlloc[f]);
        VkBuffer b = m_staging[f];
        VmaAllocation a = m_stagingAlloc[f];
        if (deferred) m_ctx->deferFrameCleanup([allocator, b, a] { vmaDestroyBuffer(allocator, b, a); });
        else vmaDestroyBuffer(allocator, b, a);
        m_staging[f] = VK_NULL_HANDLE;
        m_stagingAlloc[f] = VK_NULL_HANDLE;
        m_mapped[f] = nullptr;
    }
    if (m_image) {
        VkDevice device = m_ctx->device();
        VkImage img = m_image;
        VkImageView view = m_view;
        VmaAllocation a = m_alloc;
        auto release = [device, allocator, img, view, a] {
            vkDestroyImageView(device, view, nullptr);
            vmaDestroyImage(allocator, img, a);
        };
        if (deferred) m_ctx->deferFrameCleanup(release);
        else release();
        m_image = VK_NULL_HANDLE;
        m_view = VK_NULL_HANDLE;
        m_alloc = VK_NULL_HANDLE;
    }
    m_side = 0;
}

void VisionMap::upload(VkCommandBuffer cmd, const uint8_t* texels, uint32_t side) {
    if (!m_image || side != m_side) return;
    const uint32_t frame = texels ? m_ctx->currentFrame() % kFrames : 0;
    if (texels) std::memcpy(m_mapped[frame], texels, static_cast<size_t>(side) * side);
    vmaFlushAllocation(m_ctx->allocator(), m_stagingAlloc[frame], 0, VK_WHOLE_SIZE);

    // Sem conteudo a manter: UNDEFINED -> TRANSFER_DST descarta o anterior.
    m_ctx->cmdImageBarrier(cmd, m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {side, side, 1};
    vkCmdCopyBufferToImage(cmd, m_staging[frame], m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    m_ctx->cmdImageBarrier(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

} // namespace eruption
