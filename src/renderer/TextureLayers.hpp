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
// Opcional (setBlockFormat, spec 51 TC-05): o array A em formato de bloco, com
// o B em RGBA8 para o resto; mesma tabela, mesmas ligações, mesmo shader.
class TextureLayers {
public:
    // Ligações do set 0, na numeração de slot_textures.glsl.
    static constexpr uint32_t kLayersBinding = 0;
    static constexpr uint32_t kLayersNearestBinding = 1;
    static constexpr uint32_t kTableBinding = 2;
    static constexpr uint32_t kDirectBinding = 3;
    static constexpr uint32_t kDirectCount = 8;
    // Segundo array (mesmo formato): camadas alem do teto de um array, que num
    // celular costuma ser 2048. Fica com 2 camadas ate o primeiro encher.
    static constexpr uint32_t kLayersBBinding = kDirectBinding + kDirectCount;
    static constexpr uint32_t kLayersNearestBBinding = kLayersBBinding + 1;
    static constexpr uint32_t kBindingCount = kLayersNearestBBinding + 1;

    // ERUPTION_DISABLE_LAYER_B=1 (diagnostico): sem o 2o array, e o layout do set 0 volta a ter so'
    // as ligacoes 0..10 (como antes do 2o array), em vez de so' nao usar as 11 e 12.
    static bool layerBDisabled();
    // Ligações e tamanhos de pool do set 0 neste modo.
    static std::vector<VkDescriptorSetLayoutBinding> layoutBindings();
    static std::vector<VkDescriptorPoolSize> poolSizes(uint32_t sets);

    // `layerSize`: lado de cada camada (as texturas são reamostradas para ele).
    // `sets`: os sets do BindlessDescriptor, um por frame em voo.
    // `reserve`: camadas pedidas ja' no inicio (0 = o minimo, 32): o array cresce
    // 1/4 por vez, e cada passo faz vkDeviceWaitIdle e copia as camadas em uso
    // para um array maior (com os dois vivos), o que numa GPU lenta prende o
    // quadro por segundos. Reservar o numero esperado evita os passos.
    // `reserveB`: o mesmo para o segundo array (0 = 2 camadas): cada passo de
    // crescimento tem o array antigo e o novo vivos juntos, o que numa GPU com
    // pouca memoria interna pode derrubar o envio; reservar o B evita os passos.
    bool init(VulkanContext* ctx, uint32_t slotCount, uint32_t layerSize, const VkDescriptorSet* sets,
              VkImageView defaultView, VkSampler defaultSampler, uint32_t reserve = 0, uint32_t reserveB = 0);
    void shutdown();

    // Array A em formato de bloco (spec 51, TC-05; antes de init). BC7, ASTC 4x4
    // ou ETC2 RGBA: 1 B/texel contra 4 do RGBA8. Cada textura que ja' chega
    // comprimida (BC/ASTC/ETC2) ou e' cor sRGB vai para o A: a copia de sempre
    // desenha a camada num RGBA8 de passagem, que volta para a CPU, e' codificada
    // (opaca ou com alfa, conforme a camada) e sobe para o A; o resultado fica em
    // cache no disco pelo conteudo da camada. Dados sem compressao na origem
    // (RGBA8/R8/RG8 lineares) e as sem filtro (sprites) ficam no B, em RGBA8, que
    // tambem recebe o que nao couber no A. Tabela, ligacoes e shader nao mudam.
    // VK_FORMAT_UNDEFINED (padrao) = A em RGBA8; formato que a GPU nao copia ou
    // nao amostra em array, ou sem codificador, tambem volta para RGBA8.
    void setBlockFormat(VkFormat format) { m_blockRequest = format; }
    VkFormat blockFormat() const { return m_blockFormat; }

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
        // Slots que agora mostram a textura padrao por falta de camada (distintos)
        // e quantas vezes a alocacao falhou desde o inicio (uma por tentativa).
        uint32_t lost = 0, lostEvents = 0;
        // Memoria pedida por camada em cada array (KB: alocacao / capacidade, com mips)
        // e, com o A em bloco (TC-05): camadas em uso no A, quantas codificacoes
        // vieram do cache e quantas foram feitas, quantas foram copia direta de uma
        // fonte ja' no formato, e o tempo de CPU somado do caminho.
        double kbPerLayer = 0.0, kbPerLayerB = 0.0;
        uint32_t blockLayers = 0, blockCacheHits = 0, blockEncoded = 0, blockDirect = 0;
        double blockMs = 0.0;
    };
    Stats stats() const {
        Stats s;
        s.side = m_layerSize;
        s.capacity = m_array.capacity;
        s.blockCacheHits = m_blockCacheHits;
        s.blockEncoded = m_blockEncoded;
        s.blockDirect = m_blockDirect;
        s.blockMs = m_blockMs;
        if (m_ctx && m_array.alloc && m_array.capacity)
            s.kbPerLayer = static_cast<double>(m_ctx->allocationSize(m_array.alloc)) / 1024.0 / m_array.capacity;
        if (m_ctx && m_arrayB.alloc && m_arrayB.capacity)
            s.kbPerLayerB = static_cast<double>(m_ctx->allocationSize(m_arrayB.alloc)) / 1024.0 / m_arrayB.capacity;
        s.waiting = static_cast<uint32_t>(m_jobs.size());
        s.lost = static_cast<uint32_t>(m_lostSlots.size());
        s.lostEvents = m_layersLost;
        const uint32_t taken = m_array.next > 0 ? m_array.next - 1 : 0; // a camada 0 e' a padrao
        const uint32_t freed = static_cast<uint32_t>(m_array.freeLayers.size());
        s.used = taken > freed ? taken - freed : 0;
        if (m_blockFormat != VK_FORMAT_UNDEFINED) s.blockLayers = s.used;
        const uint32_t takenB = m_arrayB.next > 0 ? m_arrayB.next - 1 : 0; // a camada 0 do 2o array nao e' usada
        const uint32_t freedB = static_cast<uint32_t>(m_arrayB.freeLayers.size());
        s.used += takenB > freedB ? takenB - freedB : 0;
        s.capacity += m_arrayB.capacity;
        if (m_ctx && m_array.alloc) s.mb = static_cast<double>(m_ctx->allocationSize(m_array.alloc)) / (1024.0 * 1024.0);
        if (m_ctx && m_arrayB.alloc) s.mb += static_cast<double>(m_ctx->allocationSize(m_arrayB.alloc)) / (1024.0 * 1024.0);
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
        VkFormat format = VK_FORMAT_UNDEFINED; // da origem: decide se vai para o A em bloco
    };
    struct Direct {
        uint32_t slot = UINT32_MAX;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    static constexpr uint32_t kNone = UINT32_MAX;
    static constexpr uint32_t kDirectKind = 4;
    // Camada do 2o array: kBKind + os bits de sempre (sRGB, sem filtro). allocLayer
    // devolve a camada com kBLayerFlag somado quando ela e' do 2o array.
    static constexpr uint32_t kBKind = 16;
    static constexpr uint32_t kBLayerFlag = 0x10000;
    static bool isArrayKind(uint32_t k) { return k < kDirectKind || (k >= kBKind && k < kBKind + 4); }
    static bool isDirectKind(uint32_t k) { return k >= kDirectKind && k < kBKind; }
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
    LayerArray m_array;  // camadas [0, teto)
    LayerArray m_arrayB; // camadas alem do teto de m_array
    bool m_bDisabled = false; // ERUPTION_DISABLE_LAYER_B (diagnostico): sem o 2o array
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
    uint32_t m_layersLost = 0; // tentativas de alocacao que falharam (camada 0 devolvida)
    std::unordered_set<uint32_t> m_lostSlots; // slots sem camada agora (saem em release/nova tentativa)

    // Passe de cópia: triângulo de tela cheia que amostra a fonte.
    VkDescriptorSetLayout m_copySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_copyLayout = VK_NULL_HANDLE;
    VkPipeline m_copyPipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_copyPool = VK_NULL_HANDLE;

    // A em bloco (TC-05). m_blockRequest: o pedido (setBlockFormat); m_blockFormat:
    // o que o init aceitou (UNDEFINED = A em RGBA8). m_scratch: RGBA8 de passagem
    // (kBlockGroup camadas), so' enquanto ha copias para o A; some quando a fila esvazia.
    VkFormat m_blockRequest = VK_FORMAT_UNDEFINED;
    VkFormat m_blockFormat = VK_FORMAT_UNDEFINED;
    uint32_t m_blockMaxLayers = 0; // teto de camadas do formato de bloco nesta GPU
    LayerArray m_scratch;
    uint32_t m_blockCacheHits = 0, m_blockEncoded = 0, m_blockDirect = 0;
    double m_blockMs = 0.0;
    // Camadas por rodada no caminho em bloco: desenhadas em lotes de kCopyBatch,
    // lidas de volta e codificadas em paralelo (uma por thread).
    static constexpr uint32_t kBlockGroup = 16;

    bool createArray(LayerArray& arr, VkFormat format, uint32_t capacity, const char* label);
    bool growArray(LayerArray& arr);
    void destroyArray(LayerArray& arr);
    bool createCopyPipeline();
    // A textura vai para o A em bloco (fica no B em RGBA8 se false).
    bool wantsBlock(const CopyJob& job) const;
    bool blockArrayInit(); // aceita (ou recusa) m_blockRequest nesta GPU
    struct BlockTarget {
        uint32_t layer;
        bool srgb;
        CopyJob job;
    };
    void runBlockCopies(const std::vector<BlockTarget>& targets);
    // Fonte ja' no formato do A (mesma classe de bloco), quadrada e com mips ate'
    // 1x1 a partir do lado da camada: a camada e' copia de bloco dela
    // (vkCmdCopyImage), sem desenhar, ler de volta nem codificar - e sem perda.
    // Fonte ETC2 RGB num A em ETC2 RGBA (addAlpha): os blocos voltam para a CPU e
    // sobem com um bloco de alfa 255 na frente de cada um - tambem sem perda.
    struct DirectSource {
        VkImage image;
        uint32_t mip;   // mip da fonte que vira o mip 0 da camada
        uint32_t layer; // camada da fonte
        bool addAlpha;
    };
    bool directSource(const CopyJob& job, DirectSource& out) const;
    struct DirectTarget {
        uint32_t layer; // camada do A
        DirectSource src;
    };
    void runDirectCopies(const std::vector<DirectTarget>& targets);
    void runAlphaCopies(const std::vector<DirectTarget>& targets); // os addAlpha
    void settleLayers(const std::vector<uint32_t>& layers);        // falha: tira do UNDEFINED
    // ERUPTION_TEX_LAYER_PSNR=1: qualidade de cada camada em bloco (medida, log).
    struct LayerPsnr {
        float rgb = 0.0f, alpha = -1.0f; // dB no mip 0; alfa -1 = camada opaca
        bool srgb = false, valid = false;
    };
    std::vector<LayerPsnr> m_psnr;
    void logLayerPsnr(const char* tag) const;
    bool allocCopySets(size_t count, std::vector<VkDescriptorSet>& sets); // ate' kCopyBatch
    void writeCopySource(VkDescriptorSet set, const CopyJob& job);
    VkImageView makeLayerView(const LayerArray& arr, uint32_t layer); // mip 0 de uma camada
    uint32_t allocLayer(bool block);
    void freePlace(uint32_t slot);
    void setTable(uint32_t slot, uint32_t kind, uint32_t layer);
    void runCopies(size_t maxCopies);
    void recordCopy(VkCommandBuffer cmd, VkDescriptorSet copySet, VkImage image, uint32_t layer, bool srgb,
                    VkImageView layerView);
    void writeArrayBindings(VkDescriptorSet set);
    void writeDirectBindings(VkDescriptorSet set);
    std::mutex m_copiedMutex;
    std::vector<uint32_t> m_copiedStatic;
};

} // namespace eruption
