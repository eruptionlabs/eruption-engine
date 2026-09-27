// Resolucao de textura do Engine: caminho de albedo/normal/MRAH-W para modelo
// e para terreno (cache global, .etexpack ja' comprimido, sintese de PBR
// quando nao ha' mapa cozido) e os utilitarios de criacao de textura a partir
// de pixels/arquivo. Saiu do Engine.cpp em 2026-09-04, na quebra do arquivo.
//
// O upload em si NAO esta' aqui: estas funcoes so' ENFILEIRAM em
// s_pendingUploads/s_pendingMeshes (core/EngineInternal.hpp) e quem drena e'
// o MapUpload.cpp, no command buffer do frame.

#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "core/Logger.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/EtexLoader.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/TextureCache.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "formats/MapLoader.hpp"
#include <stb_image.h>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

bool Engine::createTextureFromPixels(const unsigned char* pixels, int w, int h, Engine::TextureResource& out) {
    out.width = w;
    out.height = h;
    out.isIndexed = false;
    
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize imageSize = (VkDeviceSize)w * h * 4;

    if (!pixels) return false;

    uint32_t mipLevels = m_mipmapMenu.config.enabled ? MipmapGenerator::calculateMipLevels(w, h) : 1;

    VkBuffer staging; VmaAllocation stagingAlloc;
    m_vulkan.createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* data; vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);

    m_vulkan.createImage(w, h, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VMA_MEMORY_USAGE_GPU_ONLY, out.image, out.alloc, mipLevels);
    PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, m_vulkan.allocationSize(out.alloc));
    m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
        m_vulkan.cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{}; region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.imageSubresource.layerCount = 1; region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (mipLevels > 1) {
            MipmapGenerator::generateMipmapsBlit(cmd, out.image, w, h, mipLevels);
        } else {
            m_vulkan.cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    });
    vmaDestroyBuffer(m_vulkan.allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{}; viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; viewInfo.image = out.image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = format; viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; viewInfo.subresourceRange.levelCount = mipLevels; viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(m_vulkan.device(), &viewInfo, nullptr, &out.view);
    
    return true;
}

bool Engine::createTextureFromFile(const char* path, Engine::TextureResource& out) {
    int w, h, channels;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        ERUPTION_LOG_WARN("Failed to load texture file: %s", path);
        return false;
    }

    out.width = w;
    out.height = h;
    out.isIndexed = false;
    
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize imageSize = (VkDeviceSize)w * h * 4;
    uint32_t mipLevels = m_mipmapMenu.config.enabled ? MipmapGenerator::calculateMipLevels(w, h) : 1;

    VkBuffer staging; VmaAllocation stagingAlloc;
    m_vulkan.createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* data; vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);
    
    stbi_image_free(pixels);

    m_vulkan.createImage(w, h, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VMA_MEMORY_USAGE_GPU_ONLY, out.image, out.alloc, mipLevels);
    PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, m_vulkan.allocationSize(out.alloc));
    m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
        m_vulkan.cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{}; region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.imageSubresource.layerCount = 1; region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (mipLevels > 1) {
            MipmapGenerator::generateMipmapsBlit(cmd, out.image, w, h, mipLevels);
        } else {
            m_vulkan.cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    });
    vmaDestroyBuffer(m_vulkan.allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{}; viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; viewInfo.image = out.image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = format; viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; viewInfo.subresourceRange.levelCount = mipLevels; viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(m_vulkan.device(), &viewInfo, nullptr, &out.view);
    
    return true;
}

bool Engine::findEmbeddedAlbedoPixels(const std::string& name, std::vector<uint8_t>& outPixels,
                                      int& outW, int& outH, int& outChannels) {
    if (!m_currentMap) return false;
    std::string sanitized = name;
    std::replace(sanitized.begin(), sanitized.end(), '/', '\\');
    if (!sanitized.empty() && sanitized[0] == '\\') sanitized = sanitized.substr(1);

    for (const auto& model : m_currentMap->models) {
        for (const auto& et : model.embeddedTextures) {
            if (et.name != sanitized) continue;
            if (!et.pixels.empty()) {
                outPixels = et.pixels;
                outW = et.width;
                outH = et.height;
                outChannels = et.channels > 0 ? et.channels : 4;
                return true;
            }
            if (!et.encodedData.empty()) {
                ImageData img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(),
                                                              et.encodedData.size());
                if (img.isValid()) {
                    outPixels = std::move(img.pixels);
                    outW = img.width;
                    outH = img.height;
                    outChannels = img.channels > 0 ? img.channels : 4;
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

uint32_t Engine::resolveModelTexture(const std::string& path) {
    std::string sanitized = path;
    std::replace(sanitized.begin(), sanitized.end(), '/', '\\');
    if (!sanitized.empty() && sanitized[0] == '\\') sanitized = sanitized.substr(1);

    ModelTextureGPU gpuTex;
    if (m_assetCache.getModelTexture(sanitized, gpuTex)) {
        return gpuTex.bindlessSlot;
    }

    // Textura embutida JA' COMPRIMIDA pelo bake (BC1/BC3 + mips prontos):
    // sobe direto do payload, sem decode, sem blit chain, 4-8x menos VRAM.
    {
        auto bakedIt = m_embeddedBaked.find(sanitized);
        if (bakedIt != m_embeddedBaked.end() && bakedIt->second.albedo.valid()) {
            const EtexData& e = bakedIt->second.albedo;
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = static_cast<VkFormat>(e.vkFormat);
            imageInfo.extent = { e.width, e.height, 1 };
            imageInfo.mipLevels = e.mipCount;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            VmaAllocationCreateInfo allocCI{};
            allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation alloc = VK_NULL_HANDLE;
            if (vmaCreateImage(m_vulkan.allocator(), &imageInfo, &allocCI, &image, &alloc, nullptr) == VK_SUCCESS) {
                PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, m_vulkan.allocationSize(alloc));
                VkBuffer staging;
                VmaAllocation stagingAlloc;
                m_vulkan.createBuffer(e.payload.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
                void* mapped = nullptr;
                vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
                std::memcpy(mapped, e.payload.data(), e.payload.size());
                vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);
                s_pendingUploads.push_back({image, alloc, staging, stagingAlloc,
                                            e.width, e.height, e.mipCount, e.mipSizes});

                VkImageView view;
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = image;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = static_cast<VkFormat>(e.vkFormat);
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.levelCount = e.mipCount;
                viewInfo.subresourceRange.layerCount = 1;
                vkCreateImageView(m_vulkan.device(), &viewInfo, nullptr, &view);

                uint32_t slot = m_bindless.allocateSlotSafe();
                // O formato que o bake escolheu E' o flag de opacidade: BC1 so' sai
                // quando a textura nao tem alfa (bake_assets.py). Guardado por slot
                // para o passe de sombra pular o fragment shader em caster solido.
                m_bindless.setSlotOpaque(slot, e.vkFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
                if (slot != 0) m_bindless.updateTextureSafe(slot, view, m_defaultSampler);

                ModelTextureGPU tex{};
                tex.image = image;
                tex.alloc = alloc;
                tex.view = view;
                tex.bindlessSlot = slot;
                tex.width = static_cast<int>(e.width);
                tex.height = static_cast<int>(e.height);
                tex.mipLevels = e.mipCount;
                m_assetCache.addModelTexture(sanitized, tex);
                return slot;
            }
        }
    }

    // Embedded textures inside the current map's models (e.g. GLB images).
    uint32_t embeddedSlot = [&]() -> uint32_t {
        if (!m_currentMap) {
            ERUPTION_LOG_WARN("Engine::resolveModelTexture: m_currentMap is null for '%s'", sanitized.c_str());
            return 0;
        }
        ERUPTION_LOG_INFO("Engine::resolveModelTexture: searching embedded texture '%s' in %zu models (%zu total embedded)",
                          sanitized.c_str(), m_currentMap->models.size(),
                          std::accumulate(m_currentMap->models.begin(), m_currentMap->models.end(), size_t(0),
                                          [](size_t s, const ModelFile& m) { return s + m.embeddedTextures.size(); }));
        for (const auto& model : m_currentMap->models) {
            for (const auto& et : model.embeddedTextures) {
                if (et.name != sanitized) continue;

                // Decode lazily on first use.
                std::vector<uint8_t> tempDecodedPixels;
                const uint8_t* pixelsData = nullptr;
                size_t pixelSize = 0;
                int decodedWidth = et.width;
                int decodedHeight = et.height;
                constexpr int EMBEDDED_TEXTURE_MAX_SIZE = 1024;
                if (!et.isDecoded() && et.hasEncodedData()) {
                    ImageData img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(), et.encodedData.size());
                    if (!img.isValid()) {
                        ERUPTION_LOG_WARN("Engine::resolveModelTexture: failed to decode embedded texture '%s'", sanitized.c_str());
                        continue;
                    }
                    ImageUtils::downscaleRGBA(img.width, img.height, img.pixels, EMBEDDED_TEXTURE_MAX_SIZE);
                    tempDecodedPixels = std::move(img.pixels);
                    pixelsData = tempDecodedPixels.data();
                    pixelSize = tempDecodedPixels.size();
                    decodedWidth = img.width;
                    decodedHeight = img.height;
                } else if (et.isDecoded()) {
                    pixelsData = et.pixels.data();
                    pixelSize = et.pixels.size();
                    decodedWidth = et.width;
                    decodedHeight = et.height;
                } else {
                    ERUPTION_LOG_WARN("Engine::resolveModelTexture: embedded texture '%s' has no data", sanitized.c_str());
                    continue;
                }

                if (decodedWidth <= 0 || decodedHeight <= 0 || pixelSize == 0) {
                    ERUPTION_LOG_WARN("Engine::resolveModelTexture: invalid embedded texture '%s'", sanitized.c_str());
                    continue;
                }

                ERUPTION_LOG_INFO("Engine::resolveModelTexture: using embedded texture '%s' (%dx%d)",
                                  sanitized.c_str(), decodedWidth, decodedHeight);

                uint32_t mipLevels = m_mipmapMenu.config.enabled ? MipmapGenerator::calculateMipLevels(decodedWidth, decodedHeight) : 1;

                VkImage image;
                VmaAllocation alloc;
                VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                if (mipLevels > 1) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                m_vulkan.createImage(decodedWidth, decodedHeight, VK_FORMAT_R8G8B8A8_UNORM,
                    usage, VMA_MEMORY_USAGE_GPU_ONLY, image, alloc, mipLevels);
                PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, m_vulkan.allocationSize(alloc));

                VkBuffer staging;
                VmaAllocation stagingAlloc;
                m_vulkan.createBuffer(pixelSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
                void* mapped;
                vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
                std::memcpy(mapped, pixelsData, pixelSize);
                vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);

                s_pendingUploads.push_back({image, alloc, staging, stagingAlloc, static_cast<uint32_t>(decodedWidth), static_cast<uint32_t>(decodedHeight), mipLevels, {}});

                VkImageView view;
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = image;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.levelCount = mipLevels;
                viewInfo.subresourceRange.layerCount = 1;
                vkCreateImageView(m_vulkan.device(), &viewInfo, nullptr, &view);

                uint32_t slot = m_bindless.allocateSlotSafe();
                if (slot != 0) {
                    m_bindless.updateTextureSafe(slot, view, m_defaultSampler);
                }

                ModelTextureGPU tex{};
                tex.image = image;
                tex.alloc = alloc;
                tex.view = view;
                tex.bindlessSlot = slot;
                tex.width = decodedWidth;
                tex.height = decodedHeight;
                tex.mipLevels = mipLevels;
                m_assetCache.addModelTexture(sanitized, tex);
                return slot;
            }
        }
        return 0;
    }();
    if (embeddedSlot != 0) return embeddedSlot;

    auto readFileBytes = [](const std::string& p) -> std::vector<uint8_t> {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        f.seekg(0, std::ios::end);
        size_t size = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> d(size);
        f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(size));
        return d;
    };

    size_t lastSlash = sanitized.find_last_of('\\');
    std::string filename = (lastSlash != std::string::npos) ? sanitized.substr(lastSlash + 1) : sanitized;

    // Try filesystem first (for CC0 / non-pack maps).
    std::vector<std::string> fsSearchPaths;
    std::string fsPath = sanitized;
    std::replace(fsPath.begin(), fsPath.end(), '\\', '/');
    if (!fsPath.empty() && fsPath[0] == '/') fsPath = fsPath.substr(1);
    auto addWithExtensions = [&](const std::string& base) {
        fsSearchPaths.push_back(base);
        fsSearchPaths.push_back(base + ".png");
        fsSearchPaths.push_back(base + ".jpg");
        fsSearchPaths.push_back(base + ".jpeg");
    };
    addWithExtensions("assets/data/texture/" + fsPath);
    addWithExtensions("assets/pbr/base/" + fsPath);
    addWithExtensions("assets/data/texture/" + filename);
    addWithExtensions("assets/pbr/base/" + filename);

    namespace fs = std::filesystem;
    if (fs::exists("assets/data/texture") && fs::is_directory("assets/data/texture")) {
        for (const auto& entry : fs::directory_iterator("assets/data/texture")) {
            if (entry.is_directory()) {
                addWithExtensions(entry.path().string() + "/" + filename);
            }
        }
    }

    std::vector<uint8_t> texData;
    std::string foundPath;
    for (const auto& p : fsSearchPaths) {
        texData = readFileBytes(p);
        if (!texData.empty()) {
            foundPath = p;
            break;
        }
    }

    // Fallback: extract from source resource archive.
    if (texData.empty()) {
        std::vector<std::string> searchPaths = {
            sanitized,
            "data\\texture\\" + sanitized,
            "data\\texture\\model\\" + sanitized
        };
        searchPaths.push_back("data\\texture\\model\\" + filename);
        searchPaths.push_back("data\\texture\\" + filename);

        for (const auto& p : searchPaths) {
            texData = m_packManager.extractSafe(p);
            if (!texData.empty()) break;
            // Try EUC-KR encoding for source pack entries.
            std::string euc = utf8ToEucKr(p);
            if (!euc.empty() && euc != p) {
                texData = m_packManager.extractSafe(euc);
                if (!texData.empty()) break;
            }
        }
    }

    if (texData.empty()) {
        ERUPTION_LOG_WARN("Engine::resolveModelTexture: failed to find '%s'", sanitized.c_str());
        m_assetCache.addModelTexture(sanitized, ModelTextureGPU{}); // cache miss to avoid repeated lookups
        return 0;
    }

    if (!foundPath.empty()) {
        ERUPTION_LOG_INFO("Engine::resolveModelTexture: loaded '%s' from '%s'", sanitized.c_str(), foundPath.c_str());
    }

    ImageData img = ImageUtils::loadFromMemory(texData.data(), texData.size());
    if (!img.isValid()) {
        ERUPTION_LOG_WARN("Engine::resolveModelTexture: failed to decode '%s'", sanitized.c_str());
        m_assetCache.addModelTexture(sanitized, ModelTextureGPU{});
        return 0;
    }

    uint32_t mipLevels = m_mipmapMenu.config.enabled ? MipmapGenerator::calculateMipLevels(img.width, img.height) : 1;

    VkImage image;
    VmaAllocation alloc;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    m_vulkan.createImage(img.width, img.height, VK_FORMAT_R8G8B8A8_UNORM,
        usage, VMA_MEMORY_USAGE_GPU_ONLY, image, alloc, mipLevels);
    PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, m_vulkan.allocationSize(alloc));

    VkBuffer staging;
    VmaAllocation stagingAlloc;
    VkDeviceSize pixelSize = img.pixels.size();
    m_vulkan.createBuffer(pixelSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* mapped;
    vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, img.pixels.data(), pixelSize);
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);

    m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
        m_vulkan.cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), 1 };
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (mipLevels > 1) {
            generateMipmaps(cmd, image, img.width, img.height, mipLevels, VK_FORMAT_R8G8B8A8_UNORM, 1);
        } else {
            m_vulkan.cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    });
    vmaDestroyBuffer(m_vulkan.allocator(), staging, stagingAlloc);

    VkImageView view;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(m_vulkan.device(), &viewInfo, nullptr, &view);

    uint32_t slot = m_bindless.allocateSlotSafe();
    if (slot != 0) {
        m_bindless.updateTextureSafe(slot, view, m_defaultSampler);
    }

    ModelTextureGPU tex{};
    tex.image = image;
    tex.alloc = alloc;
    tex.view = view;
    tex.bindlessSlot = slot;
    tex.width = img.width;
    tex.height = img.height;
    tex.mipLevels = mipLevels;
    m_assetCache.addModelTexture(sanitized, tex);
    return slot;
}

uint32_t Engine::resolveTerrainTexture(const std::string& path) {
    std::string sanitized = path;
    std::replace(sanitized.begin(), sanitized.end(), '/', '\\');
    if (!sanitized.empty() && sanitized[0] == '\\') sanitized = sanitized.substr(1);

    TerrainTextureGPU gpuTex;
    if (m_assetCache.getTerrainTexture(sanitized, gpuTex)) {
        return gpuTex.bindlessSlot;
    }

    // Not in cache — extract from source archive and create
    std::vector<std::string> searchPaths = {
        sanitized,
        "data\\texture\\" + sanitized,
    };
    size_t lastSlash = sanitized.find_last_of('\\');
    std::string filename = (lastSlash != std::string::npos) ? sanitized.substr(lastSlash + 1) : sanitized;
    searchPaths.push_back("data\\texture\\" + filename);

    std::vector<uint8_t> texData;
    for (const auto& p : searchPaths) {
        texData = m_packManager.extractSafe(p);
        if (!texData.empty()) break;
    }

    if (texData.empty()) {
        ERUPTION_LOG_WARN("Engine::resolveTerrainTexture: failed to find '%s'", sanitized.c_str());
        m_assetCache.addTerrainTexture(sanitized, TerrainTextureGPU{});
        return 0;
    }

    ImageData img = ImageUtils::loadFromMemory(texData.data(), texData.size());
    if (!img.isValid()) {
        ERUPTION_LOG_WARN("Engine::resolveTerrainTexture: failed to decode '%s'", sanitized.c_str());
        m_assetCache.addTerrainTexture(sanitized, TerrainTextureGPU{});
        return 0;
    }

    uint32_t mipLevels = m_mipmapMenu.config.enabled ? MipmapGenerator::calculateMipLevels(img.width, img.height) : 1;

    VkImage image;
    VmaAllocation alloc;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    m_vulkan.createImage(img.width, img.height, VK_FORMAT_R8G8B8A8_UNORM,
        usage, VMA_MEMORY_USAGE_GPU_ONLY, image, alloc, mipLevels);
    PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, m_vulkan.allocationSize(alloc));

    VkBuffer staging;
    VmaAllocation stagingAlloc;
    VkDeviceSize pixelSize = img.pixels.size();
    m_vulkan.createBuffer(pixelSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* mapped;
    vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, img.pixels.data(), pixelSize);
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);

    m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
        m_vulkan.cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), 1 };
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (mipLevels > 1) {
            generateMipmaps(cmd, image, img.width, img.height, mipLevels, VK_FORMAT_R8G8B8A8_UNORM, 0);
        } else {
            m_vulkan.cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    });
    vmaDestroyBuffer(m_vulkan.allocator(), staging, stagingAlloc);

    VkImageView view;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(m_vulkan.device(), &viewInfo, nullptr, &view);

    uint32_t slot = m_bindless.allocateSlotSafe();
    if (slot != 0) {
        m_bindless.updateTextureSafe(slot, view, m_defaultSampler);
    }

    TerrainTextureGPU tex{};
    tex.image = image;
    tex.alloc = alloc;
    tex.view = view;
    tex.bindlessSlot = slot;
    tex.width = img.width;
    tex.height = img.height;
    tex.mipLevels = mipLevels;
    m_assetCache.addTerrainTexture(sanitized, tex);
    return slot;
}

ModelMeshGPU Engine::resolveModelMesh(const std::string& legacyModelPath, uint32_t nodeIndex,
                                        const std::vector<TerrainVertex>& vertices,
                                        const std::vector<uint32_t>& indices) {
    std::string key = legacyModelPath + "#" + std::to_string(nodeIndex);

    ModelMeshGPU cached;
    if (m_assetCache.getModelMesh(key, cached)) {
        return cached;
    }

    VkDeviceSize vertexSize = vertices.size() * sizeof(TerrainVertex);
    VkDeviceSize indexSize = indices.size() * sizeof(uint32_t);

    ModelMeshGPU mesh{};
    mesh.isExternal = true; // Managed by AssetCache
    m_vulkan.createBuffer(vertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY, mesh.vertexBuffer, mesh.vertexAlloc);
    m_vulkan.createBuffer(indexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY, mesh.indexBuffer, mesh.indexAlloc);
    // E' por AQUI que a malha de mapa GLB entra (resolver do cache) - era o
    // caminho sem ALLOC que deixava Meshes em 51 MB com 769 MB de
    // vertex+index vivos (San Miguel, inventario do VMA no frame 250).
    PROFILE_VRAM_ALLOC(ProfilerCategory::Meshes, vertexSize + indexSize);

    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    m_vulkan.createBuffer(vertexSize + indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer, stagingAlloc);
    void* mapped;
    vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, vertices.data(), vertexSize);
    std::memcpy((uint8_t*)mapped + vertexSize, indices.data(), indexSize);
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);

    if (m_vulkan.isFrameRecording()) {
        VkCommandBuffer cmd = m_vulkan.currentCmdBuf();
        VkBufferCopy cv{0, 0, vertexSize};
        vkCmdCopyBuffer(cmd, stagingBuffer, mesh.vertexBuffer, 1, &cv);
        VkBufferCopy ci{vertexSize, 0, indexSize};
        vkCmdCopyBuffer(cmd, stagingBuffer, mesh.indexBuffer, 1, &ci);
        
        m_vulkan.deferFrameCleanup([vulkan = &m_vulkan, stagingBuffer, stagingAlloc]() {
            vmaDestroyBuffer(vulkan->allocator(), stagingBuffer, stagingAlloc);
        });
    } else {
        s_pendingMeshes.push_back({stagingBuffer, stagingAlloc, mesh.vertexBuffer, mesh.vertexAlloc, mesh.indexBuffer, mesh.indexAlloc, vertexSize, indexSize});
        
        if (s_pendingMeshes.size() >= 256) {
            m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
                for (auto& pm : s_pendingMeshes) {
                    VkBufferCopy cv{0, 0, pm.vSize};
                    vkCmdCopyBuffer(cmd, pm.staging, pm.vb, 1, &cv);
                    VkBufferCopy ci{pm.vSize, 0, pm.iSize};
                    vkCmdCopyBuffer(cmd, pm.staging, pm.ib, 1, &ci);
                }
            });
            for (auto& pm : s_pendingMeshes) {
                vmaDestroyBuffer(m_vulkan.allocator(), pm.staging, pm.alloc);
            }
            s_pendingMeshes.clear();
        }
    }

    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.bindlessTexSlot = vertices.empty() ? 0 : vertices[0].texIndex;
    mesh.isExternal = true; // owned by global cache

    // Compute AABB
    Vec3 aabbMin(FLT_MAX), aabbMax(-FLT_MAX);
    for (const auto& v : vertices) {
        aabbMin = glm::min(aabbMin, v.position);
        aabbMax = glm::max(aabbMax, v.position);
    }
    mesh.aabbMin = aabbMin;
    mesh.aabbMax = aabbMax;

    m_assetCache.addModelMesh(key, mesh);
    return mesh;
}

} // namespace eruption
