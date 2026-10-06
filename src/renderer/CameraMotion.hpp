#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"

namespace eruption { class Camera; }

namespace eruption {

// Motion vectors so' de camera, na resolucao de render, reconstruidos do
// depth do G-buffer (shaders/compute/camera_motion.comp). Entrada do FSR.
//
// Formato: RG16F, em UV, direcao atual -> anterior (mv = uvAnterior - uvAtual),
// SEM jitter. Objetos com movimento proprio nao aparecem aqui.
class CameraMotion {
public:
    bool init(VulkanContext* ctx, uint32_t width, uint32_t height);
    void shutdown();
    // Recria o alvo no tamanho novo. `depthView` = depth do G-buffer
    // (precisa ser chamado de novo sempre que o G-buffer recriar a view).
    void resize(uint32_t width, uint32_t height);
    void bindDepth(VkImageView depthView);
    // Velocidade de objeto do G-buffer (opcional; VK_NULL_HANDLE = so' camera).
    void bindObjectVelocity(VkImageView velocityView);

    // O depth precisa estar em SHADER_READ_ONLY_OPTIMAL. Ao voltar, o alvo
    // fica em SHADER_READ_ONLY_OPTIMAL, visivel para compute e fragment.
    // `camera` com o jitter DESTE frame ja' aplicado (o mesmo que rasterizou
    // o depth); `prevViewProjNoJitter` = viewProjNoJitter do frame anterior.
    void dispatch(VkCommandBuffer cmd, const Camera& camera, const Mat4& prevViewProjNoJitter);

    // Teste: grava o alvo atual em `path` (cabecalho "MV2" + largura +
    // altura em uint32, depois RG float32 por pixel). Espera a GPU inteira.
    bool dumpToFile(const char* path);

    VkImage image() const { return m_image; }
    VkImageView view() const { return m_view; }
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16_SFLOAT;

private:
    void createTarget();
    void destroyTarget();
    void writeSet();

    VulkanContext* m_ctx = nullptr;
    uint32_t m_width = 0, m_height = 0;
    VkImageView m_depthView = VK_NULL_HANDLE;
    VkImageView m_objectVelocityView = VK_NULL_HANDLE;

    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_alloc = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace eruption
