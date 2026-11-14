#pragma once

// Streaming de mips por distancia (o "mipmap streaming" dos motores
// comerciais), para placa com pouca memoria de video.
//
// Quem cria uma textura ja' comprimida em bloco (EtexData, mips prontos do
// bake) sobe so' a cauda pequena da piramide (baseMipFor) e registra o slot
// aqui. A cada poucos quadros o streamer:
//   1. estima, por textura, o mip mais fino que a tela pode mostrar: para cada
//      instancia estatica, texels por unidade de mundo (lado x densidade de UV
//      da malha / escala) contra pixels por unidade na distancia da caixa;
//   2. aplica o orcamento de memoria: se a soma passa, tira o mip de cima de
//      quem mais pesa, ate' caber (o "max level reduction");
//   3. cria uma imagem nova so' com os mips pedidos, sobe a partir da copia em
//      RAM (sem decodificar nem recomprimir nada), espera a cerca sem travar o
//      quadro e so' entao troca o slot. A imagem antiga morre depois dos
//      quadros em voo.
// A cauda do dono fica sempre residente: o slot nunca aponta para buraco, no
// pior caso a textura fica borrada por alguns quadros.
//
// So' com descriptor indexing (no caminho de camadas do celular a textura e'
// copiada uma vez para a camada e o slot nao muda de imagem).

#include "renderer/ModelData.hpp"
#include "renderer/VulkanContext.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace eruption {

struct EtexData;
class BindlessDescriptor;

class TextureStreamer {
public:
    struct Settings {
        bool enabled = false;
        uint32_t baseSize = 128;      // lado maximo da cauda sempre residente
        uint32_t budgetMb = 0;        // 0 = automatico (fatia do heap da GPU)
        uint32_t uploadMbPerFrame = 16;
        float mipBias = 1.0f;         // mips a mais de folga (anisotropico pede o mais fino)
        uint32_t rampFrames = 6;      // quadros por mip ao ficar nitida (0 = troca direta)
    };
    struct Stats {
        uint32_t managed = 0;         // texturas registradas e vivas
        uint32_t streamed = 0;        // com imagem acima da cauda
        uint64_t baseBytes = 0;       // caudas (do dono)
        uint64_t streamedBytes = 0;   // imagens do streamer
        uint64_t fullBytes = 0;       // o que tudo ocuparia sem streaming
        uint64_t budgetBytes = 0;
        uint64_t uploadedBytes = 0;   // acumulado
        uint32_t uploads = 0;         // acumulado
    };

    // Malha -> texturas que ela amostra: densidade de UV (UV por unidade do
    // espaco do objeto) e a caixa, no espaco do objeto, dos vertices que usam
    // a textura. A caixa e' por textura porque um mapa costuma ter poucas
    // malhas grandes: a caixa da malha inteira contem a camera e a distancia
    // daria zero para todas as texturas dela.
    struct SlotUse {
        uint16_t slot = 0;
        float uvPerUnit = 0.0f;
        Vec3 boundsMin{0.0f}, boundsMax{0.0f};
    };

    void init(VulkanContext* vk, BindlessDescriptor* bindless);
    void shutdown();
    void configure(const Settings& s);
    const Settings& settings() const { return m_settings; }
    bool active() const { return m_settings.enabled && m_vk != nullptr; }

    // Primeiro mip que o dono sobe (0 = textura inteira, sem streaming).
    // So' liga quando o mapa nao cabe no orcamento (ver setPlanner): mapa que
    // cabe sobe inteiro, como sem streaming - mesma imagem, nenhum custo.
    uint32_t baseMipFor(const EtexData& e);
    // Estimativa das texturas do mapa atual: devolve uma chave do conjunto
    // (muda de mapa = muda a chave) e, se `bytes` nao e' nulo, os bytes da
    // piramide inteira. false = nada a estimar (sem pacote): sem streaming.
    using Planner = std::function<bool(uint64_t& key, uint64_t* bytes)>;
    void setPlanner(Planner p) { m_planner = std::move(p); }
    bool engaged() const { return m_engaged; }
    // Outro slot que mostra a mesma imagem (camada de splat: cor, normal e
    // MRAH-W copiados para um trio de slots seguidos). Acompanha as trocas.
    void addAlias(uint32_t slot, uint32_t alias);
    // A cauda que o dono sobe: piramide de `base` ate' o fim, dentro do payload.
    struct Tail {
        uint32_t width = 0, height = 0, mipCount = 0;
        std::vector<uint32_t> mipSizes;
        const uint8_t* data = nullptr;
        size_t bytes = 0;
    };
    static Tail tailOf(const EtexData& e, uint32_t base);
    // Thread-safe: os resolvedores de textura rodam em threads do loader.
    void registerSlot(uint32_t slot, const EtexData& e, uint32_t baseMip, VkImageView baseView, VkSampler sampler);

    // Uma vez por quadro, com o quadro aberto (depois de beginFrame).
    // pixelsPerUnit: pixels de tela por unidade de mundo a distancia 1.
    template <class ForEachInstance>
    void update(const Vec3& cameraPos, float pixelsPerUnit, ForEachInstance&& forEach);

    Stats stats() const { return m_stats; }

    // Instancia ativa, para os caminhos sem acesso ao Engine (PbrTextureLoader).
    static TextureStreamer* active_instance() { return s_active; }

private:
    struct Entry {
        uint32_t slot = 0;
        uint32_t generation = 0;
        bool alive = false;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0, mipCount = 0;
        uint32_t baseMip = 0;
        std::vector<uint64_t> mipOffset;   // mipCount + 1 (prefixo)
        std::shared_ptr<std::vector<uint8_t>> payload;
        VkImageView baseView = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t residentMip = 0;          // mip de cima visivel no slot
        VkImage image = VK_NULL_HANDLE;    // imagem do streamer (null = cauda do dono)
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint64_t imageBytes = 0;
        bool inFlight = false;
        bool seenStatic = false;           // alguma malha estatica usa
        std::vector<std::pair<uint32_t, uint32_t>> aliases;  // (slot, geracao)
        // Rampa: views da imagem do streamer comecando no mip k (ramp[k-1]).
        // Uma view com baseMipLevel = k amostra igual a uma textura cujo topo
        // e' o mip k, entao o slot desce um mip por vez sem mexer em shader.
        std::vector<VkImageView> ramp;
        uint32_t rampLevel = 0;            // view atual no slot (0 = imagem inteira)
        uint64_t rampNext = 0;             // quadro do proximo degrau
        uint32_t visibleMip = 0;           // mip de cima que o slot mostra agora
    };
    struct Item {
        uint32_t entry = 0;
        uint32_t generation = 0;
        uint32_t mip = 0;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint64_t bytes = 0;
    };
    struct Batch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        std::vector<Item> items;
        bool arrival = false;              // primeira passada do mapa: troca direta
    };

    void drainRegistrations();
    void beginDemand();
    void noteDemand(uint32_t slot, float uvPerUnit, float distance, float scale, float pixelsPerUnit);
    void endDemandAndApply();
    uint32_t targetFromNeed(const Entry& e, float need) const;
    std::vector<uint32_t> budgetTargets();
    uint64_t budgetNow(uint64_t mine) const;
    uint64_t chainBytes(const Entry& e, uint32_t top) const { return e.mipOffset[e.mipCount] - e.mipOffset[top]; }
    bool buildImage(Entry& e, uint32_t top, Item& out);
    void submitBatch(std::vector<Item>&& items, bool wait, bool arrival);
    void retireBatches(bool wait);
    void releaseImage(VkImage image, VmaAllocation alloc, VkImageView view);
    void dropStreamedImage(Entry& e);
    void stepRamps();
    void releaseRamp(Entry& e);
    VkImageView makeView(VkImage image, VkFormat format, uint32_t baseLevel, uint32_t levels);
    std::vector<uint32_t> m_ramping;     // entradas com rampa em andamento

    VulkanContext* m_vk = nullptr;
    BindlessDescriptor* m_bindless = nullptr;
    Settings m_settings;
    Stats m_stats;
    VkCommandPool m_pool = VK_NULL_HANDLE;

    std::mutex m_regMutex;
    struct Registration {
        uint32_t slot, generation, baseMip;
        VkImageView baseView;
        VkSampler sampler;
        VkFormat format;
        uint32_t width, height, mipCount;
        std::vector<uint32_t> mipSizes;
        std::shared_ptr<std::vector<uint8_t>> payload;
    };
    std::vector<Registration> m_newRegs;
    std::vector<std::pair<uint32_t, uint32_t>> m_newAliases;  // (slot dono, alias)
    Planner m_planner;
    uint64_t m_planKey = ~0ull;
    uint64_t m_planBytes = 0;    // estimativa do mapa (pacote embutido)
    uint64_t m_seenBytes = 0;    // texturas que ja' passaram por baseMipFor neste mapa
    uint64_t m_planBudget = 0;
    std::atomic<bool> m_engaged{false};
    void pointSlot(Entry& e, VkImageView view);

    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_slotEntry;   // slot -> entrada + 1 (0 = nenhuma)
    std::vector<float> m_need;           // por entrada: mip continuo pedido no passe (kUnseen = nenhum uso)
    std::vector<Batch> m_batches;
    uint64_t m_frame = 0;
    uint32_t m_primeCount = 0;           // registros desde o ultimo passe
    uint64_t m_budgetBytes = 0;
    uint64_t m_lastStatsLog = 0;
    double m_passMs = 0.0;               // CPU dos passes (demanda + troca), para o log
    uint32_t m_passes = 0;

    static constexpr float kUnseen = 1e30f;
    static constexpr uint32_t kUpdateInterval = 4;  // quadros entre passes de demanda
    static inline TextureStreamer* s_active = nullptr;
};

template <class ForEachInstance>
void TextureStreamer::update(const Vec3& cameraPos, float pixelsPerUnit, ForEachInstance&& forEach) {
    if (!active()) return;
    ++m_frame;
    retireBatches(false);
    stepRamps();
    drainRegistrations();
    if (m_entries.empty()) return;
    // Muitos registros de uma vez = carga de mapa: o primeiro passe sobe tudo
    // e espera, para o primeiro quadro de jogo ja' sair nitido.
    const bool prime = m_primeCount >= 16;
    if (!prime && m_frame % kUpdateInterval != 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    beginDemand();
    forEach([&](const Mat4& transform, float scale, const std::vector<SlotUse>& uses) {
        // Caixa do objeto -> caixa de mundo (centro transformado + extensao pelo
        // modulo da matriz), distancia da camera ate' ela.
        const glm::mat3 m(transform);
        const glm::mat3 a(glm::abs(m[0]), glm::abs(m[1]), glm::abs(m[2]));
        const Vec3 t(transform[3]);
        for (const SlotUse& u : uses) {
            const Vec3 c = m * ((u.boundsMin + u.boundsMax) * 0.5f) + t;
            const Vec3 h = a * ((u.boundsMax - u.boundsMin) * 0.5f);
            const Vec3 q = glm::clamp(cameraPos, c - h, c + h);
            noteDemand(u.slot, u.uvPerUnit, glm::length(cameraPos - q), scale, pixelsPerUnit);
        }
    });
    endDemandAndApply();
    // A passada da carga (prime) sobe tudo e espera: fica fora da media de jogo.
    if (!prime) {
        m_passMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++m_passes;
    }
}

} // namespace eruption
