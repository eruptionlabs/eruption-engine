#include <chrono>
#include "renderer/WeatherRenderer.hpp"
#include "renderer/PostFormat.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "core/Logger.hpp"
#include <cstring>
#include <vector>
#include <random>
#include <algorithm>

namespace eruption {

// Escala da queda de peso das nuvens de chuva no rateio de gotas/coroas
// (ver updateSplashesForWeather e o laco de fatias): peso ~ 1/(1+d/K)^2.
// K menor concentra mais perto do ancoradouro. ERUPTION_TEST_RAIN_NEAR=<u>.
static const float kRainNearScale = [] {
    const char* e = std::getenv("ERUPTION_TEST_RAIN_NEAR");
    const float v = e ? static_cast<float>(std::atof(e)) : 300.0f;
    return v > 1.0f ? v : 300.0f;
}();


namespace {

VkPipeline createParticlePipeline(VulkanContext* ctx, VkPipelineLayout layout,
                                  const char* vertPath, const char* fragPath,
                                  VkFormat colorFormat) {
    auto vertCode = ShaderCompiler::loadSPIRV(vertPath);
    auto fragCode = ShaderCompiler::loadSPIRV(fragPath);
    if (vertCode.empty() || fragCode.empty()) {
        Logger::error("Failed to load particle shaders: %s / %s", vertPath, fragPath);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkShaderModule vertModule, fragModule;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(ctx->device(), &smInfo, nullptr, &vertModule);
    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(ctx->device(), &smInfo, nullptr, &fragModule);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(WeatherParticleGPU);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attrs[3] = {};
    attrs[0].binding = 0; attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[0].offset = offsetof(WeatherParticleGPU, origin);
    attrs[1].binding = 0; attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = offsetof(WeatherParticleGPU, velocity);
    attrs[2].binding = 0; attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[2].offset = offsetof(WeatherParticleGPU, data);

    VkPipelineVertexInputStateCreateInfo vertInput{};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertInput.vertexBindingDescriptionCount = 1;
    vertInput.pVertexBindingDescriptions = &binding;
    vertInput.vertexAttributeDescriptionCount = 3;
    vertInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkViewport vp = {0, 0, 1.0f, 1.0f, 0, 1};
    VkRect2D scissor = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1; viewport.pViewports = &vp;
    viewport.scissorCount = 1; viewport.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynStates;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blend;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &renderingInfo;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertInput;
    info.pInputAssemblyState = &inputAsm;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blendState;
    info.pDynamicState = &dynamic;
    info.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkc::createGraphicsPipelines(ctx->device(), ctx->pipelineCache(), 1, &info, nullptr, &pipeline);

    vkDestroyShaderModule(ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(ctx->device(), fragModule, nullptr);
    return pipeline;
}

// Fullscreen rain-occlusion debug map (DoF-visualizeCoC style): alpha-blended
// fullscreen triangle, no vertex inputs. Own layout: the shared descriptor
// layout (bindings 1=scene depth, 3=coverage) + a 128-byte fragment push.
VkPipeline createDebugMapPipeline(VulkanContext* ctx, VkDescriptorSetLayout descLayout,
                                  VkPipelineLayout* outLayout,
                                  const char* vertPath, const char* fragPath,
                                  VkFormat colorFormat) {
    auto vertCode = ShaderCompiler::loadSPIRV(vertPath);
    auto fragCode = ShaderCompiler::loadSPIRV(fragPath);
    if (vertCode.empty() || fragCode.empty()) {
        Logger::error("Failed to load debug occlusion map shaders: %s / %s", vertPath, fragPath);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkShaderModule vertModule, fragModule;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(ctx->device(), &smInfo, nullptr, &vertModule);
    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(ctx->device(), &smInfo, nullptr, &fragModule);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(Mat4) + sizeof(Vec4) * 4; // invViewProj + cameraPos + box + covBounds + misc

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &descLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(ctx->device(), &plInfo, nullptr, outLayout);

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

    VkViewport vp = {0, 0, 1.0f, 1.0f, 0, 1};
    VkRect2D scissor = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1; viewport.pViewports = &vp;
    viewport.scissorCount = 1; viewport.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynStates;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blend;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &renderingInfo;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertInput;
    info.pInputAssemblyState = &inputAsm;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blendState;
    info.pDynamicState = &dynamic;
    info.layout = *outLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkc::createGraphicsPipelines(ctx->device(), ctx->pipelineCache(), 1, &info, nullptr, &pipeline);

    vkDestroyShaderModule(ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(ctx->device(), fragModule, nullptr);
    return pipeline;
}

VkPipeline createSplashPipeline(VulkanContext* ctx, VkPipelineLayout layout,
                                const char* vertPath, const char* fragPath,
                                VkFormat colorFormat) {
    auto vertCode = ShaderCompiler::loadSPIRV(vertPath);
    auto fragCode = ShaderCompiler::loadSPIRV(fragPath);
    if (vertCode.empty() || fragCode.empty()) {
        Logger::error("Failed to load splash shaders: %s / %s", vertPath, fragPath);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkShaderModule vertModule, fragModule;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(ctx->device(), &smInfo, nullptr, &vertModule);
    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(ctx->device(), &smInfo, nullptr, &fragModule);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(WeatherSplashGPU);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].binding = 0; attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[0].offset = offsetof(WeatherSplashGPU, origin);
    attrs[1].binding = 0; attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = offsetof(WeatherSplashGPU, data);

    VkPipelineVertexInputStateCreateInfo vertInput{};
    vertInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertInput.vertexBindingDescriptionCount = 1;
    vertInput.pVertexBindingDescriptions = &binding;
    vertInput.vertexAttributeDescriptionCount = 2;
    vertInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkViewport vp = {0, 0, 1.0f, 1.0f, 0, 1};
    VkRect2D scissor = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1; viewport.pViewports = &vp;
    viewport.scissorCount = 1; viewport.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynStates;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blend;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &renderingInfo;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertInput;
    info.pInputAssemblyState = &inputAsm;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blendState;
    info.pDynamicState = &dynamic;
    info.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkc::createGraphicsPipelines(ctx->device(), ctx->pipelineCache(), 1, &info, nullptr, &pipeline);

    vkDestroyShaderModule(ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(ctx->device(), fragModule, nullptr);
    return pipeline;
}

} // anonymous namespace

bool WeatherRenderer::init(VulkanContext* ctx) {
    m_ctx = ctx;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_linearSampler);

    if (!createParticleBuffer()) return false;
    if (!createSplashBuffer()) return false;
    if (!createDescriptors()) return false;
    if (!createHeightmapResources()) return false;
    if (!createTopDownDepthResources()) return false;
    if (!createHeightmapPipeline()) return false;
    if (!createPipelines()) return false;

    return true;
}

void WeatherRenderer::shutdown() {
    if (!m_ctx) return;
    auto device = m_ctx->device();

    vkDestroyPipeline(device, m_particlePipeline, nullptr);
    vkDestroyPipeline(device, m_splashPipeline, nullptr);
    vkDestroyPipeline(device, m_debugMapPipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, m_debugMapLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, m_descLayout, nullptr);
    vkDestroyDescriptorPool(device, m_descPool, nullptr);
    vkDestroySampler(device, m_linearSampler, nullptr);

    if (m_particleBuffer) vmaDestroyBuffer(m_ctx->allocator(), m_particleBuffer, m_particleAlloc);
    if (m_splashBuffer) vmaDestroyBuffer(m_ctx->allocator(), m_splashBuffer, m_splashAlloc);
    if (m_rainOccMapped) {
        vmaUnmapMemory(m_ctx->allocator(), m_rainOccAlloc);
        m_rainOccMapped = nullptr;
    }
    if (m_rainOccBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_rainOccBuffer, m_rainOccAlloc);
        m_rainOccBuffer = VK_NULL_HANDLE;
        m_rainOccAlloc = VK_NULL_HANDLE;
    }
    if (m_tuneMapped) {
        vmaUnmapMemory(m_ctx->allocator(), m_tuneAlloc);
        m_tuneMapped = nullptr;
    }
    if (m_tuneBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_tuneBuffer, m_tuneAlloc);
        m_tuneBuffer = VK_NULL_HANDLE;
        m_tuneAlloc = VK_NULL_HANDLE;
    }

    destroyTopDownDepthResources();
    destroyHeightmapResources();

    m_ctx = nullptr;
}

bool WeatherRenderer::createParticleBuffer() {
    // Buffer starts tiny and grows on demand in updateParticlesForWeather().
    m_particleBufferCapacity = 1;
    VkDeviceSize size = m_particleBufferCapacity * sizeof(WeatherParticleGPU);
    if (!m_ctx->createBuffer(size,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY,
                             m_particleBuffer, m_particleAlloc)) return false;
    return true;
}

bool WeatherRenderer::createSplashBuffer() {
    VkDeviceSize size = MAX_SPLASH_PARTICLES * sizeof(WeatherSplashGPU);
    if (!m_ctx->createBuffer(size,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY,
                             m_splashBuffer, m_splashAlloc)) return false;
    return true;
}

bool WeatherRenderer::createDescriptors() {
    VkDescriptorSetLayoutBinding bindings[5] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // Per-pixel cloud altitude map (R16) used to clip precipitation above clouds.
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding occBinding{};
    occBinding.binding = 5;
    occBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    occBinding.descriptorCount = 1;
    occBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Per-follower coverage arrays for exact cross-cloud silhouettes.
    VkDescriptorSetLayoutBinding covArraysBinding{};
    covArraysBinding.binding = 6;
    covArraysBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    covArraysBinding.descriptorCount = MAX_RAIN_OCCLUDERS;
    covArraysBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Rain streak shape tuning (2 vec4s; see WeatherParams rainStreak*).
    VkDescriptorSetLayoutBinding tuneBinding{};
    tuneBinding.binding = 7;
    tuneBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    tuneBinding.descriptorCount = 1;
    tuneBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding allBindings[8] = {bindings[0], bindings[1], bindings[2],
                                                   bindings[3], bindings[4], occBinding,
                                                   covArraysBinding, tuneBinding};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 8;
    layoutInfo.pBindings = allBindings;
    vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_descLayout);

    // Cross-cloud rain occluders UBO (geom + coverage bounds + wind per cloud).
    m_ctx->createBuffer(3 * MAX_RAIN_OCCLUDERS * sizeof(Vec4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VMA_MEMORY_USAGE_CPU_TO_GPU, m_rainOccBuffer, m_rainOccAlloc);
    if (m_rainOccBuffer != VK_NULL_HANDLE)
        vmaMapMemory(m_ctx->allocator(), m_rainOccAlloc, &m_rainOccMapped);

    // Streak tuning UBO.
    m_ctx->createBuffer(2 * sizeof(Vec4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VMA_MEMORY_USAGE_CPU_TO_GPU, m_tuneBuffer, m_tuneAlloc);
    if (m_tuneBuffer != VK_NULL_HANDLE)
        vmaMapMemory(m_ctx->allocator(), m_tuneAlloc, &m_tuneMapped);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(Mat4) + sizeof(Vec4) * 12; // 256 bytes (added occluder)

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_descLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &plInfo, nullptr, &m_pipelineLayout);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // 4 base sets + up to MAX_RAIN_OCCLUDERS per-follower sets; each set
    // carries 5 sampler bindings + MAX_RAIN_OCCLUDERS cross-cloud coverage
    // array entries. (maxSets previously capped at 16, so followers past the
    // 12th silently failed allocation and drew with the wrong cloud's
    // coverage bound.)
    poolSizes[0].descriptorCount = (4 + MAX_RAIN_OCCLUDERS) * (5 + MAX_RAIN_OCCLUDERS);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 2 * (4 + MAX_RAIN_OCCLUDERS); // occluders + tuning UBO per set

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 4 + MAX_RAIN_OCCLUDERS;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool);

    m_descSets.resize(4);
    std::vector<VkDescriptorSetLayout> layouts(4, m_descLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 4;
    allocInfo.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, m_descSets.data());

    return true;
}

bool WeatherRenderer::createPipelines() {
    m_particlePipeline = createParticlePipeline(m_ctx, m_pipelineLayout,
                                                 "weather/particle_render.vert.spv",
                                                 "weather/particle_render.frag.spv",
                                                 postColorFormat(m_ctx->physicalDevice()));
    m_splashPipeline = createSplashPipeline(m_ctx, m_pipelineLayout,
                                             "weather/splash_render.vert.spv",
                                             "weather/splash_render.frag.spv",
                                             postColorFormat(m_ctx->physicalDevice()));
    m_debugMapPipeline = createDebugMapPipeline(m_ctx, m_descLayout, &m_debugMapLayout,
                                                 "weather/debug_occ_map.vert.spv",
                                                 "weather/debug_occ_map.frag.spv",
                                                 postColorFormat(m_ctx->physicalDevice()));
    return m_particlePipeline != VK_NULL_HANDLE && m_splashPipeline != VK_NULL_HANDLE &&
           m_debugMapPipeline != VK_NULL_HANDLE;
}

bool WeatherRenderer::createHeightmapResources() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {HEIGHTMAP_SIZE, HEIGHTMAP_SIZE, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R32_UINT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    // Raw heightmap + blurred ping-pong target.
    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_heightmapImage, &m_heightmapAlloc, nullptr);
    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_heightmapBlurImage, &m_heightmapBlurAlloc, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_UINT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    viewInfo.image = m_heightmapImage;
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_heightmapView);
    viewInfo.image = m_heightmapBlurImage;
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_heightmapBlurView);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_heightmapSampler);

    // Clear both images to 0 and leave them in SHADER_READ_ONLY_OPTIMAL so they
    // are safe to read before the first update dispatch.
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkClearColorValue clearVal{};
        clearVal.uint32[0] = 0u;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (VkImage img : {m_heightmapImage, m_heightmapBlurImage}) {
            m_ctx->cmdImageBarrier(cmd, img,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &range);
            m_ctx->cmdImageBarrier(cmd, img,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    });

    return true;
}

void WeatherRenderer::destroyHeightmapResources() {
    if (!m_ctx) return;
    vkDestroyPipeline(m_ctx->device(), m_heightmapBlurPipeline, nullptr);
    m_heightmapBlurPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(m_ctx->device(), m_heightmapUpdatePipeline, nullptr);
    m_heightmapUpdatePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(m_ctx->device(), m_heightmapPipelineLayout, nullptr);
    m_heightmapPipelineLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorPool(m_ctx->device(), m_heightmapDescPool, nullptr);
    m_heightmapDescPool = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(m_ctx->device(), m_heightmapDescLayout, nullptr);
    m_heightmapDescLayout = VK_NULL_HANDLE;
    vkDestroySampler(m_ctx->device(), m_heightmapSampler, nullptr);
    m_heightmapSampler = VK_NULL_HANDLE;
    vkDestroyImageView(m_ctx->device(), m_heightmapBlurView, nullptr);
    m_heightmapBlurView = VK_NULL_HANDLE;
    if (m_heightmapBlurImage) vmaDestroyImage(m_ctx->allocator(), m_heightmapBlurImage, m_heightmapBlurAlloc);
    m_heightmapBlurImage = VK_NULL_HANDLE;
    m_heightmapBlurAlloc = VK_NULL_HANDLE;
    vkDestroyImageView(m_ctx->device(), m_heightmapView, nullptr);
    m_heightmapView = VK_NULL_HANDLE;
    if (m_heightmapImage) vmaDestroyImage(m_ctx->allocator(), m_heightmapImage, m_heightmapAlloc);
    m_heightmapImage = VK_NULL_HANDLE;
    m_heightmapAlloc = VK_NULL_HANDLE;
    m_heightmapUpdateDescSet = VK_NULL_HANDLE;
    m_heightmapBlurDescSet = VK_NULL_HANDLE;
}

bool WeatherRenderer::createTopDownDepthResources() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {HEIGHTMAP_SIZE, HEIGHTMAP_SIZE, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_topDownDepthImage, &m_topDownDepthAlloc, nullptr) != VK_SUCCESS) {
        Logger::error("Failed to create top-down rain depth image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.image = m_topDownDepthImage;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_topDownDepthView);

    return m_topDownDepthView != VK_NULL_HANDLE;
}

void WeatherRenderer::destroyTopDownDepthResources() {
    if (!m_ctx) return;
    if (m_topDownDepthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_topDownDepthView, nullptr);
        m_topDownDepthView = VK_NULL_HANDLE;
    }
    if (m_topDownDepthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_topDownDepthImage, m_topDownDepthAlloc);
        m_topDownDepthImage = VK_NULL_HANDLE;
        m_topDownDepthAlloc = VK_NULL_HANDLE;
    }
}

void WeatherRenderer::renderTopDownDepth(VkCommandBuffer cmd,
                                          const Vec3& cameraPos,
                                          TerrainRenderer& terrain,
                                          ModelRenderer& models,
                                          SpriteRenderer& sprites,
                                          VkPipeline shadowPipeline,
                                          VkPipelineLayout shadowLayout,
                                          BindlessDescriptor* bindless,
                                          const FrameUBO& frameUbo,
                                          VkBuffer spriteInstanceBuffer,
                                          uint32_t spriteCount) {
    if (!m_ctx || shadowPipeline == VK_NULL_HANDLE || shadowLayout == VK_NULL_HANDLE) return;

    // The top-down depth pass re-renders terrain/models/sprites into a 1024²
    // target. Only run it every N frames; the heightmap compute pass can reuse
    // the previous frame's depth target (it stays in SHADER_READ_ONLY_OPTIMAL).
    if ((++m_topDownFrameCounter % TOPDOWN_DEPTH_INTERVAL) != 0) return;

    const float worldHalf = HEIGHTMAP_WORLD_SIZE * 0.5f;
    Vec3 eye = cameraPos + Vec3(0.0f, 5000.0f, 0.0f);
    Mat4 rainView = glm::lookAt(eye, cameraPos, Vec3(0.0f, 0.0f, -1.0f));
    Mat4 rainProj = glm::ortho(-worldHalf, worldHalf, -worldHalf, worldHalf, 1.0f, 10000.0f);
    rainProj[1][1] *= -1.0f; // Vulkan Y-flip
    Mat4 rainViewProj = rainProj * rainView;

    // Transition depth target to attachment-optimal (contents discarded; we clear).
    m_ctx->cmdImageBarrier(cmd, m_topDownDepthImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = m_topDownDepthView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    m_ctx->cmdBeginRendering(cmd, {}, &depthAttachment, nullptr, {HEIGHTMAP_SIZE, HEIGHTMAP_SIZE});

    VkViewport viewport{0.0f, 0.0f, (float)HEIGHTMAP_SIZE, (float)HEIGHTMAP_SIZE, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {HEIGHTMAP_SIZE, HEIGHTMAP_SIZE}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);

    if (bindless) {
        VkDescriptorSet bindlessSet = bindless->set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout,
                                0, 1, &bindlessSet, 0, nullptr);
    }

    Frustum frustum;
    frustum.extractFromMatrix(rainViewProj);

    if (terrain.isInitialized()) {
        terrain.renderShadow(cmd, shadowPipeline, shadowLayout, rainViewProj, frustum, true);
    }

    if (models.isInitialized()) {
        models.renderShadow(cmd, shadowPipeline, shadowLayout, rainViewProj, frustum, 0.0f, true);
    }

    if (spriteCount > 0) {
        sprites.renderShadow(cmd, shadowLayout, rainViewProj, frameUbo, spriteInstanceBuffer, spriteCount);
    }

    m_ctx->cmdEndRendering(cmd);

    // Make the depth target readable by the heightmap compute pass.
    m_ctx->cmdImageBarrier(cmd, m_topDownDepthImage,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
}

bool WeatherRenderer::createHeightmapPipeline() {
    auto loadModule = [&](const char* path, VkShaderModule& outModule) -> bool {
        auto code = ShaderCompiler::loadSPIRV(path);
        if (code.empty()) {
            Logger::error("Failed to load compute shader: %s", path);
            return false;
        }
        VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smInfo.codeSize = code.size() * sizeof(uint32_t);
        smInfo.pCode = code.data();
        vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &outModule);
        return true;
    };

    VkShaderModule updateModule = VK_NULL_HANDLE;
    VkShaderModule blurModule = VK_NULL_HANDLE;
    if (!loadModule("weather/heightmap_update.comp.spv", updateModule)) return false;
    if (!loadModule("weather/heightmap_blur.comp.spv", blurModule)) return false;

    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_heightmapDescLayout);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = 128;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_heightmapDescLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &plInfo, nullptr, &m_heightmapPipelineLayout);

    auto createComputePipeline = [&](VkShaderModule module, VkPipeline& pipeline) {
        VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeInfo.stage.module = module;
        pipeInfo.stage.pName = "main";
        pipeInfo.layout = m_heightmapPipelineLayout;
        vkCreateComputePipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &pipeline);
    };

    createComputePipeline(updateModule, m_heightmapUpdatePipeline);
    createComputePipeline(blurModule, m_heightmapBlurPipeline);

    vkDestroyShaderModule(m_ctx->device(), updateModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), blurModule, nullptr);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_heightmapDescPool);

    VkDescriptorSetLayout layouts[2] = {m_heightmapDescLayout, m_heightmapDescLayout};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_heightmapDescPool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts = layouts;
    VkDescriptorSet sets[2] = {};
    vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, sets);
    m_heightmapUpdateDescSet = sets[0];
    m_heightmapBlurDescSet = sets[1];

    return true;
}

void WeatherRenderer::updateHeightmapOnly(VkCommandBuffer cmd, VkImageView depthView, const RenderParams& params) {
    updateHeightmap(cmd, depthView, params);
}

void WeatherRenderer::updateHeightmap(VkCommandBuffer cmd, VkImageView depthView, const RenderParams& params) {
    if (!m_heightmapUpdatePipeline || depthView == VK_NULL_HANDLE) return;

    // 1) Clear the heightmap to 0 (transfer -> compute).
    m_ctx->cmdImageBarrier(cmd, m_heightmapImage,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkClearColorValue clearVal{};
    clearVal.uint32[0] = 0u;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, m_heightmapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &range);

    m_ctx->cmdImageBarrier(cmd, m_heightmapImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    // 2) Update descriptor set: top-down orthographic depth -> heightmap.
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthInfo.imageView = depthView;
    depthInfo.sampler = m_linearSampler;

    VkDescriptorImageInfo currInfo{};
    currInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    currInfo.imageView = m_heightmapView;

    VkWriteDescriptorSet updateWrites[2] = {};
    updateWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    updateWrites[0].dstSet = m_heightmapUpdateDescSet;
    updateWrites[0].dstBinding = 0;
    updateWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    updateWrites[0].descriptorCount = 1;
    updateWrites[0].pImageInfo = &depthInfo;

    updateWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    updateWrites[1].dstSet = m_heightmapUpdateDescSet;
    updateWrites[1].dstBinding = 1;
    updateWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    updateWrites[1].descriptorCount = 1;
    updateWrites[1].pImageInfo = &currInfo;

    vkUpdateDescriptorSets(m_ctx->device(), 2, updateWrites, 0, nullptr);

    // 3) Dispatch one thread per heightmap texel.
    struct HMPush {
        Mat4 rainInvViewProj;
        Vec4 params;     // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeZ
        Vec4 screenSize; // xy=heightmap size, zw=inv size
    } pc;

    const float worldSize = HEIGHTMAP_WORLD_SIZE;
    const float worldHalf = worldSize * 0.5f;
    // Anchor to the character (orbit target), not the camera — see
    // RenderParams::rainAnchor. Must match the top-down depth eye and every
    // worldBounds pushed to the particle/splash/overlay shaders.
    const Vec3 hmAnchor = (params.rainAnchor.y > -1.0e8f) ? params.rainAnchor : params.cameraPos;
    Vec3 eye = hmAnchor + Vec3(0.0f, 5000.0f, 0.0f);
    Mat4 rainView = glm::lookAt(eye, hmAnchor, Vec3(0.0f, 0.0f, -1.0f));
    Mat4 rainProj = glm::ortho(-worldHalf, worldHalf, -worldHalf, worldHalf, 1.0f, 10000.0f);
    rainProj[1][1] *= -1.0f; // Vulkan Y-flip
    pc.rainInvViewProj = glm::inverse(rainProj * rainView);
    pc.params = Vec4(hmAnchor.x - worldHalf,
                     hmAnchor.z - worldHalf,
                     worldSize,
                     worldSize);
    pc.screenSize = Vec4((float)HEIGHTMAP_SIZE, (float)HEIGHTMAP_SIZE,
                         1.0f / (float)HEIGHTMAP_SIZE, 1.0f / (float)HEIGHTMAP_SIZE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_heightmapUpdatePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_heightmapPipelineLayout, 0, 1, &m_heightmapUpdateDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_heightmapPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t groups = (HEIGHTMAP_SIZE + 15) / 16;
    vkCmdDispatch(cmd, groups, groups, 1);

    // 4) Blur passes: smooth the raw heights with a controllable amount of
    // 3x3 box-blur iterations. More iterations => softer rain-shadow edges.
    // Always finish with the result in m_heightmapBlurImage.
    m_ctx->cmdImageBarrier(cmd, m_heightmapBlurImage,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    // Limit blur iterations to keep the compute cost bounded. Even at maximum
    // blur we only run a few passes; the difference beyond 3 iterations is
    // barely visible but the cost grows linearly.
    int extraPasses = static_cast<int>(glm::clamp(params.heightmapBlur, 0.0f, 1.0f) * 3.0f);
    int totalPasses = 1 + extraPasses;
    if ((totalPasses % 2) == 0) ++totalPasses; // ensure final dst is m_heightmapBlurImage

    VkImageView srcView = m_heightmapView;
    VkImageView dstView = m_heightmapBlurView;
    VkImage srcImage = m_heightmapImage;
    VkImage dstImage = m_heightmapBlurImage;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_heightmapBlurPipeline);

    for (int pass = 0; pass < totalPasses; ++pass) {
        VkDescriptorImageInfo srcInfo{};
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcInfo.imageView = srcView;

        VkDescriptorImageInfo dstInfo{};
        dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dstInfo.imageView = dstView;

        VkWriteDescriptorSet blurWrites[2] = {};
        blurWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        blurWrites[0].dstSet = m_heightmapBlurDescSet;
        blurWrites[0].dstBinding = 0;
        blurWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        blurWrites[0].descriptorCount = 1;
        blurWrites[0].pImageInfo = &srcInfo;

        blurWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        blurWrites[1].dstSet = m_heightmapBlurDescSet;
        blurWrites[1].dstBinding = 1;
        blurWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        blurWrites[1].descriptorCount = 1;
        blurWrites[1].pImageInfo = &dstInfo;

        vkUpdateDescriptorSets(m_ctx->device(), 2, blurWrites, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_heightmapPipelineLayout, 0, 1, &m_heightmapBlurDescSet, 0, nullptr);
        vkCmdDispatch(cmd, groups, groups, 1);

        // Sync so the next pass can read what we just wrote.
        if (pass + 1 < totalPasses) {
            m_ctx->cmdImageBarrier(cmd, dstImage,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

            // Ping-pong
            std::swap(srcImage, dstImage);
            std::swap(srcView, dstView);
        }
    }

    // 5) Make the blurred heightmap readable by the overlay/vertex shaders.
    m_ctx->cmdImageBarrier(cmd, m_heightmapBlurImage,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

namespace {

// Per-frame context handed to every particle kind at seed time. The kinds
// below exist because rain, snow and dust are the same wrapped-box simulation
// — what changes is the movement direction and how each kind interacts with
// the map (author request 2026-08-10: "é tudo a mesma coisa, só muda direção
// e como interage com o mapa"). Pool sizing, buffer upload and draw slicing
// stay in WeatherRenderer.
struct ParticleSeedContext {
    const WeatherParams& weather;
    std::mt19937& rng;
    std::uniform_real_distribution<float>& dist;   // [-1, 1]
    std::uniform_real_distribution<float>& dist01; // [0, 1]
    Vec3 spawnCenter;       // pool center (camera box, or first follower footprint)
    Vec3 spawnBoxSize;      // pool extents (grows to the largest follower box)
    Vec3 fixedBoxCenter;    // legacy 80x80 camera box (coverage placement)
    float cloudBaseHeight;
    const CloudCoverageNoise* coverageMap; // null = uniform random placement
    Vec2 windDir;           // rain box drift direction (cloud wind)
};

class WeatherParticleKind {
public:
    virtual ~WeatherParticleKind() = default;
    virtual void seed(WeatherParticleGPU& p, const ParticleSeedContext& ctx) const = 0;

protected:
    static Vec3 randomInBox(const ParticleSeedContext& ctx, const Vec3& center, const Vec3& box) {
        Vec3 r(ctx.dist(ctx.rng), ctx.dist(ctx.rng), ctx.dist(ctx.rng));
        return center + r * box * 0.5f;
    }
    // Shared tail: gpu type, turbulence seed, lifetime scale. data.w is
    // kind-specific (fade-in vs dust layer alpha), so each kind sets it.
    static void fillCommon(WeatherParticleGPU& p, const ParticleSeedContext& ctx, float gpuType) {
        p.data.x = gpuType;
        p.data.y = ctx.dist01(ctx.rng) * 6.28f;
        p.data.z = 1.0f + ctx.dist01(ctx.rng);
    }
};

// Precipitation (rain / hail / freezing rain): falls from the cloud layer and
// spawns preferentially under real cloud coverage.
class PrecipKind : public WeatherParticleKind {
public:
    enum class Sub { Rain, Hail, FreezingRain };
    explicit PrecipKind(Sub sub) : m_sub(sub) {}

    void seed(WeatherParticleGPU& p, const ParticleSeedContext& ctx) const override {
        Vec3 origin = seedOrigin(ctx);
        // Precipitation originates from the cloud layer, never above it.
        if (origin.y > ctx.cloudBaseHeight) origin.y = ctx.cloudBaseHeight;
        p.origin = Vec4(origin, ctx.dist01(ctx.rng));
        // Fake-physics wind: drops are blown horizontally in the same direction
        // the rain box drifts with the cloud layer (windMul = 5).
        const float speed = ctx.weather.rainSpeed * 25.0f * (m_sub == Sub::Hail ? 1.4f : 1.0f);
        const float windSpeed = ctx.weather.cloudSpeed * 10.0f * 5.0f;
        const float size = (m_sub == Sub::Hail ? 0.18f : (m_sub == Sub::FreezingRain ? 0.12f : 0.08f));
        p.velocity = Vec4(ctx.windDir.x * windSpeed + ctx.dist(ctx.rng) * 0.5f,
                          -speed * (0.8f + ctx.dist01(ctx.rng) * 0.4f),
                          ctx.windDir.y * windSpeed + ctx.dist(ctx.rng) * 0.5f,
                          size);
        fillCommon(p, ctx, m_sub == Sub::Rain ? 0.0f : (m_sub == Sub::Hail ? 2.0f : 3.0f));
        p.data.w = ctx.dist01(ctx.rng);
    }

private:
    // Coverage-biased origin: try progressively lower coverage thresholds,
    // then fall back to the best of 24 tries (or the box center when the map
    // is essentially empty). Uniform random when no coverage map is active —
    // Clear with per-cloud followers has an empty global map (the fallback
    // would collapse every particle onto the box center) and suppress mode
    // has a dormant one (and the sampling itself was the transition stall).
    Vec3 seedOrigin(const ParticleSeedContext& ctx) const {
        if (!ctx.coverageMap)
            return randomInBox(ctx, ctx.spawnCenter, ctx.spawnBoxSize);
        const float thresholds[] = {0.75f, 0.60f, 0.45f};
        for (float thr : thresholds) {
            for (int attempt = 0; attempt < 10; ++attempt) {
                Vec3 origin = randomInBox(ctx, ctx.fixedBoxCenter, ctx.spawnBoxSize);
                if (ctx.coverageMap->sample(origin.x, origin.z) > thr)
                    return origin;
            }
        }
        float bestCov = -1.0f;
        Vec3 bestOrigin = ctx.fixedBoxCenter;
        for (int attempt = 0; attempt < 24; ++attempt) {
            Vec3 cand = randomInBox(ctx, ctx.fixedBoxCenter, ctx.spawnBoxSize);
            float cov = ctx.coverageMap->sample(cand.x, cand.z);
            if (cov > bestCov) { bestCov = cov; bestOrigin = cand; }
        }
        return (bestCov > 0.35f) ? bestOrigin : ctx.fixedBoxCenter;
    }

    Sub m_sub;
};

// Snow: hail-style pellets falling slowly; double horizontal wind (windMul =
// 10) so the flakes lie along the wind angle instead of falling vertical.
class SnowKind : public WeatherParticleKind {
public:
    void seed(WeatherParticleGPU& p, const ParticleSeedContext& ctx) const override {
        p.origin = Vec4(randomInBox(ctx, ctx.spawnCenter, ctx.spawnBoxSize), ctx.dist01(ctx.rng));
        const float speed = ctx.weather.snowSpeed * 2.0f;
        p.velocity = Vec4(ctx.weather.snowWind * 10.0f + ctx.dist(ctx.rng) * 0.5f,
                          -speed * (0.8f + ctx.dist01(ctx.rng) * 0.4f),
                          ctx.dist(ctx.rng) * 0.5f,
                          0.15f);
        fillCommon(p, ctx, 1.0f);
        p.data.w = ctx.dist01(ctx.rng);
    }
};

// Sand/dust: LATERAL wind storm with no cloud source. Grains are stratified
// into near/mid/far depth layers for a volumetric feel and travel sideways
// along the cloud-wind direction; the vertical component is just a small bob.
class DustKind : public WeatherParticleKind {
public:
    explicit DustKind(bool sand) : m_sand(sand) {}

    void seed(WeatherParticleGPU& p, const ParticleSeedContext& ctx) const override {
        // Wide, shallow spawn box so grains cover the horizon.
        p.origin = Vec4(randomInBox(ctx, ctx.spawnCenter, Vec3(150.0f, 35.0f, 150.0f)),
                        ctx.dist01(ctx.rng));
        // Layer pick biased toward the NEAR layer (pow 1.4): a uniform pick
        // left 2/3 of the pool beyond the frag's depth fade and the storm read
        // as pure haze (author report 2026-08-10).
        const int layer = glm::min(int(std::pow(ctx.dist01(ctx.rng), 1.4f) * 3.0f), 2); // 0=near, 1=mid, 2=far
        // Finer grains, more of them (author feedback 2026-08-11: "mais dust,
        // mais particula — areia não voa assim"; then 2026-08-11: close zoom
        // ">86%" read as a fish school — base size shrunk again).
        const float sizes[3] = {0.18f, 0.11f, 0.06f};
        const float alphas[3] = {0.75f, 0.55f, 0.35f};
        // Lateral speed grows with the weather's wind (windMul = 5 scales it
        // to ~15-45 m/s).
        const float speed = (3.0f + ctx.weather.rainWind * 5.0f) * (0.8f + layer * 0.2f);
        const float size = sizes[layer] * (m_sand ? 1.25f : 1.0f);
        p.velocity = Vec4(ctx.windDir.x * speed * 5.0f + ctx.dist(ctx.rng) * 0.5f,
                          (ctx.dist01(ctx.rng) - 0.5f) * 3.0f,
                          ctx.windDir.y * speed * 5.0f + ctx.dist(ctx.rng) * 0.5f,
                          size);
        fillCommon(p, ctx, m_sand ? 4.0f : 5.0f);
        // Encode the layer alpha in data.w (the shader fades far layers).
        p.data.w = alphas[layer];
    }

private:
    bool m_sand;
};

} // namespace

void WeatherRenderer::updateParticlesForWeather(const WeatherParams& weather, const Vec3& cameraPos, const Mat4& invViewProj, float dt, bool debugRainRegions) {
    bool isHail = weather.weatherType == WeatherType::Hail;
    bool isFreezingRain = weather.weatherType == WeatherType::FreezingRain;
    bool isSandstorm = weather.weatherType == WeatherType::Sandstorm;
    bool isDustStorm = weather.weatherType == WeatherType::DustStorm;

    // Camera-under-cloud factor: when the camera is above the cloud layer no
    // precipitation particles are spawned at all.
    float cloudBase = m_cloudBaseHeight;
    float cameraUnderCloud = debugRainRegions ? 1.0f : (1.0f - glm::smoothstep(cloudBase - 15.0f, cloudBase + 15.0f, cameraPos.y));

    // The rain box is camera-relative: it follows the player/camera in XZ but
    // keeps the vertical span from the ground up to just below the clouds.
    // Horizontal size is the original 80x80 m test area.
    const float boxHeight = glm::max(m_cloudBaseHeight * 0.95f, 60.0f);
    const Vec3 boxSize(80.0f, boxHeight, 80.0f);
    const Vec3 fixedBoxCenter(cameraPos.x, boxHeight * 0.5f, cameraPos.z);
    // Followers-only mode (global weather has no rain, e.g. Clear with
    // per-cloud boxes): the vertex shader re-wraps the pool around each
    // follower box, so the spawn distribution must already span the largest
    // follower footprint — a small spawn cluster stays clustered after the
    // mod wrap, leaving most of a big cloud without drops.
    Vec3 spawnBoxSize = boxSize;
    Vec3 spawnCenter = fixedBoxCenter;
    // In suppress mode the global precip draw is replaced by the follower
    // slices, so the pool must span the largest follower footprint exactly
    // like the followers-only path (and the coverage-map placement below is
    // skipped: it samples the dormant global layer and collapses the pool).
    if ((weather.rainIntensity <= 0.001f || m_suppressGlobalPrecip) && !m_rainFollowers.empty()) {
        float maxX = 80.0f, maxZ = 80.0f;
        for (const RainFollower& f : m_rainFollowers) {
            maxX = std::max(maxX, f.boxSizeXZ.x);
            maxZ = std::max(maxZ, f.boxSizeXZ.y);
        }
        spawnBoxSize.x = maxX;
        spawnBoxSize.z = maxZ;
        spawnCenter.x = m_rainFollowers[0].boxCenter.x;
        spawnCenter.z = m_rainFollowers[0].boxCenter.z;
    }

    // Rain intensity is multiplied for a heavy thunderstorm look while keeping the slider 0..1.
    // Particle count scales dynamically with the box area. When per-cloud rain
    // followers exist, keep a rain particle pool alive even if the global
    // weather has no rain (e.g. Clear): the follower draws force intensity.
    // Each follower draws an EXCLUSIVE slice of the pool (total quads = pool,
    // not pool x followers), so the pool scales with the follower count to
    // keep a per-cloud downpour density. Followers-only mode: the pool spreads
    // over the follower footprint(s) (hundreds of meters, not the legacy
    // 80x80 box), so scale the pool by the largest footprint to keep a
    // rain-like density.
    // NOTE: pool size is DECOUPLED from m_followerRainIntensity (which now
    // only dims each drop via push.params.y): dim drops stay dense, so the
    // rain reads as a soft shower instead of a few bright saturating dots.
    float followerPoolScale = 1.5f;
    if ((weather.rainIntensity <= 0.001f || m_suppressGlobalPrecip) && !m_rainFollowers.empty()) {
        float maxArea = 80.0f * 80.0f;
        for (const RainFollower& f : m_rainFollowers)
            maxArea = std::max(maxArea, f.boxSizeXZ.x * f.boxSizeXZ.y);
        // Target ~3 drops/m2 in the largest footprint: a real downpour when a
        // big cloud is fully loaded (the old 1.5x/80m2 reference read as
        // "minimum rain" once the box grew to the whole cloud footprint).
        const float areaScale = maxArea / (80.0f * 80.0f);
        followerPoolScale = glm::clamp(3.0f * 1.5f * areaScale, 1.5f, 80.0f);
    }
    const float followerPool = m_rainFollowers.empty() ? 0.0f : followerPoolScale;
    const float baseDensityParticles = 6144.0f; // reference visual density for the 80x80 m box
    // Pool sized by the TARGET intensity (see setPoolTargetRainIntensity):
    // one rebuild at the final count instead of a rebuild per ramp frame.
    const float poolIntensity = std::max(weather.rainIntensity, m_poolTargetRainIntensity);
    uint32_t rainCount = uint32_t(cameraUnderCloud *
                                  std::max(poolIntensity * 5.0f, followerPool) *
                                  baseDensityParticles);
    // Sliced draws: boost the pool so each follower's slice stays dense.
    // Scale with the follower count: 12 followers -> x3 (legacy look), 32
    // followers (storms) -> x8, so every storm cloud keeps a downpour
    // density instead of going sparse when the cap is raised. Capped by
    // MAX_RAIN_PARTICLES.
    if (!m_rainFollowers.empty() && rainCount > 0) {
        // Scale with the follower count: 12 followers -> x3 (legacy look), 32
        // followers (storms) -> x8, so every storm cloud keeps a downpour
        // density instead of going sparse when the cap is raised. Capped by
        // MAX_RAIN_PARTICLES. Uses the TARGET follower count while a field
        // drains in (see setPoolTargetFollowerCount): one pool rebuild at the
        // final size instead of a multi-second re-fill every 4 spawned clouds.
        const uint32_t effFollowers = glm::max(static_cast<uint32_t>(m_rainFollowers.size()),
                                               m_poolTargetFollowerCount);
        const uint32_t mult = glm::max(3u, (effFollowers + 3u) / 4u);
        rainCount *= mult;
    }
    rainCount = glm::min(rainCount, MAX_RAIN_PARTICLES);
    // 8x snow density across all snow weathers (author request 2026-08-10):
    // snow spawns hail-type pellets now, so the pool needs hail-like counts
    // (hail rain pool = intensity*5*8192 ~= 32k). Blizzard hits ~32k, snowy
    // ~28k, light snow ~10k, frost ~3k. The MAX_SNOW cap only bounds the
    // intensity=1.0 reference; the total pool cap has headroom.
    uint32_t snowCount = uint32_t(cameraUnderCloud * weather.snowIntensity * 8.0f * float(MAX_SNOW_PARTICLES));
    uint32_t dustCount = 0;
    // Wind-blown debris spawns for EVERY weather with windDebrisIntensity
    // (Light Wind, Pollen, Volcanic Ash, Tornado, ...), not only the Heat
    // storms — the old screen-space speck overlay is gone, so the world-space
    // box is the only debris render path (author request 2026-08-10).
    if (weather.windDebrisIntensity > 0.001f) {
        float mul = (isSandstorm || isDustStorm) ? 1.5f : 1.0f;
        dustCount = uint32_t(glm::min(weather.windDebrisIntensity * mul, 1.0f) * float(MAX_DUST_PARTICLES));
    }

    uint32_t total = glm::min(rainCount + snowCount + dustCount,
                              MAX_RAIN_PARTICLES + MAX_SNOW_PARTICLES + MAX_DUST_PARTICLES);

    m_rainRebuildTimer += dt;
    float camDist = glm::length(Vec2(cameraPos.x - m_lastRainCameraPos.x, cameraPos.z - m_lastRainCameraPos.z));
    // Periodic/camera-triggered rebuilds only matter for weather that animates
    // the spawn pattern (global rain/snow/debris). When the procedural cloud
    // field supplies the precipitation (m_suppressGlobalPrecip), the global
    // camera-box draw is skipped and the follower boxes are world-anchored
    // push constants — re-seeding the pool every 0.1s (plus the blocking
    // immediateSubmit upload) buys nothing and was the heavy-rain stall:
    // rebuild just on count change in that mode.
    bool animatedWeather = !m_suppressGlobalPrecip &&
                           (weather.rainIntensity > 0.001f || weather.snowIntensity > 0.001f ||
                            weather.windDebrisIntensity > 0.001f);
    // Hysteresis on the count-change rebuild: while a weather's rainIntensity
    // ramps in, `total` drifts every frame and a strict != compare would
    // re-fill + blocking-upload the whole pool EVERY frame (a 300k-particle
    // storm pool costs 10-20ms per rebuild -> burst of frame spikes right
    // after the field spawns). A 5% density lag during the ramp is invisible;
    // zero crossings and real jumps (follower cap changes) always rebuild.
    const int countDelta = std::abs(static_cast<int>(total) - static_cast<int>(m_particleCount));
    const bool countChanged = (total != m_particleCount) &&
                              (total == 0 || m_particleCount == 0 ||
                               countDelta > static_cast<int>(m_particleCount) / 20);
    // Composition trigger: the total-based check above misses a component
    // that ramps out INSIDE the 5% band (e.g. dust 3k -> 0 while the rain
    // pool is 115k: the 3k delta is under hysteresis, so the stale dust tail
    // would draw forever). A zero crossing in any component always rebuilds;
    // a >5% shift in any single component rebuilds too, so cross-fades
    // (rain -> snow) never draw stale slice ranges.
    const auto compChanged = [](uint32_t target, uint32_t pooled) {
        if (target == pooled) return false;
        if (target == 0 || pooled == 0) return true;
        return std::abs(static_cast<int>(target) - static_cast<int>(pooled)) >
               static_cast<int>(pooled) / 20;
    };
    const bool compositionChanged = compChanged(rainCount, m_poolRainCount) ||
                                    compChanged(snowCount, m_poolSnowCount) ||
                                    compChanged(dustCount, m_poolDustCount);
    bool needsRebuild = countChanged || compositionChanged ||
                        (total > 0 && animatedWeather && m_rainRebuildTimer >= RAIN_REBUILD_INTERVAL) ||
                        (total > 0 && animatedWeather && camDist >= RAIN_REBUILD_DISTANCE);
    if (!needsRebuild) return;
    m_rainRebuildTimer = 0.0f;
    m_lastRainCameraPos = cameraPos;

    std::vector<WeatherParticleGPU> particles;
    particles.reserve(total);

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    // The coverage map only biases PRECIP placement, and only when the global
    // weather actually rains outside suppress mode (see PrecipKind::seedOrigin
    // for why Clear/followers and suppress mode must use uniform placement).
    ParticleSeedContext ctx{weather, rng, dist, dist01,
                            spawnCenter, spawnBoxSize, fixedBoxCenter,
                            m_cloudBaseHeight,
                            (m_cloudCoverageMap && weather.rainIntensity > 0.001f && !m_suppressGlobalPrecip)
                                ? m_cloudCoverageMap : nullptr,
                            m_rainBoxMoveDir};

    const PrecipKind precipKind(isHail ? PrecipKind::Sub::Hail
                                       : (isFreezingRain ? PrecipKind::Sub::FreezingRain : PrecipKind::Sub::Rain));
    const SnowKind snowKind;
    const DustKind dustKind(isSandstorm);

    // Pool layout [rain | snow | dust]: the draw code slices by these ranges.
    auto spawn = [&](const WeatherParticleKind& kind, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            WeatherParticleGPU p{};
            kind.seed(p, ctx);
            particles.push_back(p);
        }
    };
    spawn(precipKind, rainCount);
    spawn(snowKind, snowCount);
    spawn(dustKind, dustCount);

    m_particleCount = static_cast<uint32_t>(particles.size());
    m_snowStart = rainCount;
    m_snowCount = (m_particleCount > 0) ? glm::min(snowCount, m_particleCount - glm::min(m_snowStart, m_particleCount)) : 0;
    m_dustStart = rainCount + m_snowCount;
    m_dustCount = (m_particleCount > 0) ? glm::min(dustCount, m_particleCount - glm::min(m_dustStart, m_particleCount)) : 0;
    m_poolRainCount = rainCount;
    m_poolSnowCount = snowCount;
    m_poolDustCount = dustCount;

    if (particles.empty()) return;

    VkDeviceSize size = particles.size() * sizeof(WeatherParticleGPU);

    // Grow (or shrink) the GPU buffer when the dynamic particle count changes.
    if (m_particleCount > m_particleBufferCapacity) {
        if (m_particleBuffer) vmaDestroyBuffer(m_ctx->allocator(), m_particleBuffer, m_particleAlloc);
        m_particleBufferCapacity = m_particleCount;
        VkDeviceSize newSize = m_particleBufferCapacity * sizeof(WeatherParticleGPU);
        if (!m_ctx->createBuffer(newSize,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VMA_MEMORY_USAGE_GPU_ONLY,
                                 m_particleBuffer, m_particleAlloc)) {
            m_particleBuffer = VK_NULL_HANDLE;
            m_particleBufferCapacity = 0;
            m_particleCount = 0;
            m_snowStart = 0;
            m_snowCount = 0;
            m_dustStart = 0;
            m_dustCount = 0;
            m_poolRainCount = 0;
            m_poolSnowCount = 0;
            m_poolDustCount = 0;
            return;
        }
    }

    VkBuffer staging;
    VmaAllocation stagingAlloc;
    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, staging, stagingAlloc)) return;

    void* data;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &data);
    memcpy(data, particles.data(), size);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    if (m_ctx->isFrameRecording()) {
        VkCommandBuffer cmd = m_ctx->currentCmdBuf();
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging, m_particleBuffer, 1, &copy);
        
        m_ctx->deferFrameCleanup([ctx = m_ctx, staging, stagingAlloc]() {
            vmaDestroyBuffer(ctx->allocator(), staging, stagingAlloc);
        });
    } else {
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy copy{};
            copy.size = size;
            vkCmdCopyBuffer(cmd, staging, m_particleBuffer, 1, &copy);
        });
        vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
    }
}

float WeatherRenderer::cameraRainExposure(const Vec3& cameraPos) const {
    float exposure = 0.0f;
    for (const RainFollower& f : m_rainFollowers) {
        const float r = f.occluder.z;
        if (r <= 0.0f) continue;
        const float d = glm::distance(Vec2(cameraPos.x, cameraPos.z),
                                      Vec2(f.occluder.x, f.occluder.y));
        exposure = glm::max(exposure, 1.0f - glm::smoothstep(r - 20.0f, r, d));
        if (exposure >= 1.0f) break;
    }
    return exposure;
}

void WeatherRenderer::updateSplashesForWeather(const WeatherParams& weather, const Vec3& cameraPos, const Vec3& rainAnchor, float dt) {    // ERUPTION_TEST_NO_SPLASH=1 (debug): skip splash spawning to isolate its cost.
    static const bool testNoSplash = std::getenv("ERUPTION_TEST_NO_SPLASH") != nullptr;
    if (testNoSplash) { m_splashCount = 0; m_splashGlobalCount = 0; m_splashGroups.clear(); return; }
    // High tier: 3D splash particles on surfaces. Global group around the
    // camera (global precipitation), then one group per rain follower inside
    // its cloud footprint, so each independent cloud splashes on the ground
    // beneath it even in Clear weather (no global rain).
    float splashAmount = weather.rainIntensity * weather.effectiveRainSplashIntensity() * weather.rainSplashAmount;
    uint32_t globalCount = uint32_t(splashAmount * float(MAX_SPLASH_PARTICLES));
    globalCount = glm::min(globalCount, MAX_SPLASH_PARTICLES);

    const uint32_t followerCount = static_cast<uint32_t>(m_rainFollowers.size());
    // Global precip suppressed (procedural cloud field): the WHOLE pool goes
    // to the per-cloud groups. They are follower-relative now (resolved
    // against the cloud's current rain-box center in the vertex shader), so
    // the around-player density comes from the clouds actually overhead —
    // the old camera-anchored global group baked cloud offsets that went
    // stale as the clouds drifted (author feedback 2026-08-10: "splash em
    // lugar que não chove / fora da box").
    if (m_suppressGlobalPrecip) {
        globalCount = 0;
    }

    // Per-cloud splash counts weighted by footprint AREA x rainRate (same law
    // as the rain-particle pool slices): a huge loaded cloud gets many
    // splashes, a small light one few — splash count per m2 follows the local
    // rain per m2 (author feedback 2026-08-10). Weights are STATIC per cloud
    // (no distance factor) so walking around never triggers a rebuild+upload.
    // Scratches estaticos: alocados+liberados por frame ANTES do early-return
    // logo abaixo, so' pra calcular a condicao de saida. (Varredura de hot
    // paths 2026-09-02.) Main thread only.
    static std::vector<uint32_t> cloudCounts;
    cloudCounts.assign(followerCount, 0);
    if (followerCount > 0) {
        // Pool scales with the weather's own rain amount (STATIC preset knob —
        // anything animated would rebuild the buffer every frame): drizzle
        // (0.10) splashes a little, thunderstorm (0.40) a lot — heavier rain,
        // more splashes per m2 (author feedback 2026-08-10: "mais chuva e
        // menos chuva não podem ter a mesma quantidade de pingo no chão").
        const float poolFrac = glm::clamp(weather.rainSplashAmount / 0.5f, 0.05f, 1.0f);
        const uint32_t pool = glm::min(MAX_SPLASH_PARTICLES - globalCount,
                                       static_cast<uint32_t>(poolFrac * static_cast<float>(MAX_SPLASH_PARTICLES)));
        float wSum = 0.0f;
        static std::vector<float> w;
        w.assign(followerCount, 0.0f);
        // PESO CAI COM A DISTANCIA. Era so' area x taxa: o orcamento de
        // coroas (fixo) se dividia por igual entre TODAS as nuvens do campo -
        // 40 delas num mapa de 4 km - e sobravam ~100 coroas por pegada de
        // centenas de metros, ou seja, nenhuma coroa visivel onde a camera
        // olha (autor 2026-09-06: "nem a coroa nos angulos que ta"). Com o
        // peso caindo em 1/(1+d/300)^2 a partir do ancoradouro, as nuvens
        // sobre a cena ficam com a maior parte do orcamento e as do outro
        // lado do mapa com quase nada - mesmo custo, densidade certa onde se
        // ve. O piso 0,02 mantem um respingo simbolico nas distantes.
        for (uint32_t i = 0; i < followerCount; ++i) {
            const RainFollower& f = m_rainFollowers[i];
            const float dx = f.boxCenter.x - rainAnchor.x;
            const float dz = f.boxCenter.z - rainAnchor.z;
            const float dn = 1.0f + std::sqrt(dx * dx + dz * dz) / kRainNearScale;
            const float near = std::max(1.0f / (dn * dn), 0.02f);
            w[i] = std::max(f.boxSizeXZ.x * f.boxSizeXZ.y, 1.0f) * f.rainRate * near;
            wSum += w[i];
        }
        if (wSum <= 0.0f) wSum = 1.0f;
        for (uint32_t i = 0; i < followerCount; ++i) {
            cloudCounts[i] = static_cast<uint32_t>(static_cast<float>(pool) * w[i] / wSum);
        }
    }
    uint32_t splashCount = globalCount;
    for (uint32_t c : cloudCounts) splashCount += c;

    m_splashTimer += dt;
    // World-anchored groups don't need the periodic reseed (nor the blocking
    // immediateSubmit upload that comes with it): rebuild only when the count
    // distribution changes (weather transition / cloud field respawn — the
    // weights above are static per cloud, so walking never rebuilds). In
    // suppress mode the global group is gone entirely; only the legacy
    // non-suppress global group still reseeds around the camera on the timer.
    const bool worldAnchored = m_suppressGlobalPrecip || globalCount == 0;
    if (splashCount == m_splashCount && cloudCounts == m_splashCloudCounts &&
        splashCount > 0 &&
        (worldAnchored || m_splashTimer < SPLASH_REBUILD_INTERVAL)) return;
    m_splashTimer = 0.0f;

    std::vector<WeatherSplashGPU> splashes;
    splashes.reserve(splashCount);

    std::mt19937 rng(54321);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    // Keep splashes inside the top-down heightmap bounds so the GPU can place
    // them on real surfaces.  Y is resolved from the heightmap, so we only
    // need a meaningful XZ origin.
    const float worldSizeX = HEIGHTMAP_WORLD_SIZE;
    const float worldSizeZ = HEIGHTMAP_WORLD_SIZE;

    auto addSplash = [&](const Vec3& origin, float seed) {
        WeatherSplashGPU s{};
        s.origin = Vec4(origin, seed);
        s.data.x = 1.0f + dist01(rng) * 2.0f;   // lifetime scale
        s.data.y = dist01(rng);                 // phase
        s.data.z = 0.30f + dist01(rng) * 0.40f; // size (was 0.12-0.30: subpixel at gameplay zoom)
        s.data.w = 0.5f + dist01(rng) * 0.5f;   // intensity
        splashes.push_back(s);
    };

    // Legacy global group (non-suppress mode only): camera-centered absolute
    // origins, reseeded on the timer. Suppress mode has globalCount = 0 — the
    // per-cloud groups below carry the whole pool.
    for (uint32_t i = 0; i < globalCount; i++) {
        Vec3 r(dist(rng), 0.0f, dist(rng));
        Vec3 origin = Vec3(cameraPos.x, 0.0f, cameraPos.z)
                    + r * Vec3(worldSizeX * 0.5f, 0.0f, worldSizeZ * 0.5f);
        addSplash(origin, dist01(rng));
    }
    m_splashGlobalCount = globalCount;

    // Per-cloud groups: origins are RELATIVE to the owning cloud's rain-box
    // center and resolved against push.rainBoxCenter in the vertex shader
    // (params.z = 2), which the per-cloud draw refreshes every frame — the
    // splash pattern drifts WITH its cloud. The old absolute bake stranded
    // splashes at the cloud's spawn position while the cloud (and its rain)
    // drifted away, so splashes showed up where nothing rained anymore
    // (author feedback 2026-08-10). Spawn is uniform inside the rain BOX
    // (not the old occluder disc, which spilled past the box edges), inset
    // to 90% so quads stay off the rain edge-fade band. Deterministic
    // pattern per slot (stable across rebuilds).
    m_splashGroups.clear();
    for (uint32_t fi = 0; fi < followerCount; ++fi) {
        const uint32_t cnt = cloudCounts[fi];
        const RainFollower& f = m_rainFollowers[fi];
        uint32_t first = static_cast<uint32_t>(splashes.size());
        if (cnt > 0) {
            const Vec2 half = f.boxSizeXZ * 0.5f * 0.9f;
            std::mt19937 crng(777u + fi * 131u);
            std::uniform_real_distribution<float> cdist(-1.0f, 1.0f);
            std::uniform_real_distribution<float> cdist01(0.0f, 1.0f);
            for (uint32_t i = 0; i < cnt; ++i) {
                Vec2 off(cdist(crng), cdist(crng));
                addSplash(Vec3(off.x * half.x, 0.0f, off.y * half.y), cdist01(rng));
            }
        }
        m_splashGroups.push_back({first, cnt});
    }

    m_splashCount = splashCount;
    m_splashCloudCounts = cloudCounts;

    if (splashes.empty()) return;

    VkDeviceSize size = splashes.size() * sizeof(WeatherSplashGPU);
    VkBuffer staging;
    VmaAllocation stagingAlloc;
    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, staging, stagingAlloc)) return;

    void* data;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &data);
    memcpy(data, splashes.data(), size);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    if (m_ctx->isFrameRecording()) {
        VkCommandBuffer cmd = m_ctx->currentCmdBuf();
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging, m_splashBuffer, 1, &copy);
        
        m_ctx->deferFrameCleanup([ctx = m_ctx, staging, stagingAlloc]() {
            vmaDestroyBuffer(ctx->allocator(), staging, stagingAlloc);
        });
    } else {
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy copy{};
            copy.size = size;
            vkCmdCopyBuffer(cmd, staging, m_splashBuffer, 1, &copy);
        });
        vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
    }
}

void WeatherRenderer::render(VkCommandBuffer cmd,
                             VkImageView inputColor,
                             VkImageView outputColor,
                             uint32_t width, uint32_t height,
                             const WeatherParams& weather,
                             const RenderParams& params) {
    if (!m_ctx || m_tier == WeatherTier::Mobile) return;

    // Camera-under-cloud cutoff: above the cloud layer no precipitation is
    // rendered, like being above the water surface.
    float cloudBase = m_cloudBaseHeight;
    float cameraUnderCloud = (params.debugRainRegions > 0.5f) ? 1.0f : (1.0f - glm::smoothstep(cloudBase - 15.0f,
                                                                                               cloudBase + 15.0f,
                                                                                               params.cameraPos.y));
    if (cameraUnderCloud < 0.001f) return;

    const float boxHeight = glm::max(m_cloudBaseHeight * 0.95f, 60.0f);

    // ERUPTION_TEST_WR_CPU=1: onde a CPU gasta DENTRO do WeatherRenderer.
    // Motivo: em fire_smoke a fase Post da CPU marcava 17,3 ms/frame (0,06 em
    // clear) e o tempo de GPU nao explicava.
    static const bool kWrCpu = std::getenv("ERUPTION_TEST_WR_CPU") != nullptr;
    static double wAcc[6] = {0,0,0,0,0,0}; static int wN = 0;
    auto wT = std::chrono::steady_clock::now();
    auto wLap = [&](int k) {
        if (!kWrCpu) return;
        auto n = std::chrono::steady_clock::now();
        wAcc[k] += std::chrono::duration<double, std::milli>(n - wT).count(); wT = n;
    };
    updateParticlesForWeather(weather, params.cameraPos, params.invViewProj, params.deltaTime, params.debugRainRegions > 0.5f);
    wLap(0);
    updateSplashesForWeather(weather, params.cameraPos,
                             (params.rainAnchor.y > -1.0e8f) ? params.rainAnchor : params.cameraPos,
                             params.deltaTime);
    // ERUPTION_TEST_SPLASH_DEBUG=1 (debug): log the splash/particle state ~1x/s.
    static const bool kSplashDebug = std::getenv("ERUPTION_TEST_SPLASH_DEBUG") != nullptr;
    if (kSplashDebug) {
        static float s_dbgTimer = 0.0f;
        s_dbgTimer += params.deltaTime;
        if (s_dbgTimer >= 1.0f) {
            s_dbgTimer = 0.0f;
            Logger::warning("[SPLASH DBG] tier=%d particles=%u splash=%u global=%u groups=%zu followers=%zu cloudBase=%.1f camY=%.1f suppress=%d",
                         static_cast<int>(m_tier), m_particleCount, m_splashCount, m_splashGlobalCount,
                         m_splashGroups.size(), m_rainFollowers.size(), m_cloudBaseHeight,
                         params.cameraPos.y, m_suppressGlobalPrecip ? 1 : 0);
        }
    }
    wLap(1);
    if (m_particleCount == 0 && m_splashCount == 0) return;

    // Assumes outputColor is already in COLOR_ATTACHMENT_OPTIMAL layout
    // and will be transitioned to SHADER_READ by the caller.

    // Update descriptor set (bindings 0/1 used by fragment, binding 2/3/4 by vertex)
    VkDescriptorImageInfo imgInfos[5] = {};
    VkWriteDescriptorSet writes[8] = {};

    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[0].imageView = inputColor;
    imgInfos[0].sampler = m_linearSampler;
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descSets[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imgInfos[0];

    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Scene camera depth for the soft rain occlusion (NOT the top-down
    // heightmap depth). Falls back to depthView if unset.
    imgInfos[1].imageView = (params.sceneDepthView != VK_NULL_HANDLE) ? params.sceneDepthView : params.depthView;
    imgInfos[1].sampler = m_linearSampler;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descSets[0];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imgInfos[1];

    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[2].imageView = heightmapView();
    imgInfos[2].sampler = m_heightmapSampler;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descSets[0];
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &imgInfos[2];

    imgInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[3].imageView = m_cloudCoverageView;
    imgInfos[3].sampler = m_cloudCoverageSampler;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descSets[0];
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &imgInfos[3];

    imgInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[4].imageView = m_cloudAltitudeView;
    imgInfos[4].sampler = m_cloudAltitudeSampler;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descSets[0];
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &imgInfos[4];

    VkDescriptorBufferInfo occBufInfo{};
    occBufInfo.buffer = m_rainOccBuffer;
    occBufInfo.offset = 0;
    occBufInfo.range = 3 * MAX_RAIN_OCCLUDERS * sizeof(Vec4);
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descSets[0];
    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[5].descriptorCount = 1;
    writes[5].pBufferInfo = &occBufInfo;

    // Binding 6: every follower's coverage array (exact cross-cloud silhouettes).
    VkDescriptorImageInfo covArrayImgs[MAX_RAIN_OCCLUDERS] = {};
    for (uint32_t j = 0; j < MAX_RAIN_OCCLUDERS; ++j) {
        covArrayImgs[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        covArrayImgs[j].imageView = m_cloudCoverageView;
        covArrayImgs[j].sampler = m_cloudCoverageSampler;
        if (j < m_rainFollowers.size() && m_rainFollowers[j].coverageView != VK_NULL_HANDLE) {
            covArrayImgs[j].imageView = m_rainFollowers[j].coverageView;
            covArrayImgs[j].sampler = m_rainFollowers[j].coverageSampler;
        }
    }
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descSets[0];
    writes[6].dstBinding = 6;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].descriptorCount = MAX_RAIN_OCCLUDERS;
    writes[6].pImageInfo = covArrayImgs;

    // Binding 7: rain streak shape tuning (refreshed every frame from WeatherParams).
    if (m_tuneMapped) {
        Vec4* tune = static_cast<Vec4*>(m_tuneMapped);
        float sw = weather.rainStreakWidth, sl = weather.rainStreakLength;
        float se = weather.rainStreakEdge, st = weather.rainStreakTipFade;
        float sa = weather.rainStreakAlpha, sb = weather.rainStreakBrightness;
        float spf = weather.rainPixelFade;
        // Debug A/B override: ERUPTION_TEST_STREAK_SHAPE="w,l,edge,tip,alpha,bright,pixelFade"
        // forces the streak shape regardless of weather type (same seeded
        // particle field, so screenshots are pixel-comparable).
        static float ov[7] = {0};
        static bool ovChecked = false, ovActive = false;
        if (!ovChecked) {
            ovChecked = true;
            if (const char* e = getenv("ERUPTION_TEST_STREAK_SHAPE")) {
                ovActive = sscanf(e, "%f,%f,%f,%f,%f,%f,%f",
                                  &ov[0], &ov[1], &ov[2], &ov[3], &ov[4], &ov[5], &ov[6]) == 7;
            }
        }
        if (ovActive) {
            sw = ov[0]; sl = ov[1]; se = ov[2]; st = ov[3];
            sa = ov[4]; sb = ov[5]; spf = ov[6];
        }
        tune[0] = Vec4(sw, sl, se, st);
        tune[1] = Vec4(sa, sb, spf, 0.0f);
    }
    VkDescriptorBufferInfo tuneBufInfo{};
    tuneBufInfo.buffer = m_tuneBuffer;
    tuneBufInfo.offset = 0;
    tuneBufInfo.range = 2 * sizeof(Vec4);
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descSets[0];
    writes[7].dstBinding = 7;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[7].descriptorCount = 1;
    writes[7].pBufferInfo = &tuneBufInfo;

    wLap(2);
    vkUpdateDescriptorSets(m_ctx->device(), 8, writes, 0, nullptr);

    // Per-follower descriptor sets: identical to set 0 but binding 3 points at
    // the owning cloud's own coverage array, so the fragment shader can test
    // occlusion against the exact rendered silhouette of each cloud.
    wLap(2);
    if (!m_rainFollowers.empty()) {
        if (m_followerDescSets.size() < m_rainFollowers.size()) {
            const uint32_t add = static_cast<uint32_t>(m_rainFollowers.size() - m_followerDescSets.size());
            std::vector<VkDescriptorSetLayout> layouts(add, m_descLayout);
            std::vector<VkDescriptorSet> sets(add);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = m_descPool;
            ai.descriptorSetCount = add;
            ai.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(m_ctx->device(), &ai, sets.data()) == VK_SUCCESS)
                m_followerDescSets.insert(m_followerDescSets.end(), sets.begin(), sets.end());
        }
        for (size_t fi = 0; fi < m_rainFollowers.size() && fi < m_followerDescSets.size(); ++fi) {
            const RainFollower& f = m_rainFollowers[fi];
            VkWriteDescriptorSet fw[8] = {writes[0], writes[1], writes[2], writes[3], writes[4], writes[5], writes[6], writes[7]};
            VkDescriptorImageInfo cov{};
            cov.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            cov.imageView = (f.coverageView != VK_NULL_HANDLE) ? f.coverageView : m_cloudCoverageView;
            cov.sampler = (f.coverageSampler != VK_NULL_HANDLE) ? f.coverageSampler : m_cloudCoverageSampler;
            for (auto& w : fw) w.dstSet = m_followerDescSets[fi];
            fw[3].pImageInfo = &cov;
            vkUpdateDescriptorSets(m_ctx->device(), 8, fw, 0, nullptr);
        }
    }

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = outputColor;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // blend over existing
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {width, height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkc::cmdBeginRendering(cmd, &info);

    VkViewport vp{0, 0, (float)width, (float)height, 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Rain-occlusion debug: per-particle reason colors only (the fullscreen
    // occlusion map overlay was removed — it duplicated the particle info).

    wLap(4);  // fim dos descritores POR SEGUIDOR
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particlePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSets[0], 0, nullptr);

    struct Push {
        Mat4 viewProj;
        Vec4 cameraPos;
        Vec4 params; // x=time, y=rainIntensity, z=snowIntensity, w=screenAspect
        Vec4 params2; // x=rainWind, y=rainSplashRadius, z=farPlane, w=fogDensity
        Vec4 params5; // x=rainShadowSoftness, y=cloudBaseHeight, z=windOffsetX, w=windOffsetY
        Vec4 screenSize;
        Vec4 worldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeZ (rain heightmap)
        Vec4 cloudWorldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeZ (cloud coverage)
        Vec4 cloudParams; // x=layer, y=threshold, z=weatherCoverage, w=unused
        Vec4 sunDir;
        Vec4 cloudRange; // x=cloudBottom, y=cloudTop, z=cloudTop-cloudBottom, w=cloudScale
        Vec4 rainBoxCenter; // xyz = fixed world-space center of the rain box, w = unused
        Vec4 occluder; // x,y = cloud blob center XZ (followers), z = blob radius (<=0 = infinite plane), w = debug occlusion flag
    } push;
    push.viewProj = params.proj * params.view;
    push.cameraPos = Vec4(params.cameraPos, 0.0f);
    push.params = Vec4(params.time, weather.rainIntensity, weather.snowIntensity, (float)width / (float)height);
    push.params2 = Vec4(weather.rainWind, weather.rainSplashRadius, params.farPlane, weather.fogDensity);
    push.params5 = Vec4(weather.rainShadowSoftness, m_cloudBaseHeight, m_cloudWindOffset.x, m_cloudWindOffset.y);
    push.screenSize = Vec4((float)width, (float)height, 1.0f / width, 1.0f / height);
    const float worldHalf = HEIGHTMAP_WORLD_SIZE * 0.5f;
    // Rain heightmap bounds anchor to the character (orbit target), not the
    // camera — see RenderParams::rainAnchor. Must match updateHeightmap and
    // the top-down depth eye.
    const Vec3 rainAnchor = (params.rainAnchor.y > -1.0e8f) ? params.rainAnchor : params.cameraPos;
    push.worldBounds = Vec4(rainAnchor.x - worldHalf,
                            rainAnchor.z - worldHalf,
                            HEIGHTMAP_WORLD_SIZE,
                            HEIGHTMAP_WORLD_SIZE);
    if (m_cloudCoverageMap) {
        Vec3 cmin = m_cloudCoverageMap->worldMin();
        Vec3 cmax = m_cloudCoverageMap->worldMax();
        push.cloudWorldBounds = Vec4(cmin.x, cmin.z, cmax.x - cmin.x, cmax.z - cmin.z);
    } else {
        push.cloudWorldBounds = Vec4(-12000.0f, -12000.0f, 24000.0f, 24000.0f);
    }
    // Cross-cloud rain occluders UBO: every rainy cloud's blob + coverage
    // mapping, so drops are hidden by ANY cloud between them and the camera
    // (not only their own), tested against the exact coverage silhouette.
    if (m_rainOccMapped) {
        Vec4* items = static_cast<Vec4*>(m_rainOccMapped);
        const size_t n = std::min(m_rainFollowers.size(), static_cast<size_t>(MAX_RAIN_OCCLUDERS));
        for (size_t i = 0; i < n; ++i) {
            const RainFollower& f = m_rainFollowers[i];
            items[i] = Vec4(f.occluder.x, f.occluder.y, f.boxCenter.y, f.occluder.z);
            items[MAX_RAIN_OCCLUDERS + i] = f.covBounds;
            items[2 * MAX_RAIN_OCCLUDERS + i] = Vec4(f.covWind.x, f.covWind.y, 0.0f, 0.0f);
        }
        for (size_t i = n; i < MAX_RAIN_OCCLUDERS; ++i) {
            items[i] = Vec4(0.0f);
            items[MAX_RAIN_OCCLUDERS + i] = Vec4(0.0f);
            items[2 * MAX_RAIN_OCCLUDERS + i] = Vec4(0.0f);
        }
    }
    const float occCount = static_cast<float>(std::min(m_rainFollowers.size(),
                                                       static_cast<size_t>(MAX_RAIN_OCCLUDERS)));
    // cloudParams.w: carries the camera near plane, used by the particle fragment
    // shader to linearize the scene depth for the soft rain occlusion. The
    // splash draws below re-push this slot with the splash opacity instead.
    // cloudParams.y: index of the drawing follower in the occluder UBO (-1 =
    // global draw). cloudParams.z: occluder count. x is free (layer unused).
    push.cloudParams = Vec4(m_cloudLayer, -1.0f, occCount, params.nearPlane);
    // sunDir.w carries the ground level under the player (orbit target Y):
    // the sand/dust box anchors there instead of spanning the whole cloud
    // column, so the grains concentrate at street level where they read.
    push.sunDir = Vec4(params.sunDir, rainAnchor.y);
    push.cloudRange = Vec4(m_cloudBottom, m_cloudTop, m_cloudTop - m_cloudBottom, m_cloudScale);
    // Rain box is centered on the camera so precipitation follows the player.
    push.rainBoxCenter = Vec4(params.cameraPos.x, boxHeight * 0.5f, params.cameraPos.z, 0.0f);
    // Global draw: no blob bound (infinite cloud plane) + debug flag for both stages.
    push.occluder = Vec4(0.0f, 0.0f, 0.0f, params.debugRainOcclusion + (params.debugRainRegions ? 10.0f : 0.0f));

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_particleBuffer, &offset);
    // Skip the camera-volume draw when there is no global precipitation (e.g.
    // Clear weather with only per-cloud rain followers): every particle would
    // be discarded by the intensity check, but the GPU still pays for them.
    // Also skipped when the procedural cloud field supplies the precipitation.
    const bool hasGlobalPrecip = !m_suppressGlobalPrecip &&
                                 (weather.rainIntensity > 0.001f || weather.snowIntensity > 0.001f ||
                                  weather.windDebrisIntensity > 0.001f);
    if (hasGlobalPrecip) {
        vkCmdDraw(cmd, 4, m_particleCount, 0, 0);
    } else if (m_suppressGlobalPrecip &&
               ((m_snowCount > 0 && m_rainFollowers.empty()) || m_dustCount > 0)) {
        // Procedural cloud field mode: snow normally renders through the
        // per-cloud followers (Snow-category clouds spawn rainy now, so their
        // flakes are masked by each cloud's exact coverage silhouette). This
        // camera-box draw is only the FALLBACK for the transition window
        // before the first follower exists — rainBoxCenter.w = 1 bypasses the
        // dormant global coverage mask, and y = 0 anchors the wrap box at the
        // ground (the legacy global y floats the box above a low camera's
        // frustum). In follower-box mode the shaders read the wrap extents
        // from cloudRange.yw, so pin them to the legacy 80x80 box.
        // Dust/sand is NOT cloud-bound (no source, lateral wind storm), so it
        // ALWAYS takes this camera-box draw in field mode — with or without
        // followers (author request 2026-08-10).
        push.rainBoxCenter = Vec4(params.cameraPos.x, 0.0f, params.cameraPos.z, 1.0f);
        push.cloudRange.y = 80.0f;
        push.cloudRange.w = 80.0f;
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        if (m_snowCount > 0 && m_rainFollowers.empty()) {
            vkCmdDraw(cmd, 4, m_snowCount, 0, m_snowStart);
        }
        if (m_dustCount > 0) {
            vkCmdDraw(cmd, 4, m_dustCount, 0, m_dustStart);
        }
    }

    // Per-cloud rain: redraw the same particle pool once per follower box
    // (particles wrap around rainBoxCenter in the vertex shader, sized to the
    // owning cloud's footprint via cloudRange.yw). The rain intensity is
    // forced so the cloud's own rain shows even in Clear weather;
    // rainBoxCenter.w = 1 tells the vertex shader to bypass the global
    // cloud-coverage mask (empty in Clear) for these boxes.
    // Each follower binds its own descriptor set (binding 3 = the owning
    // cloud's coverage array) and carries that array's world bounds + wind, so
    // the fragment shader tests occlusion against the exact rendered cloud
    // silhouette; f.occluder.z remains as a cheap radius early-out.
    if (!m_rainFollowers.empty() && m_particleCount > 0) {
        // Slice allocation proportional to each cloud's footprint AREA: drops
        // per m² become uniform across clouds instead of every cloud getting
        // the same 1/N share (a 2.8x-bigger cloud was 2.8x thinner). Near
        // clouds keep full density, far ones get a fraction — the pool is not
        // wasted on rain nobody sees, so a thunderstorm pours hard around the
        // player on ANY map size (author request 2026-08-09).
        const uint32_t followerN = static_cast<uint32_t>(m_rainFollowers.size());
        float weights[128];
        float wSum = 0.0f;
        const uint32_t wN = glm::min(followerN, 128u);
        for (uint32_t i = 0; i < wN; ++i) {
            const RainFollower& f = m_rainFollowers[i];
            const float area = std::max(f.boxSizeXZ.x * f.boxSizeXZ.y, 1.0f);
            const float dx = f.boxCenter.x - rainAnchor.x;
            const float dz = f.boxCenter.z - rainAnchor.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            // Mesma curva das coroas (ver updateSplashesForWeather): o degrau
            // 1,0/0,25 em 500 u ainda espalhava o pool por dezenas de nuvens e
            // a chuva ficava rala em qualquer camera que visse o campo todo
            // ("nao ta aparecendo os pingos", autor 2026-09-06).
            const float dn = 1.0f + dist / kRainNearScale;
            const float w = area * std::max(1.0f / (dn * dn), 0.02f);
            weights[i] = w;
            wSum += w;
        }
        if (wSum <= 0.0f) wSum = 1.0f;
        uint32_t firstInst = 0;
        for (size_t fi = 0; fi < m_rainFollowers.size(); ++fi) {
            const RainFollower& f = m_rainFollowers[fi];
            // cloudRange: x = cloud plane height (box spans ground..planeY
            // instead of the global cloudBottom), yw = the cloud's footprint
            // extents so the vertex wrap spreads drops under the WHOLE cloud.
            // z stays > 0 for the altitude-map gate.
            push.cloudRange = Vec4(f.boxCenter.y, f.boxSizeXZ.x,
                                   m_cloudTop - m_cloudBottom, f.boxSizeXZ.y);
            push.rainBoxCenter = Vec4(f.boxCenter.x, 0.0f, f.boxCenter.z, 1.0f);
            // ERUPTION_TEST_RAIN_TINT=1 (debug): occluder.w = 2 + follower index, the
            // fragment shader paints each cloud's rain a distinct color.
            static const bool kRainTint = std::getenv("ERUPTION_TEST_RAIN_TINT") != nullptr;
            float occW = kRainTint ? (2.0f + static_cast<float>(fi)) : params.debugRainOcclusion;
            if (params.debugRainRegions) occW += 10.0f;
            push.occluder = Vec4(f.occluder.x, f.occluder.y, f.occluder.z, occW);
            push.cloudWorldBounds = f.covBounds;
            push.params5.z = f.covWind.x;
            push.params5.w = f.covWind.y;
            // Scale the forced rain intensity by this cloud's loadedness so a
            // small but dark/heavy cloud pours harder than a big light one.
            push.params.y = std::max(weather.rainIntensity, m_followerRainIntensity * f.rainRate);
            push.cloudParams.y = static_cast<float>(fi); // own index in the occluder UBO
            if (fi < m_followerDescSets.size()) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                                        &m_followerDescSets[fi], 0, nullptr);
            }
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            const uint32_t instCount = (fi == m_rainFollowers.size() - 1 || fi >= wN)
                                           ? (m_particleCount - firstInst)
                                           : static_cast<uint32_t>(static_cast<float>(m_particleCount) * weights[fi] / wSum);
            if (instCount > 0) vkCmdDraw(cmd, 4, instCount, 0, firstInst);
            firstInst += instCount;
        }
    }

    // High tier: 3D splash decals on surfaces. Global group around the camera
    // plus one group per rain follower (ground splash under each independent
    // cloud). The splash shaders read cloudParams.w as opacity — the particle
    // draw carries the camera near plane there — so splash draws push w = 1.
    // Debug splash area (UI checkbox "Debug Splash Area" or
    // ERUPTION_TEST_SPLASH_VIZ=1): every splash renders as a pure RGB(255,0,0)
    // quad through the EXACT same gates as normal rendering — the red on the
    // ground is precisely where splashes really appear (splash_render,
    // params2.z = -1). The viz does NOT bypass the High-tier gate: on lower
    // tiers the 3D splashes never render, so red dots where the user never
    // sees white splashes would be a false reading (author feedback
    // 2026-08-09). Lower tiers are covered by the crown-splash debug in
    // weather_overlay.frag (cloudWindOffset.z).
    static const bool kSplashViz = std::getenv("ERUPTION_TEST_SPLASH_VIZ") != nullptr;
    const bool splashViz = kSplashViz || params.debugSplashArea > 0.5f;
    if (m_tier == WeatherTier::High && m_splashCount > 0) {
        // Splash placement anchors to the character (orbit target), not the
        // camera: the suppress-mode global group's origins are relative
        // offsets resolved against push.cameraPos in the vertex shader, and
        // cameraUnderCloud tests .y — the character's Y is the right test for
        // "is it raining where the player stands" (the orbit camera can sit
        // above the cloud base at high zoom).
        push.cameraPos = Vec4(rainAnchor, 0.0f);
        if (splashViz) push.params2.z = -1.0f;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_splashPipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_splashBuffer, &offset);

        if (m_splashGlobalCount > 0) {
            push.params5 = Vec4(weather.rainShadowSoftness, m_cloudBaseHeight,
                                m_cloudWindOffset.x, m_cloudWindOffset.y);
            // params.z (unused by the splash shaders) flags camera-relative
            // origins for the suppress-mode global group.
            push.params.z = m_suppressGlobalPrecip ? 1.0f : 0.0f;
            if (m_cloudCoverageMap) {
                Vec3 cmin = m_cloudCoverageMap->worldMin();
                Vec3 cmax = m_cloudCoverageMap->worldMax();
                push.cloudWorldBounds = Vec4(cmin.x, cmin.z, cmax.x - cmin.x, cmax.z - cmin.z);
            }
            push.cloudParams = Vec4(m_cloudLayer, m_cloudThreshold, m_cloudCoverage, 1.0f);
            push.cloudRange = Vec4(m_cloudBottom, m_cloudTop, m_cloudTop - m_cloudBottom, m_cloudScale);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                                    &m_descSets[0], 0, nullptr);
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDraw(cmd, 4, m_splashGlobalCount, 0, 0);
        }

        const size_t groupCount = std::min(m_splashGroups.size(), m_rainFollowers.size());
        for (size_t fi = 0; fi < groupCount; ++fi) {
            const RainFollower& f = m_rainFollowers[fi];
            // Force cameraUnderCloud = 1 (params5.y far above any camera): these
            // splashes belong to the cloud footprint and must show even when the
            // camera looks down from above the low independent cloud.
            push.params.z = 2.0f; // follower-relative origins (drift with the cloud)
            push.params5 = Vec4(weather.rainShadowSoftness, 1.0e9f, f.covWind.x, f.covWind.y);
            push.cloudWorldBounds = f.covBounds;
            push.cloudParams = Vec4(0.0f, m_cloudThreshold, 1.0f, 1.0f);
            push.cloudRange = Vec4(f.boxCenter.y, m_cloudTop, m_cloudTop - m_cloudBottom, m_cloudScale);
            // Debug splash area (params2.z = -1) reads these two to place/size
            // this cloud's spawn-region overlay; they otherwise hold stale
            // values from the rain-particle loop above. occluder.w carries the
            // group's firstInstance so the shader can identify its instance 0
            // (gl_InstanceIndex includes the firstInstance offset in Vulkan).
            push.rainBoxCenter = Vec4(f.boxCenter.x, 0.0f, f.boxCenter.z, 1.0f);
            push.occluder = Vec4(f.occluder.x, f.occluder.y, f.occluder.z,
                                 static_cast<float>(m_splashGroups[fi].first));
            VkDescriptorSet dset = (fi < m_followerDescSets.size()) ? m_followerDescSets[fi]
                                                                  : m_descSets[0];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                                    &dset, 0, nullptr);
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDraw(cmd, 4, m_splashGroups[fi].second, 0, m_splashGroups[fi].first);
        }
    }

    vkc::cmdEndRendering(cmd);
    wLap(3);
    if (kWrCpu && ++wN % 120 == 0) {
        ERUPTION_LOG_WARN("[WRCPU] particulas=%.2f splashes=%.2f updateDescSet_global=%.2f descSets_por_seguidor=%.2f gravacao_de_draws=%.2f (ms/frame, %u part, %u splash, %zu followers)",
                          wAcc[0]/120, wAcc[1]/120, wAcc[2]/120, wAcc[4]/120, wAcc[3]/120,
                          m_particleCount, m_splashCount, m_rainFollowers.size());
        for (double& v : wAcc) v = 0;
    }
}

} // namespace eruption
