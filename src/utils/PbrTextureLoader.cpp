#include "utils/PbrTextureLoader.hpp"
#include "utils/HeightFromAlbedo.hpp"
#include "ml/NeuralPbr.hpp"
#include "utils/BcCompressor.hpp"
#include <chrono>
#include <atomic>
#include <cmath>
#include "utils/ImageUtils.hpp"
#include "utils/Profiler.hpp"
#include "core/Logger.hpp"
#include "core/JobSystem.hpp"
#include "renderer/MipmapGenerator.hpp"
#include "renderer/SpritePickerUI.hpp" // for eucKrToUtf8

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <fstream>

namespace fs = std::filesystem;

namespace eruption {

static std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    if (size == 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static ImageData readAnyImage(const std::string& path) {
    std::vector<uint8_t> bytes = readFileBytes(path);
    if (bytes.empty()) return {};
    return ImageUtils::loadFromMemoryRaw(bytes.data(), bytes.size());
}

bool loadIndustryStandardPbr(const std::string& albedoPath, PbrTextureData& outMrahw, PbrTextureData& outNormal) {
    if (albedoPath.empty()) return false;
    std::string raw = albedoPath;
    std::replace(raw.begin(), raw.end(), '\\', '/');
    fs::path src = raw;
    std::string dir = src.parent_path().string();
    std::string stem = src.stem().string();

    std::string baseStem = stem;
    std::vector<std::string> suffixes = {"_diff", "_albedo", "_color", "_basecolor"};
    for (const auto& suf : suffixes) {
        std::string lowered = baseStem;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
        size_t pos = lowered.rfind(suf);
        if (pos != std::string::npos) {
            baseStem = baseStem.substr(0, pos);
            break;
        }
    }

    std::vector<std::string> exts = {".jpg", ".png", ".jpeg", ".tga"};
    auto findFile = [&](const std::string& keyword) -> std::string {
        for (const auto& ext : exts) {
            std::string cand1 = dir + "/" + baseStem + keyword + ext;
            if (fs::exists(cand1)) return cand1;
            std::string cand2 = dir + "/" + baseStem + keyword + "_4k" + ext;
            if (fs::exists(cand2)) return cand2;
            std::string cand3 = dir + "/" + baseStem + keyword + "_2k" + ext;
            if (fs::exists(cand3)) return cand3;
            std::string cand4 = dir + "/" + baseStem + keyword + "_1k" + ext;
            if (fs::exists(cand4)) return cand4;
        }
        return "";
    };

    std::string roughPath = findFile("_rough");
    if (roughPath.empty()) return false;

    std::string aoPath = findFile("_ao");
    std::string dispPath = findFile("_disp");
    std::string metalPath = findFile("_metal");
    if (metalPath.empty()) metalPath = findFile("_metallic");

    std::string normGlPath = findFile("_nor_gl");
    if (normGlPath.empty()) normGlPath = findFile("_normal");
    std::string normDxPath = findFile("_nor_dx");

    ImageData roughImg = readAnyImage(roughPath);
    if (!roughImg.isValid()) return false;

    ImageData aoImg = aoPath.empty() ? ImageData{} : readAnyImage(aoPath);
    ImageData dispImg = dispPath.empty() ? ImageData{} : readAnyImage(dispPath);
    ImageData metalImg = metalPath.empty() ? ImageData{} : readAnyImage(metalPath);

    int w = roughImg.width;
    int h = roughImg.height;

    outMrahw.width = w;
    outMrahw.height = h;
    outMrahw.channels = 4;
    outMrahw.pixels.assign(w * h * 4, 255);

    auto sampleGrey = [](const ImageData& img, int x, int y, uint8_t def) -> uint8_t {
        if (!img.isValid()) return def;
        float u = (float)x / img.width;
        float v = (float)y / img.height;
        int px = (int)(u * img.width);
        int py = (int)(v * img.height);
        px = std::clamp(px, 0, img.width - 1);
        py = std::clamp(py, 0, img.height - 1);
        int idx = (py * img.width + px) * img.channels;
        return img.pixels[idx];
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int outIdx = (y * w + x) * 4;
            outMrahw.pixels[outIdx + 0] = sampleGrey(metalImg, x, y, 0);
            outMrahw.pixels[outIdx + 1] = sampleGrey(roughImg, x, y, 255);
            // Blue = ambient occlusion. Always AO (255 = no occlusion when the
            // material has no _ao map), never height — the shaders read .b as AO
            // to darken crevices (the "deep" look). Height/disp goes unused for
            // now (no POM/displacement path).
            outMrahw.pixels[outIdx + 2] = sampleGrey(aoImg, x, y, 255);
            // Alpha = real displacement height when the set ships one (255 = no
            // height data; the shader falls back to a luminance proxy). Drives
            // the sun-facing contact micro-shadow march.
            outMrahw.pixels[outIdx + 3] = sampleGrey(dispImg, x, y, 255);
        }
    }

    std::string finalNormPath = !normGlPath.empty() ? normGlPath : normDxPath;
    if (!finalNormPath.empty()) {
        ImageData normImg = readAnyImage(finalNormPath);
        if (normImg.isValid()) {
            outNormal.width = normImg.width;
            outNormal.height = normImg.height;
            outNormal.channels = 4;
            outNormal.pixels.assign(normImg.width * normImg.height * 4, 255);
            for (int y = 0; y < normImg.height; ++y) {
                for (int x = 0; x < normImg.width; ++x) {
                    int srcIdx = (y * normImg.width + x) * normImg.channels;
                    int dstIdx = (y * normImg.width + x) * 4;
                    outNormal.pixels[dstIdx + 0] = normImg.pixels[srcIdx + 0];
                    outNormal.pixels[dstIdx + 1] = normImg.pixels[srcIdx + 1];
                    if (finalNormPath == normDxPath) {
                        outNormal.pixels[dstIdx + 1] = 255 - outNormal.pixels[dstIdx + 1];
                    }
                    if (normImg.channels > 2) outNormal.pixels[dstIdx + 2] = normImg.pixels[srcIdx + 2];
                }
            }
        }
    }

    ERUPTION_LOG_INFO("PbrTextureLoader: packed industry-standard PBR on the fly for %s", albedoPath.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Derive a normal + AO map from the albedo alone, for assets that ship no
// normal/ao/height (a lot of legacy textures, hand-imported meshes). Cheap: one
// downscale + a Sobel + a small box blur on luminance. Not photoreal, but it
// turns "flat albedo" into "reads as a brick" and it is stable under a moving
// light because it is authored data, not a runtime trick.
// ---------------------------------------------------------------------------
static std::string findAlbedoOnDisk(const std::string& name) {
    std::string n = name;
    std::replace(n.begin(), n.end(), '\\', '/');
    while (!n.empty() && n[0] == '/') n.erase(n.begin());
    // strip an existing extension so we can try several
    std::string stem = n;
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > stem.find_last_of('/') + 1) stem = stem.substr(0, dot);
    const char* roots[] = { "assets/data/texture/", "assets/pbr/base/", "assets/data/" };
    const char* exts[]  = { ".png", ".jpg", ".jpeg", ".bmp", ".tga" };
    for (const char* r : roots) {
        for (const char* e : exts) {
            std::string p = std::string(r) + stem + e;
            if (fs::exists(p)) return p;
        }
        // also the original name verbatim (keeps its own extension)
        std::string p = std::string(r) + n;
        if (fs::exists(p)) return p;
    }
    return "";
}

bool synthesizePbrFromAlbedo(const std::string& albedoDiskPath,
                             PbrTextureData& outMrahw, PbrTextureData& outNormal) {
    if (albedoDiskPath.empty()) return false;
    ImageData src = readAnyImage(albedoDiskPath);
    if (!src.isValid()) return false;
    return synthesizePbrFromPixels(src.pixels, src.width, src.height, src.channels,
                                   outMrahw, outNormal);
}

static uint8_t toUnorm8(float v) {
    return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
}

// Altura da rede (media zero, unidade dela) -> [0,1] centrado em 0.5, com o
// percentil 98 de |h| em 0.45: a mesma faixa util da sintese classica, que
// e' a que os controles de displacement esperam.
static std::vector<float> neuralHeightToUnit(const std::vector<float>& h) {
    if (h.empty()) return {};
    std::vector<float> mag(h.size());
    for (size_t i = 0; i < h.size(); ++i) mag[i] = std::fabs(h[i]);
    const size_t k = h.size() * 98 / 100;
    std::nth_element(mag.begin(), mag.begin() + static_cast<std::ptrdiff_t>(k), mag.end());
    const float scale = 0.45f / std::max(mag[k], 1e-6f);
    std::vector<float> out(h.size());
    for (size_t i = 0; i < h.size(); ++i) out[i] = 0.5f + h[i] * scale;
    return out;
}

// MRAH-W + normal a partir da saida da rede. Exige normal e altura (o que o
// relevo usa); metal/rugosidade/AO que a rede nao der ficam no padrao.
static bool fillPbrFromNeural(const NeuralPbrMaps& nn, PbrTextureData& outMrahw, PbrTextureData& outNormal) {
    const size_t np = static_cast<size_t>(nn.width) * nn.height;
    if (nn.normal.size() != np * 3 || nn.heightMap.size() != np) return false;
    const std::vector<float> height = neuralHeightToUnit(nn.heightMap);
    const bool hasMetal = nn.metallic.size() == np, hasRough = nn.roughness.size() == np, hasAo = nn.ao.size() == np;
    outNormal.width = outMrahw.width = nn.width;
    outNormal.height = outMrahw.height = nn.height;
    outNormal.channels = outMrahw.channels = 4;
    outNormal.pixels.resize(np * 4);
    outMrahw.pixels.resize(np * 4);
    for (size_t i = 0; i < np; ++i) {
        // A rede entrega +Y pra cima (OpenGL), a convencao que o motor le'
        // (_nor_dx e' convertido pra ela no carregamento).
        for (int c = 0; c < 3; ++c) outNormal.pixels[i * 4 + c] = toUnorm8(nn.normal[i * 3 + c] * 0.5f + 0.5f);
        outNormal.pixels[i * 4 + 3] = 1; // materialProps (none)
        outMrahw.pixels[i * 4 + 0] = hasMetal ? toUnorm8(nn.metallic[i]) : 0;
        outMrahw.pixels[i * 4 + 1] = hasRough ? toUnorm8(nn.roughness[i]) : 204; // ~0.8
        outMrahw.pixels[i * 4 + 2] = hasAo ? toUnorm8(nn.ao[i]) : 255;
        // Altura centrada em 0.5. 250 e' o teto (255 = flag "sem altura").
        outMrahw.pixels[i * 4 + 3] = static_cast<uint8_t>(std::clamp(height[i] * 250.0f, 0.0f, 250.0f));
    }
    return true;
}

bool synthesizePbrFromPixels(const std::vector<uint8_t>& pixels, int width, int height,
                             int channels,
                             PbrTextureData& outMrahw, PbrTextureData& outNormal,
                             bool organicHint) {
    if (pixels.empty() || width < 4 || height < 4 || channels < 3) return false;
    ImageData src;
    src.pixels = pixels;
    src.width = width;
    src.height = height;
    src.channels = channels;
    // Work at a modest resolution: derived detail does not need 4k and this
    // keeps the whole synth well under a millisecond.
    // 1024 (era 512): de perto, detalhe derivado em 512 aparece ampliado e
    // vira ruído. O .etex comprime isso depois, então o custo é de load.
    ImageUtils::downscaleRGBA(src.width, src.height, src.pixels, 512);
    const int w = src.width, h = src.height, ch = src.channels;
    if (w < 4 || h < 4) return false;

    // RGBA8 contiguo pro modulo (a fonte pode vir com 3 canais).
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 255);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = src.pixels[i * ch + c];

    // Rede neural configurada localmente (NeuralPbr.hpp), quando existe.
    NeuralPbrMaps nn;
    if (neuralPbrInfer(rgba.data(), w, h, nn) && fillPbrFromNeural(nn, outMrahw, outNormal)) return true;

    // Altura, normal e cavidade vem do MESMO lugar que o cooker offline
    // (HeightFromAlbedo.hpp) - antes cada um tinha a sua conta e nenhuma
    // separava rejunte de tinta.
    HeightFromAlbedoParams hp;
    hp.forceOrganic = organicHint;
    const HeightFromAlbedoResult hfa = heightFromAlbedo(rgba.data(), w, h, hp);
    if (hfa.heightMap.empty()) return false;

    const size_t np = static_cast<size_t>(w) * h;
    outNormal.width = w; outNormal.height = h; outNormal.channels = 4;
    outNormal.pixels = hfa.normalRgba;
    outMrahw.width = w; outMrahw.height = h; outMrahw.channels = 4;
    outMrahw.pixels.assign(np * 4, 255);
    for (size_t i = 0; i < np; ++i) {
        outNormal.pixels[i * 4 + 3] = 1; // materialProps (none)
        outMrahw.pixels[i * 4 + 0] = 0;   // metallic
        outMrahw.pixels[i * 4 + 1] = 204; // roughness ~0.8
        outMrahw.pixels[i * 4 + 2] = toUnorm8(hfa.cavity[i]);
        // Altura centrada em 0.5. 250 e' o teto (255 = flag "sem altura").
        outMrahw.pixels[i * 4 + 3] = static_cast<uint8_t>(std::clamp(hfa.heightMap[i] * 250.0f, 0.0f, 250.0f));
    }
    return true;
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool supportedExt(const fs::path& ext) {
    const std::string e = toLower(ext.string());
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga";
}

// Normalise a stem for fuzzy matching.  The GLB converter replaces non-ASCII
// characters and pads fields with underscores, so a stem with non-ASCII
// characters and its padded twin "#_____________abc_______06_0001" should both
// match the cooked file.
//
// IMPORTANT: non-ASCII bytes (UTF-8 names) must be preserved.  We only
// strip ASCII clutter (# _ - space) and lower-case ASCII letters so that
// "name_0001" and "name" normalise to the same key.
static std::string normalizePbrStem(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 0x80) {
            // UTF-8 continuation byte: preserve as-is.
            out.push_back(static_cast<char>(c));
        } else if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        // Drop ASCII clutter: # _ - space etc.
    }
    return out;
}

// Fallback lookup: when the albedo directory does not match the cooked PBR
// layout (e.g. cidade-A/cidade-C/albedo references only a filename, or uses a
// different folder prefix), scan assets/data/texture once and cache a map from
// cooked-map stem to the discovered mrahw/normal pair.
static const std::pair<std::string, std::string>* findPbrPairByStem(const std::string& stem) {
    struct Cache {
        std::unordered_map<std::string, std::pair<std::string, std::string>> exact;
        std::unordered_map<std::string, std::pair<std::string, std::string>> normalized;
    };
    static Cache cache;
    static std::once_flag once;
    std::call_once(once, [&]() {
        const fs::path root = "assets/data/texture";
        if (!fs::exists(root) || !fs::is_directory(root)) return;
        try {
            // Collect + SORT before populating: first-wins on ambiguous
            // (normalized) stems must be deterministic. The raw directory
            // iteration order depends on the filesystem and CHANGES when
            // unrelated files appear (e.g. baked .etex siblings) - that
            // silently swapped which cooked pair ambiguous stems resolved to
            // (two stems differing in one non-ASCII character on parana_field).
            std::vector<fs::path> mrahwFiles;
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;
                const std::string name = entry.path().filename().string();
                constexpr const char* mrahwSuffix = "_mrahw.png";
                constexpr size_t mrahwLen = 10; // strlen("_mrahw.png")
                if (name.size() > mrahwLen &&
                    std::memcmp(name.data() + name.size() - mrahwLen, mrahwSuffix, mrahwLen) == 0) {
                    mrahwFiles.push_back(entry.path());
                }
            }
            std::sort(mrahwFiles.begin(), mrahwFiles.end());
            for (const auto& path : mrahwFiles) {
                const std::string name = path.filename().string();
                std::string originalStem = name.substr(0, name.size() - 10);
                std::string normalPath = (path.parent_path() / (originalStem + "_normal.png")).string();
                if (!fs::exists(normalPath)) continue;
                std::pair<std::string, std::string> value = { path.string(), normalPath };

                std::string exactKey = toLower(originalStem);
                if (cache.exact.find(exactKey) == cache.exact.end()) {
                    cache.exact[exactKey] = value;
                }

                std::string normKey = normalizePbrStem(originalStem);
                if (!normKey.empty() && cache.normalized.find(normKey) == cache.normalized.end()) {
                    cache.normalized[normKey] = value;
                }
            }
        } catch (const std::exception& e) {
            ERUPTION_LOG_WARN("PbrTextureLoader: failed to scan assets/data/texture: %s", e.what());
        }
    });

    auto it = cache.exact.find(toLower(stem));
    if (it != cache.exact.end()) return &it->second;

    std::string normStem = normalizePbrStem(stem);
    if (!normStem.empty()) {
        auto itn = cache.normalized.find(normStem);
        if (itn != cache.normalized.end()) return &itn->second;
    }
    return nullptr;
}

struct PendingPbrUpload {
    VkImage image;
    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    // Non-empty = baked compressed mips in the staging buffer (one copy per
    // mip, no blit chain). Empty = legacy RGBA8 (single copy + blit).
    std::vector<uint32_t> mipSizes;
};

// Records the GPU side of one queued upload into any command buffer.
static void recordOnePbrUpload(VulkanContext* ctx, VkCommandBuffer cmd,
                               const PendingPbrUpload& pending) {
    if (pending.image == VK_NULL_HANDLE || pending.stagingBuffer == VK_NULL_HANDLE ||
        pending.width == 0 || pending.height == 0)
        return;
    ctx->cmdImageBarrier(cmd, pending.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!pending.mipSizes.empty()) {
        std::vector<VkBufferImageCopy> regions(pending.mipSizes.size());
        VkDeviceSize offset = 0;
        for (uint32_t i = 0; i < pending.mipSizes.size(); ++i) {
            VkBufferImageCopy& r = regions[i];
            r = {};
            r.bufferOffset = offset;
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.mipLevel = i;
            r.imageSubresource.layerCount = 1;
            r.imageExtent = { std::max(1u, pending.width >> i),
                              std::max(1u, pending.height >> i), 1 };
            offset += pending.mipSizes[i];
        }
        vkCmdCopyBufferToImage(cmd, pending.stagingBuffer, pending.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        ctx->cmdImageBarrier(cmd, pending.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        return;
    }
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { pending.width, pending.height, 1 };
    vkCmdCopyBufferToImage(cmd, pending.stagingBuffer, pending.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (pending.mipLevels > 1) {
        // generateMipmapsBlit leaves the image in SHADER_READ_ONLY_OPTIMAL.
        MipmapGenerator::generateMipmapsBlit(cmd, pending.image,
            pending.width, pending.height, pending.mipLevels);
    } else {
        ctx->cmdImageBarrier(cmd, pending.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
}

static std::vector<PendingPbrUpload> g_pendingPbrUploads;
static constexpr size_t PBR_UPLOAD_BATCH_SIZE = 64;

void flushPbrTextureUploads(VulkanContext* ctx) {
    if (!ctx || g_pendingPbrUploads.empty()) return;

    ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        for (const auto& pending : g_pendingPbrUploads) {
            recordOnePbrUpload(ctx, cmd, pending);
        }
    });

    for (const auto& pending : g_pendingPbrUploads) {
        if (pending.stagingBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(ctx->allocator(), pending.stagingBuffer, pending.stagingAlloc);
        }
    }
    g_pendingPbrUploads.clear();
}

size_t pendingPbrUploadCount() {
    return g_pendingPbrUploads.size();
}

void recordPendingPbrUploads(VulkanContext* ctx, VkCommandBuffer cmd, size_t maxItems,
                             std::vector<std::pair<VkBuffer, VmaAllocation>>& stagingOut) {
    if (!ctx || cmd == VK_NULL_HANDLE || g_pendingPbrUploads.empty() || maxItems == 0)
        return;

    const size_t count = std::min(maxItems, g_pendingPbrUploads.size());
    for (size_t i = 0; i < count; ++i) {
        recordOnePbrUpload(ctx, cmd, g_pendingPbrUploads[i]);
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& pending = g_pendingPbrUploads[i];
        if (pending.stagingBuffer != VK_NULL_HANDLE) {
            stagingOut.emplace_back(pending.stagingBuffer, pending.stagingAlloc);
        }
    }
    g_pendingPbrUploads.erase(g_pendingPbrUploads.begin(),
                              g_pendingPbrUploads.begin() + count);
}

void cancelPendingPbrUploadsForImage(VulkanContext* ctx, VkImage image) {
    if (image == VK_NULL_HANDLE) return;
    for (auto it = g_pendingPbrUploads.begin(); it != g_pendingPbrUploads.end();) {
        if (it->image == image) {
            if (ctx && it->stagingBuffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(ctx->allocator(), it->stagingBuffer, it->stagingAlloc);
            }
            it = g_pendingPbrUploads.erase(it);
        } else {
            ++it;
        }
    }
}

static PbrTextureSlot uploadPbrTextureSlot(VulkanContext* ctx, BindlessDescriptor* bindless,
                                           VkSampler sampler, const PbrTextureData& tex) {
    PbrTextureSlot out;
    if (!tex.valid() || !ctx || !bindless) return out;

    const bool baked = tex.etex.valid();
    const uint32_t mipLevels = baked
        ? tex.etex.mipCount
        : MipmapGenerator::calculateMipLevels(
              static_cast<uint32_t>(tex.width), static_cast<uint32_t>(tex.height));
    const VkFormat format = baked ? static_cast<VkFormat>(tex.etex.vkFormat)
                                  : VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { static_cast<uint32_t>(tex.width), static_cast<uint32_t>(tex.height), 1 };
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!baked) imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // blit chain
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(ctx->allocator(), &imageInfo, &allocInfo, &out.image, &out.alloc, nullptr) != VK_SUCCESS) {
        return out;
    }

    const uint8_t* srcData = baked ? tex.etex.payload.data() : tex.pixels.data();
    const size_t srcSize = baked ? tex.etex.payload.size() : tex.pixels.size();
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!ctx->createBuffer(srcSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer, stagingAlloc)) {
        vmaDestroyImage(ctx->allocator(), out.image, out.alloc);
        out.image = VK_NULL_HANDLE;
        out.alloc = VK_NULL_HANDLE;
        return out;
    }

    void* mapped = nullptr;
    vmaMapMemory(ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, srcData, srcSize);
    vmaUnmapMemory(ctx->allocator(), stagingAlloc);

    // Queue the GPU upload instead of submitting immediately. This batches
    // hundreds of PBR map uploads into one command buffer submit at map-load
    // time, cutting loading stalls dramatically.
    PendingPbrUpload pending{ out.image, stagingBuffer, stagingAlloc,
                              static_cast<uint32_t>(tex.width),
                              static_cast<uint32_t>(tex.height),
                              mipLevels, {} };
    if (baked) pending.mipSizes = tex.etex.mipSizes;
    g_pendingPbrUploads.push_back(std::move(pending));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(ctx->device(), &viewInfo, nullptr, &out.view) != VK_SUCCESS) {
        vmaDestroyImage(ctx->allocator(), out.image, out.alloc);
        out.image = VK_NULL_HANDLE;
        out.alloc = VK_NULL_HANDLE;
        return out;
    }

    out.index = bindless->allocateSlot();
    if (out.index == 0) {
        vkDestroyImageView(ctx->device(), out.view, nullptr);
        vmaDestroyImage(ctx->allocator(), out.image, out.alloc);
        out.view = VK_NULL_HANDLE;
        out.image = VK_NULL_HANDLE;
        out.alloc = VK_NULL_HANDLE;
        return out;
    }

    bindless->updateTexture(out.index, out.view, sampler);
    return out;
}

PbrTextureData loadPbrTextureData(const std::string& path) {
    static const bool kTrace = std::getenv("ERUPTION_TEST_PBR_TRACE") != nullptr;
    PbrTextureData out;
    // Baked BC texture beside the PNG: skip the decode entirely.
    out.etex = loadEtexForImage(path);
    if (out.etex.valid()) {
        out.width = static_cast<int>(out.etex.width);
        out.height = static_cast<int>(out.etex.height);
        out.channels = 4;
        if (kTrace) ERUPTION_LOG_WARN("[PBRTRACE] etex %ux%u fmt=%u mips=%u %s",
                                      out.etex.width, out.etex.height, out.etex.vkFormat,
                                      out.etex.mipCount, path.c_str());
        return out;
    }
    ImageData img = ImageUtils::loadPNG(path);
    if (!img.isValid()) {
        if (kTrace) ERUPTION_LOG_WARN("[PBRTRACE] FAIL %s", path.c_str());
        return out;
    }
    // Sem .etex cozido ao lado: comprime AQUI, em runtime, e grava o .etex para
    // o proximo load ja' pegar pronto. Antes deste ponto toda textura de disco
    // sem bake subia RGBA8 - era a maior lacuna de VRAM que restava, e VRAM e'
    // a restricao que decide a GeForce 930M (2 GB).
    //
    // Compatibilidade: o PNG continua sendo a FONTE. O .etex e' artefato
    // derivado e regeneravel, invalidado por mtime pelo proprio loadEtexForImage,
    // e a ausencia dele nunca quebra nada - so' custa mais VRAM.
    // ERUPTION_NO_TEXTURE_BC=1 desliga (mesmo interruptor do caminho embutido).
    static const bool kNoBc = std::getenv("ERUPTION_NO_TEXTURE_BC") != nullptr;
    if (!kNoBc && bcFormatSupport().any() && img.width >= 4 && img.height >= 4) {
        // BC5 em normal map NAO serve nesta engine: a amostra volta B=0/A=1 e o
        // shader faz xyz*2-1, dando z=-1 (medido: RMS 9,7 contra 0,77 do BC7).
        // Normal e mrahw vao os dois para o caminho BC7.
        BcKind kind = BcKind::Albedo;
        if (path.find("_normal") != std::string::npos ||
            path.find("_mrahw") != std::string::npos) kind = BcKind::Data;

        bool hasAlpha = false;
        for (size_t i = 3; i < img.pixels.size(); i += 4) {
            if (img.pixels[i] != 255) { hasAlpha = true; break; }
        }
        EtexData e = compressRgbaToEtex(img.pixels.data(), img.width, img.height, kind, hasAlpha);
        if (e.valid()) {
            // Grava o cache ao lado da fonte, em .tmp + rename para nunca
            // deixar arquivo pela metade se o processo morrer no meio.
            std::vector<uint8_t> bytes;
            appendEtexBytes(e, bytes);
            const std::string etexOut = path + ".etex";
            const std::string tmp = etexOut + ".tmp";
            std::error_code wec;
            {
                std::ofstream f(tmp, std::ios::binary);
                if (f) f.write(reinterpret_cast<const char*>(bytes.data()),
                               static_cast<std::streamsize>(bytes.size()));
            }
            std::filesystem::rename(tmp, etexOut, wec);
            if (wec) std::filesystem::remove(tmp, wec);

            out.etex = std::move(e);
            out.width = img.width;
            out.height = img.height;
            out.channels = 4;
            if (kTrace) ERUPTION_LOG_WARN("[PBRTRACE] bc-runtime %dx%d %s", out.width, out.height, path.c_str());
            return out;
        }
    }

    out.pixels = std::move(img.pixels);
    out.width = img.width;
    out.height = img.height;
    out.channels = img.channels;
    if (kTrace) ERUPTION_LOG_WARN("[PBRTRACE] png %dx%d %s", out.width, out.height, path.c_str());
    return out;
}

PbrTexturePathPair resolvePbrTexturePathsImpl(const std::string& albedoPath);

PbrTexturePathPair resolvePbrTexturePaths(const std::string& albedoPath) {
    PbrTexturePathPair r = resolvePbrTexturePathsImpl(albedoPath);
    static const bool kTrace = std::getenv("ERUPTION_TEST_PBR_TRACE") != nullptr;
    if (kTrace) {
        ERUPTION_LOG_WARN("[PBRRESOLVE] in='%s' -> valid=%d mrahw='%s'",
                          albedoPath.c_str(), r.valid ? 1 : 0, r.mrahw.c_str());
    }
    return r;
}

PbrTexturePathPair resolvePbrTexturePathsImpl(const std::string& albedoPath) {
    // Debug A/B: ignore pre-cooked PBR maps so the engine exercises the
    // source fallback path even when maps exist on disk.
    static const bool kDisablePbrMaps = std::getenv("ERUPTION_TEST_NO_PBR_MAPS") != nullptr;
    if (kDisablePbrMaps) return PbrTexturePathPair{};

    // Fast path: identical albedo paths are resolved repeatedly during map
    // loads (one per model/terrain texture).  Cache the result to avoid
    // thousands of redundant fs::exists calls.
    static std::unordered_map<std::string, PbrTexturePathPair> cache;
    static std::mutex cacheMutex;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(albedoPath);
        if (it != cache.end()) return it->second;
    }

    auto resolveInternal = [](const std::string& albedoPath) -> PbrTexturePathPair {
        PbrTexturePathPair out;
        if (albedoPath.empty()) return out;

        // Sanitize to a filesystem path and strip leading separators.
        std::string raw = albedoPath;
        if (!raw.empty() && (raw[0] == '\\' || raw[0] == '/')) raw = raw.substr(1);

        // Terrain/model texture paths often carry a source "data\texture\" or
        // "data/texture/" prefix. Cooked PBR maps live under "assets/data/texture/",
        // so strip that prefix before resolving the directory stem.
        {
            std::string lowered = toLower(raw);
            static const std::vector<std::string> prefixes = {
                "data\\texture\\", "data/texture/",
                "\\texture\\", "/texture/",
                "texture\\", "texture/"
            };
            for (const auto& p : prefixes) {
                if (lowered.size() >= p.size() && std::memcmp(lowered.data(), p.data(), p.size()) == 0) {
                    raw = raw.substr(p.size());
                    break;
                }
            }
        }

        std::replace(raw.begin(), raw.end(), '\\', '/');
        fs::path src = raw;

        if (!supportedExt(src.extension())) return out;

        fs::path relDir = src.parent_path();
        std::string stem = src.stem().string();

        // Cooked maps are stored under assets/data/texture mirroring the
        // original texture layout. Some albedo paths already include the
        // 'texture/' prefix, others (e.g. terrain references) are relative to
        // data\texture and omit it.
        auto makePaths = [&](const fs::path& baseDir, const std::string& dirOverride,
                             const std::string& stemOverride) {
            fs::path base = baseDir / fs::path(dirOverride);
            return std::make_pair(base / (stemOverride + "_mrahw.png"),
                                  base / (stemOverride + "_normal.png"));
        };

        // GLB-baked albedos are often renamed to "#__________<original>_<imageIdx>.png"
        // by the converter so embedded images get unique filenames.  Build a list
        // of candidate base stems so we can still find the cooked _mrahw/_normal.
        auto collectCandidateStems = [](const std::string& s) -> std::vector<std::string> {
            std::vector<std::string> out;
            out.push_back(s);

            // Strip a trailing "_<digits>" image-index suffix.
            std::string noIdx = s;
            size_t lastUnderscore = noIdx.rfind('_');
            if (lastUnderscore != std::string::npos && lastUnderscore + 1 < noIdx.size()) {
                bool allDigits = true;
                for (size_t i = lastUnderscore + 1; i < noIdx.size(); ++i) {
                    if (!std::isdigit(static_cast<unsigned char>(noIdx[i]))) { allDigits = false; break; }
                }
                if (allDigits) {
                    noIdx = noIdx.substr(0, lastUnderscore);
                    out.push_back(noIdx);
                }
            }

            // Strip the "#_____________" prefix that the GLB converter prepends.
            // The converter pads with varying numbers of underscores, so drop
            // every leading '#' and '_' character.
            size_t firstReal = 0;
            while (firstReal < s.size() && (s[firstReal] == '#' || s[firstReal] == '_')) ++firstReal;
            if (firstReal > 0 && firstReal < s.size()) {
                std::string candidate = s.substr(firstReal);
                if (!candidate.empty()) {
                    out.push_back(candidate);
                    // Also try the prefix-free version without the image-index suffix.
                    size_t candLastUnder = candidate.rfind('_');
                    if (candLastUnder != std::string::npos && candLastUnder + 1 < candidate.size()) {
                        bool allDigits = true;
                        for (size_t i = candLastUnder + 1; i < candidate.size(); ++i) {
                            if (!std::isdigit(static_cast<unsigned char>(candidate[i]))) { allDigits = false; break; }
                        }
                        if (allDigits) {
                            out.push_back(candidate.substr(0, candLastUnder));
                        }
                    }
                }
            }

            // Deduplicate while preserving order.
            std::vector<std::string> uniq;
            for (const auto& c : out) {
                if (std::find(uniq.begin(), uniq.end(), c) == uniq.end()) uniq.push_back(c);
            }
            return uniq;
        };

        std::vector<std::string> candidateStems = collectCandidateStems(stem);

        // Some source texture names are stored in EUC-KR bytes. If the raw
        // UTF-8 path does not exist on disk, try converting from EUC-KR so
        // legacy folder names match the cooked PBR maps.
        std::vector<std::pair<std::string, std::string>> pathAttempts;
        for (const auto& candStem : candidateStems) {
            pathAttempts.emplace_back(relDir.string(), candStem);
        }
        std::string utf8Dir = eucKrToUtf8(relDir.string());
        for (const auto& candStem : candidateStems) {
            std::string utf8Stem = eucKrToUtf8(candStem);
            if (utf8Dir != relDir.string() || utf8Stem != candStem) {
                pathAttempts.emplace_back(utf8Dir, utf8Stem);
            }
        }

        fs::path mrahwPath, normalPath;
        for (const auto& attempt : pathAttempts) {
            std::tie(mrahwPath, normalPath) = makePaths("assets/data/texture", attempt.first, attempt.second);
            if (fs::exists(mrahwPath) && fs::exists(normalPath)) {
                out.mrahw = mrahwPath.string();
                out.normal = normalPath.string();
                out.valid = true;
                return out;
            }
            std::tie(mrahwPath, normalPath) = makePaths("assets/data", attempt.first, attempt.second);
            if (fs::exists(mrahwPath) && fs::exists(normalPath)) {
                out.mrahw = mrahwPath.string();
                out.normal = normalPath.string();
                out.valid = true;
                return out;
            }
        }

        // Final fallback: search the whole cooked texture tree by stem. This
        // covers cidade-A/cidade-C/cidade-B assets whose albedo path does not
        // include the same directory prefix as the cooked PBR maps.
        for (const auto& stemAttempt : candidateStems) {
            if (stemAttempt.empty()) continue;
            auto pair = findPbrPairByStem(stemAttempt);
            if (pair) {
                out.mrahw = pair->first;
                out.normal = pair->second;
                out.valid = true;
                ERUPTION_LOG_INFO("PbrTextureLoader: resolved PBR maps for %s via tree scan: %s / %s",
                                  albedoPath.c_str(), out.mrahw.c_str(), out.normal.c_str());
                return out;
            }
            std::string eucStem = eucKrToUtf8(stemAttempt);
            if (eucStem != stemAttempt) {
                pair = findPbrPairByStem(eucStem);
                if (pair) {
                    out.mrahw = pair->first;
                    out.normal = pair->second;
                    out.valid = true;
                    ERUPTION_LOG_INFO("PbrTextureLoader: resolved PBR maps for %s via tree scan (EUC): %s / %s",
                                      albedoPath.c_str(), out.mrahw.c_str(), out.normal.c_str());
                    return out;
                }
            }
        }

        return out;
    };

    PbrTexturePathPair result = resolveInternal(albedoPath);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[albedoPath] = result;
    }
    return result;
}

PbrTextureSlots uploadPbrTexturesFromData(VulkanContext* ctx,
                                          BindlessDescriptor* bindless,
                                          VkSampler sampler,
                                          const PbrTextureData& mrahw,
                                          const PbrTextureData& normal) {
    PbrTextureSlots slots;
    if (!ctx || !bindless) return slots;

    slots.mrahw = uploadPbrTextureSlot(ctx, bindless, sampler, mrahw);
    slots.normal = uploadPbrTextureSlot(ctx, bindless, sampler, normal);

    if (!slots.valid()) {
        // Partial failure: release anything that was allocated so the shader
        // falls back to constants.
        slots.reset(ctx, bindless);
    }

    return slots;
}

// ---------------------------------------------------------------------------
// Parallel pre-decode of cooked PBR maps. The mesh loop resolves PBR textures
// one node at a time; decoding the (large) _mrahw/_normal PNGs there is serial
// and was the bulk of map-load time. prewarmPbrTextureData() decodes them all
// up front on the JobSystem so loadPbrTexturesForAlbedo() only uploads.
// ---------------------------------------------------------------------------
namespace {
struct PrewarmEntry { PbrTextureData mrahw; PbrTextureData normal; };
std::mutex g_prewarmMutex;
std::unordered_map<std::string, PrewarmEntry> g_prewarmCache;

std::string sanitizePbrKey(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    if (!s.empty() && s[0] == '\\') s = s.substr(1);
    return s;
}
} // namespace

static EmbeddedAlbedoProvider g_embeddedAlbedoProvider;
static BakedPbrProvider g_bakedPbrProvider;
static bool g_pbrSynthesisEnabled = true;

void setPbrSynthesisEnabled(bool enabled) { g_pbrSynthesisEnabled = enabled; }
bool pbrSynthesisEnabled() { return g_pbrSynthesisEnabled; }

void setEmbeddedAlbedoProvider(EmbeddedAlbedoProvider provider) {
    g_embeddedAlbedoProvider = std::move(provider);
}

void setBakedPbrProvider(BakedPbrProvider provider) {
    g_bakedPbrProvider = std::move(provider);
}

bool synthesizePbrForAlbedoKey(const std::string& albedoKey,
                               PbrTextureData& outMrahw, PbrTextureData& outNormal) {
    if (albedoKey.empty() || !g_pbrSynthesisEnabled) return false;
    // Par já sintetizado E comprimido em bloco pelo bake do GLB. Mesma posição
    // na cadeia que a síntese em runtime substituiria, só que pronto.
    if (g_bakedPbrProvider && g_bakedPbrProvider(albedoKey, outMrahw, outNormal)) return true;
    std::string alb = findAlbedoOnDisk(albedoKey);
    if (!alb.empty() && synthesizePbrFromAlbedo(alb, outMrahw, outNormal)) return true;
    // Not on disk: the albedo may live inside a GLB (embedded texture).
    if (g_embeddedAlbedoProvider) {
        std::vector<uint8_t> px;
        int w = 0, h = 0, ch = 0;
        if (g_embeddedAlbedoProvider(albedoKey, px, w, h, ch)) {
            return synthesizePbrFromPixels(px, w, h, ch, outMrahw, outNormal);
        }
    }
    return false;
}

void prewarmPbrTextureData(const std::vector<std::string>& albedoNames) {
    std::vector<std::string> uniq;
    {
        std::unordered_set<std::string> seen;
        uniq.reserve(albedoNames.size());
        for (const auto& raw : albedoNames) {
            if (raw.empty()) continue;
            std::string k = sanitizePbrKey(raw);
            if (k.empty()) continue;
            {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                if (g_prewarmCache.count(k)) continue;
            }
            if (seen.insert(k).second) uniq.push_back(std::move(k));
        }
    }
    if (uniq.empty()) return;

    // Warm the directory-scan cache once on this thread so the parallel jobs
    // don't all pile up on the std::call_once directory walk.
    (void)resolvePbrTexturePaths(uniq.front());

    JobSystem::instance().parallelFor(static_cast<uint32_t>(uniq.size()), [&](uint32_t i) {
        const std::string& key = uniq[i];
        PbrTexturePathPair paths = resolvePbrTexturePaths(key);
        PrewarmEntry e;
        if (paths.valid && !paths.mrahw.empty() && !paths.normal.empty()) {
            e.mrahw  = loadPbrTextureData(paths.mrahw);
            e.normal = loadPbrTextureData(paths.normal);
        } else {
            // No cooked maps: derive normal + AO from the albedo.
            std::string alb = findAlbedoOnDisk(key);
            if (alb.empty() || !synthesizePbrFromAlbedo(alb, e.mrahw, e.normal)) return;
        }
        if (!e.mrahw.valid() || !e.normal.valid()) return;
        std::lock_guard<std::mutex> lk(g_prewarmMutex);
        g_prewarmCache.emplace(key, std::move(e));
    });
}

void clearPbrTextureDataPrewarm() {
    std::lock_guard<std::mutex> lk(g_prewarmMutex);
    g_prewarmCache.clear();
}

PbrTextureSlots loadPbrTexturesForAlbedo(VulkanContext* ctx,
                                         BindlessDescriptor* bindless,
                                         VkSampler sampler,
                                         const std::string& albedoPath) {
    PbrTextureSlots slots;
    if (!ctx || !bindless || albedoPath.empty()) return slots;

    // Fast path: use a pre-decoded map from prewarmPbrTextureData() if present.
    {
        std::string key = sanitizePbrKey(albedoPath);
        std::unique_lock<std::mutex> lk(g_prewarmMutex);
        auto it = g_prewarmCache.find(key);
        if (it != g_prewarmCache.end()) {
            PrewarmEntry e = std::move(it->second);
            g_prewarmCache.erase(it);
            lk.unlock();
            return uploadPbrTexturesFromData(ctx, bindless, sampler, e.mrahw, e.normal);
        }
    }

    PbrTexturePathPair paths = resolvePbrTexturePaths(albedoPath);
    PbrTextureData mrahw;
    PbrTextureData normal;
    if (loadIndustryStandardPbr(albedoPath, mrahw, normal)) {
        return uploadPbrTexturesFromData(ctx, bindless, sampler, mrahw, normal);
    }
    if (!paths.valid) {
        // No cooked maps and no industry-standard source set: derive normal + AO
        // from the albedo so the surface still has relief. Covers both textures
        // on disk and GLB-embedded ones (via the engine's provider).
        if (synthesizePbrForAlbedoKey(albedoPath, mrahw, normal)) {
            return uploadPbrTexturesFromData(ctx, bindless, sampler, mrahw, normal);
        }
        return slots;
    }

    mrahw = loadPbrTextureData(paths.mrahw);
    normal = loadPbrTextureData(paths.normal);
    return uploadPbrTexturesFromData(ctx, bindless, sampler, mrahw, normal);
}

} // namespace eruption
