#pragma once

#include "math/Types.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace eruption {

class VulkanContext;

// LUZ VOLUMETRICA DO SOL ("raios" no ar): marcha da camera ate' o pixel (ou
// ate' max_distance) em 1/2 ou 1/4 da resolucao, testando o mapa de sombra
// do sol (o mesmo atlas e ShadowUBO do directional.frag) em cada passo. O ar
// tem a densidade da nevoa de altura do PostSettings (mais denso embaixo);
// espalhamento Henyey-Greenstein para frente; dither de ruido de gradiente
// intercalado (IGN) FIXO no tempo, para a captura repetir igual. Um borrao
// bilateral (respeita a profundidade) tira o ruido do dither. O composite soma
// o resultado antes da nevoa (binding 15; sem o efeito, 1x1 preta).
//
// Preset "volumetric_light" (0 desliga, 1 = meia resolucao, 2 = um quarto),
// "volumetric_steps", "volumetric_max_distance"; look: volumetric_intensity,
// volumetric_density, volumetric_tint. ERUPTION_TEST_NO_VOLUMETRIC=1 desliga.
class VolumetricLight {
public:
    struct Params {
        Mat4 invViewProj;
        Vec3 cameraPos;
        Vec3 toSun;          // direcao PARA o sol (normalizada)
        Vec3 sunRadiance;    // cor x intensidade do sol (unidades da cena)
        Vec3 tint = Vec3(1.0f);
        float intensity = 0.0f;
        float density = 0.0f;   // coeficiente de espalhamento na altura da nevoa (1/u)
        float fogHeight = 0.0f;
        float fogFalloff = 0.0f;
        float maxDistance = 600.0f;
        float anisotropy = 0.6f;
        int steps = 24;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
    };

    bool init(VulkanContext* ctx, uint32_t renderW, uint32_t renderH, int mode);
    void shutdown();
    void resize(uint32_t renderW, uint32_t renderH);
    void setMode(int mode); // recria os alvos se a escala mudar
    int mode() const { return m_mode; }

    // Grava a marcha + borrao. Sem o efeito (modo 0, intensidade 0), so'
    // garante a imagem 1x1 preta pronta. Devolve o tempo de GPU do quadro
    // anterior (timestamps 16..17).
    void render(VkCommandBuffer cmd, VkImageView sceneDepth, VkImageView shadowAtlas, VkBuffer shadowUbo,
                const Params& p);
    // O que o composite le' (binding 15): o resultado ou a 1x1 preta.
    VkImageView outputView() const { return m_active ? m_views[0] : m_fallbackView; }
    bool active() const { return m_active; }

private:
    void createTargets();
    void destroyTargets();
    void createPipelines();

    VulkanContext* m_ctx = nullptr;
    uint32_t m_renderW = 0, m_renderH = 0, m_w = 0, m_h = 0;
    int m_mode = 0;
    bool m_active = false;
    bool m_fallbackReady = false;

    VkImage m_images[2] = {};
    VmaAllocation m_allocs[2] = {};
    VkImageView m_views[2] = {};
    VkImage m_fallback = VK_NULL_HANDLE;
    VmaAllocation m_fallbackAlloc = VK_NULL_HANDLE;
    VkImageView m_fallbackView = VK_NULL_HANDLE;

    VkSampler m_pointSampler = VK_NULL_HANDLE;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    static constexpr uint32_t kSetsPerFrame = 3; // marcha, borrao H, borrao V
    static constexpr uint32_t kMaxFrames = 3;
    VkDescriptorSet m_sets[kSetsPerFrame * kMaxFrames] = {};
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_marchPipeline = VK_NULL_HANDLE;
    VkPipeline m_blurPipeline = VK_NULL_HANDLE;
};

} // namespace eruption
