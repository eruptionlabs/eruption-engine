#include "renderer/TextureStreamer.hpp"

#include "renderer/BindlessDescriptor.hpp"
#include "utils/EtexLoader.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <queue>

namespace eruption {

void TextureStreamer::init(VulkanContext* vk, BindlessDescriptor* bindless) {
    m_vk = vk;
    m_bindless = bindless;
    // Sem descriptor indexing (camadas) o slot nao troca de imagem depois de
    // copiado: o streaming fica desligado e o dono sobe a piramide inteira.
    if (bindless->layerSize() != 0) m_settings.enabled = false;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = vk->queueFamilies().graphicsFamily;
    vkCreateCommandPool(vk->device(), &pi, nullptr, &m_pool);
    s_active = this;
}

void TextureStreamer::configure(const Settings& s) {
    m_settings = s;
    m_settings.baseSize = std::max(16u, m_settings.baseSize);
    if (m_bindless && m_bindless->layerSize() != 0) m_settings.enabled = false;
    m_budgetBytes = 0;  // recalculado no proximo passe
}

void TextureStreamer::shutdown() {
    if (!m_vk) return;
    retireBatches(true);
    for (Entry& e : m_entries) {
        for (VkImageView v : e.ramp) vkDestroyImageView(m_vk->device(), v, nullptr);
        e.ramp.clear();
        if (e.image != VK_NULL_HANDLE) {
            vkDestroyImageView(m_vk->device(), e.view, nullptr);
            vmaDestroyImage(m_vk->allocator(), e.image, e.alloc);
        }
    }
    m_entries.clear();
    m_slotEntry.clear();
    if (m_pool != VK_NULL_HANDLE) vkDestroyCommandPool(m_vk->device(), m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
    if (s_active == this) s_active = nullptr;
    m_vk = nullptr;
}

uint32_t TextureStreamer::baseMipFor(const EtexData& e) {
    if (!active() || !e.valid() || e.mipCount < 2) return 0;
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        uint64_t key = 0, bytes = 0;
        if (!m_planner || !m_planner(key, nullptr)) {
            m_engaged = false;
            m_planKey = ~0ull;
        } else {
            bool changed = false;
            if (key != m_planKey && m_planner(key, &bytes)) {
                changed = true;
                // Mapa (ou vizinho) novo. Na carga o streamer ainda nao segura
                // nada dele: o orcamento e' o que a placa deixa agora.
                if (m_planKey == ~0ull || bytes < m_planBytes) m_seenBytes = 0;
                m_planKey = key;
                m_planBytes = bytes;
                m_planBudget = budgetNow(0);
            }
            // O pacote embutido nao e' tudo: os mapas PBR cozidos por arquivo
            // vem depois (medido: 242 MB no pacote, 728 MB geridos num mapa
            // com vizinhos). Conta tambem o que ja' passou por aqui e liga
            // quando estourar - o que ja' subiu inteiro fica como esta'.
            m_seenBytes += e.payload.size();
            const bool engage = std::max(m_planBytes, m_seenBytes) > m_planBudget;
            if (engage != m_engaged || changed) {
                m_engaged = engage;
                ERUPTION_LOG_WARN("[STREAM] mapa com %.1f MB de textura (%.1f MB ja' vistos), orcamento %.0f MB: %s",
                                  double(m_planBytes) / 1048576.0, double(m_seenBytes) / 1048576.0,
                                  double(m_planBudget) / 1048576.0,
                                  m_engaged ? "streaming ligado" : "cabe inteiro, sem streaming");
            }
        }
        if (!m_engaged) return 0;
    }
    uint32_t m = 0;
    while (m + 1 < e.mipCount && std::max(e.width >> m, e.height >> m) > m_settings.baseSize) ++m;
    return m;
}

TextureStreamer::Tail TextureStreamer::tailOf(const EtexData& e, uint32_t base) {
    Tail t;
    base = std::min(base, e.mipCount > 0 ? e.mipCount - 1 : 0u);
    size_t skip = 0;
    for (uint32_t m = 0; m < base; ++m) skip += e.mipSizes[m];
    t.width = std::max(1u, e.width >> base);
    t.height = std::max(1u, e.height >> base);
    t.mipCount = e.mipCount - base;
    t.mipSizes.assign(e.mipSizes.begin() + base, e.mipSizes.end());
    t.data = e.payload.data() + skip;
    t.bytes = e.payload.size() - skip;
    return t;
}

void TextureStreamer::registerSlot(uint32_t slot, const EtexData& e, uint32_t baseMip, VkImageView baseView,
                                   VkSampler sampler) {
    if (!active() || slot == 0 || baseMip == 0 || !e.valid()) return;
    Registration r{};
    r.slot = slot;
    r.generation = m_bindless->slotGenerationSafe(slot);
    r.baseMip = baseMip;
    r.baseView = baseView;
    r.sampler = sampler;
    r.format = static_cast<VkFormat>(e.vkFormat);
    r.width = e.width;
    r.height = e.height;
    r.mipCount = e.mipCount;
    r.mipSizes = e.mipSizes;
    // Copia inteira em RAM (a cauda e' 1/64 do total com base 128 de 1024):
    // a imagem nova leva a piramide de `top` ate' o fim, cauda inclusive, e
    // a imagem do dono nem sempre pode ser origem de copia.
    r.payload = std::make_shared<std::vector<uint8_t>>(e.payload);
    std::lock_guard<std::mutex> lock(m_regMutex);
    m_newRegs.push_back(std::move(r));
}

void TextureStreamer::addAlias(uint32_t slot, uint32_t alias) {
    if (!active() || slot == 0 || alias == 0) return;
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (m_engaged) m_newAliases.emplace_back(slot, alias);
}

void TextureStreamer::pointSlot(Entry& e, VkImageView view) {
    // O slot e as copias dele (splat); copia cujo slot ja' foi liberado sai.
    for (size_t k = 0; k < e.aliases.size();) {
        if (m_bindless->updateTextureIfGenerationSafe(e.aliases[k].first, e.aliases[k].second, view, e.sampler)) ++k;
        else e.aliases.erase(e.aliases.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

void TextureStreamer::drainRegistrations() {
    std::vector<Registration> regs;
    std::vector<std::pair<uint32_t, uint32_t>> aliases;
    {
        std::lock_guard<std::mutex> lock(m_regMutex);
        regs.swap(m_newRegs);
        aliases.swap(m_newAliases);
    }
    for (Registration& r : regs) {
        if (r.slot >= m_slotEntry.size()) m_slotEntry.resize(r.slot + 1, 0u);
        if (const uint32_t old = m_slotEntry[r.slot]) {
            // Slot reaproveitado: a entrada antiga ja' morreu (geracao).
            Entry& o = m_entries[old - 1];
            dropStreamedImage(o);
            o.alive = false;
        }
        Entry e;
        e.slot = r.slot;
        e.generation = r.generation;
        e.alive = true;
        e.format = r.format;
        e.width = r.width;
        e.height = r.height;
        e.mipCount = r.mipCount;
        e.baseMip = r.baseMip;
        e.residentMip = r.baseMip;
        e.visibleMip = r.baseMip;
        e.mipOffset.resize(r.mipCount + 1, 0);
        for (uint32_t m = 0; m < r.mipCount; ++m) e.mipOffset[m + 1] = e.mipOffset[m] + r.mipSizes[m];
        e.payload = std::move(r.payload);
        e.baseView = r.baseView;
        e.sampler = r.sampler;
        // Entrada morta reaproveitada antes de crescer o vetor.
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < m_entries.size(); ++i)
            if (!m_entries[i].alive && m_entries[i].image == VK_NULL_HANDLE && !m_entries[i].inFlight) { idx = i; break; }
        if (idx == UINT32_MAX) {
            idx = static_cast<uint32_t>(m_entries.size());
            m_entries.push_back(std::move(e));
        } else {
            m_entries[idx] = std::move(e);
        }
        m_slotEntry[r.slot] = idx + 1;
        ++m_primeCount;
    }
    for (const auto& [slot, alias] : aliases) {
        if (slot >= m_slotEntry.size() || m_slotEntry[slot] == 0) continue;
        Entry& e = m_entries[m_slotEntry[slot] - 1];
        e.aliases.emplace_back(alias, m_bindless->slotGenerationSafe(alias));
        // A copia nasceu com a view de agora; se ja' ha' imagem maior, aponta.
        if (e.image != VK_NULL_HANDLE) pointSlot(e, e.view);
    }
}

uint64_t TextureStreamer::budgetNow(uint64_t mine) const {
    if (m_settings.budgetMb > 0) return uint64_t(m_settings.budgetMb) << 20;
    // Automatico: metade do que o heap local da GPU ainda deixa usar, contando
    // o que o proprio streamer ja' segura. Em placa de 2 GB fica perto de
    // 0,5-0,7 GB para textura; numa de 24 GB nao limita nada.
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(m_vk->allocator(), budgets);
    const VkPhysicalDeviceMemoryProperties* mp = nullptr;
    vmaGetMemoryProperties(m_vk->allocator(), &mp);
    uint64_t best = 0;
    for (uint32_t h = 0; mp && h < mp->memoryHeapCount; ++h) {
        if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        const uint64_t others = budgets[h].usage > mine ? budgets[h].usage - mine : 0;
        const uint64_t free = budgets[h].budget > others ? budgets[h].budget - others : 0;
        best = std::max(best, free / 2);
    }
    return std::max<uint64_t>(best, 64ull << 20);
}

void TextureStreamer::beginDemand() {
    m_need.assign(m_entries.size(), kUnseen);
}

void TextureStreamer::noteDemand(uint32_t slot, float uvPerUnit, float distance, float scale, float pixelsPerUnit) {
    if (slot >= m_slotEntry.size()) return;
    const uint32_t idx = m_slotEntry[slot];
    if (idx == 0) return;
    Entry& e = m_entries[idx - 1];
    if (!e.alive) return;
    e.seenStatic = true;
    // Texels por unidade de mundo contra pixels por unidade nesta distancia;
    // log2 da razao = o mip que o hardware escolheria olhando de frente. A
    // folga (mipBias) cobre o anisotropico e a superficie vista de lado, que
    // pedem um mip mais fino que o de frente. Fica o menor de todos os usos.
    const float texelsPerUnit = float(std::max(e.width, e.height)) * uvPerUnit / std::max(scale, 1e-4f);
    const float pixels = pixelsPerUnit / std::max(distance, 1e-2f);
    const float mip = std::log2(std::max(texelsPerUnit / std::max(pixels, 1e-6f), 1e-6f)) - m_settings.mipBias;
    float& need = m_need[idx - 1];
    need = std::min(need, mip);
}

uint32_t TextureStreamer::targetFromNeed(const Entry& e, float need) const {
    // Sem uso por malha estatica (personagem, efeito) nao ha' como medir: sobe
    // inteira. Usada, mas nao neste passe: so' a cauda.
    if (need >= kUnseen) return e.seenStatic ? e.baseMip : 0u;
    const float m = std::clamp(std::floor(need), 0.0f, float(e.mipCount - 1));
    return std::min(static_cast<uint32_t>(m), e.baseMip);
}

std::vector<uint32_t> TextureStreamer::budgetTargets() {
    // Alvo por entrada = o mip pedido. Se a soma passar do orcamento, perde o
    // mip de cima quem menos sente: a que, descendo um nivel, ainda fica mais
    // perto do que a tela pede (necessidade continua - alvo). Empate: a maior.
    std::vector<uint32_t> target(m_entries.size(), 0);
    uint64_t total = 0;
    struct Node {
        float deficit;
        uint64_t bytes;
        uint32_t i;
        bool operator<(const Node& o) const { return deficit != o.deficit ? deficit > o.deficit : bytes < o.bytes; }
    };
    std::priority_queue<Node> cheap;
    auto deficit = [&](uint32_t i, uint32_t t) {
        const float need = m_need[i] >= kUnseen ? 0.0f : std::max(m_need[i], 0.0f);
        return float(t + 1) - need;
    };
    for (uint32_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        if (!e.alive) continue;
        const uint32_t t = targetFromNeed(e, m_need[i]);
        target[i] = t;
        total += chainBytes(e, t);
        if (t < e.baseMip) cheap.push({deficit(i, t), e.mipOffset[t + 1] - e.mipOffset[t], i});
    }
    while (total > m_budgetBytes && !cheap.empty()) {
        const uint32_t i = cheap.top().i;
        cheap.pop();
        const Entry& e = m_entries[i];
        total -= e.mipOffset[target[i] + 1] - e.mipOffset[target[i]];
        ++target[i];
        if (target[i] < e.baseMip) cheap.push({deficit(i, target[i]), e.mipOffset[target[i] + 1] - e.mipOffset[target[i]], i});
    }
    return target;
}

void TextureStreamer::endDemandAndApply() {
    const bool prime = m_primeCount >= 16;
    m_primeCount = 0;
    if (prime || m_budgetBytes == 0 || m_frame % 256 == 0) m_budgetBytes = budgetNow(m_stats.streamedBytes);
    const std::vector<uint32_t> target = budgetTargets();
    static const bool kHist = std::getenv("ERUPTION_TEST_STREAM_LOG") != nullptr;
    if (kHist && prime) {
        // Por que cada textura ficou no mip que ficou: sem malha estatica
        // (sobe inteira), pedida por distancia (histograma do mip) ou orcamento.
        uint32_t unseen = 0, hist[16] = {};
        for (uint32_t i = 0; i < m_entries.size(); ++i) {
            if (!m_entries[i].alive) continue;
            if (m_need[i] >= kUnseen) ++unseen;
            else ++hist[std::min<uint32_t>(targetFromNeed(m_entries[i], m_need[i]), 15)];
        }
        uint32_t rest = 0;
        for (uint32_t k = 5; k < 16; ++k) rest += hist[k];
        ERUPTION_LOG_WARN("[STREAM] demanda: %u sem malha estatica; mip pedido 0:%u 1:%u 2:%u 3:%u 4:%u 5+:%u", unseen,
                          hist[0], hist[1], hist[2], hist[3], hist[4], rest);
    }

    // Quem desce (camera se afastou, ou o orcamento apertou) libera ja': volta
    // para a cauda ou para uma imagem menor. Histerese de 1 mip para nao
    // ficar subindo e descendo na borda da distancia.
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < m_entries.size(); ++i) {
        Entry& e = m_entries[i];
        if (!e.alive || e.inFlight) continue;
        // O dono liberou o slot (fim do mapa, editor): a entrada morre e
        // devolve a imagem e a copia em RAM.
        if (m_bindless->slotGenerationSafe(e.slot) != e.generation) {
            e.alive = false;
            dropStreamedImage(e);
            e.payload.reset();
            m_slotEntry[e.slot] = 0;
            continue;
        }
        const uint32_t t = target[i];
        if (t == e.residentMip) continue;
        if (t > e.residentMip) {
            if (t < e.residentMip + 2 && t < e.baseMip) continue;  // histerese
            if (t >= e.baseMip) {
                dropStreamedImage(e);
                continue;
            }
        }
        order.push_back(i);
    }
    // Primeiro quem ganha mais mips (o borrado que mais aparece), depois o menor.
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const int ga = int(m_entries[a].residentMip) - int(target[a]);
        const int gb = int(m_entries[b].residentMip) - int(target[b]);
        if (ga != gb) return ga > gb;
        return chainBytes(m_entries[a], target[a]) < chainBytes(m_entries[b], target[b]);
    });
    const uint64_t uploadCap = prime ? ~0ull
                                     : (uint64_t(std::max(1u, m_settings.uploadMbPerFrame)) << 20) * kUpdateInterval;
    uint64_t queued = 0;
    std::vector<Item> items;
    for (uint32_t i : order) {
        Entry& e = m_entries[i];
        const uint64_t bytes = chainBytes(e, target[i]) - chainBytes(e, e.baseMip);
        if (!items.empty() && queued + bytes > uploadCap) break;
        Item it;
        if (!buildImage(e, target[i], it)) continue;
        it.entry = i;
        queued += bytes;
        items.push_back(it);
    }
    // Nem a primeira passada espera: o primeiro quadro sai com a cauda e as
    // texturas ficam nitidas quando o envio termina (alguns quadros).
    if (!items.empty()) submitBatch(std::move(items), false, prime);

    // Resumo periodico no log (ERUPTION_TEST_STREAM_LOG=1): residente,
    // orcamento e quanto a piramide inteira ocuparia.
    static const bool kLog = std::getenv("ERUPTION_TEST_STREAM_LOG") != nullptr;
    if (prime || (kLog && m_frame - m_lastStatsLog >= 240)) {
        m_lastStatsLog = m_frame;
        Stats& s = m_stats;
        s.managed = s.streamed = 0;
        s.baseBytes = s.fullBytes = 0;
        for (const Entry& e : m_entries) {
            if (!e.alive) continue;
            ++s.managed;
            s.streamed += e.image != VK_NULL_HANDLE;
            s.baseBytes += chainBytes(e, e.baseMip);
            s.fullBytes += chainBytes(e, 0);
        }
        s.budgetBytes = m_budgetBytes;
        ERUPTION_LOG_WARN("[STREAM] %u texturas, %u acima da cauda; residente %.1f MB (cauda %.1f + streaming %.1f) "
                          "de %.1f MB inteiras; orcamento %.0f MB; %u envios, %.1f MB enviados; passe %.3f ms (media de %u)",
                          s.managed, s.streamed, double(s.baseBytes + s.streamedBytes) / 1048576.0,
                          double(s.baseBytes) / 1048576.0, double(s.streamedBytes) / 1048576.0,
                          double(s.fullBytes) / 1048576.0, double(s.budgetBytes) / 1048576.0, s.uploads,
                          double(s.uploadedBytes) / 1048576.0, m_passes ? m_passMs / m_passes : 0.0, m_passes);
        m_passMs = 0.0;
        m_passes = 0;
    }
}

VkImageView TextureStreamer::makeView(VkImage image, VkFormat format, uint32_t baseLevel, uint32_t levels) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = baseLevel;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(m_vk->device(), &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

bool TextureStreamer::buildImage(Entry& e, uint32_t top, Item& out) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = e.format;
    ii.extent = {std::max(1u, e.width >> top), std::max(1u, e.height >> top), 1};
    ii.mipLevels = e.mipCount - top;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_vk->allocator(), &ii, &ai, &out.image, &out.alloc, nullptr) != VK_SUCCESS) return false;
    out.view = makeView(out.image, e.format, 0, ii.mipLevels);
    if (out.view == VK_NULL_HANDLE) {
        vmaDestroyImage(m_vk->allocator(), out.image, out.alloc);
        return false;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_vk->allocator(), out.alloc, &info);
    out.bytes = info.size;
    out.mip = top;
    out.generation = e.generation;
    e.inFlight = true;
    return true;
}

void TextureStreamer::submitBatch(std::vector<Item>&& items, bool wait, bool arrival) {
    // Um staging so' para o lote e um envio so', sem esperar: a troca do slot
    // acontece num passe seguinte, quando a cerca ja' sinalizou.
    // Cada textura comeca alinhada a 16 bytes: o bufferOffset de uma copia
    // para formato de bloco tem de ser multiplo do bloco (8 ou 16), e um BC1
    // com mips de 8 bytes deixava o proximo BC3/BC7 desalinhado (VUID-07975).
    auto align16 = [](uint64_t v) { return (v + 15) & ~uint64_t(15); };
    uint64_t total = 0;
    for (const Item& it : items) total = align16(total) + chainBytes(m_entries[it.entry], it.mip);
    Batch b;
    if (!m_vk->createBuffer(std::max<uint64_t>(total, 16), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY,
                            b.staging, b.stagingAlloc)) {
        for (const Item& it : items) {
            m_entries[it.entry].inFlight = false;
            vkDestroyImageView(m_vk->device(), it.view, nullptr);
            vmaDestroyImage(m_vk->allocator(), it.image, it.alloc);
        }
        return;
    }
    uint8_t* mapped = nullptr;
    vmaMapMemory(m_vk->allocator(), b.stagingAlloc, reinterpret_cast<void**>(&mapped));

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_vk->device(), &ai, &b.cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(b.cmd, &bi);

    uint64_t off = 0;
    std::vector<VkBufferImageCopy> regions;
    for (const Item& it : items) {
        const Entry& e = m_entries[it.entry];
        const uint64_t bytes = chainBytes(e, it.mip);
        off = align16(off);
        std::memcpy(mapped + off, e.payload->data() + e.mipOffset[it.mip], bytes);
        m_vk->cmdImageBarrier(b.cmd, it.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              VK_ACCESS_TRANSFER_WRITE_BIT);
        regions.clear();
        for (uint32_t m = it.mip; m < e.mipCount; ++m) {
            VkBufferImageCopy r{};
            r.bufferOffset = off + (e.mipOffset[m] - e.mipOffset[it.mip]);
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.mipLevel = m - it.mip;
            r.imageSubresource.layerCount = 1;
            r.imageExtent = {std::max(1u, e.width >> m), std::max(1u, e.height >> m), 1};
            regions.push_back(r);
        }
        vkCmdCopyBufferToImage(b.cmd, b.staging, it.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        m_vk->cmdImageBarrier(b.cmd, it.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT);
        off += bytes;
    }
    vmaUnmapMemory(m_vk->allocator(), b.stagingAlloc);
    vmaFlushAllocation(m_vk->allocator(), b.stagingAlloc, 0, VK_WHOLE_SIZE);
    b.items = std::move(items);
    b.arrival = arrival;
    vkEndCommandBuffer(b.cmd);
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(m_vk->device(), &fi, nullptr, &b.fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &b.cmd;
    vkQueueSubmit(m_vk->graphicsQueue(), 1, &si, b.fence);
    m_stats.uploadedBytes += total;
    m_stats.uploads += static_cast<uint32_t>(b.items.size());
    m_batches.push_back(std::move(b));
    if (wait) retireBatches(true);
}

void TextureStreamer::retireBatches(bool wait) {
    bool swapped = false;
    for (size_t k = 0; k < m_batches.size();) {
        Batch& b = m_batches[k];
        const VkResult r = wait ? vkWaitForFences(m_vk->device(), 1, &b.fence, VK_TRUE, UINT64_MAX)
                                : vkGetFenceStatus(m_vk->device(), b.fence);
        if (r != VK_SUCCESS) {
            ++k;
            continue;
        }
        for (Item& it : b.items) {
            Entry& e = m_entries[it.entry];
            e.inFlight = false;
            if (!e.alive || e.generation != it.generation ||
                m_bindless->slotGenerationSafe(e.slot) != e.generation) {
                releaseImage(it.image, it.alloc, it.view);
                continue;
            }
            // Rampa: o slot continua mostrando o mip de cima de agora (mesma
            // imagem na tela no quadro da troca) e desce um mip a cada
            // rampFrames quadros ate' a imagem inteira. Ficando mais borrada
            // (orcamento, camera longe) a troca e' direta.
            releaseRamp(e);
            const uint32_t levels = e.mipCount - it.mip;
            uint32_t start = e.visibleMip > it.mip ? e.visibleMip - it.mip : 0;
            // Na chegada ao mapa a troca e' direta: cai nos quadros 2-3, junto
            // com o fade do warp e a mudanca da tela inteira na partida. Medido
            // em 45 mapas: com rampa ali os degraus caiam sozinhos nos quadros
            // 7-19 e apareciam (ate' +15% de pixels com |dY| > 8 num quadro).
            if (m_settings.rampFrames == 0 || b.arrival) start = 0;
            start = std::min(start, levels - 1);
            for (uint32_t k = 1; k <= start; ++k) {
                const VkImageView v = makeView(it.image, e.format, k, levels - k);
                if (v == VK_NULL_HANDLE) {
                    start = k - 1;
                    break;
                }
                e.ramp.push_back(v);
            }
            const VkImageView shown = start > 0 ? e.ramp[start - 1] : it.view;
            if (!m_bindless->updateTextureIfGenerationSafe(e.slot, e.generation, shown, e.sampler)) {
                releaseRamp(e);
                releaseImage(it.image, it.alloc, it.view);
                e.alive = false;
                dropStreamedImage(e);
                continue;
            }
            pointSlot(e, shown);
            if (e.image != VK_NULL_HANDLE) {
                m_stats.streamedBytes -= e.imageBytes;
                releaseImage(e.image, e.alloc, e.view);
            }
            e.image = it.image;
            e.alloc = it.alloc;
            e.view = it.view;
            e.imageBytes = it.bytes;
            e.residentMip = it.mip;
            e.rampLevel = start;
            e.visibleMip = it.mip + start;
            e.rampNext = m_frame + m_settings.rampFrames;
            if (start > 0) m_ramping.push_back(it.entry);
            m_stats.streamedBytes += it.bytes;
            swapped = true;
        }
        vkDestroyFence(m_vk->device(), b.fence, nullptr);
        vkFreeCommandBuffers(m_vk->device(), m_pool, 1, &b.cmd);
        vmaDestroyBuffer(m_vk->allocator(), b.staging, b.stagingAlloc);
        m_batches.erase(m_batches.begin() + static_cast<std::ptrdiff_t>(k));
    }
    if (swapped) m_bindless->flushUpdatesSafe();
}

void TextureStreamer::releaseImage(VkImage image, VmaAllocation alloc, VkImageView view) {
    // Quadros ja' gravados ainda podem ler a imagem velha pelo slot: ela vive
    // ate' este slot de quadro voltar (todos os quadros em voo terminaram).
    VulkanContext* vk = m_vk;
    vk->deferFrameCleanup([vk, image, alloc, view]() {
        vkDestroyImageView(vk->device(), view, nullptr);
        vmaDestroyImage(vk->allocator(), image, alloc);
    });
}

void TextureStreamer::dropStreamedImage(Entry& e) {
    if (e.image == VK_NULL_HANDLE) return;
    if (e.alive) {
        if (!m_bindless->updateTextureIfGenerationSafe(e.slot, e.generation, e.baseView, e.sampler)) {
            e.alive = false;
        } else {
            pointSlot(e, e.baseView);
            m_bindless->flushUpdatesSafe();
        }
    }
    m_stats.streamedBytes -= e.imageBytes;
    releaseRamp(e);
    releaseImage(e.image, e.alloc, e.view);
    e.image = VK_NULL_HANDLE;
    e.alloc = VK_NULL_HANDLE;
    e.view = VK_NULL_HANDLE;
    e.imageBytes = 0;
    e.residentMip = e.baseMip;
    e.visibleMip = e.baseMip;
}

void TextureStreamer::releaseRamp(Entry& e) {
    if (e.ramp.empty()) return;
    // Quadros em voo ainda podem amostrar pela view do degrau.
    VulkanContext* vk = m_vk;
    std::vector<VkImageView> views = std::move(e.ramp);
    e.ramp.clear();
    e.rampLevel = 0;
    vk->deferFrameCleanup([vk, views]() {
        for (VkImageView v : views) vkDestroyImageView(vk->device(), v, nullptr);
    });
}

void TextureStreamer::stepRamps() {
    bool changed = false;
    for (size_t k = 0; k < m_ramping.size();) {
        Entry& e = m_entries[m_ramping[k]];
        if (!e.alive || e.ramp.empty() || e.rampLevel == 0 || e.image == VK_NULL_HANDLE) {
            m_ramping[k] = m_ramping.back();
            m_ramping.pop_back();
            continue;
        }
        if (m_frame < e.rampNext) {
            ++k;
            continue;
        }
        --e.rampLevel;
        const VkImageView v = e.rampLevel > 0 ? e.ramp[e.rampLevel - 1] : e.view;
        if (!m_bindless->updateTextureIfGenerationSafe(e.slot, e.generation, v, e.sampler)) {
            e.alive = false;
            dropStreamedImage(e);
            ++k;
            continue;
        }
        pointSlot(e, v);
        e.visibleMip = e.residentMip + e.rampLevel;
        e.rampNext = m_frame + m_settings.rampFrames;
        changed = true;
        if (e.rampLevel == 0) releaseRamp(e);
        ++k;
    }
    if (changed) m_bindless->flushUpdatesSafe();
}

} // namespace eruption
