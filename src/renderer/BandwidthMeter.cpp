#include "renderer/BandwidthMeter.hpp"

#include "core/Logger.hpp"
#include "utils/Profiler.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#define ERUPTION_BW_IMC 1
#include <dirent.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#define ERUPTION_BW_IMC 0
#endif

namespace eruption::bw {

// ---------------------------------------------------------------- formula

namespace {

// Blocos ASTC na ordem do enum (UNORM e SRGB lado a lado): largura x altura.
constexpr uint8_t kAstcBlocks[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

bool in(VkFormat f, VkFormat lo, VkFormat hi) { return f >= lo && f <= hi; }

} // namespace

bool isBlockCompressed(VkFormat f) {
    return in(f, VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

double bytesPerTexel(VkFormat f) {
    if (in(f, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) {
        const auto* b = kAstcBlocks[(f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2];
        return 16.0 / (b[0] * b[1]);
    }
    switch (f) {
    case VK_FORMAT_R4G4_UNORM_PACK8:
    case VK_FORMAT_S8_UINT:
        return 1.0;
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_D16_UNORM:
        return 2.0;
    case VK_FORMAT_D16_UNORM_S8_UINT:
        return 3.0;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return 4.0;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 5.0;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        return 0.5;
    default:
        break;
    }
    if (in(f, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK)) return 1.0; // BC2/3/5/6H/7
    if (in(f, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK)) return 1.0;
    if (in(f, VK_FORMAT_EAC_R11G11_UNORM_BLOCK, VK_FORMAT_EAC_R11G11_SNORM_BLOCK)) return 1.0;
    if (in(f, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SRGB)) return 1.0;
    if (in(f, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SRGB)) return 2.0;
    if (in(f, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SFLOAT)) return 2.0;
    if (in(f, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_B8G8R8_SRGB)) return 3.0;
    if (in(f, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_SINT_PACK32)) return 4.0; // RGBA8/BGRA8/ABGR8/A2*
    if (in(f, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SFLOAT)) return 4.0;
    if (in(f, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT)) return 4.0;
    if (in(f, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SFLOAT)) return 6.0;
    if (in(f, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT)) return 8.0;
    if (in(f, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SFLOAT)) return 8.0;
    if (in(f, VK_FORMAT_R64_UINT, VK_FORMAT_R64_SFLOAT)) return 8.0;
    if (in(f, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SFLOAT)) return 12.0;
    if (in(f, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SFLOAT)) return 16.0;
    if (in(f, VK_FORMAT_R64G64_UINT, VK_FORMAT_R64G64_SFLOAT)) return 16.0;
    return 0.0;
}

namespace {
bool g_noneNative = false;
} // namespace

void setStoreOpNoneNative(bool native) { g_noneNative = native; }
bool storeOpNoneNative() { return g_noneNative; }

Traffic attachmentTraffic(VkFormat f, uint32_t w, uint32_t h, uint32_t layers, VkAttachmentLoadOp load,
                          VkAttachmentStoreOp store) {
    const double bytes = bytesPerTexel(f) * static_cast<double>(w) * h * std::max(1u, layers);
    const uint64_t b = static_cast<uint64_t>(bytes + 0.5);
    Traffic t;
    if (load == VK_ATTACHMENT_LOAD_OP_LOAD) t.read = b;
    if (store == VK_ATTACHMENT_STORE_OP_STORE || (store == VK_ATTACHMENT_STORE_OP_NONE && !g_noneNative)) t.written = b;
    return t;
}

// --------------------------------------------------------------- auditoria

ImageDesc describeImage(const VkImageCreateInfo& ci) {
    ImageDesc d;
    d.format = ci.format;
    d.extent = ci.extent;
    d.mips = std::max(1u, ci.mipLevels);
    d.layers = std::max(1u, ci.arrayLayers);
    d.samples = ci.samples;
    d.tiling = ci.tiling;
    d.usage = ci.usage;
    d.flags = ci.flags;
    for (auto* n = static_cast<const VkBaseInStructure*>(ci.pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) d.formatList = true;
    return d;
}

uint32_t auditRenderTarget(const ImageDesc& d) {
    const VkImageUsageFlags att = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!(d.usage & att)) return 0;
    uint32_t r = 0;
    if (d.usage & VK_IMAGE_USAGE_STORAGE_BIT) r |= kAuditStorage;
    if ((d.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && !d.formatList) r |= kAuditMutableNoList;
    if (d.flags & VK_IMAGE_CREATE_ALIAS_BIT) r |= kAuditAlias;
    if (d.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) r |= kAuditTransferDst;
#ifdef VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT
    if (d.usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT) r |= kAuditHostTransfer;
#endif
    if (d.tiling == VK_IMAGE_TILING_LINEAR) r |= kAuditLinear;
    if (d.samples != VK_SAMPLE_COUNT_1_BIT) r |= kAuditMultisample;
    return r;
}

std::string describeAudit(uint32_t flags) {
    // Fontes: not_commit/compressao_banda/04_hardware_e_apis.md, secao 2.
    static const struct {
        uint32_t bit;
        const char* text;
    } kText[] = {
        {kAuditStorage, "STORAGE (desliga AFBC/DCC; CCS nao vale no acesso de storage)"},
        {kAuditMutableNoList, "MUTABLE_FORMAT sem VkImageFormatListCreateInfo (desliga AFBC/DCC)"},
        {kAuditAlias, "ALIAS (desliga AFBC)"},
        {kAuditTransferDst, "TRANSFER_DST (desliga AFBC em drivers Arm r16p0; copia degrada DCC)"},
        {kAuditHostTransfer, "HOST_TRANSFER (perde a compressao de framebuffer)"},
        {kAuditLinear, "tiling LINEAR (sem compressao)"},
        {kAuditMultisample, "MSAA (sem compressao no Arm; resolver no tile)"},
    };
    std::string s;
    for (const auto& t : kText) {
        if (!(flags & t.bit)) continue;
        if (!s.empty()) s += "; ";
        s += t.text;
    }
    return s;
}

bool clearLosesFastClear(const VkClearColorValue& c, VkFormat f) {
    // Formatos inteiros: o clear e' por valor inteiro, 0 e 1 sao os seguros.
    const bool integer = in(f, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT) || in(f, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT) ||
                         in(f, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT);
    for (int i = 0; i < 4; ++i) {
        if (integer) {
            if (c.uint32[i] > 1u) return true;
        } else if (c.float32[i] != 0.0f && c.float32[i] != 1.0f) {
            return true;
        }
    }
    return false;
}

void blockExtent(VkFormat f, uint32_t& bw, uint32_t& bh) {
    bw = bh = 1;
    if (in(f, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) {
        const auto* b = kAstcBlocks[(f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2];
        bw = b[0];
        bh = b[1];
    } else if (isBlockCompressed(f)) {
        bw = bh = 4; // BC, ETC2 e EAC
    }
}

uint64_t imageBytes(const ImageDesc& d) {
    const double bpt = bytesPerTexel(d.format);
    uint32_t bw = 1, bh = 1;
    blockExtent(d.format, bw, bh);
    double total = 0.0;
    uint32_t w = std::max(1u, d.extent.width), h = std::max(1u, d.extent.height);
    const uint32_t depth = std::max(1u, d.extent.depth);
    for (uint32_t m = 0; m < d.mips; ++m) {
        // Bloco: a menor unidade e' o bloco inteiro (4x4 em BC/ETC, o do
        // formato em ASTC: 6x6 ocupa 16 bytes por 36 texels).
        const uint32_t ew = (w + bw - 1) / bw * bw;
        const uint32_t eh = (h + bh - 1) / bh * bh;
        total += bpt * ew * eh * depth;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return static_cast<uint64_t>(total * d.layers * static_cast<uint32_t>(d.samples) + 0.5);
}

// ------------------------------------------------- contador real (Intel IMC)

namespace {

struct Dram {
    bool tried = false;
    bool ok = false;
    std::string status = "desligado";
    std::vector<int> fds;          // leitura e escrita de cada controlador
    std::vector<bool> isWrite;
    std::vector<double> bytesPerCount;
    std::vector<uint64_t> prev;
    std::chrono::steady_clock::time_point prevT{};
    bool havePrev = false;
};

#if ERUPTION_BW_IMC
std::string readLine(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[256] = {0};
    const bool ok = std::fgets(buf, sizeof(buf), f) != nullptr;
    std::fclose(f);
    if (!ok) return {};
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// "config:8-15" -> deslocamento 8. So' o campo config e' usado pelo IMC.
int formatShift(const std::string& pmu, const std::string& term) {
    const std::string s = readLine(pmu + "/format/" + term);
    int lo = 0, hi = 0;
    if (std::sscanf(s.c_str(), "config:%d-%d", &lo, &hi) >= 1) return lo;
    return -1;
}

// "event=0xff,umask=0x20" -> config pelos campos do formato do PMU.
bool parseEvent(const std::string& pmu, const std::string& spec, uint64_t& config) {
    config = 0;
    size_t pos = 0;
    while (pos < spec.size()) {
        const size_t comma = spec.find(',', pos);
        const std::string kv = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) return false;
        const int shift = formatShift(pmu, kv.substr(0, eq));
        if (shift < 0) return false;
        config |= std::strtoull(kv.substr(eq + 1).c_str(), nullptr, 0) << shift;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return true;
}

void openImc(Dram& d) {
    const std::string root = "/sys/bus/event_source/devices";
    DIR* dir = opendir(root.c_str());
    if (!dir) {
        d.status = "sem /sys/bus/event_source";
        return;
    }
    std::string lastError;
    while (dirent* e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name.rfind("uncore_imc_free_running", 0) != 0) continue;
        const std::string pmu = root + "/" + name;
        const int type = std::atoi(readLine(pmu + "/type").c_str());
        const int cpu = std::atoi(readLine(pmu + "/cpumask").c_str());
        for (const char* ev : {"data_read", "data_write"}) {
            uint64_t config = 0;
            if (!parseEvent(pmu, readLine(pmu + "/events/" + ev), config)) continue;
            const double scale = std::atof(readLine(pmu + "/events/" + ev + ".scale").c_str());
            const std::string unit = readLine(pmu + "/events/" + ev + ".unit");
            double perCount = scale;
            if (unit == "MiB") perCount *= 1024.0 * 1024.0;
            else if (unit == "KiB") perCount *= 1024.0;
            if (perCount <= 0.0) perCount = 64.0;
            perf_event_attr attr{};
            attr.type = static_cast<uint32_t>(type);
            attr.size = sizeof(attr);
            attr.config = config;
            const long fd = syscall(__NR_perf_event_open, &attr, -1, cpu, -1, 0);
            if (fd < 0) {
                lastError = std::string("perf_event_open: ") + std::strerror(errno);
                continue;
            }
            d.fds.push_back(static_cast<int>(fd));
            d.isWrite.push_back(std::strcmp(ev, "data_write") == 0);
            d.bytesPerCount.push_back(perCount);
        }
    }
    closedir(dir);
    if (d.fds.empty()) {
        d.status = lastError.empty() ? std::string("sem PMU uncore_imc_free_running (nao e' Intel?)")
                                     : lastError + " (perf_event_paranoid > 0?)";
        return;
    }
    d.ok = true;
    d.prev.assign(d.fds.size(), 0);
    d.status = "intel_imc";
}

bool readImc(Dram& d, std::vector<uint64_t>& now) {
    now.resize(d.fds.size());
    for (size_t i = 0; i < d.fds.size(); ++i) {
        uint64_t v = 0;
        if (read(d.fds[i], &v, sizeof(v)) != static_cast<ssize_t>(sizeof(v))) return false;
        now[i] = v;
    }
    return true;
}
#endif

// Um anexo aberto no quadro (so' com ERUPTION_TEST_FLUXO_REPORT ligado): a
// auditoria de load/store (spec 52) le' a sequencia de usos de cada imagem.
struct AttachRec {
    const char* pass;
    VkImage image;
    VkFormat format;
    uint32_t w, h, layers;
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    bool depth;
};

struct State {
    std::mutex m;
    bool detail = std::getenv("ERUPTION_TEST_FLUXO_REPORT") != nullptr;
    std::vector<AttachRec> curAtt, lastAtt;
    std::unordered_map<VkImage, ImageDesc> images;
    std::unordered_map<VkImage, TextureCategory> tags; // categoria marcada (tagImage)
    CensusTotal census[kTexCategories];
    std::unordered_map<VkImageView, VkImage> views;
    std::unordered_set<VkImage> audited;
    std::unordered_set<std::string> warned;
    AuditStat audit;
    std::vector<PassStat> cur;
    uint64_t curRead = 0, curWritten = 0, curVertex = 0;
    FrameStat last;
    uint64_t frame = 0;
    uint32_t vendor = 0;
    Dram dram;
};

State& state() {
    static State s;
    return s;
}

thread_local const char* t_pass = nullptr;

bool isTexture(const ImageDesc& d) {
    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT;
    return (d.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(d.usage & rt);
}

void addTexture(AuditStat& a, const ImageDesc& d, bool add) {
    if (!isTexture(d)) return;
    uint64_t& slot = isBlockCompressed(d.format) ? a.textureCompressedBytes : a.textureUncompressedBytes;
    const uint64_t b = imageBytes(d);
    slot = add ? slot + b : (slot > b ? slot - b : 0);
}

TextureCategory categoryOf(const State& s, VkImage image, const ImageDesc& d) {
    const auto it = s.tags.find(image);
    return it != s.tags.end() ? it->second : categorize(d);
}

void addCensus(State& s, TextureCategory c, const ImageDesc& d, bool add) {
    if (c == kTexNone) return;
    CensusTotal& t = s.census[c];
    uint64_t& slot = isBlockCompressed(d.format) ? t.block : t.raw;
    const uint64_t b = imageBytes(d);
    slot = add ? slot + b : (slot > b ? slot - b : 0);
    t.count = add ? t.count + 1 : (t.count > 0 ? t.count - 1 : 0);
}

const char* formatName(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return "R8";
    case VK_FORMAT_R8_UINT: return "R8_UINT";
    case VK_FORMAT_R8G8_UNORM: return "RG8";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "RGB10A2";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11";
    case VK_FORMAT_R16_SFLOAT: return "R16F";
    case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    case VK_FORMAT_R32_SFLOAT: return "R32F";
    case VK_FORMAT_R32G32_SFLOAT: return "RG32F";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "RGBA32F";
    case VK_FORMAT_D16_UNORM: return "D16";
    case VK_FORMAT_D32_SFLOAT: return "D32";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
    default: return nullptr;
    }
}

} // namespace

std::string formatLabel(VkFormat f) {
    if (const char* n = formatName(f)) return n;
    if (in(f, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) {
        const auto* b = kAstcBlocks[(f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2];
        return "ASTC" + std::to_string(b[0]) + "x" + std::to_string(b[1]);
    }
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1";
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2";
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3";
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: return "BC4";
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: return "BC5";
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: return "BC6H";
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7";
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return "ETC2";
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return "ETC2A1";
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return "ETC2A8";
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: case VK_FORMAT_EAC_R11_SNORM_BLOCK: return "EAC_R11";
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: return "EAC_RG11";
    default: return "fmt" + std::to_string(static_cast<int>(f));
    }
}

namespace {

PassStat& passStat(State& s, const char* pass) {
    for (auto& e : s.cur)
        if (e.name == pass) return e;
    s.cur.push_back(PassStat{pass, 0, 0, 0, 0});
    return s.cur.back();
}

void accumulate(State& s, const char* pass, const Traffic& t) {
    PassStat* p = &passStat(s, pass);
    p->read += t.read;
    p->written += t.written;
    s.curRead += t.read;
    s.curWritten += t.written;
}

void auditAttachment(State& s, const char* pass, VkImageView view, VkFormat f, const VkRenderingAttachmentInfo& a,
                     bool color) {
    const auto vi = s.views.find(view);
    if (vi == s.views.end()) return;
    const VkImage image = vi->second;
    const auto ii = s.images.find(image);
    if (ii == s.images.end() || !s.audited.insert(image).second) return;
    ++s.audit.targets;
    const uint32_t flags = auditRenderTarget(ii->second);
    const bool slowClear = color && a.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && clearLosesFastClear(a.clearValue.color, f);
    if (!flags && !slowClear) return;
    if (flags) ++s.audit.flagged;
    // Uma linha por combinacao: um alvo recriado no resize nao repete o aviso.
    const std::string sig = std::string(pass) + "|" + std::to_string(static_cast<int>(f)) + "|" +
                            std::to_string(flags) + (slowClear ? "|c" : "");
    if (!s.warned.insert(sig).second) return;
    ++s.audit.warnings;
    const ImageDesc& d = ii->second;
    std::string what = describeAudit(flags);
    if (slowClear) {
        if (!what.empty()) what += "; ";
        what += "clear fora de {0,1} (perde o fast clear na AMD e em Intel antiga)";
    }
    ERUPTION_LOG_WARN("[FLUXO] auditoria: passe '%s' anexo %ux%u %s: %s", pass, d.extent.width, d.extent.height,
                      formatLabel(f).c_str(), what.c_str());
}

} // namespace

void noteImage(VkImage image, const ImageDesc& d) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    auto it = s.images.find(image);
    if (it != s.images.end()) {
        addTexture(s.audit, it->second, false);
        addCensus(s, categoryOf(s, image, it->second), it->second, false);
    }
    s.tags.erase(image); // handle reaproveitado: a marca era da imagem antiga
    s.images[image] = d;
    s.audited.erase(image);
    addTexture(s.audit, d, true);
    addCensus(s, categorize(d), d, true);
}

void forgetImage(VkImage image) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    auto it = s.images.find(image);
    if (it == s.images.end()) return;
    addTexture(s.audit, it->second, false);
    addCensus(s, categoryOf(s, image, it->second), it->second, false);
    s.images.erase(it);
    s.tags.erase(image);
    s.audited.erase(image);
}

TextureCategory categorize(const ImageDesc& d) {
    const VkImageUsageFlags att = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    const VkImageUsageFlags need = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Profundidade copiada (destino de copia, sem ser anexo) nao e' conteudo.
    switch (d.format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return kTexNone;
    default: break;
    }
    return ((d.usage & need) == need && !(d.usage & att)) ? kTexContent : kTexNone;
}

void tagImage(VkImage image, TextureCategory c) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    const auto it = s.images.find(image);
    if (it == s.images.end()) return;
    addCensus(s, categoryOf(s, image, it->second), it->second, false);
    s.tags[image] = c;
    addCensus(s, c, it->second, true);
}

std::vector<CensusRow> textureCensus() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    std::vector<CensusRow> rows;
    for (const auto& [image, d] : s.images) {
        const TextureCategory c = categoryOf(s, image, d);
        if (c == kTexNone) continue;
        const VkFormat fmt = d.format; // C++17: lambda nao captura binding estruturado
        auto r = std::find_if(rows.begin(), rows.end(),
                              [c, fmt](const CensusRow& x) { return x.category == c && x.format == fmt; });
        if (r == rows.end()) {
            rows.push_back(CensusRow{c, d.format, 0, 0});
            r = rows.end() - 1;
        }
        ++r->count;
        r->bytes += imageBytes(d);
    }
    std::sort(rows.begin(), rows.end(), [](const CensusRow& a, const CensusRow& b) {
        return a.category != b.category ? a.category < b.category : a.bytes > b.bytes;
    });
    return rows;
}

CensusTotal censusTotal(TextureCategory c) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    return c < kTexCategories ? s.census[c] : CensusTotal{};
}

const char* categoryName(TextureCategory c) {
    switch (c) {
    case kTexContent: return "conteudo";
    case kTexLayers: return "camadas";
    case kTexSprite: return "sprite";
    default: return "-";
    }
}

bool findImage(VkImage image, ImageDesc& out) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    const auto it = s.images.find(image);
    if (it == s.images.end()) return false;
    out = it->second;
    return true;
}

void noteView(VkImageView view, VkImage image) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    s.views[view] = image;
}

void setPass(const char* name) { t_pass = name; }
const char* currentPass() { return t_pass; }

namespace {
thread_local const char* t_stack[8] = {};
thread_local int t_depth = 0;
} // namespace

void pushPass(const char* name) {
    if (t_depth < 8) t_stack[t_depth] = t_pass;
    ++t_depth;
    t_pass = name;
}

void popPass() {
    if (t_depth <= 0) return;
    --t_depth;
    if (t_depth < 8) t_pass = t_stack[t_depth];
}

void recordRendering(const VkRenderingInfo& info, VkFormat (*viewFormat)(VkImageView)) {
    const char* pass = t_pass ? t_pass : "outros";
    const uint32_t w = info.renderArea.extent.width, h = info.renderArea.extent.height;
    const uint32_t layers = std::max(1u, info.layerCount);
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
        const VkRenderingAttachmentInfo& a = info.pColorAttachments[i];
        if (a.imageView == VK_NULL_HANDLE) continue;
        const VkFormat f = viewFormat(a.imageView);
        accumulate(s, pass, attachmentTraffic(f, w, h, layers, a.loadOp, a.storeOp));
        auditAttachment(s, pass, a.imageView, f, a, true);
        if (s.detail) {
            const auto vi = s.views.find(a.imageView);
            s.curAtt.push_back({pass, vi == s.views.end() ? VK_NULL_HANDLE : vi->second, f, w, h, layers, a.loadOp,
                                a.storeOp, false});
        }
    }
    const VkRenderingAttachmentInfo* d = info.pDepthAttachment;
    if (!(d && d->imageView)) d = info.pStencilAttachment;
    if (d && d->imageView) {
        const VkFormat f = viewFormat(d->imageView);
        accumulate(s, pass, attachmentTraffic(f, w, h, layers, d->loadOp, d->storeOp));
        auditAttachment(s, pass, d->imageView, f, *d, false);
        if (s.detail) {
            const auto vi = s.views.find(d->imageView);
            s.curAtt.push_back({pass, vi == s.views.end() ? VK_NULL_HANDLE : vi->second, f, w, h, layers, d->loadOp,
                                d->storeOp, true});
        }
    }
    // Conta o passe uma vez (o nome ja' existe depois de accumulate).
    for (auto& e : s.cur)
        if (e.name == pass) ++e.renders;
}

void addVertexFetch(uint64_t bytes) {
    const char* pass = t_pass ? t_pass : "outros";
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    passStat(s, pass).vertexBytes += bytes;
    s.curVertex += bytes;
}

void beginFrame() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    s.last.frame = s.frame++;
    s.last.read = s.curRead;
    s.last.written = s.curWritten;
    s.last.vertexBytes = s.curVertex;
    s.curVertex = 0;
    s.last.passes.swap(s.cur);
    s.cur.clear();
    if (s.detail) {
        s.lastAtt.swap(s.curAtt);
        s.curAtt.clear();
    }
    s.curRead = s.curWritten = 0;
    s.last.dramValid = false;
#if ERUPTION_BW_IMC
    if (s.dram.ok) {
        std::vector<uint64_t> now;
        const auto t = std::chrono::steady_clock::now();
        if (readImc(s.dram, now)) {
            if (s.dram.havePrev) {
                double rd = 0.0, wr = 0.0;
                for (size_t i = 0; i < now.size(); ++i) {
                    const double b = static_cast<double>(now[i] - s.dram.prev[i]) * s.dram.bytesPerCount[i];
                    (s.dram.isWrite[i] ? wr : rd) += b;
                }
                s.last.dramValid = true;
                s.last.dramReadBytes = rd;
                s.last.dramWriteBytes = wr;
                s.last.dramSeconds = std::chrono::duration<double>(t - s.dram.prevT).count();
            }
            s.dram.prev = now;
            s.dram.prevT = t;
            s.dram.havePrev = true;
        }
    }
#endif
}

void setDeviceVendor(uint32_t vendorId) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    s.vendor = vendorId;
}

uint32_t deviceVendor() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    return s.vendor;
}

FrameStat lastFrame() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    return s.last;
}

AuditStat auditStats() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    return s.audit;
}

bool enableDramCounter() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    if (!s.dram.tried) {
        s.dram.tried = true;
#if ERUPTION_BW_IMC
        openImc(s.dram);
#else
        s.dram.status = "sem contador nesta plataforma";
#endif
        ERUPTION_LOG_INFO("[FLUXO] contador de DRAM: %s", s.dram.status.c_str());
    }
    return s.dram.ok;
}

const char* dramStatus() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    return s.dram.status.c_str();
}

void maybeReport(uint64_t frame) {
    // ERUPTION_TEST_FLUXO_REPORT=<quadro> (bancada, spec 50): tabela de bytes
    // por passe do quadro anterior + resumo da auditoria, uma vez.
    static const long kAt = [] {
        const char* e = std::getenv("ERUPTION_TEST_FLUXO_REPORT");
        return e ? std::strtol(e, nullptr, 10) : -1L;
    }();
    if (kAt < 0 || frame != static_cast<uint64_t>(kAt)) return;
    const FrameStat f = lastFrame();
    const AuditStat a = auditStats();
    constexpr double kMB = 1024.0 * 1024.0;
    ERUPTION_LOG_WARN("[FLUXO] quadro %llu: anexos leem %.1f MB e gravam %.1f MB (estimativa formato x area x load/store)",
                      static_cast<unsigned long long>(f.frame), f.read / kMB, f.written / kMB);
    for (const auto& p : f.passes) {
        const float ms = Profiler::getGpuTime(p.name);
        const double gbs = ms > 0.0f ? (p.read + p.written) / (ms * 1e6) : 0.0;
        ERUPTION_LOG_WARN("[FLUXO]   %-28s le %7.2f MB grava %7.2f MB  %2u render(s)  gpu %.3f ms  %.1f GB/s"
                          "  vertices %.2f MB",
                          p.name.c_str(), p.read / kMB, p.written / kMB, p.renders, ms, gbs, p.vertexBytes / kMB);
    }
    ERUPTION_LOG_WARN("[FLUXO] quadro %llu: busca de vertice %.2f MB (indices x passo, limite superior; so' passes "
                      "instrumentados)",
                      static_cast<unsigned long long>(f.frame), f.vertexBytes / kMB);
    if (f.dramValid && f.dramSeconds > 0.0)
        ERUPTION_LOG_WARN("[FLUXO] DRAM (controlador, sistema inteiro): %.2f GB/s leitura, %.2f GB/s escrita",
                          f.dramReadBytes / f.dramSeconds / 1e9, f.dramWriteBytes / f.dramSeconds / 1e9);
    // Sequencia de anexos do quadro (spec 52, auditoria de load/store): uma
    // linha por anexo, na ordem de abertura, com a imagem para seguir quem
    // le' o que o passe anterior gravou.
    std::vector<AttachRec> atts;
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.m);
        atts = s.lastAtt;
    }
    static const char* const kLoad[] = {"LOAD", "CLEAR", "DONT_CARE"};
    for (const AttachRec& r : atts) {
        const char* ld = r.load <= VK_ATTACHMENT_LOAD_OP_DONT_CARE ? kLoad[r.load] : "NONE";
        const char* st = r.store == VK_ATTACHMENT_STORE_OP_STORE       ? "STORE"
                         : r.store == VK_ATTACHMENT_STORE_OP_DONT_CARE ? "DONT_CARE"
                                                                       : "NONE";
        // Handle nao-despachavel: ponteiro em 64 bits, uint64_t em 32 (web).
        unsigned long long id = 0;
        std::memcpy(&id, &r.image, sizeof(r.image) < sizeof(id) ? sizeof(r.image) : sizeof(id));
        ERUPTION_LOG_WARN("[FLUXO]   anexo %-26s img %llx %-5s %4ux%-4u x%u %-10s %-9s %s", r.pass, id,
                          r.depth ? "depth" : "cor", r.w, r.h, r.layers, ld, st, formatLabel(r.format).c_str());
    }
    ERUPTION_LOG_WARN("[FLUXO] auditoria: %u alvos vistos, %u com flag que desliga compressao, %u avisos; texturas "
                      "%.1f MB em bloco (BC/ETC/ASTC), %.1f MB sem compressao",
                      a.targets, a.flagged, a.warnings, a.textureCompressedBytes / kMB, a.textureUncompressedBytes / kMB);
    // Censo (spec 51): por categoria, total em bloco/sem compressao e os formatos.
    const std::vector<CensusRow> rows = textureCensus();
    for (int c = kTexContent; c < kTexCategories; ++c) {
        const CensusTotal t = censusTotal(static_cast<TextureCategory>(c));
        if (t.count == 0) continue;
        std::string fmts;
        for (const CensusRow& r : rows) {
            if (r.category != c) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s x%u %.1f MB", fmts.empty() ? "" : ", ", formatLabel(r.format).c_str(),
                          r.count, r.bytes / kMB);
            fmts += buf;
        }
        ERUPTION_LOG_WARN("[TEXCOMP] censo %s: %u imagens, %.1f MB (%.1f em bloco, %.1f sem compressao): %s",
                          categoryName(static_cast<TextureCategory>(c)), t.count, (t.block + t.raw) / kMB, t.block / kMB,
                          t.raw / kMB, fmts.c_str());
    }
}

} // namespace eruption::bw
