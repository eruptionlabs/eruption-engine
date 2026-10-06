#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"

#include <array>

namespace eruption {

class SpriteRenderer;

// Camada de sprites para o FSR (pixel art nitido e estavel com upscaler
// temporal). Tres pecas por frame:
//   1. renderMask: cobertura dos sprites na resolucao de render (mesmo vertex
//      shader do G-buffer, depth do G-buffer como teste);
//   2. buildReactive: max(mascara atual, anterior) -> mascara reativa do FSR,
//      que deixa de usar historico onde ha' ou havia sprite;
//   3. renderLayer: sprites redesenhados na resolucao de display, SEM jitter,
//      por cima da saida do FSR, com a luz que o deferred calculou para eles
//      (ver sprite_layer.frag).
class SpriteLayer {
public:
    bool init(VulkanContext* ctx, SpriteRenderer* sprites, uint32_t renderW, uint32_t renderH);
    void shutdown();
    void resize(uint32_t renderW, uint32_t renderH);
    // Entradas da camada (todas na resolucao de render, SHADER_READ_ONLY).
    void bindLayerInputs(VkImageView depth, VkImageView lit, VkImageView albedo);

    // Precisa do depth do G-buffer em DEPTH_READ_ONLY_OPTIMAL.
    void renderMask(VkCommandBuffer cmd, VkImageView depthView, VkBuffer instances, uint32_t count,
                    const Mat4& viewProjJittered);
    void buildReactive(VkCommandBuffer cmd);
    // `target` em SHADER_READ_ONLY_OPTIMAL na entrada e na saida.
    void renderLayer(VkCommandBuffer cmd, VkImage target, VkImageView targetView, VkExtent2D targetExtent,
                     VkBuffer instances, uint32_t count, const Mat4& view, const Mat4& projNoJitter,
                     float nearZ, float farZ);

    VkImageView reactiveView() const { return m_reactive.view; }

private:
    struct Img {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    void createTargets();
    void destroyTargets();
    void writeReactiveSets();

    VulkanContext* m_ctx = nullptr;
    SpriteRenderer* m_sprites = nullptr;
    uint32_t m_w = 0, m_h = 0;
    std::array<Img, 2> m_mask;
    Img m_reactive;
    uint32_t m_current = 0;   // qual mascara recebe o frame atual

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_reactiveSetLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> m_reactiveSets{}; // [qual mascara e' a atual]
    VkPipelineLayout m_reactiveLayout = VK_NULL_HANDLE;
    VkPipeline m_reactivePipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layerSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_layerSet = VK_NULL_HANDLE;
};

} // namespace eruption
