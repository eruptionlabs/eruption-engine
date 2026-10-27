#include "renderer/GBuffer.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

#include <cstring>

namespace eruption {

struct QuadVertex {
    Vec2 position;
    Vec2 uv;
};

bool SpriteRenderer::init(VulkanContext* ctx, GBuffer* gbuffer, BindlessDescriptor* bindless) {
    m_ctx = ctx;
    m_gbuffer = gbuffer;
    m_bindless = bindless;

    if (!createQuadBuffer()) return false;
    if (!createBuffers()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;
    if (!createForwardPipeline()) return false;
    if (!createShadowPipeline()) return false;
    if (!createPlanarShadowPipeline()) return false;
    if (!createCircleShadowPipeline()) return false;
    if (!createRingPipeline()) return false;

    // Create palette sampler (Nearest)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_paletteSampler);

    ERUPTION_LOG_INFO("SpriteRenderer initialized (max instances: %d)", m_maxInstances);
    return true;
}

void SpriteRenderer::shutdown() {
    if (m_shadowLayoutSet != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_shadowLayoutSet, nullptr);
        m_shadowLayoutSet = VK_NULL_HANDLE;
    }
    if (m_circleShadowPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_circleShadowPipeline, nullptr);
        m_circleShadowPipeline = VK_NULL_HANDLE;
    }
    if (m_pointShadowPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pointShadowPipeline, nullptr);
        m_pointShadowPipeline = VK_NULL_HANDLE;
    }
    if (m_decalPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_decalPipelineLayout, nullptr);
        m_decalPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_ringPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_ctx->device(), m_ringPipeline, nullptr);
    if (m_ringPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_ctx->device(), m_ringPipelineLayout, nullptr);
    if (m_ringDepthPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_ctx->device(), m_ringDepthPool, nullptr);
    if (m_ringDepthLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_ctx->device(), m_ringDepthLayout, nullptr);
    m_ringPipeline = VK_NULL_HANDLE;
    m_ringPipelineLayout = VK_NULL_HANDLE;
    m_ringDepthPool = VK_NULL_HANDLE;
    m_ringDepthLayout = VK_NULL_HANDLE;
    if (m_decalDepthPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_decalDepthPool, nullptr);
        m_decalDepthPool = VK_NULL_HANDLE;
    }
    if (m_decalDepthLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_decalDepthLayout, nullptr);
        m_decalDepthLayout = VK_NULL_HANDLE;
    }
    if (m_decalDepthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_decalDepthSampler, nullptr);
        m_decalDepthSampler = VK_NULL_HANDLE;
    }
    if (m_planarShadowPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_planarShadowPipeline, nullptr);
        m_planarShadowPipeline = VK_NULL_HANDLE;
    }
    if (m_shadowPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_shadowPipeline, nullptr);
        m_shadowPipeline = VK_NULL_HANDLE;
    }
    if (m_forwardPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_forwardPipeline, nullptr);
        m_forwardPipeline = VK_NULL_HANDLE;
    }
    if (m_maskPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_ctx->device(), m_maskPipeline, nullptr);
    if (m_layerPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_ctx->device(), m_layerPipeline, nullptr);
    if (m_layerPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_ctx->device(), m_layerPipelineLayout, nullptr);
    m_layerPushRing.shutdown();
    m_maskPipeline = m_layerPipeline = VK_NULL_HANDLE;
    m_layerPipelineLayout = VK_NULL_HANDLE;
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_quadVertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_quadVertexBuffer, m_quadVertexAlloc);
        m_quadVertexBuffer = VK_NULL_HANDLE;
        m_quadVertexAlloc = VK_NULL_HANDLE;
    }
    if (m_instanceBuffer != VK_NULL_HANDLE) {
        vmaUnmapMemory(m_ctx->allocator(), m_instanceAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_instanceBuffer, m_instanceAlloc);
        m_instanceBuffer = VK_NULL_HANDLE;
        m_instanceAlloc = VK_NULL_HANDLE;
    }
    if (m_spriteLightBuffer != VK_NULL_HANDLE) {
        vmaUnmapMemory(m_ctx->allocator(), m_spriteLightAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_spriteLightBuffer, m_spriteLightAlloc);
        m_spriteLightBuffer = VK_NULL_HANDLE;
        m_spriteLightAlloc = VK_NULL_HANDLE;
        m_mappedSpriteLights = nullptr;
    }
    if (m_frameUboBuffer != VK_NULL_HANDLE) {
        vmaUnmapMemory(m_ctx->allocator(), m_frameUboAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_frameUboBuffer, m_frameUboAlloc);
        m_frameUboBuffer = VK_NULL_HANDLE;
        m_frameUboAlloc = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_frameUboLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_frameUboLayout, nullptr);
        m_frameUboLayout = VK_NULL_HANDLE;
    }
    if (m_paletteSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_paletteSampler, nullptr);
        m_paletteSampler = VK_NULL_HANDLE;
    }

    for (auto& pal : m_palettes) {
        vkDestroyImageView(m_ctx->device(), pal.view, nullptr);
        vmaDestroyImage(m_ctx->allocator(), pal.image, pal.alloc);
    }
    m_palettes.clear();

    // Pending palette uploads may still own staging buffers if the engine shuts
    // down before the deferred frame cleanup runs. Free them explicitly.
    for (auto& upload : m_pendingPaletteUploads) {
        if (upload.stagingBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_ctx->allocator(), upload.stagingBuffer, upload.stagingAlloc);
        }
    }
    m_pendingPaletteUploads.clear();
}

bool SpriteRenderer::createQuadBuffer() {
    QuadVertex vertices[6] = {
        {{-0.5f, -0.5f}, {0.0f, 1.0f}},
        {{ 0.5f, -0.5f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f}, {0.0f, 0.0f}},
    };

    VkDeviceSize size = sizeof(vertices);
    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer, stagingAlloc)) {
        return false;
    }

    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, vertices, size);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_quadVertexBuffer, m_quadVertexAlloc)) {
        vmaDestroyBuffer(m_ctx->allocator(), stagingBuffer, stagingAlloc);
        return false;
    }

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, stagingBuffer, m_quadVertexBuffer, 1, &copy);
    });

    vmaDestroyBuffer(m_ctx->allocator(), stagingBuffer, stagingAlloc);
    return true;
}

bool SpriteRenderer::createBuffers() {
    VkDeviceSize instanceSize = m_maxInstances * sizeof(SpriteInstanceData);
    if (!m_ctx->createBuffer(instanceSize,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU,
                             m_instanceBuffer, m_instanceAlloc)) {
        return false;
    }
    vmaMapMemory(m_ctx->allocator(), m_instanceAlloc, &m_mappedInstances);

    VkDeviceSize uboSize = sizeof(FrameUBO);
    if (!m_ctx->createBuffer(uboSize,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU,
                             m_frameUboBuffer, m_frameUboAlloc)) {
        return false;
    }
    vmaMapMemory(m_ctx->allocator(), m_frameUboAlloc, &m_mappedFrameUbo);

    if (!m_ctx->createBuffer(VkDeviceSize(m_maxInstances) * sizeof(SpriteLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, m_spriteLightBuffer, m_spriteLightAlloc)) {
        return false;
    }
    vmaMapMemory(m_ctx->allocator(), m_spriteLightAlloc, &m_mappedSpriteLights);
    std::memset(m_mappedSpriteLights, 0, size_t(m_maxInstances) * sizeof(SpriteLight));
    return true;
}

void SpriteRenderer::uploadSpriteLights(const std::vector<SpriteLight>& lights) {
    if (!m_mappedSpriteLights) return;
    const size_t n = std::min<size_t>(lights.size(), m_maxInstances);
    if (n) std::memcpy(m_mappedSpriteLights, lights.data(), n * sizeof(SpriteLight));
    // O resto zera: um sprite que saiu da lista nao herda a luz de outro.
    if (n < m_spriteLightsUsed)
        std::memset(static_cast<char*>(m_mappedSpriteLights) + n * sizeof(SpriteLight), 0,
                    (m_spriteLightsUsed - n) * sizeof(SpriteLight));
    m_spriteLightsUsed = n;
}

bool SpriteRenderer::createDescriptors() {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    // Os estagios de TESSELACAO tambem leem este UBO (model.tesc/model.tese
    // pegam o fator e a amplitude da banda perto). Sem eles no stageFlags a
    // leitura e' indefinida - neste driver voltava ZERO, e o deslocamento
    // simplesmente nao acontecia sem erro nenhum na tela.
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

    // Binding 1: luz principal por sprite (contraluz no sprite.frag).
    VkDescriptorSetLayoutBinding lightBinding{};
    lightBinding.binding = 1;
    lightBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    lightBinding.descriptorCount = 1;
    lightBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding frameBindings[2] = {uboBinding, lightBinding};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = frameBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_frameUboLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 10;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 10;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 10;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descriptorPool));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_frameUboLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_frameUboSet));

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_frameUboBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(FrameUBO);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_frameUboSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    VkDescriptorBufferInfo lightInfo{};
    lightInfo.buffer = m_spriteLightBuffer;
    lightInfo.offset = 0;
    lightInfo.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[2] = {write, write};
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &lightInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);

    // Shadow Descriptor Set (binding 0: shadowAtlas, binding 1: nextShadowAtlas)
    VkDescriptorSetLayoutBinding shadowBindings[2]{};
    shadowBindings[0].binding = 0;
    shadowBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBindings[0].descriptorCount = 1;
    shadowBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    shadowBindings[1].binding = 1;
    shadowBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBindings[1].descriptorCount = 1;
    shadowBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo shadowLayoutInfo{};
    shadowLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    shadowLayoutInfo.bindingCount = 2;
    shadowLayoutInfo.pBindings = shadowBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &shadowLayoutInfo, nullptr, &m_shadowLayoutSet));

    VkDescriptorSetAllocateInfo shadowAllocInfo{};
    shadowAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    shadowAllocInfo.descriptorPool = m_descriptorPool;
    shadowAllocInfo.descriptorSetCount = 1;
    shadowAllocInfo.pSetLayouts = &m_shadowLayoutSet;
    VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &shadowAllocInfo, &m_shadowSet));

    return true;
}

bool SpriteRenderer::createPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/sprite.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/sprite.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load sprite shaders");
        return false;
    }

    VkShaderModule vertModule;
    VkShaderModule fragModule;

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input: binding 0 = quad vertices, binding 1 = instance data
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(QuadVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(SpriteInstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[14];
    // Quad: position (loc 0), uv (loc 1)
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv)};
    // Instance: model matrix (loc 2-5)
    attributes[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix)};
    attributes[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + sizeof(Vec4)};
    attributes[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 2 * sizeof(Vec4)};
    attributes[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 3 * sizeof(Vec4)};
    // anchorPoint (loc 6)
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, anchorPoint)};
    // texRect (loc 7)
    attributes[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, texRect)};
    // texIndex (loc 8), normalTexIndex (loc 9), mrahwTexIndex (loc 10), flags (loc 11), paletteIndex (loc 12)
    attributes[8] = {8, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, texIndex)};
    attributes[9] = {9, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, normalTexIndex)};
    attributes[10] = {10, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, mrahwTexIndex)};
    attributes[11] = {11, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, flags)};
    attributes[12] = {12, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, paletteIndex)};
    // tintColor (loc 13)
    attributes[13] = {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, tintColor)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 14;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkDescriptorSetLayout layouts[2] = {
        m_bindless->layout(),
        m_frameUboLayout,
    };

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    // Layout: mat4 viewProjection followed by vec4 shadowLightDir (used by planar shadow pass).
    // Layout: mat4 viewProjection + vec4 shadowLightDir + vec2 planarParams(dilation, softness).
    pushRange.size = sizeof(Mat4) + sizeof(Vec4) + sizeof(Vec2);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout));

    // Um estado por alvo do G-buffer; sprite nao escreve velocidade (ele e'
    // redesenhado na camada do FSR, e o pixel fica com o movimento de camera).
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments = GBuffer::blendStates(false);

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, static_cast<float>(m_gbuffer->extent().width), static_cast<float>(m_gbuffer->extent().height))
        .setScissor(0, 0, m_gbuffer->extent().width, m_gbuffer->extent().height)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setBlendState(blendAttachments)
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats(GBuffer::colorAttachmentFormats())
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    if (pipeline == VK_NULL_HANDLE) return false;
    m_pipeline = pipeline;
    return true;
}

bool SpriteRenderer::createForwardPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/sprite_forward.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/sprite_forward.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load sprite forward shaders");
        return false;
    }

    VkShaderModule vertModule;
    VkShaderModule fragModule;

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input: binding 0 = quad vertices, binding 1 = instance data
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(QuadVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(SpriteInstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[14];
    // Quad: position (loc 0), uv (loc 1)
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv)};
    // Instance: model matrix (loc 2-5)
    attributes[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix)};
    attributes[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + sizeof(Vec4)};
    attributes[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 2 * sizeof(Vec4)};
    attributes[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 3 * sizeof(Vec4)};
    // anchorPoint (loc 6)
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, anchorPoint)};
    // texRect (loc 7)
    attributes[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, texRect)};
    // texIndex (loc 8), normalTexIndex (loc 9), mrahwTexIndex (loc 10), flags (loc 11), paletteIndex (loc 12)
    attributes[8] = {8, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, texIndex)};
    attributes[9] = {9, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, normalTexIndex)};
    attributes[10] = {10, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, mrahwTexIndex)};
    attributes[11] = {11, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, flags)};
    attributes[12] = {12, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, paletteIndex)};
    // tintColor (loc 13)
    attributes[13] = {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, tintColor)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 14;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, static_cast<float>(m_gbuffer->extent().width), static_cast<float>(m_gbuffer->extent().height))
        .setScissor(0, 0, m_gbuffer->extent().width, m_gbuffer->extent().height)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setBlendState({blend})
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    if (pipeline == VK_NULL_HANDLE) return false;
    m_forwardPipeline = pipeline;
    return true;
}

void SpriteRenderer::addSprite(const SpriteInstanceData& sprite) {
    if (m_spriteCount >= m_maxInstances) return;
    static_cast<SpriteInstanceData*>(m_mappedInstances)[m_spriteCount] = sprite;
    ++m_spriteCount;
}

void SpriteRenderer::clearSprites() {
    m_spriteCount = 0;
}

uint32_t SpriteRenderer::addPalette(const uint8_t* rgba256) {
    uint32_t width = 256;
    uint32_t height = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    PaletteTexture pal{};
    if (!m_ctx->createImage(width, height, format,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, pal.image, pal.alloc)) {
        return 0;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = pal.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &pal.view) != VK_SUCCESS) {
        return 0;
    }

    // Legível desde já: o conteúdo chega no próximo flushUploads, mas telas de
    // carregamento podem desenhar antes com o array de texturas ligado.
    m_ctx->transitionNow(pal.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    pal.bindlessSlot = m_bindless->allocateSlot();
    m_bindless->updateTexture(pal.bindlessSlot, pal.view, m_paletteSampler);
    m_bindless->flushUpdates();

    m_palettes.push_back(pal);
    updatePalette(pal.bindlessSlot, rgba256);

    return pal.bindlessSlot;
}

void SpriteRenderer::removePalette(uint32_t slot) {
    for (auto it = m_palettes.begin(); it != m_palettes.end(); ++it) {
        if (it->bindlessSlot == slot) {
            vkDestroyImageView(m_ctx->device(), it->view, nullptr);
            vmaDestroyImage(m_ctx->allocator(), it->image, it->alloc);
            m_bindless->freeSlot(it->bindlessSlot);
            m_palettes.erase(it);
            return;
        }
    }
}

void SpriteRenderer::updatePalette(uint32_t slot, const uint8_t* rgba256) {
    // Find palette by bindless slot
    int paletteIdx = -1;
    for (int i = 0; i < (int)m_palettes.size(); i++) {
        if (m_palettes[i].bindlessSlot == slot) {
            paletteIdx = i;
            break;
        }
    }
    if (paletteIdx < 0) {
        ERUPTION_LOG_WARN("updatePalette: slot %u not found", slot);
        return;
    }
    
    VkDeviceSize size = 256 * 4;
    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer, stagingAlloc)) {
        return;
    }

    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, rgba256, size);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    VkImage targetImage = m_palettes[paletteIdx].image;
    
    m_pendingPaletteUploads.push_back({stagingBuffer, stagingAlloc, targetImage});
}

void SpriteRenderer::flushUploads(VkCommandBuffer cmd) {
    if (m_pendingPaletteUploads.empty()) return;
    
    for (const auto& upload : m_pendingPaletteUploads) {
        m_ctx->cmdImageBarrier(cmd, upload.targetImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {256, 1, 1};
        vkCmdCopyBufferToImage(cmd, upload.stagingBuffer, upload.targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        m_ctx->cmdImageBarrier(cmd, upload.targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                               
        VkBuffer stagingBuf = upload.stagingBuffer;
        VmaAllocation stagingAll = upload.stagingAlloc;
        VulkanContext* ctx = m_ctx;
        m_ctx->deferFrameCleanup([ctx, stagingBuf, stagingAll]() {
            vmaDestroyBuffer(ctx->allocator(), stagingBuf, stagingAll);
        });
    }
    m_pendingPaletteUploads.clear();
}

void SpriteRenderer::updateFrameUbo(const FrameUBO& frameUbo) {
    FrameUBO finalUbo = frameUbo;
    finalUbo.spriteExposure = m_spriteExposure;
    std::memcpy(m_mappedFrameUbo, &finalUbo, sizeof(FrameUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_frameUboAlloc, 0, sizeof(FrameUBO));
}

void SpriteRenderer::renderSprites(VkCommandBuffer cmd, const FrameUBO& frameUbo) {
    renderSprites(cmd, frameUbo, m_instanceBuffer, m_spriteCount);
}

void SpriteRenderer::renderSprites(VkCommandBuffer cmd, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count) {
    if (count == 0) return;

    // Only flush if it's our internal buffer
    if (instanceBuffer == m_instanceBuffer) {
        vmaFlushAllocation(m_ctx->allocator(), m_instanceAlloc, 0,
                           count * sizeof(SpriteInstanceData));
    }

    updateFrameUbo(frameUbo);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(Mat4), &frameUbo.viewProjection);

    VkBuffer vertexBuffers[2] = {m_quadVertexBuffer, instanceBuffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);

    VkDescriptorSet sets[2] = {m_bindless->set(), m_frameUboSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 2, sets, 0, nullptr);

    vkCmdDraw(cmd, 6, count, 0, 0);
}

void SpriteRenderer::renderSpritesForward(VkCommandBuffer cmd, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count, const ShadowRenderer* /*shadows*/) {
    if (count == 0) return;

    if (instanceBuffer == m_instanceBuffer) {
        vmaFlushAllocation(m_ctx->allocator(), m_instanceAlloc, 0,
                           count * sizeof(SpriteInstanceData));
    }

    FrameUBO finalUbo = frameUbo;
    finalUbo.spriteExposure = m_spriteExposure;
    std::memcpy(m_mappedFrameUbo, &finalUbo, sizeof(FrameUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_frameUboAlloc, 0, sizeof(FrameUBO));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardPipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_gbuffer->extent().width);
    viewport.height = static_cast<float>(m_gbuffer->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = m_gbuffer->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(Mat4), &frameUbo.viewProjection);

    VkBuffer vertexBuffers[2] = {m_quadVertexBuffer, instanceBuffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);

    VkDescriptorSet sets[2] = {m_bindless->set(), m_frameUboSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 2, sets, 0, nullptr);

    vkCmdDraw(cmd, 6, count, 0, 0);
}

bool SpriteRenderer::createShadowPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/shadow/sprite_shadow.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/shadow/sprite_shadow.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load sprite shadow shaders");
        return false;
    }

    VkShaderModule vertModule;
    VkShaderModule fragModule;

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Same vertex input as regular sprite pipeline
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(QuadVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(SpriteInstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[14];
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv)};
    attributes[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix)};
    attributes[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + sizeof(Vec4)};
    attributes[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 2 * sizeof(Vec4)};
    attributes[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 3 * sizeof(Vec4)};
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, anchorPoint)};
    attributes[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, texRect)};
    attributes[8] = {8, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, texIndex)};
    attributes[9] = {9, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, normalTexIndex)};
    attributes[10] = {10, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, mrahwTexIndex)};
    attributes[11] = {11, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, flags)};
    attributes[12] = {12, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, paletteIndex)};
    attributes[13] = {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, tintColor)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 14;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0;
    blend.blendEnable = VK_FALSE;

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, 2048.0f, 2048.0f) // Dynamic
        .setScissor(0, 0, 2048, 2048)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setBlendState({blend})
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    if (pipeline == VK_NULL_HANDLE) return false;
    m_shadowPipeline = pipeline;
    return true;
}

void SpriteRenderer::renderBillboardPlanarShadows(VkCommandBuffer cmd, const FrameUBO& frameUbo, const Vec3& shadowLightDir,
                                                  const Vec2& planarParams, VkBuffer instanceBuffer, uint32_t count) {
    if (count == 0) return;

    if (instanceBuffer == m_instanceBuffer) {
        vmaFlushAllocation(m_ctx->allocator(), m_instanceAlloc, 0,
                           count * sizeof(SpriteInstanceData));
    }

    FrameUBO finalUbo = frameUbo;
    finalUbo.spriteExposure = m_spriteExposure;
    std::memcpy(m_mappedFrameUbo, &finalUbo, sizeof(FrameUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_frameUboAlloc, 0, sizeof(FrameUBO));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_planarShadowPipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_gbuffer->extent().width);
    viewport.height = static_cast<float>(m_gbuffer->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = m_gbuffer->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    struct PlanarPushConstants {
        Mat4 viewProjection;
        Vec4 shadowLightDir;
        Vec2 planarParams; // x = dilation, y = softness
    } push;
    push.viewProjection = frameUbo.viewProjection;
    push.shadowLightDir = Vec4(shadowLightDir, 0.0f);
    push.planarParams = planarParams;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push), &push);

    VkBuffer vertexBuffers[2] = {m_quadVertexBuffer, instanceBuffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);

    VkDescriptorSet sets[2] = {m_bindless->set(), m_frameUboSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 2, sets, 0, nullptr);

    vkCmdDraw(cmd, 6, count, 0, 0);
}

bool SpriteRenderer::createPlanarShadowPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/shadow/billboard_planar_shadow.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/shadow/billboard_planar_shadow.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load billboard planar shadow shaders");
        return false;
    }

    VkShaderModule vertModule;
    VkShaderModule fragModule;

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Same vertex input as forward sprite pipeline
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(QuadVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(SpriteInstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[14];
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv)};
    attributes[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix)};
    attributes[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + sizeof(Vec4)};
    attributes[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 2 * sizeof(Vec4)};
    attributes[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 3 * sizeof(Vec4)};
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, anchorPoint)};
    attributes[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, texRect)};
    attributes[8] = {8, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, texIndex)};
    attributes[9] = {9, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, normalTexIndex)};
    attributes[10] = {10, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, mrahwTexIndex)};
    attributes[11] = {11, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, flags)};
    attributes[12] = {12, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, paletteIndex)};
    attributes[13] = {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, tintColor)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 14;
    vertexInput.pVertexAttributeDescriptions = attributes;

    // Multiplicative blending that darkens the lit image without touching its
    // alpha channel.  The post-processor uses alpha for transparency, so we
    // encode the shadow intensity in the RGB color instead.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, static_cast<float>(m_gbuffer->extent().width), static_cast<float>(m_gbuffer->extent().height))
        .setScissor(0, 0, m_gbuffer->extent().width, m_gbuffer->extent().height)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(false, false, VK_COMPARE_OP_ALWAYS) // shadow is a screen-space darken; no depth test
        .setBlendState({blend})
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    if (pipeline == VK_NULL_HANDLE) return false;
    m_planarShadowPipeline = pipeline;
    return true;
}

void SpriteRenderer::renderBillboardCircleShadows(VkCommandBuffer cmd, const FrameUBO& frameUbo,
                                                  const Vec2& circleParams, VkBuffer instanceBuffer, uint32_t count,
                                                  VkImageView sceneDepth, const Vec2& planarParams,
                                                  const std::vector<SpritePointShadow>& pointShadows,
                                                  VkImageView sunShadowDepth, VkBuffer sunShadowUbo, const Vec4& sunGate) {
    if (count == 0 || sceneDepth == VK_NULL_HANDLE || m_circleShadowPipeline == VK_NULL_HANDLE) return;
    if (sunShadowDepth == VK_NULL_HANDLE || sunShadowUbo == VK_NULL_HANDLE) return;

    VkDescriptorSet depthSet = m_decalDepthSets[m_ctx->currentFrame() % kDecalFrames];
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler = m_decalDepthSampler;
    depthInfo.imageView = sceneDepth;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // 1 e 2: o mapa de sombra do sol, escritos sempre (o shader os declara)
    // mesmo quando o disco nao consulta o sol.
    VkDescriptorImageInfo sunInfo = depthInfo;
    sunInfo.imageView = sunShadowDepth;
    VkDescriptorBufferInfo sunUboInfo{sunShadowUbo, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = depthSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &depthInfo;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &sunInfo;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &sunUboInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 3, writes, 0, nullptr);

    if (instanceBuffer == m_instanceBuffer) {
        vmaFlushAllocation(m_ctx->allocator(), m_instanceAlloc, 0,
                           count * sizeof(SpriteInstanceData));
    }

    FrameUBO finalUbo = frameUbo;
    finalUbo.spriteExposure = m_spriteExposure;
    std::memcpy(m_mappedFrameUbo, &finalUbo, sizeof(FrameUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_frameUboAlloc, 0, sizeof(FrameUBO));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_circleShadowPipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_gbuffer->extent().width);
    viewport.height = static_cast<float>(m_gbuffer->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = m_gbuffer->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    struct CirclePushConstants {
        Mat4 viewProjection;
        Vec4 shadowLightDir;
        Vec2 circleParams; // x = dilation/radius, y = softness
    } push;
    push.viewProjection = frameUbo.viewProjection;
    push.shadowLightDir = sunGate;
    push.circleParams = circleParams;

    vkCmdPushConstants(cmd, m_decalPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push), &push);

    VkBuffer vertexBuffers[2] = {m_quadVertexBuffer, instanceBuffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);

    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, depthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_decalPipelineLayout,
                            0, 3, sets, 0, nullptr);

    vkCmdDraw(cmd, 6, count, 0, 0);

    // Uma sombra por (sprite, luz pontual), no mesmo set de profundidade: o
    // descritor e' escrito uma vez por quadro, antes de qualquer desenho.
    if (pointShadows.empty() || m_pointShadowPipeline == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pointShadowPipeline);
    push.circleParams = planarParams;
    for (const SpritePointShadow& s : pointShadows) {
        push.shadowLightDir = Vec4(s.light, s.strength);
        vkCmdPushConstants(cmd, m_decalPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 6, 1, 0, s.instance);
    }
}

bool SpriteRenderer::createCircleShadowPipeline() {
    // Set 2 do decal: a profundidade da cena. Sem filtro (D32 nao filtra
    // linear em GPU de celular; o shader usa texelFetch).
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_decalDepthSampler));

    // 0 = profundidade da cena; 1 e 2 = mapa de sombra do sol e as matrizes
    // dele ("map_contact": o disco so' fora do sol).
    VkDescriptorSetLayoutBinding depthBindings[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        depthBindings[i].binding = i;
        depthBindings[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depthBindings[i].descriptorCount = 1;
        depthBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 3;
    setLayoutInfo.pBindings = depthBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &setLayoutInfo, nullptr, &m_decalDepthLayout));

    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kDecalFrames},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDecalFrames}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kDecalFrames;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_decalDepthPool));
    VkDescriptorSetLayout depthLayouts[kDecalFrames] = {m_decalDepthLayout, m_decalDepthLayout, m_decalDepthLayout};
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = m_decalDepthPool;
    setAlloc.descriptorSetCount = kDecalFrames;
    setAlloc.pSetLayouts = depthLayouts;
    VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &setAlloc, m_decalDepthSets));

    // Sets 0-1 e push constant iguais ao layout dos sprites (compativel ao
    // ligar os sets), mais o set 2.
    const VkDescriptorSetLayout decalSets[3] = {m_bindless->layout(), m_frameUboLayout, m_decalDepthLayout};
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.size = sizeof(Mat4) + sizeof(Vec4) + sizeof(Vec2);
    VkPipelineLayoutCreateInfo decalLayoutInfo{};
    decalLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    decalLayoutInfo.setLayoutCount = 3;
    decalLayoutInfo.pSetLayouts = decalSets;
    decalLayoutInfo.pushConstantRangeCount = 1;
    decalLayoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_ctx->device(), &decalLayoutInfo, nullptr, &m_decalPipelineLayout));

    m_circleShadowPipeline = createDecalPipeline("shaders/shadow/billboard_circle_shadow");
    m_pointShadowPipeline = createDecalPipeline("shaders/shadow/billboard_point_shadow");
    return m_circleShadowPipeline != VK_NULL_HANDLE && m_pointShadowPipeline != VK_NULL_HANDLE;
}

bool SpriteRenderer::createRingPipeline() {
    // 0 = profundidade da cena; 1 = pbr do G-buffer (r = 0,75 marca sprite).
    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 2;
    setLayoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &setLayoutInfo, nullptr, &m_ringDepthLayout));

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kDecalFrames};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kDecalFrames;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_ringDepthPool));
    VkDescriptorSetLayout layouts[kDecalFrames] = {m_ringDepthLayout, m_ringDepthLayout, m_ringDepthLayout};
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = m_ringDepthPool;
    setAlloc.descriptorSetCount = kDecalFrames;
    setAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &setAlloc, m_ringDepthSets));

    const VkDescriptorSetLayout sets[3] = {m_bindless->layout(), m_frameUboLayout, m_ringDepthLayout};
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size = sizeof(GroundRing);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = sets;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_ringPipelineLayout));

    auto vertCode = ShaderCompiler::loadSPIRV("shaders/shadow/ground_ring.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/shadow/ground_ring.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load ground ring shaders");
        return false;
    }
    VkShaderModule modules[2] = {};
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &modules[0]);
    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &modules[1]);
    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    for (uint32_t i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }

    // Mistura alfa comum na cor; o alfa da imagem lit (transparencia do pos)
    // fica como estava.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    m_ringPipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, static_cast<float>(m_gbuffer->extent().width), static_cast<float>(m_gbuffer->extent().height))
        .setScissor(0, 0, m_gbuffer->extent().width, m_gbuffer->extent().height)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
        .setBlendState({blend})
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_ringPipelineLayout)
        .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
        .build(m_ctx->device());
    vkDestroyShaderModule(m_ctx->device(), modules[0], nullptr);
    vkDestroyShaderModule(m_ctx->device(), modules[1], nullptr);
    return m_ringPipeline != VK_NULL_HANDLE;
}

void SpriteRenderer::renderGroundRings(VkCommandBuffer cmd, const FrameUBO& frameUbo,
                                       const std::vector<GroundRing>& rings, VkImageView sceneDepth,
                                       VkImageView gbufferPbr) {
    if (rings.empty() || sceneDepth == VK_NULL_HANDLE || gbufferPbr == VK_NULL_HANDLE ||
        m_ringPipeline == VK_NULL_HANDLE)
        return;
    VkDescriptorSet depthSet = m_ringDepthSets[m_ctx->currentFrame() % kDecalFrames];
    const VkDescriptorImageInfo infos[2] = {
        {m_decalDepthSampler, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_decalDepthSampler, gbufferPbr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = depthSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);

    FrameUBO finalUbo = frameUbo;
    finalUbo.spriteExposure = m_spriteExposure;
    std::memcpy(m_mappedFrameUbo, &finalUbo, sizeof(FrameUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_frameUboAlloc, 0, sizeof(FrameUBO));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ringPipeline);
    VkViewport viewport{};
    viewport.width = static_cast<float>(m_gbuffer->extent().width);
    viewport.height = static_cast<float>(m_gbuffer->extent().height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = m_gbuffer->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, depthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ringPipelineLayout, 0, 3, sets, 0, nullptr);
    for (const GroundRing& r : rings) {
        vkCmdPushConstants(cmd, m_ringPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(GroundRing), &r);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
}

// Pipeline de decal de sombra (disco do pe', sombra de luz pontual): mesma
// entrada de vertice dos sprites, layout com a profundidade no set 2 e mistura
// multiplicativa.
VkPipeline SpriteRenderer::createDecalPipeline(const std::string& shaderBase) {
    auto vertCode = ShaderCompiler::loadSPIRV(shaderBase + ".vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV(shaderBase + ".frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load decal shadow shaders %s", shaderBase.c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModule vertModule;
    VkShaderModule fragModule;

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(QuadVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(SpriteInstanceData);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[14];
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv)};
    attributes[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix)};
    attributes[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + sizeof(Vec4)};
    attributes[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 2 * sizeof(Vec4)};
    attributes[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, modelMatrix) + 3 * sizeof(Vec4)};
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, anchorPoint)};
    attributes[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, texRect)};
    attributes[8] = {8, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, texIndex)};
    attributes[9] = {9, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, normalTexIndex)};
    attributes[10] = {10, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, mrahwTexIndex)};
    attributes[11] = {11, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, flags)};
    attributes[12] = {12, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, paletteIndex)};
    attributes[13] = {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, tintColor)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 14;
    vertexInput.pVertexAttributeDescriptions = attributes;

    // Multiplicative blending that darkens the lit image without touching its
    // alpha channel.  The post-processor uses alpha for transparency, so we
    // encode the shadow intensity in the RGB color instead.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, static_cast<float>(m_gbuffer->extent().width), static_cast<float>(m_gbuffer->extent().height))
        .setScissor(0, 0, m_gbuffer->extent().width, m_gbuffer->extent().height)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(false, false, VK_COMPARE_OP_ALWAYS) // decal: o teste e' a profundidade lida no shader
        .setBlendState({blend})
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_decalPipelineLayout)
        .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);
    return pipeline;
}

void SpriteRenderer::renderShadow(VkCommandBuffer cmd, VkPipelineLayout /*shadowLayout*/, const Mat4& lightSpaceMatrix, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count) {
    if (count == 0) return;

    if (instanceBuffer == m_instanceBuffer) {
        vmaFlushAllocation(m_ctx->allocator(), m_instanceAlloc, 0, count * sizeof(SpriteInstanceData));
    }

    FrameUBO finalUbo = frameUbo;
    finalUbo.spriteExposure = m_spriteExposure;
    std::memcpy(m_mappedFrameUbo, &finalUbo, sizeof(FrameUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_frameUboAlloc, 0, sizeof(FrameUBO));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(Mat4), &lightSpaceMatrix);

    VkBuffer vertexBuffers[2] = {m_quadVertexBuffer, instanceBuffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);

    VkDescriptorSet sets[2] = {m_bindless->set(), m_frameUboSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 2, sets, 0, nullptr);

    vkCmdDraw(cmd, 6, count, 0, 0);
}

bool SpriteRenderer::createLayerPipelines(VkDescriptorSetLayout layerInputs) {
    if (m_maskPipeline != VK_NULL_HANDLE) return true;
    VkDevice dev = m_ctx->device();
    auto loadModule = [&](const char* path) -> VkShaderModule {
        const auto code = ShaderCompiler::loadSPIRV(path);
        if (code.empty()) {
            ERUPTION_LOG_ERROR("SpriteRenderer: %s nao encontrado", path);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = code.size() * sizeof(uint32_t);
        smi.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smi, nullptr, &m);
        return m;
    };

    // Mesmo layout de vertice das outras pipelines de sprite.
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0] = {0, sizeof(QuadVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    bindings[1] = {1, sizeof(SpriteInstanceData), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription attributes[14];
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv)};
    for (uint32_t r = 0; r < 4; ++r)
        attributes[2 + r] = {2 + r, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                             static_cast<uint32_t>(offsetof(SpriteInstanceData, modelMatrix) + r * sizeof(Vec4))};
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, anchorPoint)};
    attributes[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, texRect)};
    attributes[8] = {8, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, texIndex)};
    attributes[9] = {9, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, normalTexIndex)};
    attributes[10] = {10, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, mrahwTexIndex)};
    attributes[11] = {11, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, flags)};
    attributes[12] = {12, 1, VK_FORMAT_R32_UINT, offsetof(SpriteInstanceData, paletteIndex)};
    attributes[13] = {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteInstanceData, tintColor)};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 14;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineColorBlendAttachmentState opaque{};
    opaque.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    auto stagesFor = [](VkShaderModule v, VkShaderModule f) {
        std::vector<VkPipelineShaderStageCreateInfo> st(2);
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main", nullptr};
        return st;
    };

    // 1) Mascara: vertex do G-buffer, depth do G-buffer so' para teste.
    VkShaderModule vMask = loadModule("gbuffer/sprite_mask.vert.spv");
    VkShaderModule fMask = loadModule("gbuffer/sprite_mask.frag.spv");
    if (vMask && fMask) {
        m_maskPipeline = PipelineBuilder()
            .setShaderStages(stagesFor(vMask, fMask))
            .setVertexInput(vertexInput)
            .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setViewport(0, 0, 1, 1)
            .setScissor(0, 0, 1, 1)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthState(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setBlendState({opaque})
            .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setLayout(m_pipelineLayout)
            .setColorAttachmentFormats({VK_FORMAT_R8_UNORM})
            .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
            .build(dev);
    }
    if (vMask) vkDestroyShaderModule(dev, vMask, nullptr);
    if (fMask) vkDestroyShaderModule(dev, fMask, nullptr);

    // 2) Camada: set 0 bindless, set 1 FrameUBO, set 2 entradas da camada.
    // LayerPush (160 B) vai no set 3 pelo PushRing: passa dos 128 de push constant.
    if (!m_layerPushRing.init(m_ctx, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(LayerPush),
                              8 * 1024))
        return false;
    const VkDescriptorSetLayout layouts[4] = {m_bindless->layout(), m_frameUboLayout, layerInputs,
                                              m_layerPushRing.layout()};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 4;
    li.pSetLayouts = layouts;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &m_layerPipelineLayout) != VK_SUCCESS) return false;
    VkShaderModule vLayer = loadModule("gbuffer/sprite_layer.vert.spv");
    VkShaderModule fLayer = loadModule("gbuffer/sprite_layer.frag.spv");
    if (vLayer && fLayer) {
        m_layerPipeline = PipelineBuilder()
            .setShaderStages(stagesFor(vLayer, fLayer))
            .setVertexInput(vertexInput)
            .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setViewport(0, 0, 1, 1)
            .setScissor(0, 0, 1, 1)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
            .setBlendState({opaque})
            .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setLayout(m_layerPipelineLayout)
            .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
            .build(dev);
    }
    if (vLayer) vkDestroyShaderModule(dev, vLayer, nullptr);
    if (fLayer) vkDestroyShaderModule(dev, fLayer, nullptr);
    return m_maskPipeline != VK_NULL_HANDLE && m_layerPipeline != VK_NULL_HANDLE;
}

void SpriteRenderer::drawMask(VkCommandBuffer cmd, VkBuffer instances, uint32_t count, const Mat4& viewProjJittered) {
    if (count == 0 || m_maskPipeline == VK_NULL_HANDLE) return;
    // O FrameUBO ja' foi gravado neste frame pelo passe do G-buffer (mesmas
    // matrizes com jitter): o vertex shader e' o mesmo, a cobertura bate.
    const VkExtent2D ext = m_gbuffer->extent();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_maskPipeline);
    VkViewport vp{0.0f, 0.0f, float(ext.width), float(ext.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &viewProjJittered);
    VkBuffer vbs[2] = {m_quadVertexBuffer, instances};
    VkDeviceSize offs[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    VkDescriptorSet sets[2] = {m_bindless->set(), m_frameUboSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(cmd, 6, count, 0, 0);
}

void SpriteRenderer::drawLayer(VkCommandBuffer cmd, VkBuffer instances, uint32_t count, VkDescriptorSet layerInputs,
                               const LayerPush& push, VkExtent2D extent) {
    if (count == 0 || m_layerPipeline == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layerPipeline);
    VkViewport vp{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    m_layerPushRing.push(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layerPipelineLayout, 3, &push, sizeof(LayerPush));
    VkBuffer vbs[2] = {m_quadVertexBuffer, instances};
    VkDeviceSize offs[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, layerInputs};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layerPipelineLayout, 0, 3, sets, 0, nullptr);
    vkCmdDraw(cmd, 6, count, 0, 0);
}

} // namespace eruption
