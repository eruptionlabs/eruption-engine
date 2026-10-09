#include <cstdlib>
#include "renderer/DeferredLighting.hpp"
#include "renderer/ShadowRenderer.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "math/Camera.hpp"
#include "core/Logger.hpp"

#include <cstring>
#include <cmath>
#include <algorithm>

namespace eruption {

static void generateSphere(std::vector<Vec3>& vertices, std::vector<uint32_t>& indices,
                           uint32_t stacks, uint32_t slices) {
    vertices.clear();
    indices.clear();

    for (uint32_t i = 0; i <= stacks; i++) {
        float phi = glm::pi<float>() * i / stacks;
        for (uint32_t j = 0; j <= slices; j++) {
            float theta = 2.0f * glm::pi<float>() * j / slices;
            float x = std::sin(phi) * std::cos(theta);
            float y = std::cos(phi);
            float z = std::sin(phi) * std::sin(theta);
            vertices.push_back(Vec3(x, y, z));
        }
    }

    for (uint32_t i = 0; i < stacks; i++) {
        for (uint32_t j = 0; j < slices; j++) {
            uint32_t i0 = i * (slices + 1) + j;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = (i + 1) * (slices + 1) + j;
            uint32_t i3 = i2 + 1;
            indices.push_back(i0);
            indices.push_back(i2);
            indices.push_back(i1);
            indices.push_back(i1);
            indices.push_back(i2);
            indices.push_back(i3);
        }
    }
}

static float getAnimationFactor(LightAnimType type, float time) {
    switch (type) {
        case LightAnimType::Pulse:
            return 0.8f + 0.2f * std::sin(time * 3.0f);
        case LightAnimType::Flicker:
            return 0.9f + 0.1f * std::sin(time * 20.0f) * std::sin(time * 7.0f);
        case LightAnimType::Torch:
            return 0.85f + 0.15f * (std::sin(time * 15.0f) + std::cos(time * 9.0f)) * 0.5f + 0.05f;
        case LightAnimType::Static:
        default:
            return 1.0f;
    }
}

bool DeferredLighting::init(VulkanContext* ctx, GBuffer* gbuffer, BindlessDescriptor* bindless,
                             uint32_t width, uint32_t height) {
    m_ctx = ctx;
    m_gbuffer = gbuffer;
    m_bindless = bindless;
    m_width = width;
    m_height = height;

    createLitImage();
    createLightVolumeGeometry();
    createPipelines();
    
    // Create UBOs and descriptor sets per frame
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        m_ctx->createBuffer(sizeof(ShadowUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VMA_MEMORY_USAGE_CPU_TO_GPU, m_shadowUboBuffers[i], m_shadowUboAllocs[i]);
        vmaMapMemory(m_ctx->allocator(), m_shadowUboAllocs[i], &m_shadowUboMappeds[i]);

        m_ctx->createBuffer(sizeof(LightingUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VMA_MEMORY_USAGE_CPU_TO_GPU, m_lightingUboBuffers[i], m_lightingUboAllocs[i]);
        vmaMapMemory(m_ctx->allocator(), m_lightingUboAllocs[i], &m_lightingUboMappeds[i]);
    }

    createDescriptors();

    // Create a plain linear sampler for G-Buffer sampling
    VkSamplerCreateInfo samplerInfo{}; samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_gbufferSampler);

    return m_litView != VK_NULL_HANDLE && m_directionalPipeline != VK_NULL_HANDLE;
}

void DeferredLighting::shutdown() {
    if (m_sphereVertexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_sphereVertexBuffer, m_sphereVertexAlloc);
        m_sphereVertexBuffer = VK_NULL_HANDLE;
    }
    if (m_sphereIndexBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_sphereIndexBuffer, m_sphereIndexAlloc);
        m_sphereIndexBuffer = VK_NULL_HANDLE;
    }
    if (m_litImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_litView, nullptr);
        vmaDestroyImage(m_ctx->allocator(), m_litImage, m_litAlloc);
        m_litImage = VK_NULL_HANDLE;
        m_litView = VK_NULL_HANDLE;
    }
    if (m_ssaoImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_ssaoView, nullptr);
        vmaDestroyImage(m_ctx->allocator(), m_ssaoImage, m_ssaoAlloc);
        m_ssaoImage = VK_NULL_HANDLE; m_ssaoView = VK_NULL_HANDLE;
    }
    if (m_ssaoBlurImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_ssaoBlurView, nullptr);
        vmaDestroyImage(m_ctx->allocator(), m_ssaoBlurImage, m_ssaoBlurAlloc);
        m_ssaoBlurImage = VK_NULL_HANDLE; m_ssaoBlurView = VK_NULL_HANDLE;
    }
    if (m_ssaoPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_ssaoPipeline, nullptr);
        m_ssaoPipeline = VK_NULL_HANDLE;
    }
    if (m_ssaoBlurPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_ssaoBlurPipeline, nullptr);
        m_ssaoBlurPipeline = VK_NULL_HANDLE;
    }
    if (m_directionalPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_directionalPipeline, nullptr);
        m_directionalPipeline = VK_NULL_HANDLE;
    }
    if (m_pointLightPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pointLightPipeline, nullptr);
        m_pointLightPipeline = VK_NULL_HANDLE;
    }
    if (m_ambientPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_ambientPipeline, nullptr);
        m_ambientPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_descLayout, nullptr);
        m_descLayout = VK_NULL_HANDLE;
    }
    if (m_descPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }
    if (m_gbufferSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_gbufferSampler, nullptr);
        m_gbufferSampler = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (m_shadowUboBuffers[i] != VK_NULL_HANDLE) {
            vmaUnmapMemory(m_ctx->allocator(), m_shadowUboAllocs[i]);
            vmaDestroyBuffer(m_ctx->allocator(), m_shadowUboBuffers[i], m_shadowUboAllocs[i]);
            m_shadowUboBuffers[i] = VK_NULL_HANDLE;
            m_shadowUboMappeds[i] = nullptr;
        }
        if (m_lightingUboBuffers[i] != VK_NULL_HANDLE) {
            vmaUnmapMemory(m_ctx->allocator(), m_lightingUboAllocs[i]);
            vmaDestroyBuffer(m_ctx->allocator(), m_lightingUboBuffers[i], m_lightingUboAllocs[i]);
            m_lightingUboBuffers[i] = VK_NULL_HANDLE;
            m_lightingUboMappeds[i] = nullptr;
        }
    }
}

void DeferredLighting::resize(uint32_t width, uint32_t height) {
    const SkyProbeBinding sky = m_skyProbeBinding;
    const ProbeGridBinding grid = m_probeGridBinding;
    shutdown();
    m_width = width;
    m_height = height;
    init(m_ctx, m_gbuffer, m_bindless, width, height);
    // Os descritores novos nascem sem a sonda do céu e sem a grade de
    // irradiância: sem isto o shader lia bindings nunca escritos.
    if (sky.view != VK_NULL_HANDLE) setSkyProbe(sky.view, sky.sampler, sky.sh, sky.shSize, sky.mips);
    if (grid.view != VK_NULL_HANDLE) setIrradianceProbes(grid.view, grid.sampler, grid.min, grid.invExtent, grid.enabled);
}

void DeferredLighting::setEnvironment(const LightingEnvironment& env) {
    m_env = env;
}

void DeferredLighting::setPointLights(const std::vector<PointLight>& lights) {
    m_pointLights = lights;
}

void DeferredLighting::createLitImage() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_width;
    imageInfo.extent.height = m_height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_litImage, &m_litAlloc, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_litImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_litView);

    // Half-resolution SSAO chain (raw + bilateral-blurred), R8.
    m_ssaoWidth  = std::max(1u, m_width / 2);
    m_ssaoHeight = std::max(1u, m_height / 2);
    auto makeAoImage = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        VkImageCreateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ai.imageType = VK_IMAGE_TYPE_2D;
        ai.extent = { m_ssaoWidth, m_ssaoHeight, 1 };
        ai.mipLevels = 1;
        ai.arrayLayers = 1;
        ai.format = VK_FORMAT_R8_UNORM;
        ai.tiling = VK_IMAGE_TILING_OPTIMAL;
        ai.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ai.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ai.samples = VK_SAMPLE_COUNT_1_BIT;
        ai.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aa{};
        aa.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(m_ctx->allocator(), &ai, &aa, &img, &alloc, nullptr);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8_UNORM;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkc::createImageView(m_ctx->device(), &vi, nullptr, &view);
    };
    makeAoImage(m_ssaoImage, m_ssaoAlloc, m_ssaoView);
    makeAoImage(m_ssaoBlurImage, m_ssaoBlurAlloc, m_ssaoBlurView);
    m_ssaoNeutralCleared = false; // fresh images start in UNDEFINED layout
}

void DeferredLighting::createLightVolumeGeometry() {
    std::vector<Vec3> sphereVerts;
    std::vector<uint32_t> sphereIndices;
    generateSphere(sphereVerts, sphereIndices, 16, 16);

    m_sphereVertexCount = static_cast<uint32_t>(sphereVerts.size());
    m_sphereIndexCount = static_cast<uint32_t>(sphereIndices.size());

    VkDeviceSize vSize = sphereVerts.size() * sizeof(Vec3);
    VkDeviceSize iSize = sphereIndices.size() * sizeof(uint32_t);

    m_ctx->createBuffer(vSize,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, m_sphereVertexBuffer, m_sphereVertexAlloc);
    m_ctx->createBuffer(iSize,
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, m_sphereIndexBuffer, m_sphereIndexAlloc);

    VkBuffer staging;
    VmaAllocation stagingAlloc;
    m_ctx->createBuffer(vSize + iSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);

    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, sphereVerts.data(), vSize);
    std::memcpy(static_cast<uint8_t*>(mapped) + vSize, sphereIndices.data(), iSize);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy vCopy{}; vCopy.size = vSize;
        vkCmdCopyBuffer(cmd, staging, m_sphereVertexBuffer, 1, &vCopy);
        VkBufferCopy iCopy{}; iCopy.srcOffset = vSize; iCopy.size = iSize;
        vkCmdCopyBuffer(cmd, staging, m_sphereIndexBuffer, 1, &iCopy);
    });

    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
}

void DeferredLighting::createPipelines() {
    auto loadShader = [&](const char* path, VkShaderStageFlagBits stage) -> VkShaderModule {
        auto code = ShaderCompiler::loadSPIRV(path);
        if (code.empty()) return VK_NULL_HANDLE;
        VkShaderModuleCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        VkShaderModule mod;
        vkCreateShaderModule(m_ctx->device(), &info, nullptr, &mod);
        return mod;
    };

    // Create descriptor layout for G-Buffer sampling + shadow UBO + lighting UBO
    std::vector<VkDescriptorSetLayoutBinding> bindings(12);
    for (uint32_t i = 0; i < 6; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[11].binding = 11;
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // 12 = raw SSAO (read by the blur pass), 13 = blurred SSAO (read by lighting)
    // 14 = o MESMO atlas de sombra do binding 5, mas com sampler NAO
    //      comparativo. O binding 5 e' sampler2DShadow: devolve o RESULTADO do
    //      teste de profundidade (0/1 filtrado), nunca a profundidade. PCSS
    //      precisa da profundidade CRUA do bloqueador pra calcular a largura da
    //      penumbra, entao precisa desta segunda vista da mesma imagem.
    // 15 = SONDA DE CEU (samplerCube pre-filtrado, G36); 16 = SH9 da sonda
    //      (storage buffer, 9 vec4). Escritos uma vez em setSkyProbe().
    // 17 = PROBES DE IRRADIANCIA (sampler3D, G38) - trocado ao carregar mapa.
    bindings.resize(18);
    for (uint32_t b = 12; b <= 15; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[16].binding = 16;
    bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[16].descriptorCount = 1;
    bindings[16].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[17].binding = 17;
    bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[17].descriptorCount = 1;
    bindings[17].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // UPDATE_AFTER_BIND: estes sets sao reescritos por frame com as MESMAS
    // views enquanto o CB do frame anterior ainda esta' pendente - sem a flag
    // isso e' VUID-03047 (80k+ erros por sessao). Com ela, a atualizacao
    // enquanto pendente e' legal; como o conteudo escrito e' identico, nao ha'
    // corrida real de dados.
    // Flag SO' nos combined-image-samplers (UAB de buffer exige feature propria).
    std::vector<VkDescriptorBindingFlags> uabFlags(bindings.size(), 0);
    for (size_t b = 0; b < bindings.size(); ++b)
        if (m_ctx->bindless() && bindings[b].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            uabFlags[b] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo uabInfo{};
    uabInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    uabInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    uabInfo.pBindingFlags = uabFlags.data();

    VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
    descLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descLayoutInfo.pBindings = bindings.data();
    descLayoutInfo.pNext = m_ctx->bindless() ? &uabInfo : nullptr;
    // Sem descriptor indexing nao ha' UPDATE_AFTER_BIND (caminho .nb).
    if (m_ctx->bindless()) descLayoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    vkCreateDescriptorSetLayout(m_ctx->device(), &descLayoutInfo, nullptr, &m_descLayout);

    // Push constants for lighting
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset = 0;
    // The point-light push constant struct (lightPos+radius, lightColor+intensity,
    // shadowCubemapIndex, invViewProj, viewProj, modelMatrix) is 240 bytes once
    // GLM's SIMD alignment (GLM_FORCE_INTRINSICS/AVX2) pads mat4 fields to a
    // 16-byte boundary - 224 silently truncated vkCmdPushConstants' upload,
    // corrupting/zeroing modelMatrix's tail and making every point light draw
    // invisible with no validation error (layers were off).
    pcRange.size = 256;

    VkPipelineLayoutCreateInfo layoutInfo{}; layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout);

    // --- Directional light pipeline (fullscreen quad) ---
    {
        VkShaderModule vert = loadShader("shaders/lighting/directional.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
        VkShaderModule frag = loadShader("shaders/lighting/directional.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
        if (!vert || !frag) {
            ERUPTION_LOG_ERROR("Failed to load directional light shaders");
            return;
        }

        std::vector<VkPipelineShaderStageCreateInfo> stages(2);
        stages[0] = {}; stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
        stages[1] = {}; stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{}; vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;

        m_directionalPipeline = PipelineBuilder()
            .setShaderStages(stages)
            .setVertexInput(vertexInput)
            .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
            .setViewport(0, 0, static_cast<float>(m_width), static_cast<float>(m_height))
            .setScissor(0, 0, m_width, m_height)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
            .setBlendState({blend})
            .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setLayout(m_pipelineLayout)
            .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
            .build(m_ctx->device());

        vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    }

    // --- Point light pipeline (DEPRECATED, but kept for compatibility) ---
    {
        VkShaderModule vert = loadShader("shaders/lighting/point_light.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
        VkShaderModule frag = loadShader("shaders/lighting/point_light.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
        if (!vert || !frag) {
            ERUPTION_LOG_ERROR("Failed to load point light shaders");
            return;
        }

        std::vector<VkPipelineShaderStageCreateInfo> stages(2);
        stages[0] = {}; stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
        stages[1] = {}; stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vec3);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

        VkPipelineVertexInputStateCreateInfo vertexInput{}; vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attr;

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;

        m_pointLightPipeline = PipelineBuilder()
            .setShaderStages(stages)
            .setVertexInput(vertexInput)
            .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setViewport(0, 0, static_cast<float>(m_width), static_cast<float>(m_height))
            .setScissor(0, 0, m_width, m_height)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            // No depth attachment is bound in this dynamic-rendering pass (see
            // renderPointLights: cmdBeginRendering gets colour only) - the
            // shader manually reconstructs worldPos and range-checks against
            // it instead of relying on hardware depth test. A pipeline that
            // declares depthTestEnable + a depth attachment format while the
            // actual render pass has none is a format mismatch (silently
            // dropped every draw on this driver instead of erroring).
            .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
            .setBlendState({blend})
            .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setLayout(m_pipelineLayout)
            .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
            .build(m_ctx->device());

        vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    }

    // --- Ambient + emissive pipeline ---
    {
        VkShaderModule vert = loadShader("shaders/lighting/ambient.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
        VkShaderModule frag = loadShader("shaders/lighting/ambient.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
        if (!vert || !frag) {
            ERUPTION_LOG_ERROR("Failed to load ambient shaders");
            return;
        }

        std::vector<VkPipelineShaderStageCreateInfo> stages(2);
        stages[0] = {}; stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
        stages[1] = {}; stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{}; vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_FALSE;

        m_ambientPipeline = PipelineBuilder()
            .setShaderStages(stages)
            .setVertexInput(vertexInput)
            .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
            .setViewport(0, 0, static_cast<float>(m_width), static_cast<float>(m_height))
            .setScissor(0, 0, m_width, m_height)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
            .setBlendState({blend})
            .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setLayout(m_pipelineLayout)
            .setColorAttachmentFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
            .build(m_ctx->device());

        vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    }

    // --- SSAO + bilateral blur (half-res fullscreen passes, R8) ---
    {
        auto buildAoPipeline = [&](const char* fragPath, VkPipeline& out) {
            VkShaderModule vert = loadShader("shaders/lighting/ambient.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
            VkShaderModule frag = loadShader(fragPath, VK_SHADER_STAGE_FRAGMENT_BIT);
            if (!vert || !frag) {
                ERUPTION_LOG_ERROR("Failed to load SSAO shader %s", fragPath);
                return;
            }
            std::vector<VkPipelineShaderStageCreateInfo> stages(2);
            stages[0] = {}; stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
            stages[1] = {}; stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineColorBlendAttachmentState aoBlend{};
            aoBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
            aoBlend.blendEnable = VK_FALSE;

            out = PipelineBuilder()
                .setShaderStages(stages)
                .setVertexInput(vertexInput)
                .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                .setViewport(0, 0, static_cast<float>(m_ssaoWidth), static_cast<float>(m_ssaoHeight))
                .setScissor(0, 0, m_ssaoWidth, m_ssaoHeight)
                .setPolygonMode(VK_POLYGON_MODE_FILL)
                .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
                .setBlendState({aoBlend})
                .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                .setLayout(m_pipelineLayout)
                .setColorAttachmentFormats({VK_FORMAT_R8_UNORM})
                .build(m_ctx->device());

            vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
            vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
        };
        buildAoPipeline("shaders/lighting/ssao.frag.spv", m_ssaoPipeline);
        buildAoPipeline("shaders/lighting/ssao_blur.frag.spv", m_ssaoBlurPipeline);
    }
}

void DeferredLighting::createDescriptors() {
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 16 * MAX_FRAMES;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 8 * MAX_FRAMES;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; // binding 16 (SH da sonda de ceu)
    poolSizes[2].descriptorCount = 2 * MAX_FRAMES;

    VkDescriptorPoolCreateInfo poolInfo{}; poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    if (m_ctx->bindless()) poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 4 * MAX_FRAMES;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool);

    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES, m_descLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = MAX_FRAMES;
    allocInfo.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, m_descSets);

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        // Bind shadow UBO to set
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_shadowUboBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descSets[i];
        writes[0].dstBinding = 6;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufferInfo;

        VkDescriptorBufferInfo lightingInfo{};
        lightingInfo.buffer = m_lightingUboBuffers[i];
        lightingInfo.offset = 0;
        lightingInfo.range = VK_WHOLE_SIZE;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descSets[i];
        writes[1].dstBinding = 7;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &lightingInfo;

        vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);
    }
}

void DeferredLighting::updateDescriptorSet(VkImageView shadowView, VkSampler shadowSampler,
                                           VkImageView nextShadowView, VkSampler nextShadowSampler,
                                           VkImageView cloudShadowView, VkSampler cloudShadowSampler,
                                           VkImageView rainOcclusionView, VkSampler rainOcclusionSampler,
                                           uint32_t frameIndex) {
    VkSampler gbufferSampler = m_gbufferSampler;

    std::vector<VkDescriptorImageInfo> imageInfos(13);
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = m_gbuffer->albedoView();
    imageInfos[0].sampler = gbufferSampler;

    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = m_gbuffer->normalView();
    imageInfos[1].sampler = gbufferSampler;

    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].imageView = m_gbuffer->depthView();
    imageInfos[2].sampler = gbufferSampler;

    imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[3].imageView = m_gbuffer->pbrView();
    imageInfos[3].sampler = gbufferSampler;

    imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[4].imageView = m_gbuffer->emissiveView();
    imageInfos[4].sampler = gbufferSampler;

    imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[5].imageView = shadowView;
    imageInfos[5].sampler = shadowSampler;

    imageInfos[6].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[6].imageView = nextShadowView;
    imageInfos[6].sampler = nextShadowSampler;

    imageInfos[7].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Binding 9 era gbufferWorldPos. O attachment saiu do G-buffer e nenhum
    // shader deste set o declara mais, mas o LAYOUT ainda exige um descritor
    // valido. Aponta para o NORMAL, que estes shaders ja' amostram: usar o
    // depth aqui seria apontar um sampler para a imagem que e' attachment de
    // profundidade do mesmo frame - hazard de layout, mesmo que ninguem leia.
    imageInfos[7].imageView = m_gbuffer->normalView();
    imageInfos[7].sampler = gbufferSampler;

    imageInfos[8].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[8].imageView = cloudShadowView ? cloudShadowView : m_gbuffer->depthView();
    imageInfos[8].sampler = cloudShadowSampler ? cloudShadowSampler : gbufferSampler;

    imageInfos[9].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[9].imageView = rainOcclusionView ? rainOcclusionView : m_gbuffer->depthView();
    imageInfos[9].sampler = rainOcclusionSampler ? rainOcclusionSampler : gbufferSampler;

    std::vector<VkWriteDescriptorSet> writes(13);
    for (uint32_t i = 0; i < 6; i++) {
        writes[i] = {}; writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descSets[frameIndex];
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }
    
    // nextShadowAtlas is binding 8
    writes[6] = {}; writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descSets[frameIndex];
    writes[6].dstBinding = 8; 
    writes[6].dstArrayElement = 0;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].descriptorCount = 1;
    writes[6].pImageInfo = &imageInfos[6];

    // worldPos is binding 9
    writes[7] = {}; writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descSets[frameIndex];
    writes[7].dstBinding = 9;
    writes[7].dstArrayElement = 0;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].descriptorCount = 1;
    writes[7].pImageInfo = &imageInfos[7];

    // cloudShadowMap is binding 10
    writes[8] = {}; writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = m_descSets[frameIndex];
    writes[8].dstBinding = 10;
    writes[8].dstArrayElement = 0;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[8].descriptorCount = 1;
    writes[8].pImageInfo = &imageInfos[8];

    // rainOcclusionMap is binding 11
    writes[9] = {}; writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet = m_descSets[frameIndex];
    writes[9].dstBinding = 11;
    writes[9].dstArrayElement = 0;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[9].descriptorCount = 1;
    writes[9].pImageInfo = &imageInfos[9];

    // SSAO chain: 12 = raw (blur pass input), 13 = blurred (lighting input)
    imageInfos[10].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[10].imageView = m_ssaoView;
    imageInfos[10].sampler = gbufferSampler;
    imageInfos[11].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[11].imageView = m_ssaoBlurView;
    imageInfos[11].sampler = gbufferSampler;

    for (uint32_t w = 10; w <= 11; ++w) {
        writes[w] = {}; writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[w].dstSet = m_descSets[frameIndex];
        writes[w].dstBinding = w + 2; // 12, 13
        writes[w].dstArrayElement = 0;
        writes[w].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[w].descriptorCount = 1;
        writes[w].pImageInfo = &imageInfos[w];
    }

    // Atlas de sombra outra vez, agora com sampler NAO comparativo (o
    // m_gbufferSampler ja' e' compareEnable=FALSE, entao nao precisa de sampler
    // novo). E' o que deixa o PCSS ler a profundidade do bloqueador.
    imageInfos[12].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[12].imageView = shadowView;
    imageInfos[12].sampler = gbufferSampler;
    writes[12] = {}; writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[12].dstSet = m_descSets[frameIndex];
    writes[12].dstBinding = 14;
    writes[12].dstArrayElement = 0;
    writes[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[12].descriptorCount = 1;
    writes[12].pImageInfo = &imageInfos[12];

    vkUpdateDescriptorSets(m_ctx->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void DeferredLighting::setSkyProbe(VkImageView cubeView, VkSampler sampler, VkBuffer shBuffer,
                                   VkDeviceSize shSize, uint32_t mipCount) {
    if (cubeView == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE || shBuffer == VK_NULL_HANDLE) {
        m_skyProbeBound = false;
        m_skyProbeBinding = {};
        return;
    }
    m_skyProbeBinding = {cubeView, sampler, shBuffer, shSize, mipCount};
    VkDescriptorImageInfo img{};
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img.imageView = cubeView;
    img.sampler = sampler;
    VkDescriptorBufferInfo buf{};
    buf.buffer = shBuffer;
    buf.offset = 0;
    buf.range = shSize;
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkWriteDescriptorSet w[2] = {};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = m_descSets[i];
        w[0].dstBinding = 15;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].descriptorCount = 1;
        w[0].pImageInfo = &img;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet = m_descSets[i];
        w[1].dstBinding = 16;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].descriptorCount = 1;
        w[1].pBufferInfo = &buf;
        vkUpdateDescriptorSets(m_ctx->device(), 2, w, 0, nullptr);
    }
    m_skyProbeMaxMip = static_cast<float>(mipCount > 0 ? mipCount - 1 : 0);
    m_skyProbeBound = true;
}

void DeferredLighting::setIrradianceProbes(VkImageView view, VkSampler sampler,
                                           const Vec3& gridMin, const Vec3& gridInvExtent, bool enabled) {
    if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) return;
    m_probeGridBinding = {view, sampler, gridMin, gridInvExtent, enabled};
    VkDescriptorImageInfo img{};
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img.imageView = view;
    img.sampler = sampler;
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_descSets[i];
        w.dstBinding = 17;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &img;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    }
    m_probeGridMin = gridMin;
    m_probeGridInvExtent = gridInvExtent;
    m_probesEnabled = enabled;
}

void DeferredLighting::render(VkCommandBuffer cmd, const Camera& camera, const ShadowRenderer* shadows,
                              VkImageView cloudShadowView, VkSampler cloudShadowSampler,
                              const Vec3& cloudWorldMin, const Vec3& cloudWorldMax,
                              float cloudShadowIntensity,
                              VkImageView rainOcclusionView, VkSampler rainOcclusionSampler,
                              float totalTime) {
    uint32_t frameIndex = m_ctx->currentFrame();

    // Fill Lighting UBO. SEM `{}`: sizeof(LightingUBO) = 4416 bytes (69 cache
    // lines), e `ubo{}` zerava esse total todo frame - incluindo pointLights[128]
    // e pointColors[128] (2048B cada) alem de activeLights, que o shader nunca
    // le (so' consome ate' numPointLights). Era memset puro jogado fora.
    //
    // Confirmado campo a campo (grep nos usos de `ubo.` nesta funcao) que todo
    // campo escalar declarado em LightingUBO e' escrito incondicionalmente
    // abaixo, EXCETO pad0/pad1 - esses dois nunca sao lidos por nenhum shader
    // (grep em shaders/lighting/*.frag e postprocess/*.frag: zero ocorrencias de
    // `.pad0`/`.pad1` fora da propria declaracao do layout), entao zera-los
    // aqui e' so' higiene contra ferramenta de validacao que inspecione o
    // buffer inteiro, nao correcao funcional.
    LightingUBO ubo;
    // Raio angular da fonte. 0,0046 rad = 0,53 grau = o disco solar real.
    // Valores maiores fingem uma fonte grande e proxima (softbox de estudio
    // sobre uma maquete), que e' o que endurece a sombra no contato e a
    // alarga longe do contato.
    // ERUPTION_TEST_SUN_ANGULAR=<graus> (debug): varre o tamanho da fonte sem
    // recompilar. 0.53 = disco solar; 37 = softbox de 60cm a 40cm da maquete.
    static const float kSunAngEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_SUN_ANGULAR");
        return (e && *e) ? glm::radians(std::stof(e)) : -1.0f;
    }();
    ubo.sunAngularRadius = kSunAngEnv > 0.0f ? kSunAngEnv : m_sunAngularRadius;
    ubo.pad1 = 0;
    ubo.dirLightDir = Vec4(m_env.sun.direction, m_env.sun.intensity);
    ubo.dirLightColor = Vec4(m_env.sun.color, m_env.ambientIntensity);
    
    float solarIntensity = m_env.sun.intensity;
    float artificialMod = glm::mix(1.0f, 0.15f, glm::smoothstep(0.2f, 0.8f, solarIntensity));

    uint32_t activeLights = 0;
    for (uint32_t i = 0; i < (uint32_t)m_pointLights.size() && activeLights < 128; i++) {
        const auto& pl = m_pointLights[i];
        if (!pl.enabled) continue;

        ubo.pointLights[activeLights] = Vec4(pl.position, pl.radius);
        
        float animFactor = getAnimationFactor(pl.animType, totalTime);
        float finalIntensity = pl.intensity * artificialMod * animFactor;
        ubo.pointColors[activeLights] = Vec4(pl.color, finalIntensity);
        activeLights++;
    }
    ubo.numPointLights = activeLights;
    ubo.giIntensity = m_giIntensity;
    ubo.giAmbientFloor = m_giAmbientFloor;
    ubo.usePbr = m_usePbr ? 1u : 0u;
    // ERUPTION_DEBUG_FLAT_ALBEDO=1: bit 32 pinta o albedo de ciano chapado nos
    // tres passes de luz (direcional, ambiente, pontual). Diagnostico de
    // cintilacao de sombra: com a textura fora do caminho, o que piscar entre
    // frames e' sombra ou iluminacao, nao UV/mip.
    static const uint32_t kFlatAlbedo = [] {
        const char* e = std::getenv("ERUPTION_DEBUG_FLAT_ALBEDO");
        return (e && *e == '1') ? 32u : 0u;
    }();
    // ERUPTION_TEST_PBR_DEBUG=<mask> (1=roughness 2=metallic 4=normal
    // 8=wetness 16=source): mesma visualizacao do checkbox, sem UI - e' o que
    // permite validar headless que a leitura nao muda com a hora do dia.
    static const uint32_t kPbrDebugEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_PBR_DEBUG");
        return e ? (static_cast<uint32_t>(std::atoi(e)) & 95u) : 0u; // 64 = AO cru
    }();
    ubo.pbrDebugMode = m_pbrDebugMode | kFlatAlbedo | kPbrDebugEnv;
    ubo.pbrLightScale = m_pbrLightScale;
    ubo.rainIntensity = m_rainIntensity;
    ubo.snowIntensity = m_snowIntensity;
    ubo.temperatureC = m_temperatureC;
    ubo.ambientSky = Vec4(m_env.ambientSkyColor, 0.0f);
    ubo.ambientGround = Vec4(m_env.ambientGroundColor, 0.0f);
    ubo.ssaoParams = Vec4(m_ssaoEnabled ? 1.0f : 0.0f, m_ssaoStrength, m_ssaoRadius, 0.0f);
    ubo.indirectParams = Vec4(m_envSpecEnabled ? 1.0f : 0.0f, m_envSpecIntensity,
                              m_nightAoPow, m_ambientHemiFloor);
    ubo.viewProj = camera.viewProjJittered();
    ubo.invViewProj = glm::inverse(camera.viewProjJittered());
    ubo.bounceParams = Vec4(m_sunBounceEnabled ? 1.0f : 0.0f, m_sunBounceStrength, m_fillLightIntensity, 0.0f);
    ubo.contactParams = Vec4(m_contactShadowEnabled ? 1.0f : 0.0f, m_contactShadowLength, 0.0f, 0.0f);
    ubo.skyHorizon = Vec4(m_env.skyHorizonColor, 0.0f);
    ubo.skyProbeParams = Vec4((m_skyProbeEnabled && m_skyProbeBound) ? 1.0f : 0.0f,
                              m_skyProbeMaxMip, m_skyProbeIntensity, 0.0f);
    ubo.probeGridMin = Vec4(m_probeGridMin, (m_probesEnabled && m_probeStrength > 0.0f) ? 1.0f : 0.0f);
    ubo.probeGridInvExtent = Vec4(m_probeGridInvExtent, m_probeStrength);
    
    std::memcpy(m_lightingUboMappeds[frameIndex], &ubo, sizeof(LightingUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_lightingUboAllocs[frameIndex], 0, sizeof(LightingUBO));

    // Transition lit image to color attachment
    m_ctx->cmdImageBarrier(cmd, m_litImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    // Transition G-Buffer to read
    m_gbuffer->transitionToRead(cmd);

    // Bind-time-valid descriptors for the SSAO chain and the lighting passes.
    // Must happen before the first vkCmdBindDescriptorSets of the frame.
    if (shadows) {
        updateDescriptorSet(shadows->shadowAtlasView(), shadows->shadowSampler(),
                            shadows->nextShadowAtlasView(), shadows->shadowSampler(),
                            cloudShadowView, cloudShadowSampler,
                            rainOcclusionView, rainOcclusionSampler, frameIndex);
    }

    // --- SSAO chain: raw half-res pass -> bilateral blur -> shader-read for the
    // lighting passes. Binding 13 must always hold a valid, correctly-laid-out
    // image, but when SSAO is off we only need it cleared to white ONCE - not
    // two half-res passes per frame writing 1.0.
    const bool runSsao = m_ssaoEnabled || !m_ssaoNeutralCleared;
    if (runSsao && m_ssaoPipeline != VK_NULL_HANDLE && m_ssaoBlurPipeline != VK_NULL_HANDLE) {
        VkViewport aoViewport{};
        aoViewport.width = static_cast<float>(m_ssaoWidth);
        aoViewport.height = static_cast<float>(m_ssaoHeight);
        aoViewport.minDepth = 0.0f; aoViewport.maxDepth = 1.0f;
        VkRect2D aoScissor{{0, 0}, {m_ssaoWidth, m_ssaoHeight}};
        VkExtent2D aoExtent{m_ssaoWidth, m_ssaoHeight};

        const bool doDraw = m_ssaoEnabled; // off: clear-to-white only
        auto aoPass = [&](VkImage target, VkImageView targetView, VkPipeline pipe) {
            m_ctx->cmdImageBarrier(cmd, target,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{};
            att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            att.imageView = targetView;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.color = {{1.0f, 1.0f, 1.0f, 1.0f}};
            m_ctx->cmdBeginRendering(cmd, {att}, nullptr, nullptr, aoExtent);
            if (doDraw) {
                vkCmdSetViewport(cmd, 0, 1, &aoViewport);
                vkCmdSetScissor(cmd, 0, 1, &aoScissor);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                        0, 1, &m_descSets[frameIndex], 0, nullptr);
                vkCmdDraw(cmd, 4, 1, 0, 0);
            }
            m_ctx->cmdEndRendering(cmd);
            m_ctx->cmdImageBarrier(cmd, target,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        };
        aoPass(m_ssaoImage, m_ssaoView, m_ssaoPipeline);
        aoPass(m_ssaoBlurImage, m_ssaoBlurView, m_ssaoBlurPipeline);
        // Off: both targets now hold neutral white in SHADER_READ layout and
        // can be sampled for free until SSAO is re-enabled.
        m_ssaoNeutralCleared = !m_ssaoEnabled;
    }

    VkRenderingAttachmentInfo colorAttachment{}; colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_litView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkExtent2D extent{m_width, m_height};
    m_ctx->cmdBeginRendering(cmd, {colorAttachment}, nullptr, nullptr, extent);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Pass 1: Ambient + Emissive
    renderAmbientPass(cmd);

    // Pass 2: Directional light + Point Lights (Combined in the same pipeline now)
    if (shadows) {
        renderDirectionalPass(cmd, camera, shadows, cloudShadowView, cloudShadowSampler,
                              cloudWorldMin, cloudWorldMax, cloudShadowIntensity,
                              rainOcclusionView, rainOcclusionSampler, totalTime);
    }

    // Pass 3: Point lights (restored volumetric rendering)
    renderPointLights(cmd, camera);

    m_ctx->cmdEndRendering(cmd);
}

void DeferredLighting::renderAmbientPass(VkCommandBuffer cmd) {
    uint32_t frameIndex = m_ctx->currentFrame();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ambientPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &m_descSets[frameIndex], 0, nullptr);

    struct AmbientPush {
        Vec3 ambientColor;
        float ambientIntensity;
        Vec3 emissiveColor;
        float emissiveIntensity;
    } push;
    push.ambientColor = m_env.ambientColor;
    push.ambientIntensity = m_env.ambientIntensity;
    push.emissiveColor = Vec3(1.0f);
    push.emissiveIntensity = 1.0f;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, // range do layout e' VERT|FRAG (VUID 01796)
                       0, sizeof(push), &push);

    vkCmdDraw(cmd, 4, 1, 0, 0);

}

void DeferredLighting::renderDirectionalPass(VkCommandBuffer cmd, const Camera& camera,
                                              const ShadowRenderer* shadows,
                                              VkImageView cloudShadowView, VkSampler cloudShadowSampler,
                                              const Vec3& cloudWorldMin, const Vec3& cloudWorldMax,
                                              float cloudShadowIntensity,
                                              VkImageView rainOcclusionView, VkSampler rainOcclusionSampler,
                                              float totalTime) {
    uint32_t frameIndex = m_ctx->currentFrame();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_directionalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &m_descSets[frameIndex], 0, nullptr);

    Mat4 invViewProj = glm::inverse(camera.viewProjJittered());

    // Upload shadow data to UBO
    ShadowUBO shadowUbo{};
    shadowUbo.lightSpaceMats[0] = shadows->getCascadeMatrices()[0];
    shadowUbo.lightSpaceMats[1] = shadows->getCascadeMatrices()[1];
    shadowUbo.nextLightSpaceMats[0] = shadows->getNextCascadeMatrices()[0];
    shadowUbo.nextLightSpaceMats[1] = shadows->getNextCascadeMatrices()[1];
    
    shadowUbo.cascadeSplits = shadows->getCascadeSplits();

    const auto& ss = shadows->settings();

    // ERUPTION_TEST_SHADOW_TINT=<0..1> (debug): paint sun-shadowed fragments magenta
    // with the given opacity instead of just darkening them. Hijacks the
    // unused cascadeScales slot; 0 = normal rendering. The F2 "Debug Shadows
    // (Magenta)" checkbox does the same live at 0.9 opacity (env wins).
    // ERUPTION_TEST_NO_SHADOW=1 (debug): zero the sun shadow factor in the shader
    // (lighting untouched) for A/B captures.
    static const float kShadowTintEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_SHADOW_TINT");
        return e ? std::max(0.0f, std::min(1.0f, std::stof(e))) : 0.0f;
    }();
    static const float kNoShadow = std::getenv("ERUPTION_TEST_NO_SHADOW") != nullptr ? 1.0f : 0.0f;
    const float kShadowTint = kShadowTintEnv > 0.0f ? kShadowTintEnv
                                                    : (ss.debugMagentaShadow ? 0.9f : 0.0f);
    shadowUbo.cascadeScales = Vec4(shadows->getWorldTexelSize(), shadows->getC1WorldTexelSize(),
                                   kShadowTint, ss.enabled ? kNoShadow : 1.0f);

    shadowUbo.shadowParams = Vec4(
        static_cast<float>(ss.pcfKernelSize),
        static_cast<float>(ss.poissonTaps), // poissonTaps
        ss.usePoisson ? 1.0f : 0.0f,
        ss.slopeBias /* w = fator do bias por inclinacao */);

    float finalBlendFactor = ss.enableTemporalBlend ? shadows->getBlendFactor() : 0.0f;
    // Slope bias removed, using constant depth and normal bias
    shadowUbo.shadowRanges = Vec4(ss.normalBias, shadows->getShadowDepthRange(), ss.biasValue, finalBlendFactor); 
    // Opacidade da sombra por distancia (ver Engine::m_shadowOpacity): os
    // mesmos quatro nos do relevo, avaliados no shader com a distancia ate' a
    // camera. knots.x < 0 desliga e a sombra volta a ser cheia.
    shadowUbo.shadowOpacityParams = m_shadowOpacityParams;
    shadowUbo.shadowOpacityLut0 = m_shadowOpacityLut0;
    shadowUbo.shadowOpacityLut1 = m_shadowOpacityLut1;

    std::memcpy(m_shadowUboMappeds[frameIndex], &shadowUbo, sizeof(ShadowUBO));
    vmaFlushAllocation(m_ctx->allocator(), m_shadowUboAllocs[frameIndex], 0, sizeof(ShadowUBO));

    // Descriptors are updated once per frame in render(), before the SSAO chain.

    struct DirPush {
        Vec3 lightDir;
        float intensity;
        Vec3 lightColor;
        uint32_t shadowMapIndex;
        Vec3 cameraPos;
        float cloudShadowIntensity;
        Vec4 cloudWorldMin;     // xyz = world min, w = inv size x
        Vec4 cloudWorldMax;     // xyz = world max, w = inv size z
        Mat4 invViewProj;
        Mat4 cloudShadowMatrix;
    } push;
    push.lightDir = m_env.sun.direction;
    push.intensity = m_env.sun.intensity;
    push.lightColor = m_env.sun.color;
    push.shadowMapIndex = m_env.sun.shadowMapIndex;
    push.cameraPos = camera.position();
    push.cloudShadowIntensity = cloudShadowIntensity;

    Vec3 cloudSize = cloudWorldMax - cloudWorldMin;
    push.cloudWorldMin = Vec4(cloudWorldMin, 1.0f / std::max(cloudSize.x, 0.001f));
    push.cloudWorldMax = Vec4(cloudWorldMax, 1.0f / std::max(cloudSize.z, 0.001f));

    push.invViewProj = invViewProj;
    // Simple ortho projection covering the cloud bounds, looking down the light dir.
    Vec3 lightDir = glm::normalize(m_env.sun.direction);
    Vec3 up = (std::abs(lightDir.y) < 0.99f) ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
    Vec3 right = glm::normalize(glm::cross(up, lightDir));
    up = glm::cross(lightDir, right);
    Mat4 lightView = glm::lookAt(cloudWorldMin, cloudWorldMin - lightDir, up);
    Mat4 lightProj = glm::ortho(0.0f, cloudSize.x, 0.0f, cloudSize.z, 0.0f, cloudSize.y * 2.0f);
    push.cloudShadowMatrix = lightProj * lightView;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, // range do layout e' VERT|FRAG (VUID 01796)
                       0, sizeof(push), &push);

    vkCmdDraw(cmd, 4, 1, 0, 0);

}

void DeferredLighting::renderPointLights(VkCommandBuffer cmd, const Camera& camera) {
    // ERUPTION_TEST_NO_POINT_LIGHTS=1 (debug): desliga so' este passe, com o
    // preset intacto - e' o unico jeito de medir o custo/efeito das point
    // lights isolado (trocar de preset muda dez coisas ao mesmo tempo).
    static const bool kNoPointLights = std::getenv("ERUPTION_TEST_NO_POINT_LIGHTS") != nullptr;
    if (!m_pointLightsEnabled || kNoPointLights || m_pointLights.empty()) return;

    uint32_t frameIndex = m_ctx->currentFrame();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pointLightPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &m_descSets[frameIndex], 0, nullptr);

    VkBuffer vb = m_sphereVertexBuffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, m_sphereIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

    Mat4 viewProj = camera.viewProjJittered();
    Mat4 invViewProj = glm::inverse(viewProj);

    for (const auto& light : m_pointLights) {
        if (!light.enabled) continue;

        Mat4 model = glm::translate(Mat4(1.0f), light.position);
        model = glm::scale(model, Vec3(light.radius));

        // GLSL/SPIR-V push-constant layout requires every mat4 to start at a
        // 16-byte-aligned offset - the plain C++ struct below packs tightly
        // (no padding after shadowCubemapIndex, since glm's mat4 is only
        // 4-byte aligned in this build), so without the explicit pad the GPU
        // reads invViewProj/viewProj/modelMatrix 12 bytes off from what this
        // side actually wrote: every light's geometry transformed to garbage
        // and never rasterized visibly (found via a forced-position vertex
        // shader test that DID render, proving the pipeline itself was fine).
        struct PointPush {
            Vec3 lightPos;
            float radius;
            Vec3 lightColor;
            float intensity;
            uint32_t shadowCubemapIndex;
            // Ocupa os 12 bytes que ja' eram padding de alinhamento: a posicao
            // da camera entra de graca, sem crescer o push constant (que ja'
            // esta' em 256 bytes, o limite comum).
            // ATENCAO ao lado GLSL: point_light.frag declara isto como TRES
            // floats (cameraPosX/Y/Z), NAO vec3 - em std430 um vec3 alinha em
            // 16 bytes e cairia em 48, empurrando invViewProj pra 64 e
            // desalinhando as tres matrizes. Foi esse bug que deixou toda
            // point light invisivel ate' 2026-09-05 (worldPos reconstruido
            // com matriz lixo -> `dist > radius` descartava tudo).
            Vec3 cameraPos;
            Mat4 invViewProj;
            Mat4 viewProj;
            Mat4 modelMatrix;
        } push;
        push.lightPos = light.position;
        push.radius = light.radius;
        push.lightColor = light.color;
        push.intensity = light.intensity;
        push.shadowCubemapIndex = light.shadowCubemapIndex;
        push.cameraPos = camera.position();
        push.invViewProj = invViewProj;
        push.viewProj = viewProj;
        push.modelMatrix = model;

        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);

        vkCmdDrawIndexed(cmd, m_sphereIndexCount, 1, 0, 0, 0);
    }
}

} // namespace eruption
