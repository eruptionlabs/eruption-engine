#include "renderer/VolumetricLight.hpp"
#include "renderer/VulkanContext.hpp"
#include "renderer/VkCompat.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/BandwidthMeter.hpp"
#include "core/Logger.hpp"
#include "utils/Profiler.hpp"
#include <algorithm>
#include <cstdlib>

namespace eruption {

namespace {
// Pool de timestamps: 16..17 estavam livres (ver o mapa em PostProcessor.cpp).
constexpr uint32_t kQVolBegin = 16;
constexpr uint32_t kQVolEnd = 17;
constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

VkShaderModule loadModule(VkDevice dev, const char* path) {
    const auto code = ShaderCompiler::loadSPIRV(path);
    if (code.empty()) {
        ERUPTION_LOG_ERROR("VolumetricLight: %s nao encontrado", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = code.size() * sizeof(uint32_t);
    smi.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &smi, nullptr, &m);
    return m;
}

void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
             VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkc::cmdPipelineBarrier2(cmd, &dep);
}
} // namespace

bool VolumetricLight::init(VulkanContext* ctx, uint32_t renderW, uint32_t renderH, int mode) {
    m_ctx = ctx;
    m_renderW = renderW;
    m_renderH = renderH;
    m_mode = std::clamp(mode, 0, 2);
    const VkDevice dev = ctx->device();

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    vkCreateSampler(dev, &si, nullptr, &m_pointSampler);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(dev, &si, nullptr, &m_linearSampler);

    VkDescriptorSetLayoutBinding b[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout);

    const uint32_t nSets = kSetsPerFrame * kMaxFrames;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * nSets},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nSets}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = nSets;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool);
    VkDescriptorSetLayout layouts[nSets];
    std::fill(layouts, layouts + nSets, m_setLayout);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = nSets;
    ai.pSetLayouts = layouts;
    vkAllocateDescriptorSets(dev, &ai, m_sets);

    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout);

    // Fallback 1x1 (preta, alfa 0): o composite sempre tem o que ler.
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kFormat;
    ii.extent = {1, 1, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(ctx->allocator(), &ii, &aci, &m_fallback, &m_fallbackAlloc, nullptr);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_fallback;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(dev, &vi, nullptr, &m_fallbackView);
    m_fallbackReady = false;

    createPipelines();
    createTargets();
    return m_marchPipeline != VK_NULL_HANDLE && m_blurPipeline != VK_NULL_HANDLE;
}

void VolumetricLight::createPipelines() {
    const VkDevice dev = m_ctx->device();
    VkShaderModule vert = loadModule(dev, "postprocess/fullscreen.vert.spv");
    VkShaderModule march = loadModule(dev, "lighting/volumetric_sun.frag.spv");
    VkShaderModule blur = loadModule(dev, "lighting/volumetric_blur.frag.spv");
    VkPipelineColorBlendAttachmentState opaque{};
    opaque.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT;
    VkPipelineVertexInputStateCreateInfo noVerts{};
    noVerts.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    auto build = [&](VkShaderModule frag) -> VkPipeline {
        if (!vert || !frag) return VK_NULL_HANDLE;
        VkPipelineShaderStageCreateInfo st[2] = {};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vert;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = frag;
        st[1].pName = "main";
        return PipelineBuilder()
            .setShaderStages({st[0], st[1]})
            .setVertexInput(noVerts)
            .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setViewport(0, 0, 1, 1)
            .setScissor(0, 0, 1, 1)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
            .setBlendState({opaque})
            .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setLayout(m_layout)
            .setColorAttachmentFormats({kFormat})
            .build(dev);
    };
    m_marchPipeline = build(march);
    m_blurPipeline = build(blur);
    if (vert) vkDestroyShaderModule(dev, vert, nullptr);
    if (march) vkDestroyShaderModule(dev, march, nullptr);
    if (blur) vkDestroyShaderModule(dev, blur, nullptr);
}

void VolumetricLight::createTargets() {
    if (m_mode <= 0) return;
    const uint32_t div = m_mode == 1 ? 2u : 4u;
    m_w = std::max(1u, m_renderW / div);
    m_h = std::max(1u, m_renderH / div);
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kFormat;
    ii.extent = {m_w, m_h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    for (int i = 0; i < 2; ++i) {
        vmaCreateImage(m_ctx->allocator(), &ii, &aci, &m_images[i], &m_allocs[i], nullptr);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = m_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc::createImageView(m_ctx->device(), &vi, nullptr, &m_views[i]);
    }
}

void VolumetricLight::destroyTargets() {
    // Troca de preset/tamanho (rara): quadros em voo ainda podem ler os alvos.
    if (m_images[0] || m_images[1]) vkDeviceWaitIdle(m_ctx->device());
    for (int i = 0; i < 2; ++i) {
        if (m_views[i]) vkDestroyImageView(m_ctx->device(), m_views[i], nullptr);
        if (m_images[i]) vmaDestroyImage(m_ctx->allocator(), m_images[i], m_allocs[i]);
        m_views[i] = VK_NULL_HANDLE;
        m_images[i] = VK_NULL_HANDLE;
    }
    m_active = false;
}

void VolumetricLight::shutdown() {
    if (!m_ctx) return;
    const VkDevice dev = m_ctx->device();
    destroyTargets();
    if (m_fallbackView) vkDestroyImageView(dev, m_fallbackView, nullptr);
    if (m_fallback) vmaDestroyImage(m_ctx->allocator(), m_fallback, m_fallbackAlloc);
    if (m_marchPipeline) vkDestroyPipeline(dev, m_marchPipeline, nullptr);
    if (m_blurPipeline) vkDestroyPipeline(dev, m_blurPipeline, nullptr);
    if (m_layout) vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_pointSampler) vkDestroySampler(dev, m_pointSampler, nullptr);
    if (m_linearSampler) vkDestroySampler(dev, m_linearSampler, nullptr);
    *this = VolumetricLight{};
}

void VolumetricLight::resize(uint32_t renderW, uint32_t renderH) {
    if (!m_ctx || (renderW == m_renderW && renderH == m_renderH)) return;
    destroyTargets();
    m_renderW = renderW;
    m_renderH = renderH;
    createTargets();
}

void VolumetricLight::setMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (!m_ctx || mode == m_mode) return;
    destroyTargets();
    m_mode = mode;
    createTargets();
}

void VolumetricLight::render(VkCommandBuffer cmd, VkImageView sceneDepth, VkImageView shadowAtlas, VkBuffer shadowUbo,
                             const Params& p) {
    if (!m_ctx) return;
    if (!m_fallbackReady) {
        barrier(cmd, m_fallback, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_fallback, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &r);
        barrier(cmd, m_fallback, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        m_fallbackReady = true;
    }
    static const bool kOff = std::getenv("ERUPTION_TEST_NO_VOLUMETRIC") != nullptr;
    m_active = !kOff && m_mode > 0 && m_images[0] && m_marchPipeline && m_blurPipeline && p.intensity > 0.0f &&
               p.density > 0.0f && sceneDepth && shadowAtlas && shadowUbo;
    if (!m_active) return;

    m_ctx->writeTimestamp(cmd, kQVolBegin, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    bw::pushPass("Volumetric Light");
    const uint32_t frame = m_ctx->currentFrame() % kMaxFrames;
    VkDescriptorSet* sets = &m_sets[frame * kSetsPerFrame];
    auto write = [&](VkDescriptorSet set, VkImageView v0, VkSampler s0, VkImageView v1, VkSampler s1) {
        VkDescriptorImageInfo i0{s0, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo i1{s1, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo ub{shadowUbo, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].pImageInfo = &i0;
        w[1].pImageInfo = &i1;
        w[2].pBufferInfo = &ub;
        vkUpdateDescriptorSets(m_ctx->device(), 3, w, 0, nullptr);
    };
    write(sets[0], sceneDepth, m_pointSampler, shadowAtlas, m_pointSampler);
    write(sets[1], m_views[0], m_linearSampler, sceneDepth, m_pointSampler);
    write(sets[2], m_views[1], m_linearSampler, sceneDepth, m_pointSampler);

    auto pass = [&](int target, VkPipeline pipe, VkDescriptorSet set, const void* push, uint32_t pushSize) {
        barrier(cmd, m_images[target], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = m_views[target];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {m_w, m_h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkc::cmdBeginRendering(cmd, &ri);
        const VkViewport vp{0.0f, 0.0f, float(m_w), float(m_h), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {m_w, m_h}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
        barrier(cmd, m_images[target], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    };

    struct {
        Mat4 invViewProj;
        Vec4 camPos, toSun, radiance, medium;
    } march;
    static_assert(sizeof(march) <= 128, "push da luz volumetrica passou de 128 B");
    march.invViewProj = p.invViewProj;
    march.camPos = Vec4(p.cameraPos, std::max(p.maxDistance, 1.0f));
    march.toSun = Vec4(p.toSun, std::clamp(p.anisotropy, -0.95f, 0.95f));
    march.radiance = Vec4(p.sunRadiance * p.tint * p.intensity, float(std::clamp(p.steps, 4, 128)));
    march.medium = Vec4(p.density, p.fogHeight, std::max(p.fogFalloff, 0.0f), 0.0f);
    pass(0, m_marchPipeline, sets[0], &march, sizeof(march));
    const float blurH[4] = {1.0f, 0.0f, p.nearPlane, p.farPlane};
    pass(1, m_blurPipeline, sets[1], blurH, sizeof(blurH));
    const float blurV[4] = {0.0f, 1.0f, p.nearPlane, p.farPlane};
    pass(0, m_blurPipeline, sets[2], blurV, sizeof(blurV));
    m_ctx->writeTimestamp(cmd, kQVolEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    bw::popPass();
    Profiler::addGpuTime("Volumetric Light", m_ctx->timestampDeltaMs(kQVolBegin, kQVolEnd));
}

} // namespace eruption
