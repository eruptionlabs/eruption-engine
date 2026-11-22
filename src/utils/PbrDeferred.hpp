#pragma once

// PBR ADIADO: a carga de um mapa com pacote grande a frio (HD) para de esperar
// os bytes do normal e do MRAH-W.
//
// Medida (mapa de 390 MB de pacote, disco rotacional, cache frio): 12,7 s dos
// 17,8 s da carga eram a leitura do pacote, e 75% dos bytes dele sao os mapas
// de dados do PBR (normal e MRAH-W em BC7). A CPU e a GPU nao tinham custo
// (com o pacote no cache do sistema a carga com PBR e sem PBR dura o mesmo);
// o que custava era o disco. Entao a carga le' so' o albedo (e o cabecalho dos
// blocos PBR, para saber onde cada um comeca), liga cada slot PBR a uma
// imagem neutra de 1x1 e devolve o mapa; uma thread le' o resto do pacote em
// ordem de arquivo e, quadro a quadro, a imagem de verdade entra no MESMO slot
// (indice estavel: nenhum material, camada de splat ou descritor muda).
// O estado final e' o mesmo bit a bit do carregamento completo.
//
// So' vale para pacote em arquivo solto, grande e fora do cache do sistema
// (ver EmbeddedTextureBake.cpp, readCacheDeferred). Fora disso o caminho de
// sempre roda, sem mudanca.
//
// ERUPTION_TEST_PBR_DEFER=1 (teste): adia sempre que o pacote for arquivo solto
// (ignora tamanho e cache); =0 desliga o adiamento; =2 adia e espera a troca
// de todos os blocos ao fim da carga (compara a imagem final com a leitura inteira).
// ERUPTION_TEST_PBR_DEFER_DELAY_MS=<n> (teste): espera n ms por bloco na thread de
// leitura, para imitar um disco lento e conferir a imagem no meio do caminho.

#include "renderer/BindlessDescriptor.hpp"
#include "renderer/VulkanContext.hpp"
#include "utils/PbrDeferredSource.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace eruption {

class PbrDeferred {
public:
    static PbrDeferred& instance();

    // Adiamento ligado? (so' com descriptor indexing: as camadas do celular
    // copiam a textura uma vez e nao aceitam trocar a imagem do slot.)
    static bool enabled(const BindlessDescriptor* bindless);
    // Teste: ERUPTION_TEST_PBR_DEFER (-1 sem a variavel, 0 desliga, 1 forca, 2 forca e espera).
    static int testOverride();

    // O slot `index` (normal ou MRAH-W) passa a mostrar a imagem neutra ate'
    // o bloco chegar. Thread principal.
    void registerSlot(VulkanContext* ctx, BindlessDescriptor* bindless, VkSampler sampler, uint32_t index,
                      bool isNormal, std::shared_ptr<PbrDeferredSource> src);
    // Outro slot que mostra a mesma imagem (trio de camadas do splat).
    void addAlias(BindlessDescriptor* bindless, uint32_t index, uint32_t alias);
    // View neutra (para quem copia a view do slot adiado).
    VkImageView placeholderView(VulkanContext* ctx, bool isNormal);

    // Blocos (slots) que a tela precisa primeiro: o normal e o MRAH-W (altura) do chao.
    // A thread de leitura os pega antes da ordem de arquivo e o envio os sobe antes dos
    // outros; sem isso, com disco lento, o chao ficava sem relevo ate' o fim do pacote
    // (21 s num mapa de 480 MB). Loga o tempo ate' o ultimo deles chegar.
    // `height` = os MRAH-W (altura do relevo), lidos antes dos `rest` (os normais): a tela
    // perde mais sem a altura do que sem o normal.
    void prioritize(const std::vector<uint32_t>& height, const std::vector<uint32_t>& rest);

    // A leitura em segundo plano so' comeca depois da carga (nao disputa o
    // disco com ela). Chamado ao fim do loadMap.
    void release();
    // Teste (ERUPTION_TEST_PBR_DEFER=2): espera todos os blocos virarem imagem final.
    void drainBlocking(VulkanContext* ctx, BindlessDescriptor* bindless);
    // Uma vez por quadro: recolhe as trocas ja' prontas na GPU e sobe mais um
    // lote do que a thread de leitura entregou.
    void pump(VulkanContext* ctx, BindlessDescriptor* bindless);
    // Troca de mapa / fim: para a leitura, espera a GPU e solta tudo.
    void cancelAll(VulkanContext* ctx, BindlessDescriptor* bindless);
    void shutdown(VulkanContext* ctx, BindlessDescriptor* bindless);

    // Blocos ainda sem imagem final (carga ainda nao completa).
    size_t pending() const { return m_pending.load(); }

private:
    enum State : int { Queued = 0, Reading, Ready, Uploading, Done, Dead };
    struct Alias {
        uint32_t index = 0;
        uint32_t gen = 0;
    };
    struct Entry {
        uint32_t index = 0;
        uint32_t gen = 0;
        bool isNormal = false;
        VkSampler sampler = VK_NULL_HANDLE;
        std::shared_ptr<PbrDeferredSource> src;
        std::vector<Alias> aliases;
        uint8_t prio = 0; // chao: 2 = altura (MRAH-W), 1 = normal; lido e enviado antes do resto
        std::atomic<int> state{Queued};
        EtexData data; // preenchido pela thread de leitura antes de Ready
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Batch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        std::vector<Entry*> items;
    };

    void readerLoop();
    void startReader();
    void stopReader();
    void retire(VulkanContext* ctx, BindlessDescriptor* bindless, bool wait);
    void destroyEntryImage(VulkanContext* ctx, Entry& e);
    void createPlaceholders(VulkanContext* ctx);

    mutable std::mutex m_mtx;
    std::deque<Entry> m_entries; // enderecos estaveis
    std::unordered_map<uint32_t, Entry*> m_byIndex;
    std::vector<Batch> m_batches;
    std::thread m_reader;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_gate{false};
    std::atomic<size_t> m_pending{0};
    std::atomic<uint64_t> m_readyBytes{0};
    VkCommandPool m_pool = VK_NULL_HANDLE;
    // Imagens neutras (compartilhadas entre mapas): normal plano e MRAH-W medio.
    VkImage m_phImage[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation m_phAlloc[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView m_phView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Estatistica do log final.
    std::chrono::steady_clock::time_point m_t0{};
    uint64_t m_totalBytes = 0;
    uint32_t m_total = 0;
    uint32_t m_done = 0;
    bool m_logged = false;
    std::atomic<uint32_t> m_prioPending{0};
    std::chrono::steady_clock::time_point m_prioT0{};
    uint32_t m_prioTotal = 0;
    uint64_t m_prioBytes = 0;
    void prioDone(const Entry& e);
};

} // namespace eruption
