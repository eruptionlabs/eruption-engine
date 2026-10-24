// Pintura de camadas do chão (splat): duas máscaras RGBA (8 camadas) com um
// texel por ponto da grade do mapa. Num mapa que já tem splat (vindo do
// gerador) as máscaras editáveis tomam o lugar das originais no mesmo slot;
// num mapa sem splat (New Map) os vértices do chão passam a apontar para elas
// quando a primeira camada é escolhida. Salvas como PNG ao lado da cena.
#include "core/Engine.hpp"
#include <mutex>
#include <chrono>
#include "core/JobSystem.hpp"

#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "scene/SceneEdits.hpp"
#include "utils/ImageUtils.hpp"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace eruption {

// Bits altos do slot de 16 bits de cada camada (o slot cabe em 12): a camada
// tem normal em slot+1 e/ou MRAH-W em slot+2. O vertice do chao nao ganha
// atributo novo (o celular tem teto de 16).
constexpr uint32_t kSplatHasNormal = 0x8000u;
constexpr uint32_t kSplatHasMrahw = 0x4000u;
constexpr uint32_t kSplatSlotMask = 0x3FFFu;

namespace {

namespace fs = std::filesystem;

fs::path maskPathFor(const std::string& map, int i) {
    fs::path p = SceneEdits::pathFor(map);
    p.replace_extension();
    p.replace_extension(".splat" + std::to_string(i) + ".png");
    return p;
}

// Imagem RGBA8 sem mips para a máscara: amostrada direto pelo chão e
// atualizada por retângulo enquanto se pinta.
bool createMaskImage(VulkanContext& vk, uint32_t w, uint32_t h, const uint8_t* rgba, Engine::TextureResource& out) {
    if (!vk.createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, out.image, out.alloc, 1))
        return false;
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBuffer staging;
    VmaAllocation sa;
    vk.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, sa);
    void* mapped = nullptr;
    vmaMapMemory(vk.allocator(), sa, &mapped);
    std::memcpy(mapped, rgba, bytes);
    vmaUnmapMemory(vk.allocator(), sa);
    vk.immediateSubmit([&](VkCommandBuffer cmd) {
        vk.cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        vk.cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    });
    vmaDestroyBuffer(vk.allocator(), staging, sa);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(vk.device(), &vi, nullptr, &out.view);
    out.width = w;
    out.height = h;
    return true;
}

// PNG sem tratamento (loadPNG aplica chave magenta e corte de alfa, feitos
// para sprites; numa máscara o alfa é o peso da 4ª camada).
ImageData loadRawPNG(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return {};
    return ImageUtils::loadFromMemoryRaw(bytes.data(), bytes.size());
}

// Reamostra (vizinho mais próximo) uma imagem RGBA para w x h.
std::vector<uint8_t> resampleRGBA(const uint8_t* src, int sw, int sh, int channels, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(size_t(w) * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const int sx = std::min(sw - 1, int((x + 0.5f) * sw / w));
            const int sy = std::min(sh - 1, int((y + 0.5f) * sh / h));
            for (int c = 0; c < 4; ++c)
                out[(size_t(y) * w + x) * 4 + c] = c < channels ? src[(size_t(sy) * sw + sx) * channels + c] : (c == 3 ? 0 : 0);
        }
    return out;
}

} // namespace

std::string Engine::textureNameForSlot(uint32_t slot) const {
    for (const auto& [name, s] : m_modelRenderer.getTextureCache())
        if (s == slot) return name;
    return {};
}

bool Engine::beginSplatEditing() {
    SplatEdit& sp = m_splatEdit;
    if (sp.active) return true;
    const HeightEdit& h = m_heightEdit;
    if (!h.active || h.meshTransform.empty()) return false; // chao com grade (o indice do pincel nao entra aqui)
    sp = SplatEdit{};
    sp.w = h.w;
    sp.d = h.d;
    for (auto& m : sp.mask) m.assign(size_t(sp.w) * sp.d * 4, 0);

    // Splat que o mapa já traz: slots das máscaras e das camadas nos vértices.
    uint32_t oldSlot[2] = {0, 0};
    for (const auto& [mesh, t] : h.meshTransform) {
        const auto* verts = m_modelRenderer.groundVertices(mesh);
        if (!verts) continue;
        for (const auto& v : *verts) {
            if (v.blendMaskIndex == 0) continue;
            oldSlot[0] = v.blendMaskIndex;
            oldSlot[1] = v.blendMaskIndex2;
            const uint32_t packed[4] = {v.splatTex01, v.splatTex23, v.splatTex45, v.splatTex67};
            for (int l = 0; l < 8; ++l) sp.layerSlot[l] = (packed[l / 2] >> (16 * (l % 2))) & 0xFFFFu;
            break;
        }
        if (oldSlot[0]) break;
    }
    sp.fromMap = oldSlot[0] != 0;
    for (int l = 0; l < 8; ++l)
        if (sp.layerSlot[l]) sp.layerNames[l] = textureNameForSlot(sp.layerSlot[l] & kSplatSlotMask);

    // Pixels de partida: as máscaras originais (texturas embutidas no GLB).
    for (int i = 0; i < 2 && sp.fromMap; ++i) {
        if (!oldSlot[i]) continue;
        const std::string name = textureNameForSlot(oldSlot[i]);
        if (!m_currentMap) break;
        for (auto& model : m_currentMap->models)
            for (auto& et : model.embeddedTextures) {
                if (et.name != name) continue;
                ImageData img;
                if (et.hasEncodedData()) img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(), et.encodedData.size());
                if (img.isValid())
                    sp.mask[i] = resampleRGBA(img.pixels.data(), img.width, img.height, img.channels, sp.w, sp.d);
                else if (et.isDecoded())
                    sp.mask[i] = resampleRGBA(et.pixels.data(), et.width, et.height, et.channels, sp.w, sp.d);
            }
    }
    // Máscaras salvas pelo editor têm prioridade.
    for (int i = 0; i < 2; ++i) {
        const fs::path p = maskPathFor(m_heightEdit.mapName.empty() ? m_currentMapName : m_heightEdit.mapName, i);
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        ImageData img = loadRawPNG(p);
        if (img.isValid()) sp.mask[i] = resampleRGBA(img.pixels.data(), img.width, img.height, img.channels, sp.w, sp.d);
    }
    sp.weights.resize(size_t(sp.w) * sp.d * 8);
    for (size_t g = 0; g < size_t(sp.w) * sp.d; ++g)
        for (int l = 0; l < 8; ++l) sp.weights[g * 8 + l] = sp.mask[l / 4][g * 4 + (l % 4)] / 255.0f;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_vulkan.device(), &si, nullptr, &sp.sampler);
    for (int i = 0; i < 2; ++i) {
        if (!createMaskImage(m_vulkan, sp.w, sp.d, sp.mask[i].data(), sp.gpu[i])) return false;
        // Mapa com splat: o slot original passa a mostrar a máscara editável.
        sp.slot[i] = oldSlot[i] ? oldSlot[i] : m_bindless.allocateSlot();
        sp.ownSlot[i] = oldSlot[i] == 0;
        m_bindless.updateTexture(sp.slot[i], sp.gpu[i].view, sp.sampler);
    }
    m_bindless.flushUpdates();
    sp.active = true;
    int peak = 0;
    for (const auto& m : sp.mask)
        for (uint8_t v : m) peak = std::max<int>(peak, v);
    ERUPTION_LOG_WARN("Terrain paint: %ux%u masks (%s, %s), peak weight %d/255", sp.w, sp.d,
                      sp.fromMap ? "map layers" : "editor layers",
                      maskPathFor(m_heightEdit.mapName, 0).string().c_str(), peak);
    return true;
}

void Engine::endSplatEditing() {
    SplatEdit& sp = m_splatEdit;
    if (!sp.active) return;
    m_vulkan.waitIdle();
    for (int i = 0; i < 2; ++i) {
        if (sp.ownSlot[i]) m_bindless.freeSlot(sp.slot[i]);
        if (sp.gpu[i].view) vkDestroyImageView(m_vulkan.device(), sp.gpu[i].view, nullptr);
        if (sp.gpu[i].image) vmaDestroyImage(m_vulkan.allocator(), sp.gpu[i].image, sp.gpu[i].alloc);
    }
    for (auto* set : {sp.layerTex, sp.layerNormal, sp.layerMrahw})
        for (int i = 0; i < 8; ++i) {
            TextureResource& l = set[i];
            if (!l.image) continue;
            m_bindless.freeSlot(l.slot);
            vkDestroyImageView(m_vulkan.device(), l.view, nullptr);
            vmaDestroyImage(m_vulkan.allocator(), l.image, l.alloc);
        }
    if (sp.sampler) vkDestroySampler(m_vulkan.device(), sp.sampler, nullptr);
    sp = SplatEdit{};
}


// RGBA da camada, reduzida pela metade (media 2x2, como um nivel de mip) ate'
// caber em maxSize. As camadas subiam no tamanho do arquivo (2048) mesmo com
// texture_max_size 256 no preset: 8 camadas x cor, normal e MRAH-W passavam de
// 400 MB com mips e o celular fraco fechava o jogo por falta de memoria.
static std::vector<uint8_t> splatRgba(const ImageData& img, uint32_t maxSize, uint32_t& w, uint32_t& h) {
    w = uint32_t(img.width);
    h = uint32_t(img.height);
    std::vector<uint8_t> rgba = img.channels == 4 ? img.pixels : resampleRGBA(img.pixels.data(), img.width, img.height, img.channels, w, h);
    while (std::max(w, h) > maxSize && w > 1 && h > 1) {
        const uint32_t hw = w / 2, hh = h / 2;
        std::vector<uint8_t> half(size_t(hw) * hh * 4);
        for (uint32_t y = 0; y < hh; ++y)
            for (uint32_t x = 0; x < hw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const uint8_t* p = rgba.data() + (size_t(2 * y) * w + 2 * x) * 4 + c;
                    const uint32_t sum = p[0] + p[4] + p[size_t(w) * 4] + p[size_t(w) * 4 + 4];
                    half[(size_t(y) * hw + x) * 4 + c] = uint8_t((sum + 2) / 4);
                }
        rgba = std::move(half);
        w = hw;
        h = hh;
    }
    return rgba;
}

// Camada lida do disco ja' no tamanho de envio. O cache das decodificadas
// guardava o arquivo inteiro ate' o envio: 21 PNG de 2048 (cor, normal e
// MRAH-W de 7 camadas) somavam +860 MB de pico na carga com texture_max_size
// 256, e o celular de 3 GB fechava o jogo. Reduzir no proprio job deixa so'
// as que estao decodificando ao mesmo tempo; a reducao e' a mesma do envio
// (splatRgba ja' abaixo do teto nao muda nada), entao a imagem e' identica.
static ImageData loadSplatLayer(const std::filesystem::path& path, uint32_t maxSize) {
    ImageData img = loadRawPNG(path);
    if (!img.isValid() || (std::max(img.width, img.height) <= int(maxSize) && img.channels == 4)) return img;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba = splatRgba(img, maxSize, w, h);
    img.pixels = std::move(rgba);
    img.width = int(w);
    img.height = int(h);
    img.channels = 4;
    return img;
}

// Imagem de camada: a ja' decodificada por prefetchSplatLayers, senao do disco.
ImageData Engine::splatLayerImage(const std::filesystem::path& path) {
    const std::string key = path.string();
    {
        std::lock_guard<std::mutex> lock(m_splatDecodeMutex);
        auto it = m_splatDecoded.find(key);
        if (it != m_splatDecoded.end()) {
            ImageData img = std::move(it->second);
            m_splatDecoded.erase(it);
            return img;
        }
    }
    return loadSplatLayer(path, uint32_t(m_textureMaxSize));
}

// As camadas da cena chegam juntas depois de uma troca de mapa: decodificar
// cor, normal e MRAH-W de cada uma, uma depois da outra, na thread principal
// (24 PNG de 2048 com as 8 camadas) congelava o quadro seguinte por ~4 s no
// andar de caverna. Aqui todas decodificam em paralelo; o envio continua em
// sequencia (addSplatLayer pega daqui).
namespace {

// Cor, _normal e _mrahw de cada camada que existem em disco.
std::vector<std::filesystem::path> splatLayerPaths(const std::vector<std::string>& layers) {
    std::vector<std::filesystem::path> paths;
    for (const auto& layer : layers) {
        const std::filesystem::path color = project::gameRef(layer);
        const std::filesystem::path base = color.parent_path() / color.stem();
        const std::string ext = color.extension().string();
        for (const std::filesystem::path& p : {color, std::filesystem::path(base.string() + "_normal" + ext),
                                               std::filesystem::path(base.string() + "_mrahw" + ext)}) {
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) paths.push_back(p);
        }
    }
    return paths;
}

} // namespace

void Engine::preloadSplatLayers(const std::vector<std::string>& layers) {
    for (const std::filesystem::path& p : splatLayerPaths(layers)) {
        const std::string key = p.string();
        {
            std::lock_guard<std::mutex> lock(m_splatDecodeMutex);
            if (m_splatDecoded.count(key)) continue;
        }
        JobSystem::instance().submit([this, p, key, maxSize = uint32_t(m_textureMaxSize)] {
            ImageData img = loadSplatLayer(p, maxSize);
            if (!img.isValid()) return;
            std::lock_guard<std::mutex> lock(m_splatDecodeMutex);
            m_splatDecoded[key] = std::move(img);
        });
    }
}

void Engine::prefetchSplatLayers(const std::vector<std::string>& layers) {
    std::vector<std::filesystem::path> paths = splatLayerPaths(layers);
    {
        // Ja' decodificadas pelo preload do mapa: so' o envio fica aqui.
        std::lock_guard<std::mutex> lock(m_splatDecodeMutex);
        paths.erase(std::remove_if(paths.begin(), paths.end(),
                                   [this](const std::filesystem::path& p) { return m_splatDecoded.count(p.string()) > 0; }),
                    paths.end());
    }
    if (paths.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ImageData> imgs(paths.size());
    const uint32_t maxSize = uint32_t(m_textureMaxSize);
    JobSystem::instance().parallelFor(static_cast<uint32_t>(paths.size()),
                                      [&](uint32_t i) { imgs[i] = loadSplatLayer(paths[i], maxSize); });
    std::lock_guard<std::mutex> lock(m_splatDecodeMutex);
    for (size_t i = 0; i < paths.size(); ++i)
        if (imgs[i].isValid()) m_splatDecoded[paths[i].string()] = std::move(imgs[i]);
    ERUPTION_LOG_WARN("Terrain layers: %zu imagens decodificadas em paralelo em %.0f ms", paths.size(),
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// Sobe a cor da camada e, se existirem ao lado dela, <nome>_normal e
// <nome>_mrahw (o mesmo par das pecas). Devolve o slot da cor ja' com os bits
// de quais mapas vieram; 0 se nem a cor subiu.
uint32_t Engine::uploadSplatLayer(TextureResource& colorTex, TextureResource& normalTex, TextureResource& mrahwTex,
                                  const std::filesystem::path& colorPath, const ImageData& color) {
    const std::filesystem::path base = colorPath.parent_path() / colorPath.stem();
    const std::string ext = colorPath.extension().string();
    ImageData nrm = splatLayerImage(base.string() + "_normal" + ext);
    ImageData mrh = splatLayerImage(base.string() + "_mrahw" + ext);
    const bool hasN = nrm.isValid(), hasM = mrh.isValid();
    uint32_t first = (hasN || hasM) ? m_bindless.allocateRange(3) : 0;
    const bool range = first != 0;
    if (!range) first = m_bindless.allocateSlot();
    if (first == 0) return 0;
    auto upload = [&](const ImageData& img, TextureResource& tex, uint32_t slot) {
        uint32_t w = 0, h = 0;
        const std::vector<uint8_t> rgba = splatRgba(img, uint32_t(m_textureMaxSize), w, h);
        if (!createTextureFromPixels(rgba.data(), int(w), int(h), tex)) return false;
        tex.slot = slot;
        m_bindless.updateTexture(slot, tex.view, m_defaultSampler);
        return true;
    };
    if (!upload(color, colorTex, first)) {
        m_bindless.freeSlot(first);
        if (range) { m_bindless.freeSlot(first + 1); m_bindless.freeSlot(first + 2); }
        return 0;
    }
    uint32_t packed = first;
    if (range) {
        if (hasN && upload(nrm, normalTex, first + 1)) packed |= kSplatHasNormal;
        else m_bindless.freeSlot(first + 1);
        if (hasM && upload(mrh, mrahwTex, first + 2)) packed |= kSplatHasMrahw;
        else m_bindless.freeSlot(first + 2);
    }
    return packed;
}

int Engine::splatLayerCount() const {
    int n = 0;
    for (int l = 0; l < 8; ++l)
        if (m_splatEdit.layerSlot[l]) n = l + 1;
    return n;
}

bool Engine::addSplatLayer(const std::string& png) {
    if (!beginSplatEditing()) return false;
    SplatEdit& sp = m_splatEdit;
    const int l = splatLayerCount();
    if (l >= 8 || (sp.fromMap && l >= 8)) return false;
    // Caminho RELATIVO vindo do arquivo de cena: a textura mora no projeto.
    const std::filesystem::path path = project::gameRef(png);
    ImageData img = splatLayerImage(path);
    if (!img.isValid()) return false;
    const uint32_t packedSlot = uploadSplatLayer(sp.layerTex[l], sp.layerNormal[l], sp.layerMrahw[l], path, img);
    if (packedSlot == 0) return false;
    m_bindless.flushUpdates();
    const TextureResource& tex = sp.layerTex[l];
    sp.layerSlot[l] = packedSlot;
    sp.layerNames[l] = png;
    sp.layerFiles[l] = png;
    ERUPTION_LOG_WARN("Terrain layer %d: %s (%ux%u) no slot %u%s%s", l, png.c_str(), tex.width, tex.height, tex.slot,
                      (packedSlot & kSplatHasNormal) ? " +normal" : "", (packedSlot & kSplatHasMrahw) ? " +mrahw" : "");
    // Os vértices do chão passam a ler as máscaras e as camadas.
    const uint32_t packed[4] = {sp.layerSlot[0] | (sp.layerSlot[1] << 16), sp.layerSlot[2] | (sp.layerSlot[3] << 16),
                                sp.layerSlot[4] | (sp.layerSlot[5] << 16), sp.layerSlot[6] | (sp.layerSlot[7] << 16)};
    const HeightEdit& h = m_heightEdit;
    for (const auto& [mesh, t] : h.meshTransform) {
        auto* verts = m_modelRenderer.groundVertices(mesh);
        if (!verts) continue;
        std::vector<uint32_t> all(verts->size());
        for (uint32_t i = 0; i < verts->size(); ++i) {
            TerrainVertex& v = (*verts)[i];
            const Vec3 wp = Vec3(t * Vec4(v.position, 1.0f));
            v.splatTex01 = packed[0];
            v.splatTex23 = packed[1];
            v.splatTex45 = packed[2];
            v.splatTex67 = packed[3];
            v.blendMaskIndex = static_cast<uint16_t>(sp.slot[0]);
            v.blendMaskIndex2 = static_cast<uint16_t>(sp.slot[1]);
            v.blendMaskUV = Vec2(((wp.x - h.offX) / h.scale + 0.5f) / h.w, ((wp.z - h.offZ) / h.scale + 0.5f) / h.d);
            all[i] = i;
        }
        m_modelRenderer.patchGroundVertices(mesh, all);
    }
    return true;
}

bool Engine::paintSplat(int layer, const Vec2& center, float radius, float strength, float falloff, float dt, bool erase,
                        int rect[4]) {
    SplatEdit& sp = m_splatEdit;
    const HeightEdit& h = m_heightEdit;
    if (!sp.active || radius <= 0.0f || layer < -1 || layer >= 8) return false;
    const int x0 = std::max(0, int(std::floor((center.x - radius - h.offX) / h.scale)));
    const int z0 = std::max(0, int(std::floor((center.y - radius - h.offZ) / h.scale)));
    const int x1 = std::min(int(sp.w) - 1, int(std::ceil((center.x + radius - h.offX) / h.scale)));
    const int z1 = std::min(int(sp.d) - 1, int(std::ceil((center.y + radius - h.offZ) / h.scale)));
    if (x0 > x1 || z0 > z1) return false;
    const float inner = std::clamp(1.0f - falloff, 0.0f, 1.0f);
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const float t = glm::length(Vec2(h.offX + x * h.scale, h.offZ + z * h.scale) - center) / radius;
            if (t >= 1.0f) continue;
            const float w = t <= inner ? 1.0f : 1.0f - glm::smoothstep(inner, 1.0f, t);
            const float add = std::min(1.0f, strength * w * dt);
            float* c = &sp.weights[(size_t(z) * sp.w + x) * 8];
            if (layer < 0 || erase) {
                // Base (ou apagar): a camada some e sobra a base do material.
                if (layer < 0) for (int l = 0; l < 8; ++l) c[l] *= (1.0f - add);
                else c[layer] = std::max(0.0f, c[layer] - add);
            } else {
                c[layer] = std::min(1.0f, c[layer] + add);
                // As outras cedem espaço para a soma não passar de 1.
                float others = 0.0f;
                for (int l = 0; l < 8; ++l) if (l != layer) others += c[l];
                const float room = 1.0f - c[layer];
                if (others > room && others > 0.0f)
                    for (int l = 0; l < 8; ++l) if (l != layer) c[l] *= room / others;
            }
            const size_t g = size_t(z) * sp.w + x;
            for (int l = 0; l < 8; ++l)
                sp.mask[l / 4][g * 4 + (l % 4)] = static_cast<uint8_t>(std::lround(std::clamp(c[l], 0.0f, 1.0f) * 255.0f));
        }
    for (auto& dr : sp.dirty) {
        dr[0] = std::min(dr[0], x0);
        dr[1] = std::min(dr[1], z0);
        dr[2] = std::max(dr[2], x1);
        dr[3] = std::max(dr[3], z1);
    }
    rect[0] = x0;
    rect[1] = z0;
    rect[2] = x1;
    rect[3] = z1;
    sp.painted = true;
    return true;
}

std::vector<float> Engine::splatRegion(int x0, int z0, int x1, int z1) const {
    const SplatEdit& sp = m_splatEdit;
    std::vector<float> out;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            for (int l = 0; l < 8; ++l) out.push_back(sp.weights[(size_t(z) * sp.w + x) * 8 + l]);
    return out;
}

void Engine::setSplatRegion(int x0, int z0, int x1, int z1, const std::vector<float>& values) {
    SplatEdit& sp = m_splatEdit;
    if (!sp.active) return;
    size_t k = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const size_t g = size_t(z) * sp.w + x;
            for (int l = 0; l < 8; ++l) {
                sp.weights[g * 8 + l] = values[k++];
                sp.mask[l / 4][g * 4 + (l % 4)] =
                    static_cast<uint8_t>(std::lround(std::clamp(sp.weights[g * 8 + l], 0.0f, 1.0f) * 255.0f));
            }
        }
    for (auto& dr : sp.dirty) {
        dr[0] = std::min(dr[0], x0);
        dr[1] = std::min(dr[1], z0);
        dr[2] = std::max(dr[2], x1);
        dr[3] = std::max(dr[3], z1);
    }
    sp.painted = true;
}

void Engine::flushSplatUploads(VkCommandBuffer cmd) {
    SplatEdit& sp = m_splatEdit;
    if (!sp.active) return;
    for (int i = 0; i < 2; ++i) {
        int* dr = sp.dirty[i];
        if (dr[0] > dr[2]) continue;
        const uint32_t rw = uint32_t(dr[2] - dr[0] + 1), rh = uint32_t(dr[3] - dr[1] + 1);
        const VkDeviceSize bytes = VkDeviceSize(rw) * rh * 4;
        VkBuffer staging;
        VmaAllocation sa;
        if (!m_vulkan.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, sa)) continue;
        void* mapped = nullptr;
        vmaMapMemory(m_vulkan.allocator(), sa, &mapped);
        for (uint32_t y = 0; y < rh; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + size_t(y) * rw * 4,
                        &sp.mask[i][((size_t(dr[1]) + y) * sp.w + size_t(dr[0])) * 4], size_t(rw) * 4);
        vmaUnmapMemory(m_vulkan.allocator(), sa);
        // Frames anteriores podem estar lendo a máscara: a barreira os espera.
        m_vulkan.cmdImageBarrier(cmd, sp.gpu[i].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {dr[0], dr[1], 0};
        r.imageExtent = {rw, rh, 1};
        vkCmdCopyBufferToImage(cmd, staging, sp.gpu[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        m_vulkan.cmdImageBarrier(cmd, sp.gpu[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        VmaAllocator allocator = m_vulkan.allocator();
        m_vulkan.deferFrameCleanup([allocator, staging, sa] { vmaDestroyBuffer(allocator, staging, sa); });
        dr[0] = dr[1] = INT32_MAX;
        dr[2] = dr[3] = INT32_MIN;
    }
}

bool Engine::saveSplat(std::string& error) const {
    const SplatEdit& sp = m_splatEdit;
    if (!sp.active || !sp.painted) return true;
    for (int i = 0; i < 2; ++i) {
        const fs::path p = maskPathFor(m_heightEdit.mapName.empty() ? m_currentMapName : m_heightEdit.mapName, i);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (!ImageUtils::writePNG(p.string(), int(sp.w), int(sp.d), 4, sp.mask[i].data())) {
            error = "Could not write " + p.generic_string();
            return false;
        }
    }
    return true;
}

bool Engine::hasSavedSplat(const std::string& map) {
    std::error_code ec;
    return fs::exists(maskPathFor(map, 0), ec);
}

std::vector<std::string> Engine::splatLayerFiles() const {
    std::vector<std::string> out;
    for (int l = 0; l < 8; ++l)
        if (!m_splatEdit.layerFiles[l].empty()) out.push_back(m_splatEdit.layerFiles[l]);
    return out;
}

} // namespace eruption

namespace eruption {

// Camadas pintadas de um mapa vizinho (mundo contínuo): máscaras e texturas
// próprias, ligadas aos vértices do chão dele. Só leitura (pinta-se no mapa aberto).
bool Engine::buildRegionSplat(WorldRegion& r, const std::vector<std::string>& layers) {
    if (layers.empty() || !r.data) return false;
    const TerrainFile& t = r.data->terrain;
    const uint32_t w = t.width, d = t.height;
    const float scale = t.scale > 0.0f ? t.scale : 10.0f;
    std::vector<uint8_t> mask[2];
    for (int i = 0; i < 2; ++i) {
        mask[i].assign(size_t(w) * d * 4, 0);
        ImageData img = loadRawPNG(maskPathFor(r.map, i));
        if (img.isValid()) mask[i] = resampleRGBA(img.pixels.data(), img.width, img.height, img.channels, w, d);
    }
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_vulkan.device(), &si, nullptr, &r.splatSampler);
    for (int i = 0; i < 2; ++i) {
        if (!createMaskImage(m_vulkan, w, d, mask[i].data(), r.splatMask[i])) return false;
        r.splatMask[i].slot = m_bindless.allocateSlot();
        m_bindless.updateTexture(r.splatMask[i].slot, r.splatMask[i].view, r.splatSampler);
    }
    uint32_t layerSlot[8] = {};
    for (size_t l = 0; l < layers.size() && l < 8; ++l) {
        // A camada vem do arquivo de cena com caminho RELATIVO (o mesmo que
        // o jogo exportado usa), entao ela mora no projeto - nao na pasta do
        // motor. Sem resolver aqui, mapa nenhum de projeto pinta o chao.
        const std::filesystem::path camada = project::gameRef(layers[l]);
        ImageData img = loadRawPNG(camada);
        if (!img.isValid()) {
            ERUPTION_LOG_WARN("Terrain layer %s could not be loaded (%s)", layers[l].c_str(),
                              camada.string().c_str());
            continue;
        }
        layerSlot[l] = uploadSplatLayer(r.splatLayer[l], r.splatLayerNormal[l], r.splatLayerMrahw[l], camada, img);
    }
    m_bindless.flushUpdates();
    const uint32_t packed[4] = {layerSlot[0] | (layerSlot[1] << 16), layerSlot[2] | (layerSlot[3] << 16),
                                layerSlot[4] | (layerSlot[5] << 16), layerSlot[6] | (layerSlot[7] << 16)};
    const auto& insts = m_modelRenderer.getInstances();
    std::vector<uint32_t> done;
    for (uint32_t i = r.firstInstance; i < insts.size(); ++i) {
        const uint32_t mesh = insts[i].meshIndex;
        if (!m_modelRenderer.isGroundMesh(mesh) || std::find(done.begin(), done.end(), mesh) != done.end()) continue;
        auto* verts = m_modelRenderer.groundVertices(mesh);
        if (!verts) continue;
        done.push_back(mesh);
        std::vector<uint32_t> all(verts->size());
        for (uint32_t k = 0; k < verts->size(); ++k) {
            TerrainVertex& v = (*verts)[k];
            const Vec3 wp = Vec3(insts[i].transform * Vec4(v.position, 1.0f));
            v.splatTex01 = packed[0];
            v.splatTex23 = packed[1];
            v.splatTex45 = packed[2];
            v.splatTex67 = packed[3];
            v.blendMaskIndex = static_cast<uint16_t>(r.splatMask[0].slot);
            v.blendMaskIndex2 = static_cast<uint16_t>(r.splatMask[1].slot);
            v.blendMaskUV = Vec2(((wp.x - t.offsetX) / scale + 0.5f) / w, ((wp.z - t.offsetZ) / scale + 0.5f) / d);
            all[k] = k;
        }
        m_modelRenderer.patchGroundVertices(mesh, all);
    }
    return !done.empty();
}

void Engine::releaseRegionSplat(WorldRegion& r, bool now) {
    std::vector<TextureResource> texs;
    for (auto& m : r.splatMask) texs.push_back(m);
    for (auto* set : {r.splatLayer, r.splatLayerNormal, r.splatLayerMrahw})
        for (int l = 0; l < 8; ++l) { texs.push_back(set[l]); set[l] = TextureResource{}; }
    for (auto& m : r.splatMask) m = TextureResource{};
    const VkSampler sampler = r.splatSampler;
    r.splatSampler = VK_NULL_HANDLE;
    // O slot volta para a textura padrão já; a imagem só morre depois dos
    // frames em voo (eles ainda podem amostrá-la).
    for (const auto& t : texs)
        if (t.image && t.slot) m_bindless.freeSlot(t.slot);
    m_bindless.flushUpdates();
    auto destroy = [this, texs, sampler] {
        for (const auto& t : texs) {
            if (!t.image) continue;
            vkDestroyImageView(m_vulkan.device(), t.view, nullptr);
            vmaDestroyImage(m_vulkan.allocator(), t.image, t.alloc);
        }
        if (sampler) vkDestroySampler(m_vulkan.device(), sampler, nullptr);
    };
    if (now) destroy();
    else m_vulkan.deferSlotImageCleanup(destroy);
}

} // namespace eruption
