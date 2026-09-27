#include "renderer/SpriteShadowRenderer.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/SpriteSystem.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "core/Logger.hpp"
#include "formats/TerrainParser.hpp"

#include <imgui.h>
#include <cstring>
#include <algorithm>

namespace eruption {

struct QuadVertex {
    Vec2 position;
    Vec2 uv;
};

SpriteShadowRenderer::SpriteShadowRenderer() = default;
SpriteShadowRenderer::~SpriteShadowRenderer() { shutdown(); }

bool SpriteShadowRenderer::init(VulkanContext* ctx, GBuffer* gbuffer, BindlessDescriptor* bindless) {
    m_ctx = ctx;
    m_gbuffer = gbuffer;
    m_bindless = bindless;

    if (!createPipelineLayout()) return false;
    if (!createBlobPipeline()) return false;
    if (!createShadowSSBO()) return false;
    if (!createProxyBuffers()) return false;
    if (!createDescriptors()) return false;

    m_shadowData.reserve(m_maxSprites);
    m_planarIndices.reserve(m_maxSprites);
    m_blobIndices.reserve(m_maxSprites);

    ERUPTION_LOG_WARN("SpriteShadowRenderer initialized (max sprites: %u)", m_maxSprites);
    return true;
}

void SpriteShadowRenderer::shutdown() {
    if (m_blobPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_blobPipeline, nullptr);
        m_blobPipeline = VK_NULL_HANDLE;
    }
    if (m_planarPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_planarPipeline, nullptr);
        m_planarPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_shadowSSBO != VK_NULL_HANDLE) {
        vmaUnmapMemory(m_ctx->allocator(), m_shadowSSBOAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_shadowSSBO, m_shadowSSBOAlloc);
        m_shadowSSBO = VK_NULL_HANDLE;
        m_shadowSSBOAlloc = VK_NULL_HANDLE;
    }
    if (m_proxyInstanceBuffer != VK_NULL_HANDLE) {
        vmaUnmapMemory(m_ctx->allocator(), m_proxyInstanceAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_proxyInstanceBuffer, m_proxyInstanceAlloc);
        m_proxyInstanceBuffer = VK_NULL_HANDLE;
        m_proxyInstanceAlloc = VK_NULL_HANDLE;
    }
    if (m_proxyIndexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_proxyIndexBuffer, m_proxyIndexAlloc);
        m_proxyIndexBuffer = VK_NULL_HANDLE;
        m_proxyIndexAlloc = VK_NULL_HANDLE;
    }
    if (m_proxyVertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_proxyVertexBuffer, m_proxyVertexAlloc);
        m_proxyVertexBuffer = VK_NULL_HANDLE;
        m_proxyVertexAlloc = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_frameUboLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_frameUboLayout, nullptr);
        m_frameUboLayout = VK_NULL_HANDLE;
    }
}

bool SpriteShadowRenderer::createPipelineLayout() {
    // Descriptor set layout: binding 0 = SSBO (vertex), binding 1 = noise tex (fragment)
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_frameUboLayout));

    // Push constants: viewProj + worldPosAndScale + groundNormalAndAlpha + debugMode (112 bytes)
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 112;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_frameUboLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_ctx->device(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout));

    return true;
}

bool SpriteShadowRenderer::createBlobPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/shadow/sprite_blob_shadow.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/shadow/sprite_blob_shadow.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load blob shadow shaders");
        return false;
    }

    VkShaderModule vertModule, fragModule;
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

    // No vertex input — procedural quad via gl_VertexIndex
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 0;
    vertexInput.pVertexBindingDescriptions = nullptr;
    vertexInput.vertexAttributeDescriptionCount = 0;
    vertexInput.pVertexAttributeDescriptions = nullptr;

    // Alpha blending for easy visibility in both normal and debug modes.
    // Normal: black * alpha darkens the ground.
    // Debug:  red * 1.0 is clearly visible.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    auto pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, static_cast<float>(m_gbuffer->extent().width), static_cast<float>(m_gbuffer->extent().height))
        .setScissor(0, 0, m_gbuffer->extent().width, m_gbuffer->extent().height)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
        .setBlendState({blendAttachment})
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    if (pipeline == VK_NULL_HANDLE) return false;
    m_blobPipeline = pipeline;
    return true;
}

bool SpriteShadowRenderer::createShadowSSBO() {
    VkDeviceSize size = m_maxSprites * sizeof(SpriteShadowData);
    if (!m_ctx->createBuffer(size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU,
                             m_shadowSSBO, m_shadowSSBOAlloc)) {
        return false;
    }
    vmaMapMemory(m_ctx->allocator(), m_shadowSSBOAlloc, &m_mappedShadowSSBO);
    return true;
}

bool SpriteShadowRenderer::createProxyBuffers() {
    // Proxy unit cube vertices (8 vertices, will be indexed)
    Vec3 cubeVerts[8] = {
        Vec3(-0.5f, -0.5f, -0.5f),
        Vec3( 0.5f, -0.5f, -0.5f),
        Vec3( 0.5f,  0.5f, -0.5f),
        Vec3(-0.5f,  0.5f, -0.5f),
        Vec3(-0.5f, -0.5f,  0.5f),
        Vec3( 0.5f, -0.5f,  0.5f),
        Vec3( 0.5f,  0.5f,  0.5f),
        Vec3(-0.5f,  0.5f,  0.5f),
    };

    VkDeviceSize vsize = sizeof(cubeVerts);
    VkBuffer staging;
    VmaAllocation stagingAlloc;
    if (!m_ctx->createBuffer(vsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc)) {
        return false;
    }
    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, cubeVerts, vsize);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    if (!m_ctx->createBuffer(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_proxyVertexBuffer, m_proxyVertexAlloc)) {
        vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
        return false;
    }

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = vsize;
        vkCmdCopyBuffer(cmd, staging, m_proxyVertexBuffer, 1, &copy);
    });
    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    // Index buffer for cube (36 indices)
    uint16_t cubeIndices[36] = {
        0,1,2, 0,2,3,
        4,6,5, 4,7,6,
        0,4,5, 0,5,1,
        2,6,7, 2,7,3,
        0,3,7, 0,7,4,
        1,5,6, 1,6,2
    };
    VkDeviceSize isizeIdx = sizeof(cubeIndices);
    if (!m_ctx->createBuffer(isizeIdx, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc)) {
        return false;
    }
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, cubeIndices, isizeIdx);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    if (!m_ctx->createBuffer(isizeIdx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_proxyIndexBuffer, m_proxyIndexAlloc)) {
        vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
        return false;
    }
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = isizeIdx;
        vkCmdCopyBuffer(cmd, staging, m_proxyIndexBuffer, 1, &copy);
    });
    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    // Instance buffer for proxy matrices (maxSprites)
    VkDeviceSize isize = m_maxSprites * sizeof(Mat4);
    if (!m_ctx->createBuffer(isize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU,
                             m_proxyInstanceBuffer, m_proxyInstanceAlloc)) {
        return false;
    }
    vmaMapMemory(m_ctx->allocator(), m_proxyInstanceAlloc, &m_mappedProxyInstances);
    return true;
}

bool SpriteShadowRenderer::createDescriptors() {
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descriptorPool));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_frameUboLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_frameUboSet));

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_shadowSSBO;
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_frameUboSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    return true;
}

ShadowTechnique SpriteShadowRenderer::selectTechnique(float distance) const {
    if (!m_config.enabled) return ShadowTechnique::None;
    if (m_config.forceBlob) return ShadowTechnique::Blob;
    if (distance > m_config.maxBlobDistance) return ShadowTechnique::None;
    // Hybrid LOD: 0-N m planar, N-maxBlobDistance m blob.
    // Proxy geometry in the shadow map is disabled until a dedicated pipeline
    // is bound; planar shadows already give reliable close-range results.
    if (distance < m_config.maxPlanarDistance) return ShadowTechnique::Planar;
    return ShadowTechnique::Blob;
}

void SpriteShadowRenderer::update(const std::vector<Sprite>& visibleSprites,
                                  const Vec3& cameraPos,
                                  const TerrainFile* terrain,
                                  uint32_t frameCounter) {
    m_activeCount = 0;
    m_planarCount = 0;
    m_blobCount = 0;
    m_proxyCount = 0;

    m_shadowData.clear();
    m_planarIndices.clear();
    m_blobIndices.clear();

    uint32_t count = static_cast<uint32_t>(visibleSprites.size());
    count = std::min(count, m_maxSprites);

    // Batch raycast for ground height every 3 frames
    bool doRaycast = (frameCounter % 3 == 0) && (terrain != nullptr);

    // Temp storage to keep technique order: proxy -> planar -> blob
    std::vector<SpriteShadowData> proxyData;
    std::vector<SpriteShadowData> planarData;
    std::vector<SpriteShadowData> blobData;
    proxyData.reserve(count);
    planarData.reserve(count);
    blobData.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        const Sprite& sprite = visibleSprites[i];

        float dist = glm::distance(cameraPos, sprite.position);

        // Skip if no cast shadow flag
        bool hasCastShadow = (static_cast<uint32_t>(sprite.flags) & static_cast<uint32_t>(SpriteFlags::CastShadow)) != 0;
        if (!hasCastShadow) {
            // Log removido: era um WARN POR SPRITE POR FRAME (bomba armada -
            // a classe ainda nao esta ligada, mas quando ligar isso seria
            // milhares de syscalls/frame). Varredura de hot paths 2026-09-02.
            continue;
        }

        ShadowTechnique tech = selectTechnique(dist);
        if (tech == ShadowTechnique::None) {
            continue;
        }

        // Ground height query FIRST
        float groundY = sprite.position.y;
        if (doRaycast && terrain != nullptr) {
            groundY = TerrainParser::getTerrainHeightAt(*terrain, sprite.position.x, sprite.position.z);
        }

        SpriteShadowData data{};
        // Blob sits on the ground, not at sprite height
        Vec3 groundPos(sprite.position.x, groundY, sprite.position.z);
        data.worldPosAndScale = Vec4(groundPos, m_config.blobScale * sprite.size.x * 0.05f);
        data.groundNormalAndAlpha = Vec4(0.0f, 1.0f, 0.0f, m_config.shadowIntensity * m_config.globalIntensity);
        data.spriteRightAndSizeX = Vec4(Vec3(1.0f, 0.0f, 0.0f), sprite.size.x);
        data.spriteUpAndSizeY = Vec4(Vec3(0.0f, 1.0f, 0.0f), sprite.size.y);
        data.technique = static_cast<uint32_t>(tech);
        data.groundHeight = groundY;
        data.fadeStart = m_config.maxPlanarDistance;
        data.fadeEnd = m_config.maxBlobDistance;

        // Fade alpha by distance
        float fadeAlpha = 1.0f - glm::smoothstep(data.fadeStart, data.fadeEnd, dist);
        data.groundNormalAndAlpha.w *= fadeAlpha;

        // Fade out when sprite is high above ground
        float heightAboveGround = sprite.position.y - groundY;
        float heightFade = 1.0f - glm::smoothstep(0.0f, 5.0f, heightAboveGround);
        data.groundNormalAndAlpha.w *= heightFade;

        if (tech == ShadowTechnique::Planar) {
            m_planarIndices.push_back(i);
            planarData.push_back(data);
            m_planarCount++;
        } else if (tech == ShadowTechnique::Blob) {
            m_blobIndices.push_back(i);
            blobData.push_back(data);
            m_blobCount++;
        } else if (tech == ShadowTechnique::Proxy) {
            proxyData.push_back(data);
            m_proxyCount++;
        }

        m_activeCount++;
    }

    // Concatenate in order: proxy | planar | blob
    m_shadowData.reserve(proxyData.size() + planarData.size() + blobData.size());
    m_shadowData.insert(m_shadowData.end(), proxyData.begin(), proxyData.end());
    m_shadowData.insert(m_shadowData.end(), planarData.begin(), planarData.end());
    m_shadowData.insert(m_shadowData.end(), blobData.begin(), blobData.end());

    // Write to GPU SSBO
    if (!m_shadowData.empty() && m_mappedShadowSSBO != nullptr) {
        std::memcpy(m_mappedShadowSSBO, m_shadowData.data(),
                    m_shadowData.size() * sizeof(SpriteShadowData));
        vmaFlushAllocation(m_ctx->allocator(), m_shadowSSBOAlloc, 0,
                           m_shadowData.size() * sizeof(SpriteShadowData));
    }

    // Diagnostic logging every 60 frames
    if (frameCounter % 60 == 0) {
        ERUPTION_LOG_WARN("[SpriteShadow] visible=%u active=%u proxy=%u planar=%u blob=%u camera=(%.1f,%.1f,%.1f)",
                        count, m_activeCount, m_proxyCount, m_planarCount, m_blobCount,
                        cameraPos.x, cameraPos.y, cameraPos.z);
    }

    // Build proxy instance matrices (AABB) for shadow map
    if (m_mappedProxyInstances != nullptr) {
        Mat4* proxyMats = reinterpret_cast<Mat4*>(m_mappedProxyInstances);
        uint32_t proxyIdx = 0;
        for (uint32_t i = 0; i < count && proxyIdx < m_maxSprites; ++i) {
            const Sprite& sprite = visibleSprites[i];
            if ((static_cast<uint32_t>(sprite.flags) & static_cast<uint32_t>(SpriteFlags::CastShadow)) == 0)
                continue;
            float dist = glm::distance(cameraPos, sprite.position);
            if (dist > m_config.maxBlobDistance) continue;
            ShadowTechnique tech = selectTechnique(dist);
            if (tech != ShadowTechnique::Proxy) continue;

            // Build AABB matrix
            Vec3 center = sprite.position + Vec3(0.0f, sprite.size.y * 0.5f, 0.0f);
            Mat4 model = Mat4(1.0f);
            model = glm::translate(model, center);
            model = glm::scale(model, Vec3(sprite.size.x, sprite.size.y, sprite.size.x * 0.3f));
            proxyMats[proxyIdx++] = model;
        }
        if (proxyIdx > 0) {
            vmaFlushAllocation(m_ctx->allocator(), m_proxyInstanceAlloc, 0,
                               proxyIdx * sizeof(Mat4));
        }
    }
}

void SpriteShadowRenderer::renderPlanar(VkCommandBuffer cmd, const FrameUBO& frameUbo,
                                        VkBuffer spriteInstanceBuffer, uint32_t spriteCount) {
    // Planar shadows are still rendered by SpriteRenderer for compatibility.
    // This method exists for future expansion (e.g. instanced planar with SSBO).
    (void)cmd; (void)frameUbo; (void)spriteInstanceBuffer; (void)spriteCount;
}

void SpriteShadowRenderer::renderBlob(VkCommandBuffer cmd, const FrameUBO& frameUbo) {
    if (m_blobCount == 0 || m_blobPipeline == VK_NULL_HANDLE) {
        return;
    }

    ERUPTION_LOG_WARN("[SpriteShadow] renderBlob: drawing %u blobs", m_blobCount);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blobPipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_gbuffer->extent().width);
    viewport.height = static_cast<float>(m_gbuffer->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = m_gbuffer->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    Mat4 viewProj = frameUbo.viewProjection;

    // Draw each blob via push constants
    uint32_t blobOffset = m_proxyCount + m_planarCount;
    for (uint32_t i = 0; i < m_blobCount; ++i) {
        const SpriteShadowData& sd = m_shadowData[blobOffset + i];

        // Project blob center to screen space for debugging
        Vec4 clip = viewProj * Vec4(sd.worldPosAndScale.x, sd.worldPosAndScale.y, sd.worldPosAndScale.z, 1.0f);
        Vec3 ndc = Vec3(clip.x, clip.y, clip.z) / clip.w;
        Vec2 screen = Vec2((ndc.x * 0.5f + 0.5f) * viewport.width,
                           (ndc.y * 0.5f + 0.5f) * viewport.height);
        bool inFrustum = ndc.z >= 0.0f && ndc.z <= 1.0f &&
                         ndc.x >= -1.0f && ndc.x <= 1.0f &&
                         ndc.y >= -1.0f && ndc.y <= 1.0f;

        ERUPTION_LOG_WARN("[SpriteShadow] blob[%u] world=(%.1f,%.1f,%.1f) scale=%.1f alpha=%.2f screen=(%.0f,%.0f) ndcZ=%.2f inFrustum=%d",
                        i,
                        sd.worldPosAndScale.x, sd.worldPosAndScale.y, sd.worldPosAndScale.z,
                        sd.worldPosAndScale.w, sd.groundNormalAndAlpha.w,
                        screen.x, screen.y, ndc.z, inFrustum);

        struct BlobPush {
            Mat4 viewProj;
            Vec4 worldPosAndScale;
            Vec4 groundNormalAndAlpha;
            uint32_t debugMode;
            uint32_t pad[3];
        } push;

        push.viewProj = viewProj;
        push.worldPosAndScale = sd.worldPosAndScale;
        push.groundNormalAndAlpha = sd.groundNormalAndAlpha;
        push.debugMode = m_config.debugShowBlob ? 1u : 0u;
        push.pad[0] = push.pad[1] = push.pad[2] = 0;

        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(BlobPush), &push);

        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
}

void SpriteShadowRenderer::debugUI() {
    ImGui::TextColored(ImVec4(0, 1, 1, 1), "Sprite Shadow System");
    ImGui::Text("Active: %u | Planar: %u | Blob: %u | Proxy: %u",
                m_activeCount, m_planarCount, m_blobCount, m_proxyCount);
    ImGui::Text("GPU Time: %.3f ms", m_gpuTimeMs);
    ImGui::Separator();

    ImGui::SliderFloat("Global Intensity", &m_config.globalIntensity, 0.0f, 1.0f);
    ImGui::SliderFloat("Shadow Intensity", &m_config.shadowIntensity, 0.0f, 1.0f);
    ImGui::SliderFloat("Max Planar Dist", &m_config.maxPlanarDistance, 5.0f, 50.0f);
    ImGui::SliderFloat("Max Blob Dist", &m_config.maxBlobDistance, 30.0f, 150.0f);
    ImGui::SliderFloat("Blob Scale", &m_config.blobScale, 0.2f, 2.0f);
    ImGui::Checkbox("Force Blob", &m_config.forceBlob);
    ImGui::Checkbox("Align to Terrain", &m_config.alignToTerrain);
    ImGui::Checkbox("Enabled", &m_config.enabled);
    ImGui::Checkbox("Debug Show Blob", &m_config.debugShowBlob);
    if (ImGui::Button("Reload Shaders")) {
        reloadShaders();
    }
}

void SpriteShadowRenderer::reloadShaders() {
    if (m_ctx) {
        m_ctx->waitIdle();
    }
    if (m_blobPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_blobPipeline, nullptr);
        m_blobPipeline = VK_NULL_HANDLE;
    }
    createBlobPipeline();
    ERUPTION_LOG_WARN("Sprite shadow shaders reloaded.");
}

void SpriteShadowRenderer::generateProxyGeometry(const Sprite& sprite,
                                                  std::vector<Vec3>& outVertices,
                                                  std::vector<uint32_t>& outIndices) {
    // Option A: AABB aligned to sprite
    Vec3 center = sprite.position + Vec3(0.0f, sprite.size.y * 0.5f, 0.0f);
    Vec3 halfSize(sprite.size.x * 0.5f, sprite.size.y * 0.5f, sprite.size.x * 0.3f);

    uint32_t base = static_cast<uint32_t>(outVertices.size());
    outVertices.push_back(center + Vec3(-halfSize.x, -halfSize.y, -halfSize.z));
    outVertices.push_back(center + Vec3( halfSize.x, -halfSize.y, -halfSize.z));
    outVertices.push_back(center + Vec3( halfSize.x,  halfSize.y, -halfSize.z));
    outVertices.push_back(center + Vec3(-halfSize.x,  halfSize.y, -halfSize.z));
    outVertices.push_back(center + Vec3(-halfSize.x, -halfSize.y,  halfSize.z));
    outVertices.push_back(center + Vec3( halfSize.x, -halfSize.y,  halfSize.z));
    outVertices.push_back(center + Vec3( halfSize.x,  halfSize.y,  halfSize.z));
    outVertices.push_back(center + Vec3(-halfSize.x,  halfSize.y,  halfSize.z));

    // 12 triangles (36 indices) for the box
    const uint32_t faces[36] = {
        0,1,2, 0,2,3,
        4,6,5, 4,7,6,
        0,4,5, 0,5,1,
        2,6,7, 2,7,3,
        0,3,7, 0,7,4,
        1,5,6, 1,6,2
    };
    for (int i = 0; i < 36; ++i) {
        outIndices.push_back(base + faces[i]);
    }
}

void SpriteShadowRenderer::renderProxyShadows(VkCommandBuffer cmd,
                                               VkPipelineLayout shadowLayout,
                                               const Mat4& lightSpaceMatrix) {
    if (m_proxyCount == 0 || m_proxyVertexBuffer == VK_NULL_HANDLE || m_proxyIndexBuffer == VK_NULL_HANDLE) return;

    vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(Mat4), &lightSpaceMatrix);

    VkBuffer vertexBuffers[2] = {m_proxyVertexBuffer, m_proxyInstanceBuffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_proxyIndexBuffer, 0, VK_INDEX_TYPE_UINT16);

    vkCmdDrawIndexed(cmd, 36, m_proxyCount, 0, 0, 0);
}

} // namespace eruption
