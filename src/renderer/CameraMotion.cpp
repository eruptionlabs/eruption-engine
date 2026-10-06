#include "renderer/CameraMotion.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "math/Camera.hpp"
#include "core/Logger.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/packing.hpp>

#include <cstdio>
#include <vector>

namespace eruption {

namespace {

// Espelha o bloco Params de camera_motion.comp (std430 de push constant).
struct Push {
    Mat4 viewToPrevClip;
    Mat4 projNoJitter;
    Vec4 projJ;
    Vec4 depthP;
    Vec2 invSize;
    int32_t size[2];
    int32_t useObject;
    int32_t pad[3];
};
static_assert(sizeof(Push) == 192, "push constants da camera_motion");

} // namespace

bool CameraMotion::init(VulkanContext* ctx, uint32_t width, uint32_t height) {
    m_ctx = ctx;
    VkDevice dev = m_ctx->device();

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS) return false;

    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    const auto code = ShaderCompiler::loadSPIRV("compute/camera_motion.comp.spv");
    if (code.empty()) {
        Logger::error("CameraMotion: compute/camera_motion.comp.spv nao encontrado");
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
    ci.layout = m_pipelineLayout;
    const VkResult r = vkCreateComputePipelines(dev, m_ctx->pipelineCache(), 1, &ci, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, module, nullptr);
    if (r != VK_SUCCESS) return false;

    m_width = width;
    m_height = height;
    createTarget();
    return true;
}

void CameraMotion::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    destroyTarget();
    if (m_pipeline) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

void CameraMotion::createTarget() {
    m_ctx->createImage(m_width, m_height, kFormat,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY, m_image, m_alloc);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_ctx->device(), &vi, nullptr, &m_view);
    m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    writeSet();
}

void CameraMotion::destroyTarget() {
    if (m_view) vkDestroyImageView(m_ctx->device(), m_view, nullptr);
    if (m_image) vmaDestroyImage(m_ctx->allocator(), m_image, m_alloc);
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_alloc = VK_NULL_HANDLE;
}

void CameraMotion::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    vkDeviceWaitIdle(m_ctx->device());
    destroyTarget();
    m_width = width;
    m_height = height;
    createTarget();
}

void CameraMotion::bindDepth(VkImageView depthView) {
    m_depthView = depthView;
    writeSet();
}

void CameraMotion::bindObjectVelocity(VkImageView velocityView) {
    m_objectVelocityView = velocityView;
    writeSet();
}

void CameraMotion::writeSet() {
    if (m_set == VK_NULL_HANDLE || m_view == VK_NULL_HANDLE) return;
    VkDescriptorImageInfo out{VK_NULL_HANDLE, m_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depth{m_sampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[3]{};
    // Sem velocidade de objeto o binding 2 ainda precisa de algo valido: o
    // proprio depth serve (o shader nao o le' com useObject = 0).
    VkDescriptorImageInfo obj{m_sampler, m_objectVelocityView ? m_objectVelocityView : m_depthView,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    uint32_t n = 0;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[n].dstSet = m_set;
    w[n].dstBinding = 1;
    w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[n].pImageInfo = &out;
    ++n;
    if (m_depthView != VK_NULL_HANDLE) {
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = m_set;
        w[n].dstBinding = 0;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &depth;
        ++n;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = m_set;
        w[n].dstBinding = 2;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &obj;
        ++n;
    }
    vkUpdateDescriptorSets(m_ctx->device(), n, w, 0, nullptr);
}

void CameraMotion::dispatch(VkCommandBuffer cmd, const Camera& camera, const Mat4& prevViewProjNoJitter) {
    if (m_pipeline == VK_NULL_HANDLE || m_depthView == VK_NULL_HANDLE) return;

    m_ctx->cmdImageBarrier(cmd, m_image, m_layout, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // A unica matriz que toca coordenadas de mundo (grandes) e' montada em
    // double; o resto do caminho fica em espaco de vista, ver o shader.
    const Mat4 pj = camera.projJittered();
    const bool ortho = camera.isOrthographic();
    Push push{};
    push.viewToPrevClip = Mat4(glm::dmat4(prevViewProjNoJitter) * glm::inverse(glm::dmat4(camera.viewMatrix())));
    push.projNoJitter = camera.projNoJitter();
    push.projJ = ortho ? Vec4(pj[0][0], pj[1][1], pj[3][0], pj[3][1])
                       : Vec4(pj[0][0], pj[1][1], pj[2][0], pj[2][1]);
    push.depthP = Vec4(pj[2][2], pj[3][2], ortho ? 1.0f : 0.0f, 0.0f);
    push.invSize = Vec2(1.0f / float(m_width), 1.0f / float(m_height));
    push.size[0] = int32_t(m_width);
    push.size[1] = int32_t(m_height);
    push.useObject = m_objectVelocityView != VK_NULL_HANDLE ? 1 : 0;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &m_set, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
    vkCmdDispatch(cmd, (m_width + 7) / 8, (m_height + 7) / 8, 1);

    m_ctx->cmdImageBarrier(cmd, m_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

bool CameraMotion::dumpToFile(const char* path) {
    if (m_image == VK_NULL_HANDLE || m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return false;
    vkDeviceWaitIdle(m_ctx->device());
    const VkDeviceSize bytes = VkDeviceSize(m_width) * m_height * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!m_ctx->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU,
                             staging, stagingAlloc)) return false;
    m_ctx->copyImageToBuffer(m_image, staging, m_width, m_height);
    void* mapped = nullptr;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    vmaInvalidateAllocation(m_ctx->allocator(), stagingAlloc, 0, VK_WHOLE_SIZE);
    const uint16_t* half = static_cast<const uint16_t*>(mapped);
    std::vector<float> rg(size_t(m_width) * m_height * 2);
    for (size_t i = 0; i < rg.size(); ++i) rg[i] = glm::unpackHalf1x16(half[i]);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);
    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t hdr[3] = {0x0032564Du /* "MV2" */, m_width, m_height};
    std::fwrite(hdr, sizeof(hdr), 1, f);
    std::fwrite(rg.data(), sizeof(float), rg.size(), f);
    std::fclose(f);
    Logger::info("CameraMotion: motion vectors %ux%u gravados em %s", m_width, m_height, path);
    return true;
}

} // namespace eruption
