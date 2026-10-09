#include <chrono>
#include "renderer/PostProcessor.hpp"
#include <cstdlib>
#include "renderer/PostFormat.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include <cmath>
#include <cstring>
#include "utils/Profiler.hpp"

#include <cstring>
#include <fstream>
#include <random>
#include <algorithm>
#include <vector>

namespace eruption {

// ---------------------------------------------------------------------------
// QUEBRA DO "Post Total" (bracket 8..9, escrito pela Engine em volta desta
// classe inteira). Ate' aqui o post aparecia no Profiler como UM numero de
// 7-10 ms na chuva, sem dizer se era overlay, particula, bloom ou composite.
//
// Mapa de ocupacao do pool de timestamps (VulkanContext::TIMESTAMP_QUERY_COUNT
// = 64), confirmado por grep em 2026-09-02:
//    0-11  Engine  (sombra 0-1, gbuffer 2-3, deferred 4-5, skybox 6-7,
//                   POST TOTAL 8-9, agua 10-11)
//   12-19  LIVRES
//   20-29  Engine  (cloud shadow map 20-21, minimapa 22-23, rain topdown
//                   24-25, cloud layers 26-27, cloud shadows field 28-29)
//   30-35  PostProcessor (heightmap 30-31, overlay 32-33, particulas 34-35)
//   36-39  Engine  (cloud volumes 36-37, map smoke 38-39)
//   40-41  PostProcessor (DoF / tilt-shift)
//   42-53  PostProcessor (ESTE bloco)
//   54-63  LIVRES
//
// CUIDADO ao ler a soma: sao timestamps de BOTTOM_OF_PIPE, e a GPU pode
// sobrepor passes vizinhos. Por isso o residuo vira um passe proprio
// ("Post Unattributed") em vez de ser escondido, e o aviso de divergencia
// so' dispara acima de 10%.
namespace {
constexpr uint32_t kQPostTotalBegin  = 8;   // escrito pela Engine, lido aqui
constexpr uint32_t kQPostTotalEnd    = 9;
constexpr uint32_t kQAutoExpBegin    = 42;
constexpr uint32_t kQAutoExpEnd      = 43;
constexpr uint32_t kQLensDropBegin   = 44;  // ANINHADO dentro de 32..33
constexpr uint32_t kQLensDropEnd     = 45;
constexpr uint32_t kQBloomDownBegin  = 46;
constexpr uint32_t kQBloomDownEnd    = 47;
constexpr uint32_t kQBloomUpBegin    = 48;
constexpr uint32_t kQBloomUpEnd      = 49;
constexpr uint32_t kQCompositeBegin  = 50;
constexpr uint32_t kQCompositeEnd    = 51;
constexpr uint32_t kQCloudDbgBegin   = 52;
constexpr uint32_t kQCloudDbgEnd     = 53;
} // namespace

bool PostProcessor::init(VulkanContext* ctx, uint32_t width, uint32_t height) {
    m_ctx = ctx;
    m_width = width;
    m_height = height;

    if (!m_weatherRenderer.init(ctx)) {
        Logger::error("PostProcessor: failed to initialize WeatherRenderer");
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_linearSampler);

    createImages();
    createDescriptors();
    createWeatherUbo();
    createLensDropBuffer();
    createLensDropCompute();
    createAutoExposure();
    createPipelines();
    return true;
}

void PostProcessor::shutdown() {
    if (!m_ctx) return;
    auto device = m_ctx->device();
    destroyAutoExposure();

    vkDestroyPipeline(device, m_cocPipeline, nullptr);
    vkDestroyPipeline(device, m_blurHPipeline, nullptr);
    vkDestroyPipeline(device, m_blurVPipeline, nullptr);
    vkDestroyPipeline(device, m_cocCompositePipeline, nullptr);
    vkDestroyPipeline(device, m_compositePipeline, nullptr);
    vkDestroyPipeline(device, m_copyPipeline, nullptr);
    
    vkDestroyPipeline(device, m_brightPassPipeline, nullptr);
    vkDestroyPipeline(device, m_bloomDownPipeline, nullptr);
    vkDestroyPipeline(device, m_bloomUpPipeline, nullptr);
    vkDestroyPipeline(device, m_weatherPipeline, nullptr);
    vkDestroyPipeline(device, m_cloudCoverageDebugPipeline, nullptr);

    m_weatherRenderer.shutdown();

    destroyLensDropCompute();
    destroyLensDropBuffer();
    destroyWeatherUbo();

    vkDestroyPipelineLayout(device, m_layout, nullptr);

    vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroySampler(device, m_linearSampler, nullptr);

    auto destroyImg = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (img != VK_NULL_HANDLE) {
            vmaDestroyImage(m_ctx->allocator(), img, alloc);
            img = VK_NULL_HANDLE;
        }
    };

    destroyImg(m_dofImage, m_dofAlloc, m_dofView);
    destroyImg(m_cocImage, m_cocAlloc, m_cocView);
    destroyImg(m_blurHImage, m_blurHAlloc, m_blurHView);
    destroyImg(m_blurVImage, m_blurVAlloc, m_blurVView);
    destroyImg(m_outputImage, m_outputAlloc, m_outputView);
    destroyImg(m_weatherImage, m_weatherAlloc, m_weatherView);

    for (uint32_t i = 0; i < BLOOM_MIPS; i++) {
        destroyImg(m_bloomDownImages[i], m_bloomDownAllocs[i], m_bloomDownViews[i]);
        destroyImg(m_bloomUpImages[i], m_bloomUpAllocs[i], m_bloomUpViews[i]);
    }

    m_ctx = nullptr;
}

void PostProcessor::resize(uint32_t width, uint32_t height) {
    auto* ctx = m_ctx;
    shutdown();
    m_ctx = ctx;
    m_width = width;
    m_height = height;
    // resize() retornava void e engolia a falha do init: se a recriacao falha
    // no tamanho novo, as imagens ficam nulas e o frame sai PRETO em silencio.
    if (!init(m_ctx, width, height)) {
        ERUPTION_LOG_ERROR("PostProcessor::resize: init FALHOU em %ux%u - o frame vai sair preto",
                           width, height);
    }
}

void PostProcessor::createImages() {
    // Um so' formato para toda a cadeia intermediaria - ver PostFormat.hpp.
    const VkFormat postFmt = postColorFormat(m_ctx->physicalDevice());
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = postFmt;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    auto createImg = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &img, &alloc, nullptr);
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = img;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = postFmt;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &view);
    };

    createImg(m_outputImage, m_outputAlloc, m_outputView);
    createImg(m_dofImage, m_dofAlloc, m_dofView);
    createImg(m_cocImage, m_cocAlloc, m_cocView);
    createImg(m_blurHImage, m_blurHAlloc, m_blurHView);
    createImg(m_blurVImage, m_blurVAlloc, m_blurVView);
    createImg(m_weatherImage, m_weatherAlloc, m_weatherView);

    // Bloom Pyramid
    uint32_t bw = m_width / 2;
    uint32_t bh = m_height / 2;
    for (uint32_t i = 0; i < BLOOM_MIPS; i++) {
        imageInfo.extent = {bw, bh, 1};
        createImg(m_bloomDownImages[i], m_bloomDownAllocs[i], m_bloomDownViews[i]);
        createImg(m_bloomUpImages[i], m_bloomUpAllocs[i], m_bloomUpViews[i]);
        bw = std::max(1u, bw / 2);
        bh = std::max(1u, bh / 2);
    }
}

void PostProcessor::createDescriptors() {
    VkDescriptorSetLayoutBinding bindings[9] = {};
    // Bindings 0..3: image samplers used by postprocess passes.
    // Binding 3 is also reused as the cloud color input by post_composite.frag.
    for (int i = 0; i < 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // Binding 4: lens-drop SSBO (used by weather_overlay.frag for CPU/GPU modes)
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Binding 5: cloud depth input sampled by post_composite.frag.
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Binding 6: small UBO for matrices that do not fit into push constants.
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Binding 7: cloud volume accumulation (screen-space fluff occlusion in
    // alpha) sampled by weather_overlay.frag to hide crowns/splashes behind
    // cloud puffs exactly like rain particles (author feedback 2026-08-10).
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Binding 8: half-res independent-cloud-field premultiplied accumulation
    // (rgb, alpha), sampled by post_composite.frag to keep the ground/height
    // fog from washing out a cloud that already blended into the pixel — the
    // independent field draws forward with no depth write, so the fog block's
    // depth-buffer read otherwise sees only whatever's behind the cloud.
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // UPDATE_AFTER_BIND: estes sets sao reescritos por frame com as MESMAS
    // views enquanto o CB do frame anterior ainda esta' pendente - sem a flag
    // isso e' VUID-03047 (80k+ erros por sessao). Com ela, a atualizacao
    // enquanto pendente e' legal; como o conteudo escrito e' identico, nao ha'
    // corrida real de dados.
    // Flag SO' nos combined-image-samplers: UAB de UBO/SSBO exige features
    // proprias (descriptorBinding*BufferUpdateAfterBind) que nao habilitamos.
    std::vector<VkDescriptorBindingFlags> uabFlags(9, 0);
    for (uint32_t b = 0; b < 9; ++b)
        if (bindings[b].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            uabFlags[b] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo uabInfo{};
    uabInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    uabInfo.bindingCount = 9;
    uabInfo.pBindingFlags = uabFlags.data();
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 9;
    layoutInfo.pBindings = bindings;
    layoutInfo.pNext = &uabInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_descriptorSetLayout);

    uint32_t numSets = 32;
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 7 * numSets;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = numSets;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = numSets;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = numSets;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descriptorPool);

    m_descriptorSets.resize(numSets);
    std::vector<VkDescriptorSetLayout> layouts(numSets, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = numSets;
    allocInfo.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, m_descriptorSets.data());
    // Conjuntos novos (init ou resize): os buffers de clima ainda nao foram
    // escritos neles.
    m_weatherBufferDSWritten = false;

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = 256;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_descriptorSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &plInfo, nullptr, &m_layout);
}

void PostProcessor::updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1, VkImageView view2, VkImageView view3) {
    if (setIndex >= m_descriptorSets.size()) return;
    VkDescriptorSet set = m_descriptorSets[setIndex];

    VkDescriptorImageInfo infos[4] = {};
    VkWriteDescriptorSet writes[4] = {};
    uint32_t count = 0;

    auto addWrite = [&](VkImageView v, uint32_t binding) {
        if (v != VK_NULL_HANDLE) {
            infos[count].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[count].imageView = v;
            infos[count].sampler = m_linearSampler;
            writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[count].dstSet = set;
            writes[count].dstBinding = binding;
            writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].descriptorCount = 1;
            writes[count].pImageInfo = &infos[count];
            count++;
        }
    };

    addWrite(view0, 0);
    addWrite(view1, 1);
    addWrite(view2, 2);
    addWrite(view3, 3);

    if (count > 0) {
        vkUpdateDescriptorSets(m_ctx->device(), count, writes, 0, nullptr);
    }
}

void PostProcessor::updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1, VkImageView view2, VkImageView view3, VkImageView view5, VkImageView view4) {
    if (setIndex >= m_descriptorSets.size()) return;
    VkDescriptorSet set = m_descriptorSets[setIndex];

    VkDescriptorImageInfo infos[6] = {};
    VkWriteDescriptorSet writes[6] = {};
    uint32_t count = 0;

    auto addWrite = [&](VkImageView v, uint32_t binding) {
        if (v != VK_NULL_HANDLE) {
            infos[count].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[count].imageView = v;
            infos[count].sampler = m_linearSampler;
            writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[count].dstSet = set;
            writes[count].dstBinding = binding;
            writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].descriptorCount = 1;
            writes[count].pImageInfo = &infos[count];
            count++;
        }
    };

    addWrite(view0, 0);
    addWrite(view1, 1);
    addWrite(view2, 2);
    addWrite(view3, 3);
    addWrite(view5, 5);
    addWrite(view4, 8);

    if (count > 0) {
        vkUpdateDescriptorSets(m_ctx->device(), count, writes, 0, nullptr);
    }
}

VkPipeline PostProcessor::createFullscreenPipeline(const std::vector<uint32_t>& fragCode,
                                                    uint32_t w, uint32_t h,
                                                    bool needDepth,
                                                    VkFormat colorFormat,
                                                    bool enableBlend) {
    if (colorFormat == VK_FORMAT_UNDEFINED) colorFormat = postColorFormat(m_ctx->physicalDevice());
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

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicInfo{};
    dynamicInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicInfo.dynamicStateCount = 2;
    dynamicInfo.pDynamicStates = dynamicStates;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = needDepth ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = enableBlend ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
    pipeInfo.pDynamicState = &dynamicInfo;
    pipeInfo.layout = m_layout;

    VkPipeline pipeline;
    vkc::createGraphicsPipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &pipeline);

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);
    return pipeline;
}

void PostProcessor::createPipelines() {
    auto cocFrag = ShaderCompiler::loadSPIRV("postprocess/coc_map.frag.spv");
    auto blurHFrag = ShaderCompiler::loadSPIRV("postprocess/bokeh_blur_h.frag.spv");
    auto blurVFrag = ShaderCompiler::loadSPIRV("postprocess/bokeh_blur_v.frag.spv");
    auto cocCompFrag = ShaderCompiler::loadSPIRV("postprocess/coc_composite.frag.spv");
    auto compositeFrag = ShaderCompiler::loadSPIRV("postprocess/post_composite.frag.spv");
    auto copyFrag = ShaderCompiler::loadSPIRV("postprocess/debug.frag.spv");
    
    auto brightPassFrag = ShaderCompiler::loadSPIRV("postprocess/post_brightpass.frag.spv");
    auto bloomDownFrag = ShaderCompiler::loadSPIRV("postprocess/bloom_kawase_down.frag.spv");
    auto bloomUpFrag = ShaderCompiler::loadSPIRV("postprocess/bloom_kawase_up.frag.spv");
    auto weatherFrag = ShaderCompiler::loadSPIRV("postprocess/weather_overlay.frag.spv");
    auto cloudCoverageDebugFrag = ShaderCompiler::loadSPIRV("postprocess/cloud_coverage_debug.frag.spv");

    m_cocPipeline = createFullscreenPipeline(cocFrag, m_width, m_height);
    m_blurHPipeline = createFullscreenPipeline(blurHFrag, m_width, m_height);
    m_blurVPipeline = createFullscreenPipeline(blurVFrag, m_width, m_height);
    m_cocCompositePipeline = createFullscreenPipeline(cocCompFrag, m_width, m_height);
    m_compositePipeline = createFullscreenPipeline(compositeFrag, m_width, m_height);
    m_copyPipeline = createFullscreenPipeline(copyFrag, m_width, m_height, false, m_ctx->swapFormat());

    m_brightPassPipeline = createFullscreenPipeline(brightPassFrag, m_width / 2, m_height / 2);
    m_bloomDownPipeline = createFullscreenPipeline(bloomDownFrag, m_width / 2, m_height / 2);
    m_bloomUpPipeline = createFullscreenPipeline(bloomUpFrag, m_width / 2, m_height / 2);
    m_weatherPipeline = createFullscreenPipeline(weatherFrag, m_width, m_height);
    m_cloudCoverageDebugPipeline = createFullscreenPipeline(cloudCoverageDebugFrag, m_width, m_height, false, VK_FORMAT_UNDEFINED, true);
}

static void barrierToColorAttach(VkCommandBuffer cmd, VulkanContext* ctx, VkImage image) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkc::cmdPipelineBarrier2(cmd, &dep);
}

static void barrierToShaderRead(VkCommandBuffer cmd, VulkanContext* ctx, VkImage image) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkc::cmdPipelineBarrier2(cmd, &dep);
}

static void beginFullscreenPass(VkCommandBuffer cmd, VkImageView view, uint32_t w, uint32_t h) {
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
    vkc::cmdBeginRendering(cmd, &info);

    VkViewport viewport{ 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D scissor{ {0, 0}, {w, h} };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void PostProcessor::render(VkCommandBuffer cmd, VkImageView hdrInput, VkImageView depthInput,
            VkImageView gbufferWorldPos, VkImageView gbufferNormal, VkImageView shadowInput,
            const Mat4& view, const Mat4& proj, const Mat4& invViewProj,
            const Mat4& prevViewProj,
            const Vec3& cameraPos, float nearPlane, float farPlane,
            const Vec3& lightDir, const PostSettings& settings,
            const DioramaLookConfig* lookConfig,
            VkImageView targetOutput,
            float time,
            const WeatherParams* weather,
            float sunOcclusion,
            float moonOcclusion,
            VkImageView topDownDepthView,
            const Vec3& sunVisualDir,
            const Vec3& cameraDir,
            VkImageView cloudColorView,
            VkImageView cloudDepthView,
            float cloudBaseHeight,
            const Vec3& cloudWorldMin,
            const Vec3& cloudWorldMax,
            VkImageView cloudCoverageView,
            VkSampler cloudCoverageSampler,
            float cloudLayer,
            float cloudAmount,
            float weatherCoverage,
            const Vec2& cloudWindOffset,
            VkImageView cloudAltitudeView,
            VkSampler cloudAltitudeSampler,
            float cloudBottom,
            float cloudTop,
            float cloudScale,
            VkImageView cloudVolumeOccView) {
    // ERUPTION_TEST_POST_CPU=1: onde a CPU gasta dentro do post (media 120 f).
    // Motivo: em fire_smoke a fase Post da CPU ia a 17 ms/frame (0,3 em clear)
    // e o GPU total nao explicava - era preciso achar o trecho.
    static const bool kPostCpu = std::getenv("ERUPTION_TEST_POST_CPU") != nullptr;
    static double sAcc[6] = {0, 0, 0, 0, 0, 0}; static int sN = 0;
    auto sT = std::chrono::steady_clock::now();
    auto lap = [&](int k) {
        if (!kPostCpu) return;
        auto n = std::chrono::steady_clock::now();
        sAcc[k] += std::chrono::duration<double, std::milli>(n - sT).count(); sT = n;
    };
if (!m_ctx) return;
    (void)weatherCoverage; // superseded by the field-mode flag in cloudParams.z
    VkImageView compositeSource = hdrInput;

    // Histogram auto-exposure: measure this frame's HDR luminance on the GPU.
    // The CPU consumed LAST frame's adapted value at the top of the dispatch.
    float msAutoExp = 0.0f;
    if (settings.gpuAutoExposure) {
        m_ctx->writeTimestamp(cmd, kQAutoExpBegin, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        dispatchAutoExposure(cmd, hdrInput, settings.aeDt, settings.aeTau);
        m_ctx->writeTimestamp(cmd, kQAutoExpEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        msAutoExp = m_ctx->timestampDeltaMs(kQAutoExpBegin, kQAutoExpEnd);
        Profiler::addGpuTime("Auto Exposure", msAutoExp);
    }

    // Weather screen-space overlay (copy + heat shimmer + lens drops + storm tint + lightning flash)
    // Always run when weather is present so m_weatherImage contains a valid copy of the scene.
    // Runs BEFORE DoF/bloom so rain and weather effects receive blur and bloom too.
    bool weatherActive = weather != nullptr;
    if (weatherActive) {
        float cameraUnderCloud = settings.debugRainRegions ? 1.0f : (1.0f - glm::smoothstep(cloudBaseHeight - 15.0f,
                                                                                            cloudBaseHeight + 15.0f,
                                                                                            cameraPos.y));
        if (cameraUnderCloud < 0.001f) weatherActive = false;
    }
    // Parcelas em ms deste frame (na verdade do frame anterior deste slot: os
    // resultados de query sao lidos no beginFrame). Todas vem do MESMO buffer
    // de resultados, entao somam de forma consistente com o 8..9.
    float msHeightmap = 0.0f, msOverlay = 0.0f, msParticles = 0.0f, msDof = 0.0f;
    float msBloomDown = 0.0f, msBloomUp = 0.0f, msComposite = 0.0f, msCloudDbg = 0.0f;

    WeatherRenderer::RenderParams wrParams;
    // Heightmap/splash anchor: the orbit target (character), not the
    // camera — see WeatherRenderer::RenderParams::rainAnchor.
    const Vec3 rainAnchor = (m_rainAnchor.y > -1.0e8f) ? m_rainAnchor : cameraPos;
    if (weatherActive && settings.weatherTier != WeatherTier::Mobile) {
        // Build the top-down heightmap once, before both the overlay and particle passes.
        wrParams.view = view;
        wrParams.proj = proj;
        wrParams.invViewProj = invViewProj;
        wrParams.cameraPos = cameraPos;
        wrParams.rainAnchor = rainAnchor;
        // Rain slant follows the real wind (G7). This used to be
        // Vec3(rainWind, 0, 0): precipitation could only ever lean along the X
        // axis, never Z -- the same degenerate pattern WeatherSystem had. Rain
        // now leans the way the wind actually blows, and leans harder in gusts.
        if (m_weatherSystem) {
            const Vec3 wv = m_weatherSystem->wind().velocity();
            wrParams.windDirection = (glm::dot(wv, wv) > 1e-6f)
                                   ? wv
                                   : Vec3(weather->rainWind, 0.0f, 0.0f);
        } else {
            wrParams.windDirection = Vec3(weather->rainWind, 0.0f, 0.0f);
        }
        wrParams.time = time;
        wrParams.deltaTime = 0.016f;
        wrParams.nearPlane = nearPlane;
        wrParams.farPlane = farPlane;
        wrParams.depthView = topDownDepthView;
        wrParams.sceneDepthView = depthInput; // camera scene depth: soft rain occlusion
        wrParams.screenWidth = m_width;
        wrParams.screenHeight = m_height;
        wrParams.heightmapBlur = weather->heightmapBlur;
        wrParams.cloudBaseHeight = cloudBaseHeight;
        wrParams.sunDir = lightDir;
        wrParams.debugRainOcclusion = settings.debugRainOcclusion ? 1.0f : 0.0f;
        wrParams.debugRainRegions = settings.debugRainRegions ? 1.0f : 0.0f;
        wrParams.debugSplashArea = settings.debugSplashArea ? 1.0f : 0.0f;
        // wrParams doesn't need softness; it's fixed at 2.0 via shader default/push
        m_ctx->writeTimestamp(cmd, 30, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        m_weatherRenderer.updateHeightmapOnly(cmd, topDownDepthView, wrParams);
        m_ctx->writeTimestamp(cmd, 31, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        msHeightmap = m_ctx->timestampDeltaMs(30, 31);
        Profiler::addGpuTime("Rain Heightmap", msHeightmap);
        lap(0);
    }

    if (weatherActive) {
        m_ctx->writeTimestamp(cmd, 32, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        // Upload CPU-simulated drops or dispatch GPU compute update
        bool useBufferDrops = (weather->lensDropMode == LensDropMode::CpuBuffer ||
                               weather->lensDropMode == LensDropMode::GpuCompute);
        if (useBufferDrops) {
            if (weather->lensDropMode == LensDropMode::GpuCompute) {
                // GPU simulates drops directly into the SSBO.
                // ANINHADO em 32..33: nao entra na soma das partes (seria
                // contado duas vezes) - aparece so' como detalhe do overlay.
                m_ctx->writeTimestamp(cmd, kQLensDropBegin, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                dispatchLensDropCompute(cmd, *weather, time, 0.016f);
                m_ctx->writeTimestamp(cmd, kQLensDropEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                Profiler::addGpuTime("Lens Drops (compute, em Weather Overlay)",
                                     m_ctx->timestampDeltaMs(kQLensDropBegin, kQLensDropEnd));
            } else {
                // CPU path: copy simulated drops into SSBO
                const auto& drops = m_weatherSystem ? m_weatherSystem->lensDrops() : std::vector<LensDropGPU>{};
                uint32_t dropCount = glm::min(static_cast<uint32_t>(drops.size()), LENS_DROP_MAX);
                VkDeviceSize uploadSize = dropCount * sizeof(LensDropGPU);
                if (m_lensDropMapped && uploadSize > 0) {
                    std::memcpy(m_lensDropMapped, drops.data(), uploadSize);
                    // Zero remaining capacity so stale drops don't render
                    VkDeviceSize clearSize = LENS_DROP_MAX * sizeof(LensDropGPU) - uploadSize;
                    if (clearSize > 0) {
                        std::memset(static_cast<uint8_t*>(m_lensDropMapped) + uploadSize, 0, clearSize);
                    }
                }
            }
        }

        barrierToColorAttach(cmd, m_ctx, m_weatherImage);
        VkImageView heightmapView = (settings.weatherTier != WeatherTier::Mobile)
                                        ? m_weatherRenderer.heightmapView()
                                        : VK_NULL_HANDLE;
        // Crown splash box geometry (field mode): anchored to the CHARACTER,
        // displaced FORWARD along the camera look dir (XZ), sized from the
        // visible ground extent — derived from camera height and pitch (the
        // top-of-screen ground ray), NOT just orbit distance: at low pitch
        // the visible ground stretches hundreds of meters even at 100% zoom,
        // and a distance-only box left crowns missing over most of the
        // raining region (author feedback 2026-08-09: "quando movo o pitch
        // não vejo crown preenchendo a região"). 40 m half-extent stays the
        // calibrated value at ~98.6% zoom / pitch ~54 (orbit dist ~24 m).
        Vec3 rainBoxCField = m_weatherRenderer.rainBoxCenter();
        float splashHalfField = 40.0f;
        float wetRadiusField = 350.0f;
        if (m_weatherRenderer.suppressGlobalPrecip()) {
            const float camDist = glm::length(cameraPos - rainAnchor);
            Vec2 fwdXZ(cameraDir.x, cameraDir.z);
            const float fwdLen = glm::length(fwdXZ);
            if (fwdLen > 1e-4f) fwdXZ /= fwdLen; else fwdXZ = Vec2(0.0f);
            // pitch > 0 = looking down; cameraDir.y = -sin(pitch)
            const float pitch = std::asin(glm::clamp(-cameraDir.y, -1.0f, 1.0f));
            const float camH = glm::max(cameraPos.y - rainAnchor.y, 2.0f);
            const float fovY = 2.0f * std::atan(1.0f / glm::max(proj[1][1], 1e-4f));
            // Ground point at the top edge of the screen (beyond = sky).
            const float topRay = glm::max(pitch - fovY * 0.5f, glm::radians(2.0f));
            const float farXZ = camH / std::tan(topRay);
            const float anchorXZ = camDist * std::cos(pitch);
            const float farFromAnchor = glm::max(farXZ - anchorXZ, 0.0f);
            // farXZ = camH / tan(topRay) assumes the camera sits well ABOVE the
            // rain anchor (character): it estimates "how far out does the
            // top-of-screen ray hit the ground" from that height. At a modest
            // pitch with the orbit camera close to the character's own height,
            // camH collapses toward its 2 m floor and farXZ collapses with it —
            // independent of actual zoom/distance — so the estimate massively
            // UNDER-shoots the real visible ground at low pitch (author
            // feedback 2026-08-31: wet patch far too small at pitch 10). Floor
            // farFromAnchor at a fraction of the fog draw distance itself
            // (settings.fogEnd) so the wet/crown region always reaches at
            // least as far as anything is actually visible before fog takes
            // over, regardless of how degenerate the height-based estimate is.
            const float fogReach = (settings.fogEnd > 1.0f) ? settings.fogEnd * 0.9f : farXZ;
            const float farFromAnchorFloored = glm::max(farFromAnchor, fogReach - anchorXZ);
            // Ceilings on shift/splashHalf/wetRadius used to be fixed (200/600/700 m).
            // At a wide-out zoom the visible-ground math above legitimately
            // exceeds those, and the wetness falloff got clamped back down to a
            // fixed 700 m radius while the visible ground kept growing — the
            // wet/dry patch then reads as a static circle glued to the player
            // instead of covering the raining region. Scale the ceilings with
            // the same farFromAnchor driving the box size instead of a fixed cap.
            const float shift = glm::clamp(farFromAnchorFloored * 0.5f, 0.0f, glm::max(farFromAnchorFloored, 200.0f));
            rainBoxCField = rainAnchor + Vec3(fwdXZ.x * shift, 0.0f, fwdXZ.y * shift);
            splashHalfField = glm::clamp(farFromAnchorFloored - shift + 30.0f, 40.0f, glm::max(farFromAnchorFloored + 30.0f, 600.0f));
            // Wetness falloff must cover the whole crown box (it was a fixed
            // 350 m and zeroed crowns in the far field at zoom out).
            wetRadiusField = glm::clamp(shift + splashHalfField + 50.0f, 350.0f, glm::max(farFromAnchorFloored + 100.0f, 700.0f));
        }
        // Update the weather UBO: matrices plus the rain-cloud footprints used
        // to gate splash/wetness under actual rain clouds. When there are more
        // followers than slots, keep the ones nearest the character.
        if (m_weatherUboMapped) {
            auto* uboData = static_cast<WeatherOverlayUBO*>(m_weatherUboMapped);
            uboData->invViewProj = invViewProj;
            const auto& followers = m_weatherRenderer.rainFollowers();
            // Scratch estatico: alocacao por frame (varredura 2026-09-02).
            static std::vector<uint32_t> idx;
            idx.resize(followers.size());
            for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
            if (idx.size() > SPLASH_CLOUD_MAX) {
                std::partial_sort(idx.begin(), idx.begin() + SPLASH_CLOUD_MAX, idx.end(),
                                  [&](uint32_t a, uint32_t b) {
                                      const Vec3& ca = followers[a].boxCenter;
                                      const Vec3& cb = followers[b].boxCenter;
                                      float da = (ca.x - rainAnchor.x) * (ca.x - rainAnchor.x) +
                                                 (ca.z - rainAnchor.z) * (ca.z - rainAnchor.z);
                                      float db = (cb.x - rainAnchor.x) * (cb.x - rainAnchor.x) +
                                                 (cb.z - rainAnchor.z) * (cb.z - rainAnchor.z);
                                      return da < db;
                                  });
            }
            uint32_t n = 0;
            for (; n < idx.size() && n < SPLASH_CLOUD_MAX; ++n) {
                const auto& f = followers[idx[n]];
                uboData->splashClouds[n] = Vec4(f.boxCenter.x, f.boxCenter.z,
                                                f.boxSizeXZ.x * 0.5f, f.boxSizeXZ.y * 0.5f);
                // .x = cloud plane height: the overlay occludes crowns/splashes
                // when the view ray crosses a follower box at this altitude —
                // splash must hide behind clouds like a rain particle
                // (author feedback 2026-08-10). .y = rainRate (cloud
                // loadedness 0.6..1.8): scales the crown density per region.
                uboData->splashCloudsB[n] = Vec4(f.boxCenter.y, f.rainRate, 0.0f, 0.0f);
            }
            for (uint32_t i = n; i < SPLASH_CLOUD_MAX; ++i) {
                uboData->splashClouds[i] = Vec4(0.0f);
                uboData->splashCloudsB[i] = Vec4(0.0f);
            }
            // yz = character anchor XZ, w = wetness falloff radius: the
            // overlay anchors the wetness falloff there (rainBoxCenter is
            // displaced forward with the camera zoom/pitch — see above).
            uboData->splashCloudInfo = Vec4(static_cast<float>(n), rainAnchor.x, rainAnchor.z, wetRadiusField);
            // x = 1 when binding 7 carries this frame's cloud volume
            // accumulation (screen-space fluff occlusion in alpha).
            uboData->splashOccParams = Vec4(cloudVolumeOccView != VK_NULL_HANDLE ? 1.0f : 0.0f,
                                            0.0f, 0.0f, 0.0f);

            // TORNADO ANCORADO NO MUNDO. Antes o centro era
            // `cameraPos + vec3(sin(t)*40, 0, 40 + cos(t)*20)`: o funil ficava
            // SEMPRE 40 unidades a frente da camera, entao andar pelo mapa o
            // levava junto e nao havia como contorna-lo nem deixa-lo para tras.
            // Era o defeito mais visivel do efeito. Agora a base e' uma posicao
            // de MUNDO com deriva propria: nasce perto do jogador na primeira
            // vez que o clima aparece e depois anda sozinha, entao o funil fica
            // onde esta' e quem se move e' o jogador.
            {
                const bool ehTromba = weather->weatherType == WeatherType::Waterspout;
                const bool ehRajada = weather->weatherType == WeatherType::Downburst;
                // ERUPTION_TEST_TORNADO_HERE=1 (debug): planta o funil EXATAMENTE
                // na ancora da chuva (o alvo da camera), para poder olhar para
                // ele sem cacar onde caiu. Sem isto o funil nasce deslocado de
                // proposito e pode ficar fora do quadro numa captura.
                static const bool kAqui = std::getenv("ERUPTION_TEST_TORNADO_HERE") != nullptr;
                if (!m_tornadoPlaced || kAqui) {
                    m_tornadoBase = kAqui ? Vec3(rainAnchor.x, rainAnchor.y, rainAnchor.z)
                                          : Vec3(rainAnchor.x + 120.0f, rainAnchor.y, rainAnchor.z + 160.0f);
                    m_tornadoPlaced = true;
                }
                // Deriva lenta e independente da camera (metros por segundo em
                // escala de mundo). O tornado nao persegue ninguem.
                const float tt = m_heatTimeSmooth;
                m_tornadoBase.x += std::sin(tt * 0.021f) * 0.35f;
                m_tornadoBase.z += std::cos(tt * 0.017f) * 0.35f;
                // Silhueta por tipo: tromba d'agua e' alta e fina, rajada e'
                // baixa e larga, tornado fica no meio.
                const float altura   = ehTromba ? 260.0f : (ehRajada ? 90.0f : 180.0f);
                const float raioBase = ehTromba ?   5.0f : (ehRajada ? 22.0f :   9.0f);
                const float raioTopo = ehTromba ?  26.0f : (ehRajada ? 70.0f :  46.0f);
                uboData->tornadoA = Vec4(m_tornadoBase.x, m_tornadoBase.y, m_tornadoBase.z, altura);
                uboData->tornadoB = Vec4(raioBase, raioTopo, 1.6f, 0.0f);
                if (kAqui) {
                    static bool s_printed = false;
                    if (!s_printed) {
                        s_printed = true;
                        ERUPTION_LOG_WARN("[TORNADO] base=(%.1f,%.1f,%.1f) altura=%.1f raio=%.1f..%.1f cam=(%.1f,%.1f,%.1f)",
                                          m_tornadoBase.x, m_tornadoBase.y, m_tornadoBase.z, altura,
                                          raioBase, raioTopo, cameraPos.x, cameraPos.y, cameraPos.z);
                    }
                }
            }
            // ERUPTION_TEST_DUMP_SPLASH_CLOUDS=1 (debug): dump the crown-gate inputs
            // (inverse view-proj, follower footprints, crown box, wet radius)
            // so the headless screenshot harness can project the footprints to
            // screen space and statistically validate crown placement.
            if (std::getenv("ERUPTION_TEST_DUMP_SPLASH_CLOUDS")) {
                std::ofstream df("/tmp/splash_dump.txt", std::ios::trunc);
                if (df) {
                    df << "IVP";
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r) df << ' ' << invViewProj[c][r];
                    df << "\nINFO " << n << ' ' << rainAnchor.x << ' ' << rainAnchor.y << ' '
                       << rainAnchor.z << ' ' << wetRadiusField;
                    df << "\nRAINBOX " << rainBoxCField.x << ' ' << rainBoxCField.y << ' '
                       << rainBoxCField.z << ' ' << splashHalfField << '\n';
                    for (uint32_t i = 0; i < SPLASH_CLOUD_MAX; ++i) {
                        const Vec4& c = uboData->splashClouds[i];
                        if (c.z > 0.0f && c.w > 0.0f)
                            df << "CLOUD " << c.x << ' ' << c.y << ' ' << c.z << ' ' << c.w << ' '
                               << uboData->splashCloudsB[i].x << '\n';
                    }
                }
            }
        }

        // Update set 6: binding 3 receives the cloud coverage texture so the
        // overlay can mask rain/splash/lens effects by actual cloud coverage.
        {
            VkDescriptorSet set = m_descriptorSets[6];
            VkDescriptorImageInfo infos[5] = {};
            VkWriteDescriptorSet writes[8] = {};
            uint32_t count = 0;
            // PONTEIRO PENDURADO (corrigido 2026-09-02): estes dois viviam
            // DENTRO dos ifs la' embaixo, mas o vkUpdateDescriptorSets que le'
            // os seus enderecos roda DEPOIS dos blocos fecharem - a struct ja'
            // tinha saido de escopo. Em -O0 a fatia de pilha continuava
            // intacta por sorte e nada acontecia; com -O2 o compilador reusa a
            // fatia e o driver NVIDIA le' lixo: SIGSEGV dentro de
            // libnvidia-glcore, sem UM erro de validacao (o handle e' valido,
            // o CONTEUDO e' que nao existe mais). Foi o que impediu a engine
            // de rodar otimizada. Vivem no MESMO escopo do vkUpdateDescriptorSets.
            VkDescriptorBufferInfo bufferInfo{};
            VkDescriptorBufferInfo uboInfo{};

            auto addImage = [&](VkImageView v, VkSampler s, uint32_t binding) {
                if (v != VK_NULL_HANDLE) {
                    infos[count].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    infos[count].imageView = v;
                    infos[count].sampler = s != VK_NULL_HANDLE ? s : m_linearSampler;
                    writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[count].dstSet = set;
                    writes[count].dstBinding = binding;
                    writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[count].descriptorCount = 1;
                    writes[count].pImageInfo = &infos[count];
                    count++;
                }
            };

            addImage(compositeSource, m_linearSampler, 0);
            addImage(depthInput, m_linearSampler, 1);
            // The rain heightmap is R32_UINT: sampling it with a LINEAR
            // sampler is a VUID violation and returns garbage on some
            // drivers — the rain-occlusion mask (rainShadowMask) then lets
            // crowns/splash through under roofs and canopies (author feedback
            // 2026-08-10). Bind it with the NEAREST sampler instead.
            addImage(heightmapView, m_weatherRenderer.heightmapSampler(), 2);
            // Binding 3 e' sampler2DArray - nao ha' fallback 2D valido; quando
            // nao ha' coverage o shader nao amostra (gate no UBO).
            addImage(cloudCoverageView, cloudCoverageSampler, 3);
            // Binding 7: this frame's fluff occlusion for the crown/splash
            // cloud occlusion. Always written (coverage as a harmless
            // fallback) so the overlay shader never samples an unwritten
            // descriptor; splashOccParams.x gates its actual use.
            // Fallback em cadeia ate' uma view SEMPRE valida (depthInput): um
            // binding estaticamente usado sem escrita e' VUID-02699 por draw
            // (clear weather zera cloudVolumeOccView E cloudCoverageView).
            // Binding 7 e' sampler2D: o fallback tem que ser view 2D. A
            // coverage e' 2D_ARRAY - usa-la aqui era o VUID-02699 "requires
            // VIEW_TYPE_2D but got ..." em clima claro. depthInput e' 2D e
            // sempre valido; splashOccParams.x gateia o uso real.
            addImage(cloudVolumeOccView != VK_NULL_HANDLE ? cloudVolumeOccView : depthInput,
                     cloudVolumeOccView != VK_NULL_HANDLE ? cloudCoverageSampler : m_linearSampler, 7);

            // Binding 4 (storage de gotas na lente): o shader declara e usa
            // estaticamente; escrever sempre que o buffer existe, mesmo com
            // as gotas desligadas (o UBO gateia o uso real).
            // Buffers (4=SSBO gotas, 6=UBO clima) sao ESTAVEIS: escritos uma
            // vez so'. Reescrever por frame sem a feature de buffer
            // update-after-bind e' VUID-03047 (2 por frame). Os buffers de
            // dados mudam de CONTEUDO via memcpy mapeado, nao de handle.
            if (!m_weatherBufferDSWritten) {
                if (m_lensDropBuffer != VK_NULL_HANDLE) {
                    bufferInfo.buffer = m_lensDropBuffer;
                    bufferInfo.offset = 0;
                    bufferInfo.range = LENS_DROP_MAX * sizeof(LensDropGPU);
                    writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[count].dstSet = set;
                    writes[count].dstBinding = 4;
                    writes[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[count].descriptorCount = 1;
                    writes[count].pBufferInfo = &bufferInfo;
                    count++;
                }

                uboInfo.buffer = m_weatherUbo;
                uboInfo.offset = 0;
                uboInfo.range = sizeof(WeatherOverlayUBO);
                writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[count].dstSet = set;
                writes[count].dstBinding = 6;
                writes[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[count].descriptorCount = 1;
                writes[count].pBufferInfo = &uboInfo;
                count++;
                m_weatherBufferDSWritten = true;
            }

            if (count > 0) vkUpdateDescriptorSets(m_ctx->device(), count, writes, 0, nullptr);
        }
        beginFullscreenPass(cmd, m_weatherView, m_width, m_height);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_weatherPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[6], 0, nullptr);

        struct WeatherPush {
            Vec4 params;
            Vec4 params2;
            Vec4 params3;
            Vec4 params4;
            Vec4 params5;
            Vec4 params6;
            Vec4 params7;
            Vec4 params8;
            Vec4 params9;
            Vec4 screenSize;
            Vec4 cameraPos;
            Vec4 worldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeY
            Vec4 cloudWorldBounds;
            Vec4 cloudParams; // x=layer, y=threshold, z=fieldMode (1 = rain from followers), w=rainSplashAmount
            Vec4 cloudWindOffset; // x=windOffsetX, y=windOffsetY, z=debugSplashArea, w=rainCrownGain (0=1.0)
            Vec4 rainBoxCenter; // xyz = crown splash box center (character anchor displaced forward along camera dir in field mode), w = horizontal half-extent
        } wp;
        wp.params = Vec4(weather->heatShimmer, weather->heatSpeed, weather->heatScale, weather->dropLensAmount);
        // Smooth the heat-shimmer time so high-frequency noise doesn't flicker.
        float targetTime = time;
        if (weather->heatShimmer > 0.001f) {
            m_heatTimeSmooth = glm::mix(m_heatTimeSmooth, targetTime, 0.08f);
        } else {
            m_heatTimeSmooth = targetTime;
        }
        wp.params2 = Vec4(weather->heatWorldHeight, weather->stormTint, weather->lightningFlash, m_heatTimeSmooth);
        wp.params3 = Vec4(static_cast<float>(weather->lensDropMode), static_cast<float>(weather->lensDropCount),
                          static_cast<float>(settings.weatherTier), weather->lensDropRadius);
        wp.params4 = Vec4(weather->rainIntensity, weather->rainWind, weather->effectiveRainSplashIntensity(), weather->rainSplashRadius);
        wp.params5 = Vec4(weather->rainShadowSoftness, weather->fogDensity, weather->fogHeightFalloff, cloudBaseHeight);
        wp.params6 = Vec4(static_cast<float>(weather->weatherType), weather->weatherIntensity,
                          weather->windDebrisIntensity, weather->atmosphericTint);
        wp.params7 = Vec4(sunVisualDir.x, sunVisualDir.y, sunVisualDir.z, weather->rainbowIntensity);
        wp.params8 = Vec4(cameraDir.x, cameraDir.y, cameraDir.z, weather->mirageIntensity);
        wp.params9 = Vec4(weather->shootingStarIntensity, weather->tornadoIntensity,
                          weather->dustDevilIntensity, weather->auroraIntensity);
        wp.screenSize = Vec4(static_cast<float>(m_width), static_cast<float>(m_height),
                             1.0f / m_width, 1.0f / m_height);
        wp.cameraPos = Vec4(cameraPos, 0.0f);
        const float worldHalf = WeatherRenderer::HEIGHTMAP_WORLD_SIZE * 0.5f;
        wp.worldBounds = Vec4(rainAnchor.x - worldHalf, rainAnchor.z - worldHalf,
                              WeatherRenderer::HEIGHTMAP_WORLD_SIZE,
                              WeatherRenderer::HEIGHTMAP_WORLD_SIZE);
        wp.cloudWorldBounds = Vec4(cloudWorldMin.x, cloudWorldMin.z,
                                   cloudWorldMax.x - cloudWorldMin.x,
                                   cloudWorldMax.z - cloudWorldMin.z);
        // cloudParams.z: field-mode flag (1 = global precip suppressed, rain
        // comes from the follower clouds only) — the shader gates splash and
        // wetness by the follower footprints then. Was weatherCoverage, which
        // the overlay shader never used.
        wp.cloudParams = Vec4(cloudLayer, cloudAmount,
                              m_weatherRenderer.suppressGlobalPrecip() ? 1.0f : 0.0f,
                              weather->rainSplashAmount);
        wp.cloudWindOffset = Vec4(cloudWindOffset.x, cloudWindOffset.y,
                                  settings.debugSplashArea ? 1.0f : 0.0f, weather->rainCrownGain);
        // Lens-drop rain gate: the legacy check requires the camera inside the
        // FIXED test rain box (a hardcoded world point + wind drift) — anywhere
        // else on the map inRainBox=0 and lens drops never show. In field mode
        // (suppress) rain comes from the per-cloud followers instead, so center
        // the gate on the camera and scale dropLensAmount by the camera's
        // exposure to a rainy cloud's footprint (0 away from clouds -> off).
        // Lens drops on screen: DISABLED (author request 2026-08-09) — the drip
        // overlay refracts/shifts the scene and reads as the character moving.
        // Preset/slider values are ignored; only rain itself stays.
        float dropLensAmount = 0.0f;
        // ERUPTION_TEST_NO_LENS_DROPS=1 (debug): zero the lens-drop amount for A/B
        // pixel diffs of the drip overlay.
        if (std::getenv("ERUPTION_TEST_NO_LENS_DROPS")) dropLensAmount = 0.0f;
        Vec3 rainBoxC = rainBoxCField;
        float splashHalf = splashHalfField;
        if (m_weatherRenderer.suppressGlobalPrecip()) {
            dropLensAmount *= m_weatherRenderer.cameraRainExposure(cameraPos);
        }
        wp.rainBoxCenter = Vec4(rainBoxC, splashHalf); // w = horizontal half-extent of the rain box
        wp.params.w = dropLensAmount;
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(wp), &wp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
        barrierToShaderRead(cmd, m_ctx, m_weatherImage);
        m_ctx->writeTimestamp(cmd, 33, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        msOverlay = m_ctx->timestampDeltaMs(32, 33);
        Profiler::addGpuTime("Weather Overlay", msOverlay);
        compositeSource = m_weatherView;
    }

    // Weather particle pass (Medium/High tier) rendered over the overlay copy
    if (weatherActive && settings.weatherTier != WeatherTier::Mobile) {
        m_weatherRenderer.setTier(settings.weatherTier);
        m_weatherRenderer.setCloudAltitudeTexture(cloudAltitudeView, cloudAltitudeSampler);
        m_weatherRenderer.setCloudAltitudeRange(cloudBottom, cloudTop);
        m_weatherRenderer.setCloudScale(cloudScale);

        barrierToColorAttach(cmd, m_ctx, m_weatherImage);
        m_ctx->writeTimestamp(cmd, 34, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        m_weatherRenderer.render(cmd, compositeSource, m_weatherView, m_width, m_height, *weather, wrParams);
        m_ctx->writeTimestamp(cmd, 35, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        msParticles = m_ctx->timestampDeltaMs(34, 35);
        Profiler::addGpuTime("Weather Particles", msParticles);
        lap(1);
        barrierToShaderRead(cmd, m_ctx, m_weatherImage);
        compositeSource = m_weatherView;
    }

    // DoF and bloom below read through hdrInput: rebind it to the weathered
    // scene so rain receives blur and bloom like everything else.
    hdrInput = compositeSource;

    // Cronometro do DoF DE VERDADE (40..41). O bracket 8..9 da Engine, que
    // levava este nome, envolve o post INTEIRO (overlay de clima, particulas,
    // heightmap de chuva, bloom, composite): na chuva marcava 7-10 ms e parecia
    // que o DoF explodia, quando era o overlay + particulas.
    m_ctx->writeTimestamp(cmd, 40, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (settings.enableDoF) {
        // CAMINHO UNICO DE DESFOQUE (Circle of Confusion).
        // O ramo legado (tilt-shift por profundidade) vivia aqui num
        // if/else e foi apagado: era a mesma ideia em versao pobre.
        // New CoC Pipeline
        
        // Pass 1: CoC Map Generation
        barrierToColorAttach(cmd, m_ctx, m_cocImage);
        updateDescriptorSet(1, depthInput);
        beginFullscreenPass(cmd, m_cocView, m_width, m_height);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cocPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[1], 0, nullptr);
        struct CoCPush {
            float focal_distance;
            float aperture;
            float max_blur;
            float zoom_factor;
            float near_plane;
            float far_plane;
            int enable_foreground;
        } cocPush;
        cocPush.focal_distance = settings.cocFocalDistance;
        cocPush.aperture = settings.cocAperture * 0.05f;
        cocPush.max_blur = settings.cocMaxBlur;
        cocPush.zoom_factor = settings.cocZoomFactor;
        cocPush.near_plane = nearPlane;
        cocPush.far_plane = farPlane;
        cocPush.enable_foreground = settings.cocEnableForeground ? 1 : 0;
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cocPush), &cocPush);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
        barrierToShaderRead(cmd, m_ctx, m_cocImage);

        // Pass 2a: Horizontal Blur
        barrierToColorAttach(cmd, m_ctx, m_blurHImage);
        updateDescriptorSet(2, hdrInput, m_cocView);
        beginFullscreenPass(cmd, m_blurHView, m_width, m_height);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurHPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[2], 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
        barrierToShaderRead(cmd, m_ctx, m_blurHImage);

        // Pass 2b: Vertical Blur
        barrierToColorAttach(cmd, m_ctx, m_blurVImage);
        updateDescriptorSet(3, m_blurHView, m_cocView);
        beginFullscreenPass(cmd, m_blurVView, m_width, m_height);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurVPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[3], 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
        barrierToShaderRead(cmd, m_ctx, m_blurVImage);

        // Pass 3: Composite
        barrierToColorAttach(cmd, m_ctx, m_dofImage);
        updateDescriptorSet(4, hdrInput, m_blurVView, m_cocView);
        beginFullscreenPass(cmd, m_dofView, m_width, m_height);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cocCompositePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[4], 0, nullptr);
        
        struct CoCCompPush {
            float max_blur;
            float visualize_coc;
        } compPush;
        compPush.max_blur = settings.cocMaxBlur;
        compPush.visualize_coc = settings.visualizeCoC ? 1.0f : 0.0f;
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(compPush), &compPush);
        
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
        barrierToShaderRead(cmd, m_ctx, m_dofImage);
        compositeSource = m_dofView;
    }
    m_ctx->writeTimestamp(cmd, 41, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    msDof = m_ctx->timestampDeltaMs(40, 41);
    // O rotulo do passe segue "DoF / Tilt-Shift" DE PROPOSITO, mesmo depois
    // de o tilt-shift ter sido apagado: perf/breakdown_lib.py,
    // test_overdraw.py, test_shader_cost.py, os thresholds e as baselines
    // gravadas indexam por esta string. Renomear aqui invalida baseline de
    // outro agente sem ganho nenhum.
    Profiler::addGpuTime("DoF / Tilt-Shift", msDof);
    lap(2);

    // Final Composite pass
    VkImageView compositeTarget = (targetOutput != VK_NULL_HANDLE) ? targetOutput : m_outputView;
    if (targetOutput == VK_NULL_HANDLE) {
        barrierToColorAttach(cmd, m_ctx, m_outputImage);
    }

    // --- Bloom Pass ---
    VkImageView bloomResult = VK_NULL_HANDLE;
    static const bool kNoBloom = std::getenv("ERUPTION_TEST_NO_BLOOM") != nullptr;
    if (settings.enableBloom && !kNoBloom) {
        // 1. Bright Pass
        m_ctx->writeTimestamp(cmd, kQBloomDownBegin, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        barrierToColorAttach(cmd, m_ctx, m_bloomDownImages[0]);
        updateDescriptorSet(10, hdrInput);
        beginFullscreenPass(cmd, m_bloomDownViews[0], m_width / 2, m_height / 2);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_brightPassPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[10], 0, nullptr);
        float threshold = settings.bloomThreshold;
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &threshold);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
        barrierToShaderRead(cmd, m_ctx, m_bloomDownImages[0]);

        // 2. Downsample Pyramid
        for (uint32_t i = 1; i < BLOOM_MIPS; i++) {
            barrierToColorAttach(cmd, m_ctx, m_bloomDownImages[i]);
            updateDescriptorSet(10 + i, m_bloomDownViews[i - 1]);
            uint32_t bw = std::max(1u, (m_width / 2) >> i);
            uint32_t bh = std::max(1u, (m_height / 2) >> i);
            beginFullscreenPass(cmd, m_bloomDownViews[i], bw, bh);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomDownPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[10 + i], 0, nullptr);
            
            struct { Vec2 size; float lod; float pad; } push;
            push.size = Vec2((float)std::max(1u, (m_width / 2) >> (i - 1)), (float)std::max(1u, (m_height / 2) >> (i - 1)));
            vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkc::cmdEndRendering(cmd);
            barrierToShaderRead(cmd, m_ctx, m_bloomDownImages[i]);
        }

        // Fim do bright pass + piramide de downsample. Separado do upsample
        // porque os dois tem custo bem diferente (o down le' em full-res no
        // primeiro nivel; o up faz BLOOM_MIPS-1 blends com tent filter).
        m_ctx->writeTimestamp(cmd, kQBloomDownEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        msBloomDown = m_ctx->timestampDeltaMs(kQBloomDownBegin, kQBloomDownEnd);
        Profiler::addGpuTime("Bloom Down", msBloomDown);

        // 3. Upsample Pyramid
        // Start from the smallest mip
        m_ctx->writeTimestamp(cmd, kQBloomUpBegin, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VkImageView lastView = m_bloomDownViews[BLOOM_MIPS - 1];
        for (int i = BLOOM_MIPS - 2; i >= 0; i--) {
            barrierToColorAttach(cmd, m_ctx, m_bloomUpImages[i]);
            updateDescriptorSet(20 + i, lastView, m_bloomDownViews[i]);
            uint32_t bw = std::max(1u, (m_width / 2) >> i);
            uint32_t bh = std::max(1u, (m_height / 2) >> i);
            beginFullscreenPass(cmd, m_bloomUpViews[i], bw, bh);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomUpPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[20 + i], 0, nullptr);
            
            struct { Vec2 size; float lod; float pad; } push;
            push.size = Vec2((float)std::max(1u, (m_width / 2) >> (i + 1)), (float)std::max(1u, (m_height / 2) >> (i + 1)));
            vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkc::cmdEndRendering(cmd);
            barrierToShaderRead(cmd, m_ctx, m_bloomUpImages[i]);
            lastView = m_bloomUpViews[i];
        }
        m_ctx->writeTimestamp(cmd, kQBloomUpEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        msBloomUp = m_ctx->timestampDeltaMs(kQBloomUpBegin, kQBloomUpEnd);
        Profiler::addGpuTime("Bloom Up", msBloomUp);
        bloomResult = m_bloomUpViews[0];
    }

    // Binding 8 must always get a write before the very first composite draw
    // (an unwritten combined-image-sampler is a validation error, unlike a
    // stale one) - fall back to the flat cloud coverage texture, same as
    // binding 7's fallback below, until the volumetric field is ready.
    m_ctx->writeTimestamp(cmd, kQCompositeBegin, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    updateDescriptorSet(5, compositeSource, depthInput, bloomResult, cloudColorView, cloudDepthView,
                        cloudVolumeOccView != VK_NULL_HANDLE ? cloudVolumeOccView : cloudColorView);
    beginFullscreenPass(cmd, compositeTarget, m_width, m_height);
    lap(3);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[5], 0, nullptr);

    struct CompositePush {
        Vec4 exposureBloom; // x=exposure, y=fogOpacity, z=bloomIntensity, w=enableBloom
        Vec4 fogParams;
        // Era `screenSize`: o post_composite.frag declarava e NUNCA lia. Este
        // push constant esta' EXATAMENTE no teto de 256 B (8 vec4 + 2 mat4),
        // entao esses 16 B mortos eram o unico espaco disponivel.
        // xyz = direcao PARA o sol, w = anisotropia g de Henyey-Greenstein.
        Vec4 sunFogParams;
        Vec4 cameraPos;
        Mat4 invViewProj;
        Mat4 prevViewProj;
        Vec4 motionBlurParams; // x=enable, y=amount
        Vec4 chromaticAberrationParams; // x=enable, y=amount
        Vec4 fogHeightParams; // x=fogHeight, y=fogHeightFalloff, z=enableHeightFog
        Vec4 gradingParams;   // x=saturacao, y=contraste, zw=livre
    } push;
    // Debug/test overrides for fog parameters (useful for headless screenshots).
    float fogStart = settings.fogStart;
    float fogEnd = settings.fogEnd;
    float fogOpacity = settings.fogOpacity;
    Vec3 fogColor = settings.fogColor;
    if (const char* e = std::getenv("ERUPTION_TEST_FOG_START")) fogStart = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_FOG_END")) fogEnd = std::stof(e);
    if (const char* e = std::getenv("ERUPTION_TEST_FOG_OPACITY")) fogOpacity = std::stof(e);

    push.exposureBloom = Vec4(settings.exposure, fogOpacity, settings.bloomIntensity, settings.enableBloom ? 1.0f : 0.0f);
    push.fogParams = Vec4(fogColor, settings.enableFog ? fogEnd : 0.0f);
    // Nevoa com dependencia angular: aerossol espalha para FRENTE, entao a
    // nevoa contra o sol e' muito mais clara que a nevoa de costas pra ele.
    // Gota de chuva e' particula grande e a atmosfera fica mais multiplamente
    // espalhada - o resultado LE como mais isotropico, entao g cai com a
    // chuva. g=0 devolve exatamente a nevoa antiga.
    const float rainy = weather ? std::clamp(weather->rainIntensity, 0.0f, 1.0f) : 0.0f;
    const float hgG = glm::mix(0.65f, 0.30f, rainy);
    push.sunFogParams = Vec4(glm::normalize(-lightDir), hgG);
    push.cameraPos = Vec4(cameraPos, fogStart);
    push.invViewProj = invViewProj;
    push.prevViewProj = prevViewProj;
    // z carrega o modo de tone mapping (ver applyToneMapping no composite).
    push.motionBlurParams = Vec4(settings.enableMotionBlur ? 1.0f : 0.0f, settings.motionBlurAmount,
                                 static_cast<float>(settings.toneMapMode), 0.0f);
    bool caEnabled = settings.enableChromaticAberration;
    if (std::getenv("ERUPTION_TEST_NO_CA") != nullptr) caEnabled = false;
    static const bool cloudOccDebug = std::getenv("ERUPTION_TEST_CLOUD_OCC_DEBUG") != nullptr;
    push.chromaticAberrationParams = Vec4(caEnabled ? 1.0f : 0.0f, settings.chromaticAberrationAmount, 0.0f, cloudOccDebug ? 1.0f : 0.0f);
    // ERUPTION_TEST_FOG_DEBUG=1: paint the sky-haze fogFactor as pure magenta
    // (w=1) so a headless screenshot can be analysed for hard edges / abrupt
    // cuts vs a smooth fade, instead of eyeballing it.
    static const bool fogDebugViz = std::getenv("ERUPTION_TEST_FOG_DEBUG") != nullptr;
    push.fogHeightParams = Vec4(settings.fogHeight, settings.fogHeightFalloff, settings.enableFog ? 1.0f : 0.0f, fogDebugViz ? 1.0f : 0.0f);

    // LOOKCONFIG DEIXA DE SER CODIGO MORTO.
    //
    // `lookConfig` chegava aqui como parametro e NUNCA era dereferenciado: o
    // preset era lido do disco, preenchido e jogado fora. Saturacao e contraste
    // sao os dois campos dele que o motor de fato nao tinha em lugar nenhum
    // (post_composite so' tinha o operador de tonemap, que e' outra coisa), e
    // sao justamente metade do que faz uma cena ler como maquete pintada.
    // Neutro quando nao ha' preset, para nao alterar quem nao usa.
    push.gradingParams = lookConfig
        ? Vec4(lookConfig->saturation, lookConfig->contrast, 0.0f, 0.0f)
        : Vec4(1.0f, 1.0f, 0.0f, 0.0f);
    // .z era o primeiro campo livre do push (ele esta' EXATAMENTE no teto de
    // 256 B): sinaliza visualizacao crua de G-buffer, que faz o composite sair
    // antes de fog/tonemap/grading. ERUPTION_TEST_PBR_DEBUG=<mask> liga sem UI.
    static const bool kPbrDebugEnv = [] {
        const char* e = std::getenv("ERUPTION_TEST_PBR_DEBUG");
        return e && (std::atoi(e) & 31) != 0;
    }();
    if (settings.pbrDebugActive || kPbrDebugEnv) push.gradingParams.z = 1.0f;
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkc::cmdEndRendering(cmd);
    // O composite e' UM draw fullscreen que faz tonemap + color grading +
    // aberracao cromatica + motion blur + vinheta + fog + bloom blend. Nao da'
    // para separar esses efeitos por timestamp (sao ramos do mesmo shader);
    // para isolar um deles use as env vars de A/B (ERUPTION_TEST_NO_CA,
    // ERUPTION_TEST_NO_BLOOM, ...) - e' o que perf/test_shader_cost.py faz.
    m_ctx->writeTimestamp(cmd, kQCompositeEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    msComposite = m_ctx->timestampDeltaMs(kQCompositeBegin, kQCompositeEnd);
    Profiler::addGpuTime("Composite", msComposite);

    // Debug overlay: show the raw half-res cloud ray-march result on top of
    // the frame. This is useful to verify that CloudLayerRenderer is producing
    // output independent of the depth-aware composite.
    if (settings.showCloudCoverageDebug && cloudColorView != VK_NULL_HANDLE && gbufferWorldPos != VK_NULL_HANDLE) {
        m_ctx->writeTimestamp(cmd, kQCloudDbgBegin, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        // Ensure the composite pass writes are visible before we blend on top.
        if (targetOutput == VK_NULL_HANDLE) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.image = m_outputImage;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkc::cmdPipelineBarrier2(cmd, &dep);
        }

        updateDescriptorSet(7, gbufferWorldPos, cloudColorView);
        beginFullscreenPass(cmd, compositeTarget, m_width, m_height);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudCoverageDebugPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_descriptorSets[7], 0, nullptr);

        struct {
            Vec2 worldMin;
            Vec2 worldMax;
            float blend;
            float pad;
        } dbgPush;
        dbgPush.worldMin = Vec2(cloudWorldMin.x, cloudWorldMin.z);
        dbgPush.worldMax = Vec2(cloudWorldMax.x, cloudWorldMax.z);
        dbgPush.blend = 0.85f;
        dbgPush.pad = 0.0f;
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dbgPush), &dbgPush);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkc::cmdEndRendering(cmd);
    }
    if (settings.showCloudCoverageDebug && cloudColorView != VK_NULL_HANDLE && gbufferWorldPos != VK_NULL_HANDLE) {
        m_ctx->writeTimestamp(cmd, kQCloudDbgEnd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        msCloudDbg = m_ctx->timestampDeltaMs(kQCloudDbgBegin, kQCloudDbgEnd);
        Profiler::addGpuTime("Cloud Debug Overlay", msCloudDbg);
    }

    // ------------------------------------------------------------------
    // CONFERENCIA: a soma das partes tem que bater com o "Post Total" (8..9).
    //
    // Todas as leituras acima vem do MESMO buffer de resultados (lido uma vez
    // no beginFrame), entao comparar com o 8..9 e' legitimo - nao ha' mistura
    // de frames. "Lens Drops" fica de fora porque esta' ANINHADO no overlay.
    //
    // O residuo vira um passe visivel no Profiler/telemetria em vez de sumir:
    // se "Post Unattributed" cresce, ou apareceu passe novo sem cronometro,
    // ou a GPU esta' sobrepondo passes (timestamps de BOTTOM_OF_PIPE nao sao
    // exclusivos entre si).
    const float msPostTotal = m_ctx->timestampDeltaMs(kQPostTotalBegin, kQPostTotalEnd);
    const float msParts = msAutoExp + msHeightmap + msOverlay + msParticles + msDof +
                          msBloomDown + msBloomUp + msComposite + msCloudDbg;
    if (msPostTotal > 0.0f) {
        Profiler::addGpuTime("Post Unattributed", std::max(0.0f, msPostTotal - msParts));
        const float diff = std::fabs(msPostTotal - msParts);
        // Aviso limitado a 1 a cada 600 frames (~10 s): divergencia persistente
        // avisa, mas nao inunda o log. Piso de 0,05 ms evita alarme em frames
        // triviais, onde 10% de quase-nada e' ruido de quantizacao do timestamp.
        static uint32_t sWarnCooldown = 0;
        if (sWarnCooldown > 0) --sWarnCooldown;
        if (msPostTotal > 0.05f && diff > 0.10f * msPostTotal && sWarnCooldown == 0) {
            sWarnCooldown = 600;
            ERUPTION_LOG_WARN("[POST] soma das partes nao bate com o total: total=%.3f ms "
                              "partes=%.3f ms (%.0f%%) | autoexp=%.3f heightmap=%.3f overlay=%.3f "
                              "particulas=%.3f dof=%.3f bloom_down=%.3f bloom_up=%.3f composite=%.3f "
                              "clouddbg=%.3f",
                              msPostTotal, msParts, 100.0f * msParts / msPostTotal,
                              msAutoExp, msHeightmap, msOverlay, msParticles, msDof,
                              msBloomDown, msBloomUp, msComposite, msCloudDbg);
        }
    }

    lap(4);
    if (kPostCpu && ++sN % 120 == 0) {
        ERUPTION_LOG_WARN("[POSTCPU] heightmap=%.2f weather=%.2f dof=%.2f bloom=%.2f composite+resto=%.2f (ms/frame)",
                          sAcc[0]/120, sAcc[1]/120, sAcc[2]/120, sAcc[3]/120, sAcc[4]/120);
        for (double& a : sAcc) a = 0;
    }
}

void PostProcessor::PostSettings::loadFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    exposure = j.value("exposure", exposure);
    enableFog = j.value("enable_fog", enableFog);
    fogStart = j.value("fog_start", fogStart);
    fogEnd = j.value("fog_end", fogEnd);
    fogOpacity = j.value("fog_opacity", fogOpacity);
    fogHeight = j.value("fog_height", fogHeight);
    fogHeightFalloff = j.value("fog_height_falloff", fogHeightFalloff);
    if (j.contains("fog_color") && j["fog_color"].is_array() && j["fog_color"].size() >= 3) {
        fogColor = Vec3(j["fog_color"][0], j["fog_color"][1], j["fog_color"][2]);
    }
    enableDoF = j.value("enable_dof", j.value("enable_tilt_shift", enableDoF));
    focalOffset = j.value("focal_offset", focalOffset);
    focalDistance = j.value("focal_distance", focalDistance);
    focalRange = j.value("focal_range", focalRange);
    dofAmount = j.value("dof_amount", dofAmount);
    foregroundBlurAmount = j.value("foreground_blur_amount", foregroundBlurAmount);
    foregroundBleed = j.value("foreground_bleed", foregroundBleed);
    radialStretch = j.value("radial_stretch", radialStretch);
    cocFocalDistance = j.value("coc_focal_distance", cocFocalDistance);
    cocAperture = j.value("coc_aperture", cocAperture);
    cocMaxBlur = j.value("coc_max_blur", cocMaxBlur);
    // Overrides de bancada (varredura de valores de DoF sem editar o json,
    // que e' compartilhado): ERUPTION_TEST_COC_FOCAL / _APERTURE / _MAXBLUR.
    if (const char* e = std::getenv("ERUPTION_TEST_COC_FOCAL")) cocFocalDistance = std::strtof(e, nullptr);
    if (const char* e = std::getenv("ERUPTION_TEST_COC_APERTURE")) cocAperture = std::strtof(e, nullptr);
    if (const char* e = std::getenv("ERUPTION_TEST_COC_MAXBLUR")) cocMaxBlur = std::strtof(e, nullptr);
    cocZoomFactor = j.value("coc_zoom_factor", cocZoomFactor);
    cocEnableForeground = j.value("coc_enable_foreground", cocEnableForeground);
    visualizeCoC = j.value("visualize_coc", visualizeCoC);
    debugRainOcclusion = j.value("debug_rain_occlusion", debugRainOcclusion);
    debugRainRegions = j.value("debug_rain_regions", debugRainRegions);
    debugSplashArea = j.value("debug_splash_area", debugSplashArea);
    cocEnableZoomMapping = j.value("coc_enable_zoom_mapping", cocEnableZoomMapping);
    if (j.contains("coc_focal_curve") && j["coc_focal_curve"].contains("points")) {
        cocFocalCurve = SplineCurve::fromJson(j["coc_focal_curve"]["points"]);
    }
    if (j.contains("coc_aperture_curve") && j["coc_aperture_curve"].contains("points")) {
        cocApertureCurve = SplineCurve::fromJson(j["coc_aperture_curve"]["points"]);
    }
    bokehThreshold = j.value("bokeh_threshold", bokehThreshold);
    bokehIntensity = j.value("bokeh_intensity", bokehIntensity);

    toneMapMode = j.value("tone_map_mode", toneMapMode);
    enableBloom = j.value("enable_bloom", enableBloom);
    bloomIntensity = j.value("bloom_intensity", bloomIntensity);
    bloomThreshold = j.value("bloom_threshold", bloomThreshold);

    cocEnableAdaptiveFocal = j.value("coc_enable_adaptive_focal", cocEnableAdaptiveFocal);
    std::string afMethodStr = j.value("coc_adaptive_focal_method", "MultiRayGrid");
    if (afMethodStr == "SingleRay") cocAdaptiveFocalMethod = AdaptiveFocalMethod::SingleRay;
    else if (afMethodStr == "ScreenSpace") cocAdaptiveFocalMethod = AdaptiveFocalMethod::ScreenSpace;
    else if (afMethodStr == "AngularRadius") cocAdaptiveFocalMethod = AdaptiveFocalMethod::AngularRadius;
    else cocAdaptiveFocalMethod = AdaptiveFocalMethod::MultiRayGrid;
    cocAdaptiveFocalDamping = j.value("coc_adaptive_focal_damping", cocAdaptiveFocalDamping);
    
    enableMotionBlur = j.value("enable_motion_blur", enableMotionBlur);
    motionBlurAmount = j.value("motion_blur_amount", motionBlurAmount);

    enableChromaticAberration = j.value("enable_chromatic_aberration", enableChromaticAberration);
    chromaticAberrationAmount = j.value("chromatic_aberration_amount", chromaticAberrationAmount);
}


// Weather UBO for data that does not fit into push constants.
void PostProcessor::createWeatherUbo() {
    VkDeviceSize size = sizeof(WeatherOverlayUBO);
    bool ok = m_ctx->createBuffer(size,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  VMA_MEMORY_USAGE_CPU_TO_GPU,
                                  m_weatherUbo, m_weatherUboAlloc);
    if (!ok) {
        Logger::error("PostProcessor: failed to create weather UBO");
        return;
    }
    vmaMapMemory(m_ctx->allocator(), m_weatherUboAlloc, &m_weatherUboMapped);
}

void PostProcessor::destroyWeatherUbo() {
    if (m_weatherUboMapped) {
        vmaUnmapMemory(m_ctx->allocator(), m_weatherUboAlloc);
        m_weatherUboMapped = nullptr;
    }
    if (m_weatherUbo != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_weatherUbo, m_weatherUboAlloc);
        m_weatherUbo = VK_NULL_HANDLE;
        m_weatherUboAlloc = VK_NULL_HANDLE;
    }
}

// Lens-drop buffer methods
void PostProcessor::createLensDropBuffer() {
    VkDeviceSize size = LENS_DROP_MAX * sizeof(LensDropGPU);
    bool ok = m_ctx->createBuffer(size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_CPU_TO_GPU,
                                  m_lensDropBuffer, m_lensDropAlloc);
    if (!ok) {
        Logger::error("PostProcessor: failed to create lens-drop SSBO");
        return;
    }
    vmaMapMemory(m_ctx->allocator(), m_lensDropAlloc, &m_lensDropMapped);
}

void PostProcessor::destroyLensDropBuffer() {
    if (m_lensDropMapped) {
        vmaUnmapMemory(m_ctx->allocator(), m_lensDropAlloc);
        m_lensDropMapped = nullptr;
    }
    if (m_lensDropBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_ctx->allocator(), m_lensDropBuffer, m_lensDropAlloc);
        m_lensDropBuffer = VK_NULL_HANDLE;
        m_lensDropAlloc = VK_NULL_HANDLE;
    }
}

void PostProcessor::createLensDropCompute() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_lensDropComputeLayoutDesc);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_lensDropComputePool);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_lensDropComputePool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_lensDropComputeLayoutDesc;
    vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_lensDropComputeSet);

    // Bind SSBO to compute descriptor set
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_lensDropBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = LENS_DROP_MAX * sizeof(LensDropGPU);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_lensDropComputeSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = 32;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_lensDropComputeLayoutDesc;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &plInfo, nullptr, &m_lensDropComputeLayout);

    auto compCode = ShaderCompiler::loadSPIRV("postprocess/lens_drops_update.comp.spv");
    if (compCode.empty()) {
        Logger::warning("PostProcessor: lens_drops_update.comp.spv not found; GPU compute mode falls back to CPU");
        return;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = compCode.size() * sizeof(uint32_t);
    smInfo.pCode = compCode.data();
    VkShaderModule module;
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &module);

    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeInfo.stage.module = module;
    pipeInfo.stage.pName = "main";
    pipeInfo.layout = m_lensDropComputeLayout;
    vkCreateComputePipelines(m_ctx->device(), m_ctx->pipelineCache(), 1, &pipeInfo, nullptr, &m_lensDropComputePipeline);

    vkDestroyShaderModule(m_ctx->device(), module, nullptr);
}

void PostProcessor::dispatchLensDropCompute(VkCommandBuffer cmd, const WeatherParams& weather, float time, float dt) {
    if (m_lensDropComputePipeline == VK_NULL_HANDLE || m_lensDropBuffer == VK_NULL_HANDLE) return;

    // Initialize SSBO on first GPU-compute use if it contains no valid drops
    static bool s_initialized = false;
    if (!s_initialized && m_lensDropMapped) {
        std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        LensDropGPU* drops = static_cast<LensDropGPU*>(m_lensDropMapped);
        int count = glm::min(weather.lensDropCount, static_cast<int>(LENS_DROP_MAX));
        for (int i = 0; i < count; ++i) {
            drops[i].posX = u01(rng);
            drops[i].posY = -0.1f - u01(rng) * 0.1f; // UV: y=0 top, y=1 bottom
            drops[i].radius = 0.008f + u01(rng) * 0.020f;
            drops[i].intensity = 0.3f + u01(rng) * 0.7f;
        }
        s_initialized = true;
    }

    struct Push {
        Vec4 params;  // amount, storm, wind, time
        Vec4 params2; // dt, count, radiusScale, 0
    } pc;
    pc.params = Vec4(0.0f, weather.stormTint, weather.rainWind, time); // lens drops disabled (author request 2026-08-09)
    pc.params2 = Vec4(dt, static_cast<float>(weather.lensDropCount), weather.lensDropRadius, 0.0f);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lensDropComputePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lensDropComputeLayout, 0, 1, &m_lensDropComputeSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_lensDropComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t count = glm::min(static_cast<uint32_t>(weather.lensDropCount), LENS_DROP_MAX);
    uint32_t groups = (count + 63) / 64;
    vkCmdDispatch(cmd, groups, 1, 1);

    // Barrier so fragment shader reads consistent data
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.buffer = m_lensDropBuffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &barrier;
    vkc::cmdPipelineBarrier2(cmd, &dep);
}

void PostProcessor::destroyLensDropCompute() {
    vkDestroyPipeline(m_ctx->device(), m_lensDropComputePipeline, nullptr);
    m_lensDropComputePipeline = VK_NULL_HANDLE;
    vkDestroyPipelineLayout(m_ctx->device(), m_lensDropComputeLayout, nullptr);
    m_lensDropComputeLayout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(m_ctx->device(), m_lensDropComputeLayoutDesc, nullptr);
    m_lensDropComputeLayoutDesc = VK_NULL_HANDLE;
    vkDestroyDescriptorPool(m_ctx->device(), m_lensDropComputePool, nullptr);
    m_lensDropComputePool = VK_NULL_HANDLE;
}

void PostProcessor::updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1, VkBuffer storageBuffer, VkDeviceSize storageRange, VkImageView view2, VkImageView view3) {
    if (setIndex >= m_descriptorSets.size()) return;
    VkDescriptorSet set = m_descriptorSets[setIndex];

    VkDescriptorImageInfo infos[4] = {};
    VkDescriptorBufferInfo bufferInfo{};
    VkWriteDescriptorSet writes[5] = {};
    uint32_t count = 0;

    auto addImageWrite = [&](VkImageView v, uint32_t binding) {
        if (v != VK_NULL_HANDLE) {
            infos[count].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[count].imageView = v;
            infos[count].sampler = m_linearSampler;
            writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[count].dstSet = set;
            writes[count].dstBinding = binding;
            writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].descriptorCount = 1;
            writes[count].pImageInfo = &infos[count];
            count++;
        }
    };

    addImageWrite(view0, 0);
    addImageWrite(view1, 1);
    addImageWrite(view2, 2);
    addImageWrite(view3, 3);

    if (storageBuffer != VK_NULL_HANDLE) {
        bufferInfo.buffer = storageBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = storageRange;
        writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[count].dstSet = set;
        writes[count].dstBinding = 4;
        writes[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[count].descriptorCount = 1;
        writes[count].pBufferInfo = &bufferInfo;
        count++;
    }

    if (count > 0) {
        vkUpdateDescriptorSets(m_ctx->device(), count, writes, 0, nullptr);
    }
}

void PostProcessor::updateDescriptorSet(uint32_t setIndex, VkImageView view0, VkImageView view1, VkImageView view2, VkImageView view3, VkSampler view3Sampler, VkBuffer storageBuffer, VkDeviceSize storageRange) {
    if (setIndex >= m_descriptorSets.size()) return;
    VkDescriptorSet set = m_descriptorSets[setIndex];

    VkDescriptorImageInfo infos[4] = {};
    VkDescriptorBufferInfo bufferInfo{};
    VkWriteDescriptorSet writes[5] = {};
    uint32_t count = 0;

    auto addImageWrite = [&](VkImageView v, uint32_t binding, VkSampler sampler = VK_NULL_HANDLE) {
        if (v != VK_NULL_HANDLE) {
            infos[count].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[count].imageView = v;
            infos[count].sampler = (sampler != VK_NULL_HANDLE) ? sampler : m_linearSampler;
            writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[count].dstSet = set;
            writes[count].dstBinding = binding;
            writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].descriptorCount = 1;
            writes[count].pImageInfo = &infos[count];
            count++;
        }
    };

    addImageWrite(view0, 0);
    addImageWrite(view1, 1);
    addImageWrite(view2, 2);
    addImageWrite(view3, 3, view3Sampler);

    if (storageBuffer != VK_NULL_HANDLE) {
        bufferInfo.buffer = storageBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = storageRange;
        writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[count].dstSet = set;
        writes[count].dstBinding = 4;
        writes[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[count].descriptorCount = 1;
        writes[count].pBufferInfo = &bufferInfo;
        count++;
    }

    if (count > 0) {
        vkUpdateDescriptorSets(m_ctx->device(), count, writes, 0, nullptr);
    }
}


// ---------------------------------------------------------------------------
// Histogram auto-exposure. Two compute dispatches per frame over the HDR lit
// image: luminance_histogram.comp bins log-luminance into 256 buckets,
// luminance_adaptation.comp reduces them to an exposure value with temporal
// smoothing (eye adaptation) and clears the histogram for the next frame.
// The exposure lives in a persistently-mapped buffer; the CPU reads last
// frame's result and feeds it into the tonemapper's exposure push constant.
// ---------------------------------------------------------------------------
void PostProcessor::createAutoExposure() {
    VkDevice device = m_ctx->device();

    // Buffers: 256-bin histogram (device-local ok, host-visible is simpler and
    // tiny) + {exposure, prevLum} readback.
    m_ctx->createBuffer(256 * sizeof(uint32_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, m_lumHistBuffer, m_lumHistAlloc);
    m_ctx->createBuffer(2 * sizeof(float),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_CPU_TO_GPU, m_lumExpBuffer, m_lumExpAlloc);
    vmaMapMemory(m_ctx->allocator(), m_lumExpAlloc, &m_lumExpMapped);
    if (m_lumExpMapped) {
        float init[2] = {1.0f, 0.18f};
        std::memcpy(m_lumExpMapped, init, sizeof(init));
        vmaFlushAllocation(m_ctx->allocator(), m_lumExpAlloc, 0, sizeof(init));
    }

    VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &si, nullptr, &m_lumSampler);

    // Histogram: binding 0 = HDR image, binding 1 = histogram SSBO.
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2; li.pBindings = b;
        vkCreateDescriptorSetLayout(device, &li, nullptr, &m_lumHistDescLayout);
    }
    // Adaptation: binding 0 = histogram SSBO, binding 1 = exposure SSBO.
    {
        VkDescriptorSetLayoutBinding b[2]{};
        for (int i = 0; i < 2; ++i) {
            b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2; li.pBindings = b;
        vkCreateDescriptorSetLayout(device, &li, nullptr, &m_lumAdaptDescLayout);
    }

    VkDescriptorPoolSize ps[2]{};
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = AE_FRAMES;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[1].descriptorCount = AE_FRAMES + 2;
    VkDescriptorPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = AE_FRAMES + 1; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    vkCreateDescriptorPool(device, &pi, nullptr, &m_lumDescPool);

    for (uint32_t i = 0; i < AE_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_lumDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_lumHistDescLayout;
        vkAllocateDescriptorSets(device, &ai, &m_lumHistSets[i]);
    }
    {
        VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_lumDescPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_lumAdaptDescLayout;
        vkAllocateDescriptorSets(device, &ai, &m_lumAdaptSet);
        VkDescriptorBufferInfo hb{m_lumHistBuffer, 0, 256 * sizeof(uint32_t)};
        VkDescriptorBufferInfo eb{m_lumExpBuffer, 0, 2 * sizeof(float)};
        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_lumAdaptSet; w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].descriptorCount = 1; w[0].pBufferInfo = &hb;
        w[1] = w[0]; w[1].dstBinding = 1; w[1].pBufferInfo = &eb;
        vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
    }

    // Pipelines
    auto makeCompute = [&](const char* spv, VkDescriptorSetLayout dl, uint32_t pushBytes,
                           VkPipelineLayout& outLayout, VkPipeline& outPipe) {
        auto code = ShaderCompiler::loadSPIRV(spv);
        if (code.empty()) { Logger::error("PostProcessor: missing %s", spv); return; }
        VkShaderModuleCreateInfo smi{}; smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = code.size() * sizeof(uint32_t); smi.pCode = code.data();
        VkShaderModule mod; vkCreateShaderModule(device, &smi, nullptr, &mod);
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1; pli.pSetLayouts = &dl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(device, &pli, nullptr, &outLayout);
        VkComputePipelineCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = mod; cpi.stage.pName = "main";
        cpi.layout = outLayout;
        vkCreateComputePipelines(device, VulkanContext::globalPipelineCache(), 1, &cpi, nullptr, &outPipe);
        vkDestroyShaderModule(device, mod, nullptr);
    };
    makeCompute("compute/luminance_histogram.comp.spv", m_lumHistDescLayout, 4 * sizeof(float),
                m_lumHistLayout, m_lumHistPipeline);
    makeCompute("compute/luminance_adaptation.comp.spv", m_lumAdaptDescLayout, 7 * sizeof(float),
                m_lumAdaptLayout, m_lumAdaptPipeline);
}

void PostProcessor::destroyAutoExposure() {
    VkDevice device = m_ctx->device();
    if (m_lumHistPipeline)   { vkDestroyPipeline(device, m_lumHistPipeline, nullptr);   m_lumHistPipeline = VK_NULL_HANDLE; }
    if (m_lumAdaptPipeline)  { vkDestroyPipeline(device, m_lumAdaptPipeline, nullptr);  m_lumAdaptPipeline = VK_NULL_HANDLE; }
    if (m_lumHistLayout)     { vkDestroyPipelineLayout(device, m_lumHistLayout, nullptr);  m_lumHistLayout = VK_NULL_HANDLE; }
    if (m_lumAdaptLayout)    { vkDestroyPipelineLayout(device, m_lumAdaptLayout, nullptr); m_lumAdaptLayout = VK_NULL_HANDLE; }
    if (m_lumHistDescLayout) { vkDestroyDescriptorSetLayout(device, m_lumHistDescLayout, nullptr); m_lumHistDescLayout = VK_NULL_HANDLE; }
    if (m_lumAdaptDescLayout){ vkDestroyDescriptorSetLayout(device, m_lumAdaptDescLayout, nullptr); m_lumAdaptDescLayout = VK_NULL_HANDLE; }
    if (m_lumDescPool)       { vkDestroyDescriptorPool(device, m_lumDescPool, nullptr); m_lumDescPool = VK_NULL_HANDLE; }
    if (m_lumSampler)        { vkDestroySampler(device, m_lumSampler, nullptr); m_lumSampler = VK_NULL_HANDLE; }
    if (m_lumExpBuffer) {
        vmaUnmapMemory(m_ctx->allocator(), m_lumExpAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_lumExpBuffer, m_lumExpAlloc);
        m_lumExpBuffer = VK_NULL_HANDLE; m_lumExpMapped = nullptr;
    }
    if (m_lumHistBuffer) {
        vmaDestroyBuffer(m_ctx->allocator(), m_lumHistBuffer, m_lumHistAlloc);
        m_lumHistBuffer = VK_NULL_HANDLE;
    }
}

void PostProcessor::dispatchAutoExposure(VkCommandBuffer cmd, VkImageView hdrView, float dt, float tau) {
    if (!m_lumHistPipeline || !m_lumAdaptPipeline || !hdrView) return;

    // Read back LAST frame's adapted exposure before recording this frame's.
    if (m_lumExpMapped) {
        vmaInvalidateAllocation(m_ctx->allocator(), m_lumExpAlloc, 0, sizeof(float));
        float e = *reinterpret_cast<float*>(m_lumExpMapped);
        if (std::isfinite(e) && e > 0.01f && e < 100.0f) {
            m_gpuExposure = e;
            m_gpuExposureValid = true;
        }
    }

    uint32_t frameIdx = m_ctx->currentFrame() % AE_FRAMES;
    VkDescriptorImageInfo ii{m_lumSampler, hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo hb{m_lumHistBuffer, 0, 256 * sizeof(uint32_t)};
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_lumHistSets[frameIdx];
    w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].descriptorCount = 1; w[0].pImageInfo = &ii;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_lumHistSets[frameIdx];
    w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].descriptorCount = 1; w[1].pBufferInfo = &hb;
    vkUpdateDescriptorSets(m_ctx->device(), 2, w, 0, nullptr);

    // Guarantee a clean histogram (adaptation clears it each frame, but frame 0
    // starts undefined; the fill is 1KB, negligible).
    vkCmdFillBuffer(cmd, m_lumHistBuffer, 0, 256 * sizeof(uint32_t), 0u);
    VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    const float minLogLum = -8.0f;
    const float logLumRange = 12.0f;

    struct { float minLogLum, logLumRange, timeDelta, pad; } histPush{minLogLum, logLumRange, dt, 0.0f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lumHistPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lumHistLayout, 0, 1,
                            &m_lumHistSets[frameIdx], 0, nullptr);
    vkCmdPushConstants(cmd, m_lumHistLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(histPush), &histPush);
    vkCmdDispatch(cmd, (m_width + 15) / 16, (m_height + 15) / 16, 1);

    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    struct { float minLogLum, logLumRange, timeDelta, tau; uint32_t numPixels; float lowP, highP; }
        adaptPush{minLogLum, logLumRange, dt, tau, m_width * m_height, 0.01f, 0.99f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lumAdaptPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lumAdaptLayout, 0, 1,
                            &m_lumAdaptSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_lumAdaptLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(adaptPush), &adaptPush);
    vkCmdDispatch(cmd, 1, 1, 1);
}

} // namespace eruption
