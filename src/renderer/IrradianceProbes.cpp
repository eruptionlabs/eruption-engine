#include "renderer/IrradianceProbes.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "math/Frustum.hpp"
#include "core/Logger.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace eruption {

namespace {
struct BakeUBO {
    Mat4 topVP;
    Mat4 botVP;
    Vec4 gridMin;
    Vec4 cell;
    uint32_t dims[4];
    Vec4 params;
};

VkShaderModule makeModule(VkDevice dev, const char* path) {
    auto code = ShaderCompiler::loadSPIRV(path);
    if (code.empty()) { ERUPTION_LOG_ERROR("IrradianceProbes: %s nao encontrado", path); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = code.size() * sizeof(uint32_t);
    mi.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &mi, nullptr, &mod);
    return mod;
}

bool make3D(VulkanContext* ctx, uint32_t dx, uint32_t dy, uint32_t dz, VkImageUsageFlags usage,
            VkImage& img, VmaAllocation& alloc, VkImageView& view) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {dx, dy, dz};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(ctx->allocator(), &ii, &ai, &img, &alloc, nullptr) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkc::createImageView(ctx->device(), &vi, nullptr, &view) == VK_SUCCESS;
}
} // namespace

bool IrradianceProbes::init(VulkanContext* ctx) {
    m_ctx = ctx;
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx->device(), &si, nullptr, &m_sampler) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(ctx->device(), &si, nullptr, &m_depthSampler) != VK_SUCCESS) return false;
    if (!ctx->createBuffer(sizeof(BakeUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, m_ubo, m_uboAlloc)) return false;
    vmaMapMemory(ctx->allocator(), m_uboAlloc, &m_uboMapped);
    if (!createFallback()) return false;
    if (!createPipelines()) return false;
    return true;
}

bool IrradianceProbes::createFallback() {
    if (!make3D(m_ctx, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                m_fallbackImage, m_fallbackAlloc, m_fallbackView)) return false;
    // Ceu aberto: L0 = 4pi * Y00, L1 = 0 -> E(N)/pi = 1 para toda normal.
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, m_fallbackImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                               0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue cv{};
        cv.float32[0] = 3.544908f; cv.float32[1] = 0.0f; cv.float32[2] = 0.0f; cv.float32[3] = 0.0f;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_fallbackImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &r);
        m_ctx->cmdImageBarrier(cmd, m_fallbackImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    return true;
}

bool IrradianceProbes::createPipelines() {
    VkDevice dev = m_ctx->device();
    // Bake: 3 depth samplers + UBO + image3D de saida.
    {
        VkDescriptorSetLayoutBinding b[5] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 5; li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_bakeLayout) != VK_SUCCESS) return false;
    }
    // Dilate: image3D in + out.
    {
        VkDescriptorSetLayoutBinding b[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2; li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_dilateLayout) != VK_SUCCESS) return false;
    }
    VkDescriptorPoolSize ps[3] = {};
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = 8;
    ps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[1].descriptorCount = 4;
    ps[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ps[2].descriptorCount = 8;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 4; pi.poolSizeCount = 3; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &m_bakeLayout;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_bakePipeLayout) != VK_SUCCESS) return false;
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pc.offset = 0; pc.size = 16;
    pl.pSetLayouts = &m_dilateLayout;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_dilatePipeLayout) != VK_SUCCESS) return false;

    VkShaderModule bake = makeModule(dev, "lighting/probe_bake.comp.spv");
    VkShaderModule dil = makeModule(dev, "lighting/probe_dilate.comp.spv");
    if (!bake || !dil) return false;
    VkComputePipelineCreateInfo ci[2] = {};
    for (int i = 0; i < 2; ++i) {
        ci[i].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci[i].stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci[i].stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci[i].stage.pName = "main";
    }
    ci[0].stage.module = bake; ci[0].layout = m_bakePipeLayout;
    ci[1].stage.module = dil;  ci[1].layout = m_dilatePipeLayout;
    VkPipeline out[2] = {};
    VkResult r = vkCreateComputePipelines(dev, m_ctx->pipelineCache(), 2, ci, nullptr, out);
    vkDestroyShaderModule(dev, bake, nullptr);
    vkDestroyShaderModule(dev, dil, nullptr);
    m_bakePipeline = out[0]; m_dilatePipeline = out[1];
    return r == VK_SUCCESS;
}

bool IrradianceProbes::createDepth(DepthTarget& t) {
    if (!m_ctx->createImage(kDepthSize, kDepthSize, VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, t.image, t.alloc)) return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_D32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    return vkc::createImageView(m_ctx->device(), &vi, nullptr, &t.view) == VK_SUCCESS;
}

void IrradianceProbes::destroyDepth(DepthTarget& t) {
    if (t.view) vkDestroyImageView(m_ctx->device(), t.view, nullptr);
    if (t.image) vmaDestroyImage(m_ctx->allocator(), t.image, t.alloc);
    t = DepthTarget{};
}

bool IrradianceProbes::createGrid(uint32_t dx, uint32_t dy, uint32_t dz) {
    destroyGrid();
    if (!make3D(m_ctx, dx, dy, dz, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, m_bakeImage, m_bakeAlloc, m_bakeView)) return false;
    if (!make3D(m_ctx, dx, dy, dz, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_finalImage, m_finalAlloc, m_finalView)) return false;
    m_dims[0] = dx; m_dims[1] = dy; m_dims[2] = dz;
    return true;
}

void IrradianceProbes::destroyGrid() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    if (m_bakeView) vkDestroyImageView(dev, m_bakeView, nullptr);
    if (m_bakeImage) vmaDestroyImage(m_ctx->allocator(), m_bakeImage, m_bakeAlloc);
    if (m_finalView) vkDestroyImageView(dev, m_finalView, nullptr);
    if (m_finalImage) vmaDestroyImage(m_ctx->allocator(), m_finalImage, m_finalAlloc);
    m_bakeView = m_finalView = VK_NULL_HANDLE;
    m_bakeImage = m_finalImage = VK_NULL_HANDLE;
    m_valid = false;
}

void IrradianceProbes::clear() {
    if (!m_ctx) return;
    // A grade pode estar em uso por um frame em voo: espera a GPU antes.
    vkDeviceWaitIdle(m_ctx->device());
    destroyGrid();
}

bool IrradianceProbes::bake(TerrainRenderer& terrain, ModelRenderer& models,
                            VkPipeline shadowPipeline, VkPipelineLayout shadowLayout,
                            VkPipeline instPipeline, VkPipelineLayout instLayout,
                            BindlessDescriptor* bindless,
                            const Vec3& sceneMin, const Vec3& sceneMax) {
    const bool useInst = instPipeline != VK_NULL_HANDLE && instLayout != VK_NULL_HANDLE;
    if (!m_ctx || shadowPipeline == VK_NULL_HANDLE || m_bakePipeline == VK_NULL_HANDLE) return false;
    const auto t0 = std::chrono::steady_clock::now();
    vkDeviceWaitIdle(m_ctx->device());

    // Grade: celula XZ de 24..96 unidades (64 probes por eixo no mapa
    // grande), Y de 16..64 (ate' 16 niveis). 64x16x64 = 65 k probes.
    const Vec3 extent = glm::max(sceneMax - sceneMin, Vec3(1.0f));
    const float cellXZ = std::clamp(std::max(extent.x, extent.z) / 64.0f, 24.0f, 96.0f);
    const float cellY = std::clamp(extent.y / 16.0f, 16.0f, 64.0f);
    const uint32_t dx = std::clamp<uint32_t>(uint32_t(std::ceil(extent.x / cellXZ)) + 1u, 2u, 96u);
    const uint32_t dz = std::clamp<uint32_t>(uint32_t(std::ceil(extent.z / cellXZ)) + 1u, 2u, 96u);
    const uint32_t dy = std::clamp<uint32_t>(uint32_t(std::ceil(extent.y / cellY)) + 1u, 2u, 24u);
    const Vec3 cell(extent.x / float(dx - 1), extent.y / float(dy - 1), extent.z / float(dz - 1));
    // Centro das probes da borda cai no limite do AABB: gridMin recua meia celula.
    const Vec3 gridMin = sceneMin - cell * 0.5f;
    const Vec3 gridExtent = cell * Vec3(float(dx), float(dy), float(dz));
    if (!createGrid(dx, dy, dz)) return false;

    DepthTarget terrainTop, modelTop, modelBot;
    if (!createDepth(terrainTop) || !createDepth(modelTop) || !createDepth(modelBot)) {
        destroyDepth(terrainTop); destroyDepth(modelTop); destroyDepth(modelBot);
        return false;
    }

    // Ortho de cima e de baixo cobrindo o AABB com margem.
    const Vec3 c = (sceneMin + sceneMax) * 0.5f;
    const float hx = extent.x * 0.5f + 32.0f, hz = extent.z * 0.5f + 32.0f;
    const float farD = extent.y + 200.0f;
    Mat4 proj = glm::ortho(-hx, hx, -hz, hz, 1.0f, farD);
    proj[1][1] *= -1.0f; // Vulkan Y-flip (mesma convencao do rain heightmap)
    const Vec3 eyeTop(c.x, sceneMax.y + 100.0f, c.z);
    const Vec3 eyeBot(c.x, sceneMin.y - 100.0f, c.z);
    const Mat4 topVP = proj * glm::lookAt(eyeTop, eyeTop + Vec3(0, -1, 0), Vec3(0, 0, -1));
    const Mat4 botVP = proj * glm::lookAt(eyeBot, eyeBot + Vec3(0, 1, 0), Vec3(0, 0, -1));

    BakeUBO ubo{};
    ubo.topVP = topVP;
    ubo.botVP = botVP;
    ubo.gridMin = Vec4(gridMin, 0.0f);
    ubo.cell = Vec4(cell, 0.0f);
    ubo.dims[0] = dx; ubo.dims[1] = dy; ubo.dims[2] = dz; ubo.dims[3] = 0;
    // Raio da marcha: ate' 6 celulas XZ (copa/telhado vizinho conta, a
    // montanha do outro lado do mapa nao), 24 passos, comeca a 1/4 de celula.
    {
        const float radius = std::min(cellXZ * 6.0f, 400.0f);
        const float steps = 24.0f;
        const float start = cellXZ * 0.25f;
        // Espessura minima da laje = meio passo, em profundidade do ortho
        // (linear: 1 unidade de mundo = 1/(far-near)). Ver probe_bake.comp.
        const float stepLen = (radius - start) / steps;
        const float depthEps = (stepLen * 0.5f) / (farD - 1.0f);
        ubo.params = Vec4(radius, steps, start, depthEps);
    }
    std::memcpy(m_uboMapped, &ubo, sizeof(ubo));
    vmaFlushAllocation(m_ctx->allocator(), m_uboAlloc, 0, sizeof(ubo));

    // Descritores (pool reiniciado a cada bake).
    vkResetDescriptorPool(m_ctx->device(), m_pool, 0);
    VkDescriptorSet bakeSet = VK_NULL_HANDLE, dilSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool; ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_bakeLayout;
        vkAllocateDescriptorSets(m_ctx->device(), &ai, &bakeSet);
        ai.pSetLayouts = &m_dilateLayout;
        vkAllocateDescriptorSets(m_ctx->device(), &ai, &dilSet);
        VkDescriptorImageInfo depthInfo[3] = {};
        const VkImageView dv[3] = {terrainTop.view, modelTop.view, modelBot.view};
        for (int i = 0; i < 3; ++i) {
            depthInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            depthInfo[i].imageView = dv[i];
            depthInfo[i].sampler = m_depthSampler;
        }
        VkDescriptorBufferInfo bi{m_ubo, 0, sizeof(BakeUBO)};
        VkDescriptorImageInfo bakeOut{VK_NULL_HANDLE, m_bakeView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dilIn{VK_NULL_HANDLE, m_bakeView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dilOut{VK_NULL_HANDLE, m_finalView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[7] = {};
        for (int i = 0; i < 7; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].descriptorCount = 1; }
        for (int i = 0; i < 3; ++i) { w[i].dstSet = bakeSet; w[i].dstBinding = uint32_t(i); w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &depthInfo[i]; }
        w[3].dstSet = bakeSet; w[3].dstBinding = 3; w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[3].pBufferInfo = &bi;
        w[4].dstSet = bakeSet; w[4].dstBinding = 4; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[4].pImageInfo = &bakeOut;
        w[5].dstSet = dilSet;  w[5].dstBinding = 0; w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[5].pImageInfo = &dilIn;
        w[6].dstSet = dilSet;  w[6].dstBinding = 1; w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[6].pImageInfo = &dilOut;
        vkUpdateDescriptorSets(m_ctx->device(), 7, w, 0, nullptr);
    }

    const uint32_t dimsPush[4] = {dx, dy, dz, 0};
    // Buffer de diagnostico (sempre: 65x17x65 x 8 B = 570 KB, uma vez por mapa).
    VkBuffer statsBuf = VK_NULL_HANDLE; VmaAllocation statsAlloc = VK_NULL_HANDLE;
    if (!m_ctx->createBuffer(VkDeviceSize(dx) * dy * dz * 8u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_TO_CPU, statsBuf, statsAlloc)) {
        statsBuf = VK_NULL_HANDLE;
    }
    VkBuffer dumpBuf = VK_NULL_HANDLE; VmaAllocation dumpAlloc = VK_NULL_HANDLE;
    const char* dumpDir = std::getenv("ERUPTION_PROBES_DUMP");
    if (dumpDir && !m_ctx->createBuffer(VkDeviceSize(kDepthSize) * kDepthSize * 4u * 3u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VMA_MEMORY_USAGE_GPU_TO_CPU, dumpBuf, dumpAlloc)) {
        dumpBuf = VK_NULL_HANDLE;
    }
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        // groundFilter: 1 = so' o chao (terreno + modelos isGround), 2 = tudo
        // menos o chao. Mapa GLB traz o terreno como modelo: sem separar, o
        // chao visto de baixo seria a base de toda laje.
        auto renderDepth = [&](DepthTarget& t, const Mat4& vp, int groundFilter) {
            m_ctx->cmdImageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                                   0, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo depth{};
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = t.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};
            m_ctx->cmdBeginRendering(cmd, {}, &depth, nullptr, {kDepthSize, kDepthSize});
            VkViewport vpz{0.0f, 0.0f, float(kDepthSize), float(kDepthSize), 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {kDepthSize, kDepthSize}};
            vkCmdSetViewport(cmd, 0, 1, &vpz);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            Frustum fr;
            fr.extractFromMatrix(vp);
            if (groundFilter == 1 && terrain.isInitialized()) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
                if (bindless) {
                    VkDescriptorSet bs = bindless->set();
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout, 0, 1, &bs, 0, nullptr);
                }
                terrain.renderShadow(cmd, shadowPipeline, shadowLayout, vp, fr, true);
            }
            // Modelos pelo caminho INSTANCIADO quando existe - e' o que o passe
            // de sombra real usa. Medido: o caminho por-draw so' escrevia
            // profundidade das malhas com LOD (arvores do parana_demo); chao e
            // modelos legados saiam vazios.
            const VkPipeline mp = useInst ? instPipeline : shadowPipeline;
            const VkPipelineLayout ml = useInst ? instLayout : shadowLayout;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mp);
            if (bindless) {
                VkDescriptorSet bs = bindless->set();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ml, 0, 1, &bs, 0, nullptr);
            }
            if (models.isInitialized()) {
                // Instancia com mais de 40% do mapa em X ou Z e' cenario (domo,
                // plano d'agua), nao oclusor. ERUPTION_PROBES_NO_HUGE=1 /
                // ERUPTION_PROBES_NO_CULL=1: bancada.
                static const bool kNoHuge = std::getenv("ERUPTION_PROBES_NO_HUGE") != nullptr;
                static const bool kNoCull = std::getenv("ERUPTION_PROBES_NO_CULL") != nullptr;
                static const bool kFilter0 = std::getenv("ERUPTION_PROBES_FILTER0") != nullptr;
                models.setShadowGroundFilter(kFilter0 ? 0 : groundFilter, kNoHuge ? 0.0f : 0.4f * std::max(extent.x, extent.z));
                models.renderShadow(cmd, mp, ml, vp, fr, 0.0f, !kNoCull, useInst);
                models.setShadowGroundFilter(0);
            }
            m_ctx->cmdEndRendering(cmd);
            m_ctx->cmdImageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
        };
        renderDepth(terrainTop, topVP, 1);
        renderDepth(modelTop, topVP, 2);
        renderDepth(modelBot, botVP, 2);
        // ERUPTION_PROBES_DUMP=<dir>: copia os tres heightfields para a CPU
        // (PGM 8 bits) - bancada.
        if (dumpBuf != VK_NULL_HANDLE) {
            const DepthTarget* ts[3] = {&terrainTop, &modelTop, &modelBot};
            for (int i = 0; i < 3; ++i) {
                VkBufferImageCopy rc{};
                rc.bufferOffset = VkDeviceSize(i) * kDepthSize * kDepthSize * 4u;
                rc.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                rc.imageExtent = {kDepthSize, kDepthSize, 1};
                m_ctx->cmdImageBarrier(cmd, ts[i]->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                vkCmdCopyImageToBuffer(cmd, ts[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dumpBuf, 1, &rc);
                m_ctx->cmdImageBarrier(cmd, ts[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            }
        }

        m_ctx->cmdImageBarrier(cmd, m_bakeImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               0, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_ctx->cmdImageBarrier(cmd, m_finalImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               0, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const uint32_t gx = (dx + 3) / 4, gy = (dy + 3) / 4, gz = (dz + 3) / 4;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bakePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bakePipeLayout, 0, 1, &bakeSet, 0, nullptr);
        vkCmdDispatch(cmd, gx, gy, gz);
        m_ctx->cmdImageBarrier(cmd, m_bakeImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_dilatePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_dilatePipeLayout, 0, 1, &dilSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_dilatePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dimsPush), dimsPush);
        vkCmdDispatch(cmd, gx, gy, gz);
        m_ctx->cmdImageBarrier(cmd, m_finalImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        // Diagnostico: copia a grade CRUA (antes da dilatacao) para a CPU.
        if (statsBuf != VK_NULL_HANDLE) {
            VkBufferImageCopy rc{};
            rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rc.imageExtent = {dx, dy, dz};
            m_ctx->cmdImageBarrier(cmd, m_bakeImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            vkCmdCopyImageToBuffer(cmd, m_bakeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, statsBuf, 1, &rc);
            m_ctx->cmdImageBarrier(cmd, m_bakeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }
    });
    // immediateSubmit espera a GPU: os alvos de profundidade ja' podem ir.
    destroyDepth(terrainTop); destroyDepth(modelTop); destroyDepth(modelBot);
    if (dumpBuf != VK_NULL_HANDLE) {
        void* mp = nullptr;
        vmaMapMemory(m_ctx->allocator(), dumpAlloc, &mp);
        const float* f = static_cast<const float*>(mp);
        const char* names[3] = {"terreno_topo", "modelos_topo", "modelos_base"};
        for (int i = 0; i < 3; ++i) {
            std::string path = std::string(dumpDir) + "/probes_" + names[i] + ".pgm";
            if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                std::fprintf(fp, "P5\n%u %u\n255\n", kDepthSize, kDepthSize);
                std::vector<unsigned char> row(kDepthSize);
                const float* base = f + size_t(i) * kDepthSize * kDepthSize;
                size_t written = 0;
                for (uint32_t y = 0; y < kDepthSize; ++y) {
                    for (uint32_t x = 0; x < kDepthSize; ++x) {
                        const float d = base[size_t(y) * kDepthSize + x];
                        row[x] = static_cast<unsigned char>(std::clamp(d, 0.0f, 1.0f) * 255.0f);
                        if (d < 0.999f) ++written;
                    }
                    std::fwrite(row.data(), 1, row.size(), fp);
                }
                std::fclose(fp);
                ERUPTION_LOG_WARN("[PROBES] dump %s: %zu texels com geometria", path.c_str(), written);
            }
        }
        vmaUnmapMemory(m_ctx->allocator(), dumpAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), dumpBuf, dumpAlloc);
    }
    if (statsBuf != VK_NULL_HANDLE) {
        void* mp = nullptr;
        vmaMapMemory(m_ctx->allocator(), statsAlloc, &mp);
        const uint16_t* h = static_cast<const uint16_t*>(mp);
        auto halfToFloat = [](uint16_t v) -> float {
            const uint32_t s = (v >> 15) & 1u, e = (v >> 10) & 0x1Fu, m = v & 0x3FFu;
            float f;
            if (e == 0) f = std::ldexp(float(m), -24);
            else if (e == 31) f = m ? 0.0f : 1e30f;
            else f = std::ldexp(float(m + 1024), int(e) - 25);
            return s ? -f : f;
        };
        const size_t n = size_t(dx) * dy * dz;
        size_t invalid = 0, occluded = 0; double sumVis = 0.0; float minVis = 1.0f;
        for (size_t i = 0; i < n; ++i) {
            const float L0 = halfToFloat(h[i * 4]);
            if (L0 < 0.0f) { ++invalid; continue; }
            const float L1y = halfToFloat(h[i * 4 + 2]);
            const float vis = std::clamp((0.886227f * L0 + 1.023328f * L1y) / 3.14159265f, 0.0f, 1.0f); // N = +Y
            sumVis += vis; minVis = std::min(minVis, vis);
            if (vis < 0.9f) ++occluded;
        }
        vmaUnmapMemory(m_ctx->allocator(), statsAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), statsBuf, statsAlloc);
        const size_t validN = n - invalid;
        ERUPTION_LOG_WARN("[PROBES] probes %zu: invalidas %.1f%% | validas: vis(+Y) media %.3f min %.3f, %.1f%% com vis<0.9",
                          n, 100.0 * double(invalid) / double(n),
                          validN ? sumVis / double(validN) : 0.0, minVis,
                          validN ? 100.0 * double(occluded) / double(validN) : 0.0);
    }

    m_gridMin = gridMin;
    m_gridInvExtent = Vec3(1.0f / gridExtent.x, 1.0f / gridExtent.y, 1.0f / gridExtent.z);
    m_valid = true;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    uint32_t gMeshes = 0, tMeshes = 0, gInst = 0;
    models.groundMeshStats(gMeshes, tMeshes, gInst);
    ERUPTION_LOG_WARN("[PROBES] grade %ux%ux%u (celula %.0f x %.0f x %.0f) sobre [%.0f,%.0f,%.0f]..[%.0f,%.0f,%.0f] em %lld ms | chao: %u de %u malhas (%u instancias)",
                      dx, dy, dz, cell.x, cell.y, cell.z, sceneMin.x, sceneMin.y, sceneMin.z,
                      sceneMax.x, sceneMax.y, sceneMax.z, (long long)ms, gMeshes, tMeshes, gInst);
    return true;
}

void IrradianceProbes::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    destroyGrid();
    if (m_bakePipeline) vkDestroyPipeline(dev, m_bakePipeline, nullptr);
    if (m_dilatePipeline) vkDestroyPipeline(dev, m_dilatePipeline, nullptr);
    if (m_bakePipeLayout) vkDestroyPipelineLayout(dev, m_bakePipeLayout, nullptr);
    if (m_dilatePipeLayout) vkDestroyPipelineLayout(dev, m_dilatePipeLayout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_bakeLayout) vkDestroyDescriptorSetLayout(dev, m_bakeLayout, nullptr);
    if (m_dilateLayout) vkDestroyDescriptorSetLayout(dev, m_dilateLayout, nullptr);
    if (m_uboMapped) vmaUnmapMemory(m_ctx->allocator(), m_uboAlloc);
    if (m_ubo) vmaDestroyBuffer(m_ctx->allocator(), m_ubo, m_uboAlloc);
    if (m_fallbackView) vkDestroyImageView(dev, m_fallbackView, nullptr);
    if (m_fallbackImage) vmaDestroyImage(m_ctx->allocator(), m_fallbackImage, m_fallbackAlloc);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_depthSampler) vkDestroySampler(dev, m_depthSampler, nullptr);
    m_bakePipeline = m_dilatePipeline = VK_NULL_HANDLE;
    m_bakePipeLayout = m_dilatePipeLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE; m_bakeLayout = m_dilateLayout = VK_NULL_HANDLE;
    m_uboMapped = nullptr; m_ubo = VK_NULL_HANDLE;
    m_fallbackView = VK_NULL_HANDLE; m_fallbackImage = VK_NULL_HANDLE;
    m_sampler = m_depthSampler = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

} // namespace eruption
