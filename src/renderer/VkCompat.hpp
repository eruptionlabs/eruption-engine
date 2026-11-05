#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace eruption::vkc {

// Camada de compatibilidade com Vulkan 1.1. O motor descreve passes e
// barreiras com as estruturas de dynamic rendering e synchronization2 (mais
// legíveis), e estas funções traduzem para o que existe no 1.1: render pass +
// framebuffer e vkCmdPipelineBarrier. Um caminho só, em qualquer aparelho.
//
// Os framebuffers são criados por passe e destruídos quando o mesmo slot de
// frame volta (a fence dele já garantiu que a GPU terminou). Render passes
// ficam em cache pela descrição (formatos, operações e layouts).

void init(VkDevice device, uint32_t framesInFlight);
void shutdown();
// Chamar depois de esperar a fence do slot `frameIndex`.
void beginFrame(uint32_t frameIndex);

// Mesma assinatura das funções do Vulkan.
VkResult createImageView(VkDevice device, const VkImageViewCreateInfo* info,
                         const VkAllocationCallbacks* alloc, VkImageView* view);
// Formato com que a view foi criada por createImageView (UNDEFINED se não foi).
VkFormat viewFormat(VkImageView view);
// Imagem, tipo e faixa de uma view criada por createImageView (false se não foi).
struct ViewInfo {
    VkImage image = VK_NULL_HANDLE;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageSubresourceRange range{};
};
bool viewInfo(VkImageView view, ViewInfo& out);
VkResult createGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                 const VkGraphicsPipelineCreateInfo* infos,
                                 const VkAllocationCallbacks* alloc, VkPipeline* pipelines);
void cmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info);
void cmdEndRendering(VkCommandBuffer cmd);
void cmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dep);

// Chamado depois de cada pipeline gráfica criada: a tela de carregamento conta
// a compilação de shaders (no celular, minutos na primeira vez). A cena liga
// o gancho só enquanto mostra a tela; fora dela pipelines nascem no meio de um
// frame e não podem desenhar nada.
using PipelineHook = void (*)(void* user);
class PipelineHookScope {
public:
    PipelineHookScope(PipelineHook hook, void* user);
    ~PipelineHookScope();
    PipelineHookScope(const PipelineHookScope&) = delete;
    PipelineHookScope& operator=(const PipelineHookScope&) = delete;

private:
    PipelineHook m_prevHook;
    void* m_prevUser;
};

// STORE_OP_NONE nativo (VK_KHR/EXT_load_store_op_none ligada no device, spec
// 52): o anexo so' lido no passe nao e' gravado de volta. Desligado, NONE vira
// STORE como antes (mesmo conteudo, gravacao inteira).
void setStoreOpNone(bool native);
bool storeOpNone();

// Render pass compatível com estes formatos (para pipelines feitas fora do
// motor, como a da interface).
VkRenderPass compatibleRenderPass(const std::vector<VkFormat>& colors, VkFormat depth, VkFormat stencil);

} // namespace eruption::vkc
