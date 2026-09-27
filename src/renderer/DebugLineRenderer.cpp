#include "renderer/DebugLineRenderer.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

namespace eruption {

bool DebugLineRenderer::init(VulkanContext* ctx) {
    m_ctx = ctx;
    createPipeline();
    m_initialized = (m_pipeline != VK_NULL_HANDLE);
    return m_initialized;
}

void DebugLineRenderer::shutdown() {
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_vertexBuffer, m_vertexAlloc);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexAlloc = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    m_lines.clear();
    m_initialized = false;
    m_dirty = true;
}

void DebugLineRenderer::addLine(const Vec3& a, const Vec3& b, const Vec3& color) {
    m_lines.push_back({a, color});
    m_lines.push_back({b, color});
    m_dirty = true;
}

void DebugLineRenderer::addBox(const Vec3& min, const Vec3& max, const Vec3& color) {
    addLine(Vec3(min.x, min.y, min.z), Vec3(max.x, min.y, min.z), color);
    addLine(Vec3(max.x, min.y, min.z), Vec3(max.x, min.y, max.z), color);
    addLine(Vec3(max.x, min.y, max.z), Vec3(min.x, min.y, max.z), color);
    addLine(Vec3(min.x, min.y, max.z), Vec3(min.x, min.y, min.z), color);

    addLine(Vec3(min.x, max.y, min.z), Vec3(max.x, max.y, min.z), color);
    addLine(Vec3(max.x, max.y, min.z), Vec3(max.x, max.y, max.z), color);
    addLine(Vec3(max.x, max.y, max.z), Vec3(min.x, max.y, max.z), color);
    addLine(Vec3(min.x, max.y, max.z), Vec3(min.x, max.y, min.z), color);

    addLine(Vec3(min.x, min.y, min.z), Vec3(min.x, max.y, min.z), color);
    addLine(Vec3(max.x, min.y, min.z), Vec3(max.x, max.y, min.z), color);
    addLine(Vec3(max.x, min.y, max.z), Vec3(max.x, max.y, max.z), color);
    addLine(Vec3(min.x, min.y, max.z), Vec3(min.x, max.y, max.z), color);
}

void DebugLineRenderer::clearLines() {
    m_lines.clear();
    m_dirty = true;
}

void DebugLineRenderer::upload() {
    if (!m_dirty || m_lines.empty()) return;

    VkDeviceSize neededSize = m_lines.size() * sizeof(LineVertex);

    // Recreate buffer if too small
    if (m_vertexBuffer == VK_NULL_HANDLE || m_vertexBufferSize < neededSize) {
        if (m_vertexBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_ctx->allocator(), m_vertexBuffer, m_vertexAlloc);
        }
        m_ctx->createBuffer(neededSize,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_vertexBuffer, m_vertexAlloc);
        m_vertexBufferSize = neededSize;
    }

    // Staging upload
    VkBuffer stagingBuf;
    VmaAllocation stagingAlloc;
    m_ctx->createBuffer(neededSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_CPU_ONLY, stagingBuf, stagingAlloc);

    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, m_lines.data(), neededSize);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = neededSize;
        vkCmdCopyBuffer(cmd, stagingBuf, m_vertexBuffer, 1, &copy);
    });

    vmaDestroyBuffer(m_ctx->allocator(), stagingBuf, stagingAlloc);
    m_dirty = false;
}

void DebugLineRenderer::render(VkCommandBuffer cmd, const Mat4& viewProj) {
    if (!m_initialized || m_lines.empty()) return;

    if (m_dirty) upload();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(Mat4), &viewProj);

    // The pipeline uses dynamic viewport/scissor; the state left in the
    // command buffer may be the minimap's small rect, which would clip the
    // lines away. Reset both to the full swapchain.
    VkExtent2D ext = m_ctx->swapExtent();
    VkViewport vp{0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkBuffer vb = m_vertexBuffer;
    VkDeviceSize offset = 0;
    // draw lines without spamming log
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(m_lines.size()), 1, 0, 0);
}

void DebugLineRenderer::createPipeline() {
    std::vector<uint32_t> vertCode, fragCode;
    if (!ShaderCompiler::compileOrLoad("shaders/debug/line.vert", "shaders/debug/line.vert.spv",
                                       ShaderStage::Vertex, vertCode)) {
        ERUPTION_LOG_ERROR("Failed to compile line.vert shader");
        return;
    }
    if (!ShaderCompiler::compileOrLoad("shaders/debug/line.frag", "shaders/debug/line.frag.spv",
                                       ShaderStage::Fragment, fragCode)) {
        ERUPTION_LOG_ERROR("Failed to compile line.frag shader");
        return;
    }

    VkShaderModuleCreateInfo smInfo{}; smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    VkShaderModule vertModule;
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    VkShaderModule fragModule;
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0] = {}; stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = {}; stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(LineVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attribs(2);
    attribs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, position)};
    attribs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, color)};

    VkPipelineVertexInputStateCreateInfo vertexInput{}; vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertexInput.pVertexAttributeDescriptions = attribs.data();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(Mat4);

    VkPipelineLayoutCreateInfo layoutInfo{}; layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout);

    std::vector<VkPipelineColorBlendAttachmentState> blends(1);
    blends[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blends[0].blendEnable = VK_FALSE;

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
        .setBlendState(blends)
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    m_pipeline = pipeline;
}

} // namespace eruption
