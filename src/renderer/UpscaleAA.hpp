#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"

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
    VkPipeline createPipeline(const std::vector<uint32_t>& fragCode, VkFormat colorFormat,
                              VkPipelineLayout layout, uint32_t w, uint32_t h);

    VulkanContext* m_ctx = nullptr;
    uint32_t m_renderWidth = 0, m_renderHeight = 0;
    VkFormat m_renderFormat = VK_FORMAT_UNDEFINED;

    VkSampler m_sampler = VK_NULL_HANDLE; // linear, clamp-to-edge, sem comparacao

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_fxaaSet = VK_NULL_HANDLE;
    VkDescriptorSet m_upscaleSet = VK_NULL_HANDLE;

    // Dois layouts (push constants de tamanho diferente), UM set layout so'
    // (os dois passes tem a mesma forma de binding: 1 combined-image-sampler).
    VkPipelineLayout m_fxaaLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_upscaleLayout = VK_NULL_HANDLE;
    VkPipeline m_fxaaPipeline = VK_NULL_HANDLE;
    VkPipeline m_upscalePipeline = VK_NULL_HANDLE; // dynamic rendering, formato do swapchain

    // Saida do FXAA: mesma resolucao/formato da entrada de render, e' o que
    // alimenta o passe de upscale.
    VkImage m_fxaaImage = VK_NULL_HANDLE;
    VmaAllocation m_fxaaAlloc = VK_NULL_HANDLE;
    VkImageView m_fxaaView = VK_NULL_HANDLE;

    void destroyRenderTarget();
    void createRenderTarget();
    void writeFxaaSet(VkImageView sourceView);
    void writeUpscaleSet(VkImageView fxaaOutputView);
};

} // namespace eruption
