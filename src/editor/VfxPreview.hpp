#pragma once

// PREVIA DE EFEITO do editor: o efeito sendo editado sozinho, num fundo
// neutro, com camera orbital propria. Usa um VfxSystem proprio (pool pequeno)
// para nao misturar particulas com a cena; o desenho e a simulacao sao os
// mesmos do jogo, so' a imagem final passa por um tonemap simples para a
// interface.

#include "math/Camera.hpp"
#include "math/Types.hpp"
#include "renderer/VulkanContext.hpp"
#include "renderer/vfx/VfxSystem.hpp"

#include <array>
#include <string>

namespace eruption {

class Engine;

class VfxPreview {
public:
    bool init(Engine& engine);
    void shutdown();
    bool ready() const { return m_ready; }

    // Toca `desc` (recomeca do zero). Edicao ao vivo: chamar de novo com a
    // descricao alterada continua a mesma instancia sem recomecar.
    void play(const std::string& name, const VfxEffectDesc& desc, bool restart);
    void restart();
    bool playing = true;
    float speed = 1.0f;

    // Uma vez por frame, fora de passe de render (gancho offscreen do motor).
    void render(VkCommandBuffer cmd, float dt);
    void setSize(uint32_t w, uint32_t h);
    void* texture() const { return m_texture; }

    // Camera
    void orbit(float dx, float dy);
    void pan(float dx, float dy);
    void zoom(float wheel);
    void frame(); // enquadra pelo tamanho tipico do efeito
    float effectTime() const;
    const VfxSystem::Stats& stats() const { return m_sys.stats(); }

private:
    bool createTargets();
    void destroyTargets();
    bool createTonemap();

    Engine* m_engine = nullptr;
    VulkanContext* m_ctx = nullptr;
    bool m_ready = false;
    VfxSystem m_sys;
    Camera m_cam;
    std::string m_name;
    uint32_t m_instance = 0;
    float m_yaw = 0.6f, m_pitch = 0.2f, m_dist = 6.0f; // metros
    Vec3 m_target{0.0f, 1.2f, 0.0f};

    // FrameUBO proprio (um por frame em voo) com o layout do da cena.
    VkDescriptorPool m_uboPool = VK_NULL_HANDLE;
    std::array<VkBuffer, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_ubo{};
    std::array<VmaAllocation, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_uboAlloc{};
    std::array<void*, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_uboMapped{};
    std::array<VkDescriptorSet, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_uboSets{};
    bool m_sysReady = false;

    // Alvos: HDR + profundidade (efeito) e LDR (interface).
    uint32_t m_w = 0, m_h = 0, m_wantW = 512, m_wantH = 384;
    VkImage m_hdr = VK_NULL_HANDLE, m_depth = VK_NULL_HANDLE, m_ldr = VK_NULL_HANDLE;
    VmaAllocation m_hdrAlloc = VK_NULL_HANDLE, m_depthAlloc = VK_NULL_HANDLE, m_ldrAlloc = VK_NULL_HANDLE;
    VkImageView m_hdrView = VK_NULL_HANDLE, m_depthView = VK_NULL_HANDLE, m_ldrView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    void* m_texture = nullptr;
    bool m_ldrReadable = false;

    // Tonemap
    VkDescriptorSetLayout m_tmLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_tmPipeLayout = VK_NULL_HANDLE;
    VkPipeline m_tmPipe = VK_NULL_HANDLE;
    VkDescriptorPool m_tmPool = VK_NULL_HANDLE;
    VkDescriptorSet m_tmSet = VK_NULL_HANDLE;
};

} // namespace eruption
