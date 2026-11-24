// Emissiva das imagens do mapa a partir das regioes por textura.
//
// O pacote entrega a emissiva pronta (um PNG por imagem, gerado fora do motor).
// O que o editor desenha por cima vive no arquivo da cena (SceneEdits,
// texture_regions) e e' aplicado aqui, na carga: o nome da textura emissiva de
// cada no' das imagens com regiao troca para "emissive_regions:<imagem>", e a
// textura com esse nome e' montada na CPU a partir do albedo (resolveModelTexture
// chama resolveRegionEmissive). Imagem sem regiao nao e' tocada: o mapa sem
// arquivo carrega byte a byte como antes.
#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "core/Logger.hpp"
#include "scene/SceneEdits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace eruption {

namespace {

constexpr const char* kRegionPrefix = "emissive_regions:";

// Mesma normalizacao de resolveModelTexture: barra invertida, sem barra inicial.
std::string normName(std::string n) {
    std::replace(n.begin(), n.end(), '/', '\\');
    if (!n.empty() && n[0] == '\\') n.erase(0, 1);
    return n;
}

bool hasGlowRegion(const texregion::ImageRegions& e) {
    return std::any_of(e.regions.begin(), e.regions.end(),
                       [](const texregion::Region& r) { return texregion::isGlowType(r.type); });
}

bool hasSchedule(const texregion::Region& r) { return r.onHour >= 0.0f && r.offHour >= 0.0f; }

uint64_t scheduleSignature(const texregion::ImageRegions& e, float hour) {
    uint64_t sig = 0;
    for (size_t i = 0; i < e.regions.size() && i < 64; ++i)
        if (texregion::isGlowType(e.regions[i].type) && texregion::activeAt(e.regions[i], hour)) sig |= 1ull << i;
    return sig;
}

} // namespace

texregion::ImageRegions* Engine::regionEntry(const std::string& image) {
    const std::string n = normName(image);
    for (auto& e : m_texRegions)
        if (normName(e.image) == n) return &e;
    return nullptr;
}

bool Engine::regionLive(const std::string& image) const { return m_regionLive.count(normName(image)) != 0; }

void Engine::loadRegionsForMap(const std::string& map) {
    for (const auto& [key, al] : m_regionAliases)
        if (al.slot != 0) m_bindless.freeSlotSafe(al.slot);
    m_regionAliases.clear();
    m_regionSlots.clear();
    m_regionPack.clear();
    m_regionTris.clear();
    m_regionLampSig.clear();
    m_regionTriCount = 0;
    m_emiFlicker.clear();
    m_texRegions.clear();
    m_emissiveOffNodes.clear();
    m_regionImageNames.clear();
    m_regionNodeNames.clear();
    m_regionLive.clear();
    m_regionSig.clear();
    m_regionAlbedo.clear();
    m_regionSurface.clear();
    m_regionLampSites.clear();
    m_regionLampLights.clear();
    if (!SceneEdits::fileMentions(map, "texture_regions") && !SceneEdits::fileMentions(map, "emissive_off_nodes")) return;
    SceneEdits edits;
    std::string error;
    if (!edits.load(SceneEdits::pathFor(map), error)) {
        ERUPTION_LOG_WARN("Regioes de textura: %s", error.c_str());
        return;
    }
    m_texRegions = std::move(edits.textureRegions);
    m_emissiveOffNodes = std::move(edits.emissiveOffNodes);
    ERUPTION_LOG_WARN("Regioes de textura: %zu imagem(ns), %zu no(s) sem emissiva", m_texRegions.size(),
                      m_emissiveOffNodes.size());
}

namespace {

// Chave da textura de um (imagem, no') no editor: o separador 0x1f nao aparece
// em nome de arquivo nem de no'.
std::string aliasKey(const std::string& image, const std::string& node) {
    return std::string(kRegionPrefix) + normName(image) + '\x1f' + node;
}

bool hasLampRegion(const texregion::ImageRegions& e) {
    return std::any_of(e.regions.begin(), e.regions.end(),
                       [](const texregion::Region& r) { return r.type == texregion::kTypeLamp; });
}

} // namespace

void Engine::patchRegionEmissive(ModelAsset& model) {
    auto hasEmbedded = [this](const std::string& img) {
        if (!m_currentMap) return false;
        const std::string n = normName(img);
        for (const auto& m : m_currentMap->models)
            for (const auto& et : m.embeddedTextures)
                if (et.name == n) return true;
        return false;
    };
    for (auto& node : model.nodes) {
        if (node.vertices.empty()) continue;
        if (!node.name.empty() &&
            std::find(m_regionNodeNames.begin(), m_regionNodeNames.end(), node.name) == m_regionNodeNames.end())
            m_regionNodeNames.push_back(node.name);
        if (node.emissiveTextureNames.size() < node.textureNames.size())
            node.emissiveTextureNames.resize(node.textureNames.size());
        const bool off = std::find(m_emissiveOffNodes.begin(), m_emissiveOffNodes.end(), node.name) != m_emissiveOffNodes.end();
        for (size_t i = 0; i < node.textureNames.size(); ++i) {
            const std::string& img = node.textureNames[i];
            if (img.empty() || img == "default") continue;
            if (std::find(m_regionImageNames.begin(), m_regionImageNames.end(), img) == m_regionImageNames.end())
                m_regionImageNames.push_back(img);
            std::string& emi = node.emissiveTextureNames[i];
            const std::string nimg = normName(img);
            if (m_editorMode) {
                // Editor: toda imagem com pixels passa por uma textura do motor, e
                // cada (imagem, no') tem o seu slot (aliasKey): regiao nova, emissiva
                // do pacote e liga/desliga do no' valem na hora, sem recarregar.
                if (!hasEmbedded(img)) continue;
                if (node.name.empty()) continue;
                if (!emi.empty() && emi.rfind(kRegionPrefix, 0) != 0 && !m_regionPack.count(nimg)) {
                    m_regionPack.emplace(nimg, emi);
#if ERUPTION_DEV
                    // ERUPTION_TEST_LIST_REGION_IMAGES: marca as imagens que vieram com emissiva no pacote.
                    if (std::getenv("ERUPTION_TEST_LIST_REGION_IMAGES")) ERUPTION_LOG_WARN("[REGION_PACK] %s -> %s", img.c_str(), emi.c_str());
#endif
                }
                emi = aliasKey(img, node.name);
                m_regionLive.insert(nimg);
                continue;
            }
            if (off) {
                emi.clear();
                continue;
            }
            const texregion::ImageRegions* e = regionEntry(img);
            const bool routed = e && (hasGlowRegion(*e) || !e->enabled);
            if (routed) {
                if (!e->enabled) {
                    emi.clear(); // desligada no arquivo: sem emissiva nenhuma
                } else {
                    emi = kRegionPrefix + img;
                    m_regionLive.insert(nimg);
                }
            }
        }
        gatherRegionTris(node);
    }
}

// POSTES: centro do UV e do mundo de cada triangulo que usa uma imagem. Fora do
// editor so' das imagens com regiao "lamp" (a malha e' liberada depois da
// carga); no editor, de todas, para uma regiao "lamp" nova achar os postes na
// hora (rebuildRegionLampSites). Teto de 400 mil triangulos guardados.
void Engine::gatherRegionTris(const ModelNode& node) {
    constexpr size_t kMaxTris = 400000;
    if (node.indices.size() < 3 || node.textureNames.empty() || m_regionTriCount >= kMaxTris) return;
    const size_t nt = node.textureNames.size();
    std::vector<std::vector<RegionTri>*> dest(nt, nullptr);
    bool any = false;
    for (size_t t = 0; t < nt; ++t) {
        const std::string& img = node.textureNames[t];
        if (img.empty() || img == "default") continue;
        const texregion::ImageRegions* e = regionEntry(img);
        if (!m_editorMode && !(e && e->enabled && hasLampRegion(*e))) continue;
        dest[t] = &m_regionTris[normName(img)];
        any = true;
    }
    if (!any) return;
    for (size_t t = 0; t + 2 < node.indices.size() && m_regionTriCount < kMaxTris; t += 3) {
        const uint32_t i0 = node.indices[t], i1 = node.indices[t + 1], i2 = node.indices[t + 2];
        if (i0 >= node.vertices.size() || i1 >= node.vertices.size() || i2 >= node.vertices.size()) continue;
        const uint32_t tex = i0 < node.perVertexTexIds.size() ? node.perVertexTexIds[i0] % nt : 0;
        if (!dest[tex]) continue;
        RegionTri tri;
        tri.uv = (node.vertices[i0].texCoord + node.vertices[i1].texCoord + node.vertices[i2].texCoord) / 3.0f;
        const Vec3 local = (node.vertices[i0].position + node.vertices[i1].position + node.vertices[i2].position) / 3.0f;
        tri.pos = Vec3(node.renderMatrix * Vec4(local, 1.0f));
        dest[tex]->push_back(tri);
        ++m_regionTriCount;
    }
}

void Engine::finishRegionLoad() {
    rebuildRegionLampSites(std::string());
    syncRegionLamps();
    if (!m_editorMode) {
        m_regionTris.clear();
        m_regionTriCount = 0;
    }
}

// Onde cada regiao "lamp" aparece no mundo: os triangulos guardados da imagem
// com o centro de UV dentro da regiao, agrupados em celulas de 6 unidades; cada
// celula vira uma luz (o poste inteiro e' uma celula; uma fileira de lampioes,
// varias). So' refaz as imagens cujas regioes "lamp" mudaram (assinatura).
void Engine::rebuildRegionLampSites(const std::string& onlyImage) {
    constexpr float kCell = 6.0f;
    constexpr size_t kMaxSites = 256;
    constexpr int kRes = 128;
    for (const auto& e : m_texRegions) {
        const std::string nimg = normName(e.image);
        if (!onlyImage.empty() && normName(onlyImage) != nimg) continue;
        uint64_t sig = 1469598103934665603ull;
        auto mix = [&sig](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
        mix(e.enabled);
        std::vector<int> lamps;
        for (size_t k = 0; k < e.regions.size(); ++k) {
            const texregion::Region& r = e.regions[k];
            if (r.type != texregion::kTypeLamp) continue;
            lamps.push_back(int(k));
            mix(k);
            mix(uint64_t(r.shape));
            for (float f : r.pts) {
                uint32_t bits;
                std::memcpy(&bits, &f, sizeof(bits));
                mix(bits);
            }
            mix(uint64_t(r.bw) << 20 ^ uint64_t(r.bh));
            for (uint8_t b : r.bitmap) mix(b);
        }
        auto prev = m_regionLampSig.find(nimg);
        if (prev != m_regionLampSig.end() && prev->second == sig) continue;
        m_regionLampSig[nimg] = sig;
        m_regionLampSites.erase(std::remove_if(m_regionLampSites.begin(), m_regionLampSites.end(),
                                               [&](const RegionLampSite& s) { return normName(s.image) == nimg; }),
                                m_regionLampSites.end());
        const auto trisIt = m_regionTris.find(nimg);
        if (!e.enabled || lamps.empty() || trisIt == m_regionTris.end()) continue;
        std::vector<std::vector<float>> cov(lamps.size());
        for (size_t i = 0; i < lamps.size(); ++i) texregion::rasterize(e.regions[size_t(lamps[i])], kRes, kRes, cov[i]);
        struct Acc { Vec3 sum{0.0f}; int n = 0; };
        std::vector<std::unordered_map<uint64_t, Acc>> cells(lamps.size());
        for (const RegionTri& tri : trisIt->second) {
            const int x = std::min(kRes - 1, int((tri.uv.x - std::floor(tri.uv.x)) * kRes));
            const int y = std::min(kRes - 1, int((tri.uv.y - std::floor(tri.uv.y)) * kRes));
            for (size_t r = 0; r < lamps.size(); ++r) {
                if (cov[r][size_t(y) * kRes + size_t(x)] < 0.5f) continue;
                const int cx = int(std::floor(tri.pos.x / kCell)), cy = int(std::floor(tri.pos.y / kCell)),
                          cz = int(std::floor(tri.pos.z / kCell));
                const uint64_t key = (uint64_t(uint32_t(cx) & 0xFFFFF) << 40) | (uint64_t(uint32_t(cy) & 0xFFFFF) << 20) |
                                     uint64_t(uint32_t(cz) & 0xFFFFF);
                Acc& a = cells[r][key];
                a.sum += tri.pos;
                ++a.n;
                break; // a primeira regiao que cobre ganha
            }
        }
        for (size_t r = 0; r < lamps.size(); ++r)
            for (const auto& [key, a] : cells[r]) {
                if (m_regionLampSites.size() >= kMaxSites) break;
                RegionLampSite site;
                site.pos = a.sum / float(a.n);
                site.image = e.image;
                site.region = lamps[r];
                m_regionLampSites.push_back(std::move(site));
            }
    }
    if (!m_regionLampSites.empty())
        ERUPTION_LOG_WARN("Regioes de textura: %zu luz(es) de poste achadas", m_regionLampSites.size());
}

// Refaz as luzes dos postes a partir dos pontos achados e dos valores atuais
// da regiao (cor, raio, intensidade, horario, cintilacao): mexer neles no
// editor vale na hora.
void Engine::syncRegionLamps() {
    std::vector<PointLight> lights;
    for (const auto& site : m_regionLampSites) {
        const texregion::ImageRegions* e = nullptr;
        for (const auto& r : m_texRegions)
            if (normName(r.image) == normName(site.image)) e = &r;
        if (!e || !e->enabled || site.region >= int(e->regions.size())) continue;
        const texregion::Region& r = e->regions[size_t(site.region)];
        if (r.type != texregion::kTypeLamp) continue;
        PointLight pl;
        pl.position = site.pos;
        pl.color = Vec3(r.color[0], r.color[1], r.color[2]);
        pl.intensity = r.lightIntensity;
        pl.radius = r.lightRadius;
        pl.nightOnly = true;
        pl.enabled = false; // updateNightLights decide a cada quadro
        pl.onHour = r.onHour;
        pl.offHour = r.offHour;
        pl.flickerAmp = r.flicker;
        pl.flickerSpeed = r.flickerSpeed;
        pl.glowRadius = r.glowRadius;
        pl.glowStrength = r.glowStrength;
        lights.push_back(pl);
    }
    m_regionLampLights = std::move(lights);
    rebuildWorldLights();
}

std::string Engine::regionSurfaceAt(const std::string& image, const Vec2& uv) {
    if (m_texRegions.empty()) return std::string();
    const std::string n = normName(image);
    auto it = m_regionSurface.find(n);
    if (it == m_regionSurface.end()) {
        RegionSurfaceMap m;
        const texregion::ImageRegions* e = nullptr;
        for (const auto& r : m_texRegions)
            if (normName(r.image) == n) e = &r;
        if (e && e->enabled) {
            m.types = texregion::surfaceTypes(*e);
            if (!m.types.empty()) {
                m.w = m.h = 256;
                m.id.assign(size_t(m.w) * m.h, 0);
                std::vector<float> cov;
                for (const auto& r : e->regions) { // a ultima regiao desenhada ganha
                    if (!texregion::isSurfaceType(r.type)) continue;
                    const auto t = std::find(m.types.begin(), m.types.end(), r.type);
                    texregion::rasterize(r, m.w, m.h, cov);
                    for (size_t i = 0; i < cov.size(); ++i)
                        if (cov[i] > 0.5f) m.id[i] = uint8_t(1 + (t - m.types.begin()));
                }
            }
        }
        it = m_regionSurface.emplace(n, std::move(m)).first;
    }
    const RegionSurfaceMap& m = it->second;
    if (m.id.empty()) return std::string();
    const int x = std::clamp(int(uv.x * m.w), 0, m.w - 1), y = std::clamp(int(uv.y * m.h), 0, m.h - 1);
    const uint8_t id = m.id[size_t(y) * m.w + size_t(x)];
    return id ? m.types[id - 1] : std::string();
}

bool Engine::regionAlbedo(const std::string& image, const uint8_t*& px, int& w, int& h, int& ch) {
    const std::string n = normName(image);
    auto it = m_regionAlbedo.find(n);
    if (it == m_regionAlbedo.end()) {
        RegionAlbedo a;
        a.ok = findEmbeddedAlbedoPixels(n, a.px, a.w, a.h, a.ch) && a.w > 0 && a.h > 0 && !a.px.empty();
        it = m_regionAlbedo.emplace(n, std::move(a)).first;
    }
    if (!it->second.ok) return false;
    px = it->second.px.data();
    w = it->second.w;
    h = it->second.h;
    ch = it->second.ch;
    return true;
}

uint32_t Engine::uploadRegionEmissive(const std::string& key, const uint8_t* rgba, int w, int h) {
    TextureResource tr;
    if (!createTextureFromPixels(rgba, w, h, tr)) return 0;
    const uint32_t mips = m_mipmapMenu.config.enabled ? MipmapGenerator::calculateMipLevels(w, h) : 1;
    ModelTextureGPU old;
    const bool had = m_assetCache.getModelTexture(key, old) && old.bindlessSlot != 0;
    const uint32_t slot = had ? old.bindlessSlot : m_bindless.allocateSlotSafe();
    if (slot != 0) m_bindless.updateTextureSafe(slot, tr.view, m_defaultSampler);
    ModelTextureGPU tex{};
    tex.image = tr.image;
    tex.alloc = tr.alloc;
    tex.view = tr.view;
    tex.bindlessSlot = slot;
    tex.width = w;
    tex.height = h;
    tex.mipLevels = mips;
    m_assetCache.addModelTexture(key, tex);
    if (key.rfind(kRegionPrefix, 0) == 0) {
        const std::string image = key.substr(std::strlen(kRegionPrefix));
        auto& slots = m_regionSlots[image];
        if (slot != 0 && std::find(slots.begin(), slots.end(), slot) == slots.end()) slots.push_back(slot);
        rebindRegionAliases(image);
    }
    if (had && old.image != VK_NULL_HANDLE) {
        // A textura velha ainda pode estar em quadros no ar: so' sai depois deles.
        VkDevice dev = m_vulkan.device();
        VmaAllocator allocator = m_vulkan.allocator();
        const ModelTextureGPU dead = old;
        m_vulkan.deferSlotImageCleanup([dev, allocator, dead] {
            if (dead.view != VK_NULL_HANDLE) vkDestroyImageView(dev, dead.view, nullptr);
            vmaDestroyImage(allocator, dead.image, dead.alloc);
        });
    }
    return slot;
}

uint32_t Engine::resolveRegionEmissive(const std::string& key) {
    const size_t sep = key.find('\x1f');
    if (sep != std::string::npos) {
        // Slot proprio do (imagem, no'): aponta para a emissiva da imagem, ou
        // para uma preta quando o no' esta' desligado.
        auto it = m_regionAliases.find(key);
        if (it != m_regionAliases.end()) return it->second.slot;
        RegionAlias al;
        al.image = key.substr(std::strlen(kRegionPrefix), sep - std::strlen(kRegionPrefix));
        al.node = key.substr(sep + 1);
        const VkImageView view = regionAliasView(al.image, al.node);
        if (view == VK_NULL_HANDLE) return 0;
        al.slot = m_bindless.allocateSlotSafe();
        if (al.slot == 0) return 0;
        m_bindless.updateTextureSafe(al.slot, view, m_defaultSampler);
        m_regionSlots[al.image].push_back(al.slot);
        const uint32_t slot = al.slot;
        m_regionAliases.emplace(key, std::move(al));
        return slot;
    }
    const std::string image = key.substr(std::strlen(kRegionPrefix));
    const texregion::ImageRegions* e = regionEntry(image);
    const uint8_t* px = nullptr;
    int w = 0, h = 0, ch = 4;
    std::vector<uint8_t> rgb;
    const bool glow = e && hasGlowRegion(*e);
    if (e && !e->enabled) {
        w = h = 4; // "Glow on this texture" desligado: sem emissiva nenhuma
        rgb.assign(size_t(w) * h * 3, 0);
    } else if (e && glow && regionAlbedo(image, px, w, h, ch)) {
        texregion::buildEmissive(px, w, h, ch, *e, regionHour(), rgb);
    } else {
        w = h = 4;
        rgb.assign(size_t(w) * h * 3, 0);
    }
    // Quem esta' aceso agora: updateRegionSchedule so' refaz quando isto muda
    // (tambem sem pixels, para nao refazer a cada quadro).
    if (e) m_regionSig[normName(image)] = scheduleSignature(*e, regionHour());
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 255);
    for (size_t i = 0; i < size_t(w) * h; ++i) std::memcpy(&rgba[i * 4], &rgb[i * 3], 3);
    return uploadRegionEmissive(key, rgba.data(), w, h);
}

VkImageView Engine::regionBlackView() {
    static const char* kBlack = "emissive_regions_black";
    ModelTextureGPU t;
    if (!m_assetCache.getModelTexture(kBlack, t)) {
        std::vector<uint8_t> px(4 * 4 * 4, 0);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        uploadRegionEmissive(kBlack, px.data(), 4, 4);
        if (!m_assetCache.getModelTexture(kBlack, t)) return VK_NULL_HANDLE;
    }
    return t.view;
}

// Que imagem o slot de um (imagem, no') mostra: preta se o no' esta' desligado
// ou a textura desligada; a emissiva montada das regioes quando ha' regiao de
// brilho; senao a emissiva que veio no pacote (a mesma textura de antes, sem
// copia) ou, sem nenhuma, preta.
VkImageView Engine::regionAliasView(const std::string& image, const std::string& node) {
    if (std::find(m_emissiveOffNodes.begin(), m_emissiveOffNodes.end(), node) != m_emissiveOffNodes.end())
        return regionBlackView();
    const texregion::ImageRegions* e = regionEntry(image);
    if (e && !e->enabled) return regionBlackView();
    ModelTextureGPU t;
    if (e && hasGlowRegion(*e)) {
        const std::string shared = std::string(kRegionPrefix) + normName(image);
        if (!m_assetCache.getModelTexture(shared, t)) {
            resolveRegionEmissive(shared);
            if (!m_assetCache.getModelTexture(shared, t)) return regionBlackView();
        }
        return t.view;
    }
    const auto pack = m_regionPack.find(normName(image));
    if (pack != m_regionPack.end()) {
        resolveModelTexture(pack->second); // sobe a emissiva do pacote se ainda nao subiu
        if (m_assetCache.getModelTexture(normName(pack->second), t) && t.view != VK_NULL_HANDLE) return t.view;
    }
    return regionBlackView();
}

// As regioes ou o no' mudaram (ou a emissiva montada foi refeita, view nova):
// os slots dos nos da imagem seguem.
void Engine::rebindRegionAliases(const std::string& image) {
    for (const auto& [key, al] : m_regionAliases) {
        if (al.image != image || al.slot == 0) continue;
        const VkImageView view = regionAliasView(al.image, al.node);
        if (view != VK_NULL_HANDLE) m_bindless.updateTextureSafe(al.slot, view, m_defaultSampler);
    }
}

void Engine::setRegionNodeEmissive(const std::string& node, bool on) {
    auto it = std::find(m_emissiveOffNodes.begin(), m_emissiveOffNodes.end(), node);
    if (on && it != m_emissiveOffNodes.end()) m_emissiveOffNodes.erase(it);
    else if (!on && it == m_emissiveOffNodes.end()) m_emissiveOffNodes.push_back(node);
    std::vector<std::string> images;
    for (const auto& [key, al] : m_regionAliases)
        if (al.node == node && std::find(images.begin(), images.end(), al.image) == images.end()) images.push_back(al.image);
    for (const auto& image : images) rebindRegionAliases(image);
}

void Engine::refreshRegionEmissive(const std::string& image) {
    m_regionSurface.clear(); // qualquer edicao pode ter mexido nas regioes de superficie
    rebuildRegionLampSites(image);
    syncRegionLamps(); // ...ou nos valores das luzes dos postes
    const std::string key = std::string(kRegionPrefix) + normName(image);
    ModelTextureGPU cur;
    const bool built = m_assetCache.getModelTexture(key, cur);
    const texregion::ImageRegions* e = regionEntry(image);
    if (built || (m_editorMode && e && hasGlowRegion(*e))) resolveRegionEmissive(key); // refaz (e rebinda os nos)
    rebindRegionAliases(normName(image));
}

// Cintilacao da emissiva: um fator por imagem (a regiao acesa de maior
// cintilacao), mesma curva das luzes do mundo. A tabela (slot, fator) vai no
// UBO do quadro e o shader multiplica a emissiva - a textura nao e' refeita.
void Engine::updateEmissiveFlicker() {
    m_emiFlicker.clear();
    if (m_texRegions.empty()) return;
    const float now = static_cast<float>(m_timer.elapsed());
    const float hour = regionHour();
    for (const auto& e : m_texRegions) {
        if (!e.enabled) continue;
        float amp = 0.0f, speed = 6.0f;
        for (const auto& r : e.regions)
            if (texregion::isGlowType(r.type) && r.flicker > amp && texregion::activeAt(r, hour)) {
                amp = r.flicker;
                speed = r.flickerSpeed;
            }
        if (amp <= 0.0f) continue;
        const auto slots = m_regionSlots.find(normName(e.image));
        if (slots == m_regionSlots.end()) continue;
        const float ph = float(std::hash<std::string>{}(e.image) & 0xFFFFu) * 0.001f;
        const float a = std::sin(now * speed * 6.2831853f + ph);
        const float b = std::sin(now * speed * 14.9f + ph * 1.7f);
        const float mul = 1.0f - amp * (0.5f + 0.3f * a + 0.2f * b);
        for (uint32_t s : slots->second)
            if (m_emiFlicker.size() < 64) m_emiFlicker.emplace_back(s, mul);
    }
}

// Cintilacao da emissiva e brilho dos objetos no UBO do quadro (so' o que
// o model_frag.glsl le'). Sem nada disso os campos ficam em 0 e o shader nao
// faz nenhum trabalho extra.
void Engine::fillEmissiveFrame(FrameUBO& ubo) {
    updateEmissiveFlicker();
    ubo.emiFlickerInfo = Vec4(float(m_emiFlicker.size()), 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 32; ++i) ubo.emiFlicker[i] = Vec4(0.0f);
    for (size_t i = 0; i < m_emiFlicker.size(); ++i) {
        Vec4& v = ubo.emiFlicker[i >> 1];
        const Vec2 pr(float(m_emiFlicker[i].first), m_emiFlicker[i].second);
        if (i & 1) { v.z = pr.x; v.w = pr.y; }
        else { v.x = pr.x; v.y = pr.y; }
    }
    // Luzes com brilho de objeto: as 8 mais perto da camera entre as acesas.
    struct Cand { float d; const PointLight* pl; };
    std::vector<Cand> cand;
    const Vec3 cam = m_camera.position();
    auto consider = [&](const PointLight& pl) {
        if (pl.glowRadius <= 0.0f || pl.glowStrength <= 0.0f || !pl.enabled) return;
        cand.push_back({glm::distance(pl.position, cam) - pl.glowRadius, &pl});
    };
    for (const auto& pl : m_deferredLighting.getPointLights()) consider(pl);
    for (const auto& pl : m_regionLampLights) consider(pl);
    const size_t n = std::min<size_t>(8, cand.size());
    std::partial_sort(cand.begin(), cand.begin() + long(n), cand.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    ubo.glowInfo = Vec4(float(n), 0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i < 8; ++i) {
        if (i < n) {
            const PointLight& pl = *cand[i].pl;
            ubo.glowPos[i] = Vec4(pl.position, pl.glowRadius);
            ubo.glowColor[i] = Vec4(pl.color, pl.glowStrength * pl.flickerMul);
        } else {
            ubo.glowPos[i] = ubo.glowColor[i] = Vec4(0.0f);
        }
    }
}

void Engine::updateRegionSchedule() {
    const float hour = regionHour();
    for (const auto& e : m_texRegions) {
        const std::string n = normName(e.image);
        if (!m_regionLive.count(n)) continue;
        if (!std::any_of(e.regions.begin(), e.regions.end(), [](const texregion::Region& r) {
                return texregion::isGlowType(r.type) && hasSchedule(r);
            }))
            continue;
        const uint64_t sig = scheduleSignature(e, hour);
        auto it = m_regionSig.find(n);
        if (it != m_regionSig.end() && it->second == sig) continue;
        refreshRegionEmissive(e.image);
    }
}

} // namespace eruption
