#include "editor/VfxPreview.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "renderer/VkCompat.hpp"

#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eruption {

namespace {
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;

void imgBarrier(VulkanContext* ctx, VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags src, VkAccessFlags srcA, VkPipelineStageFlags dst, VkAccessFlags dstA,
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    ctx->cmdImageBarrier(cmd, img, from, to, src, dst, srcA, dstA, aspect);
}
} // namespace

bool VfxPreview::init(Engine& engine) {
    m_engine = &engine;
    m_ctx = &engine.vulkan();
    VkDevice dev = m_ctx->device();
    VfxSystem::InitInfo base = engine.vfxInitInfo();

    // FrameUBO da previa: mesmo layout do da cena (o desenho das particulas
    // le' a camera e a luz dele), buffers proprios.
    const uint32_t n = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = n;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_uboPool) != VK_SUCCESS) return false;
    for (uint32_t f = 0; f < n; ++f) {
        if (!m_ctx->createBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
                                 m_ubo[f], m_uboAlloc[f]))
            return false;
        vmaMapMemory(m_ctx->allocator(), m_uboAlloc[f], &m_uboMapped[f]);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_uboPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &base.frameUboLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &m_uboSets[f]) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo bi{m_ubo[f], 0, sizeof(FrameUBO)};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_uboSets[f];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    // Pool pequeno: a previa mostra um efeito por vez.
    m_sys.setBudget(65536, 1.0f);
    // O set do frame e' trocado a cada frame (um por frame em voo): o
    // VfxSystem guarda um so', entao a previa liga o do frame atual por cima.
    VfxSystem::InitInfo info = engine.vfxInitInfo(m_uboSets[0]);
    if (!m_sys.init(info)) return false;
    m_sysReady = true;
    if (!createTonemap()) return false;
    m_ready = true;
    return true;
}

void VfxPreview::shutdown() {
    if (!m_ctx) return;
    m_ctx->waitIdle();
    VkDevice dev = m_ctx->device();
    destroyTargets();
    if (m_sysReady) m_sys.shutdown();
    m_sysReady = false;
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        if (m_uboMapped[f]) vmaUnmapMemory(m_ctx->allocator(), m_uboAlloc[f]);
        if (m_ubo[f]) vmaDestroyBuffer(m_ctx->allocator(), m_ubo[f], m_uboAlloc[f]);
        m_ubo[f] = VK_NULL_HANDLE;
        m_uboMapped[f] = nullptr;
    }
    if (m_uboPool) vkDestroyDescriptorPool(dev, m_uboPool, nullptr);
    if (m_tmPipe) vkDestroyPipeline(dev, m_tmPipe, nullptr);
    if (m_tmPipeLayout) vkDestroyPipelineLayout(dev, m_tmPipeLayout, nullptr);
    if (m_tmPool) vkDestroyDescriptorPool(dev, m_tmPool, nullptr);
    if (m_tmLayout) vkDestroyDescriptorSetLayout(dev, m_tmLayout, nullptr);
    m_uboPool = m_tmPool = VK_NULL_HANDLE;
    m_tmPipe = VK_NULL_HANDLE;
    m_tmPipeLayout = VK_NULL_HANDLE;
    m_tmLayout = VK_NULL_HANDLE;
    m_ready = false;
    m_ctx = nullptr;
}

bool VfxPreview::createTonemap() {
    VkDevice dev = m_ctx->device();
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   nullptr};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_tmLayout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_tmPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_tmPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_tmLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_tmSet) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_tmLayout;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_tmPipeLayout) != VK_SUCCESS) return false;

    auto load = [&](const char* path) {
        const std::vector<uint32_t> code = ShaderCompiler::loadSPIRV(path);
        VkShaderModule m = VK_NULL_HANDLE;
        if (code.empty()) return m;
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        vkCreateShaderModule(dev, &ci, nullptr, &m);
        return m;
    };
    VkShaderModule vs = load("vfx/vfx_composite.vert.spv");
    VkShaderModule fs = load("editor/vfx_preview_tonemap.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(dev, fs, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &kLdrFormat;
    VkGraphicsPipelineCreateInfo gi{};
    gi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = m_tmPipeLayout;
    const VkResult r = vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &m_tmPipe);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    return r == VK_SUCCESS;
}

bool VfxPreview::createTargets() {
    m_w = std::max(64u, m_wantW);
    m_h = std::max(64u, m_wantH);
    VkDevice dev = m_ctx->device();
    if (!m_ctx->createImage(m_w, m_h, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_hdr, m_hdrAlloc) ||
        !m_ctx->createImage(m_w, m_h, kDepthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_depth, m_depthAlloc) ||
        !m_ctx->createImage(m_w, m_h, kLdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_ldr, m_ldrAlloc))
        return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vi.image = m_hdr;
    vi.format = kHdrFormat;
    vkc::createImageView(dev, &vi, nullptr, &m_hdrView);
    vi.image = m_ldr;
    vi.format = kLdrFormat;
    vkc::createImageView(dev, &vi, nullptr, &m_ldrView);
    vi.image = m_depth;
    vi.format = kDepthFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkc::createImageView(dev, &vi, nullptr, &m_depthView);
    if (!m_sampler) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        vkCreateSampler(dev, &si, nullptr, &m_sampler);
    }
    VkDescriptorImageInfo ii{m_sampler, m_hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = m_tmSet;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    m_texture = ImGui_ImplVulkan_AddTexture(m_sampler, m_ldrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_ldrReadable = false;
    return true;
}

void VfxPreview::destroyTargets() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    if (m_texture) ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(m_texture));
    m_texture = nullptr;
    for (VkImageView* v : {&m_hdrView, &m_ldrView, &m_depthView})
        if (*v) vkDestroyImageView(dev, *v, nullptr), *v = VK_NULL_HANDLE;
    if (m_hdr) vmaDestroyImage(m_ctx->allocator(), m_hdr, m_hdrAlloc);
    if (m_ldr) vmaDestroyImage(m_ctx->allocator(), m_ldr, m_ldrAlloc);
    if (m_depth) vmaDestroyImage(m_ctx->allocator(), m_depth, m_depthAlloc);
    m_hdr = m_ldr = m_depth = VK_NULL_HANDLE;
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
}

void VfxPreview::setSize(uint32_t w, uint32_t h) {
    // Anotado: a imagem e' refeita no proximo render(), antes de usar.
    m_wantW = std::max(64u, w);
    m_wantH = std::max(64u, h);
}

void VfxPreview::play(const std::string& name, const VfxEffectDesc& desc, bool restartNow) {
    if (!m_ready) return;
    // A previa e' em metros (fundo neutro, sem mundo), a camera tambem.
    m_sys.setUnitsPerMeter(1.0f);
    m_sys.setEffect(name, desc);
    if (restartNow || name != m_name || !m_sys.isPlaying(m_instance)) {
        if (m_instance) m_sys.stop(m_instance, true);
        m_instance = m_sys.spawn(name, Mat4(1.0f));
        m_name = name;
    }
}

void VfxPreview::restart() {
    if (m_instance) m_sys.restart(m_instance);
}

float VfxPreview::effectTime() const { return m_sys.instanceTime(m_instance); }

void VfxPreview::orbit(float dx, float dy) {
    m_yaw -= dx * 0.008f;
    m_pitch = std::clamp(m_pitch + dy * 0.008f, -1.4f, 1.4f);
}

void VfxPreview::pan(float dx, float dy) {
    const Vec3 right(std::cos(m_yaw), 0.0f, -std::sin(m_yaw));
    m_target += (-right * dx + Vec3(0.0f, 1.0f, 0.0f) * dy) * (m_dist * 0.0015f);
}

void VfxPreview::zoom(float wheel) { m_dist = std::clamp(m_dist * std::pow(0.88f, wheel), 0.2f, 500.0f); }

void VfxPreview::frame() {
    m_target = Vec3(0.0f, 1.2f, 0.0f);
    m_dist = 6.0f;
    m_yaw = 0.6f;
    m_pitch = 0.2f;
}

void VfxPreview::render(VkCommandBuffer cmd, float dt) {
    if (!m_ready) return;
    if (m_wantW != m_w || m_wantH != m_h || !m_hdr) {
        // Nada deste frame usou as imagens ainda; os frames em voo terminam antes.
        m_ctx->waitIdle();
        destroyTargets();
        if (!createTargets()) return;
        m_sys.resize(m_w, m_h); // mascara reativa no tamanho da previa
    }
    const uint32_t frame = m_ctx->currentFrame();

    // Camera orbital
    const Vec3 dir(std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch), std::cos(m_pitch) * std::cos(m_yaw));
    m_cam.setPerspective(50.0f, float(m_w) / float(m_h), std::max(0.05f, m_dist * 0.01f), m_dist * 50.0f + 2000.0f);
    m_cam.setPosition(m_target + dir * m_dist);
    m_cam.setTarget(m_target);
    m_cam.updateFrustum();

    FrameUBO u{};
    u.view = m_cam.viewMatrix();
    u.projection = m_cam.projNoJitter();
    u.viewProjection = m_cam.viewProjNoJitter();
    u.inverseView = glm::inverse(u.view);
    u.inverseProjection = glm::inverse(u.projection);
    u.cameraPos = m_cam.position();
    u.time = effectTime();
    u.screenResolution = Vec2(float(m_w), float(m_h));
    u.nearPlane = m_cam.nearPlane();
    u.farPlane = m_cam.farPlane();
    // Luz de estudio fixa (sol alto + ceu neutro).
    u.sunDir = Vec4(glm::normalize(Vec3(-0.4f, -0.8f, -0.3f)), 1.6f);
    u.sunColor = Vec4(1.0f, 0.96f, 0.9f, 0.0f);
    u.ambientSky = Vec4(0.55f, 0.6f, 0.7f, 0.6f);
    u.ambientGround = Vec4(0.3f, 0.28f, 0.25f, 0.0f);
    std::memcpy(m_uboMapped[frame], &u, sizeof(u));
    vmaFlushAllocation(m_ctx->allocator(), m_uboAlloc[frame], 0, VK_WHOLE_SIZE);
    m_sys.setFrameUboSet(m_uboSets[frame]);
    m_sys.setLighting(u.sunDir, u.sunColor, u.ambientSky);

    const VkExtent2D ext{m_w, m_h};
    m_sys.update(playing ? dt * speed : 0.0f, m_cam, ext);

    // Limpa cor (fundo) e profundidade (nada na cena da previa).
    imgBarrier(m_ctx, cmd, m_hdr, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imgBarrier(m_ctx, cmd, m_depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = m_hdrView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.045f, 0.05f, 0.058f, 1.0f}};
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkc::cmdBeginRendering(cmd, &ri);
    vkc::cmdEndRendering(cmd);

    imgBarrier(m_ctx, cmd, m_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    m_sys.simulate(cmd, m_depthView);
    imgBarrier(m_ctx, cmd, m_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    m_sys.drawLowRes(cmd, m_depthView, ext);
    m_sys.draw(cmd, m_hdrView, m_depthView, ext);
    m_sys.endFrame(cmd);

    // HDR -> LDR da interface
    imgBarrier(m_ctx, cmd, m_hdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    imgBarrier(m_ctx, cmd, m_ldr, m_ldrReadable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ldr{};
    ldr.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ldr.imageView = m_ldrView;
    ldr.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ldr.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ldr.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo lri{};
    lri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    lri.renderArea = {{0, 0}, ext};
    lri.layerCount = 1;
    lri.colorAttachmentCount = 1;
    lri.pColorAttachments = &ldr;
    vkc::cmdBeginRendering(cmd, &lri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tmPipe);
    const VkViewport vp{0.0f, 0.0f, float(m_w), float(m_h), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tmPipeLayout, 0, 1, &m_tmSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkc::cmdEndRendering(cmd);
    imgBarrier(m_ctx, cmd, m_ldr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    m_ldrReadable = true;
}

} // namespace eruption
