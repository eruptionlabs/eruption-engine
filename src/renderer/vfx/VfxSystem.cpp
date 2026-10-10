#include "renderer/vfx/VfxSystem.hpp"
#include "renderer/GBuffer.hpp"
#include "renderer/vfx/VfxBuiltins.hpp"

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
constexpr uint32_t kCDraw = 12;        // 5 uints por lista (indexado)
constexpr uint32_t kCSortDispatch = 24;
constexpr uint32_t kCounterCount = 84;
constexpr uint32_t kCEventDispatch = 32;
constexpr uint32_t kRecordBytes = 64;
constexpr uint32_t kMaxEvents = 16384;
constexpr uint32_t kBindings = 16;

struct DrawPush {
    uint32_t listOffset;
    float depthScale;
};
struct MeshPush {
    Mat4 viewProjNoJitter;
    Mat4 prevViewProjNoJitter;
    uint32_t listOffset;
    uint32_t pad[3];
};
constexpr uint32_t kMeshTypes = 8;
constexpr uint32_t kCMeshDraw = 44; // 5 uints por malha
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
              createBuf(m_render, cap * 20, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_sort, 2 * 512 * 4, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_events, VkDeviceSize(kMaxEvents) * 32, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_recs, cap * kRecordBytes, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
              createBuf(m_ribbonRing, VkDeviceSize(kRibbonRingTotal) * 4, ssbo, VMA_MEMORY_USAGE_GPU_ONLY, false) &&
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
    for (Buf* b : {&m_particles, &m_dead, &m_alive, &m_render, &m_sort, &m_events, &m_recs, &m_ribbonRing, &m_counters,
                   &m_emitterDev,
                   &m_spawnDev,
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
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 13 * n},
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
    const VkDescriptorBufferInfo eventInfo{m_events.buf, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo recInfo{m_recs.buf, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo ribbonInfo{m_ribbonRing.buf, 0, VK_WHOLE_SIZE};
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
        w[s * kBindings + 13].pBufferInfo = &eventInfo;
        w[s * kBindings + 14].pBufferInfo = &recInfo;
        w[s * kBindings + 15].pBufferInfo = &ribbonInfo;
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
        !compute("vfx/vfx_sort_scatter.comp.spv", m_sortScatterPipe) ||
        !compute("vfx/vfx_event.comp.spv", m_eventPipe))
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
    // Dois alvos: o lit e a mascara reativa (R8, MAX: a particula mais
    // presente no pixel manda).
    const VkFormat colorFormats[2] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8_UNORM};
    VkPipelineColorBlendAttachmentState blends[2] = {blend, blend};
    blends[1].srcColorBlendFactor = blends[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blends[1].srcAlphaBlendFactor = blends[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blends[1].colorBlendOp = blends[1].alphaBlendOp = VK_BLEND_OP_MAX;
    blends[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    cb.attachmentCount = 2;
    cb.pAttachments = blends;
    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachmentFormats = colorFormats;
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
    VkShaderModule fsLow = loadModule(dev, "vfx/vfx_particle_low.frag.spv");
    if (r == VK_SUCCESS && fsLow) {
        st[1].module = fsLow;
        ri.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        ri.colorAttachmentCount = 1; // so' a cor: a cobertura entra pela composicao
        cb.attachmentCount = 1;
        ds.depthTestEnable = VK_FALSE;
        r = vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &m_drawLowPipe);
        ri.colorAttachmentCount = 2;
        cb.attachmentCount = 2;
    }
    if (fsLow) vkDestroyShaderModule(dev, fsLow, nullptr);
    else r = VK_ERROR_INITIALIZATION_FAILED;
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if (r != VK_SUCCESS) return false;

    // PARTICULAS DE MALHA: vertice de 32 bytes, opacas, profundidade escrita.
    {
        const VkPushConstantRange meshPush{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MeshPush)};
        pl.setLayoutCount = 3;
        pl.pSetLayouts = drawSets;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &meshPush;
        if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_meshLayout) != VK_SUCCESS) return false;
        VkShaderModule mvs = loadModule(dev, "vfx/vfx_mesh.vert.spv");
        VkShaderModule mfs =
            loadModule(dev, GBuffer::velocityEnabled() ? "vfx/vfx_mesh.frag.spv" : "vfx/vfx_mesh_novel.frag.spv");
        VkShaderModule mfwd = loadModule(dev, "vfx/vfx_mesh_fwd.frag.spv");
        const VkVertexInputBindingDescription vb{0, sizeof(VfxMeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                         {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
        VkPipelineVertexInputStateCreateInfo mvi{};
        mvi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        mvi.vertexBindingDescriptionCount = 1;
        mvi.pVertexBindingDescriptions = &vb;
        mvi.vertexAttributeDescriptionCount = 3;
        mvi.pVertexAttributeDescriptions = va;
        gi.pVertexInputState = &mvi;
        gi.layout = m_meshLayout;
        st[0].module = mvs;
        st[1].module = mfs;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        const std::vector<VkFormat> gbFormats = GBuffer::colorAttachmentFormats();
        const std::vector<VkPipelineColorBlendAttachmentState> gbBlend = GBuffer::blendStates(true);
        VkPipelineColorBlendStateCreateInfo gcb{};
        gcb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        gcb.attachmentCount = uint32_t(gbBlend.size());
        gcb.pAttachments = gbBlend.data();
        gi.pColorBlendState = &gcb;
        ri.colorAttachmentCount = uint32_t(gbFormats.size());
        ri.pColorAttachmentFormats = gbFormats.data();
        ri.depthAttachmentFormat = GBuffer::DepthFormat;
        r = (mvs && mfs && mfwd) ? vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &m_meshPipe)
                                 : VK_ERROR_INITIALIZATION_FAILED;
        // Previa: os dois alvos do draw() (lit + mascara), profundidade so'
        // leitura (o rendering do draw() nao escreve profundidade).
        if (r == VK_SUCCESS) {
            st[1].module = mfwd;
            ds.depthWriteEnable = VK_FALSE;
            gi.pColorBlendState = &cb;
            ri.colorAttachmentCount = 2;
            ri.pColorAttachmentFormats = colorFormats;
            ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
            r = vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &m_meshFwdPipe);
        }
        for (VkShaderModule m : {mvs, mfs, mfwd})
            if (m) vkDestroyShaderModule(dev, m, nullptr);
        gi.pVertexInputState = &vi;
        gi.pColorBlendState = &cb;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_FALSE;
        st[0].module = VK_NULL_HANDLE;
        if (r != VK_SUCCESS) return false;
    }

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
    // Leque de 6 triangulos sobre os 8 vertices do poligono recortado.
    if (createBuf(m_ringIndex, 18 * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
                  true)) {
        uint16_t* ix = static_cast<uint16_t*>(m_ringIndex.mapped);
        for (uint16_t t = 0; t < 6; ++t) {
            ix[t * 3 + 0] = 0;
            ix[t * 3 + 1] = uint16_t(t + 1);
            ix[t * 3 + 2] = uint16_t(t + 2);
        }
        vmaFlushAllocation(m_ctx->allocator(), m_ringIndex.alloc, 0, VK_WHOLE_SIZE);
    }
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
                          &m_compositePipe, &m_sortHistPipe, &m_sortScanPipe, &m_sortScatterPipe, &m_eventPipe,
                          &m_meshPipe, &m_meshFwdPipe})
        if (*p) vkDestroyPipeline(dev, *p, nullptr), *p = VK_NULL_HANDLE;
    destroyReactive();
    destroyBuf(m_ringIndex);
    for (GpuMesh& m : m_meshes) {
        destroyBuf(m.vb);
        destroyBuf(m.ib);
    }
    m_meshes.clear();
    if (m_meshLayout) vkDestroyPipelineLayout(dev, m_meshLayout, nullptr);
    m_meshLayout = VK_NULL_HANDLE;
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
            freeRibbon(inst.emitters.back());
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

uint32_t VfxSystem::allocRibbon(uint32_t size) {
    auto& freeList = m_ribbonFree[size];
    if (!freeList.empty()) {
        const uint32_t b = freeList.back();
        freeList.pop_back();
        return b;
    }
    if (m_ribbonBump + size > kRibbonRingTotal) {
        ERUPTION_LOG_WARN("[VFX] aneis de fita cheios (%u pontos): fita desligada", kRibbonRingTotal);
        return UINT32_MAX;
    }
    const uint32_t b = m_ribbonBump;
    m_ribbonBump += size;
    return b;
}

void VfxSystem::freeRibbon(EmitterState& es) {
    if (es.ringSize) m_ribbonFree[es.ringSize].push_back(es.ringBase);
    es.ringBase = es.ringSize = 0;
}

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
            for (EmitterState& es : inst.emitters) {
                freeSlot(es.slot);
                freeRibbon(es);
            }
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
        for (EmitterState& es : inst.emitters) {
                freeSlot(es.slot);
                freeRibbon(es);
            }
    m_instances.clear();
    m_meshMaskSimulated = 0;
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
    if (!e.mesh.empty()) {
        const uint32_t id = meshId(e.mesh);
        flags |= (id & 0xFu) << 16;
        if (id) m_meshMask |= 1u << (id - 1);
    }
    const VfxTextureInfo& tex = texture(e.texture, e.flipCols, e.flipRows);
    o.flags = glm::uvec4(flags, m_slotGen[slot], tex.slot, (e.flipCols & 0xFFu) | ((e.flipRows & 0xFFu) << 8));
    for (int k = 0; k < 4; ++k)
        o.cutout[k] = glm::packHalf2x16(glm::vec2(tex.cutout[2 * k], tex.cutout[2 * k + 1]));
    // Sub-emissores: o filho e' outro emissor desta mesma instancia (pelo nome).
    o.events = glm::uvec4(0u);
    auto childSlot = [&](const std::string& name) -> uint32_t {
        if (name.empty() || !inst.desc) return 0u;
        for (size_t i = 0; i < inst.desc->emitters.size() && i < inst.emitters.size(); ++i)
            if (inst.desc->emitters[i].name == name) return inst.emitters[i].slot + 1u;
        return 0u;
    };
    auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    o.events.x = childSlot(e.onDeath.emitter);
    o.events.y = childSlot(e.onCollision.emitter);
    o.events.z = (std::min(e.onDeath.count, 64u) & 0xFFu) | ((std::min(e.onCollision.count, 64u) & 0xFFu) << 8) |
                 (byte(e.onDeath.inheritVelocity) << 16) | (byte(e.onCollision.inheritVelocity) << 24);
    o.events.w = byte(e.onDeath.probability) | (byte(e.onCollision.probability) << 8);
    o.ribbon = glm::uvec4(0u);
    if (e.facing == VfxFacing::Ribbon) {
        // Anel do tamanho da capacidade (potencia de 2): cabem todos os pontos vivos.
        EmitterState* es = nullptr;
        for (const EmitterState& s : inst.emitters)
            if (s.slot == slot) es = const_cast<EmitterState*>(&s);
        if (es) {
            uint32_t want = 64;
            while (want < std::min<uint32_t>(e.capacity, 4096u)) want <<= 1;
            if (es->ringSize != want) {
                freeRibbon(*es);
                es->ringBase = allocRibbon(want);
                es->ringSize = es->ringBase == UINT32_MAX ? 0u : want;
                if (!es->ringSize) es->ringBase = 0;
            }
            o.ribbon = glm::uvec4(es->ringBase, es->ringSize, 0u, 0u);
        }
    }
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
        for (uint32_t m = 0; m < kMeshTypes; ++m) m_stats.rendered += c[kCMeshDraw + m * 5 + 1];
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
    m_meshMask = 0;
    m_sortThisFrame = false;
    m_anyEvents = false;
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
                m_requests.push_back({es.slot, n, m_spawnTotal, hash32(m_frameSeed * 7919u + es.slot * 104729u),
                                      es.seq, 0u, 0u, 0u});
                es.seq += n;
                m_spawnTotal += n;
                es.liveEstimate += n;
                es.livePeak = std::max(es.livePeak, es.liveEstimate);
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
            m_anyEvents = m_anyEvents || m_slotData[es.slot].events.x != 0u || m_slotData[es.slot].events.y != 0u;
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
            for (EmitterState& es : inst.emitters) {
                freeSlot(es.slot);
                freeRibbon(es);
            }
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
        m_ribbonBump = 0;
        m_ribbonFree.clear();
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
    m_viewProjNoJitter = fp.viewProj;
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
    for (uint32_t m = 0; m < kMeshTypes; ++m)
        fp.meshIndexCount[m >> 2][m & 3] = m < m_meshes.size() ? m_meshes[m].indexCount : 0u;
    fp.sunDir = m_sunDir;
    fp.sunColor = m_sunColor;
    fp.ambient = m_ambient;
    fp.viewInfo = Vec4(std::abs(camera.projNoJitter()[1][1]), 0.0f, 0.0f, 0.0f);

    std::memcpy(m_emitterBuf[frame].mapped, m_slotData.data(), size_t(m_slotHighWater) * sizeof(VfxEmitterGpu));
    if (!m_requests.empty())
        std::memcpy(m_spawnBuf[frame].mapped, m_requests.data(), m_requests.size() * sizeof(VfxSpawnRequestGpu));
    std::memcpy(m_frameBuf[frame].mapped, &fp, sizeof(fp));
    vmaFlushAllocation(m_ctx->allocator(), m_emitterBuf[frame].alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(m_ctx->allocator(), m_spawnBuf[frame].alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(m_ctx->allocator(), m_frameBuf[frame].alloc, 0, VK_WHOLE_SIZE);
}

void VfxSystem::simulate(VkCommandBuffer cmd, VkImageView depthView) {
    if (!m_ready || !m_active) {
        m_meshMaskSimulated = 0;
        return;
    }
    m_meshMaskSimulated = m_meshMask;
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
    // Filhas dos sub-emissores (um grupo por evento escrito pela simulacao).
    if (m_anyEvents) {
        computeToCompute(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_eventPipe);
        vkCmdDispatchIndirect(cmd, m_counters.buf, kCEventDispatch * 4);
    }
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
        const VkBufferCopy c0{0, 0, kCounterCount * 4};
        vkCmdCopyBuffer(cmd, m_counters.buf, rb.buf, 1, &c0);
        const VkBufferCopy c1{0, 128, 16};
        vkCmdCopyBuffer(cmd, m_render.buf, rb.buf, 1, &c1);
    });
    vmaInvalidateAllocation(m_ctx->allocator(), rb.alloc, 0, VK_WHOLE_SIZE);
    const uint32_t* c = static_cast<const uint32_t*>(rb.mapped);
    ERUPTION_LOG_WARN("[VFX] dump: alive0=%u alive1=%u dead=%u render=%u+%u dispatch=%u draw=(%u,%u) low=(%u,%u)", c[0],
                      c[1], c[2], c[3], c[4], c[kCDispatch], c[kCDraw], c[kCDraw + 1], c[kCDraw + 5], c[kCDraw + 6]);
    uint32_t idx[4];
    std::memcpy(idx, c + 32, sizeof(idx));
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
    // Mascara reativa: quantos pixels as particulas marcaram.
    if (m_reactiveImage) {
        const VkDeviceSize n = VkDeviceSize(m_reactiveExtent.width) * m_reactiveExtent.height;
        Buf mb;
        if (createBuf(mb, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, true)) {
            m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
                m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                       VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy r{};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {m_reactiveExtent.width, m_reactiveExtent.height, 1};
                vkCmdCopyImageToBuffer(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mb.buf, 1, &r);
                m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                       VK_ACCESS_SHADER_READ_BIT);
            });
            vmaInvalidateAllocation(m_ctx->allocator(), mb.alloc, 0, VK_WHOLE_SIZE);
            const uint8_t* px = static_cast<const uint8_t*>(mb.mapped);
            size_t marked = 0, sum = 0;
            for (VkDeviceSize i = 0; i < n; ++i) {
                marked += px[i] > 0;
                sum += px[i];
            }
            ERUPTION_LOG_WARN("[VFX] dump reativa: %.2f%% dos pixels marcados, media %.1f/255", 100.0 * double(marked) / double(n),
                              double(sum) / double(n));
            destroyBuf(mb);
        }
    }
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
    vkCmdBindIndexBuffer(cmd, m_ringIndex.buf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect(cmd, m_counters.buf, (kCDraw + 5) * 4, 1, 20);
    vkc::cmdEndRendering(cmd);
    m_ctx->cmdImageBarrier(cmd, m_lowImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
}

bool VfxSystem::resize(uint32_t w, uint32_t h) {
    if (!m_ctx || w == 0 || h == 0) return false;
    if (m_reactiveImage && w == m_reactiveExtent.width && h == m_reactiveExtent.height) return true;
    m_ctx->waitIdle();
    destroyReactive();
    if (!m_ctx->createImage(w, h, VK_FORMAT_R8_UNORM,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_reactiveImage, m_reactiveAlloc))
        return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_reactiveImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkc::createImageView(m_ctx->device(), &vi, nullptr, &m_reactiveView) != VK_SUCCESS) return false;
    m_reactiveExtent = {w, h};
    // Comeca zerada e legivel (o FSR/sprites leem mesmo sem particula).
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    });
    m_reactiveWritten = false;
    return true;
}

void VfxSystem::destroyReactive() {
    if (!m_ctx) return;
    if (m_reactiveView) vkDestroyImageView(m_ctx->device(), m_reactiveView, nullptr);
    if (m_reactiveImage) vmaDestroyImage(m_ctx->allocator(), m_reactiveImage, m_reactiveAlloc);
    m_reactiveView = VK_NULL_HANDLE;
    m_reactiveImage = VK_NULL_HANDLE;
    m_reactiveExtent = {0, 0};
}

void VfxSystem::endFrame(VkCommandBuffer cmd) {
    if (!m_ready || m_active || !m_reactiveWritten || !m_reactiveImage) return;
    m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    m_reactiveWritten = false;
}

void VfxSystem::draw(VkCommandBuffer cmd, VkImageView litView, VkImageView depthView, VkExtent2D extent) {
    if (!m_ready || !m_active || !m_reactiveImage) return;
    const uint32_t frame = m_ctx->currentFrame();
    writeDescriptors(frame, depthView);
    m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo colors[2]{};
    colors[0].sType = colors[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colors[0].imageView = litView;
    colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colors[1].imageView = m_reactiveView;
    colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colors[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    vkc::cmdBeginRendering(cmd, &ri);
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
    if (m_forwardMeshes && m_meshMaskSimulated)
        recordMeshes(cmd, m_meshFwdPipe, extent, m_viewProjNoJitter, m_viewProjNoJitter);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipe);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, m_drawSets[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLayout, 0, 3, sets, 0, nullptr);
    const DrawPush push{m_sortThisFrame ? 2u * m_capacity : 0u, 1.0f};
    vkCmdPushConstants(cmd, m_drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdBindIndexBuffer(cmd, m_ringIndex.buf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect(cmd, m_counters.buf, kCDraw * 4, 1, 20);
    vkc::cmdEndRendering(cmd);
    m_ctx->cmdImageBarrier(cmd, m_reactiveImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    m_reactiveWritten = true;
}

uint32_t VfxSystem::meshId(const std::string& name) {
    for (size_t i = 0; i < m_meshes.size(); ++i)
        if (m_meshes[i].name == name) return m_meshes[i].indexCount ? uint32_t(i) + 1 : 0u;
    if (m_meshes.size() >= kMeshTypes) return 0;
    GpuMesh gm;
    gm.name = name;
    VfxMeshData data;
    if (!makeVfxBuiltinMesh(name, data)) {
        ERUPTION_LOG_WARN("[VFX] malha de particula desconhecida: %s (desenha como sprite)", name.c_str());
    } else if (createBuf(gm.vb, data.vertices.size() * sizeof(VfxMeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VMA_MEMORY_USAGE_CPU_TO_GPU, true) &&
               createBuf(gm.ib, data.indices.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VMA_MEMORY_USAGE_CPU_TO_GPU, true)) {
        std::memcpy(gm.vb.mapped, data.vertices.data(), data.vertices.size() * sizeof(VfxMeshVertex));
        std::memcpy(gm.ib.mapped, data.indices.data(), data.indices.size() * sizeof(uint16_t));
        vmaFlushAllocation(m_ctx->allocator(), gm.vb.alloc, 0, VK_WHOLE_SIZE);
        vmaFlushAllocation(m_ctx->allocator(), gm.ib.alloc, 0, VK_WHOLE_SIZE);
        gm.indexCount = uint32_t(data.indices.size());
    }
    m_meshes.push_back(std::move(gm));
    return m_meshes.back().indexCount ? uint32_t(m_meshes.size()) : 0u;
}

void VfxSystem::recordMeshes(VkCommandBuffer cmd, VkPipeline pipe, VkExtent2D extent, const Mat4& vp,
                             const Mat4& prevVp) {
    const uint32_t frame = m_ctx->currentFrame();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkViewport viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, m_drawSets[frame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshLayout, 0, 3, sets, 0, nullptr);
    MeshPush push{};
    push.viewProjNoJitter = vp;
    push.prevViewProjNoJitter = prevVp;
    const uint32_t per = m_capacity / kMeshTypes;
    for (uint32_t m = 0; m < m_meshes.size(); ++m) {
        if (!(m_meshMaskSimulated & (1u << m)) || !m_meshes[m].indexCount) continue;
        push.listOffset = 4u * m_capacity + m * per;
        vkCmdPushConstants(cmd, m_meshLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        const VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_meshes[m].vb.buf, &off);
        vkCmdBindIndexBuffer(cmd, m_meshes[m].ib.buf, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexedIndirect(cmd, m_counters.buf, VkDeviceSize(kCMeshDraw + m * 5) * 4, 1, 20);
    }
}

void VfxSystem::drawMeshes(VkCommandBuffer cmd, VkImageView depthView, VkExtent2D extent, const Mat4& viewProjNoJitter,
                           const Mat4& prevViewProjNoJitter) {
    if (!m_ready || !m_active || !m_meshMaskSimulated || m_forwardMeshes) return;
    // Os sets sao (re)escritos aqui, antes de ligar: a simulacao deste frame
    // encontra tudo igual e nao reescreve um set ja' ligado.
    if (m_anyLowRes && !ensureLowResTarget(m_renderSize)) m_anyLowRes = false;
    writeDescriptors(m_ctx->currentFrame(), depthView);
    recordMeshes(cmd, m_meshPipe, extent, viewProjNoJitter, prevViewProjNoJitter);
}

void VfxSystem::collectLights(const Vec3& eye, size_t max, std::vector<Light>& out) const {
    out.clear();
    if (!m_ready || max == 0) return;
    std::vector<std::pair<float, Light>> all;
    for (const Instance& inst : m_instances) {
        if (inst.killed || !inst.desc) continue;
        const Mat4& t = inst.transform;
        const float scale = glm::length(Vec3(t[0]));
        for (size_t k = 0; k < inst.emitters.size() && k < inst.desc->emitters.size(); ++k) {
            const VfxEmitterDesc& e = inst.desc->emitters[k];
            const EmitterState& es = inst.emitters[k];
            if (!e.enabled || !e.light || es.livePeak == 0) continue;
            // Fracao viva: a luz nasce com o efeito e some com a ultima particula.
            const float alive = std::min(1.0f, float(es.liveEstimate) / float(std::max(es.livePeak, 1u)) * 1.5f);
            if (alive <= 0.01f) continue;
            // Tremor: duas senoides fora de fase por instancia (barato, sem textura).
            const float ph = float(inst.id) * 1.37f + float(k) * 2.11f;
            const float w = 0.5f * std::sin(inst.time * 13.0f + ph) + 0.3f * std::sin(inst.time * 29.0f + ph * 2.3f) +
                            0.2f * std::sin(inst.time * 7.0f + ph * 0.7f);
            Light l;
            l.position = Vec3(t * Vec4(e.lightOffset, 1.0f));
            l.radius = e.lightRadius * scale;
            l.color = e.lightColor;
            l.intensity = e.lightIntensity * alive * std::max(0.0f, 1.0f + e.lightFlicker * 0.5f * w);
            const Vec3 d = l.position - eye;
            all.emplace_back(glm::dot(d, d), l);
        }
    }
    // Mais perto primeiro: o agrupamento comeca pelo que a camera ve' de perto.
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    struct Cluster {
        Vec3 pos{0.0f}, color{0.0f};
        float weight = 0.0f, radius = 0.0f;
        uint32_t count = 0;
    };
    std::vector<Cluster> clusters;
    for (const auto& p : all) {
        const Light& l = p.second;
        Cluster* hit = nullptr;
        for (Cluster& c : clusters) {
            const Vec3 cp = c.pos / std::max(c.weight, 1e-6f);
            if (glm::length(cp - l.position) < 0.5f * std::max(c.radius, l.radius)) {
                hit = &c;
                break;
            }
        }
        if (!hit) {
            if (clusters.size() >= max) continue; // longe e sem vaga: fica de fora
            clusters.emplace_back();
            hit = &clusters.back();
        }
        const float w = std::max(l.intensity, 1e-4f);
        if (hit->weight > 0.0f) {
            const Vec3 cp = hit->pos / hit->weight;
            hit->radius = std::max(hit->radius, l.radius + 0.5f * glm::length(cp - l.position));
        } else {
            hit->radius = l.radius;
        }
        hit->pos += l.position * w;
        hit->color += l.color * w;
        hit->weight += w;
        ++hit->count;
    }
    out.reserve(clusters.size());
    for (const Cluster& c : clusters) {
        Light l;
        l.position = c.pos / c.weight;
        l.color = c.color / c.weight;
        l.radius = c.radius;
        // A soma cresce mais devagar que o numero de fontes: dez fogueiras
        // juntas clareiam mais que uma, mas nao dez vezes (some saturando).
        l.intensity = c.weight / std::sqrt(float(c.count));
        out.push_back(l);
    }
}

} // namespace eruption
