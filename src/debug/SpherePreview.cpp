#include "debug/SpherePreview.hpp"
#include "core/Logger.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>

namespace eruption {

bool SpherePreview::init(VulkanContext* ctx, BindlessDescriptor* bindless,
                         VkDescriptorSetLayout frameUboLayout, VkDescriptorSet frameUboSet) {
    m_ctx = ctx;
    m_bindless = bindless;
    m_frameUboLayout = frameUboLayout;
    m_frameUboSet = frameUboSet;

    buildSphereGeometry();
    if (!createTargets()) return false;
    if (!createInstanceBuffer()) return false;
    if (!createPipeline()) return false;

    m_initialized = true;
    ERUPTION_LOG_WARN("[SPHEREPREVIEW] inicializado: %ux%u, %zu vertices, %zu indices",
                      kSize, kSize, m_vertices.size(), m_indices.size());
    return true;
}

void SpherePreview::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    vkDeviceWaitIdle(dev);
    if (m_pipeline) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_tessPipeline) vkDestroyPipeline(dev, m_tessPipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_instLayout) vkDestroyDescriptorSetLayout(dev, m_instLayout, nullptr);
    if (m_ownPool) vkDestroyDescriptorPool(dev, m_ownPool, nullptr);
    if (m_instBuffer) { if (m_instMapped) vmaUnmapMemory(m_ctx->allocator(), m_instAlloc); vmaDestroyBuffer(m_ctx->allocator(), m_instBuffer, m_instAlloc); }
    if (m_vertexBuffer) { if (m_vertexMapped) vmaUnmapMemory(m_ctx->allocator(), m_vertexAlloc); vmaDestroyBuffer(m_ctx->allocator(), m_vertexBuffer, m_vertexAlloc); }
    if (m_indexBuffer) vmaDestroyBuffer(m_ctx->allocator(), m_indexBuffer, m_indexAlloc);
    if (m_colorView) vkDestroyImageView(dev, m_colorView, nullptr);
    if (m_colorImage) vmaDestroyImage(m_ctx->allocator(), m_colorImage, m_colorAlloc);
    if (m_depthView) vkDestroyImageView(dev, m_depthView, nullptr);
    if (m_depthImage) vmaDestroyImage(m_ctx->allocator(), m_depthImage, m_depthAlloc);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_initialized = false;
}

// UV-sphere com UV/normal reais (nao a generateSphere de DeferredLighting,
// que e' so' posicao pra volume de luz). TerrainVertex e' o formato que
// model.vert espera - mesmos atributos que qualquer malha real do jogo.
void SpherePreview::buildSphereGeometry() {
    m_vertices.clear();
    m_indices.clear();
    const float pi = glm::pi<float>();
    for (int stack = 0; stack <= kStacks; ++stack) {
        const float phi = pi * static_cast<float>(stack) / static_cast<float>(kStacks);
        const float v = static_cast<float>(stack) / static_cast<float>(kStacks);
        const float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
        for (int slice = 0; slice <= kSlices; ++slice) {
            const float theta = 2.0f * pi * static_cast<float>(slice) / static_cast<float>(kSlices);
            const float u = static_cast<float>(slice) / static_cast<float>(kSlices);
            const Vec3 dir(sinPhi * std::cos(theta), cosPhi, sinPhi * std::sin(theta));
            TerrainVertex tv{};
            tv.position = dir * kRadius;
            tv.normal = dir;
            tv.texCoord = Vec2(u, v);
            tv.color = 0xFFFFFFFFu;
            m_vertices.push_back(tv);
        }
    }
    for (int stack = 0; stack < kStacks; ++stack) {
        for (int slice = 0; slice < kSlices; ++slice) {
            const uint32_t i0 = static_cast<uint32_t>(stack * (kSlices + 1) + slice);
            const uint32_t i1 = i0 + static_cast<uint32_t>(kSlices + 1);
            m_indices.push_back(i0); m_indices.push_back(i1); m_indices.push_back(i0 + 1);
            m_indices.push_back(i0 + 1); m_indices.push_back(i1); m_indices.push_back(i1 + 1);
        }
    }
}

// CUBO com faces PLANAS, cada uma uma grade NxN tesselavel (kCubeGrid) - pedido
// do autor pra' comparar com o chao real sem a curvatura da esfera atrapalhar
// a leitura visual ("compara como o chao e' flat em relacao a essa esfera -
// bota a opcao de ser esfera ou cubo"). Face plana desloca do MESMO jeito que
// um pedaco de chao: se a face fica lisa com uma textura que devia ter
// relevo, o bug NAO e' geometria de esfera, e' a mesma coisa que acontece no
// chao. 6 faces, direcao (normal, eixo direito, eixo cima) escolhida pra'
// right x up = normal (so' documentação - cull esta' OFF nesse preview, entao
// nem precisaria, mas errado ia confundir quem olhar o codigo depois).
void SpherePreview::buildCubeGeometry() {
    m_vertices.clear();
    m_indices.clear();
    struct Face { Vec3 normal, right, up; };
    const Face faces[6] = {
        {{ 1, 0, 0}, { 0, 0,-1}, {0, 1, 0}},
        {{-1, 0, 0}, { 0, 0, 1}, {0, 1, 0}},
        {{ 0, 1, 0}, { 1, 0, 0}, {0, 0,-1}},
        {{ 0,-1, 0}, { 1, 0, 0}, {0, 0, 1}},
        {{ 0, 0, 1}, { 1, 0, 0}, {0, 1, 0}},
        {{ 0, 0,-1}, {-1, 0, 0}, {0, 1, 0}},
    };
    constexpr int N = kCubeGrid;
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(m_vertices.size());
        for (int gy = 0; gy <= N; ++gy) {
            const float v = static_cast<float>(gy) / static_cast<float>(N);
            for (int gx = 0; gx <= N; ++gx) {
                const float u = static_cast<float>(gx) / static_cast<float>(N);
                TerrainVertex tv{};
                tv.position = f.normal * kRadius + f.right * ((u * 2.0f - 1.0f) * kRadius)
                                                  + f.up * ((v * 2.0f - 1.0f) * kRadius);
                tv.normal = f.normal;
                tv.texCoord = Vec2(u, v);
                tv.color = 0xFFFFFFFFu;
                // Borda da face = costura (divide posicao com a face vizinha,
                // normal diferente) - mesma marca que o load poe no chao real,
                // entao o cubo mostra o mesmo comportamento de junta.
                if (gx == 0 || gx == N || gy == 0 || gy == N) tv.matId = 0x8000u;
                m_vertices.push_back(tv);
            }
        }
        for (int gy = 0; gy < N; ++gy) {
            for (int gx = 0; gx < N; ++gx) {
                const uint32_t i0 = base + static_cast<uint32_t>(gy * (N + 1) + gx);
                const uint32_t i1 = i0 + static_cast<uint32_t>(N + 1);
                m_indices.push_back(i0); m_indices.push_back(i1); m_indices.push_back(i0 + 1);
                m_indices.push_back(i0 + 1); m_indices.push_back(i1); m_indices.push_back(i1 + 1);
            }
        }
    }
}

void SpherePreview::applyCurrentMaterialToVertices() {
    for (auto& v : m_vertices) {
        v.texIndex = static_cast<uint16_t>(m_curTexIndex);
        v.pbrIndex = static_cast<uint16_t>(m_curPbrIndex);
        v.normalIndex = static_cast<uint16_t>(m_curNormalIndex);
    }
}

void SpherePreview::setShape(Shape s) {
    if (s == m_shape && m_initialized) return;
    m_shape = s;
    if (s == Shape::Cube) buildCubeGeometry();
    else buildSphereGeometry();
    applyCurrentMaterialToVertices();
    if (m_initialized) {
        uploadVertices();
        uploadIndices();
    }
}

void SpherePreview::uploadVertices() {
    if (m_vertexMapped) std::memcpy(m_vertexMapped, m_vertices.data(), m_vertices.size() * sizeof(TerrainVertex));
}

// Reupload do indice pra' trocar esfera<->cubo - o indice NAO e' host-visible
// mapeado que nem o vertice (e' GPU_ONLY, criado uma vez com staging), entao
// precisa do mesmo caminho staging+copy que createInstanceBuffer() usa na
// primeira vez. So' seguro chamar entre frames (troca de forma e' um clique
// raro do usuario, nao todo frame) - immediateSubmit espera a fila terminar.
void SpherePreview::uploadIndices() {
    const VkDeviceSize iSize = m_indices.size() * sizeof(uint32_t);
    VkBuffer stagingBuf; VmaAllocation stagingAlloc;
    if (!m_ctx->createBuffer(iSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuf, stagingAlloc)) return;
    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, m_indices.data(), iSize);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy c{0, 0, iSize};
        vkCmdCopyBuffer(cmd, stagingBuf, m_indexBuffer, 1, &c);
    });
    vmaDestroyBuffer(m_ctx->allocator(), stagingBuf, stagingAlloc);
}

bool SpherePreview::createTargets() {
    VkDevice dev = m_ctx->device();

    if (!m_ctx->createImage(kSize, kSize, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_colorImage, m_colorAlloc)) {
        ERUPTION_LOG_ERROR("[SPHEREPREVIEW] falha ao criar imagem de cor");
        return false;
    }
    VkImageViewCreateInfo civ{};
    civ.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    civ.image = m_colorImage; civ.viewType = VK_IMAGE_VIEW_TYPE_2D; civ.format = VK_FORMAT_R8G8B8A8_UNORM;
    civ.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkc::createImageView(dev, &civ, nullptr, &m_colorView) != VK_SUCCESS) return false;

    if (!m_ctx->createImage(kSize, kSize, VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, m_depthImage, m_depthAlloc)) {
        ERUPTION_LOG_ERROR("[SPHEREPREVIEW] falha ao criar imagem de profundidade");
        return false;
    }
    VkImageViewCreateInfo div{};
    div.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    div.image = m_depthImage; div.viewType = VK_IMAGE_VIEW_TYPE_2D; div.format = VK_FORMAT_D32_SFLOAT;
    div.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkc::createImageView(dev, &div, nullptr, &m_depthView) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    m_imguiTextureId = ImGui_ImplVulkan_AddTexture(m_sampler, m_colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool SpherePreview::createInstanceBuffer() {
    VkDevice dev = m_ctx->device();

    // Vertice: buffer HOST_VISIBLE mapeado - trocar de textura e' so'
    // reescrever texIndex/pbrIndex no vetor CPU e memcpy de novo (~100 KB,
    // irrelevante). Nada de staging: e' um popup de debug, nao um asset do
    // jogo.
    const VkDeviceSize vSize = m_vertices.size() * sizeof(TerrainVertex);
    if (!m_ctx->createBuffer(vSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, m_vertexBuffer, m_vertexAlloc)) return false;
    vmaMapMemory(m_ctx->allocator(), m_vertexAlloc, &m_vertexMapped);
    uploadVertices();

    // Buffer do indice DIMENSIONADO PRA ESFERA (6144), a maior das duas
    // formas - trocar pro cubo (uploadIndices(), menos indices) so' preenche
    // uma fracao dele, nunca precisa realocar.
    const VkDeviceSize iCapacity = m_indices.size() * sizeof(uint32_t);
    if (!m_ctx->createBuffer(iCapacity, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, m_indexBuffer, m_indexAlloc)) return false;
    uploadIndices();

    // Instancia: 1 SSBO, mapeado - a rotacao e' reescrita todo frame direto
    // aqui, sem sistema de keyframe nenhum (a esfera e' totalmente propria,
    // nao passa pela lista de instancias do ModelRenderer).
    if (!m_ctx->createBuffer(sizeof(GpuInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, m_instBuffer, m_instAlloc)) return false;
    vmaMapMemory(m_ctx->allocator(), m_instAlloc, &m_instMapped);

    VkDescriptorSetLayoutBinding ib{};
    ib.binding = 0; ib.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ib.descriptorCount = 1; ib.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1; dslInfo.pBindings = &ib;
    if (vkCreateDescriptorSetLayout(dev, &dslInfo, nullptr, &m_instLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_ownPool) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = m_ownPool; dsAlloc.descriptorSetCount = 1; dsAlloc.pSetLayouts = &m_instLayout;
    if (vkAllocateDescriptorSets(dev, &dsAlloc, &m_instSet) != VK_SUCCESS) return false;

    VkDescriptorBufferInfo bi{m_instBuffer, 0, sizeof(GpuInstance)};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = m_instSet; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return true;
}

bool SpherePreview::createPipeline() {
    VkDevice dev = m_ctx->device();
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/model_novel.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/sphere_preview.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("[SPHEREPREVIEW] shaders base (model.vert/sphere_preview.frag) ausentes");
        return false;
    }
    VkShaderModule vertModule, fragModule;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t); smInfo.pCode = vertCode.data();
    vkCreateShaderModule(dev, &smInfo, nullptr, &vertModule);
    smInfo.codeSize = fragCode.size() * sizeof(uint32_t); smInfo.pCode = fragCode.data();
    vkCreateShaderModule(dev, &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0] = {}; stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertModule; stages[0].pName = "main";
    stages[1] = {}; stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragModule; stages[1].pName = "main";

    // Layout de vertice IDENTICO ao de ModelRenderer::createPipeline() -
    // reusa model.vert.spv tal e qual, o layout tem que bater exatamente.
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0; bindingDesc.stride = sizeof(TerrainVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attribs(18);
    attribs[0]  = {0,  0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, position)};
    attribs[1]  = {1,  0, VK_FORMAT_R32G32_SFLOAT,    offsetof(TerrainVertex, texCoord)};
    attribs[2]  = {2,  0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, normal)};
    attribs[3]  = {3,  0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, texIndex)};
    attribs[4]  = {4,  0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, matId)};
    attribs[5]  = {5,  0, VK_FORMAT_R32_UINT,         offsetof(TerrainVertex, color)};
    attribs[6]  = {6,  0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, pbrIndex)};
    attribs[7]  = {7,  0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, normalIndex)};
    attribs[8]  = {8,  0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, blendTexIndex)};
    attribs[9]  = {9,  0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, blendPbrIndex)};
    attribs[10] = {10, 0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, blendNormalIndex)};
    attribs[11] = {11, 0, VK_FORMAT_R32_SFLOAT,       offsetof(TerrainVertex, blendWeight)};
    attribs[12] = {12, 0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, blendMaskIndex)};
    attribs[13] = {13, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(TerrainVertex, blendMaskUV)};
    attribs[14] = {14, 0, VK_FORMAT_R32_SFLOAT,       offsetof(TerrainVertex, emissiveStrength)};
    attribs[15] = {15, 0, VK_FORMAT_R32G32_UINT,      offsetof(TerrainVertex, splatTex01)};
    attribs[16] = {16, 0, VK_FORMAT_R32G32_UINT,      offsetof(TerrainVertex, splatTex45)};
    attribs[17] = {17, 0, VK_FORMAT_R16_UINT,         offsetof(TerrainVertex, blendMaskIndex2)};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1; vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertexInput.pVertexAttributeDescriptions = attribs.data();

    // Push constant IDENTICO ao de model.vert/tesc/tese (176 B) - mesmo
    // struct, mesmos stages. sphere_preview.frag so' le' um pedacinho dele.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(Mat4) * 2 + sizeof(Vec4) * 2 + sizeof(Vec4);

    VkDescriptorSetLayout layouts[3] = {m_bindless->layout(), m_frameUboLayout, m_instLayout};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3; layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_FALSE;
    std::vector<VkPipelineColorBlendAttachmentState> blends{blend};
    std::vector<VkFormat> colorFormats{VK_FORMAT_R8G8B8A8_UNORM};

    m_pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, static_cast<float>(kSize), static_cast<float>(kSize))
        .setScissor(0, 0, kSize, kSize)
        .setPolygonMode(VK_POLYGON_MODE_FILL)
        .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE) // sem culling: deslocamento forte pode virar o triangulo do avesso e o back-face cull furava a esfera (autor: "ainda ta aberta")
        .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setBlendState(blends)
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats(colorFormats)
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(dev);

    if (m_ctx->tessellationSupported()) {
        auto tescCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/model_novel.tesc.spv");
        auto teseCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/model_novel.tese.spv");
        if (!tescCode.empty() && !teseCode.empty()) {
            VkShaderModule tescModule = VK_NULL_HANDLE, teseModule = VK_NULL_HANDLE;
            VkShaderModuleCreateInfo tsm{};
            tsm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            tsm.codeSize = tescCode.size() * sizeof(uint32_t); tsm.pCode = tescCode.data();
            vkCreateShaderModule(dev, &tsm, nullptr, &tescModule);
            tsm.codeSize = teseCode.size() * sizeof(uint32_t); tsm.pCode = teseCode.data();
            vkCreateShaderModule(dev, &tsm, nullptr, &teseModule);
            if (tescModule && teseModule) {
                VkPipelineShaderStageCreateInfo tescStage{};
                tescStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                tescStage.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
                tescStage.module = tescModule; tescStage.pName = "main";
                VkPipelineShaderStageCreateInfo teseStage = tescStage;
                teseStage.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; teseStage.module = teseModule;
                std::vector<VkPipelineShaderStageCreateInfo> tessStages = {stages[0], tescStage, teseStage, stages[1]};
                m_tessPipeline = PipelineBuilder()
                    .setShaderStages(tessStages)
                    .setVertexInput(vertexInput)
                    .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
                    .setPatchControlPoints(3)
                    .setViewport(0, 0, static_cast<float>(kSize), static_cast<float>(kSize))
                    .setScissor(0, 0, kSize, kSize)
                    .setPolygonMode(VK_POLYGON_MODE_FILL)
                    .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE) // sem culling: deslocamento forte pode virar o triangulo do avesso e o back-face cull furava a esfera (autor: "ainda ta aberta")
                    .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .setBlendState(blends)
                    .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                    .setLayout(m_pipelineLayout)
                    .setColorAttachmentFormats(colorFormats)
                    .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
                    .build(dev);
                vkDestroyShaderModule(dev, tescModule, nullptr);
                vkDestroyShaderModule(dev, teseModule, nullptr);
            }
        }
    }

    vkDestroyShaderModule(dev, vertModule, nullptr);
    vkDestroyShaderModule(dev, fragModule, nullptr);
    return m_pipeline != VK_NULL_HANDLE;
}

void SpherePreview::orbitDrag(float dYawPixels, float dPitchPixels) {
    // Sensibilidade em radianos por pixel arrastado - valor pequeno o
    // bastante pra' nao ficar "nervoso" numa janela de 384 px.
    constexpr float kSens = 0.008f;
    m_camYaw -= dYawPixels * kSens;
    m_camPitch += dPitchPixels * kSens;
    // Trava perto dos polos: em +-90 graus exatos a base (eye-target) fica
    // paralela ao "up" e lookAt degenera (camera vira/treme).
    const float limit = glm::radians(85.0f);
    m_camPitch = glm::clamp(m_camPitch, -limit, limit);
}

void SpherePreview::zoom(float wheelDelta) {
    // Multiplicativo (nao aditivo): perto do minimo o passo fica fino,
    // longe fica grosso - do contrario 1 clique de scroll ou some a esfera
    // (perto demais) ou mal move (longe demais).
    m_camDist *= std::pow(0.9f, wheelDelta);
    m_camDist = glm::clamp(m_camDist, kMinDist, kMaxDist);
}

void SpherePreview::setMaterial(uint32_t texIndex, uint32_t pbrIndex, float dispScale, uint32_t normalIndex) {
    m_curTexIndex = texIndex;
    m_curPbrIndex = pbrIndex;
    m_curNormalIndex = normalIndex;
    applyCurrentMaterialToVertices();
    uploadVertices();
    m_dispScale = dispScale;
}

void SpherePreview::render(VkCommandBuffer cmd, float dt, const Vec3& cameraPos) {
    if (!m_initialized || !m_active) return;

    // Gira sozinha, sem sistema de keyframe/animacao nenhum - so' reescreve
    // a matriz no buffer mapeado deste frame.
    if (m_spin) m_rotation += dt * (2.0f * glm::pi<float>() / 8.0f); // 1 volta a cada 8s
    if (m_rotation > 2.0f * glm::pi<float>()) m_rotation -= 2.0f * glm::pi<float>();

    // Posiciona a esfera PERTO da camera de verdade (ver o comentario no
    // header) - so' pra' a curva de tesselacao por distancia (que le'
    // u_cameraPos do FrameUBO compartilhado) julgar ela "perto" e aplicar
    // o deslocamento cheio, igual aconteceria no chao real. Ela nunca entra
    // na cena principal (desenhada so' aqui, no proprio alvo offscreen).
    const Vec3 spherePos = cameraPos + Vec3(0.0f, 0.0f, 0.001f);
    GpuInstance inst{};
    inst.model = glm::translate(Mat4(1.0f), spherePos) * glm::rotate(Mat4(1.0f), m_rotation, Vec3(0, 1, 0));
    inst.uvTranslateRot = Vec4(0.0f);
    // ESCALA DO MUNDO: UV nativa da forma (cubo = 1 textura por face de
    // 2*kRadius u) reescalada pra' densidade que a malha REAL usa com essa
    // textura (m_worldUvPerUnit, vinda do ModelRenderer). Sem isto o chao
    // (1 repeticao a cada 40 u) ia inteiro pra' uma face de 4 u - pedra 10x
    // menor que no jogo, o mip da altura borrava tudo e o cubo saia chapado.
    const float baseUvPerUnit = (m_shape == Shape::Cube)
        ? 1.0f / (2.0f * kRadius)
        : 0.5f * (1.0f / (2.0f * glm::pi<float>() * kRadius) + 1.0f / (glm::pi<float>() * kRadius));
    const float uvScale = (m_worldScale && m_worldUvPerUnit > 0.0f) ? m_worldUvPerUnit / baseUvPerUnit : 1.0f;
    const float uvPerUnit = baseUvPerUnit * uvScale;
    inst.uvScaleDisp = Vec4(uvScale, uvScale, 0.0f, 0.0f);
    std::memcpy(m_instMapped, &inst, sizeof(inst));

    // Camera PROPRIA do popup, ORBITAVEL (m_camYaw/m_camPitch/m_camDist,
    // mexidos de ImGui.cpp por arrasto/scroll - ver orbitDrag()/zoom()).
    // Coordenadas esfericas em torno de spherePos; nao e' a camera do
    // jogador, e' so' pra' enquadrar o preview.
    const float cy = std::cos(m_camPitch);
    const Vec3 eye = spherePos + m_camDist * Vec3(cy * std::cos(m_camYaw), std::sin(m_camPitch), cy * std::sin(m_camYaw));
    Mat4 view = glm::lookAt(eye, spherePos, Vec3(0, 1, 0));
    Mat4 proj = glm::perspective(glm::radians(35.0f), 1.0f, 0.1f, kMaxDist + kRadius + 5.0f);
    proj[1][1] *= -1.0f; // convencao Vulkan (Y invertido), mesma da camera principal
    Mat4 vp = proj * view;

    // Transicao pra' anexo de escrita.
    VkImageMemoryBarrier2 colorToWrite{};
    colorToWrite.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    colorToWrite.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    colorToWrite.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    colorToWrite.srcAccessMask = m_firstFrame ? 0 : VK_ACCESS_2_SHADER_READ_BIT;
    colorToWrite.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    colorToWrite.oldLayout = m_firstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorToWrite.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorToWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; colorToWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    colorToWrite.image = m_colorImage;
    colorToWrite.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    std::vector<VkImageMemoryBarrier2> pre{colorToWrite};
    m_ctx->cmdImageBarriers(cmd, pre);
    m_firstFrame = false;

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = m_colorView; colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = {{0.10f, 0.10f, 0.12f, 1.0f}};
    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = m_depthView; depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {kSize, kSize}};
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &colorAtt; ri.pDepthAttachment = &depthAtt;
    vkc::cmdBeginRendering(cmd, &ri);

    VkViewport vp2{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp2);
    VkRect2D scissor{{0, 0}, {kSize, kSize}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkPipeline pipe = (m_tessPipeline != VK_NULL_HANDLE) ? m_tessPipeline : m_pipeline;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorSet sets[3] = {m_bindless->set(), m_frameUboSet, m_instSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 3, sets, 0, nullptr);

    struct { Mat4 vp; Mat4 model; float alpha; float metallicScale; float roughnessScale; float pad; Vec4 uvTranslateRot; Vec4 uvScale; } push{};
    push.vp = vp; push.model = inst.model; push.alpha = 1.0f; push.metallicScale = 0.0f; push.roughnessScale = 1.0f;
    push.uvTranslateRot = Vec4(0.0f);
    // .w = fator de deslocamento (push.uvScale.w, mesmo campo que
    // ModelMeshGPU::dispScale) - m_dispScale, setado em setMaterial() com o
    // MESMO valor por categoria que o chao real usa (dispScaleForCategory(),
    // PbrMaterialProfile.hpp). ANTES disto ficava 1.0 fixo pra' qualquer
    // textura clicada (pedra/madeira deslocavam mais no preview que no
    // mapa - autor: "o chao continua diferente do preview"). O buraco que
    // amplitude alta abria na malha pequena nao era do VALOR em si, era
    // backface culling descartando triangulo invertido pelo deslocamento
    // grande - resolvido com VK_CULL_MODE_NONE no createPipeline().
    // .z = UV por unidade de mundo EFETIVA (calculada acima, ja' com a escala
    // do mundo) - mesmo papel da mediana da malha no chao real (dispMipFloor()).
    push.uvScale = Vec4(1.0f, 1.0f, uvPerUnit, m_dispScale);
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                       VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                       0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(m_indices.size()), 1, 0, 0, 0);

    vkc::cmdEndRendering(cmd);

    VkImageMemoryBarrier2 colorToRead = colorToWrite;
    colorToRead.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    colorToRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    colorToRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    colorToRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    colorToRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    std::vector<VkImageMemoryBarrier2> post{colorToRead};
    m_ctx->cmdImageBarriers(cmd, post);
}

} // namespace eruption
