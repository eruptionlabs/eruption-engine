#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace eruption {

class VulkanContext;

// Textura R8 da visao de equipe (core/Vision escreve no CPU; a composicao
// amostra). Um staging mapeado por quadro em voo: o quadro anterior ainda
// pode estar copiando do seu.
class VisionMap {
public:
    // alphaMask: a view le' (0, 0, 0, R) - uma camada preta com o texel no
    // alfa, para desenhar por cima de uma imagem com mistura alfa comum.
    bool init(VulkanContext* ctx, uint32_t texels, bool alphaMask = false);
    void shutdown();
    // Recria se o lado mudou (preset ou vision.enable com outra janela).
    bool ensureSize(uint32_t texels);
    // Grava os texels e copia para a imagem; fora de um passe de render.
    void upload(VkCommandBuffer cmd, const uint8_t* texels, uint32_t side);
    VkImageView view() const { return m_view; }
    uint32_t side() const { return m_side; }

private:
    static constexpr uint32_t kFrames = 3;
    bool create(uint32_t texels);
    void destroy(bool deferred);

    VulkanContext* m_ctx = nullptr;
    uint32_t m_side = 0;
    bool m_alphaMask = false;
    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_alloc = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkBuffer m_staging[kFrames] = {};
    VmaAllocation m_stagingAlloc[kFrames] = {};
    void* m_mapped[kFrames] = {};
};

} // namespace eruption
