#if defined(__EMSCRIPTEN__)
#include "platform/web/WebMirror.hpp"
#endif
#include "renderer/TextureLayers.hpp"

#include "core/JobSystem.hpp"
#include "core/Logger.hpp"
#include "renderer/BandwidthMeter.hpp"
#include "renderer/LayerBlockRules.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/VkCompat.hpp"
#include "utils/TextureEncoder.hpp"
#include "utils/TextureFormatPolicy.hpp"

#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace eruption {

namespace {

using layerrules::isBlockFormat;
using layerrules::isSrgb;

// Cache em disco das camadas em bloco (TC-05): chave = conteudo RGBA8 da camada
// (todos os mips, como a copia os desenhou) + formato, lado e parametros do
// codificador. Troca o pacote ou a textura, troca a chave; nada a invalidar.
// ERUPTION_TEX_LAYER_CACHE=0 desliga (medida a frio).
constexpr uint32_t kBlockCacheVersion = 1;

uint64_t layerHash(const uint8_t* p, size_t n, uint64_t seed) {
    uint64_t h = seed ^ 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

bool layerCacheOn() {
    static const bool on = [] {
        const char* e = std::getenv("ERUPTION_TEX_LAYER_CACHE");
        return !(e && e[0] == '0');
    }();
    return on;
}

// Poda do cache das camadas em bloco: o que nao e' tocado ha' mais de 30 dias sai, e acima
// de 512 MB sai o mais antigo primeiro. Uma vez por execucao, ao abrir o diretorio.
void pruneLayerCache(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    constexpr uintmax_t kMaxBytes = 512ull * 1024 * 1024;
    constexpr auto kMaxAge = std::chrono::hours(24 * 30);
    struct Entry { fs::path path; fs::file_time_type time; uintmax_t size; };
    std::vector<Entry> all;
    uintmax_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const auto t = it->last_write_time(e2);
        const auto sz = it->file_size(e2);
        if (e2) continue;
        all.push_back({it->path(), t, sz});
        total += sz;
    }
    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    const auto now = fs::file_time_type::clock::now();
    size_t removed = 0;
    for (const Entry& e : all) {
        if (total <= kMaxBytes && now - e.time <= kMaxAge) continue;
        std::error_code e3;
        if (fs::remove(e.path, e3)) {
            total -= std::min(total, e.size);
            ++removed;
        }
    }
    if (removed) ERUPTION_LOG_WARN("[TEXCOMP] cache das camadas: %zu arquivo(s) podado(s), %.1f MB ficam",
                                   removed, double(total) / (1024.0 * 1024.0));
}

// Diretorio fixo (nao o cwd): ERUPTION_TEX_LAYER_CACHE_DIR, senao $XDG_CACHE_HOME ou $HOME/.cache
// + eruption/layers; sem nenhum dos dois (celular), o relativo de sempre.
const std::string& layerCacheDir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* e = std::getenv("ERUPTION_TEX_LAYER_CACHE_DIR"); e && *e) d = e;
        else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) d = std::string(x) + "/eruption/layers";
        else if (const char* h = std::getenv("HOME"); h && *h) d = std::string(h) + "/.cache/eruption/layers";
        else d = ".cache/eruption/layers";
        if (d.back() != '/') d += '/';
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        pruneLayerCache(d);
        return d;
    }();
    return dir;
}

// Versao dos codificadores na chave: trocar astcenc/etcpak/bc7enc muda os blocos, entao o cache
// antigo nao pode valer. Atualizar ao subir a versao de qualquer um.
constexpr const char* kEncoderTag = "astcenc-5.2.0;etcpak;bc7enc_rdo";

// Formatos que a cópia leva para RGBA8 sem perder nada: 8 bits por canal e os
// comprimidos de faixa normal. Float e profundidade vão para ligação direta.
bool copyable(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        return false;
    default:
        // BC1..BC7, ETC2/EAC e ASTC LDR são faixas contíguas no enum.
        return isBlockFormat(f);
    }
}

VkImageMemoryBarrier layerBarrier(VkImage image, uint32_t layer, uint32_t baseMip, uint32_t mipCount,
                                  VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, layer, 1};
    return b;
}

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, const VkImageMemoryBarrier& b) {
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkShaderModule loadModule(VkDevice device, const char* path) {
    const auto code = ShaderCompiler::loadSPIRV(path);
    if (code.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &module);
    return module;
}

} // namespace

bool TextureLayers::layerBDisabled() {
    static const bool disabled = std::getenv("ERUPTION_DISABLE_LAYER_B") != nullptr;
    return disabled;
}

std::vector<VkDescriptorSetLayoutBinding> TextureLayers::layoutBindings() {
    const uint32_t count = layerBDisabled() ? kLayersBBinding : kBindingCount;
    std::vector<VkDescriptorSetLayoutBinding> bindings(count);
    for (uint32_t i = 0; i < count; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == kTableBinding ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return bindings;
}

std::vector<VkDescriptorPoolSize> TextureLayers::poolSizes(uint32_t sets) {
    const uint32_t count = layerBDisabled() ? kLayersBBinding : kBindingCount;
    return {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (count - 1) * sets},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets}};
}

// Teto de camadas do array: o limite do dispositivo (2048 em varias GPUs de
// celular). ERUPTION_TEST_MAX_LAYERS=<n> (teste) baixa o teto para provar o
// caminho "array cheio" no PC.
static uint32_t layerLimit(const VulkanContext* ctx) {
    uint32_t limit = std::min(ctx->deviceProperties().limits.maxImageArrayLayers, 0xFFFFu);
    if (const char* e = std::getenv("ERUPTION_TEST_MAX_LAYERS")) {
        const long v = std::strtol(e, nullptr, 10);
        if (v >= 64) limit = std::min(limit, static_cast<uint32_t>(v));
    }
    return limit;
}

bool TextureLayers::init(VulkanContext* ctx, uint32_t slotCount, uint32_t layerSize, const VkDescriptorSet* sets,
                         VkImageView defaultView, VkSampler defaultSampler, uint32_t reserve, uint32_t reserveB) {
    m_ctx = ctx;
    m_blockCacheHits = m_blockEncoded = 0;
    m_blockMs = 0.0;
    m_layerSize = 1;
    while (m_layerSize * 2 <= std::clamp(layerSize, 16u, 4096u)) m_layerSize *= 2;
    m_mipLevels = 1;
    while ((m_layerSize >> m_mipLevels) > 0) ++m_mipLevels;
    m_defaultView = defaultView;
    m_defaultSampler = defaultSampler;
    for (uint32_t f = 0; f < kFrames; ++f) m_sets[f] = sets[f];

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ctx->applyAnisotropy(si, 16.0f);
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(ctx->device(), &si, nullptr, &m_linearSampler);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(ctx->device(), &si, nullptr, &m_nearestSampler);
    m_nearestSamplers.insert(m_nearestSampler);

    const uint32_t maxLayers = layerLimit(ctx);
    m_blockFormat = VK_FORMAT_UNDEFINED;
    if (m_blockRequest != VK_FORMAT_UNDEFINED) blockArrayInit();
    const bool block = m_blockFormat != VK_FORMAT_UNDEFINED;
    const uint32_t initial = std::clamp(reserve, 32u, block ? std::min(maxLayers, m_blockMaxLayers) : maxLayers);
    if (!createArray(m_array, block ? m_blockFormat : VK_FORMAT_R8G8B8A8_UNORM, initial, "A")) return false;
    if (initial > 32) ERUPTION_LOG_WARN("Texture layers: %u layers reserved at start", initial);
    // ERUPTION_DISABLE_LAYER_B=1 (diagnostico): nao cria o 2o array nem o usa;
    // as ligacoes 11/12 recebem a vista do A (precisam apontar para algo valido).
    m_bDisabled = layerBDisabled();
    if (m_bDisabled) {
        ERUPTION_LOG_WARN("Texture layers: array B desligado (ERUPTION_DISABLE_LAYER_B)");
    } else {
        const uint32_t initialB = std::clamp(reserveB, 2u, maxLayers);
        if (!createArray(m_arrayB, VK_FORMAT_R8G8B8A8_UNORM, initialB, "B")) return false;
        if (initialB > 2) ERUPTION_LOG_WARN("Texture layers: %u layers reserved at start (array B)", initialB);
    }
    if (!createCopyPipeline()) return false;

    m_table.assign(slotCount, 0u);
    m_places.assign(slotCount, Place{kNone, 0});
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (!ctx->createBuffer(slotCount * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VMA_MEMORY_USAGE_CPU_TO_GPU, m_tableBuffer[f], m_tableAlloc[f]))
            return false;
        void* mapped = nullptr;
        vmaMapMemory(ctx->allocator(), m_tableAlloc[f], &mapped);
        m_tableMapped[f] = static_cast<uint32_t*>(mapped);
        std::memset(mapped, 0, slotCount * sizeof(uint32_t));

        VkDescriptorBufferInfo bi{m_tableBuffer[f], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_sets[f];
        w.dstBinding = kTableBinding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx->device(), 1, &w, 0, nullptr);
        writeArrayBindings(m_sets[f]);
        writeDirectBindings(m_sets[f]);
    }

    // A camada 0 é a textura padrão: slot sem textura (tabela zerada) lê dela.
    m_jobs[kNone] = CopyJob{defaultView, defaultSampler, false, false};
    runCopies(SIZE_MAX);
    ERUPTION_LOG_WARN("Texture layers: %ux%u per texture, %u mips, array A %s (GPU without descriptor indexing)",
                      m_layerSize, m_layerSize, m_mipLevels, texfmt::formatTag(m_array.format));
    return true;
}

bool TextureLayers::blockArrayInit() {
    const VkFormat f = m_blockRequest;
    const char* tag = texfmt::formatTag(f);
#if defined(__EMSCRIPTEN__)
    // O desenho WebGL2 le a textura pelo slot (WebMirror), nao pelo array.
    ERUPTION_LOG_WARN("Texture layers: array em bloco %s ignorado no navegador; fica RGBA8", tag);
    return false;
#endif
    if (layerBDisabled()) {
        // Sem o B, o que nao vai em bloco (sprites, dados RGBA8) nao teria camada.
        ERUPTION_LOG_WARN("Texture layers: array em bloco %s pede o array B (ERUPTION_DISABLE_LAYER_B); fica RGBA8", tag);
        return false;
    }
    if (!texenc::available(f) || !isBlockFormat(f)) {
        ERUPTION_LOG_WARN("Texture layers: sem codificador para %s; array A fica RGBA8", tag);
        return false;
    }
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(m_ctx->physicalDevice(), f, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkImageFormatProperties ip{};
    if ((props.optimalTilingFeatures & need) != need ||
        vkGetPhysicalDeviceImageFormatProperties(
            m_ctx->physicalDevice(), f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
            &ip) != VK_SUCCESS ||
        ip.maxArrayLayers < 64 || ip.maxExtent.width < m_layerSize || ip.maxMipLevels < m_mipLevels) {
        ERUPTION_LOG_WARN("Texture layers: a GPU nao copia/amostra %s em array; array A fica RGBA8", tag);
        return false;
    }
    m_blockMaxLayers = std::min<uint32_t>(ip.maxArrayLayers, 0xFFFFu);
    m_blockFormat = f;
    return true;
}

bool TextureLayers::directSource(const CopyJob& job, DirectSource& out) const {
    static const bool off = [] {
        const char* e = std::getenv("ERUPTION_TEX_LAYER_DIRECT");
        return e && e[0] == '0'; // A/B: tudo pelo caminho que codifica
    }();
    if (off) return false;
    vkc::ViewInfo v;
    bw::ImageDesc d;
    if (!vkc::viewInfo(job.view, v) || !bw::findImage(v.image, d)) return false;
    layerrules::Source s;
    s.imageFormat = d.format;
    s.viewFormat = v.format;
    s.width = d.extent.width;
    s.height = d.extent.height;
    s.mips = d.mips;
    s.baseMip = v.range.baseMipLevel;
    s.levelCount = v.range.levelCount;
    s.baseLayer = v.range.baseArrayLayer;
    s.transferSrc = (d.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    s.view2D = v.type == VK_IMAGE_VIEW_TYPE_2D;
    s.singleSample = d.samples == VK_SAMPLE_COUNT_1_BIT;
    s.optimal = d.tiling == VK_IMAGE_TILING_OPTIMAL;
    uint32_t mip = 0;
    if (layerrules::directCopy(s, m_blockFormat, m_layerSize, m_mipLevels, mip)) {
        out = DirectSource{v.image, mip, s.baseLayer, false};
        return true;
    }
    if (layerrules::etc2AddAlpha(s, m_blockFormat, m_layerSize, m_mipLevels, mip)) {
        out = DirectSource{v.image, mip, s.baseLayer, true};
        return true;
    }
    return false;
}

void TextureLayers::runDirectCopies(const std::vector<DirectTarget>& targets) {
    const auto started = std::chrono::steady_clock::now();
    const uint32_t mips = m_mipLevels;
    constexpr size_t kPerSubmit = 32;
    for (size_t first = 0; first < targets.size(); first += kPerSubmit) {
        const size_t count = std::min(kPerSubmit, targets.size() - first);
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            std::vector<VkImageCopy> regions(mips);
            for (size_t i = 0; i < count; ++i) {
                const DirectTarget& t = targets[first + i];
                VkImageMemoryBarrier b[2] = {
                    layerBarrier(t.src.image, t.src.layer, t.src.mip, mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT),
                    layerBarrier(m_array.image, t.layer, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
                };
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 2, b);
                for (uint32_t m = 0; m < mips; ++m) {
                    VkImageCopy& r = regions[m];
                    r = VkImageCopy{};
                    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t.src.mip + m, t.src.layer, 1};
                    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, t.layer, 1};
                    const uint32_t side = std::max(1u, m_layerSize >> m);
                    r.extent = {side, side, 1};
                }
                vkCmdCopyImage(cmd, t.src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_array.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, regions.data());
                b[0] = layerBarrier(t.src.image, t.src.layer, t.src.mip, mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                                    VK_ACCESS_SHADER_READ_BIT);
                b[1] = layerBarrier(m_array.image, t.layer, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_ACCESS_SHADER_READ_BIT);
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     nullptr, 0, nullptr, 2, b);
            }
        });
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    m_blockDirect += static_cast<uint32_t>(targets.size());
    m_blockMs += ms;
    ERUPTION_LOG_WARN("Texture layers: %zu camadas em %s copiadas direto da fonte em %.1f ms", targets.size(),
                      texfmt::formatTag(m_blockFormat), ms);
}

void TextureLayers::runAlphaCopies(const std::vector<DirectTarget>& targets) {
    const auto started = std::chrono::steady_clock::now();
    VmaAllocator vma = m_ctx->allocator();
    const uint32_t mips = m_mipLevels;
    std::vector<size_t> rgbOff(mips), rgbaOff(mips);
    std::vector<uint32_t> sides(mips);
    size_t rgbBytes = 0, rgbaBytes = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        sides[m] = std::max(1u, m_layerSize >> m);
        rgbOff[m] = rgbBytes;
        rgbaOff[m] = rgbaBytes;
        const size_t blocks = static_cast<size_t>((sides[m] + 3) / 4) * ((sides[m] + 3) / 4);
        rgbBytes += blocks * 8;
        rgbaBytes += blocks * 16;
    }
    const size_t group = std::min<size_t>(kBlockGroup * 2, targets.size());
    VkBuffer readBuf = VK_NULL_HANDLE, upBuf = VK_NULL_HANDLE;
    VmaAllocation readAlloc = VK_NULL_HANDLE, upAlloc = VK_NULL_HANDLE;
    if (!m_ctx->createBuffer(group * rgbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, readBuf,
                             readAlloc) ||
        !m_ctx->createBuffer(group * rgbaBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, upBuf,
                             upAlloc)) {
        ERUPTION_LOG_ERROR("Texture layers: sem memoria para a leitura/envio das camadas ETC2");
        if (readBuf != VK_NULL_HANDLE) vmaDestroyBuffer(vma, readBuf, readAlloc);
        std::vector<uint32_t> layers;
        for (const DirectTarget& t : targets) layers.push_back(t.layer);
        settleLayers(layers);
        return;
    }
    void* readPtr = nullptr;
    void* upPtr = nullptr;
    vmaMapMemory(vma, readAlloc, &readPtr);
    vmaMapMemory(vma, upAlloc, &upPtr);
    for (size_t first = 0; first < targets.size(); first += group) {
        const size_t count = std::min(group, targets.size() - first);
        // 1) GPU: os blocos RGB da fonte, do mip que vira o 0 da camada ate' o 1x1.
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            std::vector<VkBufferImageCopy> regions(mips);
            for (size_t i = 0; i < count; ++i) {
                const DirectSource& src = targets[first + i].src;
                barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        layerBarrier(src.image, src.layer, src.mip, mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT));
                for (uint32_t m = 0; m < mips; ++m) {
                    VkBufferImageCopy& r = regions[m];
                    r = VkBufferImageCopy{};
                    r.bufferOffset = i * rgbBytes + rgbOff[m];
                    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src.mip + m, src.layer, 1};
                    r.imageExtent = {sides[m], sides[m], 1};
                }
                vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuf, mips,
                                       regions.data());
                barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        layerBarrier(src.image, src.layer, src.mip, mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                                     VK_ACCESS_SHADER_READ_BIT));
            }
            VkBufferMemoryBarrier bb{};
            bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = readBuf;
            bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb,
                                 0, nullptr);
        });
        vmaInvalidateAllocation(vma, readAlloc, 0, VK_WHOLE_SIZE);
        // 2) CPU: alfa 255 na frente de cada bloco (16 B por bloco no A).
        layerrules::etc2RgbToRgba(static_cast<const uint8_t*>(readPtr), count * rgbBytes / 8,
                                  static_cast<uint8_t*>(upPtr));
        vmaFlushAllocation(vma, upAlloc, 0, VK_WHOLE_SIZE);
        // 3) GPU: sobe para as camadas do A.
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            std::vector<VkBufferImageCopy> regions(mips);
            for (size_t i = 0; i < count; ++i) {
                const uint32_t layer = targets[first + i].layer;
                barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        layerBarrier(m_array.image, layer, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT));
                for (uint32_t m = 0; m < mips; ++m) {
                    VkBufferImageCopy& r = regions[m];
                    r = VkBufferImageCopy{};
                    r.bufferOffset = i * rgbaBytes + rgbaOff[m];
                    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, layer, 1};
                    r.imageExtent = {sides[m], sides[m], 1};
                }
                vkCmdCopyBufferToImage(cmd, upBuf, m_array.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips,
                                       regions.data());
                barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        layerBarrier(m_array.image, layer, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT));
            }
        });
    }
    vmaUnmapMemory(vma, readAlloc);
    vmaUnmapMemory(vma, upAlloc);
    vmaDestroyBuffer(vma, readBuf, readAlloc);
    vmaDestroyBuffer(vma, upBuf, upAlloc);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    m_blockDirect += static_cast<uint32_t>(targets.size());
    m_blockMs += ms;
    ERUPTION_LOG_WARN("Texture layers: %zu camadas ETC2 RGB da fonte com alfa 255 na frente em %.1f ms", targets.size(),
                      ms);
}

// Falha sem conteudo (memoria): a camada do A ao menos sai de UNDEFINED, que o
// shader nao pode amostrar; fica com o que a memoria tiver.
void TextureLayers::settleLayers(const std::vector<uint32_t>& layers) {
    if (layers.empty()) return;
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        for (uint32_t layer : layers)
            barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    layerBarrier(m_array.image, layer, 0, m_mipLevels, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT));
    });
}

bool TextureLayers::wantsBlock(const CopyJob& job) const {
    // Ja' comprimida na origem (a perda ja' foi aceita) ou cor sRGB (o albedo,
    // que o caminho com bindless tambem comprime). Dados lineares sem compressao
    // e as sem filtro (pixel art) ficam como chegaram.
    return m_blockFormat != VK_FORMAT_UNDEFINED && layerrules::wantsBlock(job.format, job.nearest);
}

void TextureLayers::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    destroyArray(m_array);
    destroyArray(m_arrayB);
    destroyArray(m_scratch);
    m_blockFormat = VK_FORMAT_UNDEFINED;
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (m_tableBuffer[f] == VK_NULL_HANDLE) continue;
        vmaUnmapMemory(m_ctx->allocator(), m_tableAlloc[f]);
        vmaDestroyBuffer(m_ctx->allocator(), m_tableBuffer[f], m_tableAlloc[f]);
        m_tableBuffer[f] = VK_NULL_HANDLE;
        m_tableMapped[f] = nullptr;
    }
    if (m_copyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_copyPipeline, nullptr);
    m_copyPipeline = VK_NULL_HANDLE;
    if (m_copyLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_copyLayout, nullptr);
    if (m_copySetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_copySetLayout, nullptr);
    if (m_copyPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_copyPool, nullptr);
    m_copyLayout = VK_NULL_HANDLE;
    m_copySetLayout = VK_NULL_HANDLE;
    m_copyPool = VK_NULL_HANDLE;
    if (m_linearSampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_linearSampler, nullptr);
    if (m_nearestSampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_nearestSampler, nullptr);
    m_linearSampler = m_nearestSampler = VK_NULL_HANDLE;
    m_jobs.clear();
    m_table.clear();
    m_places.clear();
    m_nearestSamplers.clear();
    m_ctx = nullptr;
}

bool TextureLayers::createArray(LayerArray& arr, VkFormat format, uint32_t capacity, const char* label) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {m_layerSize, m_layerSize, 1};
    ii.mipLevels = m_mipLevels;
    ii.arrayLayers = capacity;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Formato de bloco nao e' alvo de desenho: so' recebe copia (buffer ou outro array).
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               (isBlockFormat(format) ? VkImageUsageFlags{0} : VkImageUsageFlags{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT});
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &ii, &ai, &arr.image, &arr.alloc, nullptr) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Texture layers: could not create a %u-layer %s array (%s)", capacity,
                           texfmt::formatTag(format), label);
        return false;
    }
    bw::tagImage(arr.image, bw::kTexLayers); // censo de texturas (spec 51)
    {
        // Memoria pedida por camada: e' o que cada passo de crescimento pede a
        // mais (com o array velho vivo), o que estourou numa GPU de celular.
        const double bytes = static_cast<double>(m_ctx->allocationSize(arr.alloc));
        ERUPTION_LOG_WARN("Texture layers: array %s %s com %u camadas: %.1f MB pedidos, %.1f KB por camada", label,
                          texfmt::formatTag(format), capacity, bytes / (1024.0 * 1024.0), bytes / 1024.0 / capacity);
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = arr.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipLevels, 0, capacity};
    vkc::createImageView(m_ctx->device(), &vi, nullptr, &arr.view);
    arr.format = format;
    arr.capacity = capacity;

    // Camadas ainda sem cópia ficam legíveis (lixo, mas nenhum slot aponta
    // para elas).
    const VkImage image = arr.image;
    const uint32_t mips = m_mipLevels;
    m_ctx->immediateSubmit([image, mips, capacity](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b = layerBarrier(image, 0, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT);
        b.subresourceRange.layerCount = capacity;
        barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, b);
    });
    return true;
}

void TextureLayers::destroyArray(LayerArray& arr) {
    if (arr.view != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), arr.view, nullptr);
    if (arr.image != VK_NULL_HANDLE) vmaDestroyImage(m_ctx->allocator(), arr.image, arr.alloc);
    arr = LayerArray{};
}

// Array cheio: outro com o dobro de camadas, as antigas copiadas. Acontece na
// carga do mapa; espera a GPU para trocar a imagem em todos os sets.
bool TextureLayers::growArray(LayerArray& arr) {
    uint32_t maxLayers = layerLimit(m_ctx);
    if (isBlockFormat(arr.format)) maxLayers = std::min(maxLayers, m_blockMaxLayers);
    // Cresce 1/4 por vez, não o dobro: a matriz é a maior alocação do
    // celular (1024 camadas de 256² = 344 MB com ~960 em uso) e o dobro
    // pediria 688 MB mais a antiga viva durante a cópia.
    // Com o A em bloco, o B em RGBA8 so' guarda o que nao vai em bloco (sprites,
    // dados lineares): poucas camadas, entao passos de 8 (cada camada RGBA8 de
    // 256 com mips sao 0,33 MB que ficariam reservados sem uso).
    const uint32_t step = (&arr == &m_arrayB && m_blockFormat != VK_FORMAT_UNDEFINED) ? 8u : 32u;
    const uint32_t capacity = std::min(arr.capacity + std::max(step, arr.capacity / 4), maxLayers);
    if (capacity <= arr.capacity) return false;
    vkDeviceWaitIdle(m_ctx->device());
    LayerArray bigger;
    if (!createArray(bigger, arr.format, capacity, &arr == &m_arrayB ? "B (crescendo)" : "A (crescendo)")) return false;

    // Só as camadas em uso, em fatias de kGrowChunk por envio: as ~760
    // camadas num envio só levaram 2,5 s na GE8320 e prenderam a GPU.
    const VkImage from = arr.image, to = bigger.image;
    const uint32_t used = arr.next, mips = m_mipLevels, size = m_layerSize;
    for (uint32_t base = 0; base < used; base += kGrowChunk) {
        const uint32_t count = std::min(kGrowChunk, used - base);
        m_ctx->immediateSubmit([from, to, base, count, mips, size](VkCommandBuffer cmd) {
            VkImageMemoryBarrier b[2] = {
                layerBarrier(from, base, 0, mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT),
                layerBarrier(to, base, 0, mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
            };
            b[0].subresourceRange.layerCount = b[1].subresourceRange.layerCount = count;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 2, b);
            std::vector<VkImageCopy> regions(mips);
            for (uint32_t m = 0; m < mips; ++m) {
                VkImageCopy& r = regions[m];
                r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, base, count};
                r.dstSubresource = r.srcSubresource;
                const uint32_t side = std::max(1u, size >> m);
                r.extent = {side, side, 1};
            }
            vkCmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           mips, regions.data());
            b[0] = layerBarrier(from, base, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, 0);
            b[1] = layerBarrier(to, base, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
            b[0].subresourceRange.layerCount = b[1].subresourceRange.layerCount = count;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 2, b);
        });
    }
    bigger.next = arr.next;
    bigger.freeLayers = std::move(arr.freeLayers);
    destroyArray(arr);
    arr = std::move(bigger);
    for (VkDescriptorSet set : m_sets) writeArrayBindings(set);
    ERUPTION_LOG_INFO("Texture layers: array %s grown to %u layers", &arr == &m_arrayB ? "B" : "A", capacity);
    return true;
}

bool TextureLayers::createCopyPipeline() {
    VkDevice dev = m_ctx->device();
    VkDescriptorSetLayoutBinding src{};
    src.binding = 0;
    src.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    src.descriptorCount = 1;
    src.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &src;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_copySetLayout) != VK_SUCCESS) return false;
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_copySetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_copyLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kCopyBatch};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kCopyBatch;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_copyPool) != VK_SUCCESS) return false;

    VkShaderModule vert = loadModule(dev, "postprocess/fullscreen.vert.spv");
    VkShaderModule frag = loadModule(dev, "common/layer_copy.frag.spv");
    if (vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE) {
        std::vector<VkPipelineShaderStageCreateInfo> stages(2);
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[0].pName = stages[1].pName = "main";
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        m_copyPipeline = PipelineBuilder()
                             .setShaderStages(stages)
                             .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                             .setViewport(0, 0, static_cast<float>(m_layerSize), static_cast<float>(m_layerSize))
                             .setScissor(0, 0, m_layerSize, m_layerSize)
                             .setPolygonMode(VK_POLYGON_MODE_FILL)
                             .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                             .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
                             .setBlendState({blend})
                             .setLayout(m_copyLayout)
                             .setColorAttachmentFormats({VK_FORMAT_R8G8B8A8_UNORM}) // B, passagem e o A em RGBA8
                             .build(dev);
    }
    if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(dev, vert, nullptr);
    if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(dev, frag, nullptr);
    return m_copyPipeline != VK_NULL_HANDLE;
}

// Primeiro o array A (ate o teto do dispositivo), depois o B. O resultado traz
// kBLayerFlag quando a camada e' do B; 0 = nenhum dos dois tem lugar.
uint32_t TextureLayers::allocLayer(bool block) {
    for (LayerArray* arr : {&m_array, &m_arrayB}) {
        if (arr == &m_arrayB && m_bDisabled) continue;
        // A em bloco so' recebe o que vai em bloco; o resto (e o que nao couber
        // no A) vai para o B, em RGBA8.
        if (arr == &m_array && m_blockFormat != VK_FORMAT_UNDEFINED && !block) continue;
        const uint32_t flag = arr == &m_arrayB ? kBLayerFlag : 0u;
        if (!arr->freeLayers.empty()) {
            const uint32_t layer = arr->freeLayers.back();
            arr->freeLayers.pop_back();
            return layer | flag;
        }
        if (arr->next >= arr->capacity && !growArray(*arr)) continue;
        return arr->next++ | flag;
    }
    {
        // Um erro por textura enchia o log (centenas de linhas iguais em um
        // celular de teste); registra so' nas potencias de dois, com o total.
        ++m_layersLost;
        if ((m_layersLost & (m_layersLost - 1)) == 0)
            ERUPTION_LOG_ERROR("Texture layers: array full (%u layers); %u failed attempt(s) so far, %zu slot(s) without a layer now",
                               m_array.capacity + m_arrayB.capacity, m_layersLost, m_lostSlots.size());
        return 0;
    }
}

void TextureLayers::freePlace(uint32_t slot) {
    Place& p = m_places[slot];
    if (isArrayKind(p.kind)) {
        if (p.layer != 0) (p.kind >= kBKind ? m_arrayB : m_array).freeLayers.push_back(p.layer);
    } else if (p.kind != kNone) {
        m_direct[p.kind - kDirectKind] = Direct{};
        for (bool& d : m_directDirty) d = true;
    }
    p = Place{kNone, 0};
}

void TextureLayers::setTable(uint32_t slot, uint32_t kind, uint32_t layer) {
    m_table[slot] = (kind << 16) | layer;
    for (bool& d : m_tableDirty) d = true;
}

void TextureLayers::assign(uint32_t slot, VkImageView view, VkSampler sampler, bool dynamic) {
    if (slot >= m_table.size()) return;
#if defined(__EMSCRIPTEN__)
    web::setSlotView(slot, view); // o desenho WebGL2 le a textura pelo slot
#endif
    const VkFormat format = vkc::viewFormat(view);
    if (!dynamic && copyable(format)) {
        m_jobs[slot] = CopyJob{view, sampler, m_nearestSamplers.count(sampler) != 0, isSrgb(format), format};
        return;
    }
    m_jobs.erase(slot);
    m_lostSlots.erase(slot);
    Place& p = m_places[slot];
    uint32_t k = kDirectCount;
    if (isDirectKind(p.kind)) {
        k = p.kind - kDirectKind;
    } else {
        freePlace(slot);
        for (uint32_t i = 0; i < kDirectCount && k == kDirectCount; ++i)
            if (m_direct[i].slot == UINT32_MAX) k = i;
    }
    if (k == kDirectCount) {
        if (!m_warnedDirectFull) {
            ERUPTION_LOG_WARN("Texture layers: all %u direct bindings in use; slot %u shows the default texture",
                              kDirectCount, slot);
            m_warnedDirectFull = true;
        }
        setTable(slot, 0, 0);
        return;
    }
    m_direct[k] = Direct{slot, view, sampler};
    p = Place{kDirectKind + k, 0};
    setTable(slot, kDirectKind + k, 0);
    for (bool& d : m_directDirty) d = true;
}

void TextureLayers::release(uint32_t slot) {
    if (slot >= m_table.size()) return;
    m_jobs.erase(slot);
    m_lostSlots.erase(slot);
    freePlace(slot);
    setTable(slot, 0, 0);
}

void TextureLayers::recordCopy(VkCommandBuffer cmd, VkDescriptorSet copySet, VkImage image, uint32_t layer,
                               bool srgb, VkImageView layerView) {
    const int32_t size = static_cast<int32_t>(m_layerSize);
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            layerBarrier(image, layer, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));

    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = layerView;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    m_ctx->cmdBeginRendering(cmd, {att}, nullptr, nullptr, {m_layerSize, m_layerSize});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_copyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_copyLayout, 0, 1, &copySet, 0, nullptr);
    const uint32_t encodeSrgb = srgb ? 1u : 0u;
    vkCmdPushConstants(cmd, m_copyLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(encodeSrgb), &encodeSrgb);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    m_ctx->cmdEndRendering(cmd);

    // Mips por blit, cada nível a partir do anterior.
    barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            layerBarrier(image, layer, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT));
    for (uint32_t m = 1; m < m_mipLevels; ++m) {
        barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                layerBarrier(image, layer, m, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                             VK_ACCESS_TRANSFER_WRITE_BIT));
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, layer, 1};
        blit.srcOffsets[1] = {std::max(1, size >> (m - 1)), std::max(1, size >> (m - 1)), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, layer, 1};
        blit.dstOffsets[1] = {std::max(1, size >> m), std::max(1, size >> m), 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                layerBarrier(image, layer, m, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT));
    }
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            layerBarrier(image, layer, 0, m_mipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_SHADER_READ_BIT));
}

bool TextureLayers::allocCopySets(size_t count, std::vector<VkDescriptorSet>& sets) {
    VkDevice dev = m_ctx->device();
    vkResetDescriptorPool(dev, m_copyPool, 0);
    std::vector<VkDescriptorSetLayout> layouts(count, m_copySetLayout);
    sets.assign(count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_copyPool;
    ai.descriptorSetCount = static_cast<uint32_t>(count);
    ai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai, sets.data()) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Texture layers: copy descriptor sets failed");
        return false;
    }
    return true;
}

void TextureLayers::writeCopySource(VkDescriptorSet set, const CopyJob& job) {
    VkDescriptorImageInfo ii{job.nearest ? m_nearestSampler : m_linearSampler, job.view,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
}

VkImageView TextureLayers::makeLayerView(const LayerArray& arr, uint32_t layer) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = arr.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = arr.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    VkImageView view = VK_NULL_HANDLE;
    vkc::createImageView(m_ctx->device(), &vi, nullptr, &view);
    return view;
}

// A em bloco (TC-05): cada camada passa por um RGBA8 de passagem (a mesma copia
// e os mesmos mips por blit do caminho RGBA8), volta para a CPU, vira blocos
// (do cache ou do codificador, uma camada por thread) e sobe para o A. Rodadas
// de kBlockGroup camadas: a memoria extra e' a de uma rodada (passagem + leitura
// + envio), nao a da fila inteira.
void TextureLayers::runBlockCopies(const std::vector<BlockTarget>& targets) {
    const auto started = std::chrono::steady_clock::now();
    VkDevice dev = m_ctx->device();
    VmaAllocator vma = m_ctx->allocator();
    const VkFormat fmt = m_blockFormat;
    const uint32_t mips = m_mipLevels;
    // Onde fica cada mip de uma camada: no RGBA8 lido de volta e nos blocos.
    std::vector<size_t> rgbaOff(mips), blockOff(mips);
    std::vector<uint32_t> sides(mips);
    size_t rgbaBytes = 0, blockBytes = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        sides[m] = std::max(1u, m_layerSize >> m);
        rgbaOff[m] = rgbaBytes;
        rgbaBytes += static_cast<size_t>(sides[m]) * sides[m] * 4;
        blockOff[m] = blockBytes;
        blockBytes += texenc::levelBytes(fmt, static_cast<int>(sides[m]), static_cast<int>(sides[m]));
    }
    if (m_scratch.image == VK_NULL_HANDLE &&
        !createArray(m_scratch, VK_FORMAT_R8G8B8A8_UNORM, kBlockGroup, "de passagem")) {
        ERUPTION_LOG_ERROR("Texture layers: sem camada de passagem; %zu camada(s) em bloco sem conteudo", targets.size());
        std::vector<uint32_t> layers;
        for (const BlockTarget& t : targets) layers.push_back(t.layer);
        settleLayers(layers);
        return;
    }
    const size_t group = std::min<size_t>(kBlockGroup, targets.size());
    VkBuffer readBuf = VK_NULL_HANDLE, upBuf = VK_NULL_HANDLE;
    VmaAllocation readAlloc = VK_NULL_HANDLE, upAlloc = VK_NULL_HANDLE;
    if (!m_ctx->createBuffer(group * rgbaBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, readBuf,
                             readAlloc) ||
        !m_ctx->createBuffer(group * blockBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, upBuf,
                             upAlloc)) {
        ERUPTION_LOG_ERROR("Texture layers: sem memoria para a leitura/envio das camadas em bloco");
        if (readBuf != VK_NULL_HANDLE) vmaDestroyBuffer(vma, readBuf, readAlloc);
        std::vector<uint32_t> layers;
        for (const BlockTarget& t : targets) layers.push_back(t.layer);
        settleLayers(layers);
        return;
    }
    void* readPtr = nullptr;
    void* upPtr = nullptr;
    vmaMapMemory(vma, readAlloc, &readPtr);
    vmaMapMemory(vma, upAlloc, &upPtr);
    const uint8_t* readMapped = static_cast<const uint8_t*>(readPtr);
    uint8_t* upMapped = static_cast<uint8_t*>(upPtr);

    const bool cacheOn = layerCacheOn();
    const std::string dir = cacheOn ? layerCacheDir() : std::string();
    const char* tag = texfmt::formatTag(fmt);
    const size_t texels0 = static_cast<size_t>(m_layerSize) * m_layerSize;
    std::atomic<uint32_t> hits{0}, encoded{0}, opaque{0}, failed{0};
    double encodeMs = 0.0;
    // ERUPTION_TEX_LAYER_PSNR=1 (medida): PSNR do mip 0 de cada camada em bloco
    // contra o RGBA8 que a copia desenhou (cor e alfa a parte), no log.
    static const bool kMeasure = [] {
        const char* e = std::getenv("ERUPTION_TEX_LAYER_PSNR");
        return e && e[0] == '1';
    }();
    std::vector<LayerPsnr> psnr(kMeasure ? targets.size() : 0);

    for (size_t first = 0; first < targets.size(); first += group) {
        const size_t count = std::min(group, targets.size() - first);
        std::vector<char> copyFailed(count, 0);
        // 1) GPU: a copia de sempre numa camada de passagem, e de volta para a CPU.
        for (size_t sub = 0; sub < count; sub += kCopyBatch) {
            const size_t n = std::min<size_t>(kCopyBatch, count - sub);
            std::vector<VkDescriptorSet> sets;
            if (!allocCopySets(n, sets)) {
                // Sem copia, o buffer de leitura ficaria com lixo de um lote anterior: nao
                // codifica nem grava no cache estas camadas.
                for (size_t i = 0; i < n; ++i) copyFailed[sub + i] = 1;
                continue;
            }
            std::vector<VkImageView> views(n);
            for (size_t i = 0; i < n; ++i) {
                writeCopySource(sets[i], targets[first + sub + i].job);
                views[i] = makeLayerView(m_scratch, static_cast<uint32_t>(sub + i));
            }
            m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
                for (size_t i = 0; i < n; ++i)
                    recordCopy(cmd, sets[i], m_scratch.image, static_cast<uint32_t>(sub + i),
                               targets[first + sub + i].srgb, views[i]);
                VkImageMemoryBarrier b = layerBarrier(m_scratch.image, static_cast<uint32_t>(sub), 0, mips,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                      VK_ACCESS_TRANSFER_READ_BIT);
                b.subresourceRange.layerCount = static_cast<uint32_t>(n);
                barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, b);
                std::vector<VkBufferImageCopy> regions;
                regions.reserve(n * mips);
                for (size_t i = 0; i < n; ++i)
                    for (uint32_t m = 0; m < mips; ++m) {
                        VkBufferImageCopy r{};
                        r.bufferOffset = (sub + i) * rgbaBytes + rgbaOff[m];
                        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, static_cast<uint32_t>(sub + i), 1};
                        r.imageExtent = {sides[m], sides[m], 1};
                        regions.push_back(r);
                    }
                vkCmdCopyImageToBuffer(cmd, m_scratch.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuf,
                                       static_cast<uint32_t>(regions.size()), regions.data());
                VkBufferMemoryBarrier bb{};
                bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = readBuf;
                bb.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                     &bb, 0, nullptr);
            });
            for (VkImageView v : views) vkDestroyImageView(dev, v, nullptr);
        }
        vmaInvalidateAllocation(vma, readAlloc, 0, VK_WHOLE_SIZE);

        // 2) CPU: blocos do cache ou do codificador, uma camada por thread.
        const auto e0 = std::chrono::steady_clock::now();
        JobSystem::instance().parallelFor(static_cast<uint32_t>(count), [&](uint32_t i) {
            const BlockTarget& t = targets[first + i];
            if (copyFailed[i]) {
                ++failed;
                std::memset(upMapped + i * blockBytes, 0, blockBytes);
                return;
            }
            // Copia local: a memoria mapeada pode ser lenta de ler muitas vezes.
            const std::vector<uint8_t> rgba(readMapped + i * rgbaBytes, readMapped + (i + 1) * rgbaBytes);
            uint8_t* dst = upMapped + i * blockBytes;
            bool isOpaque = true;
            for (size_t px = 0; px < texels0 && isOpaque; ++px) isOpaque = rgba[px * 4 + 3] == 255;
            if (isOpaque) ++opaque;
            auto measure = [&](const uint8_t* blocks) {
                if (!kMeasure) return;
                const int s0 = static_cast<int>(m_layerSize);
                std::vector<uint8_t> back;
                if (!texenc::decodeLevel(fmt, blocks, texenc::levelBytes(fmt, s0, s0), s0, s0, back)) return;
                double seRgb = 0.0, seA = 0.0;
                for (size_t px = 0; px < texels0; ++px)
                    for (int c = 0; c < 4; ++c) {
                        const double d = double(back[px * 4 + size_t(c)]) - double(rgba[px * 4 + size_t(c)]);
                        (c < 3 ? seRgb : seA) += d * d;
                    }
                auto db = [](double mse) { return mse <= 1e-12 ? 99.0f : float(10.0 * std::log10(65025.0 / mse)); };
                psnr[first + i] = LayerPsnr{db(seRgb / (3.0 * texels0)), isOpaque ? -1.0f : db(seA / double(texels0)),
                                            t.srgb, true};
            };
            // Cor sRGB com peso de luminancia; normal, dados e a padrao com erro igual por canal.
            texenc::Params p;
            p.linear = !t.srgb;
            const uint64_t seed = (static_cast<uint64_t>(fmt) << 32) ^ (static_cast<uint64_t>(m_layerSize) << 12) ^
                                  (static_cast<uint64_t>(kBlockCacheVersion) << 4) ^ (p.linear ? 1u : 0u) ^
                                  (static_cast<uint64_t>(p.bc7Partitions) << 40) ^
                                  (static_cast<uint64_t>(p.astcQuality * 16.0f) << 48) ^
                                  layerHash(reinterpret_cast<const uint8_t*>(kEncoderTag), std::strlen(kEncoderTag), 0x51ull);
            char name[96];
            std::snprintf(name, sizeof(name), "%s_%u_%016llx.bin", tag, m_layerSize,
                          static_cast<unsigned long long>(layerHash(rgba.data(), rgba.size(), seed)));
            const std::string path = dir + name;
            if (cacheOn) {
                std::ifstream f(path, std::ios::binary);
                std::vector<uint8_t> cached(blockBytes);
                if (f && f.read(reinterpret_cast<char*>(cached.data()), static_cast<std::streamsize>(blockBytes)) &&
                    f.peek() == std::char_traits<char>::eof()) {
                    std::memcpy(dst, cached.data(), blockBytes);
                    ++hits;
                    measure(cached.data());
                    return;
                }
            }
            std::vector<uint8_t> all(blockBytes), level;
            bool ok = true;
            for (uint32_t m = 0; m < mips && ok; ++m) {
                const int s = static_cast<int>(sides[m]);
                ok = texenc::encodeLevel(fmt, rgba.data() + rgbaOff[m], s, s, level, p) &&
                     level.size() == texenc::levelBytes(fmt, s, s);
                if (ok) std::memcpy(all.data() + blockOff[m], level.data(), level.size());
            }
            if (!ok) {
                ++failed;
                std::memset(dst, 0, blockBytes);
                return;
            }
            std::memcpy(dst, all.data(), blockBytes);
            ++encoded;
            measure(all.data());
            if (cacheOn) {
                // Grava ao lado e troca: nunca deixa entrada pela metade.
                const std::string tmp = path + ".tmp";
                bool written = false;
                {
                    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
                    o.write(reinterpret_cast<const char*>(all.data()), static_cast<std::streamsize>(blockBytes));
                    written = o.good();
                }
                std::error_code ec;
                if (written) std::filesystem::rename(tmp, path, ec);
                if (!written || ec) std::filesystem::remove(tmp, ec);
            }
        });
        encodeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e0).count();
        vmaFlushAllocation(vma, upAlloc, 0, VK_WHOLE_SIZE);

        // 3) GPU: os blocos sobem para as camadas do A (todos os mips).
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            std::vector<VkBufferImageCopy> regions(mips);
            for (size_t i = 0; i < count; ++i) {
                const uint32_t layer = targets[first + i].layer;
                barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        layerBarrier(m_array.image, layer, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT));
                for (uint32_t m = 0; m < mips; ++m) {
                    VkBufferImageCopy& r = regions[m];
                    r = VkBufferImageCopy{};
                    r.bufferOffset = i * blockBytes + blockOff[m];
                    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, layer, 1};
                    r.imageExtent = {sides[m], sides[m], 1};
                }
                vkCmdCopyBufferToImage(cmd, upBuf, m_array.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips,
                                       regions.data());
                barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        layerBarrier(m_array.image, layer, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT));
            }
        });
    }
    vmaUnmapMemory(vma, readAlloc);
    vmaUnmapMemory(vma, upAlloc);
    vmaDestroyBuffer(vma, readBuf, readAlloc);
    vmaDestroyBuffer(vma, upBuf, upAlloc);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    m_blockCacheHits += hits.load();
    m_blockEncoded += encoded.load();
    m_blockMs += ms;
    if (kMeasure) {
        for (const LayerPsnr& q : psnr)
            if (q.valid) m_psnr.push_back(q);
        logLayerPsnr(tag);
    }
    if (failed.load() > 0)
        ERUPTION_LOG_ERROR("Texture layers: %u camada(s) nao codificadas em %s (ficam pretas)", failed.load(), tag);
    ERUPTION_LOG_WARN("Texture layers: %zu camadas em %s em %.1f ms (%.1f codificando): %u do cache, %u codificadas, "
                      "%u opacas, %zu com alfa",
                      targets.size(), tag, ms, encodeMs, hits.load(), encoded.load(), opaque.load(),
                      targets.size() - opaque.load());
}

// Resumo acumulado do ERUPTION_TEX_LAYER_PSNR, separado como o codificador separa:
// camadas opacas (alfa todo 255) e com alfa (RGB e o canal alfa a parte).
void TextureLayers::logLayerPsnr(const char* tag) const {
    auto summary = [](std::vector<float> v, char* out, size_t n) {
        if (v.empty()) {
            std::snprintf(out, n, "nenhuma");
            return;
        }
        std::sort(v.begin(), v.end());
        std::snprintf(out, n, "%zu, min %.1f, p10 %.1f, mediana %.1f dB", v.size(), v.front(), v[v.size() / 10],
                      v[v.size() / 2]);
    };
    std::vector<float> opaque, withAlpha, alpha;
    for (const LayerPsnr& q : m_psnr) {
        (q.alpha < 0.0f ? opaque : withAlpha).push_back(q.rgb);
        if (q.alpha >= 0.0f) alpha.push_back(q.alpha);
    }
    char o[128], w[128], a[128];
    summary(opaque, o, sizeof(o));
    summary(withAlpha, w, sizeof(w));
    summary(alpha, a, sizeof(a));
    ERUPTION_LOG_WARN("[TEXCOMP] PSNR camada %s x RGBA8 (mip 0, RGB): opacas %s; com alfa %s; canal alfa %s", tag, o, w,
                      a);
}

void TextureLayers::runCopies(size_t maxCopies) {
    const auto started = std::chrono::steady_clock::now();
    struct Target {
        uint32_t slot;
        uint32_t layer;
        bool inB; // a camada e' do 2o array
        CopyJob job;
    };
    std::vector<Target> targets;
    targets.reserve(std::min(maxCopies, m_jobs.size()));
    for (auto it = m_jobs.begin(); it != m_jobs.end() && targets.size() < maxCopies; it = m_jobs.erase(it)) {
        const uint32_t slot = it->first;
        const CopyJob& job = it->second;
        if (slot == kNone) {
            targets.push_back({kNone, 0, false, job}); // textura padrão: camada 0
            continue;
        }
        const Place p = m_places[slot];
        const bool block = wantsBlock(job);
        // Camada do A em bloco nao serve para quem fica em RGBA8 (o contrario serve).
        const bool placeInBlock = m_blockFormat != VK_FORMAT_UNDEFINED && p.kind < kBKind;
        uint32_t layer;
        if (isArrayKind(p.kind) && p.layer != 0 && (block || !placeInBlock)) {
            layer = p.layer | (p.kind >= kBKind ? kBLayerFlag : 0u); // mesma textura trocada: reaproveita a camada
        } else {
            freePlace(slot);
            layer = allocLayer(block);
            if (layer == 0) {
                m_lostSlots.insert(slot);
                continue;
            }
        }
        m_lostSlots.erase(slot);
        targets.push_back({slot, layer & ~kBLayerFlag, (layer & kBLayerFlag) != 0, job});
    }

    // Camada do A em bloco: copia de bloco da fonte quando ela ja' esta' no formato,
    // senao o caminho a parte (desenha, le de volta, codifica, sobe).
    std::vector<Target> plain;
    std::vector<BlockTarget> blocks;
    std::vector<DirectTarget> direct, withAlpha;
    plain.reserve(targets.size());
    for (const Target& t : targets) {
        DirectSource src{};
        if (t.inB || m_blockFormat == VK_FORMAT_UNDEFINED) plain.push_back(t);
        else if (t.slot != kNone && directSource(t.job, src)) (src.addAlpha ? withAlpha : direct).push_back({t.layer, src});
        else blocks.push_back({t.layer, t.job.srgb, t.job});
    }

    VkDevice dev = m_ctx->device();
    for (size_t first = 0; first < plain.size(); first += kCopyBatch) {
        const size_t count = std::min<size_t>(kCopyBatch, plain.size() - first);
        std::vector<VkDescriptorSet> sets;
        if (!allocCopySets(count, sets)) return;
        std::vector<VkImageView> layerViews(count);
        for (size_t i = 0; i < count; ++i) {
            const Target& t = plain[first + i];
            writeCopySource(sets[i], t.job);
            const LayerArray& arr = t.inB ? m_arrayB : m_array;
            layerViews[i] = makeLayerView(arr, t.layer);
        }
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            for (size_t i = 0; i < count; ++i) {
                const Target& t = plain[first + i];
                recordCopy(cmd, sets[i], t.inB ? m_arrayB.image : m_array.image, t.layer, t.job.srgb, layerViews[i]);
            }
        });
        for (VkImageView v : layerViews) vkDestroyImageView(dev, v, nullptr);
    }
    if (!direct.empty()) runDirectCopies(direct);
    if (!withAlpha.empty()) runAlphaCopies(withAlpha);
    if (!blocks.empty()) runBlockCopies(blocks);
    // A camada de passagem so' vive enquanto ha copia para o A em bloco.
    if (m_jobs.empty() && m_scratch.image != VK_NULL_HANDLE) destroyArray(m_scratch);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    ERUPTION_LOG_WARN("Texture layers: %zu copies in %.1f ms (%u layers used, %zu waiting)", targets.size(), ms,
                      m_array.next + (m_arrayB.next > 1 ? m_arrayB.next - 1 : 0), m_jobs.size());
    std::lock_guard<std::mutex> copiedLock(m_copiedMutex);
    for (const Target& t : targets) {
        if (t.slot == kNone) continue;
        const uint32_t kind = (t.job.srgb ? 1u : 0u) | (t.job.nearest ? 2u : 0u) | (t.inB ? kBKind : 0u);
        m_places[t.slot] = Place{kind, t.layer};
        setTable(t.slot, kind, t.layer);
        m_copiedStatic.push_back(t.slot);
    }
}

std::vector<uint32_t> TextureLayers::takeCopiedStatic() {
    std::lock_guard<std::mutex> lock(m_copiedMutex);
    std::vector<uint32_t> out;
    out.swap(m_copiedStatic);
    return out;
}

void TextureLayers::writeArrayBindings(VkDescriptorSet set) {
    const VkDescriptorImageInfo infos[4] = {
        {m_linearSampler, m_array.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_nearestSampler, m_array.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_linearSampler, m_bDisabled ? m_array.view : m_arrayB.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_nearestSampler, m_bDisabled ? m_array.view : m_arrayB.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    const uint32_t bindings[4] = {kLayersBinding, kLayersNearestBinding, kLayersBBinding, kLayersNearestBBinding};
    VkWriteDescriptorSet writes[4] = {};
    const uint32_t writeCount = m_bDisabled ? 2u : 4u; // sem B, as ligacoes 11/12 nem existem no layout
    for (uint32_t i = 0; i < writeCount; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = bindings[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_ctx->device(), writeCount, writes, 0, nullptr);
}

void TextureLayers::writeDirectBindings(VkDescriptorSet set) {
    VkDescriptorImageInfo infos[kDirectCount];
    VkWriteDescriptorSet writes[kDirectCount] = {};
    for (uint32_t k = 0; k < kDirectCount; ++k) {
        const Direct& d = m_direct[k];
        const bool used = d.slot != UINT32_MAX;
        infos[k] = {used ? d.sampler : m_defaultSampler, used ? d.view : m_defaultView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[k].dstSet = set;
        writes[k].dstBinding = kDirectBinding + k;
        writes[k].descriptorCount = 1;
        writes[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[k].pImageInfo = &infos[k];
    }
    vkUpdateDescriptorSets(m_ctx->device(), kDirectCount, writes, 0, nullptr);
}

void TextureLayers::apply(uint32_t frame) {
    if (!m_jobs.empty()) {
        const auto t0 = std::chrono::steady_clock::now();
        runCopies(kCopiesPerFrame);
        while (!m_jobs.empty() &&
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < kCopyBudgetMs)
            runCopies(kCopyBatch);
    }
    if (m_tableDirty[frame]) {
        std::memcpy(m_tableMapped[frame], m_table.data(), m_table.size() * sizeof(uint32_t));
        m_tableDirty[frame] = false;
    }
    if (m_directDirty[frame]) {
        writeDirectBindings(m_sets[frame]);
        m_directDirty[frame] = false;
    }
}

void TextureLayers::applyAll() {
    if (!m_jobs.empty()) runCopies(SIZE_MAX);
    vkDeviceWaitIdle(m_ctx->device());
    for (uint32_t f = 0; f < kFrames; ++f) apply(f);
}

} // namespace eruption
