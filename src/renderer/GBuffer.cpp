#include "renderer/GBuffer.hpp"
#include "core/Logger.hpp"
#include "utils/Profiler.hpp"

namespace eruption {

bool GBuffer::init(VulkanContext* ctx, uint32_t width, uint32_t height) {
    m_ctx = ctx;
    m_width = width;
    m_height = height;

    if (!createAttachment(AlbedoFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT,
                          m_albedoImage, m_albedoAlloc, m_albedoView)) return false;

    if (!createAttachment(NormalFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT,
                          m_normalImage, m_normalAlloc, m_normalView)) return false;

    if (!createAttachment(PbrFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT,
                          m_pbrImage, m_pbrAlloc, m_pbrView)) return false;

    if (!createAttachment(MaterialFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT,
                          m_materialImage, m_materialAlloc, m_materialView)) return false;

    if (!createAttachment(EmissiveFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT,
                          m_emissiveImage, m_emissiveAlloc, m_emissiveView)) return false;


    if (s_velocityEnabled &&
        !createAttachment(VelocityFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT,
                          m_velocityImage, m_velocityAlloc, m_velocityView)) return false;

    if (!createAttachment(DepthFormat,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT,
                          m_depthImage, m_depthAlloc, m_depthView)) return false;

    ERUPTION_LOG_INFO("G-Buffer created: %dx%d (5 alvos, %d B/px)", width, height, 8+8+4+1+4);
    return true;
}

void GBuffer::shutdown() {
    destroyAttachment(m_albedoImage, m_albedoAlloc, m_albedoView);
    destroyAttachment(m_normalImage, m_normalAlloc, m_normalView);
    destroyAttachment(m_pbrImage, m_pbrAlloc, m_pbrView);
    destroyAttachment(m_materialImage, m_materialAlloc, m_materialView);
    destroyAttachment(m_emissiveImage, m_emissiveAlloc, m_emissiveView);
    destroyAttachment(m_velocityImage, m_velocityAlloc, m_velocityView);
    destroyAttachment(m_depthImage, m_depthAlloc, m_depthView);
}

void GBuffer::resize(uint32_t width, uint32_t height) {
    shutdown();
    init(m_ctx, width, height);
}

void GBuffer::beginPass(VkCommandBuffer cmd) {
    transitionToWrite(cmd);

    VkRenderingAttachmentInfo colorAttachments[6];

    colorAttachments[0] = {};
    colorAttachments[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[0].imageView = m_albedoView;
    colorAttachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    colorAttachments[1] = {};
    colorAttachments[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[1].imageView = m_normalView;
    colorAttachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[1].clearValue.color = {{0.5f, 0.5f, 1.0f, 0.5f}};

    colorAttachments[2] = {};
    colorAttachments[2].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[2].imageView = m_pbrView;
    colorAttachments[2].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[2].clearValue.color = {{0.0f, 0.5f, 0.0f, 1.0f}};

    colorAttachments[3] = {};
    colorAttachments[3].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[3].imageView = m_materialView;
    colorAttachments[3].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[3].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    colorAttachments[4] = {};
    colorAttachments[4].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[4].imageView = m_emissiveView;
    colorAttachments[4].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[4].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[4].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[4].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    colorAttachments[5] = {};
    colorAttachments[5].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachments[5].imageView = m_velocityView;
    colorAttachments[5].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[5].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachments[5].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachments[5].clearValue.color = {{kVelocitySentinel, kVelocitySentinel, 0.0f, 0.0f}};


    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = m_depthView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, {m_width, m_height}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = m_velocityView != VK_NULL_HANDLE ? 6u : 5u;
    renderingInfo.pColorAttachments = colorAttachments;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkc::cmdBeginRendering(cmd, &renderingInfo);
    ERUPTION_LOG_TRACE("GBuffer::beginPass() %ux%u", m_width, m_height);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void GBuffer::endPass(VkCommandBuffer cmd) {
    vkc::cmdEndRendering(cmd);
}

void GBuffer::transitionToRead(VkCommandBuffer cmd) {
    // Engine::render e DeferredLighting::render chamavam isto em sequencia sem
    // um write no meio - a segunda transicao declarava oldLayout=COLOR com a
    // imagem ja' em SHADER_READ (VUID-VkImageMemoryBarrier2-oldLayout-01197,
    // spam por frame). O estado e' rastreado e a repeticao vira no-op.
    if (m_inReadState) return;
    m_inReadState = true;
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(7);
    auto addColor = [&](VkImage image) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        // COMPUTE tambem: o CameraMotion (FSR) le' a velocidade num compute.
        b.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        barriers.push_back(b);
    };
    addColor(m_albedoImage);
    addColor(m_normalImage);
    addColor(m_pbrImage);
    addColor(m_materialImage);
    addColor(m_emissiveImage);
    if (m_velocityImage != VK_NULL_HANDLE) addColor(m_velocityImage);

    VkImageMemoryBarrier2 depthB{};
    depthB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    depthB.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depthB.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    depthB.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthB.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthB.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthB.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthB.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthB.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthB.image = m_depthImage;
    depthB.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    barriers.push_back(depthB);

    m_ctx->cmdImageBarriers(cmd, barriers);
}

void GBuffer::transitionToWrite(VkCommandBuffer cmd) {
    m_inReadState = false;
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(7);
    auto addColor = [&](VkImage image) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        barriers.push_back(b);
    };
    addColor(m_albedoImage);
    addColor(m_normalImage);
    addColor(m_pbrImage);
    addColor(m_materialImage);
    addColor(m_emissiveImage);
    if (m_velocityImage != VK_NULL_HANDLE) addColor(m_velocityImage);

    VkImageMemoryBarrier2 depthB{};
    depthB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    depthB.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    depthB.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depthB.srcAccessMask = 0;
    depthB.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthB.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthB.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthB.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthB.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthB.image = m_depthImage;
    depthB.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    barriers.push_back(depthB);

    m_ctx->cmdImageBarriers(cmd, barriers);
}

bool GBuffer::createAttachment(VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, VkImage& image,
                               VmaAllocation& alloc, VkImageView& view) {
    VkFormatProperties formatProps;
    vkGetPhysicalDeviceFormatProperties(m_ctx->physicalDevice(), format, &formatProps);

    VkFormatFeatureFlags required = 0;
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    }

    if ((formatProps.optimalTilingFeatures & required) != required) {
        ERUPTION_LOG_ERROR("GBuffer format %d does not support required optimal tiling features for usage %u", format, usage);
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_width;
    imageInfo.extent.height = m_height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkResult result = vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &image, &alloc, nullptr);
    if (result != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Failed to create GBuffer attachment");
        return false;
    }
    
    VmaAllocationInfo aInfo;
    vmaGetAllocationInfo(m_ctx->allocator(), alloc, &aInfo);
    PROFILE_VRAM_ALLOC(ProfilerCategory::GBuffer, aInfo.size);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    result = vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &view);
    if (result != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Failed to create GBuffer attachment view");
        return false;
    }
    return true;
}

void GBuffer::destroyAttachment(VkImage& image, VmaAllocation& alloc, VkImageView& view) {
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), image, alloc);
        image = VK_NULL_HANDLE;
        alloc = VK_NULL_HANDLE;
    }
}

} // namespace eruption
