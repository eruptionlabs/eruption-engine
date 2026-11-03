#pragma once

#include "renderer/TextureLayers.hpp"
#include "renderer/VulkanContext.hpp"

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <mutex>

namespace eruption {

static constexpr uint32_t MAX_BINDLESS_TEXTURES = 4096;
// Teto com descriptor indexing, se a GPU deixar: o splat guarda o slot em 14
// bits (model_frag.glsl, & 0x3FFF), entao nao passa de 16384.
static constexpr uint32_t MAX_BINDLESS_TEXTURES_INDEXED = 16384;

class BindlessDescriptor {
public:
    bool init(VulkanContext* ctx);
    void shutdown();
    // Lado pedido da camada de textura (antes de init); 0 = o padrao.
    void setLayerSizeRequest(uint32_t size) { m_layerSizeRequest = size; }

    uint32_t allocateSlot();
    void freeSlot(uint32_t slot);
    // N slots livres SEGUIDOS: a camada do splat guarda cor, normal e MRAH-W
    // em s, s+1 e s+2 e o shader acha os dois ultimos pelo primeiro.
    // 0 = sem sequencia livre. Cada slot volta sozinho por freeSlot().
    uint32_t allocateRange(uint32_t count);

    // `dynamic`: o conteúdo da imagem muda depois do registro (render target).
    // Só faz diferença sem descriptor indexing (TextureLayers), onde a textura
    // comum é copiada uma vez para uma camada e a dinâmica fica ligada direto.
    void updateTexture(uint32_t slot, VkImageView view, VkSampler sampler, bool dynamic = false);
    void flushUpdates();
    // Sampler sem filtro (pixel art, paleta): sem descriptor indexing a
    // textura precisa ir para a ligação "nearest" das camadas.
    void markNearestSampler(VkSampler sampler);

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
    void updateTextureSafe(uint32_t slot, VkImageView view, VkSampler sampler, bool dynamic = false);
    void flushUpdatesSafe();
    // Para trabalho gravado fora do laço de frames (bakes do carregamento):
    // no caminho de camadas, espera a GPU e aplica as escritas pendentes em
    // todos os sets, senão o bake vê a textura padrão no lugar das novas.
    // Não chamar com um frame sendo gravado.
    void syncNow();
    // Diagnostico: slots em uso e marca d'agua (o maior indice ja' entregue).
    // Se highWater encosta em MAX_BINDLESS_TEXTURES e usedSlots nao cai entre
    // cargas de mapa, alguem esta' vazando slot (ver MapLoad.cpp).
    // A free list nasce cheia (init empurra 1..MAX-1), entao "usado" e' o que
    // saiu dela e nao voltou.
    uint32_t usedSlots() const { return m_allocated; }
    uint32_t highWater() const { return m_highWater; }

    VkDescriptorSetLayout layout() const { return m_layout; }
    // Com camadas (sem descriptor indexing) ha' um set por frame em voo.
    VkDescriptorSet set() const { return m_layered ? m_frameSets[m_ctx->currentFrame()] : m_set; }
    uint32_t slotCount() const { return m_slotCount; }
    // Lado das camadas (0 = com descriptor indexing, sem camadas). Textura
    // maior que isto só ocupa memória: a cópia reamostra para este lado.
    uint32_t layerSize() const { return m_layered ? m_layers.layerSize() : 0; }

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
    uint32_t m_allocated = 0;  // slots entregues e ainda nao devolvidos
    uint32_t m_highWater = 0;  // maximo simultaneo de m_allocated

    struct PendingUpdate {
        uint32_t slot;
        VkImageView view; // VK_NULL_HANDLE nas camadas: slot liberado
        VkSampler sampler;
        bool dynamic = false;
    };
    std::vector<PendingUpdate> m_pendingUpdates;

    // Caminho sem descriptor indexing (GPU de celular): as texturas vão para
    // camadas de sampler2DArray (TextureLayers) e um set em uso não pode mudar
    // (sem UPDATE_AFTER_BIND); cada set é atualizado no início do seu frame.
    bool m_layered = false;
    uint32_t m_slotCount = MAX_BINDLESS_TEXTURES;
    VkDescriptorSet m_frameSets[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    TextureLayers m_layers;
    void writeSet(VkDescriptorSet set, const std::vector<PendingUpdate>& updates);
    void onFrameBegin(uint32_t frame);

    bool createLayout();
    bool createPool();
    bool allocateSet();

    std::mutex m_mutex;
    uint32_t m_layerSizeRequest = 0;
};

} // namespace eruption
