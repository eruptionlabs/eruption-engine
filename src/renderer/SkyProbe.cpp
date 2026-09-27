#include "renderer/SkyProbe.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/skymap/SkySystem.hpp"
#include "core/Logger.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace eruption {

namespace {

// Tabela de faces do Vulkan (spec, "Cube Map Face Selection"): a direcao do
// texel (s,t) em [-1,1] da face f e' A[f] + s*B[f] + t*C[f]. O skybox.frag
// reconstroi o raio como (invViewProj * (ndc.x, ndc.y, 1, 1)).xyz, entao basta
// uma matriz cujas colunas sao (B, C, A, e4): nenhuma projecao, nenhuma
// convencao de flip - a leitura por samplerCube bate com a escrita por
// construcao. A MESMA tabela vive no sky_sh.comp.
const Vec3 kA[6] = {Vec3( 1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0,-1, 0), Vec3(0, 0, 1), Vec3( 0, 0,-1)};
const Vec3 kB[6] = {Vec3( 0, 0,-1), Vec3( 0, 0, 1), Vec3(1, 0, 0), Vec3(1, 0, 0), Vec3(1, 0, 0), Vec3(-1, 0, 0)};
const Vec3 kC[6] = {Vec3( 0,-1, 0), Vec3( 0,-1, 0), Vec3(0, 0, 1), Vec3(0, 0,-1), Vec3(0,-1, 0), Vec3( 0,-1, 0)};

Mat4 faceInvViewProj(uint32_t f) {
    Mat4 m(0.0f);
    m[0] = Vec4(kB[f], 0.0f);   // * ndc.x
    m[1] = Vec4(kC[f], 0.0f);   // * ndc.y
    m[2] = Vec4(kA[f], 0.0f);   // * clip.z (=1)
    m[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f); // * clip.w (=1) -> w de saida = 1
    return m;
}

VkImageMemoryBarrier2 imgBarrier(VkImage image, VkImageLayout oldL, VkImageLayout newL,
                                 VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                 VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                                 uint32_t baseLayer, uint32_t layerCount) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, baseMip, mipCount, baseLayer, layerCount};
    return b;
}

void submitBarriers(VkCommandBuffer cmd, const std::vector<VkImageMemoryBarrier2>& imgs,
                    const VkMemoryBarrier2* mem = nullptr) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(imgs.size());
    dep.pImageMemoryBarriers = imgs.empty() ? nullptr : imgs.data();
    dep.memoryBarrierCount = mem ? 1u : 0u;
    dep.pMemoryBarriers = mem;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

bool SkyProbe::init(VulkanContext* ctx, uint32_t faceSize) {
    m_ctx = ctx;
    m_faceSize = faceSize < 8 ? 8 : faceSize;
    m_mipCount = 1;
    for (uint32_t s = m_faceSize; s > 1; s >>= 1) ++m_mipCount;
    if (!createCube()) return false;
    if (!createCompute()) return false;
    return true;
}

bool SkyProbe::createCube() {
    VkDevice dev = m_ctx->device();
    // Cubemap RGBA16F (mesmo formato do pipeline do skybox), 6 camadas, mips
    // completos: alvo de cor (faces), blit (mips) e amostragem (lighting + SH).
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {m_faceSize, m_faceSize, 1};
    ii.mipLevels = m_mipCount;
    ii.arrayLayers = 6;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &ii, &ai, &m_cube, &m_cubeAlloc, nullptr) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("SkyProbe: falha ao criar o cubemap");
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_cube;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipCount, 0, 6};
    if (vkCreateImageView(dev, &vi, nullptr, &m_cubeView) != VK_SUCCESS) return false;
    for (uint32_t f = 0; f < 6; ++f) {
        VkImageViewCreateInfo fv = vi;
        fv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
        if (vkCreateImageView(dev, &fv, nullptr, &m_faceViews[f]) != VK_SUCCESS) return false;
    }
    // Profundidade minima: o pipeline do skybox declara D32 e o dynamic
    // rendering exige um attachment de formato igual (sem a feature
    // dynamicRenderingUnusedAttachments, imageView nulo e' invalido).
    if (!m_ctx->createImage(m_faceSize, m_faceSize, VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VMA_MEMORY_USAGE_GPU_ONLY,
                            m_depth, m_depthAlloc)) {
        ERUPTION_LOG_ERROR("SkyProbe: falha ao criar a profundidade");
        return false;
    }
    VkImageViewCreateInfo dv{};
    dv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dv.image = m_depth;
    dv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dv.format = VK_FORMAT_D32_SFLOAT;
    dv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &dv, nullptr, &m_depthView) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f;
    si.maxLod = static_cast<float>(m_mipCount);
    if (vkCreateSampler(dev, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    if (!m_ctx->createBuffer(kShBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_shBuffer, m_shAlloc)) {
        ERUPTION_LOG_ERROR("SkyProbe: falha ao criar o buffer de SH");
        return false;
    }
    // Zera o SH: antes da primeira sonda o lighting le' zeros (ambiente
    // escuro por um frame) em vez de lixo.
    m_ctx->immediateSubmit([&](VkCommandBuffer c) {
        vkCmdFillBuffer(c, m_shBuffer, 0, kShBytes, 0u);
    });
    return true;
}

bool SkyProbe::createCompute() {
    VkDevice dev = m_ctx->device();
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_descLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps[2] = {};
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_descPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_descLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_descSet) != VK_SUCCESS) return false;

    VkDescriptorImageInfo img{};
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img.imageView = m_cubeView;
    img.sampler = m_sampler;
    VkDescriptorBufferInfo buf{};
    buf.buffer = m_shBuffer; buf.offset = 0; buf.range = kShBytes;
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_descSet; w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].descriptorCount = 1; w[0].pImageInfo = &img;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_descSet; w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].descriptorCount = 1; w[1].pBufferInfo = &buf;
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pc.offset = 0; pc.size = 8;
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &m_descLayout;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &m_pipeLayout) != VK_SUCCESS) return false;

    auto code = ShaderCompiler::loadSPIRV("skybox/sky_sh.comp.spv");
    if (code.empty()) {
        ERUPTION_LOG_ERROR("SkyProbe: skybox/sky_sh.comp.spv nao encontrado");
        return false;
    }
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = code.size() * sizeof(uint32_t);
    mi.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &mi, nullptr, &mod) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = m_pipeLayout;
    VkResult r = vkCreateComputePipelines(dev, m_ctx->pipelineCache(), 1, &ci, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
    return r == VK_SUCCESS;
}

void SkyProbe::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    if (m_pipeline) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipeLayout) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_shBuffer) vmaDestroyBuffer(m_ctx->allocator(), m_shBuffer, m_shAlloc);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_depthView) vkDestroyImageView(dev, m_depthView, nullptr);
    if (m_depth) vmaDestroyImage(m_ctx->allocator(), m_depth, m_depthAlloc);
    for (auto& v : m_faceViews) { if (v) vkDestroyImageView(dev, v, nullptr); v = VK_NULL_HANDLE; }
    if (m_cubeView) vkDestroyImageView(dev, m_cubeView, nullptr);
    if (m_cube) vmaDestroyImage(m_ctx->allocator(), m_cube, m_cubeAlloc);
    m_pipeline = VK_NULL_HANDLE; m_pipeLayout = VK_NULL_HANDLE; m_descPool = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE; m_shBuffer = VK_NULL_HANDLE; m_sampler = VK_NULL_HANDLE;
    m_depthView = VK_NULL_HANDLE; m_depth = VK_NULL_HANDLE; m_cubeView = VK_NULL_HANDLE; m_cube = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

void SkyProbe::update(VkCommandBuffer cmd, SkySystem& sky, const DayNightCycle& cycle) {
    if (!m_ctx || m_cube == VK_NULL_HANDLE) return;
    const VkImageLayout prev = (m_updates == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    // 1) Todo o cubo -> alvo de cor (o conteudo antigo e' descartado: tudo e'
    //    regravado). Profundidade -> attachment.
    submitBarriers(cmd, {
        imgBarrier(m_cube, prev == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_UNDEFINED : prev,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipCount, 0, 6),
        imgBarrier(m_depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1)});

    // 2) As 6 faces (mip 0).
    bool drew = true;
    for (uint32_t f = 0; f < 6 && drew; ++f) {
        drew = sky.renderProbeFace(cmd, cycle, faceInvViewProj(f), m_faceViews[f], m_depthView, m_faceSize, f);
    }
    if (!drew) {
        // Backend sem sonda: deixa o cubo legivel (preto) e sai.
        submitBarriers(cmd, {imgBarrier(m_cube, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                        readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipCount, 0, 6)});
        ++m_updates;
        return;
    }

    // 3) Mips por blit (cadeia): mip 0 vira fonte, os demais destino.
    std::vector<VkImageMemoryBarrier2> bs;
    bs.push_back(imgBarrier(m_cube, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6));
    if (m_mipCount > 1) {
        bs.push_back(imgBarrier(m_cube, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, 1, m_mipCount - 1, 0, 6));
    }
    submitBarriers(cmd, bs);
    int32_t srcSize = static_cast<int32_t>(m_faceSize);
    for (uint32_t m = 1; m < m_mipCount; ++m) {
        const int32_t dstSize = srcSize > 1 ? srcSize / 2 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 6};
        blit.srcOffsets[1] = {srcSize, srcSize, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 6};
        blit.dstOffsets[1] = {dstSize, dstSize, 1};
        vkCmdBlitImage(cmd, m_cube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        // Este mip vira fonte do proximo.
        submitBarriers(cmd, {imgBarrier(m_cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 6)});
        srcSize = dstSize;
    }
    // 4) Tudo legivel (SH no compute, lighting no fragment); SSBO livre pra
    //    ser reescrito (o frame anterior ja' leu - dependencia de execucao).
    VkMemoryBarrier2 toCompute{};
    toCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    toCompute.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toCompute.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    toCompute.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toCompute.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    submitBarriers(cmd, {imgBarrier(m_cube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                    readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipCount, 0, 6)}, &toCompute);

    // 5) SH9.
    struct { uint32_t faceSize; float maxRadiance; } pc{m_faceSize, 64.0f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier2 toFrag{};
    toFrag.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    toFrag.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toFrag.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toFrag.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toFrag.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    submitBarriers(cmd, {}, &toFrag);
    ++m_updates;
}

} // namespace eruption
