// Sprites do Engine: decodificacao de .spr/.png para textura bindless,
// recorte do conteudo real do quadro, limpeza das texturas e submissao dos
// quadros de animacao ao SpriteSystem. Saiu do Engine.cpp em 2026-09-04, na
// quebra do arquivo.
//
// O namespace anonimo no topo (abertura morfologica da mascara de alfa e
// deteccao de sprite de classe) veio junto: so' a decodificacao de sprite usa.

#include "core/Engine.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/Profiler.hpp"
#include "formats/SpriteTypes.hpp"
#include "core/JobSystem.hpp"
#include <stb_image.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <vector>

namespace eruption {

namespace {
    // Morphological opening (erosion followed by dilation) on an alpha mask.
    // Removes thin ghost pixels around class sprites so content bounds reflect
    // the real silhouette.
    void morphologicalOpen(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, int w, int h, int iterations) {
        std::vector<uint8_t> tmp(src.size());
        std::vector<uint8_t> srcCopy(src);
        std::vector<uint8_t>* in = &srcCopy;
        std::vector<uint8_t>* out = &tmp;
        for (int it = 0; it < iterations; ++it) {
            // Erosion
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    size_t i = y * w + x;
                    if ((*in)[i] == 0) { (*out)[i] = 0; continue; }
                    uint8_t v = 1;
                    if (x > 0)        v &= (*in)[i - 1];
                    if (x < w - 1)    v &= (*in)[i + 1];
                    if (y > 0)        v &= (*in)[i - w];
                    if (y < h - 1)    v &= (*in)[i + w];
                    (*out)[i] = v;
                }
            }
            std::swap(in, out);
            // Dilation
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    size_t i = y * w + x;
                    if ((*in)[i] == 1) { (*out)[i] = 1; continue; }
                    uint8_t v = 0;
                    if (x > 0)        v |= (*in)[i - 1];
                    if (x < w - 1)    v |= (*in)[i + 1];
                    if (y > 0)        v |= (*in)[i - w];
                    if (y < h - 1)    v |= (*in)[i + w];
                    (*out)[i] = v;
                }
            }
            std::swap(in, out);
        }
        dst.assign(in->begin(), in->end());
    }

    struct ContentBounds {
        int minX = -1, minY = -1, maxX = -1, maxY = -1;
        bool empty() const { return minX < 0; }
    };

    [[maybe_unused]] ContentBounds computeAlphaBounds(const uint8_t* rgba, int w, int h, int alphaThreshold) {
        ContentBounds b;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (rgba[(y * w + x) * 4 + 3] > alphaThreshold) {
                    if (b.empty()) { b.minX = b.maxX = x; b.minY = b.maxY = y; }
                    else {
                        b.minX = std::min(b.minX, x); b.maxX = std::max(b.maxX, x);
                        b.minY = std::min(b.minY, y); b.maxY = std::max(b.maxY, y);
                    }
                }
            }
        }
        return b;
    }

    bool isClassSpritePath(const std::string& path) {
        static const char* classNames[] = { "storm_wizard", "berserker", "falconer", "morning_star" };
        std::string lower;
        lower.reserve(path.size());
        for (char c : path) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        for (const char* name : classNames) {
            if (lower.find(name) != std::string::npos) return true;
        }
        return false;
    }
}

uint32_t Engine::createTextureFromSprImage(const SprImage& img, const uint8_t* palette, bool usePalette, std::vector<TextureResource>& outResources, bool swapRB, const std::string& sourcePath, bool preCleaned) {
    TextureResource res;
    res.width = img.width; res.height = img.height;
    
    std::vector<uint8_t> convertedPixels;
    bool isActuallyIndexed = img.isIndexed;

    if (usePalette && palette) {
        convertedPixels.resize(img.width * img.height * 4);
        for (size_t i = 0; i < img.pixels.size(); ++i) {
            uint8_t idx = img.pixels[i];
            if (idx == 0) {
                // Transparent index 0: map to magenta with alpha 0 (cleaned in next pass)
                convertedPixels[i*4]   = 255;
                convertedPixels[i*4+1] = 0;
                convertedPixels[i*4+2] = 255;
                convertedPixels[i*4+3] = 0;
            } else {
                convertedPixels[i*4]   = palette[idx*4];
                convertedPixels[i*4+1] = palette[idx*4+1];
                convertedPixels[i*4+2] = palette[idx*4+2];
                convertedPixels[i*4+3] = 255; // Opaque
            }
        }
        isActuallyIndexed = false; // Converted to RGBA!
    }

    res.isIndexed = isActuallyIndexed;
    
    VkFormat format = isActuallyIndexed ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t bytesPerPixel = isActuallyIndexed ? 1 : 4;
    VkDeviceSize imageSize = (VkDeviceSize)img.width * img.height * bytesPerPixel;

    if (img.width == 0 || img.height == 0 || img.pixels.empty()) {
        ERUPTION_LOG_WARN("createTextureFromSprImage: empty image (%dx%d, pixels=%zu)", img.width, img.height, img.pixels.size());
        res.slot = 0;
        outResources.push_back(res);
        return 0;
    }

    uint32_t mipLevels = m_mipmapMenu.config.enabled ? MipmapGenerator::calculateMipLevels(img.width, img.height) : 1;

    const uint8_t* srcPixels = (usePalette && palette) ? convertedPixels.data() : img.pixels.data();
    // Build an RGBA CPU copy for content-bound analysis.
    std::vector<uint8_t> rgbaPixels;
    if (preCleaned) {
        // folha preparada pelo preload: nada a analisar nem copiar
    } else if (img.isIndexed && palette && usePalette) {
        rgbaPixels.resize((size_t)img.width * img.height * 4);
        for (size_t i = 0; i < (size_t)img.width * img.height; ++i) {
            uint8_t idx = img.pixels[i];
            rgbaPixels[i * 4 + 0] = palette[idx * 3 + 0];
            rgbaPixels[i * 4 + 1] = palette[idx * 3 + 1];
            rgbaPixels[i * 4 + 2] = palette[idx * 3 + 2];
            rgbaPixels[i * 4 + 3] = (idx == 0) ? 0 : 255;
        }
    } else {
        rgbaPixels.resize(img.pixels.size());
        memcpy(rgbaPixels.data(), img.pixels.data(), img.pixels.size());
        if (swapRB) {
            for (size_t i = 0; i < (size_t)img.width * img.height; ++i) {
                std::swap(rgbaPixels[i * 4 + 0], rgbaPixels[i * 4 + 2]);
            }
        }
    }

    // Class sprites may have fake ghost pixels to force dimensions; run a
    // morphological opening to discover the real silhouette bounds.
    if (!preCleaned && !sourcePath.empty() && isClassSpritePath(sourcePath)) {
        ERUPTION_LOG_WARN("TODO: deprecated future - applying erosion+dilation to class sprite %s", sourcePath.c_str());
        std::vector<uint8_t> mask((size_t)img.width * img.height);
        for (size_t i = 0; i < mask.size(); ++i) mask[i] = rgbaPixels[i * 4 + 3] > 10 ? 1 : 0;
        std::vector<uint8_t> opened;
        morphologicalOpen(mask, opened, img.width, img.height, 1);
        ContentBounds b;
        for (int y = 0; y < (int)img.height; ++y) {
            for (int x = 0; x < (int)img.width; ++x) {
                if (opened[y * img.width + x]) {
                    if (b.empty()) { b.minX = b.maxX = x; b.minY = b.maxY = y; }
                    else {
                        b.minX = std::min(b.minX, x); b.maxX = std::max(b.maxX, x);
                        b.minY = std::min(b.minY, y); b.maxY = std::max(b.maxY, y);
                    }
                }
            }
        }
        if (!b.empty()) {
            res.contentMinX = b.minX; res.contentMinY = b.minY;
            res.contentMaxX = b.maxX; res.contentMaxY = b.maxY;
            ERUPTION_LOG_WARN("  content bounds: (%d,%d)-(%d,%d)", b.minX, b.minY, b.maxX, b.maxY);
        }
    }

    VkBuffer staging; VmaAllocation stagingAlloc;
    m_vulkan.createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* data; vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &data);
    if (!isActuallyIndexed && !preCleaned) {
        // CPU pre-pass to remove transparent magenta (FF00FF) bleeding
        std::vector<uint8_t> cleanPixels(img.width * img.height * 4);
        std::memcpy(cleanPixels.data(), srcPixels, (size_t)imageSize);
        int w = img.width;
        int h = img.height;

        // Same magenta-keying + dilation as every other texture path
        // (single BFS fill inside ImageUtils::dilate).
        ImageUtils::dilate(w, h, cleanPixels);

        memcpy(data, cleanPixels.data(), (size_t)imageSize);
    } else {
        memcpy(data, srcPixels, (size_t)imageSize);
    }
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);
    if (!img.pixels.empty()) {
        if (isActuallyIndexed) {
            ERUPTION_LOG_INFO("  Sprite pixels (indexed): first=%02x last=%02x", img.pixels[0], img.pixels.back());
        } else {
            ERUPTION_LOG_INFO("  Sprite pixels (rgba): first=%02x%02x%02x%02x", srcPixels[0], srcPixels[1], srcPixels[2], srcPixels[3]);
        }
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    m_vulkan.createImage(img.width, img.height, format, usage, VMA_MEMORY_USAGE_GPU_ONLY, res.image, res.alloc, mipLevels);
    std::vector<SpriteUpload> single;
    std::vector<SpriteUpload>& uploads = m_spriteUploads ? *m_spriteUploads : single;
    uploads.push_back({staging, stagingAlloc, res.image, img.width, img.height, mipLevels, format});
    if (!m_spriteUploads) flushSpriteUploads(single);

    VkImageViewCreateInfo viewInfo{}; viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; viewInfo.image = res.image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = format; viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; viewInfo.subresourceRange.levelCount = mipLevels; viewInfo.subresourceRange.layerCount = 1;
    vkc::createImageView(m_vulkan.device(), &viewInfo, nullptr, &res.view);

    res.slot = m_bindless.allocateSlot();
    res.mipLevels = mipLevels;
    m_bindless.updateTexture(res.slot, res.view, m_nearestSampler);
    outResources.push_back(res);
    ERUPTION_LOG_INFO("Created sprite texture: %dx%d %s -> slot %u (mips=%u)", img.width, img.height, isActuallyIndexed ? "indexed" : "rgba", res.slot, mipLevels);
    return res.slot;
}

namespace {

// Os quadros de uma folha .spr v2/v3 com o mesmo preparo da carga no quadro
// (corte de alfa do loadEruptSprV2 + dilatacao do createTextureFromSprImage),
// sem tocar na GPU: roda num job. Nulo se o arquivo nao e' desse formato.
std::shared_ptr<Engine::PreparedSheet> prepareSpriteSheet(const std::string& sprPath) {
    std::ifstream f(sprPath, std::ios::binary);
    char magic[8] = {};
    f.read(magic, 8);
    if (!f || std::memcmp(magic, "ERUPTSPR", 8) != 0) return nullptr;
    auto u32 = [&f]() { uint32_t v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; };
    const uint32_t version = u32();
    if (version != 2 && version != 3) return nullptr;
    const uint32_t count = u32();
    if (count == 0 || count > 4096) return nullptr;
    auto sheet = std::make_shared<Engine::PreparedSheet>();
    sheet->sizes.reserve(size_t(count) * 2);
    sheet->pixels.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        int w = static_cast<int>(u32()), h = static_cast<int>(u32());
        if (!f || w <= 0 || h <= 0 || w > 4096 || h > 4096) return nullptr;
        std::vector<uint8_t>& px = sheet->pixels[i];
        px.resize(size_t(w) * h * 4);
        f.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(px.size()));
        if (!f) return nullptr;
        for (size_t p = 0; p < size_t(w) * h; ++p) {
            uint8_t* c = &px[p * 4];
            if (c[3] < 128) c[0] = c[1] = c[2] = c[3] = 0; else c[3] = 255;
        }
        ImageUtils::dilate(w, h, px);
        sheet->sizes.push_back(static_cast<uint32_t>(w));
        sheet->sizes.push_back(static_cast<uint32_t>(h));
    }
    return sheet;
}

} // namespace

void Engine::preloadCharacterSprite(const std::string& png) {
    std::filesystem::path spr = project::gameRef(png);
    spr.replace_extension(".spr");
    const std::string key = spr.string();
    if (m_characterSpriteCache.count(png) || m_spritePreloads.count(key)) return; // ja' na GPU ou a caminho
    std::error_code ec;
    if (!std::filesystem::exists(spr, ec)) return; // so' PNG: carga simples, sem preparo caro
    m_spritePreloads.emplace(key, JobSystem::instance().submit([key] { return prepareSpriteSheet(key); }).share());
}

namespace {

// ERUPTION_TEST_NO_UPLOAD_BATCH=1 (teste): um envio por quadro de folha, como
// antes do lote - so' para medir o lote contra a carga antiga no mesmo
// binario (18:14: 455-462 ms sem lote, 89-92 com, 3 folhas do elenco).
bool uploadBatchEnabled() {
    static const bool kOn = std::getenv("ERUPTION_TEST_NO_UPLOAD_BATCH") == nullptr;
    return kOn;
}

} // namespace

void Engine::flushSpriteUploads(std::vector<SpriteUpload>& uploads) {
    if (uploads.empty()) return;
    m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
        for (const SpriteUpload& u : uploads) {
            m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{}; region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.imageSubresource.layerCount = 1; region.imageExtent = { u.width, u.height, 1 };
            vkCmdCopyBufferToImage(cmd, u.staging, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            if (u.mipLevels > 1) {
                generateMipmaps(cmd, u.image, u.width, u.height, u.mipLevels, u.format, 2);
            } else {
                m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            }
        }
    });
    for (const SpriteUpload& u : uploads) vmaDestroyBuffer(m_vulkan.allocator(), u.staging, u.stagingAlloc);
    uploads.clear();
}

bool Engine::loadSpriteFromPng(const std::string& path, LoadedSprite& out) {
    cleanupSpriteTextures(out);

    // Caminho relativo citado por cena/script: sai da pasta do projeto.
    const std::string disk = project::gameRef(path).string();
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(disk.c_str(), &w, &h, &channels, 4);
    if (!data) {
        ERUPTION_LOG_WARN("loadSpriteFromPng: failed to load %s", path.c_str());
        return false;
    }

    SprImage img;
    img.width = static_cast<uint16_t>(w);
    img.height = static_cast<uint16_t>(h);
    img.isIndexed = false;
    img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);

    // Pre-process: remove anti-aliased purple/magenta fringes from the alpha border.
    // Some PNGs carry colored semi-transparent pixels that become a visible outline
    // under nearest/bilinear sampling; threshold them to hard alpha and neutral RGB.
    for (int i = 0; i < w * h; ++i) {
        uint8_t a = img.pixels[i * 4 + 3];
        if (a < 128) {
            img.pixels[i * 4 + 0] = 0;
            img.pixels[i * 4 + 1] = 0;
            img.pixels[i * 4 + 2] = 0;
            img.pixels[i * 4 + 3] = 0;
        } else {
            img.pixels[i * 4 + 3] = 255;
        }
    }

    createTextureFromSprImage(img, nullptr, false, out.textures, false, path);
    out.indexedTextureCount = 0;
    out.isPngSprite = true;

    // Build a minimal ACT with a single action/frame/sprite so renderSpritePart works.
    AnimFile anim;
    ActAction action;
    ActFrame frame;
    ActSprite spr{};
    spr.index = 0;
    spr.type = 1; // RGBA texture
    spr.offsetX = 0;
    spr.offsetY = 0;
    // High-resolution PNG sprites are much larger than source sprites;
    // scale them down so they match the in-world size of a character.
    // default.png is 57x84; source sprites are typically 64x128. Scale so that
    // the visual height matches a source sprite (scale 1.0 = 84px, source = 128px).
    // Calibrate so a 57x84 PNG matches the in-world size of a source sprite.
    // Source sprites are rendered with an additional 0.15 scale in renderSpritePart.
    // A typical source sprite cell is ~128 px tall; default.png is 84 px tall.
    // Target visual height = 128 * 0.15 ~= 19.2 world units, so:
    //   pngScale = 128 / 84 ~= 1.52
    // This makes default.png appear at the same size as a standard source body sprite.
    float pngScale = 2.0f;
    spr.scaleX = pngScale;
    spr.scaleY = pngScale;
    spr.width = img.width;
    spr.height = img.height;
    spr.color = Vec4(1.0f);
    frame.sprites.push_back(spr);
    action.frames.push_back(frame);
    action.delay = 150.0f;
    anim.actions.push_back(action);
    out.act = anim;
    out.loaded = true;

    m_bindless.flushUpdates();
    ERUPTION_LOG_INFO("loadSpriteFromPng: loaded %s (%dx%d) as sprite scale=%.2f", path.c_str(), w, h, pngScale);
    return true;
}

bool Engine::loadSpriteFromEruptSpr(const std::string& path, LoadedSprite& out) {
    cleanupSpriteTextures(out);

    std::ifstream f(project::gameRef(path), std::ios::binary);
    if (!f) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: failed to open %s", path.c_str());
        return false;
    }

    auto readU32 = [&]() -> uint32_t {
        uint32_t v;
        f.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    };
    auto readI32 = [&]() -> int32_t {
        int32_t v;
        f.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    };
    auto readF32 = [&]() -> float {
        float v;
        f.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    };

    char magic[8];
    f.read(magic, 8);
    if (std::memcmp(magic, "ERUPTSPR", 8) != 0) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid magic in %s", path.c_str());
        return false;
    }

    uint32_t version = readU32();
    if (version == 2 || version == 3) return loadEruptSprV2(f, path, out, version);
    if (version != 1) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: unsupported version %u in %s", version, path.c_str());
        return false;
    }

    uint32_t texWidth = readU32();
    uint32_t texHeight = readU32();
    uint32_t texChannels = readU32();
    if (texWidth == 0 || texHeight == 0 || texChannels != 4) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid texture %ux%ux%u in %s", texWidth, texHeight, texChannels, path.c_str());
        return false;
    }

    size_t pixelBytes = static_cast<size_t>(texWidth) * texHeight * texChannels;
    std::vector<uint8_t> pixels(pixelBytes);
    f.read(reinterpret_cast<char*>(pixels.data()), pixelBytes);
    if (!f) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: truncated pixel data in %s", path.c_str());
        return false;
    }

    // Pre-process alpha igual ao PNG.
    for (size_t i = 0; i < texWidth * texHeight; ++i) {
        uint8_t a = pixels[i * 4 + 3];
        if (a < 128) {
            pixels[i * 4 + 0] = 0;
            pixels[i * 4 + 1] = 0;
            pixels[i * 4 + 2] = 0;
            pixels[i * 4 + 3] = 0;
        } else {
            pixels[i * 4 + 3] = 255;
        }
    }

    SprImage img;
    img.width = static_cast<uint16_t>(texWidth);
    img.height = static_cast<uint16_t>(texHeight);
    img.isIndexed = false;
    img.pixels = std::move(pixels);
    createTextureFromSprImage(img, nullptr, false, out.textures, false, path);

    uint32_t dirCount = readU32();
    uint32_t actionCount = readU32();
    if (dirCount == 0 || dirCount > 64 || actionCount == 0 || actionCount > 64) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid dirCount=%u actionCount=%u in %s", dirCount, actionCount, path.c_str());
        return false;
    }

    AnimFile anim;
    for (uint32_t a = 0; a < actionCount; ++a) {
        ActAction action;
        action.delay = 150.0f;
        uint32_t framesPerDir = readU32();
        if (framesPerDir == 0 || framesPerDir > 256) {
            ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid framesPerDir=%u in %s", framesPerDir, path.c_str());
            return false;
        }
        for (uint32_t d = 0; d < dirCount; ++d) {
            ActFrame frame;
            for (uint32_t fr = 0; fr < framesPerDir; ++fr) {
                uint32_t spriteCount = readU32();
                if (spriteCount == 0 || spriteCount > 256) {
                    ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid spriteCount=%u in %s", spriteCount, path.c_str());
                    return false;
                }
                for (uint32_t s = 0; s < spriteCount; ++s) {
                    ActSprite spr{};
                    spr.index = readU32();
                    spr.type = 1; // RGBA texture
                    spr.offsetX = readI32();
                    spr.offsetY = readI32();
                    spr.scaleX = readF32();
                    spr.scaleY = readF32();
                    spr.color = Vec4(readF32(), readF32(), readF32(), readF32());
                    spr.width = img.width;
                    spr.height = img.height;
                    frame.sprites.push_back(spr);
                }
            }
            action.frames.push_back(frame);
        }
        anim.actions.push_back(action);
    }

    out.act = anim;
    out.indexedTextureCount = 0;
    out.isPngSprite = true; // Usa o mesmo offset vertical corrigido do PNG.
    out.loaded = true;

    m_bindless.flushUpdates();
    ERUPTION_LOG_INFO("loadSpriteFromEruptSpr: loaded %s (%dx%d) actions=%u dirs=%u", path.c_str(), texWidth, texHeight, actionCount, dirCount);
    return true;
}

bool Engine::loadEruptSprV2(std::ifstream& f, const std::string& path, LoadedSprite& out, uint32_t version) {
    // Todos os quadros da folha num envio so': um immediateSubmit por quadro
    // (dezenas a centenas por folha) era o grosso do custo de criar um elenco
    // na chegada a um mapa. O envio sai ao fim desta funcao, por qualquer
    // retorno - as imagens de uma folha recusada so' morrem depois dos
    // quadros em voo (cleanupSpriteTextures), entao a copia ainda vale.
    // ERUPTION_TEST_LOAD_PHASES=1: [SPRITEPHASE] por folha - leitura e
    // preparo na CPU (corte de alfa, dilatacao) contra o envio a' GPU.
    static const bool kPhases = std::getenv("ERUPTION_TEST_LOAD_PHASES") != nullptr;
    struct BatchScope {
        Engine& e;
        const std::string& path;
        std::vector<SpriteUpload> uploads;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        BatchScope(Engine& en, const std::string& p) : e(en), path(p) {
            if (uploadBatchEnabled()) e.m_spriteUploads = &uploads;
        }
        ~BatchScope() {
            e.m_spriteUploads = nullptr;
            const size_t n = uploads.size();
            const auto t1 = std::chrono::steady_clock::now();
            e.flushSpriteUploads(uploads);
            if (kPhases) {
                const auto t2 = std::chrono::steady_clock::now();
                const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
                ERUPTION_LOG_WARN("[SPRITEPHASE] %s: %zu quadros, cpu %.1f, envio %.1f ms", path.c_str(), n, ms(t0, t1),
                                  ms(t1, t2));
            }
        }
    } batch(*this, path);
    auto u32 = [&]() { uint32_t v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; };
    auto i32 = [&]() { int32_t v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; };
    auto f32 = [&]() { float v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; };
    const uint32_t imageCount = u32();
    if (imageCount == 0 || imageCount > 4096) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid image count %u in %s", imageCount, path.c_str());
        return false;
    }
    // Folha preparada por characters.preload: so' a pronta (nunca espera o
    // job) e so' se bate com o arquivo; os pixels do arquivo sao pulados.
    std::shared_ptr<PreparedSheet> ready;
    if (auto it = m_spritePreloads.find(path); it != m_spritePreloads.end()) {
        if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            ready = it->second.get();
            m_spritePreloads.erase(it); // na GPU, a folha fica no cache de personagens
            if (ready && ready->pixels.size() != imageCount) ready.reset();
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    for (uint32_t i = 0; i < imageCount; ++i) {
        const uint32_t w = u32(), h = u32();
        if (w == 0 || h == 0 || w > 4096 || h > 4096) {
            ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid image %ux%u in %s", w, h, path.c_str());
            cleanupSpriteTextures(out);
            return false;
        }
        SprImage img;
        img.width = static_cast<uint16_t>(w);
        img.height = static_cast<uint16_t>(h);
        img.isIndexed = false;
        const size_t bytes = size_t(w) * h * 4;
        if (ready && ready->pixels[i].size() == bytes && ready->sizes[i * 2] == w && ready->sizes[i * 2 + 1] == h) {
            f.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
            img.pixels = std::move(ready->pixels[i]);
            createTextureFromSprImage(img, nullptr, false, out.textures, false, path, true);
            sizes.push_back({w, h});
            continue;
        }
        img.pixels.resize(bytes);
        f.read(reinterpret_cast<char*>(img.pixels.data()), static_cast<std::streamsize>(img.pixels.size()));
        if (!f) {
            ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: truncated image data in %s", path.c_str());
            cleanupSpriteTextures(out);
            return false;
        }
        // Mesmo corte de alfa do PNG (sem meia-transparência no G-buffer).
        for (size_t p = 0; p < size_t(w) * h; ++p) {
            uint8_t* px = &img.pixels[p * 4];
            if (px[3] < 128) px[0] = px[1] = px[2] = px[3] = 0; else px[3] = 255;
        }
        createTextureFromSprImage(img, nullptr, false, out.textures, false, path);
        sizes.push_back({w, h});
    }
    const uint32_t actionCount = u32();
    if (actionCount == 0 || actionCount > 256) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid action count %u in %s", actionCount, path.c_str());
        cleanupSpriteTextures(out);
        return false;
    }
    AnimFile anim;
    for (uint32_t a = 0; a < actionCount; ++a) {
        ActAction action;
        action.delay = static_cast<float>(std::max(16u, u32()));
        const uint32_t frames = u32();
        if (frames == 0 || frames > 256) {
            ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid frame count %u in %s", frames, path.c_str());
            cleanupSpriteTextures(out);
            return false;
        }
        for (uint32_t fr = 0; fr < frames; ++fr) {
            ActFrame frame;
            const uint32_t spriteCount = u32();
            if (spriteCount == 0 || spriteCount > 64) {
                ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: invalid sprite count %u in %s", spriteCount, path.c_str());
                cleanupSpriteTextures(out);
                return false;
            }
            for (uint32_t k = 0; k < spriteCount; ++k) {
                ActSprite spr{};
                spr.index = static_cast<int32_t>(u32());
                spr.type = 1;
                spr.offsetX = i32();
                spr.offsetY = i32();
                spr.scaleX = f32();
                spr.scaleY = f32();
                spr.color = Vec4(f32(), f32(), f32(), f32());
                spr.flags = static_cast<int32_t>(u32());
                if (spr.index < 0 || spr.index >= static_cast<int32_t>(imageCount)) {
                    ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: frame uses missing image %d in %s", spr.index, path.c_str());
                    cleanupSpriteTextures(out);
                    return false;
                }
                spr.width = static_cast<int>(sizes[spr.index].first);
                spr.height = static_cast<int>(sizes[spr.index].second);
                frame.sprites.push_back(spr);
            }
            action.frames.push_back(std::move(frame));
        }
        anim.actions.push_back(std::move(action));
    }
    if (!f) {
        ERUPTION_LOG_WARN("loadSpriteFromEruptSpr: truncated file %s", path.c_str());
        cleanupSpriteTextures(out);
        return false;
    }
    out.named.clear();
    if (version >= 3) {
        // Animações extras: nome -> primeira das 8 ações (uma por direção).
        const uint32_t count = u32();
        for (uint32_t i = 0; i < count && i < 256 && f; ++i) {
            uint16_t len = 0;
            f.read(reinterpret_cast<char*>(&len), 2);
            std::string name(len, '\0');
            f.read(name.data(), len);
            const uint32_t first = u32(), loop = u32();
            if (f && first + 8 <= anim.actions.size()) out.named.push_back({name, first, loop != 0});
        }
    }
    out.act = std::move(anim);
    out.indexedTextureCount = 0;
    out.isPngSprite = true;
    out.authoredTiming = true;
    out.loaded = true;
    m_bindless.flushUpdates();
    ERUPTION_LOG_INFO("loadSpriteFromEruptSpr: loaded %s (v2, %u images, %u actions)", path.c_str(), imageCount, actionCount);
    return true;
}

void Engine::cleanupSpriteTextures(LoadedSprite& ls) {
    // Folha dividida com outra personagem: só desencosta. Quem soltar por
    // último é que devolve os slots (ver Engine::loadCharacterSprite).
    if (ls.texturesShared && ls.texturesShared.use_count() > 1) {
        ls.texturesShared.reset();
        ls.textures.clear();
        ls.indexedTextureCount = 0;
        ls.loaded = false;
        ls.authoredTiming = false;
        return;
    }
    ls.texturesShared.reset();
    // Frames em voo ainda podem amostrar as texturas: destruir depois deles.
    for (auto& tex : ls.textures) {
        VkDevice dev = m_vulkan.device();
        VmaAllocator allocator = m_vulkan.allocator();
        VkImageView view = tex.view;
        VkImage image = tex.image;
        VmaAllocation alloc = tex.alloc;
        m_vulkan.deferSlotImageCleanup([dev, allocator, view, image, alloc]() {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
            if (image != VK_NULL_HANDLE) vmaDestroyImage(allocator, image, alloc);
        });
        m_bindless.freeSlot(tex.slot);
    }
    m_bindless.flushUpdates(); // o slot volta à textura padrão antes de a imagem morrer
    ls.textures.clear();
    ls.indexedTextureCount = 0;
    ls.loaded = false;
    ls.authoredTiming = false;
}

Engine::TextureStats Engine::getTextureStats() const {
    TextureStats stats;
    
    auto calcVRAM = [](uint32_t width, uint32_t height, uint32_t mipLevels, bool isIndexed) -> uint64_t {
        uint64_t totalBytes = 0;
        uint32_t w = width;
        uint32_t h = height;
        uint32_t bytesPerPixel = isIndexed ? 1 : 4;
        for (uint32_t i = 0; i < mipLevels; i++) {
            totalBytes += (uint64_t)w * h * bytesPerPixel;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        return totalBytes;
    };
    
    // 1. Model textures
    {
        std::lock_guard<std::mutex> lock(m_assetCache.mutex);
        for (const auto& pair : m_assetCache.modelTextures) {
            const auto& tex = pair.second.tex;
            if (tex.image == VK_NULL_HANDLE) continue;
            stats.totalTextures++;
            stats.modelTexCount++;
            if (tex.mipLevels > 1) {
                stats.texturesWithMipmaps++;
            }
            stats.totalVRAMBytes += calcVRAM(tex.width, tex.height, tex.mipLevels, false);
            stats.baseVRAMBytes += (uint64_t)tex.width * tex.height * 4;
        }
        
        // 2. Terrain textures
        for (const auto& pair : m_assetCache.terrainTextures) {
            const auto& tex = pair.second.tex;
            if (tex.image == VK_NULL_HANDLE) continue;
            stats.totalTextures++;
            stats.terrainTexCount++;
            if (tex.mipLevels > 1) {
                stats.texturesWithMipmaps++;
            }
            stats.totalVRAMBytes += calcVRAM(tex.width, tex.height, tex.mipLevels, false);
            stats.baseVRAMBytes += (uint64_t)tex.width * tex.height * 4;
        }
    }
    
    // 3. Sprite textures
    if (m_playerController) {
        auto processSprite = [&](const LoadedSprite& ls) {
            if (!ls.loaded) return;
            for (const auto& tex : ls.textures) {
                if (tex.image == VK_NULL_HANDLE) continue;
                stats.totalTextures++;
                stats.spriteTexCount++;
                if (tex.mipLevels > 1) {
                    stats.texturesWithMipmaps++;
                }
                stats.totalVRAMBytes += calcVRAM(tex.width, tex.height, tex.mipLevels, tex.isIndexed);
                stats.baseVRAMBytes += (uint64_t)tex.width * tex.height * (tex.isIndexed ? 1 : 4);
            }
        };
        processSprite(m_playerController->bodySprite());
        processSprite(m_playerController->hairSprite());
    }
    
    return stats;
}

void Engine::calculateMaxCharHeight() {
    float maxHeight = 0.0f;
    float scale = 0.15f;

    auto scanSprite = [&](LoadedSprite& ls) {
        if (!ls.loaded) return;
        for (const auto& action : ls.act.actions) {
            for (const auto& frame : action.frames) {
                for (const auto& spr : frame.sprites) {
                    int texIdx = spr.index;
                    if (spr.type == 1) texIdx += (int)ls.indexedTextureCount;
                    if (texIdx < 0 || texIdx >= (int)ls.textures.size()) continue;
                    
                    const auto& tex = ls.textures[texIdx];
                    float h = (float)tex.height * scale * spr.scaleY;
                    // PNG billboards are placed with their bottom edge at the feet
                    // (see renderSpritePart), so their top is the full height;
                    // .spr frames are centred on the pivot with an offset.
                    float topY = ls.isPngSprite ? h : ((-spr.offsetY * scale) + (h * 0.5f));
                    if (topY > maxHeight) maxHeight = topY;
                }
            }
        }
    };

    scanSprite(m_playerController->bodySpriteRef());
    scanSprite(m_playerController->hairSpriteRef());

    if (maxHeight < 1.0f) maxHeight = 2.0f;
    m_playerController->spriteHeightRef() = maxHeight;
    ERUPTION_LOG_INFO("Recalculated character max height: %.2f", m_playerController->spriteHeightRef());
}

void Engine::renderSpritePart(LoadedSprite& ls, uint32_t paletteSlot, int& frameIdx, float& timer, float dt, int& outCount, bool isBody) {
    // Don't draw the player sprite until the active map has finished loading,
    // otherwise it appears in the void while the background loader is still
    // uploading GPU resources.
    if (!m_playerSpawnedOnActiveMap) return;
    if (m_playerController->hidden()) return;
    if (!ls.loaded || ls.act.actions.empty()) {
        if (ls.loaded) ERUPTION_LOG_WARN("renderSpritePart: loaded but no actions");
        return;
    }
    
    // 8-Angle selection: Calculate source direction (0-7) based on relative angle
    // charYaw: 0=S (+Z), 90=E (+X), 180=N (-Z), 270=W (-X)
    // We use the camera's forward vector to get its world-space Yaw
    int direction;
    if (m_playerController->tacticalView()) {
        // Tactical view: sprite is glued to screen, always use the same facing
        direction = 0;
    } else {
        Vec3 camForward = m_camera.forward();
        float camAngle = glm::degrees(std::atan2(camForward.x, camForward.z));
        float charAngle = glm::degrees(m_playerController->yaw());
        
        // Relative angle: how the character is moving relative to the camera's view
        // 180 degree offset is necessary because source direction 0 is "facing the camera"
        float roAngle = camAngle - charAngle + 180.0f;
        
        while (roAngle < 0) roAngle += 360.0f;
        while (roAngle >= 360.0f) roAngle -= 360.0f;
        
        direction = static_cast<int>((roAngle + 22.5f) / 45.0f) % 8;
    }
    
    // Most source sprites have 8 directions per state. 
    // Action 0-7: Idle, 8-15: Walk, 16-23: Attack1, 24-31: Hurt, 32-39: ReadyFight, etc.
    int actionIdx = direction; 
    // Animação extra pedida pelo jogo (sprite v3): por cima de parado e andar.
    const LoadedSprite::Named* named = isBody && !m_playerAction.empty() ? ls.findNamed(m_playerAction) : nullptr;
    if (isBody && !m_playerAction.empty() && !named) m_playerAction.clear();
    if (named) {
        if (m_playerActionStarted) {
            frameIdx = 0;
            timer = 0.0f;
            m_playerActionStarted = false;
        }
        actionIdx = static_cast<int>(named->first) + direction;
    } else if (m_playerController->tacticalView()) {
        // Tactical view: use ReadyFight (stance with weapon ready) instead of plain idle
        actionIdx = 4 * 8 + direction; // READYFIGHT = action 4
    } else if (m_playerController->isMoving()) {
        actionIdx += 8; // Switch to Walk action
    }
    if (actionIdx >= (int)ls.act.actions.size()) actionIdx %= 8; // Fallback to Idle
    // PNG avulso tem uma ação só: qualquer direção cai nela.
    if (actionIdx >= (int)ls.act.actions.size()) actionIdx = 0;

    const auto& action = ls.act.actions[actionIdx];
    if (action.frames.empty()) {
        ERUPTION_LOG_WARN("renderSpritePart: action %d has no frames", actionIdx);
        return;
    }

    // Safety: ensure frameIdx is within bounds for the CURRENT action
    if (frameIdx >= (int)action.frames.size()) {
        frameIdx = 0;
    }

    float delay = action.delay > 0 ? action.delay : 150.0f;
    // Proportional Animation Scaling: 100 speed = 65ms delay.
    // As speed increases (including Shift), delay decreases (animation faster).
    if (m_playerController->isMoving() && !m_playerController->tacticalView()) {
        if (ls.authoredTiming) {
            // SPRITE IMPORTADO: o atraso da folha e' o do passo natural do
            // boneco, ~1,1 altura de corpo por segundo (a mesma regra dos
            // personagens em Characters.cpp). A referencia antiga era uma
            // velocidade fixa (100), que so' casava com um boneco do tamanho
            // em que o elenco foi desenhado: um jogo que encolhe o personagem
            // via as pernas andarem devagar demais para o chao que passava.
            // A velocidade e' a do CHAO (o passo do controle sai pela metade
            // fora da camera livre, ver PlayerController::updateMovement).
            float altura = 0.0f;
            const auto& camadas = action.frames[static_cast<size_t>(frameIdx)].sprites;
            if (!camadas.empty()) {
                const auto& spr = camadas.front();
                const int idx = spr.index + (spr.type == 1 ? static_cast<int>(ls.indexedTextureCount) : 0);
                if (idx >= 0 && idx < static_cast<int>(ls.textures.size()))
                    altura = static_cast<float>(ls.textures[static_cast<size_t>(idx)].height) * 0.15f * spr.scaleY;
            }
            const float chao = m_playerController->actualMoveSpeed() * (m_playerController->freeCamera() ? 1.0f : 0.5f);
            if (altura > 1.0f && chao > 1.0f)
                delay *= std::clamp(1.1f * altura / chao, 0.25f, 2.0f);
        } else {
            // Sprites antigos (sem atraso proprio): 65 ms a 100 de velocidade.
            delay = 65.0f * (100.0f / std::max(1.0f, m_playerController->actualMoveSpeed()));
        }
    }
    
    timer += dt * 1000.0f;
    if (timer >= delay) { 
        // Action index 8-15 is walking
        bool isWalkAction = (actionIdx >= 8 && actionIdx <= 15);
        if (isBody || m_playerController->isMoving() || isWalkAction) {
            frameIdx = (frameIdx + 1) % action.frames.size();
            // Uma vez só: terminou ao voltar ao primeiro quadro.
            if (named && frameIdx == 0 && !m_playerActionLoop) {
                m_playerAction.clear();
                frameIdx = static_cast<int>(action.frames.size()) - 1;
            }
        } else {
            // Head/hair frames are directions (Straight, Left, Right), not animation frames while idle
            frameIdx = 0; 
        }
        timer = 0.0f; 
    }
    const auto& frame = action.frames[frameIdx];
    int validSprites = 0, invalidSprites = 0;
    
    // Update body attachment point for head alignment
    if (isBody) {
        m_playerController->bodyAttachPointRef() = Vec2(0.0f);
        for (const auto& ap : frame.attachPoints) {
            if (ap.attr == 0) {
                m_playerController->bodyAttachPointRef() = Vec2((float)ap.x, (float)ap.y);
                // Mirror body anchor if body sprite is mirrored
                if (!frame.sprites.empty() && (frame.sprites[0].flags & 1)) {
                    m_playerController->bodyAttachPointRef().x = -m_playerController->bodyAttachPointRef().x;
                }
                break;
            }
        }
    }

    // For head/hair, find its own attachment point to align with body's
    Vec2 headAttachPoint(0.0f);
    if (!isBody && (m_playerController->isMoving() || m_playerController->tacticalView())) {
        for (const auto& ap : frame.attachPoints) {
            if (ap.attr == 0) {
                headAttachPoint = Vec2((float)ap.x, (float)ap.y);
                // Mirror head anchor if head sprite is mirrored
                if (!frame.sprites.empty() && (frame.sprites[0].flags & 1)) {
                    headAttachPoint.x = -headAttachPoint.x;
                }
                break;
            }
        }
    }
    
    for (const auto& spr : frame.sprites) {
        int texIdx = spr.index;
        if (spr.type == 1) {
            texIdx += (int)ls.indexedTextureCount;
        }
        if (texIdx < 0 || texIdx >= (int)ls.textures.size()) {
            if (spr.index >= 0) {
                ERUPTION_LOG_WARN("renderSpritePart: sprite index %d (type=%d) out of range [0,%zu)", 
                                 spr.index, spr.type, ls.textures.size());
            }
            invalidSprites++;
            continue;
        }
        const auto& tex = ls.textures[texIdx];
        Sprite s;
        float scale = m_playerController->tacticalView() ? 1.0f : 0.15f;
        bool mirror = (spr.flags & 1);
        
        s.position = m_playerController->pos();
        s.position.y += 2.0f; // Body base height
        
        // Pivot at the CENTER of the sprite, not the feet.
        // This makes the billboard rotate around its own center of mass,
        // eliminating the "joão bobo" arc completely.
        float centerY = m_playerController->spriteHeightRef() * 0.5f;
        s.anchorPoint = m_playerController->pos();
        s.anchorPoint.y += centerY;

        float spriteWorldWidth = (float)tex.width * scale * spr.scaleX;
        float spriteWorldHeight = (float)tex.height * scale * spr.scaleY;

        // PNG billboards are single-texture replacements for source sprites; place
        // their base at the character's feet instead of centering on m_pos.
        if (ls.isPngSprite) {
            // The shader computes localPos.y = a_position.y * scaleY - centerY.
            // a_position.y ranges from -0.5 to 0.5, so the sprite is centered
            // around anchorPoint when centerY == 0. Shift it up by half height
            // so the bottom edge sits at the character's feet.
            // PNG replacement art usually has transparent padding below the feet.
            // Start with the bottom edge at the character's feet (centerY = -height/2)
            // and let content with padding sit slightly above the ground.
            centerY = -spriteWorldHeight * 0.5f;
            s.anchorPoint = m_playerController->pos();
        }

        float xOff = (float)spr.offsetX;
        float yOff = (float)spr.offsetY;

        if (mirror) xOff = -xOff;

        if (!isBody && (m_playerController->isMoving() || m_playerController->tacticalView())) {
            xOff += m_playerController->bodyAttachPointRef().x - headAttachPoint.x;
            yOff += m_playerController->bodyAttachPointRef().y - headAttachPoint.y;
        }

        // We pass the local offsets (scaled) to the shader.
        // texRect.xy = ACT offsets (xOff, yOff) relative to the original feet pivot
        // texRect.w  = centerY offset so the shader can rebase offsets from feet to center
        float spriteYaw;
        if (m_playerController->tacticalView()) {
            // In tactical view: sprite faces forward (character's direction), not the camera
            spriteYaw = m_playerController->yaw();
        } else {
            // Normal view: continuous billboard yaw so the sprite smoothly follows the camera
            // CRITICAL: use m_camera.target() (the actual orbit pivot) NOT m_playerController->m_pos,
            // because the camera orbits around its smoothed target, not the raw character position.
            Vec3 toCam = m_camera.position() - m_camera.target();
            spriteYaw = std::atan2(toCam.x, toCam.z);
        }
        s.texRect = Vec4(xOff * scale, yOff * scale, spriteYaw, centerY);

        s.size = Vec2(spriteWorldWidth, spriteWorldHeight);
        if (std::getenv("ERUPTION_TEST_HUD_DEBUG")) {
            static int s_left = 2;
            if (s_left-- > 0)
                ERUPTION_LOG_WARN("[HUD-DEBUG] sprite %s tex=%ux%u scale=%.2f sx=%.2f sy=%.2f -> size=(%.2f,%.2f) centerY=%.2f anchor=(%.1f,%.1f,%.1f) png=%d camDist=%.1f",
                    isBody ? "body" : "hair", tex.width, tex.height, scale, spr.scaleX, spr.scaleY, s.size.x, s.size.y, centerY,
                    s.anchorPoint.x, s.anchorPoint.y, s.anchorPoint.z, (int)ls.isPngSprite, glm::length(m_camera.position() - s.anchorPoint));
        }
        s.texIndex = tex.slot;
        s.paletteIndex = paletteSlot;
        s.flags = SpriteFlags::Billboard;
        if (!m_playerController->tacticalView()) {
            s.flags = s.flags | SpriteFlags::CastShadow;
        }
        if (mirror) s.flags = s.flags | SpriteFlags::FlipX;
        if (tex.isIndexed) s.flags = s.flags | SpriteFlags::UsePalette;
        if (ls.isPngSprite) s.flags = s.flags | SpriteFlags::PngSprite;
        s.tintColor = spr.color;
        s.sortOrder = isBody ? 0.0f : 10.0f;
        m_spriteSystem.submitSprite(s);
        outCount++;
        validSprites++;
    }
    if (validSprites > 0 || invalidSprites > 0) {
        ERUPTION_LOG_INFO("renderSpritePart: frame %d/%zu valid=%d invalid=%d (incl -1) tex=%zu idx=%u",
                         frameIdx, action.frames.size(), validSprites, invalidSprites,
                         ls.textures.size(), ls.indexedTextureCount);
    }
}

} // namespace eruption
