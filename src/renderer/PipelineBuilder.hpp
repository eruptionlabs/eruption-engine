#pragma once

#include <vulkan/vulkan.h>
#include <vector>

namespace eruption {

class PipelineBuilder {
public:
    PipelineBuilder();
    PipelineBuilder& setShaderStages(const std::vector<VkPipelineShaderStageCreateInfo>& stages);
    PipelineBuilder& setVertexInput(const VkPipelineVertexInputStateCreateInfo& info);
    PipelineBuilder& setPrimitiveTopology(VkPrimitiveTopology topology);
    // Pontos de controle do patch (so' faz sentido com PATCH_LIST e os
    // estagios de tesselacao ligados; 0 = sem estagio de tesselacao).
    PipelineBuilder& setPatchControlPoints(uint32_t points);
    PipelineBuilder& setViewport(float x, float y, float w, float h);
    PipelineBuilder& setScissor(int32_t x, int32_t y, uint32_t w, uint32_t h);
    PipelineBuilder& setPolygonMode(VkPolygonMode mode);
    PipelineBuilder& setCullMode(VkCullModeFlags cull, VkFrontFace front);
    PipelineBuilder& setDepthState(bool test, bool write, VkCompareOp compare);
    PipelineBuilder& setBlendState(const std::vector<VkPipelineColorBlendAttachmentState>& attachments);
    PipelineBuilder& setDynamicState(const std::vector<VkDynamicState>& states);
    PipelineBuilder& setLayout(VkPipelineLayout layout);
    PipelineBuilder& setColorAttachmentFormats(const std::vector<VkFormat>& formats);
    PipelineBuilder& setDepthAttachmentFormat(VkFormat format);

    VkPipeline build(VkDevice device);

private:
    uint32_t m_patchControlPoints = 0;
    std::vector<VkPipelineShaderStageCreateInfo> m_stages;
    // sType has to be set even when nobody calls setVertexInput(): a plain {}
    // leaves it at 0 and every pipeline without explicit vertex input (all the
    // fullscreen passes) trips VUID-VkPipelineVertexInputStateCreateInfo-sType.
    VkPipelineVertexInputStateCreateInfo m_vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr, 0, 0, nullptr, 0, nullptr};
    VkPipelineInputAssemblyStateCreateInfo m_inputAssembly{};
    VkPipelineViewportStateCreateInfo m_viewportState{};
    VkViewport m_viewport{};
    VkRect2D m_scissor{};
    VkPipelineRasterizationStateCreateInfo m_rasterizer{};
    VkPipelineMultisampleStateCreateInfo m_multisampling{};
    VkPipelineDepthStencilStateCreateInfo m_depthStencil{};
    VkPipelineColorBlendStateCreateInfo m_colorBlending{};
    std::vector<VkPipelineColorBlendAttachmentState> m_blendAttachments;
    VkPipelineDynamicStateCreateInfo m_dynamicState{};
    std::vector<VkDynamicState> m_dynamicStates;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::vector<VkFormat> m_colorFormats;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
};

} // namespace eruption
