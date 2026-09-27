#include "renderer/CloudLayerRenderer.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <utility>

namespace eruption {

namespace {

// True if the world-space AABB is completely outside the camera frustum.
// Planes are extracted from the view-projection (GLM column-major,
// GLM_FORCE_DEPTH_ZERO_TO_ONE: near clip is z >= 0 -> plane is row2).
// Catches the "cloud behind the camera" case the scissor code below
// cannot (it falls back to a full-screen draw there).
bool aabbOutsideFrustum(const Mat4& vp, const Vec3& bmin, const Vec3& bmax) {
    const Vec4 r0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const Vec4 r1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const Vec4 r2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const Vec4 r3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    const Vec4 planes[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
    for (const Vec4& pl : planes) {
        // p-vertex: the corner farthest along the plane normal.
        const Vec3 p(pl.x >= 0.0f ? bmax.x : bmin.x,
                     pl.y >= 0.0f ? bmax.y : bmin.y,
                     pl.z >= 0.0f ? bmax.z : bmin.z);
        if (pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < 0.0f) return true;
    }
    return false;
}

// Distance from a point to the nearest point of an AABB (0 when inside).
float aabbDistance(const Vec3& p, const Vec3& bmin, const Vec3& bmax) {
    const float dx = std::max({bmin.x - p.x, 0.0f, p.x - bmax.x});
    const float dy = std::max({bmin.y - p.y, 0.0f, p.y - bmax.y});
    const float dz = std::max({bmin.z - p.z, 0.0f, p.z - bmax.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

CloudLayerRenderer::~CloudLayerRenderer() {
    shutdown();
}

bool CloudLayerRenderer::init(VulkanContext* ctx, uint32_t width, uint32_t height, const Config& cfg) {
    shutdown();
    m_ctx = ctx;
    m_cfg = cfg;
    m_width = width;
    m_height = height;

    if (!m_ctx) {
        Logger::error("CloudLayerRenderer::init called without VulkanContext");
        return false;
    }

    if (!createTargets()) return false;
    if (!createShadowMap()) return false;
    if (!createQuad()) return false;
    if (!createBuffers()) return false;

    float layerSpacing = m_cfg.cloudThickness / glm::max(1u, m_cfg.layerCount - 1);
    if (m_cfg.debugPlaneY <= 0.0f) {
        m_cfg.debugPlaneY = m_cfg.cloudBottom + m_cfg.cloudThickness * 0.5f;
    }
    m_coverageArray.init(m_ctx, m_cfg.layerCount, m_cfg.coverageSize,
                         Vec3(-48000.0f, 0.0f, -48000.0f),
                         Vec3(48000.0f, 2000.0f, 48000.0f),
                         3, m_cfg.cloudAmount, 12345u,
                         m_cfg.cloudBottom, layerSpacing);

    if (!createDescriptors()) return false;
    if (!createDebugPlane()) return false;
    if (!createDebugPlanePipeline()) {
        ERUPTION_LOG_WARN("CloudLayerRenderer: debug plane pipeline failed; continuing without debug plane");
        // Non-fatal: cloud rendering still works, only the debug visualization is disabled.
    }
    if (!createBillboardPipeline()) {
        ERUPTION_LOG_WARN("CloudLayerRenderer: billboard pipeline failed; continuing without cloud billboards");
    }
    if (!createCloudShadowsPipeline()) {
        ERUPTION_LOG_WARN("CloudLayerRenderer: cloud shadows pipeline failed; continuing without cloud shadows");
    }
    if (!createPipelines()) return false;

    return true;
}

void CloudLayerRenderer::shutdown() {
    if (!m_ctx) return;

    // Independent cloud layer slots: free their UBOs/VBOs (descriptor sets die
    // with the pool in destroyDescriptors). Arrays are owned by the caller.
    for (auto& slot : m_cloudLayers) {
        for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
            if (slot.uboMapped[f]) {
                vmaUnmapMemory(m_ctx->allocator(), slot.uboAlloc[f]);
                slot.uboMapped[f] = nullptr;
            }
            if (slot.ubo[f] != VK_NULL_HANDLE) {
                vmaDestroyBuffer(m_ctx->allocator(), slot.ubo[f], slot.uboAlloc[f]);
            }
        }
        if (slot.planeVbo != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_ctx->allocator(), slot.planeVbo, slot.planeVboAlloc);
        }
        slot = CloudLayerSlot{};
    }
    m_cloudLayers.clear();

    destroyPipelines();
    destroyCloudShadowsPipeline();
    destroyCloudShadowsDepthDescriptor();
    destroyBillboardPipeline();
    destroyDebugPlanePipeline();
    destroyDescriptors();
    destroyBuffers();
    destroyDebugPlane();
    destroyQuad();
    destroyShadowMap();
    destroyTargets();

    m_coverageArray.shutdown();
    m_ctx = nullptr;
}

void CloudLayerRenderer::resize(uint32_t width, uint32_t height) {
    if (!m_ctx) return;
    VulkanContext* ctx = m_ctx;
    Config cfg = m_cfg;
    shutdown();
    init(ctx, width, height, cfg);
}

void CloudLayerRenderer::setConfig(const Config& cfg) {
    bool needsReinit = (cfg.layerCount != m_cfg.layerCount ||
                        cfg.coverageSize != m_cfg.coverageSize ||
                        cfg.shadowMapSize != m_cfg.shadowMapSize);
    m_cfg = cfg;
    if (needsReinit && m_ctx) {
        resize(m_width, m_height);
    }
}

void CloudLayerRenderer::setWeather(float cloudCoverage, float rainIntensity, float stormTint) {
    m_weatherCoverage = glm::clamp(cloudCoverage, 0.0f, 1.0f);
    m_rainIntensity   = glm::clamp(rainIntensity, 0.0f, 1.0f);
    m_stormTint       = glm::clamp(stormTint, 0.0f, 1.0f);
}

void CloudLayerRenderer::update(float dt) {
    if (!m_ctx || !m_cfg.enabled) return;
    m_coverageArray.update(dt, m_cfg.windDirection, m_cfg.windSpeed);
}

bool CloudLayerRenderer::createTargets() {
    // Half-resolution ray-marching balances quality and cost. The composite pass
    // upsamples with bilinear filtering and depth-aware blending.
    uint32_t w = m_width / 2;
    uint32_t h = m_height / 2;

    auto createColorImage = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view, VkFormat format, VkImageUsageFlags usage) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {w, h, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &img, &alloc, nullptr) != VK_SUCCESS) {
            Logger::error("CloudLayerRenderer: failed to create target image");
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = img;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
            Logger::error("CloudLayerRenderer: failed to create target view");
            return false;
        }
        return true;
    };

    if (!createColorImage(m_cloudColorImage, m_cloudColorAlloc, m_cloudColorView,
                          VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return false;

    if (!createColorImage(m_cloudDepthImage, m_cloudDepthAlloc, m_cloudDepthView,
                          VK_FORMAT_R32_SFLOAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return false;

    // Full-res god ray target.
    if (!createColorImage(m_godRayImage, m_godRayAlloc, m_godRayView,
                          VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return false;

    return true;
}

void CloudLayerRenderer::destroyTargets() {
    auto destroy = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_ctx->device(), view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (img != VK_NULL_HANDLE) {
            vmaDestroyImage(m_ctx->allocator(), img, alloc);
            img = VK_NULL_HANDLE;
            alloc = VK_NULL_HANDLE;
        }
    };
    destroy(m_cloudColorImage, m_cloudColorAlloc, m_cloudColorView);
    destroy(m_cloudDepthImage, m_cloudDepthAlloc, m_cloudDepthView);
    destroy(m_godRayImage, m_godRayAlloc, m_godRayView);
}

bool CloudLayerRenderer::createShadowMap() {
    if (!m_cfg.castShadows) return true;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16_SFLOAT;
    imageInfo.extent = {m_cfg.shadowMapSize, m_cfg.shadowMapSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_shadowMapImage, &m_shadowMapAlloc, nullptr) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create shadow map image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowMapImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_shadowMapView) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create shadow map view");
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_shadowMapSampler) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create shadow map sampler");
        return false;
    }

    return true;
}

void CloudLayerRenderer::destroyShadowMap() {
    if (m_shadowMapSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_shadowMapSampler, nullptr);
        m_shadowMapSampler = VK_NULL_HANDLE;
    }
    if (m_shadowMapView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_shadowMapView, nullptr);
        m_shadowMapView = VK_NULL_HANDLE;
    }
    if (m_shadowMapImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_shadowMapImage, m_shadowMapAlloc);
        m_shadowMapImage = VK_NULL_HANDLE;
        m_shadowMapAlloc = VK_NULL_HANDLE;
    }
}

bool CloudLayerRenderer::createQuad() {
    // Fullscreen triangle (no index buffer).
    float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         3.0f, -1.0f, 2.0f, 0.0f,
        -1.0f,  3.0f, 0.0f, 2.0f
    };

    VkDeviceSize size = sizeof(vertices);
    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, m_quadBuffer, m_quadAlloc)) {
        Logger::error("CloudLayerRenderer: failed to create quad buffer");
        return false;
    }

    void* mapped = nullptr;
    if (vmaMapMemory(m_ctx->allocator(), m_quadAlloc, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, vertices, size);
        vmaUnmapMemory(m_ctx->allocator(), m_quadAlloc);
    }
    return true;
}

void CloudLayerRenderer::destroyQuad() {
    if (m_quadBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_quadBuffer, m_quadAlloc);
        m_quadBuffer = VK_NULL_HANDLE;
        m_quadAlloc = VK_NULL_HANDLE;
    }
}

bool CloudLayerRenderer::createDebugPlane() {
    // Horizontal quad covering the cloud world bounds.
    float minX = -3000.0f, maxX = 3000.0f;
    float minZ = -3000.0f, maxZ = 3000.0f;
    float vertices[] = {
        minX, 0.0f, minZ, 0.0f, 0.0f,
        maxX, 0.0f, minZ, 1.0f, 0.0f,
        maxX, 0.0f, maxZ, 1.0f, 1.0f,
        minX, 0.0f, minZ, 0.0f, 0.0f,
        maxX, 0.0f, maxZ, 1.0f, 1.0f,
        minX, 0.0f, maxZ, 0.0f, 1.0f,
    };

    VkDeviceSize size = sizeof(vertices);
    if (!m_ctx->createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, m_debugPlaneBuffer, m_debugPlaneAlloc)) {
        Logger::error("CloudLayerRenderer: failed to create debug plane buffer");
        return false;
    }

    void* mapped = nullptr;
    if (vmaMapMemory(m_ctx->allocator(), m_debugPlaneAlloc, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, vertices, size);
        vmaUnmapMemory(m_ctx->allocator(), m_debugPlaneAlloc);
    }
    return true;
}

void CloudLayerRenderer::destroyDebugPlane() {
    if (m_debugPlaneBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_debugPlaneBuffer, m_debugPlaneAlloc);
        m_debugPlaneBuffer = VK_NULL_HANDLE;
        m_debugPlaneAlloc = VK_NULL_HANDLE;
    }
}

bool CloudLayerRenderer::createDebugPlanePipeline() {
    ERUPTION_LOG_DEBUG("CloudLayerRenderer: createDebugPlanePipeline start");
    auto vert = ShaderCompiler::loadSPIRV("clouds/coverage_plane.vert.spv");
    auto frag = ShaderCompiler::loadSPIRV("clouds/coverage_plane.frag.spv");
    if (vert.empty() || frag.empty()) {
        Logger::error("CloudLayerRenderer: failed to load debug plane shaders");
        return false;
    }
    ERUPTION_LOG_DEBUG("CloudLayerRenderer: debug plane shaders loaded (vert=%zu words, frag=%zu words)", vert.size(), frag.size());

    // Create the depth descriptor set layout so the coverage plane can discard
    // pixels where the scene is not sky (e.g. the gray void around the map).
    VkDescriptorSetLayoutBinding depthBinding{};
    depthBinding.binding = 0;
    depthBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    depthBinding.descriptorCount = 1;
    depthBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo depthLayoutInfo{};
    depthLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    depthLayoutInfo.bindingCount = 1;
    depthLayoutInfo.pBindings = &depthBinding;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &depthLayoutInfo, nullptr, &m_debugPlaneDepthLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create debug plane depth layout");
        return false;
    }

    // Push constants: layer, threshold, pomHeight, pomSteps, edgeSoftness,
    // rayMarchSteps, cloudThickness, pitchThreshold, spikeHeight, baseDepth, spikeWidth,
    // weatherCoverage, rainIntensity, stormTint, noRepeat, tintAltitude.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(float) * 16;

    VkDescriptorSetLayout layouts[3] = {m_descriptorSetLayout, m_compositeDescriptorSetLayout, m_debugPlaneDepthLayout};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_debugPlanePipelineLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create debug plane pipeline layout");
        return false;
    }
    ERUPTION_LOG_DEBUG("CloudLayerRenderer: debug plane pipeline layout created");

    VkShaderModuleCreateInfo vsInfo{};
    vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsInfo.codeSize = vert.size() * sizeof(uint32_t);
    vsInfo.pCode = vert.data();
    VkShaderModule vsModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &vsInfo, nullptr, &vsModule) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create debug plane vertex shader module");
        vkDestroyPipelineLayout(m_ctx->device(), m_debugPlanePipelineLayout, nullptr);
        m_debugPlanePipelineLayout = VK_NULL_HANDLE;
        return false;
    }

    VkShaderModuleCreateInfo fsInfo{};
    fsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsInfo.codeSize = frag.size() * sizeof(uint32_t);
    fsInfo.pCode = frag.data();
    VkShaderModule fsModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &fsInfo, nullptr, &fsModule) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create debug plane fragment shader module");
        vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
        vkDestroyPipelineLayout(m_ctx->device(), m_debugPlanePipelineLayout, nullptr);
        m_debugPlanePipelineLayout = VK_NULL_HANDLE;
        return false;
    }
    ERUPTION_LOG_DEBUG("CloudLayerRenderer: debug plane shader modules created");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 5 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = 3 * sizeof(float);
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)m_width, (float)m_height, 0, 1};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.pViewports = &vp;
    viewport.scissorCount = 1;
    viewport.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Cloud layer respects scene depth so terrain/objects in front occlude it.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &colorFormat;
    ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    // Scissor is set per draw: the global plane uses full-screen, independent
    // cloud layers scissor to their volume's screen projection (the ray-march
    // is expensive, so fragments outside the cloud must not be shaded).
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 1;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = &ri;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vi;
    pipeInfo.pInputAssemblyState = &ia;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &rs;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pDepthStencilState = &ds;
    pipeInfo.pColorBlendState = &cb;
    pipeInfo.pDynamicState = &dyn;
    pipeInfo.layout = m_debugPlanePipelineLayout;

    ERUPTION_LOG_DEBUG("CloudLayerRenderer: about to create debug plane graphics pipeline");
    VkResult result = vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &m_debugPlanePipeline);
    ERUPTION_LOG_DEBUG("CloudLayerRenderer: debug plane graphics pipeline result = %d", (int)result);

    vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);

    if (result != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create debug plane pipeline");
        return false;
    }

    if (!createDebugPlaneDepthDescriptor()) {
        Logger::error("CloudLayerRenderer: failed to create debug plane depth descriptor");
        return false;
    }

    return true;
}

void CloudLayerRenderer::destroyDebugPlanePipeline() {
    if (m_debugPlanePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_debugPlanePipeline, nullptr);
        m_debugPlanePipeline = VK_NULL_HANDLE;
    }
    if (m_debugPlanePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_debugPlanePipelineLayout, nullptr);
        m_debugPlanePipelineLayout = VK_NULL_HANDLE;
    }
    destroyDebugPlaneDepthDescriptor();
}

bool CloudLayerRenderer::createBillboardPipeline() {
    // Volumetric puffs: own pipeline layout (same 3 descriptor set layouts as
    // the debug plane, but a 104-byte push block for vertex+fragment carrying
    // the per-cloud box and density params). Requires createDebugPlanePipeline
    // first (set layouts + depth descriptor set).
    if (m_debugPlanePipelineLayout == VK_NULL_HANDLE) return false;

    auto vert = ShaderCompiler::loadSPIRV("clouds/cloud_billboard.vert.spv");
    auto frag = ShaderCompiler::loadSPIRV("clouds/cloud_billboard.frag.spv");
    if (vert.empty() || frag.empty()) {
        Logger::error("CloudLayerRenderer: failed to load billboard shaders");
        return false;
    }

    VkShaderModuleCreateInfo vsInfo{};
    vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsInfo.codeSize = vert.size() * sizeof(uint32_t);
    vsInfo.pCode = vert.data();
    VkShaderModule vsModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &vsInfo, nullptr, &vsModule) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create billboard vertex shader module");
        return false;
    }
    VkShaderModuleCreateInfo fsInfo{};
    fsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsInfo.codeSize = frag.size() * sizeof(uint32_t);
    fsInfo.pCode = frag.data();
    VkShaderModule fsModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &fsInfo, nullptr, &fsModule) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create billboard fragment shader module");
        vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsModule;
    stages[1].pName = "main";

    // Unit cube positions only; the box transform arrives via push constants.
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 3 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{};
    attr.binding = 0;
    attr.location = 0;
    attr.format = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset = 0;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)m_width, (float)m_height, 0, 1};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.pViewports = &vp;
    viewport.scissorCount = 1;
    viewport.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // Cull exactly one side: the ray-box intersection in the fragment works
    // off any box face (near or far), and culling avoids shading the volume
    // twice per pixel. The engine flips NDC Y (Camera proj[1][1] *= -1), which
    // inverts framebuffer winding: with CCW front faces the cube rasterizes
    // the NEAR faces from outside and CULLS EVERYTHING when the camera enters
    // the box (the zoom/pitch pop). CLOCKWISE + cull FRONT keeps the FAR
    // faces from outside and keeps the box visible from inside.
    rs.cullMode = VK_CULL_MODE_FRONT_BIT;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // NO hardware depth test: the drawn face is the box's BACK face, which
    // sits behind the cloud body — distant horizon terrain would win the test
    // and kill pixels whose cloud mass is actually in front of it (clouds cut
    // at the horizon line / popping as the terrain edge sweeps the box).
    // Occlusion is handled analytically in the fragment by clamping the march
    // at the reconstructed scene depth instead.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

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

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &colorFormat;
    ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    // Scissor is set per draw: each cloud clips to its box projection.
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 1;
    dyn.pDynamicStates = dynStates;

    // Set-3 layout + pool: per-layer local-cloud SSBO read by the puff
    // fragment shader (analytic 3D density).
    VkDescriptorSetLayoutBinding ssboBinding{};
    ssboBinding.binding = 0;
    ssboBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssboBinding.descriptorCount = 1;
    ssboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ssboLayoutInfo{};
    ssboLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ssboLayoutInfo.bindingCount = 1;
    ssboLayoutInfo.pBindings = &ssboBinding;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &ssboLayoutInfo, nullptr, &m_billboardCloudsLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create billboard clouds layout");
        vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
        vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);
        return false;
    }
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = MAX_CLOUD_LAYERS;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // unregisterCloudLayer() chama vkFreeDescriptorSets neste pool, e isso SO'
    // e' legal com esta flag (VUID-vkFreeDescriptorSets-descriptorPool-00312).
    // Sem ela nao e' so' barulho de validacao: os sets nao voltam para o pool,
    // e como nuvem de campo e' registrada e removida a cada troca de clima, o
    // pool (maxSets = MAX_CLOUD_LAYERS) se esgota e a alocacao passa a falhar.
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = MAX_CLOUD_LAYERS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_billboardCloudsPool) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create billboard clouds pool");
        vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
        vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);
        return false;
    }

    // Push block: boxMin/boxMax (vec4), seed, rainIntensity, stormTint,
    // tintAltitude, blobCount, pad x3. GLSL block = 64 bytes; the range covers
    // the C++ mirror as well.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = 112;

    VkDescriptorSetLayout layouts[4] = {m_descriptorSetLayout, m_compositeDescriptorSetLayout,
                                        m_debugPlaneDepthLayout, m_billboardCloudsLayout};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 4;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_billboardPipelineLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create billboard pipeline layout");
        vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
        vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = &ri;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vi;
    pipeInfo.pInputAssemblyState = &ia;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &rs;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pDepthStencilState = &ds;
    pipeInfo.pColorBlendState = &cb;
    pipeInfo.pDynamicState = &dyn;
    pipeInfo.layout = m_billboardPipelineLayout;

    VkResult result = vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo,
                                                nullptr, &m_billboardPipeline);

    // Half-res accum variant: identical, but alpha accumulates "over"-style
    // (ONE / ONE_MINUS_SRC_ALPHA) so the offscreen holds the exact premultiplied
    // composite of every cloud, and the viewport is dynamic (half-res target).
    // No depth attachment in the accum pass (occlusion is analytic in-shader),
    // so the variant drops the depth format from the rendering create info.
    VkPipelineRenderingCreateInfo riAccum = ri;
    riAccum.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    pipeInfo.pNext = &riAccum;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkDynamicState dynStatesAccum[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynAccum{};
    dynAccum.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynAccum.dynamicStateCount = 2;
    dynAccum.pDynamicStates = dynStatesAccum;
    pipeInfo.pColorBlendState = &cb;
    pipeInfo.pDynamicState = &dynAccum;
    VkResult resultAccum = vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo,
                                                     nullptr, &m_billboardAccumPipeline);

    vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);
    if (result != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create billboard pipeline");
        return false;
    }
    if (resultAccum != VK_SUCCESS) {
        // Half-res volume accumulation is optional: fall back to direct draws.
        Logger::error("CloudLayerRenderer: failed to create billboard accum pipeline (half-res volumes disabled)");
        m_billboardAccumPipeline = VK_NULL_HANDLE;
    }

    // Static unit cube [0,1]^3, outward-facing triangles (36 verts, vec3).
    static const float cube[] = {
        // +X
        1,0,0, 1,1,0, 1,1,1,  1,0,0, 1,1,1, 1,0,1,
        // -X
        0,0,1, 0,1,1, 0,1,0,  0,0,1, 0,1,0, 0,0,0,
        // +Y
        0,1,0, 0,1,1, 1,1,1,  0,1,0, 1,1,1, 1,1,0,
        // -Y
        0,0,0, 1,0,0, 1,0,1,  0,0,0, 1,0,1, 0,0,1,
        // +Z
        0,0,1, 1,0,1, 1,1,1,  0,0,1, 1,1,1, 0,1,1,
        // -Z
        0,0,0, 0,1,0, 1,1,0,  0,0,0, 1,1,0, 1,0,0,
    };
    if (!m_ctx->createBuffer(sizeof(cube), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, m_billboardCubeVbo, m_billboardCubeVboAlloc)) {
        Logger::error("CloudLayerRenderer: failed to create billboard cube VBO");
        return false;
    }
    void* mapped = nullptr;
    if (vmaMapMemory(m_ctx->allocator(), m_billboardCubeVboAlloc, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, cube, sizeof(cube));
        vmaUnmapMemory(m_ctx->allocator(), m_billboardCubeVboAlloc);
    } else {
        Logger::error("CloudLayerRenderer: failed to map billboard cube VBO");
        return false;
    }
    if (!createVolumeApplyPipeline()) return false;
    // Fumaça: mesma família de passe (cubo + ray-march), pipeline própria.
    // Falha aqui não é fatal: o mapa só fica sem fumaça.
    if (!createSmokePipeline()) {
        Logger::error("CloudLayerRenderer: smoke plume pipeline unavailable (map smoke disabled)");
    }
    return true;
}

// Coluna de fumaça: reusa m_billboardPipelineLayout (mesmos 4 set layouts,
// mesmo push range de 112 bytes) e o mesmo VBO de cubo unitário. Só o par de
// shaders muda. Blend premultiplicado correto (ONE / 1-SRC_ALPHA): o shader
// já emite cor premultiplicada.
bool CloudLayerRenderer::createSmokePipeline() {
    if (m_billboardPipelineLayout == VK_NULL_HANDLE) return false;

    auto vert = ShaderCompiler::loadSPIRV("clouds/smoke_plume.vert.spv");
    auto frag = ShaderCompiler::loadSPIRV("clouds/smoke_plume.frag.spv");
    if (vert.empty() || frag.empty()) {
        Logger::error("CloudLayerRenderer: failed to load smoke plume shaders");
        return false;
    }

    VkShaderModuleCreateInfo vsInfo{};
    vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsInfo.codeSize = vert.size() * sizeof(uint32_t);
    vsInfo.pCode = vert.data();
    VkShaderModule vsModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &vsInfo, nullptr, &vsModule) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo fsInfo{};
    fsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsInfo.codeSize = frag.size() * sizeof(uint32_t);
    fsInfo.pCode = frag.data();
    VkShaderModule fsModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &fsInfo, nullptr, &fsModule) != VK_SUCCESS) {
        vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 3 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{};
    attr.binding = 0;
    attr.location = 0;
    attr.format = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset = 0;
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)m_width, (float)m_height, 0, 1};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.pViewports = &vp;
    viewport.scissorCount = 1;
    viewport.pScissors = &scissor;

    // Mesma regra do puff: face TRASEIRA da caixa (CLOCKWISE + cull FRONT),
    // para a coluna continuar visível com a câmera dentro da caixa.
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_FRONT_BIT;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Sem teste de profundidade por hardware: a oclusão é analítica no
    // fragment (clamp no depth reconstruído), igual ao puff.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // cor já premultiplicada
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &colorFormat;
    ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 1;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = &ri;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vi;
    pipeInfo.pInputAssemblyState = &ia;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &rs;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pDepthStencilState = &ds;
    pipeInfo.pColorBlendState = &cb;
    pipeInfo.pDynamicState = &dyn;
    pipeInfo.layout = m_billboardPipelineLayout;

    VkResult result = vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo,
                                                nullptr, &m_smokePipeline);
    vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);
    if (result != VK_SUCCESS) {
        m_smokePipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void CloudLayerRenderer::renderSmokePlume(VkCommandBuffer cmd, const SmokeEmitter& e,
                                          const Camera& camera, VkImageView depthView,
                                          const Vec2& windXZ, const SmokeQuality& quality) {
    if (!m_cfg.enabled || !m_ctx) return;
    if (m_smokePipeline == VK_NULL_HANDLE || m_billboardCubeVbo == VK_NULL_HANDLE) return;
    if (!e.enabled || e.density <= 0.0f) return;

    // AABB da pluma: a coluna nasce em `position`, sobe `height` e é
    // arrastada pelo vento durante a subida — a caixa precisa cobrir a
    // deriva total, senão o topo é cortado.
    const float topAge = e.height / std::max(e.rise, 0.01f);
    const Vec2 drift = windXZ * topAge;
    // Extensão radial máxima do shader: bojos empilhados (1 + 0.18 + 0.10) x
    // deslocamento máximo do ruído (1 + 0.5). Precisa bater com
    // smoke_plume.frag ou a caixa corta a pluma / infla sem motivo.
    constexpr float kSmokeRadialExtent = 1.28f * 1.5f;
    const float rBase = e.radius * kSmokeRadialExtent;
    const float rTop = e.radius * (1.0f + e.spread) * kSmokeRadialExtent;
    const float margin = 0.10f * e.radius + 2.0f; // meandro lateral (sway)
    const float minX = std::min(e.position.x - rBase, e.position.x + drift.x - rTop) - margin;
    const float maxX = std::max(e.position.x + rBase, e.position.x + drift.x + rTop) + margin;
    const float minZ = std::min(e.position.z - rBase, e.position.z + drift.y - rTop) - margin;
    const float maxZ = std::max(e.position.z + rBase, e.position.z + drift.y + rTop) + margin;
    const float yBot = e.position.y;
    const float yTop = e.position.y + e.height;

    const Mat4 vp = camera.viewProjectionMatrix();
    const Vec3 boxMin3(minX, yBot, minZ), boxMax3(maxX, yTop, maxZ);
    if (aabbOutsideFrustum(vp, boxMin3, boxMax3)) return;

    // Scissor na projeção da caixa: o ray-march não é de graça.
    {
        float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
        bool crossesNear = false;
        for (int i = 0; i < 8; ++i) {
            const float x = (i & 1) ? maxX : minX;
            const float y = (i & 2) ? yTop : yBot;
            const float z = (i & 4) ? maxZ : minZ;
            const Vec4 p = vp * Vec4(x, y, z, 1.0f);
            if (p.w <= 0.0f) { crossesNear = true; break; }
            sx0 = std::min(sx0, p.x / p.w); sx1 = std::max(sx1, p.x / p.w);
            sy0 = std::min(sy0, p.y / p.w); sy1 = std::max(sy1, p.y / p.w);
        }
        if (!crossesNear) {
            const float W = static_cast<float>(m_width), H = static_cast<float>(m_height);
            int ix0 = static_cast<int>(std::floor((sx0 * 0.5f + 0.5f) * W)) - 2;
            int iy0 = static_cast<int>(std::floor((sy0 * 0.5f + 0.5f) * H)) - 2;
            int ix1 = static_cast<int>(std::ceil((sx1 * 0.5f + 0.5f) * W)) + 2;
            int iy1 = static_cast<int>(std::ceil((sy1 * 0.5f + 0.5f) * H)) + 2;
            ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
            ix1 = std::min(ix1, static_cast<int>(m_width));
            iy1 = std::min(iy1, static_cast<int>(m_height));
            if (ix0 >= ix1 || iy0 >= iy1) return; // fora da tela
            VkRect2D tight{{ix0, iy0}, {static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0)}};
            vkCmdSetScissor(cmd, 0, 1, &tight);
        } else {
            VkRect2D full{{0, 0}, {m_width, m_height}};
            vkCmdSetScissor(cmd, 0, 1, &full);
        }
    }

    updateDebugPlaneDepthDescriptor(depthView);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smokePipeline);
    // O shader só usa set 0 (UBOs globais) e set 2 (profundidade da cena);
    // sets 1 e 3 do layout não são acessados, então não precisam ser ligados.
    VkDescriptorSet set0 = m_descriptorSets[m_ctx->currentFrame()];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_billboardPipelineLayout,
                            0, 1, &set0, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_billboardPipelineLayout,
                            2, 1, &m_debugPlaneDepthSet, 0, nullptr);
    VkBuffer vb = m_billboardCubeVbo;
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);

    // Distância -> LOD de passos: uma pluma longe não precisa de 32 passos.
    const Vec3 boxCenter3 = (boxMin3 + boxMax3) * 0.5f;
    const float boxRadius = glm::length(boxMax3 - boxMin3) * 0.5f;
    const float centerDist = glm::max(glm::distance(camera.position(), boxCenter3), 1.0f);
    const float angular = boxRadius / centerDist;
    const float lod = glm::mix(0.35f, 1.0f, glm::smoothstep(0.05f, 0.30f, angular));

    struct {
        glm::vec4 boxMin;
        glm::vec4 boxMax;
        glm::vec4 origin;
        glm::vec4 shape;
        glm::vec4 wind;
        glm::vec4 color;
        glm::vec4 misc;
    } pc;
    pc.boxMin = glm::vec4(boxMin3, 1.0f);
    pc.boxMax = glm::vec4(boxMax3, 1.0f);
    pc.origin = glm::vec4(e.position, e.radius);
    pc.shape = glm::vec4(e.height, e.spread, e.rise, e.density);
    pc.wind = glm::vec4(windXZ.x, windXZ.y, e.turbulence, std::max(e.rate, 0.0f));
    pc.color = glm::vec4(e.color, e.glow);
    // seed estável por emissor (posição): duas bocas não pulsam em fase.
    const float seed = std::fmod(std::abs(e.position.x) * 7.3f + std::abs(e.position.z) * 13.1f, 1024.0f);
    static const bool kNoDepthClip = std::getenv("ERUPTION_TEST_SMOKE_NODEPCLIP") != nullptr;
    int flags = 0;
    if (quality.sunTap) flags |= 1;
    if (kNoDepthClip) flags |= 2;
    if (quality.detail) flags |= 4;
    if (e.fire > 0.5f) flags |= 8;
    pc.misc = glm::vec4(seed,
                        std::max(8.0f, static_cast<float>(quality.maxSteps) * lod),
                        1.0f,
                        static_cast<float>(flags));

    vkCmdPushConstants(cmd, m_billboardPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 36, 1, 0, 0);
}

void CloudLayerRenderer::destroyBillboardPipeline() {
    destroyVolumeApplyPipeline();
    if (m_smokePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_smokePipeline, nullptr);
        m_smokePipeline = VK_NULL_HANDLE;
    }
    destroyVolumeAccumTarget();
    if (m_billboardAccumPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_billboardAccumPipeline, nullptr);
        m_billboardAccumPipeline = VK_NULL_HANDLE;
    }
    if (m_billboardCubeVbo != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_billboardCubeVbo, m_billboardCubeVboAlloc);
        m_billboardCubeVbo = VK_NULL_HANDLE;
        m_billboardCubeVboAlloc = VK_NULL_HANDLE;
    }
    if (m_billboardPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_billboardPipeline, nullptr);
        m_billboardPipeline = VK_NULL_HANDLE;
    }
    if (m_billboardPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_billboardPipelineLayout, nullptr);
        m_billboardPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_billboardCloudsPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_billboardCloudsPool, nullptr);
        m_billboardCloudsPool = VK_NULL_HANDLE;
    }
    if (m_billboardCloudsLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_billboardCloudsLayout, nullptr);
        m_billboardCloudsLayout = VK_NULL_HANDLE;
    }
}

bool CloudLayerRenderer::createDebugPlaneDepthDescriptor() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_debugPlaneDepthPool) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create debug plane depth pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_debugPlaneDepthPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_debugPlaneDepthLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_debugPlaneDepthSet) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to allocate debug plane depth set");
        return false;
    }
    return true;
}

void CloudLayerRenderer::destroyDebugPlaneDepthDescriptor() {
    if (m_debugPlaneDepthPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_debugPlaneDepthPool, nullptr);
        m_debugPlaneDepthPool = VK_NULL_HANDLE;
    }
    if (m_debugPlaneDepthLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_debugPlaneDepthLayout, nullptr);
        m_debugPlaneDepthLayout = VK_NULL_HANDLE;
    }
}

void CloudLayerRenderer::updateDebugPlaneDepthDescriptor(VkImageView depthView) {
    if (m_debugPlaneDepthSet == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE) return;
    if (depthView == m_debugPlaneDepthViewCached) return;
    m_debugPlaneDepthViewCached = depthView;

    VkDescriptorImageInfo depthInfo{};
    // O depth do G-buffer esta' em DEPTH_READ_ONLY_OPTIMAL quando os planos de
    // nuvem desenham (Engine faz a barreira antes, porque o mesmo depth e' o
    // attachment read-only do passe). Declarar SHADER_READ_ONLY aqui gerava
    // VUID-VkDescriptorImageInfo-imageLayout-00344 em CADA draw de plano -
    // 90 mil erros por sessao no zoom 0 do parana (40 nuvens de campo).
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depthInfo.imageView = depthView;
    depthInfo.sampler = m_linearSampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_debugPlaneDepthSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &depthInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
}

bool CloudLayerRenderer::createCloudShadowsPipeline() {
    ERUPTION_LOG_DEBUG("CloudLayerRenderer: createCloudShadowsPipeline start");
    auto vert = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");
    auto frag = ShaderCompiler::loadSPIRV("clouds/cloud_shadows.frag.spv");
    if (vert.empty() || frag.empty()) {
        Logger::error("CloudLayerRenderer: failed to load cloud shadows shaders");
        return false;
    }

    // Create the depth descriptor set layout.
    VkDescriptorSetLayoutBinding depthBinding{};
    depthBinding.binding = 0;
    depthBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    depthBinding.descriptorCount = 1;
    depthBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo depthLayoutInfo{};
    depthLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    depthLayoutInfo.bindingCount = 1;
    depthLayoutInfo.pBindings = &depthBinding;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &depthLayoutInfo, nullptr, &m_cloudShadowsDepthLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadows depth layout");
        return false;
    }

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(float) * 6;

    VkDescriptorSetLayout layouts[3] = {m_descriptorSetLayout, m_compositeDescriptorSetLayout, m_cloudShadowsDepthLayout};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_cloudShadowsPipelineLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadows pipeline layout");
        return false;
    }

    VkShaderModuleCreateInfo vsInfo{};
    vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsInfo.codeSize = vert.size() * sizeof(uint32_t);
    vsInfo.pCode = vert.data();
    VkShaderModule vsModule = VK_NULL_HANDLE;
    vkCreateShaderModule(m_ctx->device(), &vsInfo, nullptr, &vsModule);

    VkShaderModuleCreateInfo fsInfo{};
    fsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsInfo.codeSize = frag.size() * sizeof(uint32_t);
    fsInfo.pCode = frag.data();
    VkShaderModule fsModule = VK_NULL_HANDLE;
    vkCreateShaderModule(m_ctx->device(), &fsInfo, nullptr, &fsModule);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)m_width, (float)m_height, 0, 1};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.pViewports = &vp;
    viewport.scissorCount = 1;
    viewport.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Fullscreen overlay: no depth test, alpha blend over the lit scene.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &colorFormat;

    // Viewport and scissor are set per draw (independent cloud layers scissor
    // their ground-shadow region instead of shading the whole screen).
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    auto buildPipeline = [&](const VkPipelineColorBlendAttachmentState& blend) -> VkPipeline {
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;

        VkGraphicsPipelineCreateInfo pipeInfo{};
        pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeInfo.pNext = &ri;
        pipeInfo.stageCount = 2;
        pipeInfo.pStages = stages;
        pipeInfo.pVertexInputState = &vi;
        pipeInfo.pInputAssemblyState = &ia;
        pipeInfo.pViewportState = &viewport;
        pipeInfo.pRasterizationState = &rs;
        pipeInfo.pMultisampleState = &ms;
        pipeInfo.pDepthStencilState = &ds;
        pipeInfo.pColorBlendState = &cb;
        pipeInfo.pDynamicState = &dyn;
        pipeInfo.layout = m_cloudShadowsPipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &pipeline) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return pipeline;
    };

    // Fullscreen overlay: no depth test, alpha blend over the lit scene.
    // Direct variant: blends black over the lit scene, dst alpha overwritten.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    m_cloudShadowsPipeline = buildPipeline(blend);

    // Accum variant: same color blend (dst rgb stays 0, unused) but alpha
    // accumulates the occlusion "over" style: A = a + A * (1 - a), so after
    // N clouds A = 1 - prod(1 - a_i) — the exact same darkening the direct
    // passes applied to the scene color.
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_cloudShadowAccumPipeline = buildPipeline(blend);

    vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);

    if (m_cloudShadowsPipeline == VK_NULL_HANDLE || m_cloudShadowAccumPipeline == VK_NULL_HANDLE) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadows pipeline");
        return false;
    }

    if (!createCloudShadowsDepthDescriptor()) return false;
    if (!createCloudShadowApplyPipeline()) return false;
    return true;
}

void CloudLayerRenderer::destroyCloudShadowsPipeline() {
    if (m_cloudShadowsPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_cloudShadowsPipeline, nullptr);
        m_cloudShadowsPipeline = VK_NULL_HANDLE;
    }
    if (m_cloudShadowAccumPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_cloudShadowAccumPipeline, nullptr);
        m_cloudShadowAccumPipeline = VK_NULL_HANDLE;
    }
    if (m_cloudShadowsPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_cloudShadowsPipelineLayout, nullptr);
        m_cloudShadowsPipelineLayout = VK_NULL_HANDLE;
    }
    destroyCloudShadowApplyPipeline();
    destroyShadowAccumTarget();
}

bool CloudLayerRenderer::createCloudShadowApplyPipeline() {
    auto vert = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");
    auto frag = ShaderCompiler::loadSPIRV("clouds/cloud_shadow_apply.frag.spv");
    if (vert.empty() || frag.empty()) {
        Logger::error("CloudLayerRenderer: failed to load cloud shadow apply shaders");
        return false;
    }

    VkDescriptorSetLayoutBinding accumBinding{};
    accumBinding.binding = 0;
    accumBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    accumBinding.descriptorCount = 1;
    accumBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &accumBinding;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &setLayoutInfo, nullptr, &m_cloudShadowApplyLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadow apply layout");
        return false;
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_cloudShadowApplyLayout;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_cloudShadowApplyPipelineLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadow apply pipeline layout");
        return false;
    }

    VkShaderModuleCreateInfo vsInfo{};
    vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsInfo.codeSize = vert.size() * sizeof(uint32_t);
    vsInfo.pCode = vert.data();
    VkShaderModule vsModule = VK_NULL_HANDLE;
    vkCreateShaderModule(m_ctx->device(), &vsInfo, nullptr, &vsModule);

    VkShaderModuleCreateInfo fsInfo{};
    fsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsInfo.codeSize = frag.size() * sizeof(uint32_t);
    fsInfo.pCode = frag.data();
    VkShaderModule fsModule = VK_NULL_HANDLE;
    vkCreateShaderModule(m_ctx->device(), &fsInfo, nullptr, &fsModule);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Same blend as the direct shadow passes: black over the scene.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &colorFormat;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = &ri;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vi;
    pipeInfo.pInputAssemblyState = &ia;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &rs;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pDepthStencilState = &ds;
    pipeInfo.pColorBlendState = &cb;
    pipeInfo.pDynamicState = &dyn;
    pipeInfo.layout = m_cloudShadowApplyPipelineLayout;

    VkResult result = vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &m_cloudShadowApplyPipeline);

    vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);

    if (result != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadow apply pipeline");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_cloudShadowApplyPool) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadow apply pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_cloudShadowApplyPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_cloudShadowApplyLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_cloudShadowApplySet) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to allocate cloud shadow apply set");
        return false;
    }
    return true;
}

void CloudLayerRenderer::destroyCloudShadowApplyPipeline() {
    if (m_cloudShadowApplyPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_cloudShadowApplyPipeline, nullptr);
        m_cloudShadowApplyPipeline = VK_NULL_HANDLE;
    }
    if (m_cloudShadowApplyPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_cloudShadowApplyPipelineLayout, nullptr);
        m_cloudShadowApplyPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_cloudShadowApplyPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_cloudShadowApplyPool, nullptr);
        m_cloudShadowApplyPool = VK_NULL_HANDLE;
        m_cloudShadowApplySet = VK_NULL_HANDLE;
    }
    if (m_cloudShadowApplyLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_cloudShadowApplyLayout, nullptr);
        m_cloudShadowApplyLayout = VK_NULL_HANDLE;
    }
}

bool CloudLayerRenderer::createShadowAccumTarget(uint32_t width, uint32_t height) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_shadowAccumImage, &m_shadowAccumAlloc, nullptr) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create shadow accum image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowAccumImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_shadowAccumView) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create shadow accum view");
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_shadowAccumSampler) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create shadow accum sampler");
        return false;
    }

    m_shadowAccumWidth = width;
    m_shadowAccumHeight = height;
    m_shadowAccumFirstUse = true;

    VkDescriptorImageInfo accumInfo{};
    accumInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    accumInfo.imageView = m_shadowAccumView;
    accumInfo.sampler = m_shadowAccumSampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_cloudShadowApplySet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &accumInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    return true;
}

void CloudLayerRenderer::destroyShadowAccumTarget() {
    if (m_shadowAccumSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_shadowAccumSampler, nullptr);
        m_shadowAccumSampler = VK_NULL_HANDLE;
    }
    if (m_shadowAccumView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_shadowAccumView, nullptr);
        m_shadowAccumView = VK_NULL_HANDLE;
    }
    if (m_shadowAccumImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_shadowAccumImage, m_shadowAccumAlloc);
        m_shadowAccumImage = VK_NULL_HANDLE;
        m_shadowAccumAlloc = VK_NULL_HANDLE;
    }
    m_shadowAccumWidth = 0;
    m_shadowAccumHeight = 0;
    m_shadowAccumFirstUse = true;
    m_shadowAccumMode = false;
}

bool CloudLayerRenderer::beginCloudShadowAccum(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!m_cfg.enabled || !m_cfg.showCloudShadows || !m_ctx) return false;
    if (m_cloudShadowAccumPipeline == VK_NULL_HANDLE || m_cloudShadowApplyPipeline == VK_NULL_HANDLE) return false;
    if (width == 0 || height == 0) return false;
    if (m_shadowAccumView == VK_NULL_HANDLE || m_shadowAccumWidth != width || m_shadowAccumHeight != height) {
        destroyShadowAccumTarget();
        if (!createShadowAccumTarget(width, height)) return false;
    }

    m_ctx->cmdImageBarrier(cmd, m_shadowAccumImage,
                           m_shadowAccumFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_shadowAccumFirstUse = false;

    VkRenderingAttachmentInfo accumAttach{};
    accumAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    accumAttach.imageView = m_shadowAccumView;
    accumAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    accumAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    accumAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    accumAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingInfo accumInfo{};
    accumInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    accumInfo.renderArea = {{0, 0}, {width, height}};
    accumInfo.layerCount = 1;
    accumInfo.colorAttachmentCount = 1;
    accumInfo.pColorAttachments = &accumAttach;

    vkCmdBeginRendering(cmd, &accumInfo);
    m_shadowAccumMode = true;
    return true;
}

void CloudLayerRenderer::endCloudShadowAccumApply(VkCommandBuffer cmd, VkImageView litView,
                                                  uint32_t width, uint32_t height) {
    if (!m_shadowAccumMode) return;
    vkCmdEndRendering(cmd);

    m_ctx->cmdImageBarrier(cmd, m_shadowAccumImage,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkRenderingAttachmentInfo litAttach{};
    litAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    litAttach.imageView = litView;
    litAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    litAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    litAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo litInfo{};
    litInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    litInfo.renderArea = {{0, 0}, {width, height}};
    litInfo.layerCount = 1;
    litInfo.colorAttachmentCount = 1;
    litInfo.pColorAttachments = &litAttach;

    vkCmdBeginRendering(cmd, &litInfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudShadowApplyPipeline);
    VkViewport vp{0, 0, (float)width, (float)height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudShadowApplyPipelineLayout,
                          0, 1, &m_cloudShadowApplySet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    m_shadowAccumMode = false;
}

bool CloudLayerRenderer::createVolumeApplyPipeline() {
    auto vert = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");
    auto frag = ShaderCompiler::loadSPIRV("clouds/cloud_volume_apply.frag.spv");
    if (vert.empty() || frag.empty()) {
        Logger::error("CloudLayerRenderer: failed to load cloud volume apply shaders");
        return false;
    }

    VkDescriptorSetLayoutBinding accumBinding{};
    accumBinding.binding = 0;
    accumBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    accumBinding.descriptorCount = 1;
    accumBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &accumBinding;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &setLayoutInfo, nullptr, &m_volumeApplyLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud volume apply layout");
        return false;
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_volumeApplyLayout;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_volumeApplyPipelineLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud volume apply pipeline layout");
        return false;
    }

    VkShaderModuleCreateInfo vsInfo{};
    vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsInfo.codeSize = vert.size() * sizeof(uint32_t);
    vsInfo.pCode = vert.data();
    VkShaderModule vsModule = VK_NULL_HANDLE;
    vkCreateShaderModule(m_ctx->device(), &vsInfo, nullptr, &vsModule);

    VkShaderModuleCreateInfo fsInfo{};
    fsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsInfo.codeSize = frag.size() * sizeof(uint32_t);
    fsInfo.pCode = frag.data();
    VkShaderModule fsModule = VK_NULL_HANDLE;
    vkCreateShaderModule(m_ctx->device(), &fsInfo, nullptr, &fsModule);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Premultiplied "over": the accum target already stores premultiplied rgb.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkPipelineRenderingCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &colorFormat;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = &ri;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vi;
    pipeInfo.pInputAssemblyState = &ia;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &rs;
    pipeInfo.pMultisampleState = &ms;
    pipeInfo.pDepthStencilState = &ds;
    pipeInfo.pColorBlendState = &cb;
    pipeInfo.pDynamicState = &dyn;
    pipeInfo.layout = m_volumeApplyPipelineLayout;

    VkResult result = vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &m_volumeApplyPipeline);

    vkDestroyShaderModule(m_ctx->device(), vsModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fsModule, nullptr);

    if (result != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud volume apply pipeline");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_volumeApplyPool) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud volume apply pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_volumeApplyPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_volumeApplyLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_volumeApplySet) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to allocate cloud volume apply set");
        return false;
    }
    return true;
}

void CloudLayerRenderer::destroyVolumeApplyPipeline() {
    if (m_volumeApplyPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_volumeApplyPipeline, nullptr);
        m_volumeApplyPipeline = VK_NULL_HANDLE;
    }
    if (m_volumeApplyPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_volumeApplyPipelineLayout, nullptr);
        m_volumeApplyPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_volumeApplyPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_volumeApplyPool, nullptr);
        m_volumeApplyPool = VK_NULL_HANDLE;
        m_volumeApplySet = VK_NULL_HANDLE;
    }
    if (m_volumeApplyLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_volumeApplyLayout, nullptr);
        m_volumeApplyLayout = VK_NULL_HANDLE;
    }
}

bool CloudLayerRenderer::createVolumeAccumTarget(uint32_t width, uint32_t height) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_volumeAccumImage, &m_volumeAccumAlloc, nullptr) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create volume accum image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_volumeAccumImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_volumeAccumView) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create volume accum view");
        return false;
    }

    m_volumeAccumWidth = width;
    m_volumeAccumHeight = height;
    m_volumeAccumFirstUse = true;

    VkDescriptorImageInfo accumInfo{};
    accumInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    accumInfo.imageView = m_volumeAccumView;
    accumInfo.sampler = m_linearSampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_volumeApplySet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &accumInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    return true;
}

void CloudLayerRenderer::destroyVolumeAccumTarget() {
    if (m_volumeAccumView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_volumeAccumView, nullptr);
        m_volumeAccumView = VK_NULL_HANDLE;
    }
    if (m_volumeAccumImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_volumeAccumImage, m_volumeAccumAlloc);
        m_volumeAccumImage = VK_NULL_HANDLE;
        m_volumeAccumAlloc = VK_NULL_HANDLE;
    }
    m_volumeAccumWidth = 0;
    m_volumeAccumHeight = 0;
    m_volumeAccumFirstUse = true;
    m_volumeAccumMode = false;
}

bool CloudLayerRenderer::beginCloudVolumeAccum(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!m_cfg.enabled || !m_ctx) return false;
    if (m_billboardAccumPipeline == VK_NULL_HANDLE || m_volumeApplyPipeline == VK_NULL_HANDLE) return false;
    if (width == 0 || height == 0) return false;
    if (m_volumeAccumView == VK_NULL_HANDLE || m_volumeAccumWidth != width || m_volumeAccumHeight != height) {
        destroyVolumeAccumTarget();
        if (!createVolumeAccumTarget(width, height)) return false;
    }

    m_ctx->cmdImageBarrier(cmd, m_volumeAccumImage,
                           m_volumeAccumFirstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_volumeAccumFirstUse = false;

    VkRenderingAttachmentInfo accumAttach{};
    accumAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    accumAttach.imageView = m_volumeAccumView;
    accumAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    accumAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    accumAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    accumAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingInfo accumInfo{};
    accumInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    accumInfo.renderArea = {{0, 0}, {width, height}};
    accumInfo.layerCount = 1;
    accumInfo.colorAttachmentCount = 1;
    accumInfo.pColorAttachments = &accumAttach;

    vkCmdBeginRendering(cmd, &accumInfo);
    m_volumeAccumMode = true;
    return true;
}

void CloudLayerRenderer::endCloudVolumeAccumApply(VkCommandBuffer cmd, VkImageView litView,
                                                  uint32_t width, uint32_t height) {
    if (!m_volumeAccumMode) return;
    vkCmdEndRendering(cmd);

    m_ctx->cmdImageBarrier(cmd, m_volumeAccumImage,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkRenderingAttachmentInfo litAttach{};
    litAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    litAttach.imageView = litView;
    litAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    litAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    litAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo litInfo{};
    litInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    litInfo.renderArea = {{0, 0}, {width, height}};
    litInfo.layerCount = 1;
    litInfo.colorAttachmentCount = 1;
    litInfo.pColorAttachments = &litAttach;

    vkCmdBeginRendering(cmd, &litInfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumeApplyPipeline);
    VkViewport vp{0, 0, (float)width, (float)height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumeApplyPipelineLayout,
                          0, 1, &m_volumeApplySet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    m_volumeAccumMode = false;
}

bool CloudLayerRenderer::createCloudShadowsDepthDescriptor() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_cloudShadowsDepthPool) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create cloud shadows depth pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_cloudShadowsDepthPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_cloudShadowsDepthLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_cloudShadowsDepthSet) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to allocate cloud shadows depth set");
        return false;
    }
    return true;
}

void CloudLayerRenderer::destroyCloudShadowsDepthDescriptor() {
    if (m_cloudShadowsDepthPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_cloudShadowsDepthPool, nullptr);
        m_cloudShadowsDepthPool = VK_NULL_HANDLE;
    }
    if (m_cloudShadowsDepthLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_cloudShadowsDepthLayout, nullptr);
        m_cloudShadowsDepthLayout = VK_NULL_HANDLE;
    }
}

void CloudLayerRenderer::updateCloudShadowsDepthDescriptor(VkImageView depthView) {
    if (m_cloudShadowsDepthSet == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE) return;
    if (depthView == m_cloudShadowsDepthViewCached) return;
    m_cloudShadowsDepthViewCached = depthView;

    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthInfo.imageView = depthView;
    depthInfo.sampler = m_linearSampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_cloudShadowsDepthSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &depthInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
}

bool CloudLayerRenderer::createBuffers() {
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        if (!m_ctx->createBuffer(sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_CPU_TO_GPU, m_uboBuffers[f], m_uboAllocs[f])) {
            Logger::error("CloudLayerRenderer: failed to create UBO");
            return false;
        }
        vmaMapMemory(m_ctx->allocator(), m_uboAllocs[f], &m_uboMapped[f]);

        if (!m_ctx->createBuffer(sizeof(CloudUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_CPU_TO_GPU, m_cloudUboBuffers[f], m_cloudUboAllocs[f])) {
            Logger::error("CloudLayerRenderer: failed to create cloud UBO");
            return false;
        }
        vmaMapMemory(m_ctx->allocator(), m_cloudUboAllocs[f], &m_cloudUboMapped[f]);
    }

    return true;
}

void CloudLayerRenderer::destroyBuffers() {
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        if (m_uboMapped[f]) {
            vmaUnmapMemory(m_ctx->allocator(), m_uboAllocs[f]);
            m_uboMapped[f] = nullptr;
        }
        if (m_uboBuffers[f] != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_ctx->allocator(), m_uboBuffers[f], m_uboAllocs[f]);
            m_uboBuffers[f] = VK_NULL_HANDLE;
            m_uboAllocs[f] = VK_NULL_HANDLE;
        }
        if (m_cloudUboMapped[f]) {
            vmaUnmapMemory(m_ctx->allocator(), m_cloudUboAllocs[f]);
            m_cloudUboMapped[f] = nullptr;
        }
        if (m_cloudUboBuffers[f] != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_ctx->allocator(), m_cloudUboBuffers[f], m_cloudUboAllocs[f]);
            m_cloudUboBuffers[f] = VK_NULL_HANDLE;
            m_cloudUboAllocs[f] = VK_NULL_HANDLE;
        }
    }
}

VkSampler CloudLayerRenderer::createLinearSampler(VulkanContext* ctx) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(ctx->device(), &info, nullptr, &sampler);
    return sampler;
}

bool CloudLayerRenderer::createDescriptors() {
    m_linearSampler = createLinearSampler(m_ctx);

    // Set 0: UBOs.
    VkDescriptorSetLayoutBinding uboBindings[2] = {};
    uboBindings[0].binding = 0;
    uboBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBindings[0].descriptorCount = 1;
    uboBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboBindings[1].binding = 1;
    uboBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBindings[1].descriptorCount = 1;
    uboBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo uboLayoutInfo{};
    uboLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    uboLayoutInfo.bindingCount = 2;
    uboLayoutInfo.pBindings = uboBindings;
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &uboLayoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create descriptor set layout");
        return false;
    }

    // Set 1: coverage array.
    VkDescriptorSetLayoutBinding coverageBindings[2] = {};
    coverageBindings[0].binding = 0;
    coverageBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    coverageBindings[0].descriptorCount = 1;
    coverageBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    coverageBindings[1].binding = 1;
    coverageBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    coverageBindings[1].descriptorCount = 1;
    coverageBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo coverageLayoutInfo{};
    coverageLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    coverageLayoutInfo.bindingCount = 2;
    coverageLayoutInfo.pBindings = coverageBindings;
    // Note: m_compositeDescriptorSetLayout is reused here for the coverage set.
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &coverageLayoutInfo, nullptr, &m_compositeDescriptorSetLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create coverage layout");
        return false;
    }

    // Pool sized for the global sets (3 per-frame UBO sets + 1 coverage) plus
    // MAX_CLOUD_LAYERS independent layers (3 per-frame UBO sets + 1 coverage
    // set each).
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8 + MAX_CLOUD_LAYERS * 6;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 4 + MAX_CLOUD_LAYERS * 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // Mesmo motivo do pool de billboards acima: unregisterCloudLayer() libera
    // os 4 sets de cada camada aqui, o que exige esta flag.
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 4 + MAX_CLOUD_LAYERS * 4;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create descriptor pool");
        return false;
    }

    VkDescriptorSetLayout uboLayouts[VulkanContext::MAX_FRAMES_IN_FLIGHT];
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) uboLayouts[f] = m_descriptorSetLayout;
    VkDescriptorSetAllocateInfo uboAllocInfo{};
    uboAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    uboAllocInfo.descriptorPool = m_descriptorPool;
    uboAllocInfo.descriptorSetCount = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    uboAllocInfo.pSetLayouts = uboLayouts;
    if (vkAllocateDescriptorSets(m_ctx->device(), &uboAllocInfo, m_descriptorSets) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to allocate per-frame descriptor sets");
        return false;
    }
    VkDescriptorSetAllocateInfo covAllocInfo{};
    covAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    covAllocInfo.descriptorPool = m_descriptorPool;
    covAllocInfo.descriptorSetCount = 1;
    covAllocInfo.pSetLayouts = &m_compositeDescriptorSetLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &covAllocInfo, &m_compositeDescriptorSet) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to allocate descriptor sets");
        return false;
    }

    // Update UBO descriptors (set 0), one per frame-in-flight buffer.
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_uboBuffers[f];
        uboInfo.offset = 0;
        uboInfo.range = sizeof(UBO);

        VkDescriptorBufferInfo cloudUboInfo{};
        cloudUboInfo.buffer = m_cloudUboBuffers[f];
        cloudUboInfo.offset = 0;
        cloudUboInfo.range = sizeof(CloudUBO);

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptorSets[f];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &uboInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descriptorSets[f];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &cloudUboInfo;

        vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);
    }

    // Update coverage descriptor (set 1).
    VkDescriptorImageInfo coverageInfo{};
    coverageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    coverageInfo.imageView = m_coverageArray.imageView();
    coverageInfo.sampler = m_coverageArray.sampler();

    VkDescriptorImageInfo altitudeInfo{};
    altitudeInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    altitudeInfo.imageView = m_coverageArray.altitudeView();
    altitudeInfo.sampler = m_coverageArray.altitudeSampler();

    VkWriteDescriptorSet covWrites[2] = {};
    covWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    covWrites[0].dstSet = m_compositeDescriptorSet; // set 1 in shader
    covWrites[0].dstBinding = 0;
    covWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    covWrites[0].descriptorCount = 1;
    covWrites[0].pImageInfo = &coverageInfo;

    covWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    covWrites[1].dstSet = m_compositeDescriptorSet;
    covWrites[1].dstBinding = 1;
    covWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    covWrites[1].descriptorCount = 1;
    covWrites[1].pImageInfo = &altitudeInfo;

    vkUpdateDescriptorSets(m_ctx->device(), 2, covWrites, 0, nullptr);

    return true;
}

void CloudLayerRenderer::destroyDescriptors() {
    if (m_linearSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_linearSampler, nullptr);
        m_linearSampler = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_compositeDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_compositeDescriptorSetLayout, nullptr);
        m_compositeDescriptorSetLayout = VK_NULL_HANDLE;
    }
}

bool CloudLayerRenderer::createPipelines() {
    auto vertCode = ShaderCompiler::loadSPIRV("clouds/layers_raymarch.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("clouds/layers_raymarch.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        Logger::error("CloudLayerRenderer: failed to load shaders");
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkShaderModule vertModule, fragModule;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
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
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = 2 * sizeof(float);
    vertInput.vertexBindingDescriptionCount = 1;
    vertInput.pVertexBindingDescriptions = &binding;
    vertInput.vertexAttributeDescriptionCount = 2;
    vertInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)m_width / 2.0f, (float)m_height / 2.0f, 0, 1};
    VkRect2D scissor{{0, 0}, {m_width / 2, m_height / 2}};
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

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend[2] = {};
    blend[0].blendEnable = VK_FALSE;
    blend[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend[1].blendEnable = VK_FALSE;
    blend[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 2;
    blendState.pAttachments = blend;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat colorFormats[2] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT};
    renderingInfo.colorAttachmentCount = 2;
    renderingInfo.pColorAttachmentFormats = colorFormats;

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = 128;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkDescriptorSetLayout layouts[2] = {m_descriptorSetLayout, m_compositeDescriptorSetLayout};
    plInfo.setLayoutCount = 2;
    plInfo.pSetLayouts = layouts;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create pipeline layout");
        return false;
    }

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
    pipeInfo.pDepthStencilState = &ds;
    pipeInfo.pColorBlendState = &blendState;
    pipeInfo.layout = m_pipelineLayout;

    if (vkCreateGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &m_raymarchPipeline) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to create raymarch pipeline");
        return false;
    }

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);

    // Composite pipeline (simplified: just blend cloud over scene).
    // We reuse a simple fullscreen fragment that blends cloudColor using cloudDepth.
    // For now the composite is done inside PostProcessor; this pipeline is a placeholder.
    m_compositePipeline = VK_NULL_HANDLE;

    return true;
}

void CloudLayerRenderer::destroyPipelines() {
    if (m_raymarchPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_raymarchPipeline, nullptr);
        m_raymarchPipeline = VK_NULL_HANDLE;
    }
    if (m_compositePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_compositePipeline, nullptr);
        m_compositePipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
}

void CloudLayerRenderer::updateUBOs(const Camera& camera, const Vec3& sunDir, float sunIntensity,
                                    float time, uint32_t frameIndex) {
    UBO ubo{};
    ubo.viewProj = camera.viewProjectionMatrix();
    ubo.invViewProj = glm::inverse(ubo.viewProj);
    ubo.cameraPos = camera.position();
    ubo.time = time;
    ubo.sunDir = sunDir;
    ubo.sunIntensity = sunIntensity;
    m_sceneSunIntensity = sunIntensity;
    ubo.screenSize = Vec2((float)m_width, (float)m_height);
    ubo.frameIndex = frameIndex;
    std::memcpy(m_uboMapped[m_ctx->currentFrame()], &ubo, sizeof(ubo));

    CloudUBO c{};
    c.worldMin = m_coverageArray.worldMin();
    c.worldMax = m_coverageArray.worldMax();
    c.cloudBottom = m_cfg.cloudBottom;
    c.cloudTop = m_cfg.cloudBottom + m_cfg.cloudThickness;
    c.layerSpacing = m_cfg.cloudThickness / glm::max(1u, m_cfg.layerCount - 1);
    c.layerCount = m_cfg.layerCount;
    c.noiseScale = m_cfg.noiseScale;
    c.detailStrength = m_cfg.detailStrength;
    c.windOffset = Vec3(m_coverageArray.windOffset().x, 0.0f, m_coverageArray.windOffset().y);
    c.extinctionScale = 0.25f;
    c.scatteringAlbedo = 0.50f;
    c.phaseG1 = 0.5f;
    c.phaseG2 = -0.25f;
    c.phaseAlpha = 0.8f;
    c.powderStrength = 0.4f;
    c.ambientIntensity = 0.08f;
    c.godRayMode = static_cast<uint32_t>(m_cfg.godRayMode);
    c.debugMode = m_cfg.debugMode;
    c.weatherCoverage = m_weatherCoverage;
    c.rainIntensity = m_rainIntensity;
    c.stormTint = m_stormTint;
    c.cloudCoverageThreshold = 0.0f; // threshold is now baked into the coverage texture on CPU
    c.cloudScale = m_cfg.cloudScale;
    c.cloudLightness = m_cfg.cloudLightness;
    c.cloudShade = m_cfg.cloudShade;
    c.cloudSoftness = m_cfg.cloudSoftness;
    c.marchStepBudget = static_cast<float>(m_marchStepBudget);
    std::memcpy(m_cloudUboMapped[m_ctx->currentFrame()], &c, sizeof(c));
}

void CloudLayerRenderer::render(VkCommandBuffer cmd,
                                const Camera& camera,
                                const Vec3& sunDir,
                                float sunIntensity,
                                float time,
                                uint32_t frameIndex) {
    if (!m_cfg.enabled || !m_ctx) return;

    updateUBOs(camera, sunDir, sunIntensity, time, frameIndex);

    // Transition half-res targets to COLOR_ATTACHMENT_OPTIMAL.
    m_ctx->cmdImageBarrier(cmd, m_cloudColorImage,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_ctx->cmdImageBarrier(cmd, m_cloudDepthImage,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkRenderingAttachmentInfo colorAttach[2] = {};
    colorAttach[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach[0].imageView = m_cloudColorView;
    colorAttach[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    colorAttach[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach[1].imageView = m_cloudDepthView;
    colorAttach[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {m_width / 2, m_height / 2}};
    info.layerCount = 1;
    info.colorAttachmentCount = 2;
    info.pColorAttachments = colorAttach;

    vkCmdBeginRendering(cmd, &info);

    VkViewport viewport{0, 0, (float)m_width / 2.0f, (float)m_height / 2.0f, 0, 1};
    VkRect2D scissor{{0, 0}, {m_width / 2, m_height / 2}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_raymarchPipeline);
    VkDescriptorSet sets[2] = {m_descriptorSets[m_ctx->currentFrame()], m_compositeDescriptorSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2, sets, 0, nullptr);

    float pcData[4] = {m_cfg.debugShowMissColor ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcData), pcData);

    VkBuffer vb = m_quadBuffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    // Transition to shader read.
    m_ctx->cmdImageBarrier(cmd, m_cloudColorImage,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_ctx->cmdImageBarrier(cmd, m_cloudDepthImage,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

void CloudLayerRenderer::renderDebugPlane(VkCommandBuffer cmd,
                                          const Camera& camera,
                                          VkImageView colorView,
                                          VkImageView depthView,
                                          uint32_t width,
                                          uint32_t height) {
    (void)colorView; (void)depthView;
    if (!m_cfg.enabled || !m_cfg.debugPlane || !m_ctx) return;

    // Use the configured debug plane altitude (defaults to the cloud mid-layer).
    float planeY = m_cfg.debugPlaneY;

    // Fit the debug plane to the camera frustum at the chosen altitude.
    Vec3 wmin = m_coverageArray.worldMin();
    Vec3 wmax = m_coverageArray.worldMax();
    float minX = wmin.x, maxX = wmax.x;
    float minZ = wmin.z, maxZ = wmax.z;

    Mat4 invVP = glm::inverse(camera.viewProjectionMatrix());

    Vec3 corners[8];
    int cidx = 0;
    for (int iz = 0; iz <= 1; ++iz) {
        for (int iy = -1; iy <= 1; iy += 2) {
            for (int ix = -1; ix <= 1; ix += 2) {
                Vec4 p = invVP * Vec4((float)ix, (float)iy, (float)iz, 1.0f);
                p /= p.w;
                corners[cidx++] = Vec3(p);
            }
        }
    }

    const int edges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };

    Vec3 intersectPoints[12];
    int intersectCount = 0;
    const float eps = 1e-4f;
    for (int e = 0; e < 12; ++e) {
        const Vec3& a = corners[edges[e][0]];
        const Vec3& b = corners[edges[e][1]];
        float da = a.y - planeY;
        float db = b.y - planeY;
        if (da * db < 0.0f) {
            float t = da / (da - db);
            Vec3 p = a + (b - a) * t;
            if (std::isfinite(p.x) && std::isfinite(p.z)) {
                intersectPoints[intersectCount++] = p;
            }
        } else if (std::abs(da) < eps) {
            intersectPoints[intersectCount++] = a;
        }
    }

    if (intersectCount > 0) {
        float fxmin = intersectPoints[0].x, fxmax = intersectPoints[0].x;
        float fzmin = intersectPoints[0].z, fzmax = intersectPoints[0].z;
        for (int i = 1; i < intersectCount; ++i) {
            fxmin = std::min(fxmin, intersectPoints[i].x);
            fxmax = std::max(fxmax, intersectPoints[i].x);
            fzmin = std::min(fzmin, intersectPoints[i].z);
            fzmax = std::max(fzmax, intersectPoints[i].z);
        }
        // No margin needed: pure raymarch relies entirely on the pixel frustum intersection.
        // Massive artificial margins blow up FP32 precision at grazing angles, causing
        // horrific jitter/snapping on the cloud vertices (e.g., around pitch ~25-30 deg).
        minX = fxmin; maxX = fxmax;
        minZ = fzmin; maxZ = fzmax;
    }

    float worldSizeX = wmax.x - wmin.x;
    float worldSizeZ = wmax.z - wmin.z;
    float invWorldX = (worldSizeX > 1e-5f) ? 1.0f / worldSizeX : 0.0f;
    float invWorldZ = (worldSizeZ > 1e-5f) ? 1.0f / worldSizeZ : 0.0f;
    Vec2 wind = m_coverageArray.windOffset();
    auto worldToUv = [&](float x, float z) -> std::pair<float, float> {
        float u = (x - wmin.x) * invWorldX - wind.x;
        float v = (z - wmin.z) * invWorldZ - wind.y;
        return {u, v};
    };
    auto uvMin = worldToUv(minX, minZ);
    auto uvMax = worldToUv(maxX, maxZ);

    float vertices[] = {
        minX, planeY, minZ, uvMin.first, uvMin.second,
        maxX, planeY, minZ, uvMax.first, uvMin.second,
        maxX, planeY, maxZ, uvMax.first, uvMax.second,
        minX, planeY, minZ, uvMin.first, uvMin.second,
        maxX, planeY, maxZ, uvMax.first, uvMax.second,
        minX, planeY, maxZ, uvMin.first, uvMax.second,
    };
    void* mapped = nullptr;
    if (vmaMapMemory(m_ctx->allocator(), m_debugPlaneAlloc, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, vertices, sizeof(vertices));
        vmaUnmapMemory(m_ctx->allocator(), m_debugPlaneAlloc);
    }

    updateDebugPlaneDepthDescriptor(depthView);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPlanePipeline);
    // The plane pipeline uses dynamic scissor (independent layers set a tight
    // rect); the global plane always covers the whole frame.
    VkRect2D fullScissor{{0, 0}, {width, height}};
    vkCmdSetScissor(cmd, 0, 1, &fullScissor);
    VkDescriptorSet sets[3] = {m_descriptorSets[m_ctx->currentFrame()], m_compositeDescriptorSet, m_debugPlaneDepthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPlanePipelineLayout, 0, 3, sets, 0, nullptr);

    float pcLayer = glm::clamp(m_cfg.layerCount * 0.5f, 0.0f, static_cast<float>(m_cfg.layerCount - 1));
    struct {
        float layer;
        float threshold;
        float edgeSoftness;
        float rayMarchSteps;
        float cloudThickness;
        float pitchThreshold;
        float spikeHeight;
        float baseDepth;
        float spikeWidth;
        float weatherCoverage;
        float rainIntensity;
        float stormTint;
        float noRepeat;
        float tintAltitude;
    } pc;
    pc.layer = pcLayer;
    pc.threshold = 0.0f; // threshold is now baked into the coverage texture on CPU
    pc.edgeSoftness = m_cfg.cloudEdgeSoftness;
    pc.rayMarchSteps = m_cfg.cloudRayMarchSteps;
    pc.cloudThickness = m_cfg.cloudDebugThickness;
    pc.spikeHeight = m_cfg.cloudSpikeHeight;
    pc.baseDepth = m_cfg.cloudBaseDepth;
    pc.spikeWidth = m_cfg.cloudSpikeWidth;
    pc.weatherCoverage = m_weatherCoverage;
    pc.rainIntensity = m_rainIntensity;
    pc.stormTint = m_stormTint;
    pc.noRepeat = 0.0f;       // global layer tiles
    pc.tintAltitude = -1.0f;  // tint disabled for the global layer
    vkCmdPushConstants(cmd, m_debugPlanePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    VkBuffer vb = m_debugPlaneBuffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

void CloudLayerRenderer::renderCloudShadows(VkCommandBuffer cmd,
                                            const Camera& camera,
                                            VkImageView colorView,
                                            VkImageView depthView,
                                            uint32_t width,
                                            uint32_t height) {
    (void)colorView; (void)camera;
    if (!m_cfg.enabled || !m_cfg.showCloudShadows || !m_ctx) return;
    if (m_cloudShadowsPipeline == VK_NULL_HANDLE) return;
    if (depthView == VK_NULL_HANDLE) return;

    updateCloudShadowsDepthDescriptor(depthView);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudShadowsPipeline);

    VkViewport vp{0, 0, (float)width, (float)height, 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDescriptorSet sets[3] = {m_descriptorSets[m_ctx->currentFrame()], m_compositeDescriptorSet, m_cloudShadowsDepthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudShadowsPipelineLayout, 0, 3, sets, 0, nullptr);

    float pcLayer = glm::clamp(m_cfg.layerCount * 0.5f, 0.0f, static_cast<float>(m_cfg.layerCount - 1));
    // Push layout: layer, shadowIntensity, coveragePlaneHeight, threshold, weatherCoverage, noRepeat.
    // Distance fade was removed; shadows now cover the whole world until the
    // cloud coverage texture bounds (sampler set to CLAMP_TO_BORDER black).
    float pcData[6] = {pcLayer, m_cfg.cloudShadowOpacity, m_cfg.cloudBottom, cloudShadowBaseStrength(), m_weatherCoverage, 0.0f};
    vkCmdPushConstants(cmd, m_cloudShadowsPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcData), pcData);

    // Fullscreen triangle, no vertex buffer.
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

uint32_t CloudLayerRenderer::registerCloudLayer(CloudCoverageArray* array) {
    if (!m_ctx || !array) return UINT32_MAX;

    uint32_t id = UINT32_MAX;
    for (uint32_t i = 0; i < m_cloudLayers.size(); ++i) {
        if (!m_cloudLayers[i].inUse) { id = i; break; }
    }
    if (id == UINT32_MAX) {
        if (m_cloudLayers.size() >= MAX_CLOUD_LAYERS) {
            Logger::error("CloudLayerRenderer: no free independent cloud layer slots");
            return UINT32_MAX;
        }
        m_cloudLayers.emplace_back();
        id = static_cast<uint32_t>(m_cloudLayers.size() - 1);
    }
    CloudLayerSlot& slot = m_cloudLayers[id];

    // Per-layer CloudUBOs (own world bounds / wind offset), one per
    // frame-in-flight.
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        if (!m_ctx->createBuffer(sizeof(CloudUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_CPU_TO_GPU, slot.ubo[f], slot.uboAlloc[f])) {
            Logger::error("CloudLayerRenderer: failed to create layer UBO");
            return UINT32_MAX;
        }
        vmaMapMemory(m_ctx->allocator(), slot.uboAlloc[f], &slot.uboMapped[f]);
    }

    // Per-layer vertex buffer: 12 vertices * 5 floats (fits both the 6-vertex
    // coverage plane and the 12-vertex cross-billboard).
    if (!m_ctx->createBuffer(60 * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, slot.planeVbo, slot.planeVboAlloc)) {
        Logger::error("CloudLayerRenderer: failed to create layer plane VBO");
        for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
            vmaUnmapMemory(m_ctx->allocator(), slot.uboAlloc[f]);
            vmaDestroyBuffer(m_ctx->allocator(), slot.ubo[f], slot.uboAlloc[f]);
            slot.ubo[f] = VK_NULL_HANDLE;
            slot.uboAlloc[f] = VK_NULL_HANDLE;
            slot.uboMapped[f] = nullptr;
        }
        return UINT32_MAX;
    }

    VkDescriptorSetLayout layouts[4] = {m_descriptorSetLayout, m_descriptorSetLayout,
                                        m_descriptorSetLayout, m_compositeDescriptorSetLayout};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 4;
    allocInfo.pSetLayouts = layouts;
    VkDescriptorSet sets[4] = {};
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, sets) != VK_SUCCESS) {
        Logger::error("CloudLayerRenderer: failed to allocate layer descriptor sets");
        for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
            vmaDestroyBuffer(m_ctx->allocator(), slot.ubo[f], slot.uboAlloc[f]);
            slot.ubo[f] = VK_NULL_HANDLE;
            slot.uboAlloc[f] = VK_NULL_HANDLE;
            slot.uboMapped[f] = nullptr;
        }
        return UINT32_MAX;
    }
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) slot.set0[f] = sets[f];
    slot.set1 = sets[3];

    // Set 0 (per frame): shared GlobalUBO (binding 0) + own CloudUBO (binding 1).
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_uboBuffers[f];
        uboInfo.offset = 0;
        uboInfo.range = sizeof(UBO);
        VkDescriptorBufferInfo cloudUboInfo{};
        cloudUboInfo.buffer = slot.ubo[f];
        cloudUboInfo.offset = 0;
        cloudUboInfo.range = sizeof(CloudUBO);
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = slot.set0[f];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &uboInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = slot.set0[f];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &cloudUboInfo;
        vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);
    }

    // Set 1: own coverage + altitude textures.
    VkDescriptorImageInfo coverageInfo{};
    coverageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    coverageInfo.imageView = array->imageView();
    coverageInfo.sampler = array->sampler();
    VkDescriptorImageInfo altitudeInfo{};
    altitudeInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    altitudeInfo.imageView = array->altitudeView();
    altitudeInfo.sampler = array->altitudeSampler();
    VkWriteDescriptorSet covWrites[2] = {};
    covWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    covWrites[0].dstSet = slot.set1;
    covWrites[0].dstBinding = 0;
    covWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    covWrites[0].descriptorCount = 1;
    covWrites[0].pImageInfo = &coverageInfo;
    covWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    covWrites[1].dstSet = slot.set1;
    covWrites[1].dstBinding = 1;
    covWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    covWrites[1].descriptorCount = 1;
    covWrites[1].pImageInfo = &altitudeInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 2, covWrites, 0, nullptr);

    // Set 3: own local-cloud SSBO for the volumetric puff shader. Optional:
    // if the billboard pipeline failed to initialize, the puff pass is skipped
    // and the flat deck still works.
    if (m_billboardCloudsPool != VK_NULL_HANDLE && array->localCloudBuffer() != VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ssboAlloc{};
        ssboAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ssboAlloc.descriptorPool = m_billboardCloudsPool;
        ssboAlloc.descriptorSetCount = 1;
        ssboAlloc.pSetLayouts = &m_billboardCloudsLayout;
        if (vkAllocateDescriptorSets(m_ctx->device(), &ssboAlloc, &slot.set3) == VK_SUCCESS) {
            VkDescriptorBufferInfo ssboInfo{};
            ssboInfo.buffer = array->localCloudBuffer();
            ssboInfo.offset = 0;
            ssboInfo.range = LocalCloud::MAX_COUNT * sizeof(LocalCloud);
            VkWriteDescriptorSet ssboWrite{};
            ssboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ssboWrite.dstSet = slot.set3;
            ssboWrite.dstBinding = 0;
            ssboWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ssboWrite.descriptorCount = 1;
            ssboWrite.pBufferInfo = &ssboInfo;
            vkUpdateDescriptorSets(m_ctx->device(), 1, &ssboWrite, 0, nullptr);
        } else {
            slot.set3 = VK_NULL_HANDLE;
        }
    }

    slot.array = array;
    slot.inUse = true;
    updateCloudLayerUBO(id);
    // Seed the other frame slots too: updateCloudLayerUBO only writes the
    // current frame, and the remaining in-flight frames read their own copies.
    const uint32_t cur = m_ctx->currentFrame();
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        if (f != cur && slot.uboMapped[f] && slot.uboMapped[cur])
            std::memcpy(slot.uboMapped[f], slot.uboMapped[cur], sizeof(CloudUBO));
    }
    return id;
}

void CloudLayerRenderer::unregisterCloudLayer(uint32_t id) {
    if (!m_ctx || id >= m_cloudLayers.size()) return;
    CloudLayerSlot& slot = m_cloudLayers[id];
    if (!slot.inUse) return;

    if (slot.set0[0] != VK_NULL_HANDLE || slot.set1 != VK_NULL_HANDLE) {
        VkDescriptorSet sets[4] = {slot.set0[0], slot.set0[1], slot.set0[2], slot.set1};
        vkFreeDescriptorSets(m_ctx->device(), m_descriptorPool, 4, sets);
        for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) slot.set0[f] = VK_NULL_HANDLE;
        slot.set1 = VK_NULL_HANDLE;
    }
    if (slot.set3 != VK_NULL_HANDLE && m_billboardCloudsPool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(m_ctx->device(), m_billboardCloudsPool, 1, &slot.set3);
        slot.set3 = VK_NULL_HANDLE;
    }
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        if (slot.uboMapped[f]) {
            vmaUnmapMemory(m_ctx->allocator(), slot.uboAlloc[f]);
            slot.uboMapped[f] = nullptr;
        }
        if (slot.ubo[f] != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_ctx->allocator(), slot.ubo[f], slot.uboAlloc[f]);
            slot.ubo[f] = VK_NULL_HANDLE;
            slot.uboAlloc[f] = VK_NULL_HANDLE;
        }
    }
    if (slot.planeVbo != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), slot.planeVbo, slot.planeVboAlloc);
        slot.planeVbo = VK_NULL_HANDLE;
        slot.planeVboAlloc = VK_NULL_HANDLE;
    }
    slot.array = nullptr;
    slot.inUse = false;
}

uint32_t CloudLayerRenderer::cloudLayerCount() const {
    uint32_t n = 0;
    for (const auto& slot : m_cloudLayers) if (slot.inUse) ++n;
    return n;
}

void CloudLayerRenderer::setCloudLayerWeather(uint32_t id, float rainIntensity, float stormTint) {
    if (id >= m_cloudLayers.size()) return;
    CloudLayerSlot& slot = m_cloudLayers[id];
    if (!slot.inUse) return;
    slot.rainOverride = rainIntensity;
    slot.stormOverride = stormTint;
}

void CloudLayerRenderer::updateCloudLayerUBO(uint32_t id) {
    if (id >= m_cloudLayers.size()) return;
    CloudLayerSlot& slot = m_cloudLayers[id];
    if (!slot.inUse || !slot.array) return;
    const uint32_t frame = m_ctx->currentFrame();
    if (!slot.uboMapped[frame]) return;

    CloudUBO c{};
    c.worldMin = slot.array->worldMin();
    c.worldMax = slot.array->worldMax();
    c.cloudBottom = m_cfg.cloudBottom;
    c.cloudTop = m_cfg.cloudBottom + m_cfg.cloudThickness;
    c.layerSpacing = m_cfg.cloudThickness;
    c.layerCount = 1;
    c.noiseScale = m_cfg.noiseScale;
    c.detailStrength = m_cfg.detailStrength;
    c.windOffset = Vec3(slot.array->windOffset().x, 0.0f, slot.array->windOffset().y);
    c.extinctionScale = 0.25f;
    c.scatteringAlbedo = 0.50f;
    c.phaseG1 = 0.5f;
    c.phaseG2 = -0.25f;
    c.phaseAlpha = 0.8f;
    c.powderStrength = 0.4f;
    c.ambientIntensity = 0.08f;
    c.godRayMode = static_cast<uint32_t>(m_cfg.godRayMode);
    c.debugMode = m_cfg.debugMode;
    c.weatherCoverage = m_weatherCoverage;
    c.rainIntensity = m_rainIntensity;
    c.stormTint = m_stormTint;
    c.cloudCoverageThreshold = 0.0f;
    c.cloudScale = m_cfg.cloudScale;
    c.cloudLightness = m_cfg.cloudLightness;
    c.cloudShade = m_cfg.cloudShade;
    c.cloudSoftness = m_cfg.cloudSoftness;
    c.marchStepBudget = static_cast<float>(m_marchStepBudget);
    std::memcpy(slot.uboMapped[frame], &c, sizeof(c));
}

void CloudLayerRenderer::renderCloudLayerPlane(VkCommandBuffer cmd, uint32_t id, float planeY,
                                               const Camera& camera, VkImageView depthView,
                                               float tintAltitude, bool legacyVolume) {
    if (!m_cfg.enabled || !m_ctx) return;
    if (id >= m_cloudLayers.size()) return;
    CloudLayerSlot& slot = m_cloudLayers[id];
    if (!slot.inUse || !slot.array) return;

    // Fit the plane to the camera frustum at the layer's altitude (same logic
    // as the global debug plane, but with this layer's world bounds/wind).
    Vec3 wmin = slot.array->worldMin();
    Vec3 wmax = slot.array->worldMax();
    float minX = wmin.x, maxX = wmax.x;
    float minZ = wmin.z, maxZ = wmax.z;

    Mat4 invVP = glm::inverse(camera.viewProjectionMatrix());
    Vec3 corners[8];
    int cidx = 0;
    for (int iz = 0; iz <= 1; ++iz) {
        for (int iy = -1; iy <= 1; iy += 2) {
            for (int ix = -1; ix <= 1; ix += 2) {
                Vec4 p = invVP * Vec4((float)ix, (float)iy, (float)iz, 1.0f);
                p /= p.w;
                corners[cidx++] = Vec3(p);
            }
        }
    }
    const int edges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    Vec3 intersectPoints[12];
    int intersectCount = 0;
    const float eps = 1e-4f;
    for (int e = 0; e < 12; ++e) {
        const Vec3& a = corners[edges[e][0]];
        const Vec3& b = corners[edges[e][1]];
        float da = a.y - planeY;
        float db = b.y - planeY;
        if (da * db < 0.0f) {
            float t = da / (da - db);
            Vec3 p = a + (b - a) * t;
            if (std::isfinite(p.x) && std::isfinite(p.z)) {
                intersectPoints[intersectCount++] = p;
            }
        } else if (std::abs(da) < eps) {
            intersectPoints[intersectCount++] = a;
        }
    }
    if (intersectCount > 0) {
        float fxmin = intersectPoints[0].x, fxmax = intersectPoints[0].x;
        float fzmin = intersectPoints[0].z, fzmax = intersectPoints[0].z;
        for (int i = 1; i < intersectCount; ++i) {
            fxmin = std::min(fxmin, intersectPoints[i].x);
            fxmax = std::max(fxmax, intersectPoints[i].x);
            fzmin = std::min(fzmin, intersectPoints[i].z);
            fzmax = std::max(fzmax, intersectPoints[i].z);
        }
        // Exact frustum fit to avoid extreme FP32 precision jitter at grazing angles.
        minX = fxmin; maxX = fxmax;
        minZ = fzmin; maxZ = fzmax;
    }

    // Restrict the quad to the cloud's actual world footprint (baked blobs +
    // wind drift) expanded by the vertical volume the ray-march can cover.
    // The frustum fit alone spans the entire visible slice and every fragment
    // ray-marches, so a few small clouds cost ~15 ms GPU without this.
    // Hoisted: the push constants below also use these footprint-driven values.
    // legacyVolume (non-field clouds) keeps the config dome values verbatim,
    // exactly like the pre-field commit: 200m spikes x 300m cells.
    float layerSpikeH = m_cfg.cloudSpikeHeight;
    float layerBaseD = m_cfg.cloudBaseDepth;
    float layerSpikeW = m_cfg.cloudSpikeWidth;
    {
        const Vec2 windUv = slot.array->windOffset();
        const Vec2 drift(windUv.x * (wmax.x - wmin.x), windUv.y * (wmax.z - wmin.z));
        float fminX = 1e30f, fmaxX = -1e30f, fminZ = 1e30f, fmaxZ = -1e30f;
        for (const LocalCloud& lc : slot.array->localClouds()) {
            if (!lc.alive) continue;
            // coverage_gen: density > 0 while d < 1/falloff (d normalized by radius).
            const float invF = 1.0f / std::max(lc.falloff, 0.01f);
            const float hx = lc.radius.x * invF;
            const float hz = lc.radius.y * invF;
            const float cr = std::cos(lc.rotation), sr = std::sin(lc.rotation);
            const float ex = std::sqrt(hx * hx * cr * cr + hz * hz * sr * sr);
            const float ez = std::sqrt(hx * hx * sr * sr + hz * hz * cr * cr);
            const float cx = lc.center.x + drift.x;
            const float cz = lc.center.y + drift.y;
            fminX = std::min(fminX, cx - ex); fmaxX = std::max(fmaxX, cx + ex);
            fminZ = std::min(fminZ, cz - ez); fmaxZ = std::max(fmaxZ, cz + ez);
        }
        if (fminX > fmaxX) return; // no baked blobs: nothing to draw

        // Ellipsoid proportions scale with this layer's footprint: the
        // global-layer config values (spikeHeight 200m / spikeWidth 300m)
        // were tuned for clouds kilometers away and turn near independent
        // clouds into a forest of giant columns. The plane stays nearly
        // FLAT (the vertical body comes from the cross-billboard cards):
        // a low, wide puff reads as a cloud from any pitch.
        // legacyVolume skips this rescale and keeps the config dome values.
        const float extent = std::max(fmaxX - fminX, fmaxZ - fminZ);
        if (!legacyVolume) {
            layerSpikeH = glm::clamp(extent * 0.06f, 4.0f, 18.0f);
            layerBaseD = glm::clamp(extent * 0.02f, 1.0f, 8.0f);
            layerSpikeW = glm::clamp(extent * 0.25f, 15.0f, 100.0f);
        }

        // Scissor to the exact screen projection of the cloud's volume AABB
        // (footprint XZ, planeY-baseDepth .. planeY+spikeHeight, plus the
        // legacy thickness pad for volumetric domes):
        // the ray-march only runs for fragments that can actually show cloud.
        {
            const float yBot = planeY - layerBaseD;
            const float yTop = planeY + (legacyVolume ? m_cfg.cloudDebugThickness * 0.5f : 2.0f) + layerSpikeH;
            const Mat4 vp = camera.viewProjectionMatrix();
            float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
            bool crossesNear = false;
            for (int i = 0; i < 8; ++i) {
                const float cx = (i & 1) ? fmaxX : fminX;
                const float cy = (i & 2) ? yTop : yBot;
                const float cz = (i & 4) ? fmaxZ : fminZ;
                const Vec4 p = vp * Vec4(cx, cy, cz, 1.0f);
                if (p.w <= 0.0f) { crossesNear = true; break; }
                const float nx = p.x / p.w, ny = p.y / p.w;
                sx0 = std::min(sx0, nx); sx1 = std::max(sx1, nx);
                sy0 = std::min(sy0, ny); sy1 = std::max(sy1, ny);
            }
            if (!crossesNear) {
                const float W = static_cast<float>(m_width), H = static_cast<float>(m_height);
                int ix0 = static_cast<int>(std::floor((sx0 * 0.5f + 0.5f) * W)) - 2;
                int iy0 = static_cast<int>(std::floor((sy0 * 0.5f + 0.5f) * H)) - 2;
                int ix1 = static_cast<int>(std::ceil((sx1 * 0.5f + 0.5f) * W)) + 2;
                int iy1 = static_cast<int>(std::ceil((sy1 * 0.5f + 0.5f) * H)) + 2;
                ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
                ix1 = std::min(ix1, static_cast<int>(m_width));
                iy1 = std::min(iy1, static_cast<int>(m_height));
                if (ix0 >= ix1 || iy0 >= iy1) return; // volume off-screen
                VkRect2D tight{{ix0, iy0}, {static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0)}};
                vkCmdSetScissor(cmd, 0, 1, &tight);
            } else {
                VkRect2D full{{0, 0}, {m_width, m_height}};
                vkCmdSetScissor(cmd, 0, 1, &full);
            }
        }

        // Margin for the ray-marched volume: rays hitting the volume above or
        // below the plane cross the plane outside the blob footprint.
        const float vHalf = (legacyVolume ? m_cfg.cloudDebugThickness * 0.5f : 2.0f) + layerSpikeH + layerBaseD;
        const Vec2 fc((fminX + fmaxX) * 0.5f, (fminZ + fmaxZ) * 0.5f);
        const float dy = camera.position().y - planeY;
        const float horiz = std::max(glm::length(Vec2(camera.position().x, camera.position().z) - fc), 1.0f);
        const float tanE = std::max(std::abs(dy) / horiz, 0.15f);
        const float margin = glm::clamp(vHalf / tanE, 0.0f, 4.0f * vHalf);
        fminX -= margin; fmaxX += margin; fminZ -= margin; fmaxZ += margin;

        // Intersect with the current quad bounds; empty means off-screen.
        minX = std::max(minX, fminX); maxX = std::min(maxX, fmaxX);
        minZ = std::max(minZ, fminZ); maxZ = std::min(maxZ, fmaxZ);
        if (minX >= maxX || minZ >= maxZ) return;
    }

    float worldSizeX = wmax.x - wmin.x;
    float worldSizeZ = wmax.z - wmin.z;
    float invWorldX = (worldSizeX > 1e-5f) ? 1.0f / worldSizeX : 0.0f;
    float invWorldZ = (worldSizeZ > 1e-5f) ? 1.0f / worldSizeZ : 0.0f;
    Vec2 wind = slot.array->windOffset();
    auto worldToUv = [&](float x, float z) -> std::pair<float, float> {
        float u = (x - wmin.x) * invWorldX - wind.x;
        float v = (z - wmin.z) * invWorldZ - wind.y;
        return {u, v};
    };
    auto uvMin = worldToUv(minX, minZ);
    auto uvMax = worldToUv(maxX, maxZ);

    float vertices[] = {
        minX, planeY, minZ, uvMin.first, uvMin.second,
        maxX, planeY, minZ, uvMax.first, uvMin.second,
        maxX, planeY, maxZ, uvMax.first, uvMax.second,
        minX, planeY, minZ, uvMin.first, uvMin.second,
        maxX, planeY, maxZ, uvMax.first, uvMax.second,
        minX, planeY, maxZ, uvMin.first, uvMax.second,
    };
    void* mapped = nullptr;
    if (vmaMapMemory(m_ctx->allocator(), slot.planeVboAlloc, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, vertices, sizeof(vertices));
        vmaUnmapMemory(m_ctx->allocator(), slot.planeVboAlloc);
    }

    updateDebugPlaneDepthDescriptor(depthView);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPlanePipeline);
    VkDescriptorSet sets[3] = {slot.set0[m_ctx->currentFrame()], slot.set1, m_debugPlaneDepthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPlanePipelineLayout, 0, 3, sets, 0, nullptr);

    struct {
        float layer;
        float threshold;
        float edgeSoftness;
        float rayMarchSteps;
        float cloudThickness;
        float pitchThreshold;
        float spikeHeight;
        float baseDepth;
        float spikeWidth;
        float weatherCoverage;
        float rainIntensity;
        float stormTint;
        float noRepeat;
        float tintAltitude;
    } pc;
    pc.layer = 0.0f; // independent layers always have a single slice
    pc.threshold = 0.0f;
    pc.edgeSoftness = m_cfg.cloudEdgeSoftness;
    pc.rayMarchSteps = m_cfg.cloudRayMarchSteps;
    pc.cloudThickness = m_cfg.cloudDebugThickness;
    // Weather-field layers render the flat POM coverage at every pitch except
    // near-straight-down (threshold -1 => POM whenever viewDir.y > -0.85):
    // the ray-march extrusion reads as a forest of stumps at gameplay
    // pitches, while the vertical body already comes from the cross-billboard
    // cards. legacyVolume (classic/manual clouds) keeps the commit-era
    // behavior: configured threshold, so gameplay pitches get the full
    // volumetric ray-march dome. Global layer uses the configured threshold
    // too (renderDebugPlane).
    pc.spikeHeight = layerSpikeH;
    pc.baseDepth = layerBaseD;
    pc.spikeWidth = layerSpikeW;
    pc.weatherCoverage = m_weatherCoverage;
    pc.rainIntensity = (slot.rainOverride >= 0.0f) ? slot.rainOverride : m_rainIntensity;
    pc.stormTint = (slot.stormOverride >= 0.0f) ? slot.stormOverride : m_stormTint;
    pc.noRepeat = 1.0f; // the cloud exists once and drifts until it leaves the map
    pc.tintAltitude = tintAltitude;
    vkCmdPushConstants(cmd, m_debugPlanePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    VkBuffer vb = slot.planeVbo;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

void CloudLayerRenderer::renderCloudLayerBillboard(VkCommandBuffer cmd, uint32_t id, float planeY,
                                                   const Camera& camera, VkImageView depthView,
                                                   float tintAltitude) {
    if (!m_cfg.enabled || !m_ctx) return;
    if (m_billboardPipeline == VK_NULL_HANDLE || m_billboardCubeVbo == VK_NULL_HANDLE) return;
    if (id >= m_cloudLayers.size()) return;
    CloudLayerSlot& slot = m_cloudLayers[id];
    // Half-res accumulation: scissor/viewport target dims shrink to the
    // offscreen while the camera math stays in normalized space.
    const uint32_t targetW = m_volumeAccumMode ? m_volumeAccumWidth : m_width;
    const uint32_t targetH = m_volumeAccumMode ? m_volumeAccumHeight : m_height;
    static const bool kPuffLogEarly = std::getenv("ERUPTION_TEST_PUFF_LOG") != nullptr;
    if (!slot.inUse || !slot.array) {
        if (kPuffLogEarly) { static int f0 = 0; if (++f0 % 60 == 0) ERUPTION_LOG_WARN("[PUFF] id=%u SKIP no slot/array", id); }
        return;
    }
    if (slot.set3 == VK_NULL_HANDLE) {
        if (kPuffLogEarly) { static int f1 = 0; if (++f1 % 60 == 0) ERUPTION_LOG_WARN("[PUFF] id=%u SKIP no set3", id); }
        return; // no local-cloud SSBO bound
    }

    Vec3 wmin = slot.array->worldMin();
    Vec3 wmax = slot.array->worldMax();

    // Footprint of the baked blobs with wind drift applied (same bounds math
    // as the old cross-billboard; the volume covers the whole merged cloud,
    // composite sub-blobs included, so it matches the shadow/rain silhouette).
    const Vec2 windUv = slot.array->windOffset();
    const Vec2 drift(windUv.x * (wmax.x - wmin.x), windUv.y * (wmax.z - wmin.z));
    float fminX = 1e30f, fmaxX = -1e30f, fminZ = 1e30f, fmaxZ = -1e30f;
    float puffTop = 0.0f;
    for (const LocalCloud& lc : slot.array->localClouds()) {
        if (!lc.alive) continue;
        const float invF = 1.0f / std::max(lc.falloff, 0.01f);
        const float hx = lc.radius.x * invF;
        const float hz = lc.radius.y * invF;
        const float cr = std::cos(lc.rotation), sr = std::sin(lc.rotation);
        const float ex = std::sqrt(hx * hx * cr * cr + hz * hz * sr * sr);
        const float ez = std::sqrt(hx * hx * sr * sr + hz * hz * cr * cr);
        const float cx = lc.center.x + drift.x;
        const float cz = lc.center.y + drift.y;
        fminX = std::min(fminX, cx - ex); fmaxX = std::max(fmaxX, cx + ex);
        fminZ = std::min(fminZ, cz - ez); fmaxZ = std::max(fmaxZ, cz + ez);
        // Tallest puff in the layer (same ry law as the shader's blobDensity):
        // the box MUST cover it or the frustum cull drops the cloud while its
        // top is still on screen (the pop-in/out at the frame edges).
        // Y extent of the ellipsoid mask: center planeY+ry, half-height ry/falloff.
        const float ry = glm::clamp(std::fmax(lc.radius.x, lc.radius.y) * invF * 0.6f, 6.0f, 100.0f);
        puffTop = std::max(puffTop, ry * (1.0f + invF));
    }
    if (fminX > fmaxX) {
        if (kPuffLogEarly) { static int f2 = 0; if (++f2 % 60 == 0) ERUPTION_LOG_WARN("[PUFF] id=%u SKIP no alive blobs (total=%zu)", id, slot.array->localClouds().size()); }
        return; // no baked blobs: nothing to draw
    }

    // Vertical body rising from the plane (never hanging below it): tall
    // enough to contain every puff in the layer, with a floor so tiny blobs
    // still have room for the noise to breathe.
    const float bodyH = std::max(puffTop, 12.0f);
    const float yBot = planeY;
    const float yTop = planeY + bodyH;

    // ERUPTION_TEST_PUFF_LOG=1 (debug): dump every layer's draw decision once per
    // second, to chase the pitch/zoom pop.
    static const bool kPuffLog = std::getenv("ERUPTION_TEST_PUFF_LOG") != nullptr;
    if (kPuffLog) {
        static int s_frame = 0;
        if (++s_frame % 300 == 1) {
            const Vec3 cp = camera.position();
            ERUPTION_LOG_WARN("[PUFF] id=%u cam=(%.1f,%.1f,%.1f) box=(%.1f..%.1f, %.1f..%.1f, %.1f..%.1f) blobs=%zu",
                            id, cp.x, cp.y, cp.z, fminX, fmaxX, yBot, yTop, fminZ, fmaxZ,
                            slot.array->localClouds().size());
            int bi = 0;
            for (const LocalCloud& lc : slot.array->localClouds()) {
                ERUPTION_LOG_WARN("[PUFF]   blob%d alive=%u c=(%.1f,%.1f) r=(%.1f,%.1f) fall=%.2f rot=%.2f dens=%.2f cov=%.2f",
                                bi++, lc.alive, lc.center.x, lc.center.y, lc.radius.x, lc.radius.y,
                                lc.falloff, lc.rotation, lc.density, lc.coverage);
            }
        }
    }

    // Frustum cull + distance LOD: clouds outside the view (e.g. behind the
    // camera during orbits) skip the draw entirely instead of degenerating
    // to a full-screen march; distant clouds ray-march at half steps.
    // ERUPTION_TEST_CLOUD_NO_LOD=1 (debug): restore the pre-optimization paths
    // (no cull, full steps, sun tap) for A/B screenshot diffs.
    static const bool kNoLod = std::getenv("ERUPTION_TEST_CLOUD_NO_LOD") != nullptr;
    const Mat4 vp = camera.viewProjectionMatrix();
    const Vec3 boxMin3(fminX, yBot, fminZ), boxMax3(fmaxX, yTop, fmaxZ);
    if (!kNoLod && aabbOutsideFrustum(vp, boxMin3, boxMax3)) return;
    const float camDist = aabbDistance(camera.position(), boxMin3, boxMax3);
    (void)camDist;
    // Angular-size LOD: tier by how BIG the cloud is on screen
    // (boxRadius / distance-to-center), not by raw distance — with the storm
    // field's huge clouds a box edge can brush the near tier while the cloud
    // is really a distant, few-hundred-pixel blob. Full quality is reserved
    // for clouds that actually fill the view (e.g. directly overhead).
    const Vec3 boxCenter3 = (boxMin3 + boxMax3) * 0.5f;
    const float boxRadius = glm::length(boxMax3 - boxMin3) * 0.5f;
    const float centerDist = glm::max(glm::distance(camera.position(), boxCenter3), 1.0f);
    const float angular = boxRadius / centerDist;
    // Continuous step scale: smooth ramps between the far/mid/near anchors
    // instead of discrete tiers — a tier boundary pops the cloud's march
    // quality (and, via pad2, its noise band) on a single frame during zoom.
    // The far plateau matches the mid one (0.28): at 0.14 distant thin
    // clouds sampled so coarsely (8 steps over the whole box) that they
    // vanished/flickered and then "popped" into existence on a small zoom-in
    // (author report 2026-08-10, A/B vs ERUPTION_TEST_CLOUD_NO_LOD at zoom 0).
    // Cost stays <= the old mid tier everywhere; ramp to full quality over
    // angular 2.0..3.5 (previously the jump to 1.0 happened at 2.0).
    float stepScale = 1.0f;
    if (!kNoLod) {
        if (angular <= 2.0f) {
            stepScale = 0.28f;
        } else if (angular <= 3.5f) {
            stepScale = glm::mix(0.28f, 1.0f, glm::smoothstep(2.0f, 3.5f, angular));
        }
    }

    // Scissor to the box projection (the march is not free).
    // ERUPTION_TEST_PUFF_FULLSCISSOR=1 (debug): always full-screen, to isolate
    // whether the box projection culls visible clouds.
    static const bool kFullScissor = std::getenv("ERUPTION_TEST_PUFF_FULLSCISSOR") != nullptr;
    {
        float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
        bool crossesNear = false;
        for (int i = 0; i < 8; ++i) {
            const float x = (i & 1) ? fmaxX : fminX;
            const float y = (i & 2) ? yTop : yBot;
            const float z = (i & 4) ? fmaxZ : fminZ;
            const Vec4 p = vp * Vec4(x, y, z, 1.0f);
            if (p.w <= 0.0f) { crossesNear = true; break; }
            sx0 = std::min(sx0, p.x / p.w); sx1 = std::max(sx1, p.x / p.w);
            sy0 = std::min(sy0, p.y / p.w); sy1 = std::max(sy1, p.y / p.w);
        }
        if (!crossesNear && !kFullScissor) {
            const float W = static_cast<float>(targetW), H = static_cast<float>(targetH);
            int ix0 = static_cast<int>(std::floor((sx0 * 0.5f + 0.5f) * W)) - 2;
            int iy0 = static_cast<int>(std::floor((sy0 * 0.5f + 0.5f) * H)) - 2;
            int ix1 = static_cast<int>(std::ceil((sx1 * 0.5f + 0.5f) * W)) + 2;
            int iy1 = static_cast<int>(std::ceil((sy1 * 0.5f + 0.5f) * H)) + 2;
            ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
            ix1 = std::min(ix1, static_cast<int>(targetW));
            iy1 = std::min(iy1, static_cast<int>(targetH));
            if (ix0 >= ix1 || iy0 >= iy1) return; // off-screen
            VkRect2D tight{{ix0, iy0}, {static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0)}};
            vkCmdSetScissor(cmd, 0, 1, &tight);
        } else {
            VkRect2D full{{0, 0}, {targetW, targetH}};
            vkCmdSetScissor(cmd, 0, 1, &full);
        }
    }

    updateDebugPlaneDepthDescriptor(depthView);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_volumeAccumMode ? m_billboardAccumPipeline : m_billboardPipeline);
    if (m_volumeAccumMode) {
        VkViewport vp{0, 0, (float)targetW, (float)targetH, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp);
    }
    VkDescriptorSet sets[4] = {slot.set0[m_ctx->currentFrame()], slot.set1, m_debugPlaneDepthSet, slot.set3};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_billboardPipelineLayout, 0, 4, sets, 0, nullptr);
    VkBuffer vb = m_billboardCubeVbo;
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);

    struct {
        glm::vec4 boxMin;
        glm::vec4 boxMax;
        float seed;
        float rainIntensity;
        float stormTint;
        float tintAltitude;
        int32_t blobCount;
        float pad0;
        float pad1;
        float pad2;
        float targetScale;
        float marchCap;   // teto de passos da marcha (preset cloud_march_steps)
    } pc;
    pc.boxMin = glm::vec4(fminX, yBot, fminZ, 1.0f);
    pc.boxMax = glm::vec4(fmaxX, yTop, fmaxZ, 1.0f);
    pc.seed = static_cast<float>(slot.array->seed());
    pc.rainIntensity = (slot.rainOverride >= 0.0f) ? slot.rainOverride : m_rainIntensity;
    pc.stormTint = (slot.stormOverride >= 0.0f) ? slot.stormOverride : m_stormTint;
    pc.tintAltitude = tintAltitude;
    pc.blobCount = static_cast<int32_t>(slot.array->localClouds().size());
    pc.pad2 = 0.0f;
    // ERUPTION_TEST_PUFF_NODEPCLIP=1 (debug): skip the scene-depth clamp in the
    // shader, to isolate occlusion-clamp culling from geometry culling.
    static const bool kNoDepClip = std::getenv("ERUPTION_TEST_PUFF_NODEPCLIP") != nullptr;
    pc.pad0 = kNoDepClip ? 1.0f : 0.0f;
    pc.marchCap = static_cast<float>(m_marchStepBudget);
    // ERUPTION_TEST_PUFF_SOLID=1 / ERUPTION_TEST_PUFF_DENSITY=1 (debug): box solid / march
    // density visualization in the fragment.
    static const bool kSolid = std::getenv("ERUPTION_TEST_PUFF_SOLID") != nullptr;
    static const bool kDensViz = std::getenv("ERUPTION_TEST_PUFF_DENSITY") != nullptr;
    static const bool kMaskViz = std::getenv("ERUPTION_TEST_PUFF_MASK") != nullptr;
    static const bool kNoiseViz = std::getenv("ERUPTION_TEST_PUFF_NOISE") != nullptr;
    pc.pad1 = kNoiseViz ? 4.0f : (kMaskViz ? 3.0f : (kDensViz ? 2.0f : (kSolid ? 1.0f : 0.0f)));
    // Ray-march step scale (distance LOD, see above; shader clamps to >= 0.25).
    pc.pad2 = stepScale;
    pc.targetScale = m_volumeAccumMode
                         ? static_cast<float>(m_volumeAccumWidth) / static_cast<float>(m_width)
                         : 1.0f;

    vkCmdPushConstants(cmd, m_billboardPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 36, 1, 0, 0);
}

void CloudLayerRenderer::renderCloudLayerShadow(VkCommandBuffer cmd, uint32_t id, float planeY,
                                                const Camera& camera, const Vec3& lightDir,
                                                VkImageView depthView, uint32_t width, uint32_t height) {
    if (!m_cfg.enabled || !m_cfg.showCloudShadows || !m_ctx) return;
    if (m_cloudShadowsPipeline == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE) return;
    if (id >= m_cloudLayers.size()) return;
    CloudLayerSlot& slot = m_cloudLayers[id];
    if (!slot.inUse || !slot.array) return;

    // ERUPTION_TEST_CLOUD_SHADOW_DEBUG=1 (debug): per-cloud cull trace, 1x/segundo.
    static const bool kCsDebug = std::getenv("ERUPTION_TEST_CLOUD_SHADOW_DEBUG") != nullptr;
    static int csFrame = -1;
    ++csFrame;
    const bool csLog = kCsDebug && ((csFrame % 60) == 0);
    if (csLog) {
        ERUPTION_LOG_WARN("[CSHADOW] id=%u planeY=%.0f lightDir=(%.2f,%.2f,%.2f) camPos=(%.0f,%.0f,%.0f)",
                        id, planeY, lightDir.x, lightDir.y, lightDir.z,
                        camera.position().x, camera.position().y, camera.position().z);
    }

    updateCloudShadowsDepthDescriptor(depthView);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_shadowAccumMode ? m_cloudShadowAccumPipeline : m_cloudShadowsPipeline);

    VkViewport vp{0, 0, (float)width, (float)height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);

    // Scissor to the screen projection of the region the ground shadow can
    // cover: the blob footprint plus its projection along the sun direction
    // (same formula as the cloud_shadows fragment shader / rain followers).
    VkRect2D scissor{{0, 0}, {width, height}};
    {
        const Vec3 wmin = slot.array->worldMin();
        const Vec3 wmax = slot.array->worldMax();
        const Vec2 windUv = slot.array->windOffset();
        const Vec2 drift(windUv.x * (wmax.x - wmin.x), windUv.y * (wmax.z - wmin.z));
        float fminX = 1e30f, fmaxX = -1e30f, fminZ = 1e30f, fmaxZ = -1e30f;
        for (const LocalCloud& lc : slot.array->localClouds()) {
            if (!lc.alive) continue;
            const float invF = 1.0f / std::max(lc.falloff, 0.01f);
            const float hx = lc.radius.x * invF;
            const float hz = lc.radius.y * invF;
            const float cr = std::cos(lc.rotation), sr = std::sin(lc.rotation);
            const float ex = std::sqrt(hx * hx * cr * cr + hz * hz * sr * sr);
            const float ez = std::sqrt(hx * hx * sr * sr + hz * hz * cr * cr);
            const float cx = lc.center.x + drift.x;
            const float cz = lc.center.y + drift.y;
            fminX = std::min(fminX, cx - ex); fmaxX = std::max(fmaxX, cx + ex);
            fminZ = std::min(fminZ, cz - ez); fmaxZ = std::max(fmaxZ, cz + ez);
        }
        if (fminX <= fmaxX) {
            // Shadow shift at ground level 0 (worst case): n * planeY * tanAngle.
            // lightDir points DOWN (direction the light travels), so the
            // L.y > 0 gate is never true — the shadow falls straight down,
            // deliberately (see cloud_shadows.frag). Kept for parity.
            Vec3 L = glm::normalize(lightDir);
            if (L.y > 0.01f) {
                const float tanAngle = glm::min(glm::length(Vec2(L.x, L.z)) / L.y, 4.0f);
                const Vec2 n = glm::normalize(Vec2(L.x, L.z) + Vec2(0.0001f));
                const float sx = n.x * planeY * tanAngle;
                const float sz = n.y * planeY * tanAngle;
                fminX = std::min(fminX, fminX + sx); fmaxX = std::max(fmaxX, fmaxX + sx);
                fminZ = std::min(fminZ, fminZ + sz); fmaxZ = std::max(fmaxZ, fmaxZ + sz);
            }
            const Mat4 viewProj = camera.viewProjectionMatrix();
            // Frustum cull + distance skip: off-screen clouds degenerate to a
            // full-screen pass via crossesNear, and shadows cast by clouds far
            // from the camera are invisible in the storm haze anyway.
            // The shadow only lands on terrain, so the cull/scissor box stops
            // at the highest ground the shadow can fall on instead of rising
            // to the cloud plane — with y up to planeY the camera sits inside
            // nearly every cloud's box and crossesNear forces a full-screen
            // pass for all 100 clouds (that alone costs ~10x the frame).
            const float shadowClipY = glm::min(planeY, 300.0f);
            const Vec3 sboxMin(fminX, 0.0f, fminZ), sboxMax(fmaxX, shadowClipY, fmaxZ);
            if (aabbOutsideFrustum(viewProj, sboxMin, sboxMax)) {
                if (csLog) ERUPTION_LOG_WARN("[CSHADOW]   id=%u CULLED frustum box=(%.0f..%.0f, %.0f..%.0f)", id, fminX, fmaxX, fminZ, fmaxZ);
                return;
            }
            const float csDist = aabbDistance(camera.position(), sboxMin, sboxMax);
            // ALCANCE PROPORCIONAL AO QUE A CAMERA VE. O limite era 800 u
            // fixos - numero de camera de JOGO (orbita 60-300 u). Numa camera
            // de diorama (orbita 320-900 u, que e' a da demo) o campo de
            // nuvens se espalha por 0,6 x o mapa (2400 u no parana_field) e
            // TODA nuvem cai fora de 800 u: em "cloudy" o passe media 0,011 ms
            // e a sombra de nuvem simplesmente nao existia no chao (autor,
            // 2026-09-06: "nem a sombra da nuvem ta aparecendo no chao").
            // A justificativa do corte e' a nevoa engolir a sombra distante -
            // e "distante" escala com a distancia de observacao, nao e' uma
            // constante. Piso de 800 u preserva o comportamento de jogo.
            // ERUPTION_TEST_CLOUD_SHADOW_DIST=<u> fixa o valor (bancada).
            static const float kCsDistEnv = [] {
                const char* e = std::getenv("ERUPTION_TEST_CLOUD_SHADOW_DIST");
                return e ? std::max(0.0f, static_cast<float>(std::atof(e))) : 0.0f;
            }();
            // Alcance = ate' onde a camera VE CHAO (mesma conta da cascata de
            // sombra em ShadowRenderer::updateCascades): altura sobre o alvo
            // h = d*sin(pitch), e o raio de baixo do frustum toca o chao no
            // angulo (pitch - fov/2); abaixo de ~4 graus aponta pro horizonte
            // e vale o teto. Uma sombra de nuvem sobre chao VISIVEL nunca e'
            // descartada por distancia; alem do chao visivel, nao ha' o que
            // sombrear.
            float csMaxDist = kCsDistEnv;
            if (csMaxDist <= 0.0f) {
                const float h = std::max(camera.orbitDistance() *
                                         std::sin(std::max(camera.orbitPitch(), 0.0f)), 1.0f);
                const float ang = camera.orbitPitch() - glm::radians(camera.fov()) * 0.5f;
                const float groundFar = (ang > glm::radians(4.0f)) ? (h / std::sin(ang)) * 1.15f
                                                                   : 6000.0f;
                csMaxDist = glm::clamp(groundFar, 800.0f, 6000.0f);
            }
            if (csDist > csMaxDist) {
                if (csLog) ERUPTION_LOG_WARN("[CSHADOW]   id=%u CULLED dist=%.0f>%.0f box=(%.0f..%.0f, %.0f..%.0f)", id, csDist, csMaxDist, fminX, fmaxX, fminZ, fmaxZ);
                return;
            }
            float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
            bool crossesNear = false;
            for (int i = 0; i < 8; ++i) {
                const float cx = (i & 1) ? fmaxX : fminX;
                const float cy = (i & 2) ? shadowClipY : 0.0f;
                const float cz = (i & 4) ? fmaxZ : fminZ;
                const Vec4 p = viewProj * Vec4(cx, cy, cz, 1.0f);
                if (p.w <= 0.0f) { crossesNear = true; break; }
                const float nx = p.x / p.w, ny = p.y / p.w;
                sx0 = std::min(sx0, nx); sx1 = std::max(sx1, nx);
                sy0 = std::min(sy0, ny); sy1 = std::max(sy1, ny);
            }
            if (!crossesNear) {
                const float W = static_cast<float>(width), H = static_cast<float>(height);
                int ix0 = static_cast<int>(std::floor((sx0 * 0.5f + 0.5f) * W)) - 2;
                int iy0 = static_cast<int>(std::floor((sy0 * 0.5f + 0.5f) * H)) - 2;
                int ix1 = static_cast<int>(std::ceil((sx1 * 0.5f + 0.5f) * W)) + 2;
                int iy1 = static_cast<int>(std::ceil((sy1 * 0.5f + 0.5f) * H)) + 2;
                ix0 = std::max(ix0, 0); iy0 = std::max(iy0, 0);
                ix1 = std::min(ix1, static_cast<int>(width));
                iy1 = std::min(iy1, static_cast<int>(height));
                if (ix0 >= ix1 || iy0 >= iy1) {
                    if (csLog) ERUPTION_LOG_WARN("[CSHADOW]   id=%u CULLED offscreen-scissor", id);
                    return; // shadow region off-screen
                }
                scissor = {{ix0, iy0}, {static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0)}};
                if (csLog) ERUPTION_LOG_WARN("[CSHADOW]   id=%u DRAW scissor=(%d,%d %dx%d) dist=%.0f", id, ix0, iy0, ix1 - ix0, iy1 - iy0, csDist);
            } else if (csLog) {
                ERUPTION_LOG_WARN("[CSHADOW]   id=%u DRAW fullscreen (crossesNear)", id);
            }
        }
    }
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDescriptorSet sets[3] = {slot.set0[m_ctx->currentFrame()], slot.set1, m_cloudShadowsDepthSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudShadowsPipelineLayout, 0, 3, sets, 0, nullptr);

    // Push layout: layer, shadowIntensity, coveragePlaneHeight, threshold, weatherCoverage, noRepeat.
    float pcData[6] = {0.0f, m_cfg.cloudShadowOpacity, planeY, cloudShadowBaseStrength(), m_weatherCoverage, 1.0f};
    vkCmdPushConstants(cmd, m_cloudShadowsPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcData), pcData);

    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void CloudLayerRenderer::composite(VkCommandBuffer cmd,
                                   VkImageView sceneColorView,
                                   VkImageView sceneDepthView,
                                   VkImageView outputView,
                                   const Vec3& sunVisualDir) {
    // TODO: implement depth-aware composite if not done in PostProcessor.
    (void)cmd; (void)sceneColorView; (void)sceneDepthView; (void)outputView; (void)sunVisualDir;
}

} // namespace eruption
