// Backend Vulkan da RHI (rhi/Rhi.hpp): os mesmos comandos que os renderers
// gravavam direto, para a migração sair pixel a pixel igual.

#include "rhi/Rhi.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/VkCompat.hpp"
#include "renderer/VulkanContext.hpp"
#include "core/Logger.hpp"

#include <vk_mem_alloc.h>

namespace eruption::rhi {

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE; // nulo = embrulho de fora
    Format format = Format::Undefined;
    uint32_t width = 0, height = 0;
    VkImageLayout rest = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Sampler {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct BindGroupLayout {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<BindGroupLayoutEntry> entries;
};

struct BindGroup {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

struct RenderPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderStageFlags pushStages = 0;
};

namespace {

VkShaderStageFlags vkStages(uint32_t stages) {
    VkShaderStageFlags f = 0;
    if (stages & kStageVertex) f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (stages & kStageFragment) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (stages & kStageCompute) f |= VK_SHADER_STAGE_COMPUTE_BIT;
    return f;
}

VkFilter vkFilter(Filter f) { return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

VkSamplerAddressMode vkAddress(AddressMode a) {
    switch (a) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: break;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkAttachmentLoadOp vkLoad(LoadOp op) {
    switch (op) {
    case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::DontCare: break;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkc::cmdPipelineBarrier2(cmd, &dep);
}

VkShaderModule loadModule(VkDevice dev, const char* path) {
    const auto code = ShaderCompiler::loadSPIRV(path);
    if (code.empty()) {
        ERUPTION_LOG_ERROR("RHI: shader %s nao encontrado", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &info, nullptr, &module);
    return module;
}

} // namespace

// ---------------------------------------------------------------- formatos

Format Device::fromVulkan(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return Format::RGBA8Srgb;
    case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return Format::BGRA8Srgb;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return Format::RGBA16Float;
    case VK_FORMAT_R16G16_SFLOAT: return Format::RG16Float;
    case VK_FORMAT_R16_SFLOAT: return Format::R16Float;
    case VK_FORMAT_R32_SFLOAT: return Format::R32Float;
    case VK_FORMAT_R8_UNORM: return Format::R8Unorm;
    case VK_FORMAT_R8G8_UNORM: return Format::RG8Unorm;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return Format::RGB10A2Unorm;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return Format::RG11B10Float;
    case VK_FORMAT_D32_SFLOAT: return Format::Depth32Float;
    default: return Format::Undefined;
    }
}

VkFormat Device::toVulkan(Format f) {
    switch (f) {
    case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
    case Format::R16Float: return VK_FORMAT_R16_SFLOAT;
    case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
    case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
    case Format::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
    case Format::RGB10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::RG11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::Depth32Float: return VK_FORMAT_D32_SFLOAT;
    case Format::Undefined: break;
    }
    return VK_FORMAT_UNDEFINED;
}

Format Device::swapchainFormat() const { return fromVulkan(m_ctx->swapFormat()); }

void Device::waitIdle() { vkDeviceWaitIdle(m_ctx->device()); }

// ---------------------------------------------------------------- texturas

Texture* Device::createTexture(const TextureDesc& desc) {
    VkImageUsageFlags usage = 0;
    if (desc.usage & kUsageSampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.usage & kUsageStorage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (desc.usage & kUsageRenderTarget)
        usage |= desc.format == Format::Depth32Float ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.usage & kUsageCopySrc) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc.usage & kUsageCopyDst) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto* t = new Texture;
    t->format = desc.format;
    t->width = desc.width;
    t->height = desc.height;
    const VkFormat fmt = toVulkan(desc.format);
    if (!m_ctx->createImage(desc.width, desc.height, fmt, usage, VMA_MEMORY_USAGE_GPU_ONLY, t->image, t->alloc)) {
        delete t;
        return nullptr;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_ctx->device(), &vi, nullptr, &t->view);
    t->rest = (desc.usage & kUsageSampled) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return t;
}

Texture* Device::wrapVulkan(VkImage image, VkImageView view, Format format, uint32_t width, uint32_t height,
                            VkImageLayout rest) {
    auto* t = new Texture;
    t->image = image;
    t->view = view;
    t->format = format;
    t->width = width;
    t->height = height;
    t->rest = rest;
    return t;
}

void Device::destroy(Texture* t) {
    if (!t) return;
    if (t->alloc) {
        if (t->view) vkDestroyImageView(m_ctx->device(), t->view, nullptr);
        vmaDestroyImage(m_ctx->allocator(), t->image, t->alloc);
    }
    delete t;
}

Sampler* Device::createSampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = vkFilter(desc.magFilter);
    si.minFilter = vkFilter(desc.minFilter);
    si.mipmapMode = desc.mipFilter == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = vkAddress(desc.address);
    si.compareEnable = VK_FALSE;
    si.minLod = desc.minLod;
    si.maxLod = desc.maxLod;
    auto* s = new Sampler;
    if (vkCreateSampler(m_ctx->device(), &si, nullptr, &s->sampler) != VK_SUCCESS) {
        delete s;
        return nullptr;
    }
    return s;
}

void Device::destroy(Sampler* s) {
    if (!s) return;
    vkDestroySampler(m_ctx->device(), s->sampler, nullptr);
    delete s;
}

// ---------------------------------------------------------------- ligações

BindGroupLayout* Device::createBindGroupLayout(const std::vector<BindGroupLayoutEntry>& entries) {
    std::vector<VkDescriptorSetLayoutBinding> bindings(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        bindings[i] = {};
        bindings[i].binding = entries[i].binding;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vkStages(entries[i].stages);
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = uint32_t(bindings.size());
    li.pBindings = bindings.data();
    auto* l = new BindGroupLayout;
    l->entries = entries;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &li, nullptr, &l->layout) != VK_SUCCESS) {
        delete l;
        return nullptr;
    }
    return l;
}

void Device::destroy(BindGroupLayout* l) {
    if (!l) return;
    vkDestroyDescriptorSetLayout(m_ctx->device(), l->layout, nullptr);
    delete l;
}

BindGroup* Device::createBindGroup(BindGroupLayout* layout, const std::vector<BindGroupEntry>& entries) {
    VkDevice dev = m_ctx->device();
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(layout->entries.size())};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    auto* g = new BindGroup;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &g->pool) != VK_SUCCESS) {
        delete g;
        return nullptr;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = g->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout->layout;
    if (vkAllocateDescriptorSets(dev, &ai, &g->set) != VK_SUCCESS) {
        vkDestroyDescriptorPool(dev, g->pool, nullptr);
        delete g;
        return nullptr;
    }
    std::vector<VkDescriptorImageInfo> images(entries.size());
    std::vector<VkWriteDescriptorSet> writes(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        images[i] = {entries[i].sampler ? entries[i].sampler->sampler : VK_NULL_HANDLE,
                     entries[i].texture ? entries[i].texture->view : VK_NULL_HANDLE,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g->set;
        writes[i].dstBinding = entries[i].binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
    return g;
}

void Device::destroy(BindGroup* g) {
    if (!g) return;
    vkDestroyDescriptorPool(m_ctx->device(), g->pool, nullptr);
    delete g;
}

// ---------------------------------------------------------------- pipelines

RenderPipeline* Device::createRenderPipeline(const RenderPipelineDesc& desc) {
    VkDevice dev = m_ctx->device();
    auto* p = new RenderPipeline;
    p->pushStages = vkStages(desc.pushStages);

    std::vector<VkDescriptorSetLayout> sets;
    for (BindGroupLayout* l : desc.bindGroups) sets.push_back(l->layout);
    VkPushConstantRange range{p->pushStages, 0, desc.pushSize};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = uint32_t(sets.size());
    pli.pSetLayouts = sets.data();
    pli.pushConstantRangeCount = desc.pushSize > 0 ? 1 : 0;
    pli.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &p->layout) != VK_SUCCESS) {
        delete p;
        return nullptr;
    }

    VkShaderModule vert = loadModule(dev, desc.vertexShader);
    VkShaderModule frag = loadModule(dev, desc.fragmentShader);
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(dev, vert, nullptr);
        if (frag) vkDestroyShaderModule(dev, frag, nullptr);
        destroy(p);
        return nullptr;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    if (desc.liteSpecialization) stages[1].pSpecializationInfo = m_ctx->liteSpecialization();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = desc.topology == Topology::TriangleStrip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                  : desc.topology == Topology::LineList   ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // Viewport e recorte sempre dinâmicos (o passe define).
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = desc.cull == CullMode::Front  ? VK_CULL_MODE_FRONT_BIT
                  : desc.cull == CullMode::Back ? VK_CULL_MODE_BACK_BIT
                                                : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    std::vector<VkPipelineColorBlendAttachmentState> blends(desc.colorFormats.size());
    for (auto& b : blends) {
        b = {};
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = uint32_t(blends.size());
    cb.pAttachments = blends.data();
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    di.dynamicStateCount = 2;
    di.pDynamicStates = dyn;
    std::vector<VkFormat> colors;
    for (Format f : desc.colorFormats) colors.push_back(toVulkan(f));
    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = uint32_t(colors.size());
    ri.pColorAttachmentFormats = colors.data();
    ri.depthAttachmentFormat = toVulkan(desc.depthFormat);

    VkGraphicsPipelineCreateInfo gi{};
    gi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vertexInput;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &di;
    gi.layout = p->layout;
    vkc::createGraphicsPipelines(dev, m_ctx->pipelineCache(), 1, &gi, nullptr, &p->pipeline);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    if (p->pipeline == VK_NULL_HANDLE) {
        destroy(p);
        return nullptr;
    }
    return p;
}

void Device::destroy(RenderPipeline* p) {
    if (!p) return;
    if (p->pipeline) vkDestroyPipeline(m_ctx->device(), p->pipeline, nullptr);
    if (p->layout) vkDestroyPipelineLayout(m_ctx->device(), p->layout, nullptr);
    delete p;
}

// ---------------------------------------------------------------- comandos

void CommandList::beginRenderPass(const RenderPassDesc& desc) {
    std::vector<VkRenderingAttachmentInfo> colors(desc.colors.size());
    m_passTargets.clear();
    for (size_t i = 0; i < desc.colors.size(); ++i) {
        const ColorTarget& c = desc.colors[i];
        Texture* t = c.texture;
        m_passTargets.push_back(t);
        if (t->rest != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            // Conteúdo descartado (DontCare/Clear): parte de UNDEFINED.
            const bool keep = c.load == LoadOp::Load;
            imageBarrier(m_cmd, t->image, keep ? t->rest : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         keep ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         keep ? VK_ACCESS_2_SHADER_READ_BIT : 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        colors[i] = {};
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = t->view;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = vkLoad(c.load);
        colors[i].storeOp = c.store == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        for (int k = 0; k < 4; ++k) colors[i].clearValue.color.float32[k] = c.clear[k];
    }
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {desc.width, desc.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = uint32_t(colors.size());
    info.pColorAttachments = colors.data();
    vkc::cmdBeginRendering(m_cmd, &info);
    VkViewport viewport{0.0f, 0.0f, float(desc.width), float(desc.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {desc.width, desc.height}};
    vkCmdSetViewport(m_cmd, 0, 1, &viewport);
    vkCmdSetScissor(m_cmd, 0, 1, &scissor);
}

void CommandList::endRenderPass() {
    vkc::cmdEndRendering(m_cmd);
    for (Texture* t : m_passTargets) {
        if (t->rest != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;
        imageBarrier(m_cmd, t->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
    m_passTargets.clear();
}

void CommandList::setPipeline(RenderPipeline* pipeline) {
    m_pipeline = pipeline;
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
}

void CommandList::setBindGroup(uint32_t index, BindGroup* group) {
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->layout, index, 1, &group->set, 0,
                            nullptr);
}

void CommandList::setPush(const void* data, uint32_t size) {
    vkCmdPushConstants(m_cmd, m_pipeline->layout, m_pipeline->pushStages, 0, size, data);
}

void CommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex) {
    vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, 0);
}

} // namespace eruption::rhi

