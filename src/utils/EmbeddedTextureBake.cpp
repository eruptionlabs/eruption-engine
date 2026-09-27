#include "utils/EmbeddedTextureBake.hpp"

#include "core/JobSystem.hpp"
#include "core/Logger.hpp"
#include "formats/ModelFile.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "utils/PbrTextureLoader.hpp"

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace eruption {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kPackMagic = 0x504b5445; // 'ETKP'
// Bump when the encoder or the synthesis changes so old caches are ignored.
constexpr uint32_t kPackVersion = 3;

void put32(std::vector<uint8_t>& v, uint32_t x) {
    const size_t o = v.size();
    v.resize(o + 4);
    std::memcpy(v.data() + o, &x, 4);
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    const size_t o = v.size();
    v.resize(o + 8);
    std::memcpy(v.data() + o, &x, 8);
}

struct Reader {
    const uint8_t* d;
    size_t n;
    size_t c = 0;
    bool u32(uint32_t& x) {
        if (c + 4 > n) return false;
        std::memcpy(&x, d + c, 4); c += 4; return true;
    }
    bool u64(uint64_t& x) {
        if (c + 8 > n) return false;
        std::memcpy(&x, d + c, 8); c += 8; return true;
    }
    bool f32(float& x) {
        if (c + 4 > n) return false;
        std::memcpy(&x, d + c, 4); c += 4; return true;
    }
    bool str(std::string& s) {
        uint32_t len = 0;
        if (!u32(len) || len > (1u << 20) || c + len > n) return false;
        s.assign(reinterpret_cast<const char*>(d + c), len);
        c += len;
        return true;
    }
};

// Máscara de diagnóstico: ERUPTION_BC_MASK=<bits> — 1 albedo, 2 normal,
// 4 mrahw. Default 7 (tudo). Serve para atribuir uma regressão visual ao mapa
// certo em vez de chutar; entra na chave do cache para não misturar bakes.
uint32_t bcMask() {
    static const uint32_t m = []() -> uint32_t {
        if (const char* e = std::getenv("ERUPTION_BC_MASK")) {
            return static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) & 7u;
        }
        return 7u;
    }();
    return m;
}

uint32_t formatKey() {
    const BcFormatSupport& s = bcFormatSupport();
    return (s.bc1 ? 1u : 0u) | (s.bc3 ? 2u : 0u) | (s.bc5 ? 4u : 0u) | (s.bc7 ? 8u : 0u)
         | (bcMask() << 4);
}

// O pack e' chaveado por maxSize e por sintese de PBR, mas era gravado num
// caminho UNICO. Resultado: trocar de preset (low usa 512, high usa 1024)
// invalidava o cache e recomprimia TUDO - medido em parana_field: 23,8 s de
// re-bake por troca de preset. Agora cada combinacao tem o seu arquivo, entao
// alternar entre presets so' paga na primeira vez de cada um.
std::string packPathFor(const std::string& glbPath, uint32_t maxSize, bool synthPbr) {
    return glbPath + "." + std::to_string(maxSize) + (synthPbr ? "s" : "n") + ".etexpack";
}
// Caminho historico, sem sufixo. Continua sendo LIDO para nao invalidar packs
// que ja' existem no disco de quem atualiza a engine - o cache e' artefato
// derivado, mas re-gerar custa 24 s num mapa grande.
std::string legacyPackPathFor(const std::string& glbPath) { return glbPath + ".etexpack"; }

bool readCache(const std::string& glbPath, uint32_t maxSize, bool synthPbr,
               EmbeddedBakeResult& out) {
    std::error_code ec;
    std::string path = packPathFor(glbPath, maxSize, synthPbr);
    if (!fs::exists(path, ec)) {
        // Compatibilidade: pack antigo sem sufixo. O cabecalho carrega
        // maxSize/synthPbr, entao a validacao abaixo rejeita se nao bater.
        path = legacyPackPathFor(glbPath);
        if (!fs::exists(path, ec)) return false;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 40) return false;

    Reader r{buf.data(), buf.size()};
    uint32_t magic = 0, version = 0, cachedMax = 0, cachedFmt = 0, cachedSynth = 0, count = 0;
    uint64_t srcSize = 0, srcMtime = 0;
    if (!r.u32(magic) || magic != kPackMagic) return false;
    if (!r.u32(version) || version != kPackVersion) return false;
    if (!r.u64(srcSize) || !r.u64(srcMtime)) return false;
    if (!r.u32(cachedMax) || !r.u32(cachedFmt) || !r.u32(cachedSynth) || !r.u32(count)) return false;

    // Invalidate on: source edited, resolution cap changed (preset), GPU format
    // set changed, PBR synthesis toggled.
    const uint64_t realSize = static_cast<uint64_t>(fs::file_size(glbPath, ec));
    if (ec || realSize != srcSize) return false;
    const auto mt = fs::last_write_time(glbPath, ec);
    if (ec) return false;
    const uint64_t realMtime =
        static_cast<uint64_t>(mt.time_since_epoch().count());
    if (realMtime != srcMtime) return false;
    if (cachedMax != maxSize || cachedFmt != formatKey() ||
        cachedSynth != (synthPbr ? 1u : 0u))
        return false;

    uint32_t hasGround = 0;
    if (!r.u32(hasGround)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!r.f32(out.groundAlbedo[i])) return false;
    }
    out.hasGroundAlbedo = hasGround != 0;

    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        if (!r.str(name)) return false;
        uint32_t mask = 0;
        if (!r.u32(mask)) return false;
        BakedEmbeddedTexture t;
        if (mask & 1u) { if (!readEtexBytes(r.d, r.n, r.c, t.albedo)) return false; }
        if (mask & 2u) { if (!readEtexBytes(r.d, r.n, r.c, t.normal)) return false; }
        if (mask & 4u) { if (!readEtexBytes(r.d, r.n, r.c, t.mrahw)) return false; }
        out.bytes += t.albedo.byteSize() + t.normal.byteSize() + t.mrahw.byteSize();
        out.textures.emplace(std::move(name), std::move(t));
    }
    out.fromCache = true;
    return true;
}

void writeCache(const std::string& glbPath, uint32_t maxSize, bool synthPbr,
                const EmbeddedBakeResult& res) {
    std::error_code ec;
    const uint64_t srcSize = static_cast<uint64_t>(fs::file_size(glbPath, ec));
    if (ec) return;
    const auto mt = fs::last_write_time(glbPath, ec);
    if (ec) return;

    std::vector<uint8_t> buf;
    buf.reserve(res.bytes + 4096);
    put32(buf, kPackMagic);
    put32(buf, kPackVersion);
    put64(buf, srcSize);
    put64(buf, static_cast<uint64_t>(mt.time_since_epoch().count()));
    put32(buf, maxSize);
    put32(buf, formatKey());
    put32(buf, synthPbr ? 1u : 0u);
    put32(buf, static_cast<uint32_t>(res.textures.size()));
    put32(buf, res.hasGroundAlbedo ? 1u : 0u);
    for (int i = 0; i < 3; ++i) {
        const size_t o = buf.size();
        buf.resize(o + 4);
        std::memcpy(buf.data() + o, &res.groundAlbedo[i], 4);
    }
    for (const auto& [name, t] : res.textures) {
        put32(buf, static_cast<uint32_t>(name.size()));
        buf.insert(buf.end(), name.begin(), name.end());
        uint32_t mask = 0;
        if (t.albedo.valid()) mask |= 1u;
        if (t.normal.valid()) mask |= 2u;
        if (t.mrahw.valid()) mask |= 4u;
        put32(buf, mask);
        if (mask & 1u) appendEtexBytes(t.albedo, buf);
        if (mask & 2u) appendEtexBytes(t.normal, buf);
        if (mask & 4u) appendEtexBytes(t.mrahw, buf);
    }

    // Write to a temp file and rename: a crash mid-bake must not leave a
    // truncated cache that the next run would happily read.
    const std::string finalPath = packPathFor(glbPath, maxSize, synthPbr);
    const std::string tmpPath = finalPath + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            ERUPTION_LOG_WARN("EmbeddedTextureBake: cannot write cache '%s'", tmpPath.c_str());
            return;
        }
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    fs::rename(tmpPath, finalPath, ec);
    if (ec) fs::remove(tmpPath, ec);
}

} // namespace

namespace {
std::atomic<uint32_t> g_maxSize{1024};
}
void setEmbeddedTextureMaxSize(uint32_t maxSize) { g_maxSize.store(maxSize); }
uint32_t embeddedTextureMaxSize() { return g_maxSize.load(); }

namespace {

// Quanto cada textura contribui para a cor da luz que o chao devolve.
//
// Esta media alimenta DOIS consumidores reais: o hemisferio inferior do
// ambiente e o termo de "GI de um bounce" do passe direcional. E' literalmente
// a cor da luz que sobe do chao e ilumina a parte de baixo de tudo.
//
// Era a media de TODAS as texturas embutidas com peso IGUAL - ceu, agua, metal,
// parede, interface. Num mapa vulcanico, vermelho e marrom no chao, o resultado
// saia cinza-arroxeado, com o azul puxando a media para longe do que o chao
// realmente e'.
//
// O criterio agora e' o fisicamente correto e nao depende de autoria nenhuma:
// AREA VOLTADA PARA CIMA. Luz refletida difusa sobe de superficie horizontal;
// parede vertical devolve para o lado e teto/domo de ceu nao devolve nada para
// cima. Uma textura usada so' em parede ou no domo do ceu recebe peso ~0
// sozinha, sem ninguem precisar marca-la.
//
// A normal e' tomada em espaco de modelo. Para terreno de mapa isso coincide
// com o mundo; um asset girado no proprio no' seria mal classificado, mas
// terreno nao e' girado e e' ele que domina a area.
std::unordered_map<std::string, double> computeUpFacingArea(const ModelFile& model) {
    std::unordered_map<std::string, double> area;

    for (const auto& node : model.nodes) {
        if (node.vertices.empty() || node.indices.size() < 3) continue;
        if (node.textureNames.empty()) continue;

        // Subamostra: a cor media nao precisa de todo triangulo, e mapas
        // grandes tem milhoes deles. 1 em cada 7 ja' estabiliza a media.
        const size_t triCount = node.indices.size() / 3;
        const size_t stride = (triCount > 20000) ? 7 : 1;

        for (size_t t = 0; t < triCount; t += stride) {
            const uint32_t i0 = node.indices[t * 3 + 0];
            const uint32_t i1 = node.indices[t * 3 + 1];
            const uint32_t i2 = node.indices[t * 3 + 2];
            if (i0 >= node.vertices.size() || i1 >= node.vertices.size() ||
                i2 >= node.vertices.size()) continue;

            const Vec3& a = node.vertices[i0];
            const Vec3& b = node.vertices[i1];
            const Vec3& c = node.vertices[i2];
            const Vec3 cross = glm::cross(b - a, c - a);
            const float len = glm::length(cross);
            if (len <= 1e-8f) continue;

            // O Y da normal E' o cosseno com a vertical, entao o produto
            // (area x cosseno) e' exatamente a projecao horizontal do
            // triangulo - a grandeza que governa quanta luz ele manda para
            // cima. Sai de graca: len/2 * (cross.y/len) = cross.y/2.
            const float upward = cross.y * 0.5f;
            if (upward <= 0.0f) continue; // virado para baixo ou vertical

            uint16_t texLocal = 0;
            if (!node.perVertexTexIds.empty() && i0 < node.perVertexTexIds.size())
                texLocal = node.perVertexTexIds[i0];
            if (texLocal >= node.textureNames.size()) continue;

            area[node.textureNames[texLocal]] += double(upward) * double(stride);
        }
    }
    return area;
}

} // namespace

EmbeddedBakeResult bakeEmbeddedTextures(const ModelFile& model, uint32_t maxTextureSize,
                                        bool synthesizePbr) {
    EmbeddedBakeResult res;
    if (model.embeddedTextures.empty()) return res;
    if (!bcFormatSupport().any()) {
        // No block-compressed format on this GPU (e.g. a mobile/Android build):
        // keep the plain RGBA8 path, nothing to bake.
        return res;
    }
    bcCompressorInit();

    if (!model.filePath.empty() &&
        readCache(model.filePath, maxTextureSize, synthesizePbr, res)) {
        ERUPTION_LOG_WARN("EmbeddedTextureBake: cache HIT '%s' (%zu texturas, %.1f MB comprimidos)",
                          model.filePath.c_str(), res.textures.size(),
                          static_cast<double>(res.bytes) / (1024.0 * 1024.0));
        return res;
    }

    // Peso de cada textura na cor de bounce: area voltada para cima.
    const auto upArea = computeUpFacingArea(model);
    double maxUpArea = 0.0;
    for (const auto& kv : upArea) maxUpArea = std::max(maxUpArea, kv.second);
    auto textureGroundWeight = [&](const std::string& texName) -> float {
        // Agua e metal ficam de fora mesmo sendo horizontais: sao praticamente
        // especulares e refletem a CENA, nao uma cor difusa propria. Entrar na
        // media so' tinge tudo de azul ou cinza.
        const std::string& cat = getPbrProfile(texName).category;
        if (cat == "water" || cat == "metal") return 0.0f;

        if (maxUpArea <= 0.0) return 1.0f; // sem geometria util: media antiga
        const auto it = upArea.find(texName);
        if (it == upArea.end()) return 0.0f;
        // Raiz da fracao de area: uma unica textura de chao dominante nao
        // apaga completamente as demais, mas parede e ceu continuam perto de 0.
        return static_cast<float>(std::sqrt(it->second / maxUpArea));
    };

    const auto t0 = std::chrono::steady_clock::now();
    const size_t n = model.embeddedTextures.size();
    std::vector<BakedEmbeddedTexture> baked(n);
    std::vector<std::string> names(n);
    // Ground-albedo accumulation happens here because this is the only place
    // the decoded pixels exist once the RGBA8 path is gone.
    std::vector<double> sumR(n, 0.0), sumG(n, 0.0), sumB(n, 0.0), sumN(n, 0.0);
    std::atomic<size_t> compressed{0};

    JobSystem::instance().parallelFor(static_cast<uint32_t>(n), [&](uint32_t i) {
        const EmbeddedTexture& et = model.embeddedTextures[i];
        names[i] = et.name;

        std::vector<uint8_t> pixels;
        int w = 0, h = 0, ch = 4;
        if (et.isDecoded()) {
            pixels = et.pixels;
            w = et.width; h = et.height; ch = et.channels > 0 ? et.channels : 4;
        } else if (et.hasEncodedData()) {
            ImageData img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(), et.encodedData.size());
            if (!img.isValid()) return;
            pixels = std::move(img.pixels);
            w = img.width; h = img.height; ch = img.channels > 0 ? img.channels : 4;
        } else {
            return;
        }
        if (ch != 4 || w <= 0 || h <= 0) return;

        // Resolution cap (preset): the 2048^2 sheets are most of the pixels.
        ImageUtils::downscaleRGBA(w, h, pixels, static_cast<int>(maxTextureSize));

        // --- Cor da luz refletida pelo chao ---------------------------------
        // Esta media alimenta DOIS consumidores reais: o hemisferio inferior do
        // ambiente e o termo de "GI de um bounce" do passe direcional. Ou seja,
        // e' literalmente a cor da luz que sobe do chao e ilumina a parte de
        // baixo de tudo.
        //
        // Ela era a media de TODAS as texturas embutidas do GLB, com peso
        // igual: ceu, agua, metal, interface, sprite, tudo. Num mapa vulcanico
        // - vermelho e marrom no chao - o resultado saia cinza-arroxeado,
        // porque o azul da agua e o cinza do metal puxavam a media para longe
        // do que o chao realmente e'.
        //
        // Agora pesa por CATEGORIA do material, que ja' esta' autorada em
        // assets/data/pbr_materials.json. O criterio e' fisico: so' contribui
        // quem de fato devolve luz DIFUSA para cima.
        const float weight = textureGroundWeight(et.name);
        if (weight > 0.0f) {
            for (size_t p = 0; p + 1 < static_cast<size_t>(w) * h; p += 97) {
                const uint8_t* px = &pixels[p * 4];
                if (px[0] > 230 && px[1] < 25 && px[2] > 230) continue; // chroma key magenta
                if (px[0] + px[1] + px[2] < 24) continue;               // preto = vazio
                sumR[i] += px[0] * weight; sumG[i] += px[1] * weight;
                sumB[i] += px[2] * weight; sumN[i] += weight;
            }
        }

        bool hasAlpha = false;
        for (size_t p = 3; p < pixels.size(); p += 4) {
            if (pixels[p] < 250) { hasAlpha = true; break; }
        }
        if (bcMask() & 1u) {
            baked[i].albedo = compressRgbaToEtex(pixels.data(), w, h, BcKind::Albedo, hasAlpha);
        }

        if (synthesizePbr) {
            PbrTextureData mrahw, normal;
            if (synthesizePbrFromPixels(pixels, w, h, 4, mrahw, normal) &&
                mrahw.valid() && normal.valid()) {
                // Normal: BC7, not BC5. This engine's normal maps are NOT unit
                // length (flat blue ~239) and their alpha carries materialProps,
                // so the two-channel BC5 + z-reconstruction convention would
                // change the look. BC7 keeps all four channels at the same
                // 1 byte/texel, and is still 4x smaller than RGBA8.
                if (bcMask() & 2u) {
                    baked[i].normal = compressRgbaToEtex(normal.pixels.data(), normal.width,
                                                         normal.height, BcKind::Normal);
                }
                if (bcMask() & 4u) {
                    // Variancia da normal por mip (Toksvig) - broadening de
                    // roughness. Calculada mesmo que o normal map em si NAO
                    // seja BC-comprimido (bcMask bit 2 desligado): e' so'
                    // matematica sobre os pixels que ja' estao em memoria,
                    // nao depende do resultado comprimido.
                    auto normalVariance = computeNormalMipVariance(normal.pixels.data(),
                                                                    normal.width, normal.height);
                    // ERUPTION_DEBUG_TOKSVIG=1: confirma que a roughness
                    // sintetizada deixou de ser constante (204 = ~0.8 fixo)
                    // e passou a variar por mip - sem isso e' dificil ver o
                    // efeito olhando so' o mip0 de perto (r=1.0 sempre ali).
                    static const bool kDebugToksvig = std::getenv("ERUPTION_DEBUG_TOKSVIG") != nullptr;
                    if (kDebugToksvig) {
                        for (size_t lvl = 0; lvl < normalVariance.size(); ++lvl) {
                            double sumR = 0.0;
                            for (float v : normalVariance[lvl]) sumR += v;
                            const double avgR = sumR / std::max<size_t>(1, normalVariance[lvl].size());
                            ERUPTION_LOG_WARN("[TOKSVIG] %s mip%zu r_medio=%.4f (n=%zu)",
                                              et.name.c_str(), lvl, avgR, normalVariance[lvl].size());
                        }
                    }
                    baked[i].mrahw = compressRgbaToEtex(mrahw.pixels.data(), mrahw.width,
                                                        mrahw.height, BcKind::Data,
                                                        /*hasAlpha=*/false, &normalVariance);
                }
            }
        }
        compressed.fetch_add(1, std::memory_order_relaxed);
    });

    double r = 0, g = 0, b = 0, cnt = 0;
    for (size_t i = 0; i < n; ++i) { r += sumR[i]; g += sumG[i]; b += sumB[i]; cnt += sumN[i]; }
    if (cnt > 100) {
        res.groundAlbedo[0] = static_cast<float>(r / cnt) / 255.0f;
        res.groundAlbedo[1] = static_cast<float>(g / cnt) / 255.0f;
        res.groundAlbedo[2] = static_cast<float>(b / cnt) / 255.0f;
        res.hasGroundAlbedo = true;
    }

    for (size_t i = 0; i < n; ++i) {
        if (names[i].empty()) continue;
        if (!baked[i].albedo.valid() && !baked[i].normal.valid() && !baked[i].mrahw.valid())
            continue;
        res.bytes += baked[i].albedo.byteSize() + baked[i].normal.byteSize()
                   + baked[i].mrahw.byteSize();
        res.textures.emplace(names[i], std::move(baked[i]));
    }

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    ERUPTION_LOG_WARN("EmbeddedTextureBake: comprimiu %zu/%zu texturas embutidas em %.0f ms "
                      "(%.1f MB de payload BC, teto %u)",
                      compressed.load(), n, ms,
                      static_cast<double>(res.bytes) / (1024.0 * 1024.0), maxTextureSize);

    if (!model.filePath.empty() && !res.textures.empty()) {
        writeCache(model.filePath, maxTextureSize, synthesizePbr, res);
    }
    return res;
}

} // namespace eruption
