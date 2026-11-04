#include "renderer/ShadowRenderer.hpp"
#include "renderer/LeanFormats.hpp"
#include <chrono>
#include "utils/Profiler.hpp"
#include <cstdlib>
#include "renderer/DeferredLighting.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/TerrainRenderer.hpp"
#include "renderer/ModelRenderer.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "math/Frustum.hpp"
#include "core/Logger.hpp"
#include "formats/TerrainParser.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

bool ShadowRenderer::init(VulkanContext* ctx, BindlessDescriptor* bindless) {
    m_ctx = ctx;
    m_bindless = bindless;
    if (m_settings.atlasSize > 0) {
        m_currentAtlasSize = std::clamp<uint32_t>(
            static_cast<uint32_t>(m_settings.atlasSize), MIN_ATLAS_SIZE, 8192u);
    }
    createShadowAtlas(m_currentAtlasSize);
    createShadowSampler();
    createShadowPipeline();
    return m_atlasView != VK_NULL_HANDLE && m_shadowPipeline != VK_NULL_HANDLE;
}

void ShadowRenderer::shutdown() {
    if (m_shadowPipelineD32 != VK_NULL_HANDLE && m_shadowPipelineD32 != m_shadowPipeline)
        vkDestroyPipeline(m_ctx->device(), m_shadowPipelineD32, nullptr);
    m_shadowPipelineD32 = VK_NULL_HANDLE;
    if (m_shadowInstPipelineD32 != VK_NULL_HANDLE && m_shadowInstPipelineD32 != m_shadowInstPipeline)
        vkDestroyPipeline(m_ctx->device(), m_shadowInstPipelineD32, nullptr);
    m_shadowInstPipelineD32 = VK_NULL_HANDLE;
    if (m_shadowPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_shadowPipeline, nullptr);
        m_shadowPipeline = VK_NULL_HANDLE;
    }
    if (m_shadowLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_shadowLayout, nullptr);
        m_shadowLayout = VK_NULL_HANDLE;
    }
    if (m_shadowInstOpaquePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_shadowInstOpaquePipeline, nullptr);
        m_shadowInstOpaquePipeline = VK_NULL_HANDLE;
    }
    if (m_shadowInstPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_shadowInstPipeline, nullptr);
        m_shadowInstPipeline = VK_NULL_HANDLE;
    }
    if (m_shadowInstLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_shadowInstLayout, nullptr);
        m_shadowInstLayout = VK_NULL_HANDLE;
    }
    if (m_shadowInstSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_shadowInstSetLayout, nullptr);
        m_shadowInstSetLayout = VK_NULL_HANDLE;
    }
    if (m_shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }
    if (m_atlasView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_atlasView, nullptr);
        m_atlasView = VK_NULL_HANDLE;
    }
    if (m_atlasImage != VK_NULL_HANDLE) {
        PROFILE_VRAM_FREE(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_atlasAlloc));
        vmaDestroyImage(m_ctx->allocator(), m_atlasImage, m_atlasAlloc);
        m_atlasImage = VK_NULL_HANDLE;
    }
    if (m_nextAtlasView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_nextAtlasView, nullptr);
        m_nextAtlasView = VK_NULL_HANDLE;
    }
    if (m_nextAtlasImage != VK_NULL_HANDLE) {
        PROFILE_VRAM_FREE(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_nextAtlasAlloc));
        vmaDestroyImage(m_ctx->allocator(), m_nextAtlasImage, m_nextAtlasAlloc);
        m_nextAtlasImage = VK_NULL_HANDLE;
    }
    destroyCacheImage();
}

void ShadowRenderer::resizeAtlas(uint32_t newSize) {
    if (newSize == m_currentAtlasSize) return;
    
    m_ctx->waitIdle(); // Heavy but safe for infrequent resizing
    
    // Destroy old resources
    if (m_atlasView != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), m_atlasView, nullptr);
    if (m_atlasImage != VK_NULL_HANDLE) {
        PROFILE_VRAM_FREE(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_atlasAlloc));
        vmaDestroyImage(m_ctx->allocator(), m_atlasImage, m_atlasAlloc);
    }
    if (m_nextAtlasView != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), m_nextAtlasView, nullptr);
    if (m_nextAtlasImage != VK_NULL_HANDLE) {
        PROFILE_VRAM_FREE(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_nextAtlasAlloc));
        vmaDestroyImage(m_ctx->allocator(), m_nextAtlasImage, m_nextAtlasAlloc);
    }
    // ZERAR os handles depois de destruir. Antes ficavam pendurados e o
    // createShadowAtlas mascarava isso sobrescrevendo os quatro sem olhar;
    // com o "next" opcional, createNextAtlasImage viu um handle nao-nulo de
    // imagem ja' destruida, devolveu "ja' existe", e a view foi criada sobre
    // memoria liberada: SIGSEGV no primeiro frame com temporal blend ligado
    // (o preset grafico chama resizeAtlas no boot, 4096 -> 3072).
    m_atlasView = VK_NULL_HANDLE;      m_atlasImage = VK_NULL_HANDLE;     m_atlasAlloc = VK_NULL_HANDLE;
    m_nextAtlasView = VK_NULL_HANDLE;  m_nextAtlasImage = VK_NULL_HANDLE; m_nextAtlasAlloc = VK_NULL_HANDLE;
    destroyCacheImage();

    m_currentAtlasSize = newSize;
    createShadowAtlas(newSize);
    
    ERUPTION_LOG_INFO("Shadow Atlas resized to %ux%u", newSize, newSize);
}

void ShadowRenderer::createShadowAtlas(uint32_t size) {
    // As cascatas ficam lado a lado: a largura é size * CASCADE_COUNT e não
    // pode passar do limite do aparelho (4096 numa GPU de celular).
    const auto& lim = m_ctx->deviceProperties().limits;
    const uint32_t maxWidth = std::min(lim.maxImageDimension2D, lim.maxFramebufferWidth);
    const uint32_t maxHeight = std::min(lim.maxImageDimension2D, lim.maxFramebufferHeight);
    uint32_t currentSize = std::min({size, maxWidth / CASCADE_COUNT, maxHeight});
    VkResult res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    
    while (currentSize >= MIN_ATLAS_SIZE) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        // Cascades are laid out side by side: c0 at x=0, c1 at x=size.
        imageInfo.extent.width = currentSize * CASCADE_COUNT;
        imageInfo.extent.height = currentSize;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = lean::shadowDepthFormat();
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // TRANSFER_DST: a sombra pre-assada chega ao atlas por copia.
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        
        // O atlas "next" existe para o temporal blend (troca suave entre duas
        // cascatas), que a UI marca como "Legacy (causes artifacts - avoid)" e
        // data/shadows.json deixa desligado. Mesmo assim ele era SEMPRE
        // alocado: 72 MB a 3072 que nunca eram escritos nem lidos - 12,6% da
        // VRAM do parana_field, 3,5% de uma 930M (inventario do VMA,
        // 2026-09-11). Agora so' nasce quando o blend esta' ligado; se
        // alguem ligar em runtime, renderCascades cria na hora
        // (ensureNextAtlas). Enquanto nao existe, nextShadowAtlasView()
        // devolve a view do atlas principal como placeholder - o shader nunca
        // a amostra com shadowRanges.w == 0 (directional.frag:388), mas o
        // descriptor precisa de uma view valida.
        res = vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_atlasImage, &m_atlasAlloc, nullptr);
        if (res == VK_SUCCESS) {
            m_currentAtlasSize = currentSize;
            PROFILE_VRAM_ALLOC(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_atlasAlloc));
            if (m_settings.enableTemporalBlend && !createNextAtlasImage(currentSize)) {
                // Sem o "next" o blend nao funciona; cai para o atlas unico em
                // vez de falhar a alocacao inteira.
                ERUPTION_LOG_WARN("Atlas 'next' (temporal blend) nao alocado a %u - blend desligado", currentSize);
                m_settings.enableTemporalBlend = false;
            }
            break;
        }
        
        ERUPTION_LOG_WARN("Failed to allocate %ux%u Shadow Atlas, falling back...", currentSize, currentSize);
        currentSize >>= 1;
    }

    if (res != VK_SUCCESS) {
        ERUPTION_LOG_FATAL("Failed to allocate any Shadow Atlas!");
        return;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_atlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = lean::shadowDepthFormat();
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_atlasView);

    if (m_nextAtlasImage != VK_NULL_HANDLE) {
        viewInfo.image = m_nextAtlasImage;
        vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_nextAtlasView);
    }
}

bool ShadowRenderer::createNextAtlasImage(uint32_t size) {
    if (m_nextAtlasImage != VK_NULL_HANDLE) return true;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { size * 2, size, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = lean::shadowDepthFormat();
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo,
                       &m_nextAtlasImage, &m_nextAtlasAlloc, nullptr) != VK_SUCCESS) {
        m_nextAtlasImage = VK_NULL_HANDLE;
        m_nextAtlasAlloc = VK_NULL_HANDLE;
        return false;
    }
    PROFILE_VRAM_ALLOC(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_nextAtlasAlloc));
    return true;
}

void ShadowRenderer::ensureNextAtlas() {
    if (m_nextAtlasView != VK_NULL_HANDLE || !m_settings.enableTemporalBlend) return;
    if (!createNextAtlasImage(m_currentAtlasSize)) {
        m_settings.enableTemporalBlend = false;
        return;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_nextAtlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = lean::shadowDepthFormat();
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_nextAtlasView);
    ERUPTION_LOG_WARN("[SOMBRA] atlas 'next' criado tardiamente a %u (temporal blend ligado em runtime)", m_currentAtlasSize);
}

void ShadowRenderer::createShadowSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_shadowSampler);
}

void ShadowRenderer::createShadowPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/shadow/shadow.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/shadow/shadow.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("Failed to load shadow shaders");
        return;
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
    stages[0] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = {};
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input: full TerrainVertex layout for alpha test
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(TerrainVertex); // keep in sync with vertex layout
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // texIndex e matId aos pares (R16G16, offset alinhado a 4): um R16 sozinho
    // em offset so' par de 2 bytes faz a PowerVR de celular buscar o vertice
    // por um caminho lento (a sombra custava 700 ms).
    std::vector<VkVertexInputAttributeDescription> attribs(5);
    attribs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attribs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, texCoord)};
    attribs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, normal)};
    attribs[3] = {3, 0, VK_FORMAT_R16G16_UINT, offsetof(TerrainVertex, texIndex)}; // + matId
    attribs[4] = {5, 0, VK_FORMAT_R32_UINT, offsetof(TerrainVertex, color)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertexInput.pVertexAttributeDescriptions = attribs.data();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(Mat4);

    VkDescriptorSetLayout bindlessLayout = m_bindless ? m_bindless->layout() : VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (bindlessLayout != VK_NULL_HANDLE) {
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &bindlessLayout;
    }
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_shadowLayout);

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0;
    blend.blendEnable = VK_FALSE;

    PipelineBuilder shadowBuilder;
    shadowBuilder.setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, 2048.0f, 2048.0f) // Dynamic state will override this
        .setScissor(0, 0, 2048, 2048)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(true, true, VK_COMPARE_OP_LESS)
        .setBlendState({blend})
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_shadowLayout);
    m_shadowPipeline = shadowBuilder.setDepthAttachmentFormat(lean::shadowDepthFormat()).build(m_ctx->device());
    // Atlas em D16 (spec 50): chuva vista de cima e probes usam estas mesmas
    // pipelines nos proprios alvos D32, entao ganham a variante D32.
    m_shadowPipelineD32 = lean::shadowD16()
        ? shadowBuilder.setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT).build(m_ctx->device())
        : m_shadowPipeline;

    // ---- Variante instanciada (so' modelos) ----
    // (criada ANTES de destruir os modulos: o frag e' compartilhado)
    // ERUPTION_TEST_SHADOW_NO_INSTANCING=1 (debug): modelos pelo caminho por-draw.
    const bool kNoInstancing = std::getenv("ERUPTION_TEST_SHADOW_NO_INSTANCING") != nullptr;
    auto instVertCode = kNoInstancing ? std::vector<uint32_t>{}
                                      : ShaderCompiler::loadSPIRV("shaders/shadow/shadow_inst.vert.spv");
    if (!instVertCode.empty()) {
        VkShaderModule instVert;
        VkShaderModuleCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ivci.codeSize = instVertCode.size() * sizeof(uint32_t);
        ivci.pCode = instVertCode.data();
        if (vkCreateShaderModule(m_ctx->device(), &ivci, nullptr, &instVert) == VK_SUCCESS) {
            // Set 1: SSBO de matrizes. Layout ESTRUTURALMENTE identico ao do
            // ModelRenderer - compatibilidade de set layout no Vulkan e' por
            // descricao, entao os sets alocados la' bindam aqui.
            if (m_shadowInstSetLayout == VK_NULL_HANDLE) {
                VkDescriptorSetLayoutBinding ib{};
                ib.binding = 0;
                ib.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                ib.descriptorCount = 1;
                ib.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
                VkDescriptorSetLayoutCreateInfo ici{};
                ici.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                ici.bindingCount = 1;
                ici.pBindings = &ib;
                vkCreateDescriptorSetLayout(m_ctx->device(), &ici, nullptr, &m_shadowInstSetLayout);
            }
            // A variante instanciada carrega, alem do cascadeVP, o vetor de
            // vento (FrameUBO.windParams) e o swayAmount da malha do draw
            // atual - shadow_inst.vert usa isso pra aplicar o MESMO
            // applyWindSway do model.vert, senao a sombra de vegetacao fica
            // cravada enquanto a copa balanca no G-buffer. O passe nao-
            // instanciado (terreno, chuva top-down) nao precisa disso e
            // continua com o pcRange menor (so' o mat4).
            VkPushConstantRange pcRangeInst{};
            pcRangeInst.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            pcRangeInst.offset = 0;
            pcRangeInst.size = sizeof(Mat4) + sizeof(Vec4) + sizeof(float);

            VkDescriptorSetLayout instSets[2] = { bindlessLayout, m_shadowInstSetLayout };
            VkPipelineLayoutCreateInfo il{};
            il.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            il.setLayoutCount = (bindlessLayout != VK_NULL_HANDLE) ? 2u : 0u;
            il.pSetLayouts = instSets;
            il.pushConstantRangeCount = 1;
            il.pPushConstantRanges = &pcRangeInst;
            vkCreatePipelineLayout(m_ctx->device(), &il, nullptr, &m_shadowInstLayout);

            VkPipelineShaderStageCreateInfo instStages[2] = { stages[0], stages[1] };
            instStages[0].module = instVert;
            // Matriz da instância como atributo por instância (entrada 1, o
            // mesmo buffer do ModelRenderer), não como SSBO lido no vértice: na
            // PowerVR de celular a leitura do SSBO por gl_InstanceIndex derrubava
            // a GPU a cada quadro (715 ms contra 7 ms).
            const VkVertexInputBindingDescription instBindings[2] = {
                bindingDesc, {1, sizeof(Mat4), VK_VERTEX_INPUT_RATE_INSTANCE}};
            std::vector<VkVertexInputAttributeDescription> instAttribs = attribs;
            for (uint32_t c = 0; c < 4; ++c)
                instAttribs.push_back({6 + c, 1, VK_FORMAT_R32G32B32A32_SFLOAT, c * static_cast<uint32_t>(sizeof(Vec4))});
            VkPipelineVertexInputStateCreateInfo instVertexInput = vertexInput;
            instVertexInput.vertexBindingDescriptionCount = 2;
            instVertexInput.pVertexBindingDescriptions = instBindings;
            instVertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(instAttribs.size());
            instVertexInput.pVertexAttributeDescriptions = instAttribs.data();
            PipelineBuilder instBuilder;
            instBuilder.setShaderStages({instStages[0], instStages[1]})
                .setVertexInput(instVertexInput)
                .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                .setViewport(0, 0, 2048.0f, 2048.0f)
                .setScissor(0, 0, 2048, 2048)
                .setPolygonMode(VK_POLYGON_MODE_FILL)
                .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .setDepthState(true, true, VK_COMPARE_OP_LESS)
                .setBlendState({blend})
                .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                .setLayout(m_shadowInstLayout);
            m_shadowInstPipeline =
                instBuilder.setDepthAttachmentFormat(lean::shadowDepthFormat()).build(m_ctx->device());
            m_shadowInstPipelineD32 = lean::shadowD16()
                ? instBuilder.setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT).build(m_ctx->device())
                : m_shadowInstPipeline;
            // CASTER SOLIDO: mesma pipeline, SEM fragment shader nenhum.
            //
            // shadow.frag existe so' para o teste de alfa (texture()+discard).
            // Numa malha cujas texturas nao tem alfa ele nunca descarta nada -
            // e' custo puro, e pior: a mera presenca de `discard` tira do
            // hardware o Z antecipado do passe. Medido em parana_field: so' os
            // casters solidos custam 1,158 ms com o shader e 0,691 sem ele,
            // ou seja 0,456 ms de fragment shader que nao decide coisa
            // alguma. Vulkan aceita pipeline grafica sem estagio de fragmento
            // quando so' se escreve profundidade, que e' exatamente o caso.
            m_shadowInstOpaquePipeline = PipelineBuilder()
                .setShaderStages({instStages[0]})
                .setVertexInput(instVertexInput)
                .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                .setViewport(0, 0, 2048.0f, 2048.0f)
                .setScissor(0, 0, 2048, 2048)
                .setPolygonMode(VK_POLYGON_MODE_FILL)
                .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .setDepthState(true, true, VK_COMPARE_OP_LESS)
                .setBlendState({blend})
                .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                .setLayout(m_shadowInstLayout)
                .setDepthAttachmentFormat(lean::shadowDepthFormat())
                .build(m_ctx->device());
            if (m_shadowInstOpaquePipeline == VK_NULL_HANDLE) {
                ERUPTION_LOG_WARN("[SOMBRA] pipeline opaca indisponivel - caster solido "
                                  "continua pagando o teste de alfa");
            }
            vkDestroyShaderModule(m_ctx->device(), instVert, nullptr);
        }
    }
    if (m_shadowInstPipeline == VK_NULL_HANDLE) {
        ERUPTION_LOG_WARN("Sombra instanciada indisponivel - modelos usam o caminho por-draw");
    }
    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);
}

void ShadowRenderer::setShadowSnaps(const float* yawSnaps, const float* timeSnaps, uint32_t count) {
    m_shadowSnapCount = glm::min(count, MAX_SHADOW_SNAPS);
    for (uint32_t i = 0; i < m_shadowSnapCount; i++) {
        m_shadowYawSnaps[i] = yawSnaps[i];
        m_shadowTimeSnaps[i] = timeSnaps[i];
    }
}

void ShadowRenderer::updateCascades(const DirectionalLight& light, const Camera& camera, float zoomPercent, float timeOfDay) {
    m_zoomPercent = zoomPercent;
    Vec3 trueLightDir = glm::normalize(light.direction);
    float yaw = std::atan2(trueLightDir.z, trueLightDir.x);
    float pitch = std::asin(trueLightDir.y);

    // -----------------------------------------------------------------
    // LIGHT DIRECTION DETERMINATION
    // -----------------------------------------------------------------
    // Default: use continuous sun direction (smooth, no jumps).
    // This is the RECOMMENDED path when enablePositionalSnap is active.
    float yawFloor = yaw;
    float yawCeil = yaw;
    float blend = 0.0f;

    // Legacy angle snapping: only used if positional snap is OFF.
    // NOTE: Angle snapping + temporal blend causes massive ghosting because
    // crossfading between two shadow maps with DIFFERENT light directions
    // creates a "blur" where the shadow edges don't align.
    if (!m_settings.enablePositionalSnap && m_settings.enableAngleSnapping && m_shadowSnapCount >= 2) {
        int snapIdx = 0;
        for (uint32_t i = 0; i < m_shadowSnapCount - 1; i++) {
            if (timeOfDay >= m_shadowTimeSnaps[i] && timeOfDay < m_shadowTimeSnaps[i + 1]) {
                snapIdx = i;
                break;
            }
        }
        if (timeOfDay >= m_shadowTimeSnaps[m_shadowSnapCount - 1]) {
            snapIdx = static_cast<int>(m_shadowSnapCount - 1);
        }

        int nextIdx = (snapIdx + 1) % m_shadowSnapCount;
        yawFloor = m_shadowYawSnaps[snapIdx];
        yawCeil = m_shadowYawSnaps[nextIdx];

        float timeFloor = m_shadowTimeSnaps[snapIdx];
        float timeCeil = m_shadowTimeSnaps[nextIdx];
        float rawBlend;
        if (timeCeil > timeFloor) {
            rawBlend = (timeOfDay - timeFloor) / (timeCeil - timeFloor);
        } else {
            float range = (1.0f - timeFloor) + timeCeil;
            float elapsed = (timeOfDay >= timeFloor) ? (timeOfDay - timeFloor) : (timeOfDay + (1.0f - timeFloor));
            rawBlend = elapsed / range;
        }
        rawBlend = glm::clamp(rawBlend, 0.0f, 1.0f);
        blend = rawBlend * rawBlend * (3.0f - 2.0f * rawBlend);
    }

    if (!m_settings.enableTemporalBlend) {
        blend = 0.0f;
    }
    m_blendFactor = blend;

    float pitchReal = glm::clamp(pitch, -glm::half_pi<float>() + 0.01f, glm::half_pi<float>() - 0.01f);

    Vec3 currentLightDir = Vec3(std::cos(pitchReal) * std::cos(yawFloor), std::sin(pitchReal), std::cos(pitchReal) * std::sin(yawFloor));
    Vec3 nextLightDir = Vec3(std::cos(pitchReal) * std::cos(yawCeil), std::sin(pitchReal), std::cos(pitchReal) * std::sin(yawCeil));

    m_currentLightDir = currentLightDir;

    // -----------------------------------------------------------------
    // RANGE & CASCADE SETUP
    // -----------------------------------------------------------------
    float orbitDist = camera.orbitDistance();
    float camPitch = camera.orbitPitch();
    float tPitch = glm::clamp((glm::degrees(camPitch) - 10.0f) / (90.0f - 10.0f), 0.0f, 1.0f);

    float dynamicFarRange = 30000.0f;
    if (m_settings.enableAdaptiveShadows && !m_settings.farRangeCurve.pointsX.empty()) {
        dynamicFarRange = m_settings.farRangeCurve.evaluate(tPitch);
    } else {
        dynamicFarRange = 30000.0f * (1.0f / std::max(std::sin(camPitch), 0.2f));
    }

    float zoomFactor = glm::clamp(1.0f - m_zoomPercent, 0.0f, 1.0f);
    float dynamicRange = glm::mix(5000.0f, dynamicFarRange, zoomFactor);
    float farPlane = glm::min(camera.farPlane(), dynamicRange);
    // m_cascadeSplits/m_cascadeSplitsVec are set inside computeMatrices below
    // (c0 = legacy box half size, c1 = far plane).

    // ERUPTION_TEST_SHADOW_DEBUG=1 (debug): log effective coverage once per second.
    static const bool kShadowDebug = std::getenv("ERUPTION_TEST_SHADOW_DEBUG") != nullptr;
    float dbgOrthoL = 0.0f, dbgOrthoR = 0.0f, dbgOrthoB = 0.0f, dbgOrthoT = 0.0f;

    // -----------------------------------------------------------------
    // MATRIX COMPUTATION WITH STABILIZATION
    // -----------------------------------------------------------------
    auto computeMatrices = [&](Vec3 lDir, Mat4* renderMats, Mat4* lookupMats) {
        Vec3 up = std::abs(lDir.y) < 0.99f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);

        Vec3 camTarget = camera.target();
        Vec3 shadowCenter = Vec3(camTarget.x, 0.0f, camTarget.z);

        float lightPitchFactor = 1.0f / std::max(std::abs(lDir.y), 0.15f);
        float eyeOffset = 5000.0f * lightPitchFactor;
        float depthRange = 15000.0f * lightPitchFactor;

        // Quantize halfSize to prevent the world-texel-size from changing every frame.
        // This is critical: if halfSize varies continuously, any snapping would jump
        // by unpredictable amounts. By quantizing, texelSize stays constant for long
        // periods, and the positional snap below works flawlessly.
        // Low-pitch boost: tPitch is 0 at <=10 deg camera pitch, 1 at 90 deg;
        // the boost fades out by ~46 deg so top-down play keeps tight texels.
        // Capped so the final world texel never exceeds lowPitchMaxTexel —
        // at far zoom the c0 box is already km-wide (coarse texels), and
        // boosting it further only makes the positional snap jump in huge
        // steps without adding useful coverage.
        // MODO SINGLE: box e' funcao SO' de zoom (singleSizeCurve), nunca do
        // ray-grid fit por angulo/rotacao que gerava o antigo c1 - essa
        // dependencia de angulo era literalmente o "culling de angulo de
        // camera" que o autor via, e o estado entre frames (freeze-by-view)
        // era outra fonte propria de cintilacao. Board maior que o c0 antigo
        // porque agora cobre em TODAS as direcoes a partir do alvo (o c0
        // so' precisava do entorno imediato, pois o c1 esticava pra frente).
        float rawHalfSize = m_settings.singleMap
            ? m_settings.singleSizeCurve.evaluate(m_zoomPercent)
            : m_settings.c0SizeCurve.evaluate(m_zoomPercent);
        if (m_settings.singleMap) {
            // Orcamento de texel como teto de seguranca (o mesmo espirito do
            // antigo single_max_texel): a curva e' tunavel livremente, mas
            // nunca deixa o texel ficar mais grosso que isto.
            const float capHalf = m_settings.singleMaxTexel *
                                  static_cast<float>(m_currentAtlasSize) * 0.5f;
            rawHalfSize = std::min(rawHalfSize, capHalf);
        }
        float pitchBoost = glm::mix(m_settings.lowPitchSizeBoost, 1.0f,
                                    glm::smoothstep(0.0f, 0.45f, tPitch));
        const float texelCap = m_settings.lowPitchMaxTexel * static_cast<float>(m_currentAtlasSize) /
                               (2.0f * std::max(rawHalfSize, 1.0f));
        pitchBoost = glm::clamp(pitchBoost, 1.0f, std::max(texelCap, 1.0f));
        rawHalfSize *= pitchBoost;
        float quantizeStep = std::max(m_settings.snapQuantizeStep, 1.0f);
        float candidateHalfSize = std::floor(rawHalfSize / quantizeStep) * quantizeStep;
        candidateHalfSize = std::max(candidateHalfSize, quantizeStep);
        // Hysteresis: once a quantized box size is chosen, keep it until the raw
        // size has moved at least half a quantization step. This prevents the
        // c0/c1 boundary from oscillating frame-to-frame at pitch ~10° (where
        // the boost is most sensitive to tiny camera changes), which was causing
        // flickering black rectangles as fragments popped between cascades.
        float halfSize = candidateHalfSize;
        if (m_currentHalfSize > 0.0f &&
            std::abs(candidateHalfSize - m_currentHalfSize) < quantizeStep * 0.5f) {
            halfSize = m_currentHalfSize;
        }
        m_currentHalfSize = halfSize;

        m_currentWorldTexelSize = (halfSize * 2.0f) / static_cast<float>(m_currentAtlasSize);
        if (renderMats == m_cascadeRenderMatrices)
            m_shadowDepthRange = depthRange; // exposed to shader via shadowRanges.y

        Mat4 lightViewRaw = glm::lookAt(shadowCenter - lDir * eyeOffset, shadowCenter, up);

        // Positional Snap, DONE RIGHT: the old code rounded shadowCenter in
        // WORLD axes. But the shadow texel lattice lives in LIGHT space,
        // whose axes are rotated by the sun yaw — so a world-axis snap left
        // a sub-texel offset in light space and the shadow pattern slid
        // DIAGONALLY across the ground as the camera moved (the shimmering
        // "diagonal stripes"). The correct lock: measure the light-space
        // translation and subtract its sub-texel fraction, so the lattice
        // only ever moves in whole-texel steps.
        auto viewSnapDelta = [&](float texelX, float texelY) -> Vec2 {
            Vec3 t0 = Vec3(lightViewRaw * Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            return Vec2(t0.x - std::floor(t0.x / texelX) * texelX,
                        t0.y - std::floor(t0.y / texelY) * texelY);
        };

        Mat4 lightView0 = lightViewRaw;
        if (m_settings.enablePositionalSnap) {
            Vec2 d = viewSnapDelta(m_currentWorldTexelSize, m_currentWorldTexelSize);
            lightView0 = glm::translate(Mat4(1.0f), Vec3(-d.x, -d.y, 0.0f)) * lightViewRaw;
        }

        Mat4 biasMat(1.0f);
        biasMat[0][0] = 0.5f; biasMat[1][1] = 0.5f; biasMat[2][2] = 1.0f;
        biasMat[3][0] = 0.5f; biasMat[3][1] = 0.5f; biasMat[3][2] = 0.0f;

        if (m_settings.csmStable) {
            // ============================================================
            // CSM ESTAVEL (Valient, ShaderX6; MJP "A Sampling of Shadow
            // Techniques"). A raiz de TODA cintilacao de cascata e' o
            // tamanho/orientacao da janela mudando com a pose da camera.
            // A receita fecha as duas portas:
            //  1. ESFERA envolvente da fatia do frustum: raio e' funcao SO'
            //     de fov/aspect/splits - rodar ou andar com a camera NAO
            //     muda o tamanho da janela, nunca. (Era a peca que faltava
            //     em todas as tentativas anteriores desta engine.)
            //  2. Snap do CENTRO na grade de texel em light space (grade
            //     ancorada na origem do mundo via viewSnapDelta) - o centro
            //     so' anda em passos inteiros de texel.
            // Splits pelo "practical split" (Zhang et al.): mistura
            // uniforme/logaritmico por csmLambda.
            // ============================================================
            const float fovy = glm::radians(camera.fov());
            const float aspect = camera.aspect();
            const float tanHalf = std::tan(fovy * 0.5f);
            const float k2 = tanHalf * tanHalf * (aspect * aspect + 1.0f);

            // NEAR DA CASCATA = ONDE O CHAO COMECA, nao o near plane. A receita
            // de CSM e' de camera em primeira pessoa (near ~1 u); numa camera
            // ORBITAL a 1000 u do alvo, pitch 12, fovy 45, o chao visivel so'
            // comeca a ~340 u de profundidade - os primeiros 340 u sao ar. Com
            // sn = 1 e csmFar 1200, o practical split (lambda 0,6) punha a c0
            // em [1, 261]: a cascata FINA cobria ar puro e TUDO que o jogador
            // ve caia na c1 grossa - onde caster com < 4 texels e' descartado
            // (csmFarTexelBias). E' o "sombra pequena, acompanha a camera e
            // some" do autor (2026-09-05): a fronteira c0/c1 anda com a camera
            // e alem dela as sombras pequenas nao existem. Profundidade do
            // chao no raio de baixo do frustum: x = h/tan(pitch+fovy/2),
            // depth = x*cos(pitch) + h*sin(pitch). Quantizado com histerese
            // (mesma razao do sf adaptativo abaixo).
            float snRaw = std::max(camera.nearPlane(), 1.0f);
            // Fator do filtro exponencial pro near/far adaptativos: relogio
            // proprio porque updateCascades nao recebe dt. dt travado em
            // [0, 0.1] (pausa/carga nao pode virar salto). Com
            // ERUPTION_TEST_FIXED_DT o A/B fica deterministico.
            // A/B do snap: ERUPTION_SHADOW_FAR_QUANT=1 traz de volta o
            // caminho QUANTIZADO de antes de 7c8d64b (degraus com histerese),
            // para medir o antes/depois com UM binario so'. Nao e' caminho de
            // producao - o padrao e' 0, a suavizacao.
            static const bool kLegacyQuant = [] {
                const char* e = std::getenv("ERUPTION_SHADOW_FAR_QUANT");
                return e && std::atoi(e) != 0;
            }();
            float smoothK = 1.0f;
            if (renderMats == m_cascadeRenderMatrices) {
                static const float kFixedDt = [] {
                    const char* e = std::getenv("ERUPTION_TEST_FIXED_DT");
                    return e ? std::strtof(e, nullptr) : 0.0f;
                }();
                const double now = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                float dt = (m_csmSmoothLastTime < 0.0) ? 0.0f
                         : static_cast<float>(now - m_csmSmoothLastTime);
                m_csmSmoothLastTime = now;
                if (kFixedDt > 0.0f) dt = kFixedDt;
                dt = glm::clamp(dt, 0.0f, 0.1f);
                const float tau = std::max(m_settings.csmFarSmoothTau, 1e-3f);
                smoothK = 1.0f - std::exp(-dt / tau);
            }
            if (m_settings.csmFarAdaptive) {
                const float h = std::max(orbitDist * std::sin(std::max(camPitch, 0.0f)), 1.0f);
                const float angDown = camPitch + fovy * 0.5f;
                if (angDown < glm::radians(89.0f) && angDown > glm::radians(1.0f)) {
                    const float x = h / std::tan(angDown);
                    const float depth = x * std::cos(camPitch) + h * std::sin(camPitch);
                    const float nearGround = depth * 0.9f;
                    // Suavizacao continua (ver o far abaixo): sem degrau.
                    float cand;
                    if (kLegacyQuant) {
                        const float step = 100.0f;
                        cand = m_csmNearCurrent;
                        if (m_csmNearCurrent <= 0.0f ||
                            nearGround > m_csmNearCurrent + step * 1.25f ||
                            nearGround < m_csmNearCurrent - step * 0.25f) {
                            cand = std::floor(nearGround / step) * step;
                        }
                    } else {
                        cand = (m_csmNearCurrent <= 0.0f)
                            ? nearGround
                            : glm::mix(m_csmNearCurrent, nearGround, smoothK);
                    }
                    if (renderMats == m_cascadeRenderMatrices) m_csmNearCurrent = cand;
                    snRaw = std::max(snRaw, cand);
                }
            }
            const float sn = snRaw;
            // (Tentativa 2026-09-02 de alcance adaptativo ao chao visivel foi
            // REVERTIDA: medido em cidade-A pitch 20, o teto 3000 nao acrescentou
            // sombra nenhuma no fundo - a esfera da c1 ja' cobre o chao alem de
            // csmFar - e custava +4,5 ms no zoom 0 do parana.)
            // ALCANCE ADAPTATIVO (2026-09-05). O sf fixo deixava sem sombra
            // todo chao alem de csmFar - visivel em zoom alto / pitch baixo,
            // onde a camera orbital enxerga chao a 2-3 mil unidades. Ate onde
            // ela ve: altura h = d*sin(pitch) sobre o plano do alvo; o raio de
            // baixo do frustum toca o chao no angulo (pitch - fovy/2). Abaixo
            // de ~4 graus ele aponta pro horizonte e vale o teto.
            float sfRaw = m_settings.csmFar;
            if (m_settings.csmFarAdaptive) {
                const float h = std::max(orbitDist * std::sin(std::max(camPitch, 0.0f)), 1.0f);
                const float ang = camPitch - fovy * 0.5f;
                float groundFar = (ang > glm::radians(4.0f)) ? (h / std::sin(ang)) * 1.15f
                                                             : m_settings.csmFarMax;
                groundFar = glm::clamp(groundFar, m_settings.csmFar, m_settings.csmFarMax);
                // SUAVIZACAO CONTINUA (2026-09-05). Antes: degraus de
                // csmFarStep com histerese - cada degrau redimensionava o
                // texel de uma vez e o autor viu isso como "snap de sombra no
                // pitch". Agora o alcance persegue o alvo com um filtro
                // exponencial de constante csmFarSmoothTau: o texel muda um
                // pouco por frame enquanto o pitch anda e para de mudar quando
                // a camera para. A esfera de Valient continua a fixar a
                // ORIGEM do texel; so' a escala varia, e devagar.
                float cand;
                if (kLegacyQuant) {
                    const float step = std::max(m_settings.csmFarStep, 16.0f);
                    cand = m_csmFarCurrent;
                    if (m_csmFarCurrent <= 0.0f ||
                        groundFar > m_csmFarCurrent + step * 0.25f ||
                        groundFar < m_csmFarCurrent - step * 1.25f) {
                        cand = std::ceil(groundFar / step) * step;
                    }
                } else {
                    cand = (m_csmFarCurrent <= 0.0f)
                        ? groundFar
                        : glm::mix(m_csmFarCurrent, groundFar, smoothK);
                }
                if (renderMats == m_cascadeRenderMatrices) m_csmFarCurrent = cand;
                sfRaw = cand;
            }
            const float sf = std::max(sfRaw, sn + 10.0f);
            // ERUPTION_TEST_CSM_LOG=1 (debug): imprime o alcance da cascata
            // por frame. E' o que "snapa" no pitch - a medida por pixel e'
            // dominada por ruido temporal do dither/alfa, esta e' direta.
            static const bool kCsmLog = [] {
                const char* e = std::getenv("ERUPTION_TEST_CSM_LOG");
                return e && std::atoi(e) != 0;
            }();
            if (kCsmLog && renderMats == m_cascadeRenderMatrices) {
                ERUPTION_LOG_WARN("[CSMLOG] sn=%.3f sf=%.3f pitch=%.4f", sn, sf, camPitch);
            }
            float splits[CASCADE_COUNT + 1];
            splits[0] = sn;
            splits[CASCADE_COUNT] = sf;
            for (uint32_t i = 1; i < CASCADE_COUNT; ++i) {
                const float t = static_cast<float>(i) / CASCADE_COUNT;
                const float uni = sn + (sf - sn) * t;
                const float log = sn * std::pow(sf / sn, t);
                splits[i] = glm::mix(uni, log, m_settings.csmLambda);
            }

            const Vec3 camPos = camera.position();
            const Vec3 camFwd = camera.forward();

            for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
                const float n = splits[i];
                const float f = splits[i + 1];
                // Centro otimo da esfera na direcao do olhar (derivacao:
                // igualar distancia do centro aos cantos do near e do far
                // da fatia): C = (n+f)(1+k2)/2, clampado a fatia.
                float C = glm::clamp((n + f) * 0.5f * (1.0f + k2), n, f);
                const float dFar = std::sqrt((f - C) * (f - C) + f * f * k2);
                const float dNear = std::sqrt((C - n) * (C - n) + n * n * k2);
                float radius = std::max(dFar, dNear);
                // Quantiza o raio (teto): imune a jitter de fov/aspect
                // (resize de janela) e mantem o texel em valores redondos.
                radius = std::ceil(radius / 16.0f) * 16.0f;

                const float texel = 2.0f * radius / static_cast<float>(m_currentAtlasSize);
                // ERUPTION_TEST_CSM_LOG=1: o TAMANHO DO TEXEL em unidades de
                // mundo e' o denominador do criterio de descolamento da sombra
                // (tessellation_viabilidade.md:81): deslocar o receptor sem
                // deslocar o caster e' invisivel enquanto a amplitude ficar
                // abaixo de um texel. Sem este numero o G4 vira estetica.
                if (kCsmLog && renderMats == m_cascadeRenderMatrices) {
                    ERUPTION_LOG_WARN("[CSMTEXEL] c%u raio=%.2f texel=%.4f atlas=%u",
                                      i, radius, texel, m_currentAtlasSize);
                }

                // Grade ABSOLUTA: remove a fracao sub-texel da origem do
                // mundo (viewSnapDelta) e depois trava o centro na grade.
                Vec2 d = viewSnapDelta(texel, texel);
                const Mat4 lightViewI =
                    glm::translate(Mat4(1.0f), Vec3(-d.x, -d.y, 0.0f)) * lightViewRaw;

                const Vec3 centerWorld = camPos + camFwd * C;
                Vec3 cLs = Vec3(lightViewI * Vec4(centerWorld, 1.0f));
                cLs.x = std::floor(cLs.x / texel) * texel;
                cLs.y = std::floor(cLs.y / texel) * texel;

                Mat4 proj = glm::ortho(cLs.x - radius, cLs.x + radius,
                                       cLs.y - radius, cLs.y + radius,
                                       0.1f, depthRange);
                proj[1][1] *= -1.0f; // Vulkan Y-flip
                renderMats[i] = proj * lightViewI;
                lookupMats[i] = biasMat * renderMats[i];

                if (renderMats == m_cascadeRenderMatrices) {
                    if (i == 0) m_currentWorldTexelSize = texel;
                    else        m_c1WorldTexelSize = texel;
                }
            }
            if (renderMats == m_cascadeRenderMatrices) {
                m_cascadeSplits[0] = splits[1];
                m_cascadeSplits[1] = sf;
                // .z = 0: shader NAO aplica o fade de borda do modo single -
                // fora da c0 o caminho existente cai na c1 (handoff limpo).
                m_cascadeSplitsVec = Vec4(splits[1], sf, 0.0f, 0.0f);
            }
            return; // caminho legado (c0 curva + c1 ray-fit) nao roda no csm
        }

        // c0: the legacy square box — same center, same size, same texel as
        // before; only the snap moved from world axes to the light-space
        // lattice (see above).
        Mat4 lightProj0 = glm::ortho(-halfSize, halfSize, -halfSize, halfSize, 0.1f, depthRange);
        lightProj0[1][1] *= -1.0f; // Vulkan Y-flip
        renderMats[0] = lightProj0 * lightView0;
        lookupMats[0] = biasMat * renderMats[0];

        // c1: rectangular fit — coverage-only overflow layer sampled OUTSIDE
        // the c0 box. The square box wastes coverage behind the camera and
        // still misses the top of the screen at low pitch. Extend the ortho
        // bounds PER AXIS (in light space) to cover the ground the camera
        // actually sees, derived from a ray-grid fit onto a relief plane
        // below the target. Never shrinks below the c0 box. Capped per side
        // by the texel budget (lowPitchMaxTexel); coarse texels are fine
        // here — this layer only shadows what c0 leaves fully lit.
        if (m_settings.singleMap) {
            // MODO SINGLE (reescrito do zero, nao ajustado): nao ha' segunda
            // caixa. O slot 1 e' identico ao slot 0 (a mesma caixa simples,
            // centrada no alvo, tamanho so' de zoom+pitch com histerese - a
            // receita que ja' era estavel pro c0 antigo). O ray-grid-fit que
            // gerava a antiga c1 foi REMOVIDO, nao ajustado outra vez: ele
            // dependia de angulo/rotacao da camera (o "culling de angulo" que
            // o autor notou) e de estado entre frames (freeze-by-view,
            // histerese de degrau) - cada dependencia dessas era uma fonte
            // PROPRIA de cintilacao, e cada rodada de ajuste nelas so' trocava
            // um sintoma por outro. Trade-off aceito: caixa mais larga que o
            // fit "otimizado" preferia - zero estado, zero angulo, zero
            // degrau. O slot 1 do atlas nem e' desenhado (renderCascades
            // pula), o que corta o passe de sombra pela metade.
            renderMats[1] = renderMats[0];
            lookupMats[1] = lookupMats[0];
            if (renderMats == m_cascadeRenderMatrices) {
                m_c1WorldTexelSize = m_currentWorldTexelSize;
            }
        } else {
        float orthoL = -halfSize, orthoR = halfSize;

        float orthoB = -halfSize, orthoT = halfSize;
        {
            const Mat4 invViewProj = glm::inverse(camera.viewProjNoJitter());
            const Vec3 camPos = camera.position();
            const float groundY = camTarget.y - 300.0f; // relief allowance
            const float capHalf = m_settings.lowPitchMaxTexel *
                                  static_cast<float>(m_currentAtlasSize) / 2.0f;
            for (int iy = 0; iy < 5; ++iy) {
                for (int ix = 0; ix < 5; ++ix) {
                    float nx = -1.0f + 2.0f * (static_cast<float>(ix) / 4.0f);
                    float ny = -1.0f + 2.0f * (static_cast<float>(iy) / 4.0f);
                    Vec4 p = invViewProj * Vec4(nx, ny, 1.0f, 1.0f); // far plane (Vulkan z=1)
                    if (std::abs(p.w) < 1e-6f) p.w = 1e-6f;
                    Vec3 w = Vec3(p.x, p.y, p.z) / p.w;
                    Vec3 dir = w - camPos;
                    if (dir.y > -1e-4f) continue; // ray at/above horizon: no ground hit
                    float t = (groundY - camPos.y) / dir.y;
                    if (t < 0.0f) continue;
                    Vec3 hit = camPos + dir * t;
                    Vec3 ls = Vec3(lightViewRaw * Vec4(hit, 1.0f));
                    orthoL = std::min(orthoL, std::max(ls.x, -capHalf));
                    orthoR = std::max(orthoR, std::min(ls.x,  capHalf));
                    orthoB = std::min(orthoB, std::max(ls.y, -capHalf));
                    orthoT = std::max(orthoT, std::min(ls.y,  capHalf));
                }
            }
            // O ajuste bruto fica como esta'. A estabilizacao do TAMANHO
            // acontece DEPOIS do recentramento (mais abaixo), porque a
            // extensao necessaria depende de onde o centro ficou.
        }

        float c1TexelX = std::max((orthoR - orthoL) / static_cast<float>(m_currentAtlasSize), 1e-3f);
        float c1TexelY = std::max((orthoT - orthoB) / static_cast<float>(m_currentAtlasSize), 1e-3f);
        if (renderMats == m_cascadeRenderMatrices) {
            m_c1WorldTexelSize = std::max(c1TexelX, c1TexelY);
        }
        Mat4 lightView1 = lightViewRaw;
        if (m_settings.enablePositionalSnap) {
            // Same light-space lattice lock as c0, with c1's own texel sizes;
            // then shift the fit bounds into the snapped frame and snap the
            // box center to the per-axis texel lattice.
            // ORDEM CERTA: primeiro o TAMANHO (congelado por vista), depois
            // a grade. A versao anterior travava a lightView na grade do texel
            // PRE-quantizacao e so' depois quantizava o tamanho - o texel final
            // era outro, entao a trava ficava sub-texel fora e a sombra
            // deslizava a cada passo (shimmering reportado andando).
            float hx = (orthoR - orthoL) * 0.5f;
            float hy = (orthoT - orthoB) * 0.5f;
            float cx0 = (orthoL + orthoR) * 0.5f;
            float cy0 = (orthoB + orthoT) * 0.5f;
            // margem p/ o snap de centro (meio texel por lado, folga inteira)
            float need = std::max(hx, hy) + 2.0f * std::max(c1TexelX, c1TexelY);

            const float step = 128.0f;
            const float yawNow = camera.orbitYaw();
            const float pitchNow = camera.orbitPitch();
            const bool sameView = m_c1StableHalf > 0.0f &&
                std::abs(yawNow - m_c1KeyYaw) < 0.02f &&
                std::abs(pitchNow - m_c1KeyPitch) < 0.02f &&
                std::abs(m_zoomPercent - m_c1KeyZoom) < 0.01f;
            // (o orcamento de texel de modo single foi para o ramo novo
            // acima - este bloco so' roda no modo cascade, que nunca e' single)
            float qh;
            if (sameView) {
                qh = (need > m_c1StableHalf)
                   ? std::ceil(need / step) * step   // cobertura manda: cresce
                   : m_c1StableHalf;                 // andando: congelado
            } else {
                qh = std::ceil(need / step) * step;  // vista mudou: recalcula
                m_c1KeyYaw = yawNow;
                m_c1KeyPitch = pitchNow;
                m_c1KeyZoom = m_zoomPercent;
            }
            m_c1StableHalf = qh;
            if (kShadowDebug && renderMats == m_cascadeRenderMatrices) {
                ERUPTION_LOG_WARN("[C1F] half=%.0f yaw=%.3f pitch=%.3f zoom=%.3f same=%d",
                                  qh, yawNow, pitchNow, m_zoomPercent, (int)sameView);
            }

            // Texel do mapa FINAL - e' NESTA grade que a lightView trava e o
            // centro snapa. Um texel so', coerente com o bias do shader.
            const float finalTexel =
                std::max(2.0f * qh / static_cast<float>(m_currentAtlasSize), 1e-3f);
            c1TexelX = finalTexel;
            c1TexelY = finalTexel;
            if (renderMats == m_cascadeRenderMatrices) {
                m_c1WorldTexelSize = finalTexel;
            }

            Vec2 d = viewSnapDelta(finalTexel, finalTexel);
            lightView1 = glm::translate(Mat4(1.0f), Vec3(-d.x, -d.y, 0.0f)) * lightViewRaw;
            float cx = std::floor((cx0 - d.x) / finalTexel) * finalTexel;
            float cy = std::floor((cy0 - d.y) / finalTexel) * finalTexel;

            orthoL = cx - qh; orthoR = cx + qh;
            orthoB = cy - qh; orthoT = cy + qh;
        }
        if (kShadowDebug && renderMats == m_cascadeRenderMatrices) {
            dbgOrthoL = orthoL; dbgOrthoR = orthoR;
            dbgOrthoB = orthoB; dbgOrthoT = orthoT;
        }

        Mat4 lightProj1 = glm::ortho(orthoL, orthoR, orthoB, orthoT, 0.1f, depthRange);
        lightProj1[1][1] *= -1.0f; // Vulkan Y-flip
        renderMats[1] = lightProj1 * lightView1;
        lookupMats[1] = biasMat * renderMats[1];

        }

        if (renderMats == m_cascadeRenderMatrices) {
            m_cascadeSplits[0] = halfSize;
            m_cascadeSplits[1] = farPlane;
            // .z = 1 SO' no modo single: liga o fade de borda no shader (no
            // csm/cascade, fora da c0 o handoff e' pra c1, nao pra luz).
            m_cascadeSplitsVec = Vec4(halfSize, farPlane,
                                      m_settings.singleMap ? 1.0f : 0.0f, 0.0f);
        }
    };

    computeMatrices(currentLightDir, m_cascadeRenderMatrices, m_cascadeLookupMatrices);
    computeMatrices(nextLightDir, m_nextCascadeRenderMatrices, m_nextCascadeLookupMatrices);
    holdFarCascade(camera, currentLightDir);
    holdStaticCache(camera.target());
    {
        // Receptores: a visao da camera sem jitter, 2% mais larga (o jitter
        // e o filtro da sombra leem um pouco alem da borda). A cascata
        // guardada vale por mais frames com a camera andando ate' 3% da meia
        // largura dela e girando ate' ~1,8 graus: a folga cobre os dois.
        Mat4 widen(1.0f);
        widen[0][0] = widen[1][1] = 0.98f;
        m_receivers.extractFromMatrix(widen * camera.viewProjNoJitter());
        // Alcance do filtro (directional.frag), em mundo: a busca de
        // bloqueador do PCSS anda tan(raio do sol) * 6 em UV do atlas (a
        // cascata ocupa meia largura, entao 1 UV em x = 4 meias larguras) e o
        // PCF depois anda kernel x 4 (teto da penumbra) texels. Um caster a
        // essa distancia da visao ainda escurece pixel visivel.
        const float size = static_cast<float>(std::max(m_currentAtlasSize, 1u));
        const float searchUv = std::max(std::tan(m_sunAngularRadius) * 6.0f, 1.0f / size);
        const float pcfTexels = static_cast<float>(m_settings.pcfKernelSize < 3 ? 7 : m_settings.pcfKernelSize) * 4.0f + 2.0f;
        for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
            const float half = 1.0f / std::max(std::abs(m_cascadeRenderMatrices[c][0][0]), 1e-6f);
            m_receiverMargin[c] = half * (4.0f * searchUv + pcfTexels * 2.0f / size);
        }
        if (m_settings.csmFarInterval > 1) {
            const float half = 1.0f / std::max(std::abs(m_cascadeRenderMatrices[1][0][0]), 1e-6f);
            m_receiverMargin[1] += 0.12f * half;
        }
    }

    if (kShadowDebug) {
        static int frame = 0;
        if (++frame % 60 == 0) {
            const float rawHalf = m_settings.singleMap
                ? m_settings.singleSizeCurve.evaluate(m_zoomPercent)
                : m_settings.c0SizeCurve.evaluate(m_zoomPercent);
            const float texel = m_currentWorldTexelSize;
            const float cap = m_settings.lowPitchMaxTexel * static_cast<float>(m_currentAtlasSize) /
                              (2.0f * std::max(rawHalf, 1.0f));
            const float boost = glm::clamp(glm::mix(m_settings.lowPitchSizeBoost, 1.0f,
                                                    glm::smoothstep(0.0f, 0.45f, tPitch)),
                                           1.0f, std::max(cap, 1.0f));
            ERUPTION_LOG_WARN("[SHADOW] pitch=%.1f deg zoom=%.2f rawHalf=%.0f texel=%.2fm (boost x%.2f) extX=[%.0f,%.0f] extY=[%.0f,%.0f]",
                            glm::degrees(camPitch), m_zoomPercent, rawHalf, texel, boost,
                            dbgOrthoL, dbgOrthoR, dbgOrthoB, dbgOrthoT);
        }
    }
}

// Sombra pre-assada: enquanto o cozimento ainda serve (mesmo tamanho de
// caixa, sol quase no mesmo lugar, camera dentro da folga), a iluminacao fica
// com as matrizes dele; senao ele vale como perdido e o proximo renderCascades
// coze de novo com as matrizes deste quadro. Parada, a imagem e' identica a
// sem cache; andando, so' a borda da caixa atrasa ate' a folga.
void ShadowRenderer::holdStaticCache(const Vec3& center) {
    m_cache.center = center;
    if (!staticCacheActive() || !m_cache.valid) return;
    const float slack = m_cache.halfSize * m_settings.staticCacheMargin;
    const Vec2 moved(center.x - m_cache.bakedCenter.x, center.z - m_cache.bakedCenter.z);
    const bool fits = m_cache.size == m_currentAtlasSize && m_cascadeSplitsVec.x == m_cache.halfSize &&
                      glm::dot(moved, moved) <= slack * slack &&
                      glm::dot(m_currentLightDir, m_cache.lightDir) >=
                          std::cos(glm::radians(m_settings.staticCacheMaxDeg));
    if (!fits) {
        m_cache.valid = false;
        return;
    }
    m_cascadeRenderMatrices[0] = m_cascadeRenderMatrices[1] = m_cache.render;
    m_cascadeLookupMatrices[0] = m_cascadeLookupMatrices[1] = m_cache.lookup;
    m_cascadeSplitsVec = m_cache.splits;
    m_currentWorldTexelSize = m_c1WorldTexelSize = m_cache.texel;
    m_shadowDepthRange = m_cache.depthRange;
    m_currentLightDir = m_cache.lightDir;
}

bool ShadowRenderer::ensureCacheImage() {
    if (m_cache.image != VK_NULL_HANDLE) return true;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {m_currentAtlasSize, m_currentAtlasSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = lean::shadowDepthFormat();
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_cache.image, &m_cache.alloc, nullptr) !=
        VK_SUCCESS) {
        ERUPTION_LOG_WARN("[SOMBRA] sem memoria para a sombra pre-assada - desligada");
        m_cache.image = VK_NULL_HANDLE;
        m_settings.staticCache = false;
        return false;
    }
    PROFILE_VRAM_ALLOC(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_cache.alloc));
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_cache.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = lean::shadowDepthFormat();
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_cache.view);
    m_cache.size = m_currentAtlasSize;
    m_cache.valid = false;
    return true;
}

void ShadowRenderer::destroyCacheImage() {
    if (m_cache.view != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), m_cache.view, nullptr);
    if (m_cache.image != VK_NULL_HANDLE) {
        PROFILE_VRAM_FREE(ProfilerCategory::ShadowMaps, m_ctx->allocationSize(m_cache.alloc));
        vmaDestroyImage(m_ctx->allocator(), m_cache.image, m_cache.alloc);
    }
    m_cache.view = VK_NULL_HANDLE;
    m_cache.image = VK_NULL_HANDLE;
    m_cache.alloc = VK_NULL_HANDLE;
    m_cache.size = 0;
    m_cache.valid = false;
}

// Cozimento (quando preciso) do cenario parado na imagem propria, copia dela
// para a cascata 0 do atlas e, por cima, so' os casters que se mexem.
void ShadowRenderer::renderWithStaticCache(VkCommandBuffer cmd, const CasterSources& src) {
    const uint64_t version = src.models ? src.models->staticShadowVersion() : 0;
    const VkExtent2D extent{m_cache.size, m_cache.size};
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    if (version != m_cache.seenVersion) {
        m_cache.seenVersion = version;
        m_cache.stableFrames = 0;
    } else if (m_cache.stableFrames < kStaticSettleFrames) {
        ++m_cache.stableFrames;
    }
    m_cache.staleFrames = m_cache.version != version ? m_cache.staleFrames + 1 : 0;
    const bool settled = m_cache.stableFrames >= kStaticSettleFrames || m_cache.staleFrames >= kStaticMaxStaleFrames;
    if (!m_cache.valid || (m_cache.version != version && settled)) {
        m_ctx->cmdImageBarrier(cmd, m_cache.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        depth.imageView = m_cache.view;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        m_ctx->cmdBeginRendering(cmd, {}, &depth, nullptr, extent);
        // Sem recorte por receptor: a camera gira sem pedir cozimento novo.
        drawCascade(cmd, src, m_cascadeRenderMatrices[0], 0, nullptr, Casters::Static);
        m_ctx->cmdEndRendering(cmd);
        m_ctx->cmdImageBarrier(cmd, m_cache.image,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        // Diagnóstico de GPU de celular sem timestamp (PowerVR): cada
        // cozimento desenha todo o cenário parado; vários seguidos (objetos
        // nascendo em quadros seguidos) somam num quadro longo.
        ERUPTION_LOG_WARN("Shadow cache: baked (%s, version %llu)", m_cache.valid ? "static set changed" : "sun, camera or first",
                          static_cast<unsigned long long>(version));
        m_cache.valid = true;
        m_cache.version = version;
        m_cache.staleFrames = 0;
        m_cache.bakedCenter = m_cache.center;
        m_cache.lightDir = m_currentLightDir;
        m_cache.halfSize = m_cascadeSplitsVec.x;
        m_cache.render = m_cascadeRenderMatrices[0];
        m_cache.lookup = m_cascadeLookupMatrices[0];
        m_cache.splits = m_cascadeSplitsVec;
        m_cache.texel = m_currentWorldTexelSize;
        m_cache.depthRange = m_shadowDepthRange;
    }

    m_ctx->cmdImageBarrier(cmd, m_atlasImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    const VkImageSubresourceLayers layer{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    const VkImageCopy region{layer, {0, 0, 0}, layer, {0, 0, 0}, {m_cache.size, m_cache.size, 1}};
    vkCmdCopyImage(cmd, m_cache.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    m_ctx->cmdImageBarrier(cmd, m_atlasImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    depth.imageView = m_atlasView;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    m_ctx->cmdBeginRendering(cmd, {}, &depth, nullptr, extent);
    drawCascade(cmd, src, m_cascadeRenderMatrices[0], 0, m_settings.receiverCull ? &m_receivers : nullptr,
                Casters::Moving);
    m_ctx->cmdEndRendering(cmd);
    m_ctx->cmdImageBarrier(cmd, m_atlasImage,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
}

// Decide se a cascata distante e' redesenhada neste frame. Se nao, a busca da
// iluminacao fica com a matriz do frame em que ela foi desenhada (o atlas
// guarda aquela imagem), entao a sombra continua no lugar certo; so' a
// folhagem ao vento la' longe anda a meio passo. Salto de camera, luz que
// virou ou atlas novo redesenham na hora.
void ShadowRenderer::holdFarCascade(const Camera& camera, const Vec3& lightDir) {
    const uint32_t interval = m_settings.csmFarInterval;
    const bool canHold = interval > 1 && !m_settings.singleMap && !m_settings.enableTemporalBlend;
    const Vec3 cam = camera.position();
    const Vec3 light = glm::normalize(lightDir);
    // Meia largura da cascata pela escala da projecao ortografica (2 / largura).
    const float half = 1.0f / std::max(std::abs(m_cascadeRenderMatrices[1][0][0]), 1e-6f);
    // Giro da camera tambem redesenha: a sombra guardada so' traz os casters
    // da visao daquele frame (receiverCull), e a ponta distante do quadro
    // anda muito mais que a posicao num giro.
    const Vec3 fwd = camera.forward();
    bool draw = !canHold || !m_farHeldValid || m_farHeldAtlas != m_currentAtlasSize ||
                glm::distance(cam, m_farHeldCam) > 0.03f * half || glm::dot(light, m_farHeldLight) < 0.99996f ||
                glm::dot(fwd, m_farHeldFwd) < 0.9995f;
    if (!draw && ++m_farAge >= interval) draw = true;
    m_drawFar = draw;
    if (draw) {
        m_farAge = 0;
        m_farHeldValid = canHold;
        m_farHeldAtlas = m_currentAtlasSize;
        m_farHeldCam = cam;
        m_farHeldLight = light;
        m_farHeldFwd = fwd;
        m_farRenderHeld = m_cascadeRenderMatrices[1];
        m_farLookupHeld = m_cascadeLookupMatrices[1];
    } else {
        m_cascadeRenderMatrices[1] = m_farRenderHeld;
        m_cascadeLookupMatrices[1] = m_farLookupHeld;
    }
}

// ERUPTION_TEST_SHADOW_OPAQUE_PIPE=0 devolve o caster solido para a pipeline
// com teste de alfa - A/B do ganho, e a prova de que a imagem nao muda.
static const bool kOpaqueShadowPipe = [] {
    const char* e = std::getenv("ERUPTION_TEST_SHADOW_OPAQUE_PIPE");
    return !(e && std::atoi(e) == 0);
}();

VkRect2D ShadowRenderer::cascadeRect(uint32_t cascade) const {
    return {{static_cast<int32_t>((cascade % 2) * m_currentAtlasSize), static_cast<int32_t>((cascade / 2) * m_currentAtlasSize)},
            {m_currentAtlasSize, m_currentAtlasSize}};
}

// Os casters de uma cascata, com o rendering ja' aberto no atlas (ou na
// imagem da sombra pre-assada, que tem o tamanho de uma cascata).
void ShadowRenderer::drawCascade(VkCommandBuffer cmd, const CasterSources& src, const Mat4& mat, uint32_t cascade,
                                 const Frustum* receivers, Casters which) {
    const VkRect2D scissor = cascadeRect(cascade);
    const VkViewport viewport{static_cast<float>(scissor.offset.x), static_cast<float>(scissor.offset.y),
                              static_cast<float>(scissor.extent.width),
                              static_cast<float>(scissor.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (m_bindless) {
        VkDescriptorSet bindlessSet = m_bindless->set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowLayout,
                                0, 1, &bindlessSet, 0, nullptr);
    }

    // Render terrain and models for this cascade
    Frustum shadowFrustum;
    shadowFrustum.extractFromMatrix(mat);

    static const bool kProf = std::getenv("ERUPTION_TEST_SHADOW_DEBUG") != nullptr;
    auto tProf = std::chrono::steady_clock::now();
    auto lap = [&](const char* tag) {
        if (!kProf) return;
        auto now = std::chrono::steady_clock::now();
        static int fr = 0; static double acc[3] = {0,0,0}; static const char* tags[3];
        int slot = (tag[0]=='t')?0:(tag[0]=='m')?1:2;
        acc[slot] += std::chrono::duration<double,std::milli>(now - tProf).count();
        tags[slot] = tag; tProf = now;
        if (slot == 2 && ++fr % 120 == 0) {
            ERUPTION_LOG_WARN("[SHPROF] %s=%.2f %s=%.2f %s=%.2f (ms, media 120f)",
                              tags[0], acc[0]/120, tags[1], acc[1]/120, tags[2], acc[2]/120);
            acc[0]=acc[1]=acc[2]=0;
        }
    };

    // Terrain shadows
    // DIAGNOSTICO: ERUPTION_TEST_SHADOW_NO_TERRAIN=1 mede a fatia do
    // terreno LEGADO (formato .ter; em mapa GLB o chao e' malha de modelo
    // e ja' passa pelo ModelRenderer). Nao e' caminho de producao.
    static const bool kNoTerrainCaster = [] {
        const char* e = std::getenv("ERUPTION_TEST_SHADOW_NO_TERRAIN");
        return e && std::atoi(e) != 0;
    }();
    if (!kNoTerrainCaster && which != Casters::Moving) {
        src.terrain->renderShadow(cmd, m_shadowPipeline, m_shadowLayout, mat,
                              shadowFrustum, m_settings.enableShadowCulling);
    }
    lap("terrain");

    // Model shadows. ERUPTION_TEST_SHADOW_NO_MODELS=1 (debug): mede o
    // passe sem os modelos, num aparelho sem timestamp de GPU.
    static const bool kNoModelCasters = std::getenv("ERUPTION_TEST_SHADOW_NO_MODELS") != nullptr;
    ModelRenderer* models = src.models;
    if (models && models->isInitialized() && !kNoModelCasters) {
        float minSize = 0.0f;
        if (m_settings.enableAdaptiveShadows) {
            minSize = m_settings.minModelShadowSizeCurve.evaluate(m_zoomPercent);
            // Limita a curva ao que ESTA cascata nao consegue resolver.
            // O texel e' POR CASCATA: usar sempre o da c0 (fino) fazia
            // a cascata 1 desenhar milhares de props que ali ocupam
            // MENOS DE UM TEXEL - invisiveis, e o item mais caro do
            // modo csm (medido: Shadow CPU 0.86ms no single de 1 slot
            // contra 4.59ms nas 2 cascatas; a c1 e' quem arrasta).
            // Com o texel certo, um prop de 2u so' entra na cascata
            // onde ele de fato cobre pixel.
            const float cascadeTexel = (cascade == 0) ? m_currentWorldTexelSize
                                                : m_c1WorldTexelSize;
            const float texelReq = (cascade == 0) ? m_settings.minShadowTexels
                                            : m_settings.minShadowTexels * m_settings.csmFarTexelBias;
            const float texelFloor = cascadeTexel * texelReq;
            if (texelFloor > 0.0f) {
                // c0 mantem o comportamento antigo (min entre curva e
                // piso). Cascatas distantes usam o PISO como minimo
                // efetivo: nelas o piso e' o criterio fisico (nao da'
                // para resolver o que e' menor que um texel), a curva
                // e' so' preferencia artistica de perto.
                minSize = (cascade == 0) ? std::min(minSize, texelFloor)
                                   : std::max(minSize, texelFloor);
                // CSM: so' o criterio FISICO (menor que N texels desta
                // cascata nao resolve). A curva por zoom era o
                // "culling esquisito" que o autor viu em cidade-A ao
                // afastar: em zoom 0 ela cortava tudo abaixo de 10 u.
                // O que e' pequeno DEMAIS NA TELA e' cortado no
                // ModelRenderer por pixels projetados (shadow_caster_
                // min_px), continuo no zoom e na distancia.
                if (m_settings.csmStable) minSize = texelFloor;
            }
        }
        // ERUPTION_TEST_NO_SIZE_CULL=1 (referencia da bancada): caster
        // menor que o texel da cascata tambem entra.
        static const bool kNoSizeCull = std::getenv("ERUPTION_TEST_NO_SIZE_CULL") != nullptr;
        if (kNoSizeCull) minSize = 0.0f;
        models->setShadowCasterFilter(which == Casters::All ? 0 : which == Casters::Static ? 1 : 2);
        // Texel desta cascata em unidades do mundo: o nivel de cada caster sai
        // do tamanho dele NESTA cascata (ModelRenderer::shadowLevelFor).
        models->setShadowTexel(cascade == 0 ? m_currentWorldTexelSize : m_c1WorldTexelSize);
        models->renderShadow(cmd,
                             m_shadowInstPipeline != VK_NULL_HANDLE ? m_shadowInstPipeline : m_shadowPipeline,
                             m_shadowInstLayout != VK_NULL_HANDLE ? m_shadowInstLayout : m_shadowLayout,
                             mat, shadowFrustum, minSize, m_settings.enableShadowCulling,
                             m_shadowInstPipeline != VK_NULL_HANDLE, src.frameUbo.windParams,
                             kOpaqueShadowPipe && m_shadowInstPipeline != VK_NULL_HANDLE
                                 ? m_shadowInstOpaquePipeline : VK_NULL_HANDLE,
                             receivers, m_receiverMargin[cascade]);
        models->setShadowCasterFilter(0);
    }

    lap("models");

    // Sprite shadows e oclusores extras: sempre do lado de quem se mexe.
    if (which != Casters::Static) {
        if (src.sprites && src.spriteCount > 0)
            src.sprites->renderShadow(cmd, m_shadowLayout, mat, src.frameUbo, src.spriteBuffer, src.spriteCount);
        lap("sprites");
        if (m_extraCasters) m_extraCasters(cmd, mat, cascade);
    }
}

void ShadowRenderer::renderCascades(VkCommandBuffer cmd, TerrainRenderer* terrain, ModelRenderer* models, SpriteRenderer* sprites, const FrameUBO& frameUbo, VkBuffer spriteInstanceBuffer, uint32_t spriteCount) {
    // ERUPTION_TEST_SHADOW_DEBUG=1: quem entra no atlas por frame (a cada 120).
    static const bool kCastDbg = std::getenv("ERUPTION_TEST_SHADOW_DEBUG") != nullptr;
    if (kCastDbg) {
        static int f = 0;
        if (++f % 120 == 0)
            ERUPTION_LOG_WARN("[SHADOWIN] L=(%.2f,%.2f,%.2f) depthRange=%.0f atlas=%u sprites=%u",
                              m_currentLightDir.x, m_currentLightDir.y, m_currentLightDir.z,
                              m_shadowDepthRange, m_currentAtlasSize, spriteCount);
    }

    if (!terrain || !terrain->isInitialized()) return;
    const CasterSources src{terrain, models, sprites, frameUbo, spriteInstanceBuffer, spriteCount};
    if (staticCacheActive() && !m_settings.enableTemporalBlend && ensureCacheImage()) {
        renderWithStaticCache(cmd, src);
        return;
    }

    // Helper to render one atlas
    auto renderAtlas = [&](VkImage atlasImage, VkImageView atlasView, const Mat4* renderMats) {
        // Cascata distante guardada (holdFarCascade): o atlas mantem a imagem
        // dela e so' a regiao da c0 e' limpa.
        const bool keepFar = renderMats == m_cascadeRenderMatrices && !m_drawFar;
        // O atlas da mistura temporal pinta com a luz do proximo passo: fica
        // sem o recorte por receptor.
        const Frustum* receivers = (m_settings.receiverCull && renderMats == m_cascadeRenderMatrices)
                                       ? &m_receivers : nullptr;
        if (keepFar) {
            m_ctx->cmdImageBarrier(cmd, atlasImage,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT);
        } else {
            m_ctx->cmdImageBarrier(cmd, atlasImage,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT);
        }

        // Modo single: so' o slot 0 e' desenhado (o clear do beginRendering ja'
        // deixa o slot 1 em 1.0 = sem oclusor, e o lookup morto nunca amostra).
        const uint32_t drawCount = m_settings.singleMap ? 1u : CASCADE_COUNT;
        for (uint32_t i = 0; i < drawCount; i++) {
            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = atlasView;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = (i == 0 && !keepFar) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue.depthStencil = {1.0f, 0};

            const VkRect2D scissor = cascadeRect(i);

            if (i == 0) {
                // Cascades are side by side: atlas is size*CASCADE_COUNT wide, size tall.
                // So' a area das cascatas desenhadas: no modo de mapa unico a
                // metade de fora nem era lida, e limpar/gravar ela custava
                // banda (GPU de celular paga por ladrilho).
                uint32_t renderW = m_currentAtlasSize * drawCount;
                uint32_t renderH = m_currentAtlasSize;
                m_ctx->cmdBeginRendering(cmd, {}, &depthAttachment, nullptr, {renderW, renderH});
                if (keepFar) {
                    VkClearAttachment clear{};
                    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    clear.clearValue.depthStencil = {1.0f, 0};
                    const VkClearRect rect{scissor, 0, 1};
                    vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
                }
            }
            if (keepFar && i == 1) {
                if (i == drawCount - 1) m_ctx->cmdEndRendering(cmd);
                continue;
            }

            drawCascade(cmd, src, renderMats[i], i, receivers, Casters::All);

            if (i == drawCount - 1) {
                m_ctx->cmdEndRendering(cmd);
            }
        }

        m_ctx->cmdImageBarrier(cmd, atlasImage,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    };

    renderAtlas(m_atlasImage, m_atlasView, m_cascadeRenderMatrices);

    if (m_settings.enableTemporalBlend) {
        ensureNextAtlas();   // no-op se ja' existe; cria se ligaram em runtime
        if (m_nextAtlasView != VK_NULL_HANDLE) {
            renderAtlas(m_nextAtlasImage, m_nextAtlasView, m_nextCascadeRenderMatrices);
        }
    }
}

SplineCurve ShadowSettings::loadSplineFromJson(const json& node) {
    if (!node.is_array() || node.size() < 2) return SplineCurve();
    std::vector<float> xs, ys;
    std::vector<InterpolationMode> modes;
    for (size_t i = 0; i < node.size(); i++) {
        const auto& pt = node[i];
        if (pt.contains("x")) xs.push_back(pt["x"].get<float>());
        if (pt.contains("y")) ys.push_back(pt["y"].get<float>());
        if (i < node.size() - 1 && pt.contains("interpolation")) {
            std::string modeStr = pt.value("interpolation", "Linear");
            if (modeStr == "Smoothstep") modes.push_back(InterpolationMode::Smoothstep);
            else if (modeStr == "CatmullRom") modes.push_back(InterpolationMode::CatmullRom);
            else if (modeStr == "Step") modes.push_back(InterpolationMode::Step);
            else modes.push_back(InterpolationMode::Linear);
        }
    }
    if (xs.size() >= 2 && ys.size() >= 2) {
        if (modes.size() == xs.size() - 1) return SplineCurve(xs, ys, modes);
        return SplineCurve(xs, ys, InterpolationMode::Linear);
    }
    return SplineCurve();
}

void ShadowSettings::loadFromJson(const json& j) {
    singleMaxTexel = j.value("single_max_texel", singleMaxTexel);
    csmFar = j.value("csm_far", csmFar);
    csmFarAdaptive = j.value("csm_far_adaptive", csmFarAdaptive);
    if (const char* e = std::getenv("ERUPTION_SHADOW_FAR_ADAPTIVE")) csmFarAdaptive = std::atoi(e) != 0;
    csmFarMax = std::max(j.value("csm_far_max", csmFarMax), csmFar);
    csmFarStep = j.value("csm_far_step", csmFarStep);
    csmFarSmoothTau = j.value("csm_far_smooth_tau", csmFarSmoothTau);
    if (const char* e = std::getenv("ERUPTION_SHADOW_FAR_TAU")) csmFarSmoothTau = std::strtof(e, nullptr);
    csmLambda = std::clamp(j.value("csm_lambda", csmLambda), 0.0f, 1.0f);
    csmFarTexelBias = std::max(j.value("csm_far_texel_bias", csmFarTexelBias), 1.0f);
    csmFarInterval = std::clamp(j.value("csm_far_interval", csmFarInterval), 1u, 8u);
    if (const char* e = std::getenv("ERUPTION_SHADOW_FAR_INTERVAL"))
        csmFarInterval = std::clamp(static_cast<uint32_t>(std::max(std::atoi(e), 1)), 1u, 8u);
    receiverCull = j.value("receiver_cull", receiverCull);
    if (const char* e = std::getenv("ERUPTION_SHADOW_RECEIVER_CULL")) receiverCull = std::atoi(e) != 0;
    {
        std::string mode = j.value("mode", std::string("csm"));
        if (const char* env = std::getenv("ERUPTION_SHADOW_MODE")) mode = env;
        csmStable = (mode == "csm");
        singleMap = (mode == "single");
        // qualquer outro valor cai no "cascade" legado (ray-fit)
    }
    if (!j.is_object()) return;
    enabled = j.value("enabled", enabled);
    atlasSize = j.value("atlas_size", atlasSize);
    usePoisson = j.value("use_poisson", usePoisson);
    poissonTaps = j.value("poisson_taps", poissonTaps);
    pcfKernelSize = j.value("pcf_kernel_size", pcfKernelSize);
    debugLightRays = j.value("debug_light_rays", debugLightRays);
    enableTemporalBlend = j.value("enable_temporal_blend", enableTemporalBlend);
    enableAngleSnapping = j.value("enable_angle_snapping", enableAngleSnapping);
    enablePositionalSnap = j.value("enable_positional_snap", enablePositionalSnap);
    snapQuantizeStep = j.value("snap_quantize_step", snapQuantizeStep);
    biasValue = j.value("bias_value", biasValue);
    normalBias = j.value("normal_bias", normalBias);
    slopeBias = j.value("slope_bias", slopeBias);
    enableAdaptiveShadows = j.value("adaptive", json::object()).value("enabled", enableAdaptiveShadows);
    if (j.contains("adaptive")) {
        const auto& adaptive = j["adaptive"];
        if (adaptive.contains("c0_size_curve") && adaptive["c0_size_curve"].contains("points"))
            c0SizeCurve = loadSplineFromJson(adaptive["c0_size_curve"]["points"]);
        if (adaptive.contains("single_size_curve") && adaptive["single_size_curve"].contains("points"))
            singleSizeCurve = loadSplineFromJson(adaptive["single_size_curve"]["points"]);
        if (adaptive.contains("min_model_shadow_size_curve") && adaptive["min_model_shadow_size_curve"].contains("points"))
            minModelShadowSizeCurve = loadSplineFromJson(adaptive["min_model_shadow_size_curve"]["points"]);
        if (adaptive.contains("far_range_curve") && adaptive["far_range_curve"].contains("points"))
            farRangeCurve = loadSplineFromJson(adaptive["far_range_curve"]["points"]);
        lowPitchSizeBoost = adaptive.value("low_pitch_size_boost", lowPitchSizeBoost);
        lowPitchMaxTexel = adaptive.value("low_pitch_max_texel", lowPitchMaxTexel);
        minShadowTexels = adaptive.value("min_shadow_texels", minShadowTexels);
        // O json guarda este campo dentro de "adaptive"; a leitura na raiz
        // (acima) nunca o encontrava - o efetivo era o default do .hpp (2.5).
        csmFarTexelBias = std::max(adaptive.value("csm_far_texel_bias", csmFarTexelBias), 1.0f);
    }
}

} // namespace eruption
