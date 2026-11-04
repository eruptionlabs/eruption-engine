#pragma once

#include "renderer/VulkanContext.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace eruption {

// Texturas por slot numa GPU sem descriptor indexing e com poucos samplers por
// estágio (PowerVR/Mali de celular: 16 a 32, sem índice variável em array de
// samplers). Cada textura de conteúdo é copiada, num tamanho só, para uma
// camada de um sampler2DArray RGBA8 (as sRGB com o valor codificado; o shader
// decodifica); o shader acha a camada numa tabela por slot
// (shaders/common/slot_textures.glsl). Render targets e
// formatos fora de 8 bits por canal (float, profundidade) não cabem na cópia e
// ficam numa das poucas ligações diretas.
//
// Quem usa é o BindlessDescriptor: ele guarda os slots e chama assign/release;
// as cópias acontecem em apply/flush, fora da gravação de um frame.
class TextureLayers {
public:
    // Ligações do set 0, na numeração de slot_textures.glsl.
    static constexpr uint32_t kLayersBinding = 0;
    static constexpr uint32_t kLayersNearestBinding = 1;
    static constexpr uint32_t kTableBinding = 2;
    static constexpr uint32_t kDirectBinding = 3;
    static constexpr uint32_t kDirectCount = 8;
    static constexpr uint32_t kBindingCount = kDirectBinding + kDirectCount;

    // Ligações e tamanhos de pool do set 0 neste modo.
    static std::vector<VkDescriptorSetLayoutBinding> layoutBindings();
    static std::vector<VkDescriptorPoolSize> poolSizes(uint32_t sets);

    // `layerSize`: lado de cada camada (as texturas são reamostradas para ele).
    // `sets`: os sets do BindlessDescriptor, um por frame em voo.
    bool init(VulkanContext* ctx, uint32_t slotCount, uint32_t layerSize, const VkDescriptorSet* sets,
              VkImageView defaultView, VkSampler defaultSampler);
    void shutdown();

    // Sampler sem filtro (pixel art, paleta): a textura é lida pela ligação
    // "nearest" do array.
    void markNearest(VkSampler sampler) { m_nearestSamplers.insert(sampler); }

    // `dynamic`: o conteúdo muda depois do registro (render target); vai
    // direto, sem cópia.
    void assign(uint32_t slot, VkImageView view, VkSampler sampler, bool dynamic);
    void release(uint32_t slot);

    // Começo do frame `frame` (a fence dele já foi esperada): faz as cópias
    // pendentes e atualiza o set e a tabela desse frame.
    void apply(uint32_t frame);
    // Faz as cópias pendentes e atualiza todos os frames (espera a GPU).
    void applyAll();

    uint32_t layerSize() const { return m_layerSize; }

    // Slots cuja textura ESTATICA ja' foi copiada para uma camada: a imagem de
    // origem nao e' mais lida pela GPU (a copia e' sincrona) e quem a possui
    // pode libera-la. Devolve a lista e a esvazia.
    std::vector<uint32_t> takeCopiedStatic();

    // Situacao do array de camadas (telemetria): camadas em uso, capacidade, lado
    // e memoria da alocacao (inclui as camadas ainda livres e os mips).
    struct Stats {
        uint32_t used = 0, capacity = 0, side = 0;
        uint32_t waiting = 0; // copias ainda por fazer (cada uma segura a imagem de origem)
        double mb = 0.0;
    };
    Stats stats() const {
        Stats s;
        s.side = m_layerSize;
        s.capacity = m_array.capacity;
        s.waiting = static_cast<uint32_t>(m_jobs.size());
        const uint32_t taken = m_array.next > 0 ? m_array.next - 1 : 0; // a camada 0 e' a padrao
        const uint32_t freed = static_cast<uint32_t>(m_array.freeLayers.size());
        s.used = taken > freed ? taken - freed : 0;
        if (m_ctx && m_array.alloc) s.mb = static_cast<double>(m_ctx->allocationSize(m_array.alloc)) / (1024.0 * 1024.0);
        return s;
    }

private:
    struct LayerArray {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t capacity = 0;
        uint32_t next = 1; // a camada 0 é a textura padrão
        std::vector<uint32_t> freeLayers;
    };
    // Onde um slot está: kind 0-3 = camada (bit 0 sRGB, bit 1 sem filtro),
    // 4+k = ligação direta k, kNone = textura padrão.
    struct Place {
        uint32_t kind;
        uint32_t layer;
    };
    struct CopyJob {
        VkImageView view;
        VkSampler sampler;
        bool nearest;
        bool srgb;
    };
    struct Direct {
        uint32_t slot = UINT32_MAX;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    static constexpr uint32_t kNone = UINT32_MAX;
    static constexpr uint32_t kDirectKind = 4;
    // Cópias por envio: na PowerVR GE8320 cada cópia (camada com 9 mips) leva
    // ~13 ms de GPU, e 64 num envio davam ~0,9 s, na beira do ~1 s que prende
    // a GPU até reiniciar (travou nos primeiros quadros depois da carga).
    static constexpr uint32_t kCopyBatch = 8;
    // Por quadro em apply(): sempre kCopiesPerFrame e, com fila, mais lotes
    // até kCopyBudgetMs (CPU, já com a espera da GPU de cada envio). No web a
    // cópia custa ~0,02 ms e o teto fixo de 16 levava ~38 s para esvaziar 427
    // camadas; na PowerVR um lote de 8 já passa do orçamento, e fica como era.
    // O resto vai para os quadros seguintes (o slot lê a textura padrão até
    // lá). applyAll copia tudo.
    static constexpr uint32_t kCopiesPerFrame = 16;
    static constexpr double kCopyBudgetMs = 8.0;
    // Camadas por envio ao crescer a matriz (cópia da antiga para a nova).
    static constexpr uint32_t kGrowChunk = 32;
    static constexpr uint32_t kFrames = VulkanContext::MAX_FRAMES_IN_FLIGHT;

    VulkanContext* m_ctx = nullptr;
    uint32_t m_layerSize = 256;
    uint32_t m_mipLevels = 1;
    LayerArray m_array;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    VkImageView m_defaultView = VK_NULL_HANDLE;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    std::unordered_set<VkSampler> m_nearestSamplers;

    VkDescriptorSet m_sets[kFrames] = {};
    VkBuffer m_tableBuffer[kFrames] = {};
    VmaAllocation m_tableAlloc[kFrames] = {};
    uint32_t* m_tableMapped[kFrames] = {};
    std::vector<uint32_t> m_table; // cópia mestra, vai para o buffer do frame
    std::vector<Place> m_places;
    Direct m_direct[kDirectCount];
    std::unordered_map<uint32_t, CopyJob> m_jobs;
    bool m_tableDirty[kFrames] = {};
    bool m_directDirty[kFrames] = {};
    bool m_warnedDirectFull = false;

    // Passe de cópia: triângulo de tela cheia que amostra a fonte.
    VkDescriptorSetLayout m_copySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_copyLayout = VK_NULL_HANDLE;
    VkPipeline m_copyPipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_copyPool = VK_NULL_HANDLE;

    bool createArray(LayerArray& arr, VkFormat format, uint32_t capacity);
    bool growArray();
    void destroyArray(LayerArray& arr);
    bool createCopyPipeline();
    uint32_t allocLayer();
    void freePlace(uint32_t slot);
    void setTable(uint32_t slot, uint32_t kind, uint32_t layer);
    void runCopies(size_t maxCopies);
    void recordCopy(VkCommandBuffer cmd, VkDescriptorSet copySet, uint32_t layer, bool srgb, VkImageView layerView);
    void writeArrayBindings(VkDescriptorSet set);
    void writeDirectBindings(VkDescriptorSet set);
    std::mutex m_copiedMutex;
    std::vector<uint32_t> m_copiedStatic;
};

} // namespace eruption
