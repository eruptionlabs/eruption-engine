#include "renderer/UpscaleAA.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

namespace eruption {

struct FxaaPush {
    Vec2 invResolution;
};

struct UpscalePush {
    Vec2 srcSize;
    Vec2 invSrcSize;
    Vec2 invDstSize;
    float sharpenAmount;
    float _pad;
};

static void barrierColorToShaderRead(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void barrierUndefinedToColorAttach(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void beginPass(VkCommandBuffer cmd, VkImageView view, uint32_t w, uint32_t h) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {w, h}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &info);

    VkViewport viewport{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

bool UpscaleAA::init(VulkanContext* ctx, uint32_t renderWidth, uint32_t renderHeight, VkFormat renderFormat) {
    m_ctx = ctx;
    m_renderWidth = renderWidth;
    m_renderHeight = renderHeight;
    m_renderFormat = renderFormat;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2; // fxaa input + upscale input
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool) != VK_SUCCESS) return false;

    VkDescriptorSetLayout layouts[2] = {m_setLayout, m_setLayout};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts = layouts;
    VkDescriptorSet sets[2];
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, sets) != VK_SUCCESS) return false;
    m_fxaaSet = sets[0];
    m_upscaleSet = sets[1];

    VkPushConstantRange fxaaRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FxaaPush)};
    VkPipelineLayoutCreateInfo fxaaLayoutInfo{};
    fxaaLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    fxaaLayoutInfo.setLayoutCount = 1;
    fxaaLayoutInfo.pSetLayouts = &m_setLayout;
    fxaaLayoutInfo.pushConstantRangeCount = 1;
    fxaaLayoutInfo.pPushConstantRanges = &fxaaRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &fxaaLayoutInfo, nullptr, &m_fxaaLayout) != VK_SUCCESS) return false;

    VkPushConstantRange upscaleRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UpscalePush)};
    VkPipelineLayoutCreateInfo upscaleLayoutInfo = fxaaLayoutInfo;
    upscaleLayoutInfo.pPushConstantRanges = &upscaleRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &upscaleLayoutInfo, nullptr, &m_upscaleLayout) != VK_SUCCESS) return false;

    auto fxaaFrag = ShaderCompiler::loadSPIRV("postprocess/fxaa.frag.spv");
    m_fxaaPipeline = createPipeline(fxaaFrag, m_renderFormat, m_fxaaLayout, m_renderWidth, m_renderHeight);

    auto upscaleFrag = ShaderCompiler::loadSPIRV("postprocess/upscale_sharpen.frag.spv");
    m_upscalePipeline = createPipeline(upscaleFrag, m_ctx->swapFormat(), m_upscaleLayout,
                                       m_ctx->swapExtent().width, m_ctx->swapExtent().height);

    createRenderTarget();
    return m_fxaaPipeline != VK_NULL_HANDLE && m_upscalePipeline != VK_NULL_HANDLE;
}

VkPipeline UpscaleAA::createPipeline(const std::vector<uint32_t>& fragCode, VkFormat colorFormat,
                                    VkPipelineLayout layout, uint32_t w, uint32_t h) {
    auto vert = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkShaderModule vertModule, fragModule;
    smInfo.codeSize = vert.size() * sizeof(uint32_t);
    smInfo.pCode = vert.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);
    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
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

    VkPipelineVertexInputStateCreateInfo vertInput{};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D scissor = {{0, 0}, {w, h}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.pViewports = &vp;
    viewport.scissorCount = 1;
    viewport.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicInfo{};
    dynamicInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicInfo.dynamicStateCount = 2;
    dynamicInfo.pDynamicStates = dynamicStates;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsState{};
    dsState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blend;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = &renderingInfo;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vertInput;
    pipeInfo.pInputAssemblyState = &inputAsm;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pDepthStencilState = &dsState;
    pipeInfo.pColorBlendState = &blendState;
    pipeInfo.pDynamicState = &dynamicInfo;
    pipeInfo.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &pipeline);

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);
    return pipeline;
}

void UpscaleAA::createRenderTarget() {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    m_ctx->createImage(m_renderWidth, m_renderHeight, m_renderFormat, usage, VMA_MEMORY_USAGE_GPU_ONLY,
                       m_fxaaImage, m_fxaaAlloc);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_fxaaImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_renderFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_fxaaView);
    // Escreve UMA VEZ - a view e' estavel ate' o proximo resize (nao ha'
    // ping-pong por frame-in-flight). Reescrever isto TODO FRAME, como a
    // primeira versao fazia, e' exatamente o padrao que dispara
    // VUID-vkUpdateDescriptorSets-None-03047 (set sendo atualizado enquanto
    // um command buffer de frame anterior ainda pode estar lendo dele).
    writeUpscaleSet(m_fxaaView);
}

void UpscaleAA::destroyRenderTarget() {
    if (m_fxaaView != VK_NULL_HANDLE) { vkDestroyImageView(m_ctx->device(), m_fxaaView, nullptr); m_fxaaView = VK_NULL_HANDLE; }
    if (m_fxaaImage != VK_NULL_HANDLE) { vmaDestroyImage(m_ctx->allocator(), m_fxaaImage, m_fxaaAlloc); m_fxaaImage = VK_NULL_HANDLE; }
}

void UpscaleAA::resizeRenderTarget(uint32_t renderWidth, uint32_t renderHeight, VkFormat renderFormat) {
    if (renderWidth == m_renderWidth && renderHeight == m_renderHeight && renderFormat == m_renderFormat) return;
    vkDeviceWaitIdle(m_ctx->device());
    destroyRenderTarget();
    m_renderWidth = renderWidth;
    m_renderHeight = renderHeight;
    m_renderFormat = renderFormat;
    createRenderTarget();

    // O FXAA pipeline tem formato/tamanho fixos na criacao (mesmo padrao das
    // outras pipelines fullscreen da engine - viewport dinamico, mas o
    // PipelineRenderingCreateInfo.colorAttachmentFormats precisa bater com o
    // formato real do render target). Recriar so' se o formato mudou de
    // verdade evita recompilar shader por causa so' de resize de janela.
    vkDestroyPipeline(m_ctx->device(), m_fxaaPipeline, nullptr);
    auto fxaaFrag = ShaderCompiler::loadSPIRV("postprocess/fxaa.frag.spv");
    m_fxaaPipeline = createPipeline(fxaaFrag, m_renderFormat, m_fxaaLayout, m_renderWidth, m_renderHeight);
}

void UpscaleAA::bindSource(VkImageView source) {
    writeFxaaSet(source);
}

void UpscaleAA::writeFxaaSet(VkImageView sourceView) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = m_sampler;
    imgInfo.imageView = sourceView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_fxaaSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
}

void UpscaleAA::writeUpscaleSet(VkImageView fxaaOutputView) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = m_sampler;
    imgInfo.imageView = fxaaOutputView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_upscaleSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
}

void UpscaleAA::render(VkCommandBuffer cmd, VkImageView source, VkImage sourceImage,
                       VkImageView dstView, VkImage dstImage, VkExtent2D dstExtent,
                       bool enableFXAA, float sharpenAmount) {
    // `source` (saida do PostProcessor) chega em SHADER_READ_ONLY_OPTIMAL -
    // e' o layout que o passe de composite do PostProcessor ja' deixa. Ele nao
    // e' lido por nome aqui: quem le' `source` e' o descriptor escrito uma vez
    // em bindSource(), e o upscale sempre le' `m_fxaaView` (comentario abaixo).
    (void)source;

    if (enableFXAA) {
        barrierUndefinedToColorAttach(cmd, m_fxaaImage);
        beginPass(cmd, m_fxaaView, m_renderWidth, m_renderHeight);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaLayout, 0, 1, &m_fxaaSet, 0, nullptr);
        FxaaPush push{Vec2(1.0f / m_renderWidth, 1.0f / m_renderHeight)};
        vkCmdPushConstants(cmd, m_fxaaLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        barrierColorToShaderRead(cmd, m_fxaaImage);
    }

    // Nao ha' write de descriptor aqui: os dois sets sao escritos UMA VEZ
    // (bindSource() e a criacao do render target), nao a cada frame - ver o
    // comentario em createRenderTarget(). Quando FXAA esta' desligado, a
    // entrada logica do upscale seria `source`, mas o descriptor SEMPRE aponta
    // pra `m_fxaaView`: por isso o passe de FXAA roda de qualquer jeito
    // quando enableFXAA=false, so' que como uma copia (nenhum pixel se move,
    // o range check do early-out cobre isso) - custa 1 passe extra barato em
    // vez de manter DOIS descriptors de upscale escritos condicionalmente.
    if (!enableFXAA) {
        barrierUndefinedToColorAttach(cmd, m_fxaaImage);
        beginPass(cmd, m_fxaaView, m_renderWidth, m_renderHeight);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaLayout, 0, 1, &m_fxaaSet, 0, nullptr);
        FxaaPush push{Vec2(1.0f / m_renderWidth, 1.0f / m_renderHeight)};
        vkCmdPushConstants(cmd, m_fxaaLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        barrierColorToShaderRead(cmd, m_fxaaImage);
    }
    // O chamador ja' deixa `dstImage` em COLOR_ATTACHMENT_OPTIMAL (mesma
    // pre-condicao que o blit antigo exigia).
    beginPass(cmd, dstView, dstExtent.width, dstExtent.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upscalePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upscaleLayout, 0, 1, &m_upscaleSet, 0, nullptr);
    UpscalePush push{};
    push.srcSize = Vec2((float)m_renderWidth, (float)m_renderHeight);
    push.invSrcSize = Vec2(1.0f / m_renderWidth, 1.0f / m_renderHeight);
    push.invDstSize = Vec2(1.0f / dstExtent.width, 1.0f / dstExtent.height);
    push.sharpenAmount = sharpenAmount;
    vkCmdPushConstants(cmd, m_upscaleLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    // Deixa dstImage em COLOR_ATTACHMENT_OPTIMAL pro ImGui desenhar em cima -
    // vkCmdEndRendering nao muda o layout sozinho, e o passe escreveu nesse
    // layout o tempo todo (beginPass usa COLOR_ATTACHMENT_OPTIMAL), entao nao
    // precisa de barreira extra aqui.
}

void UpscaleAA::shutdown() {
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();
    destroyRenderTarget();
    if (m_fxaaPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_fxaaPipeline, nullptr);
    if (m_upscalePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_upscalePipeline, nullptr);
    if (m_fxaaLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_fxaaLayout, nullptr);
    if (m_upscaleLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_upscaleLayout, nullptr);
    if (m_descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, m_descPool, nullptr);
    if (m_setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(device, m_sampler, nullptr);
    m_fxaaPipeline = m_upscalePipeline = VK_NULL_HANDLE;
    m_fxaaLayout = m_upscaleLayout = VK_NULL_HANDLE;
    m_descPool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

} // namespace eruption
