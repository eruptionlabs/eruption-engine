#include "editor/ModelPreview.hpp"

#include "core/Logger.hpp"
#include "formats/TerrainParser.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/ModelRenderer.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/VkCompat.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>

namespace eruption {

namespace {

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// Push do model_novel.vert (176 B) + o que o fragment do visualizador lê a mais.
struct PreviewPush {
    Mat4 viewProjection;
    Mat4 model;
    float alpha, metallicScale, roughnessScale, pad;
    Vec4 eye;        // uvTranslateRot: o vértice não lê; o fragment usa como olho
    Vec4 uvScale;    // z: UV por unidade, w: deslocamento (0 = desligado)
    Vec4 lightMode;  // xyz: direção da luz, w: modo
    Vec4 wireCut;    // rgb: cor do aramado, a: corte de alfa
};
static_assert(sizeof(PreviewPush) == 208, "push do visualizador");

VkShaderModule loadModule(VkDevice dev, const char* path) {
    const std::vector<uint32_t> code = ShaderCompiler::loadSPIRV(path);
    if (code.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &info, nullptr, &m);
    return m;
}

void barrier(VulkanContext* ctx, VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
             VkAccessFlags2 dstAccess, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    std::vector<VkImageMemoryBarrier2> list{b};
    ctx->cmdImageBarriers(cmd, list);
}

} // namespace

bool ModelPreview::init(VulkanContext* ctx, BindlessDescriptor* bindless, ModelRenderer* models,
                        VkDescriptorSetLayout frameUboLayout, VkDescriptorSet frameUboSet) {
    m_ctx = ctx;
    m_bindless = bindless;
    m_models = models;
    m_frameUboLayout = frameUboLayout;
    m_frameUboSet = frameUboSet;
    if (!createInstances() || !createPipelines()) {
        ERUPTION_LOG_ERROR("Model viewer: could not create its pipeline");
        shutdown();
        return false;
    }
    return createTargets();
}

void ModelPreview::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    vkDeviceWaitIdle(dev);
    close();
    destroyTargets();
    if (m_fillPipeline) vkDestroyPipeline(dev, m_fillPipeline, nullptr);
    if (m_linePipeline) vkDestroyPipeline(dev, m_linePipeline, nullptr);
    if (m_layout) vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_instLayout) vkDestroyDescriptorSetLayout(dev, m_instLayout, nullptr);
    if (m_instBuffer) {
        vmaUnmapMemory(m_ctx->allocator(), m_instAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_instBuffer, m_instAlloc);
    }
    m_fillPipeline = m_linePipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_instLayout = VK_NULL_HANDLE;
    m_instBuffer = VK_NULL_HANDLE;
    m_instMapped = nullptr;
    m_ctx = nullptr;
}

bool ModelPreview::createInstances() {
    VkDevice dev = m_ctx->device();
    const VkDeviceSize region = VkDeviceSize(kMaxNodes) * sizeof(GpuInstance);
    if (!m_ctx->createBuffer(region * VulkanContext::MAX_FRAMES_IN_FLIGHT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, m_instBuffer, m_instAlloc))
        return false;
    void* mapped = nullptr;
    vmaMapMemory(m_ctx->allocator(), m_instAlloc, &mapped);
    m_instMapped = static_cast<GpuInstance*>(mapped);

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_instLayout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VulkanContext::MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_instLayout;
        if (vkAllocateDescriptorSets(dev, &ai, &m_instSets[f]) != VK_SUCCESS) return false;
        // Uma faixa do buffer por frame em voo: escrever a do frame atual não
        // mexe no que um frame anterior ainda está lendo.
        const VkDescriptorBufferInfo bi{m_instBuffer, region * f, region};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_instSets[f];
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    return true;
}

bool ModelPreview::createPipelines() {
    VkDevice dev = m_ctx->device();
    VkShaderModule vert = loadModule(dev, "shaders/gbuffer/model_novel.vert.spv");
    VkShaderModule frag = loadModule(dev, "shaders/editor/model_preview.frag.spv");
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(dev, vert, nullptr);
        if (frag) vkDestroyShaderModule(dev, frag, nullptr);
        return false;
    }
    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    for (auto& st : stages) {
        st = {};
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;

    // Mesmo layout de vértice do ModelRenderer (o vértice é o model_novel.vert).
    VkVertexInputBindingDescription binding{0, sizeof(TerrainVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::vector<VkVertexInputAttributeDescription> attribs = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, texCoord)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, normal)},
        {3, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, texIndex)},
        {4, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, matId)},
        {5, 0, VK_FORMAT_R32_UINT, offsetof(TerrainVertex, color)},
        {6, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, pbrIndex)},
        {7, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, normalIndex)},
        {8, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendTexIndex)},
        {9, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendPbrIndex)},
        {10, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendNormalIndex)},
        {11, 0, VK_FORMAT_R32_SFLOAT, offsetof(TerrainVertex, blendWeight)},
        {12, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendMaskIndex)},
        {13, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, blendMaskUV)},
        {14, 0, VK_FORMAT_R32_SFLOAT, offsetof(TerrainVertex, emissiveStrength)},
        {15, 0, VK_FORMAT_R32G32_UINT, offsetof(TerrainVertex, splatTex01)},
        {16, 0, VK_FORMAT_R32G32_UINT, offsetof(TerrainVertex, splatTex45)},
        {17, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendMaskIndex2)},
        {18, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, emissiveIndex)},
    };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vi.pVertexAttributeDescriptions = attribs.data();

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size = sizeof(PreviewPush);
    VkDescriptorSetLayout sets[3] = {m_bindless->layout(), m_frameUboLayout, m_instLayout};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 3;
    li.pSetLayouts = sets;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    const bool ok = vkCreatePipelineLayout(dev, &li, nullptr, &m_layout) == VK_SUCCESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    const std::vector<VkPipelineColorBlendAttachmentState> blends{blend};
    auto build = [&](VkPolygonMode poly, bool depthWrite) {
        return PipelineBuilder()
            .setShaderStages(stages)
            .setVertexInput(vi)
            .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setViewport(0, 0, 64, 64)
            .setScissor(0, 0, 64, 64)
            .setPolygonMode(poly)
            .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE) // folhas: os dois lados
            .setDepthState(true, depthWrite, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setBlendState(blends)
            .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setLayout(m_layout)
            .setColorAttachmentFormats({kColorFormat})
            .setDepthAttachmentFormat(kDepthFormat)
            .build(dev);
    };
    if (ok) {
        m_fillPipeline = build(VK_POLYGON_MODE_FILL, true);
        // Aramado: precisa de fillModeNonSolid (sem ele, só o sólido).
        if (m_ctx->wireframeSupported()) m_linePipeline = build(VK_POLYGON_MODE_LINE, false);
    }
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    return ok && m_fillPipeline != VK_NULL_HANDLE;
}

bool ModelPreview::createTargets() {
    m_w = std::max(64u, m_wantW);
    m_h = std::max(64u, m_wantH);
    VkDevice dev = m_ctx->device();
    if (!m_ctx->createImage(m_w, m_h, kColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_color, m_colorAlloc) ||
        !m_ctx->createImage(m_w, m_h, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_depth, m_depthAlloc))
        return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.image = m_color;
    vi.format = kColorFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(dev, &vi, nullptr, &m_colorView);
    vi.image = m_depth;
    vi.format = kDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkc::createImageView(dev, &vi, nullptr, &m_depthView);
    if (!m_sampler) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 1.0f;
        vkCreateSampler(dev, &si, nullptr, &m_sampler);
    }
    m_texture = ImGui_ImplVulkan_AddTexture(m_sampler, m_colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_colorReadable = false;
    return true;
}

void ModelPreview::destroyTargets() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    if (m_texture) ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(m_texture));
    if (m_colorView) vkDestroyImageView(dev, m_colorView, nullptr);
    if (m_depthView) vkDestroyImageView(dev, m_depthView, nullptr);
    if (m_color) vmaDestroyImage(m_ctx->allocator(), m_color, m_colorAlloc);
    if (m_depth) vmaDestroyImage(m_ctx->allocator(), m_depth, m_depthAlloc);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_texture = nullptr;
    m_colorView = m_depthView = VK_NULL_HANDLE;
    m_color = m_depth = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

void ModelPreview::setSize(uint32_t w, uint32_t h) {
    // Só anota: a interface é montada depois do desenho deste frame, que já
    // usou a imagem; ela é refeita no próximo render(), antes de usar.
    m_wantW = std::max(64u, w);
    m_wantH = std::max(64u, h);
}

bool ModelPreview::open(const std::string& assetPath, int node) {
    close();
    if (!m_models) return false;
    const ModelAsset* asset = m_models->asset(assetPath);
    if (!asset) return false;
    m_flipY = asset->versionMajor * 256 + asset->versionMinor < 0x202; // modelo antigo (Y para baixo)
    const Mat4 flip = m_flipY ? glm::scale(Mat4(1.0f), Vec3(1.0f, -1.0f, 1.0f)) : Mat4(1.0f);
    for (const auto& n : m_models->assetNodes(assetPath)) {
        if (node >= 0 && static_cast<int>(n.node) != node) continue;
        if (m_nodes.size() >= kMaxNodes) break;
        Node pn{n.mesh, flip * n.matrix, -1};
        // Modelo com esqueleto: cópia animada só do visualizador.
        if (m_models->isSkinBase(n.mesh)) {
            pn.skin = m_models->addPreviewSkin(n.mesh);
            if (pn.skin >= 0 && m_animations.empty()) m_animations = m_models->previewSkinAnimations(pn.skin);
        }
        m_nodes.push_back(pn);
    }
    if (m_nodes.empty()) return false;
    m_path = assetPath;
    m_node = node;
    animation = -1;
    m_playing = -2;
    computeBounds();
    frame();
    return true;
}

void ModelPreview::close() {
    if (m_models)
        for (const Node& n : m_nodes)
            if (n.skin >= 0) m_models->removePreviewSkin(n.skin);
    m_nodes.clear();
    m_animations.clear();
    m_path.clear();
    m_node = -1;
}

void ModelPreview::computeBounds() {
    Vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const Node& n : m_nodes) {
        const ModelMeshGPU* m = m_models->meshAt(n.mesh);
        if (!m || m->indexCount == 0) continue;
        for (int c = 0; c < 8; ++c) {
            const Vec3 p((c & 1) ? m->aabbMax.x : m->aabbMin.x, (c & 2) ? m->aabbMax.y : m->aabbMin.y,
                         (c & 4) ? m->aabbMax.z : m->aabbMin.z);
            const Vec3 w = Vec3(n.matrix * Vec4(p, 1.0f));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    }
    if (lo.x > hi.x) lo = hi = Vec3(0.0f);
    m_boundsMin = lo;
    m_boundsMax = hi;
}

void ModelPreview::frame() {
    m_target = (m_boundsMin + m_boundsMax) * 0.5f;
    // A caixa cabe na imagem: altura pelo campo vertical, largura (qualquer
    // giro) pelo horizontal, mais a metade da profundidade na frente.
    const Vec3 size = glm::max(m_boundsMax - m_boundsMin, Vec3(0.01f));
    const float halfH = size.y * 0.5f, halfW = std::max(size.x, size.z) * 0.5f;
    const float tanY = std::tan(glm::radians(35.0f) * 0.5f);
    const float aspect = m_h ? float(m_w) / float(m_h) : 1.0f;
    m_dist = std::max(halfH / tanY, halfW / (tanY * aspect)) * 1.12f + halfW;
}

void ModelPreview::orbit(float dx, float dy) {
    // Arrastar para a direita gira o modelo para a direita; para baixo, a
    // câmera desce (o eixo que o autor pediu).
    m_yaw -= dx * 0.008f;
    m_pitch = glm::clamp(m_pitch - dy * 0.008f, glm::radians(-85.0f), glm::radians(85.0f));
}

void ModelPreview::pan(float dx, float dy) {
    const float cp = std::cos(m_pitch);
    const Vec3 fwd(cp * std::cos(m_yaw), std::sin(m_pitch), cp * std::sin(m_yaw));
    const Vec3 right = glm::normalize(glm::cross(Vec3(0, 1, 0), fwd));
    const Vec3 up = glm::cross(fwd, right);
    const float k = m_dist * 0.0015f;
    m_target += (right * dx + up * dy) * k;
}

void ModelPreview::zoom(float wheel) {
    m_dist = std::max(0.01f, m_dist * std::pow(0.88f, wheel));
}

ModelPreview::Stats ModelPreview::stats() const {
    Stats s;
    s.nodes = m_nodes.size();
    s.size = m_boundsMax - m_boundsMin;
    for (const Node& n : m_nodes) {
        const ModelMeshGPU* m = m_models ? m_models->meshAt(n.mesh) : nullptr;
        if (!m) continue;
        const uint64_t full = m->indexCount / 3;
        const uint64_t nearT = m->loIndexCount ? m->loIndexCount / 3 : full;
        const uint64_t farT = m->farIndexCount ? m->farIndexCount / 3 : nearT;
        s.trisOriginal += full;
        s.trisNear += nearT;
        s.trisFar += farT;
        s.trisShadow += m->shIndexCount ? m->shIndexCount / 3 : nearT;
        s.switchNear = std::max(s.switchNear, m->loSwitchDistance);
        s.switchFar = std::max(s.switchFar, m->farSwitchDistance);
        s.skinned |= n.skin >= 0;
        s.foliage |= m->swayAmount > 0.0f;
        s.ground |= m->isGround;
        s.shadowOpaque &= m->shadowOpaque;
    }
    s.trisDrawn = lod == Lod::Original ? s.trisOriginal
                : lod == Lod::Near   ? s.trisNear
                : lod == Lod::Far    ? s.trisFar
                                     : s.trisShadow;
    return s;
}

void ModelPreview::render(VkCommandBuffer cmd, float dt) {
    if (!m_ctx) return;
    if (m_wantW != m_w || m_wantH != m_h) {
        // Nada deste frame usou a imagem ainda; os frames em voo terminam antes.
        m_ctx->waitIdle();
        destroyTargets();
        createTargets();
    }
    if (!m_color) return;
    const uint32_t frameIndex = m_ctx->currentFrame();
    if (spin) m_yaw += dt * 0.5f;
    if (m_playing != animation || m_playingSpeed != animationSpeed) {
        for (const Node& n : m_nodes)
            if (n.skin >= 0) m_models->playPreviewSkin(n.skin, animation, animationSpeed);
        m_playing = animation;
        m_playingSpeed = animationSpeed;
    }

    barrier(m_ctx, cmd, m_color, m_colorReadable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    barrier(m_ctx, cmd, m_depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = m_colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.16f, 0.17f, 0.19f, 1.0f}};
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {m_w, m_h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkc::cmdBeginRendering(cmd, &ri);

    if (!m_nodes.empty()) {
        const VkViewport vp{0, 0, static_cast<float>(m_w), static_cast<float>(m_h), 0, 1};
        const VkRect2D sc{{0, 0}, {m_w, m_h}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        const float cp = std::cos(m_pitch);
        const Vec3 eye = m_target + m_dist * Vec3(cp * std::cos(m_yaw), std::sin(m_pitch), cp * std::sin(m_yaw));
        const float radius = std::max(glm::length(m_boundsMax - m_boundsMin) * 0.5f, 0.01f);
        const float nearZ = std::max(m_dist - radius * 2.0f, m_dist * 0.002f);
        Mat4 proj = glm::perspective(glm::radians(35.0f), float(m_w) / float(m_h), nearZ, m_dist + radius * 3.0f);
        proj[1][1] *= -1.0f;
        const Mat4 viewProj = proj * glm::lookAt(eye, m_target, Vec3(0, 1, 0));
        const float ly = glm::radians(lightYaw), lp = glm::radians(lightPitch);
        const Vec3 toLight(std::cos(lp) * std::cos(ly), std::sin(lp), std::cos(lp) * std::sin(ly));

        GpuInstance* inst = m_instMapped + size_t(frameIndex) * kMaxNodes;
        VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, m_instSets[frameIndex]};
        auto drawAll = [&](VkPipeline pipe, float modeValue) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 3, sets, 0, nullptr);
            for (uint32_t i = 0; i < m_nodes.size(); ++i) {
                const Node& n = m_nodes[i];
                const uint32_t meshIdx = n.skin >= 0 ? m_models->previewSkinMesh(n.skin) : n.mesh;
                const ModelMeshGPU* m = m_models->meshAt(meshIdx);
                if (!m || m->indexCount == 0 || !m->vertexBuffer) continue;
                // Nível escolhido; o que a malha não tiver cai no anterior (como no jogo).
                VkBuffer vb = m->vertexBuffer, ib = m->indexBuffer;
                uint32_t count = m->indexCount;
                auto useLo = [&] {
                    if (!m->loIndexCount) return;
                    ib = m->loIndexBuffer;
                    count = m->loIndexCount;
                    if (m->loVertexBuffer) vb = m->loVertexBuffer;
                };
                if (lod == Lod::Near) useLo();
                else if (lod == Lod::Far) {
                    useLo();
                    if (m->farIndexCount) {
                        ib = m->farIndexBuffer;
                        count = m->farIndexCount;
                        vb = m->farVertexBuffer ? m->farVertexBuffer : m->vertexBuffer;
                    }
                } else if (lod == Lod::Shadow) {
                    if (m->shIndexCount) {
                        ib = m->shIndexBuffer;
                        count = m->shIndexCount;
                    } else {
                        useLo();
                    }
                }
                inst[i].model = n.matrix;
                inst[i].uvTranslateRot = Vec4(0.0f);
                inst[i].uvScaleDisp = Vec4(1.0f, 1.0f, 0.0f, 0.0f); // sem vento nem deslocamento
                PreviewPush push{};
                push.viewProjection = viewProj;
                push.model = n.matrix;
                push.alpha = 1.0f;
                push.metallicScale = m->metallicScale;
                push.roughnessScale = m->roughnessScale;
                push.eye = Vec4(eye, 1.0f);
                push.uvScale = Vec4(1.0f, 1.0f, m->uvPerUnit, 0.0f);
                push.lightMode = Vec4(-toLight, modeValue);
                push.wireCut = Vec4(0.95f, 0.55f, 0.20f, alphaCut ? 0.5f : 0.0f);
                vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);
                const VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, count, 1, 0, 0, i);
            }
        };
        drawAll(m_fillPipeline, static_cast<float>(mode));
        if (wireframe && m_linePipeline) drawAll(m_linePipeline, 4.0f);
        vmaFlushAllocation(m_ctx->allocator(), m_instAlloc, VkDeviceSize(frameIndex) * kMaxNodes * sizeof(GpuInstance),
                           VkDeviceSize(m_nodes.size()) * sizeof(GpuInstance));
    }
    vkc::cmdEndRendering(cmd);
    barrier(m_ctx, cmd, m_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_colorReadable = true;
}

} // namespace eruption
