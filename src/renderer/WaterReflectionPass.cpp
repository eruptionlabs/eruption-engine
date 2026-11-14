#include "renderer/WaterReflectionPass.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

#include <algorithm>

namespace eruption {

namespace {

// Push do composite: 6 vec4 = 96 B (limite de 128).
struct ReflectionCompositePush {
    Vec4 toSun;       // xyz = direcao ate' o sol, w = intensidade
    Vec4 sunColor;    // rgb, w = escala do difuso direto
    Vec4 ambientSky;  // rgb = zenite, w = intensidade do GI
    Vec4 ambientGround; // rgb, w = piso do hemisferio
    Vec4 skyHorizon;  // rgb, w = escala do emissivo
    Vec4 clouds;      // x = 1 com nuvens no binding 3, 0 = fallback (nao le')
};
static_assert(sizeof(ReflectionCompositePush) <= 128, "push do reflexo espelhado passa de 128 bytes");

constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

} // namespace

bool WaterReflectionPass::init(VulkanContext* ctx, BindlessDescriptor* bindless) {
    m_ctx = ctx;
    m_bindless = bindless;
    return true;
}

void WaterReflectionPass::shutdown() {
    if (!m_ctx) return;
    destroyTargets();
    VkDevice dev = m_ctx->device();
    if (m_pipeline) { vkDestroyPipeline(dev, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_layout) { vkDestroyPipelineLayout(dev, m_layout, nullptr); m_layout = VK_NULL_HANDLE; }
    if (m_pool) {
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
        for (auto& s : m_sets) s = VK_NULL_HANDLE;
    }
    if (m_setLayout) { vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr); m_setLayout = VK_NULL_HANDLE; }
    if (m_sampler) { vkDestroySampler(dev, m_sampler, nullptr); m_sampler = VK_NULL_HANDLE; }
    if (m_nearest) { vkDestroySampler(dev, m_nearest, nullptr); m_nearest = VK_NULL_HANDLE; }
}

void WaterReflectionPass::destroyTargets() {
    if (!m_ctx) return;
    if (m_slot != 0) { m_bindless->freeSlot(m_slot); m_slot = 0; }
    if (m_colorView) { vkDestroyImageView(m_ctx->device(), m_colorView, nullptr); m_colorView = VK_NULL_HANDLE; }
    if (m_colorImage) {
        vmaDestroyImage(m_ctx->allocator(), m_colorImage, m_colorAlloc);
        m_colorImage = VK_NULL_HANDLE;
        m_colorAlloc = VK_NULL_HANDLE;
    }
    if (m_gbufferLive) { m_gbuffer.shutdown(); m_gbufferLive = false; }
    m_width = m_height = 0;
}

bool WaterReflectionPass::createPipeline() {
    if (m_pipeline != VK_NULL_HANDLE) return true;
    VkDevice dev = m_ctx->device();

    // 0 = albedo (a = AO, 0 onde nada foi desenhado), 1 = normal, 2 = emissivo,
    // 3 = nuvens da camera espelhada (pre-multiplicado, resolucao do acumulo).
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 4;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    constexpr uint32_t kSets = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kSets};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout setLayouts[kSets];
    for (auto& l : setLayouts) l = m_setLayout;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts = setLayouts;
    if (vkAllocateDescriptorSets(dev, &ai, m_sets) != VK_SUCCESS) return false;

    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ReflectionCompositePush)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS) return false;

    auto vertCode = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("water/reflection_composite.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_WARN("[WATER] reflection_composite.frag.spv ausente: reflexo espelhado desligado");
        return false;
    }
    auto makeModule = [dev](const std::vector<uint32_t>& code) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * sizeof(uint32_t);
        ci.pCode = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &mod);
        return mod;
    };
    VkShaderModule vMod = makeModule(vertCode);
    VkShaderModule fMod = makeModule(fragCode);
    if (vMod != VK_NULL_HANDLE && fMod != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vMod;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fMod;
        st[1].pName = "main";
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        PipelineBuilder builder;
        builder.setShaderStages({st[0], st[1]})
               .setLayout(m_layout)
               .setColorAttachmentFormats({kColorFormat})
               .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
               .setPolygonMode(VK_POLYGON_MODE_FILL)
               .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
               .setBlendState({blend})
               .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR});
        m_pipeline = builder.build(dev);
    }
    if (vMod) vkDestroyShaderModule(dev, vMod, nullptr);
    if (fMod) vkDestroyShaderModule(dev, fMod, nullptr);
    if (m_pipeline == VK_NULL_HANDLE) {
        ERUPTION_LOG_WARN("[WATER] reflexo espelhado: falha ao criar o composite");
        return false;
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    vkCreateSampler(dev, &si, nullptr, &m_sampler);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(dev, &si, nullptr, &m_nearest);
    return m_sampler != VK_NULL_HANDLE && m_nearest != VK_NULL_HANDLE;
}

bool WaterReflectionPass::ensureSize(uint32_t sceneW, uint32_t sceneH, uint32_t divisor) {
    if (!m_ctx) return false;
    divisor = std::max(divisor, 1u);
    const uint32_t w = std::max(16u, sceneW / divisor), h = std::max(16u, sceneH / divisor);
    if (m_colorImage != VK_NULL_HANDLE && w == m_width && h == m_height) return true;
    if (!createPipeline()) return false;
    // Troca de tamanho com o passe ja' em uso (preset novo, render scale):
    // quadros em voo ainda leem as views velhas. Raro; espera a GPU.
    if (m_colorImage != VK_NULL_HANDLE) vkDeviceWaitIdle(m_ctx->device());
    destroyTargets();
    m_divisor = divisor;

    if (!m_gbuffer.init(m_ctx, w, h)) {
        m_gbuffer.shutdown();
        ERUPTION_LOG_WARN("[WATER] reflexo espelhado: falha ao criar o G-buffer %ux%u", w, h);
        return false;
    }
    m_gbufferLive = true;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.format = kColorFormat;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &ii, &aci, &m_colorImage, &m_colorAlloc, nullptr) != VK_SUCCESS) {
        m_colorImage = VK_NULL_HANDLE;
        destroyTargets();
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_colorImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kColorFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkc::createImageView(m_ctx->device(), &vi, nullptr, &m_colorView) != VK_SUCCESS) {
        destroyTargets();
        return false;
    }
    // O slot fica registrado o tempo todo: a imagem tem de estar num layout
    // legivel mesmo antes do primeiro passe.
    m_ctx->transitionNow(m_colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    m_slot = m_bindless->allocateSlot();
    m_bindless->updateTexture(m_slot, m_colorView, m_sampler, true);
    m_bindless->flushUpdates();

    // Composite le' o G-buffer espelhado por texelFetch (1:1 com a saida).
    // Binding 3 comeca no fallback (o emissivo, nunca lido com clouds.x = 0):
    // todo descritor que o pipeline usa fica escrito mesmo sem nuvens.
    VkDescriptorImageInfo imgs[4]{};
    imgs[0] = {m_nearest, m_gbuffer.albedoView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[1] = {m_nearest, m_gbuffer.normalView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[2] = {m_nearest, m_gbuffer.emissiveView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[3] = {m_sampler, m_gbuffer.emissiveView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        VkWriteDescriptorSet wr[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = m_sets[f];
            wr[i].dstBinding = i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &imgs[i];
        }
        vkUpdateDescriptorSets(m_ctx->device(), 4, wr, 0, nullptr);
    }

    m_width = w;
    m_height = h;
    ERUPTION_LOG_INFO("[WATER] reflexo espelhado: %ux%u (1/%u da cena)", w, h, divisor);
    return true;
}

void WaterReflectionPass::resize(uint32_t sceneW, uint32_t sceneH) {
    if (m_colorImage == VK_NULL_HANDLE) return;
    ensureSize(sceneW, sceneH, m_divisor);
}

void WaterReflectionPass::composite(VkCommandBuffer cmd, const Lighting& light, VkImageView cloudView) {
    m_gbuffer.transitionToRead(cmd);
    // Nuvens: o set deste quadro aponta para o acumulo (ou para o fallback).
    // A GPU ja' terminou o uso anterior deste set (cerca do quadro em voo).
    // Reescrito todo quadro (um descritor): o acumulo e' recriado quando o
    // tamanho da cena muda, e uma view nova pode vir com o mesmo handle.
    const VkDescriptorSet set = m_sets[m_ctx->currentFrame()];
    {
        const VkImageView cloudBind = cloudView != VK_NULL_HANDLE ? cloudView : m_gbuffer.emissiveView();
        VkDescriptorImageInfo img{m_sampler, cloudBind, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = set;
        wr.dstBinding = 3;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &img;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &wr, 0, nullptr);
    }
    // A agua do quadro anterior leu a imagem; o conteudo e' todo reescrito.
    m_ctx->cmdImageBarrier(cmd, m_colorImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = m_colorView;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // o triangulo cobre tudo
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    m_ctx->cmdBeginRendering(cmd, {att}, nullptr, nullptr, VkExtent2D{m_width, m_height});
    VkViewport vp{0.0f, 0.0f, float(m_width), float(m_height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {m_width, m_height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
    ReflectionCompositePush pc{};
    pc.toSun = Vec4(light.toSun, light.sunIntensity);
    pc.sunColor = Vec4(light.sunColor, light.directScale);
    pc.ambientSky = Vec4(light.ambientSky, light.giIntensity);
    pc.ambientGround = Vec4(light.ambientGround, light.hemiFloor);
    pc.skyHorizon = Vec4(light.skyHorizon, light.emissiveScale);
    pc.clouds = Vec4(cloudView != VK_NULL_HANDLE ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    m_ctx->cmdEndRendering(cmd);

    m_ctx->cmdImageBarrier(cmd, m_colorImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

} // namespace eruption
