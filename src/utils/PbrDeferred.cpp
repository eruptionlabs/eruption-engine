#include "utils/PbrDeferred.hpp"

#include "core/Logger.hpp"
#include "renderer/VkCompat.hpp"
#include "utils/BcCompressor.hpp"
#include "utils/EmbeddedTextureBake.hpp"
#include "utils/TextureFormatPolicy.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <tuple>

namespace eruption {

namespace {
std::atomic<uint32_t> g_syncLoads{0}; // blocos lidos na hora por quem precisou do payload
// Orcamento de cada passe: o suficiente para o fim da leitura nao arrastar,
// pouco para o quadro nao notar (16 MB sobem em ~1 ms por PCIe).
// 24 MB por passe deram quadros de 37-40 ms logo apos a troca (copia para o staging + criacao das imagens); 6 MB
// (3 MB nao melhorou o pior quadro: mapa de floresta ~27 ms nos dois) mantem o pior quadro em ~25-30 ms.
constexpr uint64_t kBatchBytes = 6ull << 20;
// Bytes lidos e ainda sem subir: a leitura espera quando passa disto (RAM).
constexpr uint64_t kMaxReadyBytes = 256ull << 20;
constexpr size_t kMaxBatchesInFlight = 2;

uint64_t align16(uint64_t v) { return (v + 15) & ~uint64_t(15); }

uint64_t chainBytes(const EtexData& e) {
    uint64_t b = 0;
    for (uint32_t s : e.mipSizes) b += s;
    return b;
}
} // namespace

// ---------------------------------------------------------------------------
// Leitura de um bloco do pacote
// ---------------------------------------------------------------------------
void testDiskThrottle(uint64_t bytes, bool seek) {
    static const double kMbps = [] {
        const char* e = std::getenv("ERUPTION_TEST_DISK_MBPS");
        return e ? std::atof(e) : 0.0;
    }();
    if (kMbps <= 0.0) return;
    static const double kSeekMs = [] {
        const char* e = std::getenv("ERUPTION_TEST_DISK_SEEK_MS");
        return e ? std::atof(e) : 0.0;
    }();
    const double ms = static_cast<double>(bytes) / (kMbps * 1048.576) + (seek ? kSeekMs : 0.0);
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

bool readPbrBlock(const std::string& path, uint64_t offset, uint64_t bytes, EtexData& out) {
    if (bytes < 20 || bytes > (1ull << 30)) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(bytes));
    {
        // Uma leitura grande por vez no disco (ver diskReadMutex).
        std::lock_guard<std::mutex> disco(diskReadMutex());
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        f.seekg(static_cast<std::streamoff>(offset));
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(bytes));
        if (static_cast<uint64_t>(f.gcount()) != bytes) return false;
        testDiskThrottle(bytes, true);
    }
    size_t cursor = 0;
    EtexData e;
    if (!readEtexBytes(buf.data(), buf.size(), cursor, e) || cursor != buf.size()) return false;
    out = std::move(e);
    return true;
}

const EtexData& PbrDeferredSource::load() const {
    std::call_once(m_once, [this] {
        g_syncLoads.fetch_add(1);
        if (!readPbrBlock(path, offset, bytes, m_data)) {
            ERUPTION_LOG_WARN("[PBRDEFER] leitura do bloco em '%s'+%llu falhou", path.c_str(),
                              static_cast<unsigned long long>(offset));
            m_data = EtexData{};
        }
        m_loaded.store(true, std::memory_order_release);
    });
    return m_data;
}

// ---------------------------------------------------------------------------
// Gerente
// ---------------------------------------------------------------------------
PbrDeferred& PbrDeferred::instance() {
    static PbrDeferred s;
    return s;
}

int PbrDeferred::testOverride() {
    static const int v = [] {
        const char* e = std::getenv("ERUPTION_TEST_PBR_DEFER");
        return e ? (e[0] == '0' ? 0 : (e[0] == '2' ? 2 : 1)) : -1;
    }();
    return v;
}

bool PbrDeferred::enabled(const BindlessDescriptor* bindless) {
    if (testOverride() == 0) return false;
    return bindless && bindless->layerSize() == 0;
}

void PbrDeferred::createPlaceholders(VulkanContext* ctx) {
    if (m_phView[0] != VK_NULL_HANDLE) return;
    // Normal plano (0,0,1) e MRAH-W neutro: metal 0, rugosidade media, AO 1,
    // altura no meio (sem deslocamento).
    const uint8_t px[2][4] = {{128, 128, 255, 255}, {0, 160, 255, 128}};
    for (int k = 0; k < 2; ++k) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {1, 1, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(ctx->allocator(), &ii, &ai, &m_phImage[k], &m_phAlloc[k], nullptr) != VK_SUCCESS) {
            m_phImage[k] = VK_NULL_HANDLE;
            continue;
        }
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        if (!ctx->createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc))
            continue;
        void* mapped = nullptr;
        vmaMapMemory(ctx->allocator(), stagingAlloc, &mapped);
        std::memcpy(mapped, px[k], 4);
        vmaUnmapMemory(ctx->allocator(), stagingAlloc);
        const VkImage img = m_phImage[k];
        ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            ctx->cmdImageBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy r{};
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.layerCount = 1;
            r.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            ctx->cmdImageBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_2_SHADER_READ_BIT);
        });
        vmaDestroyBuffer(ctx->allocator(), staging, stagingAlloc);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkc::createImageView(ctx->device(), &vi, nullptr, &m_phView[k]);
    }
}

VkImageView PbrDeferred::placeholderView(VulkanContext* ctx, bool isNormal) {
    std::lock_guard<std::mutex> lock(m_mtx);
    createPlaceholders(ctx);
    return m_phView[isNormal ? 0 : 1];
}

void PbrDeferred::registerSlot(VulkanContext* ctx, BindlessDescriptor* bindless, VkSampler sampler, uint32_t index,
                               bool isNormal, std::shared_ptr<PbrDeferredSource> src) {
    if (!ctx || !bindless || index == 0 || !src) return;
    std::lock_guard<std::mutex> lock(m_mtx);
    createPlaceholders(ctx);
    bindless->updateTexture(index, m_phView[isNormal ? 0 : 1], sampler);
    m_entries.emplace_back();
    Entry& e = m_entries.back();
    e.index = index;
    e.gen = bindless->slotGenerationSafe(index);
    e.isNormal = isNormal;
    e.sampler = sampler;
    e.src = std::move(src);
    m_byIndex[index] = &e;
    if (m_total == 0) m_t0 = std::chrono::steady_clock::now();
    ++m_total;
    m_totalBytes += e.src->bytes;
    m_logged = false;
    m_pending.fetch_add(1);
}

void PbrDeferred::addAlias(BindlessDescriptor* bindless, uint32_t index, uint32_t alias) {
    if (!bindless || alias == 0) return;
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_byIndex.find(index);
    if (it == m_byIndex.end()) return;
    Entry& e = *it->second;
    e.aliases.push_back({alias, bindless->slotGenerationSafe(alias)});
    // Ja' trocada: o alias entra direto na imagem final.
    if (e.state.load() == Done) bindless->updateTextureSafe(alias, e.view, e.sampler);
}

void PbrDeferred::prioritize(const std::vector<uint32_t>& height, const std::vector<uint32_t>& rest) {
    std::lock_guard<std::mutex> lock(m_mtx);
    uint32_t n = 0;
    uint64_t bytes = 0;
    for (int tier = 2; tier >= 1; --tier) {
        for (const uint32_t index : tier == 2 ? height : rest) {
            const auto it = m_byIndex.find(index);
            if (it == m_byIndex.end() || it->second->prio || it->second->state.load() == Done) continue;
            it->second->prio = static_cast<uint8_t>(tier);
            if (tier == 2) { // o log conta so' a altura: e' ela que da' o relevo
                bytes += it->second->src->bytes;
                ++n;
            }
        }
    }
    if (n == 0) return;
    m_prioTotal = n;
    m_prioBytes = bytes;
    m_prioPending.store(n);
    m_prioT0 = std::chrono::steady_clock::now();
}

void PbrDeferred::prioDone(const Entry& e) {
    if (e.prio != 2 || m_prioPending.load() == 0) return;
    if (m_prioPending.fetch_sub(1) != 1) return;
    ERUPTION_LOG_WARN("[PBRDEFER] relevo do chao (MRAH-W): %u blocos, %.1f MB em %.2f s depois da carga", m_prioTotal,
                      static_cast<double>(m_prioBytes) / 1048576.0,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - m_prioT0).count());
}

void PbrDeferred::release() {
    m_gate.store(true);
}

void PbrDeferred::drainBlocking(VulkanContext* ctx, BindlessDescriptor* bindless) {
    m_gate.store(true);
    while (m_pending.load() > 0 || !m_batches.empty()) {
        pump(ctx, bindless);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void PbrDeferred::startReader() {
    if (m_reader.joinable()) return;
    m_stop.store(false);
    m_reader = std::thread([this] { readerLoop(); });
}

void PbrDeferred::stopReader() {
    m_stop.store(true);
    if (m_reader.joinable()) m_reader.join();
    m_stop.store(false);
}

void PbrDeferred::readerLoop() {
    static const int kDelayMs = [] {
        const char* e = std::getenv("ERUPTION_TEST_PBR_DEFER_DELAY_MS");
        return e ? std::max(0, std::atoi(e)) : 0;
    }();
    std::string lastPath;
    uint64_t lastOffset = 0;
    while (!m_stop.load()) {
        Entry* pick = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            // Ordem do arquivo a partir da ultima leitura (varredura circular):
            // o disco anda sempre para frente.
            // Os do chao primeiro (prioritize), cada grupo na ordem do arquivo.
            for (int pass = 0; pass < 3 && !pick; ++pass) {
                Entry* best = nullptr;
                Entry* wrap = nullptr;
                for (Entry& e : m_entries) {
                    if (e.state.load() != Queued) continue;
                    if (e.prio != 2 - pass) continue;
                    const bool ahead = e.src->path == lastPath && e.src->offset >= lastOffset;
                    Entry*& slot = ahead ? best : wrap;
                    if (!slot || std::tie(e.src->path, e.src->offset) < std::tie(slot->src->path, slot->src->offset))
                        slot = &e;
                }
                pick = best ? best : wrap;
            }
            if (pick) pick->state.store(Reading);
        }
        if (!pick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // Memoria: espera o quadro subir o que ja' foi lido.
        while (m_readyBytes.load() > kMaxReadyBytes && !m_stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (m_stop.load()) {
            pick->state.store(Queued);
            break;
        }
        if (kDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));
        EtexData data;
        const bool ok = readPbrBlock(pick->src->path, pick->src->offset, pick->src->bytes, data);
        lastPath = pick->src->path;
        lastOffset = pick->src->offset;
        if (!ok || !data.valid()) {
            ERUPTION_LOG_WARN("[PBRDEFER] bloco em '%s'+%llu nao leu; o slot %u fica com a imagem neutra",
                              pick->src->path.c_str(), static_cast<unsigned long long>(pick->src->offset), pick->index);
            pick->state.store(Dead);
            m_pending.fetch_sub(1);
            prioDone(*pick);
            continue;
        }
        pick->data = std::move(data);
        m_readyBytes.fetch_add(pick->data.payload.size());
        pick->state.store(Ready, std::memory_order_release);
    }
}

void PbrDeferred::destroyEntryImage(VulkanContext* ctx, Entry& e) {
    if (e.view != VK_NULL_HANDLE) vkDestroyImageView(ctx->device(), e.view, nullptr);
    if (e.image != VK_NULL_HANDLE) vmaDestroyImage(ctx->allocator(), e.image, e.alloc);
    e.view = VK_NULL_HANDLE;
    e.image = VK_NULL_HANDLE;
    e.alloc = VK_NULL_HANDLE;
}

void PbrDeferred::retire(VulkanContext* ctx, BindlessDescriptor* bindless, bool wait) {
    for (size_t k = 0; k < m_batches.size();) {
        Batch& b = m_batches[k];
        const VkResult r = wait ? vkWaitForFences(ctx->device(), 1, &b.fence, VK_TRUE, UINT64_MAX)
                                : vkGetFenceStatus(ctx->device(), b.fence);
        if (r != VK_SUCCESS) {
            ++k;
            continue;
        }
        for (Entry* e : b.items) {
            // A imagem que chegou: o slot (e os alias) passam a mostra-la.
            // Slot ja' devolvido (outra geracao) = a imagem nao serve mais.
            const bool alive = e->state.load() == Uploading;
            if (alive && bindless->updateTextureIfGenerationSafe(e->index, e->gen, e->view, e->sampler)) {
                for (const Alias& a : e->aliases)
                    bindless->updateTextureIfGenerationSafe(a.index, a.gen, e->view, e->sampler);
                e->state.store(Done);
                ++m_done;
                m_pending.fetch_sub(1);
                prioDone(*e);
            } else {
                destroyEntryImage(ctx, *e);
                e->state.store(Dead);
                if (alive) m_pending.fetch_sub(1);
                prioDone(*e);
            }
        }
        vmaDestroyBuffer(ctx->allocator(), b.staging, b.stagingAlloc);
        vkDestroyFence(ctx->device(), b.fence, nullptr);
        vkFreeCommandBuffers(ctx->device(), m_pool, 1, &b.cmd);
        m_batches.erase(m_batches.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

void PbrDeferred::pump(VulkanContext* ctx, BindlessDescriptor* bindless) {
    if (!ctx || !bindless) return;
    if (m_pending.load() == 0 && m_batches.empty()) {
        if (!m_logged && m_total > 0) {
            m_logged = true;
            ERUPTION_LOG_WARN("[PBRDEFER] PBR completo: %u de %u blocos, %.1f MB em %.2f s depois da carga "
                              "(%u lidos na hora por consumidores)",
                              m_done, m_total, static_cast<double>(m_totalBytes) / 1048576.0,
                              std::chrono::duration<double>(std::chrono::steady_clock::now() - m_t0).count(),
                              g_syncLoads.load());
        }
        return;
    }
    if (!m_gate.load()) return;
    startReader();
    retire(ctx, bindless, false);
    if (m_batches.size() >= kMaxBatchesInFlight) return;

    // Lote: o que ja' esta' lido, ate' o orcamento (ao menos um bloco).
    std::vector<Entry*> items;
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (int pass = 0; pass < 3; ++pass) {
            bool full = false;
            for (Entry& e : m_entries) {
                if (e.state.load(std::memory_order_acquire) != Ready || e.prio != 2 - pass) continue;
                const uint64_t b = align16(chainBytes(e.data));
                if (!items.empty() && total + b > kBatchBytes) { full = true; break; }
                items.push_back(&e);
                total += b;
            }
            if (full) break;
        }
    }
    if (items.empty()) return;

    if (m_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = ctx->queueFamilies().graphicsFamily;
        vkCreateCommandPool(ctx->device(), &pi, nullptr, &m_pool);
    }

    // Imagens do lote; as que a memoria recusar ficam com a neutra.
    std::vector<Entry*> ok;
    uint64_t okBytes = 0;
    for (Entry* e : items) {
        const EtexData& d = e->data;
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = static_cast<VkFormat>(d.vkFormat);
        ii.extent = {d.width, d.height, 1};
        ii.mipLevels = d.mipCount;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        // Igual ao envio de sempre (uploadPbrTextureSlot): bloco pode ser copiado para camada.
        if (texfmt::blockInfo(ii.format).bytes > 0) ii.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(ctx->allocator(), &ii, &ai, &e->image, &e->alloc, nullptr) != VK_SUCCESS) {
            e->image = VK_NULL_HANDLE;
            e->state.store(Dead);
            m_readyBytes.fetch_sub(d.payload.size());
            m_pending.fetch_sub(1);
            prioDone(*e);
            e->data = EtexData{};
            continue;
        }
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = e->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = d.mipCount;
        vi.subresourceRange.layerCount = 1;
        if (vkc::createImageView(ctx->device(), &vi, nullptr, &e->view) != VK_SUCCESS) {
            vmaDestroyImage(ctx->allocator(), e->image, e->alloc);
            e->image = VK_NULL_HANDLE;
            e->state.store(Dead);
            m_readyBytes.fetch_sub(d.payload.size());
            m_pending.fetch_sub(1);
            prioDone(*e);
            e->data = EtexData{};
            continue;
        }
        okBytes += align16(chainBytes(d));
        ok.push_back(e);
    }
    if (ok.empty()) return;

    Batch b;
    if (!ctx->createBuffer(std::max<uint64_t>(okBytes, 16), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VMA_MEMORY_USAGE_CPU_ONLY, b.staging, b.stagingAlloc)) {
        for (Entry* e : ok) destroyEntryImage(ctx, *e); // volta a Ready: tenta no proximo quadro
        return;
    }
    uint8_t* mapped = nullptr;
    vmaMapMemory(ctx->allocator(), b.stagingAlloc, reinterpret_cast<void**>(&mapped));

    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = m_pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx->device(), &ca, &b.cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(b.cmd, &bi);

    uint64_t off = 0;
    std::vector<VkBufferImageCopy> regions;
    for (Entry* e : ok) {
        EtexData& d = e->data;
        off = align16(off);
        std::memcpy(mapped + off, d.payload.data(), d.payload.size());
        ctx->cmdImageBarrier(b.cmd, e->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT);
        regions.assign(d.mipCount, VkBufferImageCopy{});
        uint64_t mo = 0;
        for (uint32_t i = 0; i < d.mipCount; ++i) {
            VkBufferImageCopy& r = regions[i];
            r.bufferOffset = off + mo;
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.mipLevel = i;
            r.imageSubresource.layerCount = 1;
            r.imageExtent = {std::max(1u, d.width >> i), std::max(1u, d.height >> i), 1};
            mo += d.mipSizes[i];
        }
        vkCmdCopyBufferToImage(b.cmd, b.staging, e->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        ctx->cmdImageBarrier(b.cmd, e->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT);
        off += mo;
        m_readyBytes.fetch_sub(d.payload.size());
        d = EtexData{}; // o payload ja' esta' no staging
        e->state.store(Uploading);
    }
    vmaUnmapMemory(ctx->allocator(), b.stagingAlloc);
    vmaFlushAllocation(ctx->allocator(), b.stagingAlloc, 0, VK_WHOLE_SIZE);
    vkEndCommandBuffer(b.cmd);
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx->device(), &fi, nullptr, &b.fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &b.cmd;
    vkQueueSubmit(ctx->graphicsQueue(), 1, &si, b.fence);
    b.items = std::move(ok);
    m_batches.push_back(std::move(b));
}

void PbrDeferred::cancelAll(VulkanContext* ctx, BindlessDescriptor* bindless) {
    stopReader();
    m_gate.store(false);
    if (!ctx) return;
    bool any = !m_batches.empty();
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        for (Entry& e : m_entries)
            if (e.image != VK_NULL_HANDLE) any = true;
    }
    if (any) vkDeviceWaitIdle(ctx->device());
    retire(ctx, bindless, true);
    std::lock_guard<std::mutex> lock(m_mtx);
    for (Entry& e : m_entries) {
        // Os descritores voltam a imagem neutra antes de a imagem real morrer
        // (um slot que sobrar vivo nao aponta para uma view destruida).
        if (e.image != VK_NULL_HANDLE && bindless) {
            const VkImageView ph = m_phView[e.isNormal ? 0 : 1];
            if (ph != VK_NULL_HANDLE) {
                bindless->updateTextureIfGenerationSafe(e.index, e.gen, ph, e.sampler);
                for (const Alias& a : e.aliases) bindless->updateTextureIfGenerationSafe(a.index, a.gen, ph, e.sampler);
            }
        }
    }
    // As escritas na fila (inclusive as da imagem real que o retire acabou de enfileirar)
    // sobem ANTES de as views morrerem: o driver le' a view de cada escrita pendente, e uma
    // view destruida ali derrubava o processo no vkUpdateDescriptorSets (troca de mapa com
    // um lote do PBR adiado recem-terminado).
    if (bindless) bindless->flushUpdatesSafe();
    for (Entry& e : m_entries) destroyEntryImage(ctx, e);
    if (m_pending.load() > 0)
        ERUPTION_LOG_WARN("[PBRDEFER] troca de mapa com %zu bloco(s) PBR ainda sem imagem final (descartados)",
                          m_pending.load());
    m_entries.clear();
    m_byIndex.clear();
    m_pending.store(0);
    m_prioPending.store(0);
    m_readyBytes.store(0);
    m_total = 0;
    m_done = 0;
    m_totalBytes = 0;
    m_logged = true;
}

void PbrDeferred::shutdown(VulkanContext* ctx, BindlessDescriptor* bindless) {
    cancelAll(ctx, bindless);
    if (!ctx) return;
    for (int k = 0; k < 2; ++k) {
        if (m_phView[k] != VK_NULL_HANDLE) vkDestroyImageView(ctx->device(), m_phView[k], nullptr);
        if (m_phImage[k] != VK_NULL_HANDLE) vmaDestroyImage(ctx->allocator(), m_phImage[k], m_phAlloc[k]);
        m_phView[k] = VK_NULL_HANDLE;
        m_phImage[k] = VK_NULL_HANDLE;
        m_phAlloc[k] = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx->device(), m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
}

} // namespace eruption
