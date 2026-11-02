#include "renderer/UpscaleAA.hpp"
#include "core/Logger.hpp"

namespace eruption {

struct FxaaPush {
    Vec2 invResolution;
};

struct UpscalePush {
    Vec2 srcSize;
    Vec2 invSrcSize;
    Vec2 invDstSize;
    float sharpenAmount;
    float _pad;
};

bool UpscaleAA::init(VulkanContext* ctx, uint32_t renderWidth, uint32_t renderHeight, VkFormat renderFormat) {
    m_ctx = ctx;
    m_device = std::make_unique<rhi::Device>(ctx);
    m_renderWidth = renderWidth;
    m_renderHeight = renderHeight;
    m_renderFormat = rhi::Device::fromVulkan(renderFormat);

    m_sampler = m_device->createSampler({});
    if (!m_sampler) return false;
    m_groupLayout = m_device->createBindGroupLayout({{0, rhi::BindingType::TextureSampler, rhi::kStageFragment}});
    if (!m_groupLayout) return false;

    m_fxaaPipeline = createFxaaPipeline();

    rhi::RenderPipelineDesc up;
    up.vertexShader = "postprocess/fullscreen.vert.spv";
    up.fragmentShader = "postprocess/upscale_sharpen.frag.spv";
    up.liteSpecialization = true; // LITE do upscale_sharpen.frag
    up.bindGroups = {m_groupLayout};
    up.pushSize = sizeof(UpscalePush);
    up.colorFormats = {m_device->swapchainFormat()};
    m_upscalePipeline = m_device->createRenderPipeline(up);

    createRenderTarget();
    return m_fxaaPipeline && m_upscalePipeline && m_fxaaTarget;
}

rhi::RenderPipeline* UpscaleAA::createFxaaPipeline() {
    rhi::RenderPipelineDesc d;
    d.vertexShader = "postprocess/fullscreen.vert.spv";
    d.fragmentShader = "postprocess/fxaa.frag.spv";
    d.liteSpecialization = true;
    d.bindGroups = {m_groupLayout};
    d.pushSize = sizeof(FxaaPush);
    d.colorFormats = {m_renderFormat};
    return m_device->createRenderPipeline(d);
}

void UpscaleAA::createRenderTarget() {
    m_fxaaTarget = m_device->createTexture(
        {m_renderWidth, m_renderHeight, m_renderFormat, rhi::kUsageRenderTarget | rhi::kUsageSampled});
    if (!m_fxaaTarget) return;
    // Criado UMA VEZ por alvo - o alvo e' estavel ate' o proximo resize (nao
    // ha' ping-pong por frame-in-flight). Reescrever o descritor TODO FRAME,
    // como a primeira versao fazia, dispara VUID-vkUpdateDescriptorSets-None-03047
    // (set atualizado enquanto um command buffer anterior ainda le' dele).
    m_upscaleGroup = m_device->createBindGroup(m_groupLayout, {{0, m_fxaaTarget, m_sampler}});
}

void UpscaleAA::destroyRenderTarget() {
    m_device->destroy(m_upscaleGroup);
    m_device->destroy(m_fxaaTarget);
    m_upscaleGroup = nullptr;
    m_fxaaTarget = nullptr;
}

void UpscaleAA::resizeRenderTarget(uint32_t renderWidth, uint32_t renderHeight, VkFormat renderFormat) {
    const rhi::Format format = rhi::Device::fromVulkan(renderFormat);
    if (renderWidth == m_renderWidth && renderHeight == m_renderHeight && format == m_renderFormat) return;
    m_device->waitIdle();
    destroyRenderTarget();
    m_renderWidth = renderWidth;
    m_renderHeight = renderHeight;
    // O FXAA tem o formato do alvo fixo na pipeline (viewport e' dinamico):
    // so' um formato novo recompila o shader, resize de janela nao.
    const bool newFormat = format != m_renderFormat;
    m_renderFormat = format;
    createRenderTarget();
    if (newFormat) {
        m_device->destroy(m_fxaaPipeline);
        m_fxaaPipeline = createFxaaPipeline();
    }
}

void UpscaleAA::bindSource(VkImageView source) {
    // O grupo antigo pode estar num command buffer em voo.
    if (m_fxaaGroup) m_device->waitIdle();
    m_device->destroy(m_fxaaGroup);
    m_device->destroy(m_source);
    // So' amostrada, nunca alvo: a imagem nao entra no embrulho.
    m_source = m_device->wrapVulkan(VK_NULL_HANDLE, source, m_renderFormat, m_renderWidth, m_renderHeight,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_fxaaGroup = m_device->createBindGroup(m_groupLayout, {{0, m_source, m_sampler}});
}

void UpscaleAA::render(VkCommandBuffer cmd, VkImageView source, VkImage sourceImage,
                       VkImageView dstView, VkImage dstImage, VkExtent2D dstExtent,
                       bool enableFXAA, float sharpenAmount) {
    // `source` chega em SHADER_READ_ONLY_OPTIMAL e e' lido pelo grupo criado
    // em bindSource(). Com FXAA desligado o passe roda igual (o upscale sempre
    // le' o alvo do FXAA, e o early-out por contraste deixa os pixels como
    // estao): um passe barato a mais em vez de dois grupos de upscale.
    (void)source;
    (void)sourceImage;
    (void)enableFXAA;
    if (!m_fxaaPipeline || !m_upscalePipeline || !m_fxaaGroup || !m_upscaleGroup) return;

    rhi::CommandList cl(cmd);
    rhi::RenderPassDesc fxaa;
    fxaa.colors = {{m_fxaaTarget, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}}};
    fxaa.width = m_renderWidth;
    fxaa.height = m_renderHeight;
    cl.beginRenderPass(fxaa);
    cl.setPipeline(m_fxaaPipeline);
    cl.setBindGroup(0, m_fxaaGroup);
    const FxaaPush fxaaPush{Vec2(1.0f / m_renderWidth, 1.0f / m_renderHeight)};
    cl.setPush(&fxaaPush, sizeof(fxaaPush));
    cl.draw(3);
    cl.endRenderPass();

    // O chamador deixa a swapchain em COLOR_ATTACHMENT_OPTIMAL e espera ela
    // assim na volta, pro ImGui desenhar em cima: e' o repouso do embrulho.
    rhi::Texture* dst = m_device->wrapVulkan(dstImage, dstView, m_device->swapchainFormat(), dstExtent.width,
                                             dstExtent.height, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rhi::RenderPassDesc up;
    up.colors = {{dst, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}}};
    up.width = dstExtent.width;
    up.height = dstExtent.height;
    cl.beginRenderPass(up);
    cl.setPipeline(m_upscalePipeline);
    cl.setBindGroup(0, m_upscaleGroup);
    UpscalePush push{};
    push.srcSize = Vec2((float)m_renderWidth, (float)m_renderHeight);
    push.invSrcSize = Vec2(1.0f / m_renderWidth, 1.0f / m_renderHeight);
    push.invDstSize = Vec2(1.0f / dstExtent.width, 1.0f / dstExtent.height);
    push.sharpenAmount = sharpenAmount;
    cl.setPush(&push, sizeof(push));
    cl.draw(3);
    cl.endRenderPass();
    m_device->destroy(dst);
}

void UpscaleAA::shutdown() {
    if (!m_ctx) return;
    destroyRenderTarget();
    m_device->destroy(m_fxaaGroup);
    m_device->destroy(m_source);
    m_device->destroy(m_fxaaPipeline);
    m_device->destroy(m_upscalePipeline);
    m_device->destroy(m_groupLayout);
    m_device->destroy(m_sampler);
    m_fxaaGroup = nullptr;
    m_source = nullptr;
    m_fxaaPipeline = m_upscalePipeline = nullptr;
    m_groupLayout = nullptr;
    m_sampler = nullptr;
    m_device.reset();
    m_ctx = nullptr;
}

} // namespace eruption
