#pragma once

#include "renderer/VulkanContext.hpp"

#include <vector>
#include <cstdint>
#include <mutex>

namespace eruption {

static constexpr uint32_t MAX_BINDLESS_TEXTURES = 4096;

class BindlessDescriptor {
public:
    bool init(VulkanContext* ctx);
    void shutdown();

    uint32_t allocateSlot();
    void freeSlot(uint32_t slot);

    void updateTexture(uint32_t slot, VkImageView view, VkSampler sampler);
    void flushUpdates();

    // Thread-safe wrappers
    uint32_t allocateSlotSafe();

    // OPACIDADE POR SLOT (passe de sombra). O bake escolhe BC1 quando a
    // textura NAO tem alfa e BC3 quando tem (tools/bake_assets.py: use_bc3 =
    // has_alpha or is_data_alpha), entao o formato que chegou no upload ja' e'
    // o flag de opacidade, de graca. Serve para o caster solido pular o
    // fragment shader inteiro no shadow map - ver ShadowRenderer.
    //
    // O PADRAO E' `false` DE PROPOSITO: so' o caminho do etex cozido sabe o
    // formato; textura que sobe como RGBA8 fica marcada como "pode ter alfa" e
    // continua pagando o teste. Errar para o lado do caro nao quebra imagem;
    // errar para o lado do barato faz cerca virar retangulo solido.
    void setSlotOpaque(uint32_t slot, bool opaque) {
        if (slot >= m_slotOpaque.size()) m_slotOpaque.resize(slot + 1, 0u);
        m_slotOpaque[slot] = opaque ? 1u : 0u;
    }
    bool slotOpaque(uint32_t slot) const {
        return slot < m_slotOpaque.size() && m_slotOpaque[slot] != 0u;
    }
    void freeSlotSafe(uint32_t slot);
    void updateTextureSafe(uint32_t slot, VkImageView view, VkSampler sampler);
    void flushUpdatesSafe();
    // Diagnostico: slots em uso e marca d'agua (o maior indice ja' entregue).
    // Se highWater encosta em MAX_BINDLESS_TEXTURES e usedSlots nao cai entre
    // cargas de mapa, alguem esta' vazando slot (ver MapLoad.cpp).
    // A free list nasce cheia (init empurra 1..MAX-1), entao "usado" e' o que
    // saiu dela e nao voltou - nao depende de m_nextSlot.
    uint32_t usedSlots() const { return m_allocated; }
    uint32_t highWater() const { return m_highWater; }

    VkDescriptorSetLayout layout() const { return m_layout; }
    VkDescriptorSet set() const { return m_set; }

private:
    std::vector<uint8_t> m_slotOpaque;
    VulkanContext* m_ctx = nullptr;

    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    // Default texture (slot 0)
    VkImage m_defaultImage = VK_NULL_HANDLE;
    VmaAllocation m_defaultAlloc = VK_NULL_HANDLE;
    VkImageView m_defaultView = VK_NULL_HANDLE;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;

    std::vector<uint32_t> m_freeSlots;
    uint32_t m_nextSlot = 1; // slot 0 reserved for default/null
    uint32_t m_allocated = 0;  // slots entregues e ainda nao devolvidos
    uint32_t m_highWater = 0;  // maximo simultaneo de m_allocated

    struct PendingUpdate {
        uint32_t slot;
        VkImageView view;
        VkSampler sampler;
    };
    std::vector<PendingUpdate> m_pendingUpdates;

    bool createLayout();
    bool createPool();
    bool allocateSet();

    std::mutex m_mutex;
};

} // namespace eruption
