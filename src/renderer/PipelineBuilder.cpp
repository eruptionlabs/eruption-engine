#include "renderer/PipelineBuilder.hpp"
#include "renderer/VulkanContext.hpp"
#include "core/Logger.hpp"

namespace eruption {

PipelineBuilder::PipelineBuilder() {
    m_multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    m_multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
}

PipelineBuilder& PipelineBuilder::setShaderStages(const std::vector<VkPipelineShaderStageCreateInfo>& stages) {
    m_stages = stages;
    return *this;
}

PipelineBuilder& PipelineBuilder::setVertexInput(const VkPipelineVertexInputStateCreateInfo& info) {
    m_vertexInput = info;
    return *this;
}

PipelineBuilder& PipelineBuilder::setPrimitiveTopology(VkPrimitiveTopology topology) {
    m_inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    m_inputAssembly.topology = topology;
    m_inputAssembly.primitiveRestartEnable = VK_FALSE;
    return *this;
}

PipelineBuilder& PipelineBuilder::setPatchControlPoints(uint32_t points) {
    m_patchControlPoints = points;
    return *this;
}

PipelineBuilder& PipelineBuilder::setViewport(float x, float y, float w, float h) {
    m_viewport.x = x;
    m_viewport.y = y;
    m_viewport.width = w;
    m_viewport.height = h;
    m_viewport.minDepth = 0.0f;
    m_viewport.maxDepth = 1.0f;
    m_viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    m_viewportState.viewportCount = 1;
    m_viewportState.pViewports = &m_viewport;
    return *this;
}

PipelineBuilder& PipelineBuilder::setScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    m_scissor.offset = {x, y};
    m_scissor.extent = {w, h};
    m_viewportState.scissorCount = 1;
    m_viewportState.pScissors = &m_scissor;
    return *this;
}

PipelineBuilder& PipelineBuilder::setPolygonMode(VkPolygonMode mode) {
    m_rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    m_rasterizer.polygonMode = mode;
    m_rasterizer.lineWidth = 1.0f;
    m_rasterizer.cullMode = VK_CULL_MODE_NONE;
    m_rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    return *this;
}

PipelineBuilder& PipelineBuilder::setCullMode(VkCullModeFlags cull, VkFrontFace front) {
    m_rasterizer.cullMode = cull;
    m_rasterizer.frontFace = front;
    return *this;
}

PipelineBuilder& PipelineBuilder::setDepthState(bool test, bool write, VkCompareOp compare) {
    m_depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    m_depthStencil.depthTestEnable = test ? VK_TRUE : VK_FALSE;
    m_depthStencil.depthWriteEnable = write ? VK_TRUE : VK_FALSE;
    m_depthStencil.depthCompareOp = compare;
    return *this;
}

PipelineBuilder& PipelineBuilder::setBlendState(const std::vector<VkPipelineColorBlendAttachmentState>& attachments) {
    m_blendAttachments = attachments;
    m_colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    m_colorBlending.logicOpEnable = VK_FALSE;
    m_colorBlending.attachmentCount = static_cast<uint32_t>(m_blendAttachments.size());
    m_colorBlending.pAttachments = m_blendAttachments.data();
    return *this;
}

PipelineBuilder& PipelineBuilder::setDynamicState(const std::vector<VkDynamicState>& states) {
    m_dynamicStates = states;
    m_dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    m_dynamicState.dynamicStateCount = static_cast<uint32_t>(m_dynamicStates.size());
    m_dynamicState.pDynamicStates = m_dynamicStates.data();
    return *this;
}

PipelineBuilder& PipelineBuilder::setLayout(VkPipelineLayout layout) {
    m_pipelineLayout = layout;
    return *this;
}

PipelineBuilder& PipelineBuilder::setColorAttachmentFormats(const std::vector<VkFormat>& formats) {
    m_colorFormats = formats;
    return *this;
}

PipelineBuilder& PipelineBuilder::setDepthAttachmentFormat(VkFormat format) {
    m_depthFormat = format;
    return *this;
}

VkPipeline PipelineBuilder::build(VkDevice device) {
    // Dynamic state
    m_dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    m_dynamicState.dynamicStateCount = static_cast<uint32_t>(m_dynamicStates.size());
    m_dynamicState.pDynamicStates = m_dynamicStates.data();

    // If viewport/scissor are dynamic, Vulkan still requires a valid viewport state
    // with count=1 (the actual values are set via vkCmdSetViewport/Scissor).
    if (!m_viewportState.sType && !m_dynamicStates.empty()) {
        bool dynamicViewport = false, dynamicScissor = false;
        for (auto ds : m_dynamicStates) {
            if (ds == VK_DYNAMIC_STATE_VIEWPORT) dynamicViewport = true;
            if (ds == VK_DYNAMIC_STATE_SCISSOR) dynamicScissor = true;
        }
        if (dynamicViewport || dynamicScissor) {
            m_viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            m_viewportState.viewportCount = dynamicViewport ? 1 : 0;
            m_viewportState.scissorCount = dynamicScissor ? 1 : 0;
            m_viewportState.pViewports = nullptr;
            m_viewportState.pScissors = nullptr;
        }
    }

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(m_colorFormats.size());
    renderingInfo.pColorAttachmentFormats = m_colorFormats.empty() ? nullptr : m_colorFormats.data();
    renderingInfo.depthAttachmentFormat = m_depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    // Estagio de tesselacao: so' entra quando o pipeline pediu patch.
    VkPipelineTessellationStateCreateInfo tessState{};
    tessState.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessState.patchControlPoints = m_patchControlPoints;
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    if (!m_colorFormats.empty() || m_depthFormat != VK_FORMAT_UNDEFINED) {
        pipelineInfo.pNext = &renderingInfo;
    }
    pipelineInfo.stageCount = static_cast<uint32_t>(m_stages.size());
    pipelineInfo.pStages = m_stages.data();
    pipelineInfo.pVertexInputState = &m_vertexInput;
    pipelineInfo.pInputAssemblyState = &m_inputAssembly;
    pipelineInfo.pViewportState = &m_viewportState;
    pipelineInfo.pRasterizationState = &m_rasterizer;
    pipelineInfo.pMultisampleState = &m_multisampling;
    pipelineInfo.pDepthStencilState = &m_depthStencil;
    pipelineInfo.pColorBlendState = &m_colorBlending;
    pipelineInfo.pTessellationState = (m_patchControlPoints > 0) ? &tessState : nullptr;
    pipelineInfo.pDynamicState = m_dynamicStates.empty() ? nullptr : &m_dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE; // Dynamic rendering
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkc::createGraphicsPipelines(device, VulkanContext::globalPipelineCache(), 1, &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Failed to create graphics pipeline: %d", result);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

} // namespace eruption
