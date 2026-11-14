#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/GBuffer.hpp"
#include "math/Types.hpp"

namespace eruption {

// REFLEXO ESPELHADO DA AGUA HD-2D (preset "water_reflection_pass").
//
// O reflexo planar em espaco de tela (sspr.comp) so' espelha o que esta' NA
// TELA: a arvore ou a casa acima da borda de cima do quadro nunca chega ao
// G-buffer e o reflexo dela some da agua perto do meio/baixo da tela - e
// "estala" de volta quando a camera anda e o objeto entra no quadro. Este
// passe desenha a cena de verdade pela camera espelhada no plano d'agua, num
// G-buffer proprio em resolucao reduzida, com o culling feito pelo frustum
// espelhado (entao objeto fora da tela entra), e acende esse G-buffer com
// sol + ambiente + emissivo (sem sombra) numa textura alinhada a' tela. O
// liquid.frag le' essa textura onde o SSPR nao achou nada, antes de cair no
// ceu.
//
// Quem desenha a geometria e' o Engine (renderWaterReflection): esta classe
// so' guarda os alvos, o composite e o slot bindless do resultado.
class WaterReflectionPass {
public:
    bool init(VulkanContext* ctx, BindlessDescriptor* bindless);
    void shutdown();

    // Tamanho do G-buffer da cena: o passe usa uma fracao dele (divisor 2 =
    // meia resolucao em cada eixo). Cria na primeira vez; recria se mudou.
    // Recriar destroi views em uso: so' com a GPU ociosa (resize do motor) ou
    // antes do primeiro uso.
    bool ensureSize(uint32_t sceneW, uint32_t sceneH, uint32_t divisor);
    // Resize do motor (GPU ociosa): so' redimensiona se ja' existia.
    void resize(uint32_t sceneW, uint32_t sceneH);
    bool ready() const { return m_colorImage != VK_NULL_HANDLE && m_pipeline != VK_NULL_HANDLE; }

    GBuffer& gbuffer() { return m_gbuffer; }
    VkExtent2D extent() const { return {m_width, m_height}; }

    struct Lighting {
        Vec3 toSun = Vec3(0.0f, 1.0f, 0.0f);
        float sunIntensity = 0.0f;
        Vec3 sunColor = Vec3(1.0f);
        float directScale = 1.0f;     // pbrLightScale / pi (o difuso do passe direcional)
        Vec3 ambientSky = Vec3(0.4f, 0.5f, 0.7f);
        float giIntensity = 1.0f;
        Vec3 ambientGround = Vec3(0.1f, 0.08f, 0.05f);
        float hemiFloor = 0.35f;
        Vec3 skyHorizon = Vec3(0.6f, 0.65f, 0.75f);
        float emissiveScale = 1.0f;
    };
    // Depois de gbuffer().endPass(): acende o G-buffer espelhado na textura
    // de reflexo e a deixa em SHADER_READ_ONLY para a agua. cloudView: as
    // nuvens vistas pela camera espelhada (CloudLayerRenderer::endMirrorView,
    // RGBA pre-multiplicado, em SHADER_READ_ONLY), compostas por cima da
    // cena; VK_NULL_HANDLE = sem nuvens neste quadro.
    void composite(VkCommandBuffer cmd, const Lighting& light, VkImageView cloudView = VK_NULL_HANDLE);

    // Slot bindless da textura de reflexo (RGB linear, A = cobertura).
    uint32_t slot() const { return m_slot; }

private:
    void destroyTargets();
    bool createPipeline();

    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;
    GBuffer m_gbuffer;
    bool m_gbufferLive = false;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_divisor = 2;

    VkImage m_colorImage = VK_NULL_HANDLE;
    VmaAllocation m_colorAlloc = VK_NULL_HANDLE;
    VkImageView m_colorView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    uint32_t m_slot = 0;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    // Um set por quadro em voo: o binding 3 (nuvens) troca de view entre
    // quadros (acumulo ou o fallback), e um set em uso pela GPU nao pode ser
    // reescrito.
    VkDescriptorSet m_sets[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkSampler m_nearest = VK_NULL_HANDLE;
};

} // namespace eruption
