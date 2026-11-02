#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"
#include "rhi/Rhi.hpp"

#include <memory>

namespace eruption {

// Substitui o blit BILINEAR cru (VulkanContext::copyImageToImageScaled,
// VK_FILTER_LINEAR) que copiava a imagem de RENDER pro swapchain de DISPLAY.
// Nao existia AA nenhum na engine (grep fxaa|smaa|cmaa|taa = zero); a diferenca
// entre render_scale e a janela era coberta so' por esse bilinear cego.
//
// Duas etapas, dois passes:
//   1) FXAA na resolucao de RENDER (fxaa.frag) - resolve serrilha de
//      silhueta que sobra depois do specular AA (esse resolve cintilacao de
//      brilho, nao aliasing geometrico).
//   2) Upscale bicubico Catmull-Rom + nitidez adaptativa por contraste local
//      (upscale_sharpen.frag), escrevendo DIRETO no swapchain - substitui o
//      blit e a etapa de nitidez num passe so'.
//
// Ver o cabecalho dos dois shaders para o porque de NAO ser SMAA (precisa de
// LUT externa) nem um port byte-a-byte do FSR1 da AMD (pesos nao-verificaveis
// de memoria).
class UpscaleAA {
public:
    bool init(VulkanContext* ctx, uint32_t renderWidth, uint32_t renderHeight,
              VkFormat renderFormat);
    void shutdown();
    void resizeRenderTarget(uint32_t renderWidth, uint32_t renderHeight, VkFormat renderFormat);

    // `source` = saida do PostProcessor, na resolucao de RENDER, formato
    // `renderFormat` passado a init()/resizeRenderTarget(). `dstView`/`dstExtent`
    // = a imagem do SWAPCHAIN (ja' em COLOR_ATTACHMENT_OPTIMAL) e sua extensao.
    // Ao voltar, a imagem de destino permanece em COLOR_ATTACHMENT_OPTIMAL
    // (mesma pos-condicao que o blit antigo deixava, pro ImGui desenhar
    // encima em seguida).
    void render(VkCommandBuffer cmd, VkImageView source, VkImage sourceImage,
                VkImageView dstView, VkImage dstImage, VkExtent2D dstExtent,
                bool enableFXAA, float sharpenAmount);

    // `source` = a MESMA view que sera' passada a render() todo frame (a
    // saida do PostProcessor - view ESTAVEL, o PostProcessor nao faz
    // ping-pong por frame-in-flight). Chamar de novo se a view mudar (nunca
    // muda em uso normal; so' teoricamente se o PostProcessor for recriado).
    void bindSource(VkImageView source);

private:
    // Primeiro renderer na RHI (rhi/Rhi.hpp): o mesmo codigo serve o alvo web.
    rhi::RenderPipeline* createFxaaPipeline();
    void destroyRenderTarget();
    void createRenderTarget();
    void rebuildFxaaGroup();

    VulkanContext* m_ctx = nullptr;
    std::unique_ptr<rhi::Device> m_device;
    uint32_t m_renderWidth = 0, m_renderHeight = 0;
    rhi::Format m_renderFormat = rhi::Format::Undefined;

    rhi::Sampler* m_sampler = nullptr; // linear, clamp-to-edge, sem comparacao
    // Um layout so' (os dois passes tem a mesma forma: 1 textura amostrada).
    rhi::BindGroupLayout* m_groupLayout = nullptr;
    rhi::Texture* m_source = nullptr; // embrulho da saida do PostProcessor/FSR
    rhi::BindGroup* m_fxaaGroup = nullptr;
    rhi::BindGroup* m_upscaleGroup = nullptr;
    rhi::RenderPipeline* m_fxaaPipeline = nullptr;
    rhi::RenderPipeline* m_upscalePipeline = nullptr; // formato da swapchain

    // Saida do FXAA: mesma resolucao/formato da entrada de render, e' o que
    // alimenta o passe de upscale.
    rhi::Texture* m_fxaaTarget = nullptr;
};

} // namespace eruption
