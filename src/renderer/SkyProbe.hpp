#pragma once
// SONDA DE CEU (G36 / P2.2): cubemap pre-filtrado do ceu procedural.
//
// O ambiente da engine era um gradiente analitico de tres cores (zenite,
// horizonte, chao) - o alargamento em forma fechada do directional.frag JA
// ERA o "prefiltro" desse gradiente. O que faltava era tudo que o skybox.frag
// desenha e o gradiente nao ve: nuvens (cobertura escurece e achata o
// ambiente), a assimetria do ceu em torno do sol (lado do sol mais claro),
// a tempestade (stormTint), o crepusculo. Aqui o proprio skybox.frag e'
// renderizado em 6 faces de 32x32 (sem disco solar - o sol e' luz direcional),
// os mips vao por blit (especular pre-filtrado: roughness -> lod, aproximacao
// classica para ceu suave) e um compute de um workgroup projeta a face 0 em
// SH9 (irradiancia difusa, Ramamoorthi & Hanrahan 2001).
//
// Custo: 6 draws de 1024 px + 5 blits + 1 dispatch, a cada N frames. Nao
// depende da cena, so' da hora e do clima.
#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"
#include "utils/DayNightCycle.hpp"

namespace eruption {

class SkySystem;

class SkyProbe {
public:
    bool init(VulkanContext* ctx, uint32_t faceSize = 32);
    void shutdown();

    // Regrava as 6 faces via `sky`, gera os mips e projeta em SH. Chamar FORA
    // de qualquer render pass. Faz as barreiras que precisa.
    void update(VkCommandBuffer cmd, SkySystem& sky, const DayNightCycle& cycle);

    VkImageView cubeView() const { return m_cubeView; }
    VkSampler sampler() const { return m_sampler; }
    VkBuffer shBuffer() const { return m_shBuffer; }
    VkDeviceSize shBufferSize() const { return kShBytes; }
    uint32_t mipCount() const { return m_mipCount; }
    uint32_t faceSize() const { return m_faceSize; }
    bool ready() const { return m_updates > 0; }
    uint32_t updates() const { return m_updates; }

    static constexpr VkDeviceSize kShBytes = 9 * 4 * sizeof(float);

private:
    bool createCube();
    bool createCompute();

    VulkanContext* m_ctx = nullptr;
    uint32_t m_faceSize = 32;
    uint32_t m_mipCount = 1;
    uint32_t m_updates = 0;

    VkImage m_cube = VK_NULL_HANDLE;
    VmaAllocation m_cubeAlloc = VK_NULL_HANDLE;
    VkImageView m_cubeView = VK_NULL_HANDLE;   // samplerCube, todos os mips
    VkImageView m_faceViews[6] = {};           // 2D, mip 0, uma camada
    VkImage m_depth = VK_NULL_HANDLE;
    VmaAllocation m_depthAlloc = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkBuffer m_shBuffer = VK_NULL_HANDLE;
    VmaAllocation m_shAlloc = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace eruption
