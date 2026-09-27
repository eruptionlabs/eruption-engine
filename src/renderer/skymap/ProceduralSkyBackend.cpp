#include "renderer/skymap/ProceduralSkyBackend.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <cstdlib>

namespace eruption {

struct SkyParamsUBO {
    Vec4 skyTop;            // xyz = topColor, w = timeOfDay
    Vec4 skyHorizon;        // xyz = horizonColor, w = starDensity
    Vec4 sunDirIntensity;   // xyz = sunDir, w = sunIntensity
    Vec4 moonDirIntensity;  // xyz = moonDir, w = moonIntensity
    Vec4 sunColor;
    Vec4 moonColor;
    Vec4 proceduralParams;  // x=cloudCoverage, y=cloudSpeed, z=cloudScale, w=cloudLightness
    Vec4 proceduralParams2; // x=cloudShade, y=sunRayCount, z=moonPhaseOffset, w=sunSize
    Vec4 proceduralParams3; // x=moonSize, y=enableStars, z=enableClouds, w=starTwinkleSpeed
    Vec4 proceduralParams4; // x=cloudSoftness, y=cloudThickness, z=stormTint, w=sunLimbDarkening
    Vec4 proceduralParams5; // x=sunHaloIntensity, y=sunHaloRays, z=sunHaloSize, w=moonPhase
    Vec4 proceduralParams6; // x=moonPhaseAuto
    Mat4 invViewProj;
};
static_assert(sizeof(SkyParamsUBO) <= 256, "SkyParamsUBO too large");

ProceduralSkyBackend::ProceduralSkyBackend(const SkyConfig& config) : m_config(config) {}

bool ProceduralSkyBackend::init(VulkanContext* ctx, uint32_t width, uint32_t height) {
    m_ctx = ctx;
    m_width = width;
    m_height = height;
    createOutputImage();
    createDepthImage();
    createUBO();
    createPipeline();
    return true;
}

void ProceduralSkyBackend::shutdown() {
    if (!m_ctx) return;
    vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
    vkDestroyPipelineLayout(m_ctx->device(), m_layout, nullptr);
    destroyUBO();
    vkDestroySampler(m_ctx->device(), m_outputSampler, nullptr);
    vkDestroyImageView(m_ctx->device(), m_outputView, nullptr);
    vmaDestroyImage(m_ctx->allocator(), m_outputImage, m_outputAlloc);
    if (m_depthView) vkDestroyImageView(m_ctx->device(), m_depthView, nullptr);
    if (m_depthImage) vmaDestroyImage(m_ctx->allocator(), m_depthImage, m_depthAlloc);
    m_depthView = VK_NULL_HANDLE;
    m_depthImage = VK_NULL_HANDLE;
    m_depthAlloc = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

void ProceduralSkyBackend::resize(uint32_t width, uint32_t height) {
    VulkanContext* ctx = m_ctx;
    shutdown();
    init(ctx, width, height);
}

void ProceduralSkyBackend::createOutputImage() {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_outputImage, &m_outputAlloc, nullptr);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_outputImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_outputView);

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_outputSampler);
}

void ProceduralSkyBackend::createDepthImage() {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_depthImage, &m_depthAlloc, nullptr);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_depthView);
}

void ProceduralSkyBackend::createUBO() {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(SkyParamsUBO);
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    allocInfo.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    vmaCreateBuffer(m_ctx->allocator(), &bufferInfo, &allocInfo, &m_uboBuffer, &m_uboAlloc, nullptr);
    vmaMapMemory(m_ctx->allocator(), m_uboAlloc, &m_uboMapped);

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_uboLayout);

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = 1 + kProbeFaces;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1 + kProbeFaces;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_uboPool);

    VkDescriptorSetAllocateInfo allocSetInfo = {};
    allocSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocSetInfo.descriptorPool = m_uboPool;
    allocSetInfo.descriptorSetCount = 1;
    allocSetInfo.pSetLayouts = &m_uboLayout;
    vkAllocateDescriptorSets(m_ctx->device(), &allocSetInfo, &m_uboSet);

    VkDescriptorBufferInfo descBuffer = {};
    descBuffer.buffer = m_uboBuffer;
    descBuffer.offset = 0;
    descBuffer.range = sizeof(SkyParamsUBO);

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_uboSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &descBuffer;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);

    // Sonda de ceu (G36): 6 UBOs/sets, um por face - as faces sao gravadas
    // no MESMO command buffer, e um UBO host-mapped so' teria o valor da
    // ultima quando a GPU executasse.
    for (uint32_t f = 0; f < kProbeFaces; ++f) {
        vmaCreateBuffer(m_ctx->allocator(), &bufferInfo, &allocInfo, &m_probeUbo[f], &m_probeUboAlloc[f], nullptr);
        vmaMapMemory(m_ctx->allocator(), m_probeUboAlloc[f], &m_probeUboMapped[f]);
        vkAllocateDescriptorSets(m_ctx->device(), &allocSetInfo, &m_probeSets[f]);
        VkDescriptorBufferInfo pb = {};
        pb.buffer = m_probeUbo[f];
        pb.offset = 0;
        pb.range = sizeof(SkyParamsUBO);
        VkWriteDescriptorSet pw = write;
        pw.dstSet = m_probeSets[f];
        pw.pBufferInfo = &pb;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &pw, 0, nullptr);
    }
}

void ProceduralSkyBackend::destroyUBO() {
    if (m_uboMapped) {
        vmaUnmapMemory(m_ctx->allocator(), m_uboAlloc);
        m_uboMapped = nullptr;
    }
    for (uint32_t f = 0; f < kProbeFaces; ++f) {
        if (m_probeUboMapped[f]) { vmaUnmapMemory(m_ctx->allocator(), m_probeUboAlloc[f]); m_probeUboMapped[f] = nullptr; }
        if (m_probeUbo[f] != VK_NULL_HANDLE) { vmaDestroyBuffer(m_ctx->allocator(), m_probeUbo[f], m_probeUboAlloc[f]); m_probeUbo[f] = VK_NULL_HANDLE; }
    }
    vkDestroyDescriptorPool(m_ctx->device(), m_uboPool, nullptr);
    vkDestroyDescriptorSetLayout(m_ctx->device(), m_uboLayout, nullptr);
    vmaDestroyBuffer(m_ctx->allocator(), m_uboBuffer, m_uboAlloc);
}

void ProceduralSkyBackend::createPipeline() {
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_uboLayout;
    vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_layout);

    auto vert = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");
    auto frag = ShaderCompiler::loadSPIRV("skybox/skybox.frag.spv");

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vert.size() * sizeof(uint32_t);
    smInfo.pCode = vert.data();
    VkShaderModule vertModule;
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = frag.size() * sizeof(uint32_t);
    smInfo.pCode = frag.data();
    VkShaderModule fragModule;
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertInput = {};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAsm = {};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp = {0, 0, (float)m_width, (float)m_height, 0, 1};
    VkRect2D scissor = {{0, 0}, {m_width, m_height}};
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.pViewports = &vp;
    viewport.scissorCount = 1;
    viewport.pScissors = &scissor;

    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates = dynStates;

    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend = {};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blend;

    VkPipelineRenderingCreateInfo renderingInfo = {};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    renderingInfo.pColorAttachmentFormats = &fmt;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo pipeInfo = {};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = &renderingInfo;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vertInput;
    pipeInfo.pInputAssemblyState = &inputAsm;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pDepthStencilState = &ds;
    pipeInfo.pColorBlendState = &blendState;
    pipeInfo.pDynamicState = &dynState;
    pipeInfo.layout = m_layout;

    vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &m_pipeline);

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);
}

// Parametros do ceu para um frame. Compartilhado entre o skybox de tela, o
// ceu-para-textura (reflexo da agua) e a sonda de ceu (G36).
static void fillSkyParams(SkyParamsUBO& ubo, const SkyConfig& cfg, const DayNightCycle& cycle) {
    ubo = SkyParamsUBO{};
    Vec3 top = cycle.getSkyTopColor();
    Vec3 hor = cycle.getSkyHorizonColor();
    Vec3 sunDir = cycle.getSunVisualDirection();
    Vec3 moonDir = cycle.getMoonVisualDirection();
    Vec3 sunColor = cycle.getSunColor();
    Vec3 moonColor = cycle.getMoonColor();

    ubo.skyTop = Vec4(top, cycle.timeOfDay());
    ubo.skyHorizon = Vec4(hor, cfg.procedural.starDensity);
    ubo.sunDirIntensity = Vec4(sunDir, cycle.getSunIntensity() * 1.5f);
    ubo.moonDirIntensity = Vec4(moonDir, cycle.getMoonIntensity());
    ubo.sunColor = Vec4(sunColor, 1.0f);
    ubo.moonColor = Vec4(moonColor, 1.0f);
    ubo.proceduralParams = Vec4(cfg.procedural.cloudCoverage,
                                cfg.procedural.cloudSpeed,
                                cfg.procedural.cloudScale,
                                cfg.procedural.cloudLightness);
    ubo.proceduralParams2 = Vec4(cfg.procedural.cloudShade,
                                 static_cast<float>(cfg.procedural.sunRayCount),
                                 cfg.procedural.moonPhaseOffset,
                                 cfg.procedural.sunSize);
    ubo.proceduralParams3 = Vec4(cfg.procedural.moonSize,
                                 cfg.procedural.enableStars ? 1.0f : 0.0f,
                                 cfg.procedural.enableClouds ? 1.0f : 0.0f,
                                 cfg.procedural.starTwinkleSpeed);
    float moonPhase = cfg.procedural.moonPhaseAuto ? cycle.getMoonPhase() : cfg.procedural.moonPhase;
    ubo.proceduralParams4 = Vec4(cfg.procedural.cloudSoftness,
                                 cfg.procedural.cloudThickness,
                                 cfg.procedural.stormTint,
                                 cfg.procedural.sunLimbDarkening);
    ubo.proceduralParams5 = Vec4(cfg.procedural.sunHaloIntensity,
                                 cfg.procedural.sunHaloRays,
                                 cfg.procedural.sunHaloSize,
                                 moonPhase);
    // ERUPTION_TEST_SKY_GRADIENT_DEBUG=1: proceduralParams6.y doubles as a
    // debug flag read by skybox.frag to posterize the sky's luminance into
    // contrasting colour bands, so a real (even subtle) vertical gradient
    // shows up as visible stripes and a genuinely flat sky shows as one
    // solid colour - easier to tell apart from a screenshot than eyeballing
    // a smooth gradient directly.
    static const bool skyGradientDebug = std::getenv("ERUPTION_TEST_SKY_GRADIENT_DEBUG") != nullptr;
    ubo.proceduralParams6 = Vec4(cfg.procedural.moonPhaseAuto ? 1.0f : 0.0f,
                                 skyGradientDebug ? 1.0f : 0.0f, 0.0f, 0.0f);
}

void ProceduralSkyBackend::updateUBO(const DayNightCycle& cycle, const Mat4& viewProj) {
    SkyParamsUBO ubo;
    fillSkyParams(ubo, m_config, cycle);
    ubo.invViewProj = glm::inverse(viewProj);
    std::memcpy(m_uboMapped, &ubo, sizeof(ubo));
}

bool ProceduralSkyBackend::renderProbeFace(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& invViewProj,
                                           VkImageView faceView, VkImageView depthView,
                                           uint32_t size, uint32_t face) {
    if (face >= kProbeFaces || !m_probeUboMapped[face] || m_pipeline == VK_NULL_HANDLE) return false;
    SkyParamsUBO ubo;
    fillSkyParams(ubo, m_config, cycle);
    // Sem disco solar, sem halo, sem lua, sem estrelas: a sonda alimenta o
    // AMBIENTE (difuso SH + especular pre-filtrado). O sol ja' entra como luz
    // direcional; um disco de intensidade 1,5x num texel de 32x32 viraria um
    // lobulo L1 falso e um brilho especular duplicado.
    ubo.proceduralParams2.w = 0.0f;                      // sunSize
    ubo.proceduralParams3.x = 0.0f;                      // moonSize
    ubo.proceduralParams3.y = 0.0f;                      // enableStars
    ubo.proceduralParams5.x = 0.0f;                      // sunHaloIntensity
    ubo.invViewProj = invViewProj;
    std::memcpy(m_probeUboMapped[face], &ubo, sizeof(ubo));

    VkRenderingAttachmentInfo colorAttach = {};
    colorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach.imageView = faceView;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingAttachmentInfo depthAttach = {};
    depthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttach.imageView = depthView;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri = {};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {size, size}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAttach;
    ri.pDepthAttachment = &depthAttach;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    VkViewport vp{};
    vp.width = static_cast<float>(size);
    vp.height = static_cast<float>(size);
    vp.minDepth = 1.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {size, size}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_probeSets[face], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    return true;
}

void ProceduralSkyBackend::render(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport vp{};
    vp.x = 0;
    vp.y = 0;
    vp.width = static_cast<float>(m_width);
    vp.height = static_cast<float>(m_height);
    vp.minDepth = 1.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    updateUBO(cycle, viewProj);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_uboSet, 0, nullptr);

    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void ProceduralSkyBackend::renderToTexture(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) {
    VkImageMemoryBarrier barriers[2] = {};
    // Color attachment
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].image = m_outputImage;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Depth attachment
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[1].image = m_depthImage;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);

    VkRenderingAttachmentInfo colorAttach = {};
    colorAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach.imageView = m_outputView;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingAttachmentInfo depthAttach = {};
    depthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttach.imageView = m_depthView;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo = {};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, {m_width, m_height}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttach;
    renderingInfo.pDepthAttachment = &depthAttach;

    vkCmdBeginRendering(cmd, &renderingInfo);
    render(cmd, cycle, viewProj);
    vkCmdEndRendering(cmd);

    barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);
}

Vec3 ProceduralSkyBackend::getSkyColor(const Vec3& direction, const DayNightCycle& cycle) const {
    float t = glm::clamp(direction.y * 0.5f + 0.5f, 0.0f, 1.0f);
    Vec3 top = cycle.getSkyTopColor();
    Vec3 hor = cycle.getSkyHorizonColor();
    return glm::mix(hor, top, t);
}

void ProceduralSkyBackend::reloadConfig(const SkyConfig& config) {
    m_config = config;
}

} // namespace eruption
