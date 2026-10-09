#include "renderer/SpriteLayer.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

namespace eruption {

namespace {
constexpr VkFormat kMaskFormat = VK_FORMAT_R8_UNORM;
}

bool SpriteLayer::init(VulkanContext* ctx, SpriteRenderer* sprites, uint32_t renderW, uint32_t renderH) {
    m_ctx = ctx;
    m_sprites = sprites;
    VkDevice dev = m_ctx->device();

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    // Set da mescla: 0 = mascara atual, 1 = anterior, 2 = reativa (storage).
    {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 3;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_reactiveSetLayout) != VK_SUCCESS) return false;
    }
    // Set 2 da camada: depth, iluminado, albedo (todos de render).
    {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 3;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_layerSetLayout) != VK_SUCCESS) return false;
    }
    const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 2 + 3},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 3;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout rl[2] = {m_reactiveSetLayout, m_reactiveSetLayout};
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = rl;
    if (vkAllocateDescriptorSets(dev, &ai, m_reactiveSets.data()) != VK_SUCCESS) return false;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_layerSetLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_layerSet) != VK_SUCCESS) return false;

    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t) * 2};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_reactiveSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_reactiveLayout) != VK_SUCCESS) return false;
    const auto code = ShaderCompiler::loadSPIRV("compute/sprite_reactive.comp.spv");
    if (code.empty()) {
        Logger::error("SpriteLayer: compute/sprite_reactive.comp.spv nao encontrado");
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = code.size() * sizeof(uint32_t);
    smi.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = m_reactiveLayout;
    const VkResult r = vkCreateComputePipelines(dev, m_ctx->pipelineCache(), 1, &ci, nullptr, &m_reactivePipeline);
    vkDestroyShaderModule(dev, module, nullptr);
    if (r != VK_SUCCESS) return false;

    if (!m_sprites->createLayerPipelines(m_layerSetLayout)) {
        Logger::error("SpriteLayer: falha ao criar as pipelines de mascara/camada");
        return false;
    }
    m_w = renderW;
    m_h = renderH;
    createTargets();
    return true;
}

void SpriteLayer::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    vkDeviceWaitIdle(dev);
    destroyTargets();
    if (m_reactivePipeline) vkDestroyPipeline(dev, m_reactivePipeline, nullptr);
    if (m_reactiveLayout) vkDestroyPipelineLayout(dev, m_reactiveLayout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_reactiveSetLayout) vkDestroyDescriptorSetLayout(dev, m_reactiveSetLayout, nullptr);
    if (m_layerSetLayout) vkDestroyDescriptorSetLayout(dev, m_layerSetLayout, nullptr);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_reactivePipeline = VK_NULL_HANDLE;
    m_reactiveLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_reactiveSetLayout = m_layerSetLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

void SpriteLayer::createTargets() {
    auto make = [&](Img& img, VkImageUsageFlags usage) {
        m_ctx->createImage(m_w, m_h, kMaskFormat, usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VMA_MEMORY_USAGE_GPU_ONLY, img.image, img.alloc);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kMaskFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc::createImageView(m_ctx->device(), &vi, nullptr, &img.view);
    };
    for (Img& m : m_mask) make(m, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    make(m_reactive, VK_IMAGE_USAGE_STORAGE_BIT);
    // Tudo comeca em zero e legivel: o primeiro frame le' a mascara "anterior".
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        for (Img* img : {&m_mask[0], &m_mask[1], &m_reactive}) {
            m_ctx->cmdImageBarrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            m_ctx->cmdImageBarrier(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    });
    writeReactiveSets();
}

void SpriteLayer::destroyTargets() {
    for (Img* img : {&m_mask[0], &m_mask[1], &m_reactive}) {
        if (img->view) vkDestroyImageView(m_ctx->device(), img->view, nullptr);
        if (img->image) vmaDestroyImage(m_ctx->allocator(), img->image, img->alloc);
        *img = Img{};
    }
}

void SpriteLayer::resize(uint32_t renderW, uint32_t renderH) {
    if (!m_ctx || (renderW == m_w && renderH == m_h)) return;
    vkDeviceWaitIdle(m_ctx->device());
    destroyTargets();
    m_w = renderW;
    m_h = renderH;
    createTargets();
}

void SpriteLayer::writeReactiveSets() {
    for (uint32_t cur = 0; cur < 2; ++cur) {
        VkDescriptorImageInfo a{m_sampler, m_mask[cur].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo b{m_sampler, m_mask[cur ^ 1u].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo c{VK_NULL_HANDLE, m_reactive.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3]{};
        const VkDescriptorImageInfo* infos[3] = {&a, &b, &c};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = m_reactiveSets[cur];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = infos[i];
        }
        vkUpdateDescriptorSets(m_ctx->device(), 3, w, 0, nullptr);
    }
}

void SpriteLayer::bindLayerInputs(VkImageView depth, VkImageView lit, VkImageView albedo) {
    if (!m_ctx) return;
    vkDeviceWaitIdle(m_ctx->device());
    const VkImageView views[3] = {depth, lit, albedo};
    VkDescriptorImageInfo infos[3];
    VkWriteDescriptorSet w[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        infos[i] = {m_sampler, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = m_layerSet;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_ctx->device(), 3, w, 0, nullptr);
}

void SpriteLayer::renderMask(VkCommandBuffer cmd, VkImageView depthView, VkBuffer instances, uint32_t count,
                             const Mat4& viewProjJittered) {
    if (!m_ctx) return;
    m_current ^= 1u;
    Img& mask = m_mask[m_current];
    m_ctx->cmdImageBarrier(cmd, mask.image, mask.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = mask.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    m_ctx->cmdBeginRendering(cmd, {color}, &depth, nullptr, {m_w, m_h});
    m_sprites->drawMask(cmd, instances, count, viewProjJittered);
    vkc::cmdEndRendering(cmd);
    m_ctx->cmdImageBarrier(cmd, mask.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    mask.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void SpriteLayer::buildReactive(VkCommandBuffer cmd) {
    if (!m_ctx) return;
    m_ctx->cmdImageBarrier(cmd, m_reactive.image, m_reactive.layout, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_reactivePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_reactiveLayout, 0, 1,
                            &m_reactiveSets[m_current], 0, nullptr);
    const int32_t size[2] = {int32_t(m_w), int32_t(m_h)};
    vkCmdPushConstants(cmd, m_reactiveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    vkCmdDispatch(cmd, (m_w + 7) / 8, (m_h + 7) / 8, 1);
    m_ctx->cmdImageBarrier(cmd, m_reactive.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    m_reactive.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void SpriteLayer::renderLayer(VkCommandBuffer cmd, VkImage target, VkImageView targetView, VkExtent2D targetExtent,
                              VkBuffer instances, uint32_t count, const Mat4& view, const Mat4& projNoJitter,
                              float nearZ, float farZ) {
    if (!m_ctx || count == 0) return;
    m_ctx->cmdImageBarrier(cmd, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = targetView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    m_ctx->cmdBeginRendering(cmd, {color}, nullptr, nullptr, targetExtent);
    SpriteRenderer::LayerPush push{};
    push.view = view;
    push.proj = projNoJitter;
    push.sizes = Vec4(float(m_w), float(m_h), float(targetExtent.width), float(targetExtent.height));
    push.planes = Vec4(nearZ, farZ, 0.0f, 0.0f);
    m_sprites->drawLayer(cmd, instances, count, m_layerSet, push, targetExtent);
    vkc::cmdEndRendering(cmd);
    m_ctx->cmdImageBarrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

} // namespace eruption
