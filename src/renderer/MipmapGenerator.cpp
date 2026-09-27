#include "renderer/MipmapGenerator.hpp"
#include "renderer/VulkanContext.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

#include <cmath>
#include <algorithm>

namespace eruption {

uint32_t MipmapGenerator::calculateMipLevels(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return 1;
    uint32_t maxDim = std::max(width, height);
    return static_cast<uint32_t>(std::floor(std::log2(maxDim))) + 1;
}

void MipmapGenerator::generateMipmapsBlit(VkCommandBuffer cmd,
                                            VkImage image,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t mipLevels,
                                            VkImageAspectFlags aspect) {
    if (mipLevels <= 1) return;

    // Barrier template
    auto barrier = [&](uint32_t baseMip, uint32_t levelCount,
                       VkImageLayout oldLayout, VkImageLayout newLayout,
                       VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.baseMipLevel = baseMip;
        b.subresourceRange.levelCount = levelCount;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;

        VkDependencyInfo depInfo{};
        depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    };

    int32_t mipWidth = static_cast<int32_t>(width);
    int32_t mipHeight = static_cast<int32_t>(height);

    for (uint32_t i = 1; i < mipLevels; i++) {
        // Transicionar nível anterior (i-1) de DST → SRC
        barrier(i - 1, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        int32_t dstWidth = std::max(1, mipWidth / 2);
        int32_t dstHeight = std::max(1, mipHeight / 2);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = aspect;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {dstWidth, dstHeight, 1};
        blit.dstSubresource.aspectMask = aspect;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(cmd,
                       image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        // Transicionar nível anterior (i-1) para SHADER_READ_ONLY_OPTIMAL
        // Não precisamos mais dele como SRC (a não ser que seja o último, mas deixamos para o final)
        barrier(i - 1, 1,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        mipWidth = dstWidth;
        mipHeight = dstHeight;
    }

    // O último nível ainda está em TRANSFER_DST_OPTIMAL, precisamos transicioná-lo
    barrier(mipLevels - 1, 1,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

bool MipmapGenerator::canApplyMode(MipGenerationMode mode, bool hasPBR) {
    switch (mode) {
        case MipGenerationMode::BLIT_LINEAR:
            return true;
        case MipGenerationMode::DIRECTXTEX_OFFLINE:
            return true;
        case MipGenerationMode::TOKSVIG_PBR:
            return hasPBR;
        default:
            return false;
    }
}

// Static members definitions
VkDescriptorSetLayout MipmapGenerator::s_descLayout = VK_NULL_HANDLE;
VkPipelineLayout MipmapGenerator::s_pipelineLayout = VK_NULL_HANDLE;
VkPipeline MipmapGenerator::s_pipeline = VK_NULL_HANDLE;
VkDescriptorPool MipmapGenerator::s_descPool = VK_NULL_HANDLE;
VkSampler MipmapGenerator::s_sampler = VK_NULL_HANDLE;
bool MipmapGenerator::s_computeInitialized = false;

bool MipmapGenerator::initCompute(VulkanContext* ctx) {
    if (s_computeInitialized) return true;
    
    // 1. Create sampler (linear, clamp to edge)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    VK_CHECK(vkCreateSampler(ctx->device(), &samplerInfo, nullptr, &s_sampler));

    // 2. Descriptor Set Layout bindings for 1 source + 4 storage destination mips
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = nullptr;

    for (int i = 1; i <= 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = nullptr;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx->device(), &layoutInfo, nullptr, &s_descLayout));

    // 3. Push Constant Range
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &s_descLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx->device(), &pipelineLayoutInfo, nullptr, &s_pipelineLayout));

    // 4. Load compute shader
    auto compCode = ShaderCompiler::loadSPIRV("shaders/compute/mipmap_gamma_alpha.comp.spv");
    if (compCode.empty()) {
        Logger::error("Failed to load shaders/compute/mipmap_gamma_alpha.comp.spv");
        return false;
    }

    VkShaderModule compModule = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = compCode.size() * sizeof(uint32_t);
    smInfo.pCode = compCode.data();
    VK_CHECK(vkCreateShaderModule(ctx->device(), &smInfo, nullptr, &compModule));

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = compModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = s_pipelineLayout;
    VK_CHECK(vkCreateComputePipelines(ctx->device(), ctx->pipelineCache(), 1, &pipelineInfo, nullptr, &s_pipeline));

    vkDestroyShaderModule(ctx->device(), compModule, nullptr);

    s_computeInitialized = true;
    return true;
}

void MipmapGenerator::shutdownCompute(VulkanContext* ctx) {
    if (!s_computeInitialized) return;
    if (s_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(ctx->device(), s_sampler, nullptr);
        s_sampler = VK_NULL_HANDLE;
    }
    if (s_descLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(ctx->device(), s_descLayout, nullptr);
        s_descLayout = VK_NULL_HANDLE;
    }
    if (s_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx->device(), s_pipelineLayout, nullptr);
        s_pipelineLayout = VK_NULL_HANDLE;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx->device(), s_pipeline, nullptr);
        s_pipeline = VK_NULL_HANDLE;
    }
    s_computeInitialized = false;
}

void MipmapGenerator::generateMipmapsCompute(VkCommandBuffer cmd,
                                             VulkanContext* ctx,
                                             VkImage image,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t mipLevels,
                                             bool isSRGB,
                                             uint32_t alphaMode) {
    if (mipLevels <= 1) return;
    if (!s_computeInitialized) {
        if (!initCompute(ctx)) return;
    }

    // Allocate transient descriptor pool. Each pass downsamples up to 4 levels.
    uint32_t passes = (mipLevels + 2) / 4; // ceiling( (mipLevels-1) / 4 )
    if (passes == 0) passes = 1;

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = passes;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = passes * 4;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = passes;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    VkDescriptorPool localPool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(ctx->device(), &poolInfo, nullptr, &localPool) != VK_SUCCESS) {
        Logger::error("MipmapGenerator: failed to create local descriptor pool");
        return;
    }

    // Transition level 0 to SHADER_READ_ONLY_OPTIMAL as source
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Create Image Views for all levels
    std::vector<VkImageView> views(mipLevels);
    for (uint32_t i = 0; i < mipLevels; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = i;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(ctx->device(), &viewInfo, nullptr, &views[i]) != VK_SUCCESS) {
            Logger::error("MipmapGenerator: failed to create image view for level %u", i);
            for (uint32_t j = 0; j < i; j++) vkDestroyImageView(ctx->device(), views[j], nullptr);
            vkDestroyDescriptorPool(ctx->device(), localPool, nullptr);
            return;
        }
    }

    // Loop through mip levels in blocks of 4
    for (uint32_t srcLevel = 0; srcLevel < mipLevels - 1; srcLevel += 4) {
        uint32_t levelsInPass = std::min(4u, mipLevels - 1 - srcLevel);

        uint32_t dstLevel1 = srcLevel + 1;
        uint32_t dstLevel2 = (levelsInPass >= 2) ? (srcLevel + 2) : dstLevel1;
        uint32_t dstLevel3 = (levelsInPass >= 3) ? (srcLevel + 3) : dstLevel1;
        uint32_t dstLevel4 = (levelsInPass >= 4) ? (srcLevel + 4) : dstLevel1;

        // Transition destination levels from UNDEFINED to GENERAL
        for (uint32_t j = 0; j < levelsInPass; j++) {
            barrier.subresourceRange.baseMipLevel = srcLevel + 1 + j;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocInfo.descriptorPool = localPool;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &s_descLayout;
        
        VkDescriptorSet descSet;
        if (vkAllocateDescriptorSets(ctx->device(), &setAllocInfo, &descSet) != VK_SUCCESS) {
            Logger::error("MipmapGenerator: failed to allocate descriptor set");
            break;
        }

        // Update descriptors
        VkDescriptorImageInfo srcDescInfo{};
        srcDescInfo.imageView = views[srcLevel];
        srcDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        srcDescInfo.sampler = s_sampler;

        VkDescriptorImageInfo dstDescInfo1{VK_NULL_HANDLE, views[dstLevel1], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dstDescInfo2{VK_NULL_HANDLE, views[dstLevel2], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dstDescInfo3{VK_NULL_HANDLE, views[dstLevel3], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dstDescInfo4{VK_NULL_HANDLE, views[dstLevel4], VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &srcDescInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dstDescInfo1;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo = &dstDescInfo2;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = descSet;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo = &dstDescInfo3;

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = descSet;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[4].pImageInfo = &dstDescInfo4;

        vkUpdateDescriptorSets(ctx->device(), 5, writes, 0, nullptr);

        // Bind and dispatch
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipelineLayout, 0, 1, &descSet, 0, nullptr);

        uint32_t srcWidthLevel = std::max(1u, width >> srcLevel);
        uint32_t srcHeightLevel = std::max(1u, height >> srcLevel);
        uint32_t dstWidthLevel1 = std::max(1u, srcWidthLevel >> 1);
        uint32_t dstHeightLevel1 = std::max(1u, srcHeightLevel >> 1);

        PushConstants pc{};
        pc.srcWidth = srcWidthLevel;
        pc.srcHeight = srcHeightLevel;
        pc.numLevels = levelsInPass;
        pc.gammaCorrect = isSRGB ? 1 : 0;
        pc.alphaMode = alphaMode;

        vkCmdPushConstants(cmd, s_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &pc);

        // Dispatch matching the first destination mip size (local_size_x/y = 16)
        uint32_t groupCountX = (dstWidthLevel1 + 15) / 16;
        uint32_t groupCountY = (dstHeightLevel1 + 15) / 16;
        vkCmdDispatch(cmd, groupCountX, groupCountY, 1);

        // Transition destination levels from GENERAL to SHADER_READ_ONLY_OPTIMAL
        for (uint32_t j = 0; j < levelsInPass; j++) {
            barrier.subresourceRange.baseMipLevel = srcLevel + 1 + j;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }

    // Cleanup ImageView resources
    for (auto v : views) {
        vkDestroyImageView(ctx->device(), v, nullptr);
    }
    // Cleanup pool
    vkDestroyDescriptorPool(ctx->device(), localPool, nullptr);
}

} // namespace eruption
