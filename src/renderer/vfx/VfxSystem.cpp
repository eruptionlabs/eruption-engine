#include "renderer/vfx/VfxSystem.hpp"

#include "core/Logger.hpp"
#include "math/Camera.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/VkCompat.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace eruption {

namespace {

// Contadores (uint) - precisa bater com VFXC_* em vfx_common.glsl.
constexpr uint32_t kCDead = 2;
constexpr uint32_t kCRender = 3;
constexpr uint32_t kCDispatch = 8;
constexpr uint32_t kCDraw = 12;
constexpr uint32_t kCSortDispatch = 20;
constexpr uint32_t kCounterCount = 28;
constexpr uint32_t kBindings = 13;

struct DrawPush {
    uint32_t listOffset;
    float depthScale;
};
constexpr uint32_t kParticleBytes = 48;

uint32_t hash32(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

VkShaderModule loadModule(VkDevice dev, const char* path) {
    const std::vector<uint32_t> code = ShaderCompiler::loadSPIRV(path);
    if (code.empty()) {
        ERUPTION_LOG_ERROR("VFX: shader %s nao encontrado", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                VkAccessFlags dstAccess) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, srcAccess, dstAccess};
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

} // namespace

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

bool VfxSystem::createBuf(Buf& b, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage mem, bool map) {
    b.size = size;
    if (!m_ctx->createBuffer(size, usage, mem, b.buf, b.alloc)) return false;
    if (map && vmaMapMemory(m_ctx->allocator(), b.alloc, &b.mapped) != VK_SUCCESS) return false;
    return true;
}

void VfxSystem::destroyBuf(Buf& b) {
    if (b.mapped) vmaUnmapMemory(m_ctx->allocator(), b.alloc);
    if (b.buf) vmaDestroyBuffer(m_ctx->allocator(), b.buf, b.alloc);
    b = Buf{};
}

bool VfxSystem::createBuffers() {
    const VkDeviceSize cap = m_capacity;
    // TRANSFER_SRC: o dump de depuracao (ERUPTION_DEBUG_VFX) le' daqui.
    const VkBufferUsageFlags ssbo =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bool ok = createBuf(m_particles, cap * kParticleBytes, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_dead, cap * 4, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_alive, cap * 8, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_render, cap * 16, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_sort, 2 * 512 * 4, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_counters, kCounterCount * 4,
                        ssbo | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, false);
    // Por frame em voo: uma area de envio (CPU) com emissores, pedidos e
    // parametros, copiada para a GPU no comeco da simulacao. A simulacao le'
    // o emissor por particula: da memoria da placa, nao do outro lado do PCIe.
    for (uint32_t f = 0; ok && f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        ok = createBuf(m_emitterBuf[f], VkDeviceSize(kMaxSlots) * sizeof(VfxEmitterGpu),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, true) &&
             createBuf(m_spawnBuf[f], VkDeviceSize(kMaxRequests) * sizeof(VfxSpawnRequestGpu),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, true) &&
             createBuf(m_frameBuf[f], sizeof(VfxFrameParamsGpu), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VMA_MEMORY_USAGE_CPU_ONLY, true) &&
             createBuf(m_readback[f], kCounterCount * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VMA_MEMORY_USAGE_GPU_TO_CPU, true);
        m_readbackPending[f] = false;
    }
    for (uint32_t f = 0; ok && f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f)
        ok = createBuf(m_graphStage[f], VkDeviceSize(kGraphCodeWords) * 4 + VkDeviceSize(kGraphConsts) * 16,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, true);
    ok = ok && createBuf(m_graphCode, VkDeviceSize(kGraphCodeWords) * 4, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
         createBuf(m_graphConst, VkDeviceSize(kGraphConsts) * 16, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false);
    m_graphDirty = !m_graphCodeCpu.empty();
    ok = ok && createBuf(m_emitterDev, VkDeviceSize(kMaxSlots) * sizeof(VfxEmitterGpu), ssbo,
                         VMA_MEMORY_USAGE_GPU_ONLY, false) &&
         createBuf(m_spawnDev, VkDeviceSize(kMaxRequests) * sizeof(VfxSpawnRequestGpu), ssbo,
                   VMA_MEMORY_USAGE_GPU_ONLY, false) &&
         createBuf(m_frameDev, sizeof(VfxFrameParamsGpu),
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
                   false);
    if (!ok) {
        ERUPTION_LOG_ERROR("VFX: falha ao criar buffers (%u particulas)", m_capacity);
        return false;
    }

    // Pool inicial: todas mortas (lista 0..cap-1), contadores zerados.
    std::vector<uint32_t> dead(m_capacity);
    std::iota(dead.begin(), dead.end(), 0u);
    uint32_t counters[kCounterCount] = {};
    counters[kCDead] = m_capacity;
    Buf staging;
    const VkDeviceSize deadBytes = VkDeviceSize(m_capacity) * 4;
    if (!createBuf(staging, deadBytes + sizeof(counters), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_CPU_ONLY, true))
        return false;
    std::memcpy(staging.mapped, dead.data(), size_t(deadBytes));
    std::memcpy(static_cast<char*>(staging.mapped) + deadBytes, counters, sizeof(counters));
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        const VkBufferCopy c0{0, 0, deadBytes};
        vkCmdCopyBuffer(cmd, staging.buf, m_dead.buf, 1, &c0);
        const VkBufferCopy c1{deadBytes, 0, sizeof(counters)};
        vkCmdCopyBuffer(cmd, staging.buf, m_counters.buf, 1, &c1);
    });
    destroyBuf(staging);
    m_cur = 0;
    m_setBuffersWritten.fill(false);
    return true;
}

void VfxSystem::destroyBuffers() {
    for (Buf* b : {&m_particles, &m_dead, &m_alive, &m_render, &m_sort, &m_counters, &m_emitterDev, &m_spawnDev,
                   &m_frameDev, &m_graphCode, &m_graphConst})
        destroyBuf(*b);
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        destroyBuf(m_emitterBuf[f]);
        destroyBuf(m_spawnBuf[f]);
        destroyBuf(m_frameBuf[f]);
        destroyBuf(m_readback[f]);
        destroyBuf(m_graphStage[f]);
    }
}

// ---------------------------------------------------------------------------
// Pipelines e descritores
// ---------------------------------------------------------------------------

bool VfxSystem::createDescriptors() {
    VkDevice dev = m_ctx->device();
    VkDescriptorSetLayoutBinding b[kBindings]{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    b[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = kBindings;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    const uint32_t n = VulkanContext::MAX_FRAMES_IN_FLIGHT * 2;
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10 * n},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * n}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = n;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, VulkanContext::MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai, m_sets.data()) != VK_SUCCESS) return false;
    if (vkAllocateDescriptorSets(dev, &ai, m_drawSets.data()) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &m_depthSampler) != VK_SUCCESS) return false;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    return vkCreateSampler(dev, &si, nullptr, &m_linearSampler) == VK_SUCCESS;
}

void VfxSystem::writeDescriptors(uint32_t frame, VkImageView depthView) {
    if (m_setBuffersWritten[frame] && m_setDepth[frame] == depthView && m_setLow[frame] == m_lowView) return;
    VkDescriptorBufferInfo bi[8] = {
        {m_particles.buf, 0, VK_WHOLE_SIZE}, {m_dead.buf, 0, VK_WHOLE_SIZE},    {m_alive.buf, 0, VK_WHOLE_SIZE},
        {m_counters.buf, 0, VK_WHOLE_SIZE},  {m_render.buf, 0, VK_WHOLE_SIZE},  {m_emitterDev.buf, 0, VK_WHOLE_SIZE},
        {m_spawnDev.buf, 0, VK_WHOLE_SIZE},  {m_frameDev.buf, 0, VK_WHOLE_SIZE}};
    const VkDescriptorBufferInfo sortInfo{m_sort.buf, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo graphCodeInfo{m_graphCode.buf, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo graphConstInfo{m_graphConst.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo ii[2] = {{m_depthSampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {m_depthSampler, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    // Sem alvo de meia resolucao ainda: a profundidade ocupa o binding (nunca
    // e' lida enquanto nao houver emissor lowRes).
    const VkDescriptorImageInfo lowInfo{m_linearSampler, m_lowView ? m_lowView : depthView,
                                        m_lowView ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2 * kBindings]{};
    for (uint32_t s = 0; s < 2; ++s) {
        for (uint32_t i = 0; i < kBindings; ++i) {
            VkWriteDescriptorSet& x = w[s * kBindings + i];
            x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.dstSet = s == 0 ? m_sets[frame] : m_drawSets[frame];
            x.dstBinding = i;
            x.descriptorCount = 1;
            x.descriptorType = i == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i < 8) x.pBufferInfo = &bi[i];
        }
        w[s * kBindings + 8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[s * kBindings + 8].pImageInfo = &ii[s];
        w[s * kBindings + 9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[s * kBindings + 9].pImageInfo = &lowInfo;
        w[s * kBindings + 10].pBufferInfo = &sortInfo;
        w[s * kBindings + 11].pBufferInfo = &graphCodeInfo;
        w[s * kBindings + 12].pBufferInfo = &graphConstInfo;
    }
    vkUpdateDescriptorSets(m_ctx->device(), 2 * kBindings, w, 0, nullptr);
    m_setBuffersWritten[frame] = true;
    m_setDepth[frame] = depthView;
    m_setLow[frame] = m_lowView;
}

bool VfxSystem::createPipelines() {
    VkDevice dev = m_ctx->device();
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_setLayout;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_compositeLayout) != VK_SUCCESS) return false;
    const VkPushConstantRange computePush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &computePush;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_computeLayout) != VK_SUCCESS) return false;
    VkDescriptorSetLayout drawSets[3] = {m_bindless->layout(), m_frameUboLayout, m_setLayout};
    const VkPushConstantRange drawPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPush)};
    pl.setLayoutCount = 3;
    pl.pSetLayouts = drawSets;
    pl.pPushConstantRanges = &drawPush;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_drawLayout) != VK_SUCCESS) return false;

    auto compute = [&](const char* path, VkPipeline& out) {
        VkShaderModule m = loadModule(dev, path);
        if (!m) return false;
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = m_computeLayout;
        const VkResult r = vkCreateComputePipelines(dev, m_ctx->pipelineCache(), 1, &ci, nullptr, &out);
        vkDestroyShaderModule(dev, m, nullptr);
        return r == VK_SUCCESS;
    };
    if (!compute("vfx/vfx_emit.comp.spv", m_emitPipe) || !compute("vfx/vfx_args.comp.spv", m_argsPipe) ||
        !compute("vfx/vfx_sim.comp.spv", m_simPipe) || !compute("vfx/vfx_finish.comp.spv", m_finishPipe) ||
        !compute("vfx/vfx_sort_hist.comp.spv", m_sortHistPipe) ||
        !compute("vfx/vfx_sort_scan.comp.spv", m_sortScanPipe) ||
        !compute("vfx/vfx_sort_scatter.comp.spv", m_sortScatterPipe))
        return false;

    VkShaderModule vs = loadModule(dev, "vfx/vfx_particle.vert.spv");
    VkShaderModule fs = loadModule(dev, "vfx/vfx_particle.frag.spv");
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
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Teste de profundidade sim (o que esta' atras da parede nem chega ao
    // pixel), escrita nao (transparente).
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // Mistura premultiplicada: cobre aditiva (alfa 0), premultiplicada e alfa.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    const VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &colorFormat;
    ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
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
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = m_drawLayout;
    VkResult r = vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &m_drawPipe);
    // Meia resolucao: mesmo shader, sem attachment de profundidade (o teste
    // vira amostra da profundidade cheia no fragment).
    if (r == VK_SUCCESS) {
        ri.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        ds.depthTestEnable = VK_FALSE;
        r = vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &m_drawLowPipe);
    }
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if (r != VK_SUCCESS) return false;

    // Composicao: triangulo de tela cheia, mesma mistura premultiplicada.
    vs = loadModule(dev, "vfx/vfx_composite.vert.spv");
    fs = loadModule(dev, "vfx/vfx_composite.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(dev, fs, nullptr);
        return false;
    }
    st[0].module = vs;
    st[1].module = fs;
    ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    gi.layout = m_compositeLayout;
    r = vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &m_compositePipe);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    return r == VK_SUCCESS;
}

bool VfxSystem::init(const InitInfo& info) {
    m_ctx = info.ctx;
    m_bindless = info.bindless;
    m_frameUboLayout = info.frameUboLayout;
    m_frameUboSet = info.frameUboSet;
    m_resolveTexture = info.resolveTexture;
    m_resolveEffectPath = info.resolveEffectPath;
    // setBudget() pode ter vindo antes (preset aplicado antes do init).
    m_capacity = std::max<uint32_t>(m_capacity ? m_capacity : info.capacity, 1024u);
    m_slotData.assign(kMaxSlots, VfxEmitterGpu{});
    m_slotGen.assign(kMaxSlots, 0);
    m_freeSlots.clear();
    for (uint32_t s = kMaxSlots; s-- > 0;) m_freeSlots.push_back(s);
    for (auto& d : m_slotData) d.flags = glm::uvec4(1u << 12, 0u, 0xFFFFFFFFu, 0u); // morto
    if (!createDescriptors() || !createPipelines() || !createBuffers()) {
        ERUPTION_LOG_ERROR("VFX: inicializacao falhou; efeitos desligados");
        shutdown();
        return false;
    }
    ERUPTION_LOG_INFO("VFX: pool de %u particulas (%.1f MB)", m_capacity,
                      double(m_capacity) * (kParticleBytes + 16) / (1024.0 * 1024.0));
    m_ready = true;
    return true;
}

void VfxSystem::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    destroyBuffers();
    for (VkPipeline* p : {&m_emitPipe, &m_argsPipe, &m_simPipe, &m_finishPipe, &m_drawPipe, &m_drawLowPipe,
                          &m_compositePipe, &m_sortHistPipe, &m_sortScanPipe, &m_sortScatterPipe})
        if (*p) vkDestroyPipeline(dev, *p, nullptr), *p = VK_NULL_HANDLE;
    if (m_lowView) vkDestroyImageView(dev, m_lowView, nullptr);
    if (m_lowImage) vmaDestroyImage(m_ctx->allocator(), m_lowImage, m_lowAlloc);
    m_lowView = VK_NULL_HANDLE;
    m_lowImage = VK_NULL_HANDLE;
    m_lowExtent = {0, 0};
    if (m_compositeLayout) vkDestroyPipelineLayout(dev, m_compositeLayout, nullptr);
    if (m_linearSampler) vkDestroySampler(dev, m_linearSampler, nullptr);
    m_compositeLayout = VK_NULL_HANDLE;
    m_linearSampler = VK_NULL_HANDLE;
    if (m_computeLayout) vkDestroyPipelineLayout(dev, m_computeLayout, nullptr);
    if (m_drawLayout) vkDestroyPipelineLayout(dev, m_drawLayout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_depthSampler) vkDestroySampler(dev, m_depthSampler, nullptr);
    m_computeLayout = m_drawLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_depthSampler = VK_NULL_HANDLE;
    m_instances.clear();
    m_ready = false;
    m_ctx = nullptr;
}

void VfxSystem::setBudget(uint32_t capacity, float spawnScale) {
    m_spawnScale = std::clamp(spawnScale, 0.0f, 4.0f);
    capacity = std::clamp<uint32_t>(capacity, 1024u, 4u << 20);
    if (!m_ready) {
        m_capacity = capacity;
        return;
    }
    if (capacity == m_capacity) return;
    // Pool novo: as particulas vivas se perdem (os efeitos seguem nascendo).
    m_ctx->waitIdle();
    destroyBuffers();
    m_capacity = capacity;
    if (!createBuffers()) {
        ERUPTION_LOG_ERROR("VFX: sem memoria para %u particulas", capacity);
        m_ready = false;
        return;
    }
    for (Instance& inst : m_instances)
        for (EmitterState& es : inst.emitters) {
            es.liveChunks.clear();
            es.liveEstimate = 0;
        }
}

// ---------------------------------------------------------------------------
// Efeitos e instancias
// ---------------------------------------------------------------------------

const VfxEffectDesc* VfxSystem::effect(const std::string& name) {
    auto it = m_effects.find(name);
    if (it != m_effects.end()) return it->second.get();
    const std::string path = m_resolveEffectPath ? m_resolveEffectPath(name) : name;
    auto fx = std::make_shared<VfxEffectDesc>();
    std::string err;
    if (path.empty() || !loadVfxEffect(path, *fx, &err)) {
        ERUPTION_LOG_WARN("VFX: efeito '%s' indisponivel (%s)", name.c_str(), err.empty() ? "sem arquivo" : err.c_str());
        return nullptr;
    }
    if (fx->name.empty()) fx->name = name;
    m_effects[name] = fx;
    return fx.get();
}

void VfxSystem::setEffect(const std::string& name, const VfxEffectDesc& desc) {
    auto fx = std::make_shared<VfxEffectDesc>(desc);
    m_effects[name] = fx;
    for (Instance& inst : m_instances) {
        if (inst.effect != name) continue;
        // Emissores a mais ou a menos: ajusta os slots sem reiniciar o efeito.
        while (inst.emitters.size() > fx->emitters.size()) {
            freeSlot(inst.emitters.back().slot);
            inst.emitters.pop_back();
        }
        while (inst.emitters.size() < fx->emitters.size()) {
            const uint32_t s = allocSlot();
            if (s == UINT32_MAX) break;
            EmitterState es;
            es.slot = s;
            inst.emitters.push_back(es);
        }
        inst.desc = fx;
    }
}

void VfxSystem::forgetEffect(const std::string& name) { m_effects.erase(name); }

uint32_t VfxSystem::allocSlot() {
    if (m_freeSlots.empty()) return UINT32_MAX;
    const uint32_t s = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_slotGen[s] = static_cast<uint16_t>(m_slotGen[s] + 1);
    m_slotHighWater = std::max(m_slotHighWater, s + 1);
    return s;
}

void VfxSystem::freeSlot(uint32_t slot) {
    if (slot >= kMaxSlots) return;
    // O slot sobe como morto: particulas que ainda apontam para ele morrem
    // na proxima simulacao (geracao ou bit KILLED).
    m_slotData[slot].flags.x = 1u << 12;
    m_slotGen[slot] = static_cast<uint16_t>(m_slotGen[slot] + 1);
    m_slotData[slot].flags.y = m_slotGen[slot];
    m_freeSlots.push_back(slot);
}

uint32_t VfxSystem::spawn(const std::string& effectName, const Mat4& transform) {
    if (!m_ready) return 0;
    const VfxEffectDesc* d = effect(effectName);
    if (!d) return 0;
    Instance inst;
    inst.id = m_nextId++;
    inst.effect = effectName;
    inst.desc = m_effects[effectName];
    // Escala do mundo entra na transformacao (metros -> unidades).
    inst.transform = transform * glm::scale(Mat4(1.0f), Vec3(m_unitsPerMeter));
    inst.prevPos = Vec3(transform[3]);
    for (size_t i = 0; i < d->emitters.size(); ++i) {
        const uint32_t s = allocSlot();
        if (s == UINT32_MAX) {
            for (EmitterState& es : inst.emitters) freeSlot(es.slot);
            ERUPTION_LOG_WARN("VFX: sem slot de emissor para '%s' (limite %u)", effectName.c_str(), kMaxSlots);
            return 0;
        }
        EmitterState es;
        es.slot = s;
        inst.emitters.push_back(es);
    }
    m_instances.push_back(std::move(inst));
    m_active = true;
    m_idleTime = 0.0f;
    return m_instances.back().id;
}

void VfxSystem::setTransform(uint32_t id, const Mat4& transform) {
    for (Instance& inst : m_instances)
        if (inst.id == id) inst.transform = transform * glm::scale(Mat4(1.0f), Vec3(m_unitsPerMeter));
}

void VfxSystem::stop(uint32_t id, bool immediate) {
    for (Instance& inst : m_instances) {
        if (inst.id != id) continue;
        if (immediate) inst.killed = true;
        else if (inst.stopTime < 0.0f) inst.stopTime = inst.time;
    }
}

void VfxSystem::restart(uint32_t id) {
    for (Instance& inst : m_instances) {
        if (inst.id != id) continue;
        inst.time = 0.0f;
        inst.stopTime = -1.0f;
        inst.killed = false;
        for (EmitterState& es : inst.emitters) {
            es.spawnAccum = 0.0f;
            es.burstTimer = 0.0f;
            es.burstsDone = 0;
        }
    }
}

bool VfxSystem::isPlaying(uint32_t id) const {
    for (const Instance& inst : m_instances)
        if (inst.id == id) return !inst.killed;
    return false;
}

float VfxSystem::instanceTime(uint32_t id) const {
    for (const Instance& inst : m_instances)
        if (inst.id == id) return inst.time;
    return 0.0f;
}

void VfxSystem::clear() {
    for (Instance& inst : m_instances)
        for (EmitterState& es : inst.emitters) freeSlot(es.slot);
    m_instances.clear();
}

const VfxGraphCompiled& VfxSystem::graphFor(const VfxGraph& g) {
    // Chave = o grafo em texto (o que muda o programa: nos, valores, ligacoes).
    std::string key;
    key.reserve(g.nodes.size() * 48 + g.links.size() * 16);
    char buf[160];
    for (const auto& n : g.nodes) {
        std::snprintf(buf, sizeof(buf), "%d:%s:%.6g,%.6g,%.6g,%.6g;", n.id, n.op.c_str(), n.value.x, n.value.y,
                      n.value.z, n.value.w);
        key += buf;
    }
    for (const auto& l : g.links) {
        std::snprintf(buf, sizeof(buf), "%d.%d>%d.%d;", l.fromNode, l.fromPin, l.toNode, l.toPin);
        key += buf;
    }
    auto it = m_graphLib.find(key);
    if (it != m_graphLib.end()) return it->second;
    VfxGraphCompiled c = compileVfxGraph(g, uint32_t(m_graphCodeCpu.size()), uint32_t(m_graphConstCpu.size()));
    if (!c.error.empty()) {
        ERUPTION_LOG_WARN("[VFX] grafo com erro: %s", c.error.c_str());
    } else if (m_graphCodeCpu.size() + c.code.size() <= kGraphCodeWords &&
               m_graphConstCpu.size() + c.constants.size() <= kGraphConsts) {
        m_graphCodeCpu.insert(m_graphCodeCpu.end(), c.code.begin(), c.code.end());
        m_graphConstCpu.insert(m_graphConstCpu.end(), c.constants.begin(), c.constants.end());
        m_graphDirty = true;
    } else {
        c = VfxGraphCompiled{}; // sem espaco neste frame: sem grafo ate' a biblioteca recomecar
    }
    return m_graphLib[key] = std::move(c);
}

const VfxTextureInfo& VfxSystem::texture(const std::string& path, uint32_t cols, uint32_t rows) {
    static const VfxTextureInfo kDisc{};
    if (path.empty() || !m_resolveTexture) return kDisc;
    const std::string key = path + "#" + std::to_string(cols) + "x" + std::to_string(rows);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) return it->second;
    return m_textures[key] = m_resolveTexture(path, cols, rows);
}

void VfxSystem::fillEmitterGpu(const Instance& inst, const VfxEmitterDesc& e, uint32_t slot, VfxEmitterGpu& o) {
    const Mat4& t = inst.transform;
    const float scale = std::max(glm::length(Vec3(t[0])), 1e-4f);
    const Vec3 c0 = Vec3(t[0]) / scale;
    const Vec3 c1 = Vec3(t[1]) / std::max(glm::length(Vec3(t[1])), 1e-4f);
    const Vec3 c2 = Vec3(t[2]) / std::max(glm::length(Vec3(t[2])), 1e-4f);
    const Vec3 pos = Vec3(t[3]);
    o.posTime = Vec4(pos, inst.time);
    o.prevPos = Vec4(inst.firstFrame ? pos : inst.prevPos, 0.0f);
    o.rot0 = Vec4(c0, scale);
    o.rot1 = Vec4(c1, 0.0f);
    o.rot2 = Vec4(c2, 0.0f);
    o.velInherit = Vec4(inst.velocity, e.inheritVelocity);
    o.shape = Vec4(e.shapeSize, glm::radians(std::clamp(e.coneAngle, 0.0f, 179.0f)) * 0.5f);
    const Vec3 dir = glm::length(e.direction) > 1e-6f ? glm::normalize(e.direction) : Vec3(0, 1, 0);
    uint32_t flags = (uint32_t(e.shape) & 7u) | (e.surfaceOnly ? 1u << 3 : 0u) | ((uint32_t(e.facing) & 3u) << 4) |
                     ((uint32_t(e.blend) & 3u) << 6) | (e.flipBlend ? 1u << 8 : 0u) |
                     (e.flipRandomStart || e.flipMode == VfxFlipMode::Random ? 1u << 9 : 0u) | (e.collidePlane ? 1u << 10 : 0u) |
                     (e.collideDepth ? 1u << 11 : 0u);
    if (inst.killed) flags |= 1u << 12;
    if (e.lowRes) flags |= 1u << 14;
    const VfxTextureInfo& tex = texture(e.texture, e.flipCols, e.flipRows);
    o.flags = glm::uvec4(flags, m_slotGen[slot], tex.slot, (e.flipCols & 0xFFu) | ((e.flipRows & 0xFFu) << 8));
    for (int k = 0; k < 4; ++k)
        o.cutout[k] = glm::packHalf2x16(glm::vec2(tex.cutout[2 * k], tex.cutout[2 * k + 1]));
    o.graphA = o.graphB = glm::uvec4(0u);
    if (!e.graph.empty()) {
        const VfxGraphCompiled& g = graphFor(e.graph);
        o.graphA = glm::uvec4(g.spawnVelocity.offset, g.spawnVelocity.length, g.force.offset, g.force.length);
        o.graphB = glm::uvec4(g.color.offset, g.color.length, g.size.offset, g.size.length);
    }
    o.life = Vec4(e.lifetime.x, std::max(e.lifetime.x, e.lifetime.y), e.speed.x, e.speed.y);
    o.dirSpread = Vec4(dir, e.spread);
    o.sizeRot = Vec4(e.size.x, e.size.y, glm::radians(e.rotation.x), glm::radians(e.rotation.y));
    o.angVel = Vec4(glm::radians(e.angularVelocity.x), glm::radians(e.angularVelocity.y), e.stretch, e.softDistance);
    o.gravityDrag = Vec4(e.gravity, e.drag);
    o.noise = Vec4(e.noiseStrength, e.noiseFrequency, e.noiseSpeed, e.bounce);
    o.collide = Vec4(e.planeY, e.friction, e.collisionLifeLoss, 0.0f);
    o.render = Vec4(e.emissive, e.lit, e.maxScreenSize, e.nearFade);
    o.flip = Vec4(e.flipFps, float(e.flipCols * e.flipRows), float(uint32_t(e.flipMode)), 0.0f);
    for (int i = 0; i < 16; ++i) {
        const Vec4 c = glm::clamp(e.color.sample(float(i) / 15.0f), Vec4(0.0f), Vec4(1.0f));
        o.colorLut[i] = glm::packUnorm4x8(c);
    }
    for (int i = 0; i < 8; ++i)
        o.sizeLut[i] = glm::packHalf2x16(
            glm::vec2(e.sizeOverLife.sample(float(2 * i) / 15.0f), e.sizeOverLife.sample(float(2 * i + 1) / 15.0f)));
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void VfxSystem::update(float dt, const Camera& camera, VkExtent2D renderSize) {
    if (!m_ready) return;
    const uint32_t frame = m_ctx->currentFrame();
    m_renderSize = renderSize;

    // Estatistica com atraso: os contadores que este slot de frame copiou da
    // ultima vez (a cerca dele ja' foi esperada).
    if (m_readbackPending[frame]) {
        vmaInvalidateAllocation(m_ctx->allocator(), m_readback[frame].alloc, 0, VK_WHOLE_SIZE);
        const uint32_t* c = static_cast<const uint32_t*>(m_readback[frame].mapped);
        m_stats.alive = c[1u - m_readbackList[frame]];
        m_stats.rendered = c[kCRender] + c[kCRender + 1];
        m_readbackPending[frame] = false;
    }

    // Biblioteca de grafos cheia (edicao ao vivo gera um grafo novo por
    // gesto): recomeca; os ativos se recompilam neste mesmo update.
    if (m_graphCodeCpu.size() > kGraphCodeWords * 3 / 4 || m_graphConstCpu.size() > kGraphConsts * 3 / 4) {
        m_graphLib.clear();
        m_graphCodeCpu.clear();
        m_graphConstCpu.clear();
    }
    m_requests.clear();
    m_spawnTotal = 0;
    m_anyLowRes = false;
    m_sortThisFrame = false;
    float maxLife = 0.0f;
    uint32_t emitterCount = 0;
    for (size_t ii = 0; ii < m_instances.size();) {
        Instance& inst = m_instances[ii];
        const VfxEffectDesc& fx = *inst.desc;
        const Vec3 pos = Vec3(inst.transform[3]);
        if (inst.firstFrame) inst.prevPos = pos;
        inst.velocity = dt > 1e-6f ? (pos - inst.prevPos) / dt : Vec3(0.0f);
        const float moved = glm::length(pos - inst.prevPos);
        const float prevTime = inst.time;
        inst.time += dt;
        // Efeito com duracao: em laco recomeca as rajadas; sem laco para de nascer.
        float effTime = inst.time;
        bool wrapped = false;
        if (fx.duration > 0.0f) {
            if (fx.loop) {
                effTime = std::fmod(inst.time, fx.duration);
                wrapped = std::floor(inst.time / fx.duration) != std::floor(prevTime / fx.duration);
            } else if (inst.time >= fx.duration && inst.stopTime < 0.0f) {
                inst.stopTime = fx.duration;
            }
        }
        const bool stopped = inst.killed || inst.stopTime >= 0.0f;
        bool anySpawning = false;
        const float life = fx.maxLifetime();
        maxLife = std::max(maxLife, life);

        for (size_t ei = 0; ei < inst.emitters.size() && ei < fx.emitters.size(); ++ei) {
            EmitterState& es = inst.emitters[ei];
            const VfxEmitterDesc& e = fx.emitters[ei];
            if (wrapped) {
                es.burstsDone = 0;
                es.burstTimer = 0.0f;
            }
            const float local = effTime - e.startDelay;
            const bool emitting = e.enabled && !stopped && local >= 0.0f && (e.duration <= 0.0f || local < e.duration);
            uint32_t n = 0;
            if (emitting) {
                anySpawning = true;
                es.spawnAccum += (e.rate * dt + e.ratePerMeter * moved) * m_spawnScale;
                const float whole = std::floor(es.spawnAccum);
                n = static_cast<uint32_t>(whole);
                es.spawnAccum -= whole;
                if (e.burstCount > 0) {
                    const uint32_t burst = static_cast<uint32_t>(std::ceil(float(e.burstCount) * m_spawnScale));
                    if (es.burstsDone == 0) {
                        n += burst;
                        es.burstsDone = 1;
                        es.burstTimer = 0.0f;
                    } else if (e.burstInterval > 0.0f) {
                        es.burstTimer += dt;
                        while (es.burstTimer >= e.burstInterval) {
                            n += burst;
                            es.burstTimer -= e.burstInterval;
                        }
                    }
                }
            }
            // Teto do emissor sem ler a GPU: cada lote conta como vivo ate'
            // a maior vida possivel (estimativa conservadora).
            while (!es.liveChunks.empty() && es.liveChunks.front().first <= inst.time) {
                es.liveEstimate -= es.liveChunks.front().second;
                es.liveChunks.erase(es.liveChunks.begin());
            }
            n = std::min(n, e.capacity > es.liveEstimate ? e.capacity - es.liveEstimate : 0u);
            if (n > 0 && m_requests.size() < kMaxRequests) {
                m_requests.push_back({es.slot, n, m_spawnTotal, hash32(m_frameSeed * 7919u + es.slot * 104729u)});
                m_spawnTotal += n;
                es.liveEstimate += n;
                if (!es.liveChunks.empty() && es.liveChunks.back().first == inst.time + e.lifetime.y)
                    es.liveChunks.back().second += n;
                else
                    es.liveChunks.emplace_back(inst.time + e.lifetime.y, n);
            }
            fillEmitterGpu(inst, e, es.slot, m_slotData[es.slot]);
            if (emitting) m_slotData[es.slot].flags.x |= 1u << 13;
            // Particulas deste emissor ainda podem estar vivas mesmo parado:
            // a decisao de meia resolucao/ordenacao vale enquanto o efeito existe.
            m_anyLowRes = m_anyLowRes || e.lowRes;
            m_sortThisFrame = m_sortThisFrame || e.blend == VfxBlend::Alpha || e.blend == VfxBlend::Premultiplied;
            ++emitterCount;
        }
        inst.prevPos = pos;
        inst.firstFrame = false;

        // Fim: morto agora, ou parou de nascer e a ultima particula ja' morreu.
        const bool done = inst.killed || (!anySpawning && stopped && inst.time - inst.stopTime > life) ||
                          (!anySpawning && !stopped && fx.duration <= 0.0f && inst.time > 1.0f &&
                           std::all_of(fx.emitters.begin(), fx.emitters.end(),
                                       [&](const VfxEmitterDesc& e) {
                                           return !e.enabled ||
                                                  (e.duration > 0.0f && inst.time > e.startDelay + e.duration + life) ||
                                                  (e.rate <= 0.0f && e.ratePerMeter <= 0.0f && e.burstInterval <= 0.0f &&
                                                   inst.time > e.startDelay + life);
                                       }));
        if (done) {
            for (EmitterState& es : inst.emitters) freeSlot(es.slot);
            m_instances.erase(m_instances.begin() + long(ii));
            continue;
        }
        ++ii;
    }
    m_frameSeed = hash32(m_frameSeed + 0x9E3779B9u);

    // Sem instancia: o sistema segue simulando so' ate' a ultima particula
    // possivel morrer; depois custa zero (nem dispatch, nem draw).
    m_maxLifeSeen = std::max(m_maxLifeSeen, maxLife);
    if (m_instances.empty()) m_idleTime += dt;
    else m_idleTime = 0.0f;
    m_active = !m_instances.empty() || m_idleTime <= m_maxLifeSeen + 0.5f;
    if (!m_active) {
        m_slotHighWater = 0;
        m_maxLifeSeen = 0.0f;
    }
    m_stats.instances = static_cast<uint32_t>(m_instances.size());
    m_stats.emitters = emitterCount;
    m_stats.spawnedThisFrame = m_spawnTotal;
    m_stats.capacity = m_capacity;
    if (!m_active) {
        m_stats.alive = 0;
        m_stats.rendered = 0;
        return;
    }

    // Parametros do frame
    VfxFrameParamsGpu& fp = m_frameParams;
    fp.viewProj = camera.viewProjNoJitter();
    fp.invViewProj = glm::inverse(fp.viewProj);
    const Frustum& fr = camera.frustum();
    for (int i = 0; i < 6; ++i) {
        const Plane& p = fr.getPlane(static_cast<Frustum::PlaneIndex>(i));
        fp.frustum[i] = Vec4(p.normal, p.distance);
    }
    fp.camPos = Vec4(camera.position(), dt);
    fp.timeInfo = Vec4(m_time += dt, float(m_requests.size()), float(m_spawnTotal), camera.nearPlane());
    fp.depthInfo = Vec4(float(renderSize.width), float(renderSize.height), camera.farPlane(), 1.0f);
    fp.counts = glm::uvec4(m_cur, m_capacity, m_sortThisFrame ? 1u : 0u, 0u);

    std::memcpy(m_emitterBuf[frame].mapped, m_slotData.data(), size_t(m_slotHighWater) * sizeof(VfxEmitterGpu));
    if (!m_requests.empty())
        std::memcpy(m_spawnBuf[frame].mapped, m_requests.data(), m_requests.size() * sizeof(VfxSpawnRequestGpu));
    std::memcpy(m_frameBuf[frame].mapped, &fp, sizeof(fp));
    vmaFlushAllocation(m_ctx->allocator(), m_emitterBuf[frame].alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(m_ctx->allocator(), m_spawnBuf[frame].alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(m_ctx->allocator(), m_frameBuf[frame].alloc, 0, VK_WHOLE_SIZE);
}

void VfxSystem::simulate(VkCommandBuffer cmd, VkImageView depthView) {
    if (!m_ready || !m_active) return;
    const uint32_t frame = m_ctx->currentFrame();
    // O alvo de meia resolucao nasce/muda ANTES de qualquer set ser ligado
    // neste command buffer (reescrever um set ja' ligado invalida a gravacao).
    if (m_anyLowRes && !ensureLowResTarget(m_renderSize)) m_anyLowRes = false;
    writeDescriptors(frame, depthView);

    // O frame anterior ainda pode estar lendo emissores/pedidos/contadores.
    memBarrier(cmd,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT);
    if (m_slotHighWater > 0) {
        const VkBufferCopy c{0, 0, VkDeviceSize(m_slotHighWater) * sizeof(VfxEmitterGpu)};
        vkCmdCopyBuffer(cmd, m_emitterBuf[frame].buf, m_emitterDev.buf, 1, &c);
    }
    if (!m_requests.empty()) {
        const VkBufferCopy c{0, 0, VkDeviceSize(m_requests.size()) * sizeof(VfxSpawnRequestGpu)};
        vkCmdCopyBuffer(cmd, m_spawnBuf[frame].buf, m_spawnDev.buf, 1, &c);
    }
    {
        const VkBufferCopy c{0, 0, sizeof(VfxFrameParamsGpu)};
        vkCmdCopyBuffer(cmd, m_frameBuf[frame].buf, m_frameDev.buf, 1, &c);
    }
    if (m_graphDirty && !m_graphCodeCpu.empty()) {
        // Grafos novos/alterados: sobe o codigo e as constantes inteiros.
        char* st = static_cast<char*>(m_graphStage[frame].mapped);
        const VkDeviceSize codeBytes = VkDeviceSize(m_graphCodeCpu.size()) * 4;
        const VkDeviceSize constBytes = VkDeviceSize(m_graphConstCpu.size()) * 16;
        std::memcpy(st, m_graphCodeCpu.data(), size_t(codeBytes));
        if (constBytes) std::memcpy(st + VkDeviceSize(kGraphCodeWords) * 4, m_graphConstCpu.data(), size_t(constBytes));
        vmaFlushAllocation(m_ctx->allocator(), m_graphStage[frame].alloc, 0, VK_WHOLE_SIZE);
        const VkBufferCopy cc{0, 0, codeBytes};
        vkCmdCopyBuffer(cmd, m_graphStage[frame].buf, m_graphCode.buf, 1, &cc);
        if (constBytes) {
            const VkBufferCopy kc{VkDeviceSize(kGraphCodeWords) * 4, 0, constBytes};
            vkCmdCopyBuffer(cmd, m_graphStage[frame].buf, m_graphConst.buf, 1, &kc);
        }
        m_graphDirty = false;
    }
    memBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
    // A profundidade deste frame (G-buffer) ja' esta' em SHADER_READ_ONLY:
    // so' falta a dependencia de execucao ate' o compute que colide com ela.
    memBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT);

    const VkMemoryBarrier rw{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    auto computeToCompute = [&](VkPipelineStageFlags extraDst, VkAccessFlags extraAccess) {
        VkMemoryBarrier b = rw;
        b.dstAccessMask |= extraAccess;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | extraDst,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computeLayout, 0, 1, &m_sets[frame], 0, nullptr);
    if (m_spawnTotal > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_emitPipe);
        vkCmdDispatch(cmd, (m_spawnTotal + 63) / 64, 1, 1);
        computeToCompute(0, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_argsPipe);
    vkCmdDispatch(cmd, 1, 1, 1);
    computeToCompute(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_simPipe);
    vkCmdDispatchIndirect(cmd, m_counters.buf, kCDispatch * 4);
    computeToCompute(0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_finishPipe);
    vkCmdDispatch(cmd, 1, 1, 1);
    // Ordem de tras para frente so' quando ha mistura alfa tocando (aditiva
    // nao precisa): contagem por balde, soma prefixada, espalhamento.
    if (m_sortThisFrame) {
        computeToCompute(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        for (uint32_t list = 0; list < 2; ++list) {
            if (list == 1 && !m_anyLowRes) break;
            vkCmdPushConstants(cmd, m_computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &list);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortHistPipe);
            vkCmdDispatchIndirect(cmd, m_counters.buf, (kCSortDispatch + list * 4) * 4);
            computeToCompute(0, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortScanPipe);
            vkCmdDispatch(cmd, 1, 1, 1);
            computeToCompute(0, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortScatterPipe);
            vkCmdDispatchIndirect(cmd, m_counters.buf, (kCSortDispatch + list * 4) * 4);
            computeToCompute(0, 0);
        }
    }
    memBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    // Contadores para a estatistica (lidos quando este slot voltar).
    const VkBufferCopy rc{0, 0, kCounterCount * 4};
    vkCmdCopyBuffer(cmd, m_counters.buf, m_readback[frame].buf, 1, &rc);
    m_readbackPending[frame] = true;
    m_readbackList[frame] = m_cur;
    m_cur = 1u - m_cur;
}

void VfxSystem::debugDump() {
    if (!m_ready) return;
    m_ctx->waitIdle();
    Buf rb;
    if (!createBuf(rb, 256 + 4 * kParticleBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, true))
        return;
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        const VkBufferCopy c0{0, 0, 64};
        vkCmdCopyBuffer(cmd, m_counters.buf, rb.buf, 1, &c0);
        const VkBufferCopy c1{0, 64, 16};
        vkCmdCopyBuffer(cmd, m_render.buf, rb.buf, 1, &c1);
    });
    vmaInvalidateAllocation(m_ctx->allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
    const uint32_t* c = static_cast<const uint32_t*>(rb.mapped);
    ERUPTION_LOG_WARN("[VFX] dump: alive0=%u alive1=%u dead=%u render=%u dispatch=%u draw=(%u,%u,%u,%u)", c[0], c[1],
                      c[2], c[3], c[4], c[8], c[9], c[10], c[11]);
    uint32_t idx[4];
    std::memcpy(idx, c + 16, sizeof(idx));
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        for (int i = 0; i < 4; ++i) {
            if (idx[i] >= m_capacity) continue;
            const VkBufferCopy cp{VkDeviceSize(idx[i]) * kParticleBytes, 256 + VkDeviceSize(i) * kParticleBytes,
                                  kParticleBytes};
            vkCmdCopyBuffer(cmd, m_particles.buf, rb.buf, 1, &cp);
        }
    });
    vmaInvalidateAllocation(m_ctx->allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < 4; ++i) {
        const float* p = reinterpret_cast<const float*>(static_cast<const char*>(rb.mapped) + 256 + i * kParticleBytes);
        uint32_t sg;
        std::memcpy(&sg, p + 8, 4);
        uint32_t sr;
        std::memcpy(&sr, p + 10, 4);
        const glm::vec2 szr = glm::unpackHalf2x16(sr);
        ERUPTION_LOG_WARN("[VFX] dump p%u: pos=(%.1f,%.1f,%.1f) age=%.2f vel=(%.1f,%.1f,%.1f) life=%.2f slot=%u gen=%u size=%.2f",
                          idx[i], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], sg & 0xFFFFu, sg >> 16, szr.x);
    }
    destroyBuf(rb);
}

bool VfxSystem::ensureLowResTarget(VkExtent2D full) {
    const VkExtent2D want{std::max(1u, (full.width + 1) / 2), std::max(1u, (full.height + 1) / 2)};
    if (m_lowImage && want.width == m_lowExtent.width && want.height == m_lowExtent.height) return true;
    // Tamanho mudou (janela, painel do editor): o antigo morre quando os
    // frames em voo que o usam terminarem.
    if (m_lowImage) {
        VkImage img = m_lowImage;
        VmaAllocation alloc = m_lowAlloc;
        VkImageView view = m_lowView;
        VulkanContext* ctx = m_ctx;
        m_ctx->deferFrameCleanup([ctx, img, alloc, view]() {
            vkDestroyImageView(ctx->device(), view, nullptr);
            vmaDestroyImage(ctx->allocator(), img, alloc);
        });
        m_lowImage = VK_NULL_HANDLE;
        m_lowView = VK_NULL_HANDLE;
    }
    if (!m_ctx->createImage(want.width, want.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
                            m_lowImage, m_lowAlloc))
        return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_lowImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkc::createImageView(m_ctx->device(), &vi, nullptr, &m_lowView) != VK_SUCCESS) return false;
    m_lowExtent = want;
    return true;
}

void VfxSystem::drawLowRes(VkCommandBuffer cmd, VkImageView depthView, VkExtent2D extent) {
    if (!m_ready || !m_active || !m_anyLowRes || !m_lowView) return;
    const uint32_t frame = m_ctx->currentFrame();
    writeDescriptors(frame, depthView);
    m_ctx->cmdImageBarrier(cmd, m_lowImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = m_lowView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, m_lowExtent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkc::cmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLowPipe);
    const VkViewport vp{0.0f, 0.0f, float(m_lowExtent.width), float(m_lowExtent.height), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, m_lowExtent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, m_drawSets[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLayout, 0, 3, sets, 0, nullptr);
    const DrawPush push{(m_sortThisFrame ? 3u : 1u) * m_capacity, float(extent.width) / float(m_lowExtent.width)};
    vkCmdPushConstants(cmd, m_drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDrawIndirect(cmd, m_counters.buf, (kCDraw + 4) * 4, 1, 16);
    vkc::cmdEndRendering(cmd);
    m_ctx->cmdImageBarrier(cmd, m_lowImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
}

void VfxSystem::draw(VkCommandBuffer cmd, VkImageView depthView, VkExtent2D extent) {
    if (!m_ready || !m_active) return;
    const uint32_t frame = m_ctx->currentFrame();
    writeDescriptors(frame, depthView);
    const VkViewport vp{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, extent};
    // Meia resolucao primeiro (fumaca, atras), depois a resolucao cheia.
    if (m_anyLowRes && m_lowView) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipe);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositeLayout, 0, 1, &m_drawSets[frame], 0,
                                nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipe);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, m_drawSets[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLayout, 0, 3, sets, 0, nullptr);
    const DrawPush push{m_sortThisFrame ? 2u * m_capacity : 0u, 1.0f};
    vkCmdPushConstants(cmd, m_drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDrawIndirect(cmd, m_counters.buf, kCDraw * 4, 1, 16);
}

} // namespace eruption
