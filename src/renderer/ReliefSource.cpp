#include "renderer/ReliefSource.hpp"

#include "core/Logger.hpp"
#include "formats/TerrainParser.hpp"
#include "utils/EtexLoader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <vulkan/vulkan.h>

#ifdef ERUPTION_HAVE_BC_ENCODER
// Headers de terceiro: struct anonima dispara -Wpedantic que nao e' nosso.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "rgbcx.h"
#include "bc7decomp.h"
#pragma GCC diagnostic pop
#endif

namespace eruption {
namespace fs = std::filesystem;

namespace {

// Hash por palavra de 8 bytes (FNV-1a por palavra): a chave do cache passa
// pelos vertices inteiros da malha, entao byte a byte custaria caro.
uint64_t hashBytes(const void* data, size_t bytes, uint64_t h) {
    const auto* b = static_cast<const uint8_t*>(data);
    constexpr uint64_t kP = 1099511628211ULL;
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * kP;
        h ^= h >> 29;
    }
    for (; i < bytes; ++i) h = (h ^ b[i]) * kP;
    return h;
}

struct Key {
    uint64_t h = 14695981039346656037ULL;
    void add(const void* d, size_t n) { h = hashBytes(d, n, h); }
    template <class T> void val(const T& v) { add(&v, sizeof(T)); }
};

// Altura mediana de uma textura num raio: uma por conteudo e raio, dividida
// entre as malhas (chao e pecas repetem as mesmas texturas).
struct HeightMap {
    std::vector<uint8_t> h;
    int w = 0, hh = 0;
    float mean = 0.5f;
};
std::mutex g_mapMutex;
std::unordered_map<uint64_t, std::shared_ptr<const HeightMap>> g_maps;

std::shared_ptr<const HeightMap> heightMapFor(uint64_t id, const std::vector<uint8_t>& alpha, int w, int h,
                                              int radius, int blur) {
    const uint64_t key = hashBytes(&blur, sizeof(blur), hashBytes(&radius, sizeof(radius), id));
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_maps.find(key);
        if (it != g_maps.end()) return it->second;
    }
    auto m = std::make_shared<HeightMap>();
    double sum = 0.0;
    for (uint8_t a : alpha) sum += a;
    // Linha de base = media da textura inteira (o ultimo mip que a tesselacao
    // le' em heightBaseMip).
    m->mean = float(sum / double(std::max<size_t>(alpha.size(), 1)) / 255.0);
    m->h = reliefMedian(alpha.data(), w, h, radius);
    for (int pass = 0; pass < 2; ++pass) m->h = reliefBoxBlur(m->h.data(), w, h, blur);
    m->w = w;
    m->hh = h;
    std::lock_guard<std::mutex> lock(g_mapMutex);
    return g_maps.emplace(key, std::move(m)).first->second;
}

template <class T> void put(std::FILE* f, const std::vector<T>& v) {
    const uint64_t n = v.size();
    std::fwrite(&n, sizeof(n), 1, f);
    if (n) std::fwrite(v.data(), sizeof(T), n, f);
}
template <class T> bool get(std::FILE* f, std::vector<T>& v) {
    uint64_t n = 0;
    if (std::fread(&n, sizeof(n), 1, f) != 1 || n > (uint64_t(1) << 32)) return false;
    v.resize(n);
    return n == 0 || std::fread(v.data(), sizeof(T), n, f) == n;
}

constexpr uint32_t kMagic = 0x31564C52; // "RLV1"
constexpr uint32_t kVersion = 2; // 2: caixa dupla no Nyquist da grade e leitura bilinear

bool readRecord(const std::string& path, size_t vertsIn, size_t trisIn, ReliefBakeRecord& r) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t magic = 0;
    uint64_t nv = 0;
    bool ok = std::fread(&magic, 4, 1, f) == 1 && magic == kMagic && std::fread(&nv, 8, 1, f) == 1 &&
              nv == vertsIn && get(f, r.parents) && get(f, r.offset) && get(f, r.dir) && get(f, r.children) &&
              get(f, r.indices);
    std::fclose(f);
    const size_t total = vertsIn + r.parents.size() / 2;
    ok = ok && r.parents.size() % 2 == 0 && r.offset.size() == total && r.dir.size() == total * 3 &&
         r.children.size() == trisIn && r.indices.size() % 3 == 0;
    for (size_t i = 0; ok && i < r.parents.size(); ++i) ok = r.parents[i] < vertsIn + i / 2;
    for (size_t i = 0; ok && i < r.indices.size(); ++i) ok = r.indices[i] < total;
    if (!ok) r = {};
    return ok;
}

bool writeRecord(const std::string& path, size_t vertsIn, const ReliefBakeRecord& r) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const uint32_t magic = kMagic;
    const uint64_t nv = vertsIn;
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&nv, 8, 1, f);
    put(f, r.parents);
    put(f, r.offset);
    put(f, r.dir);
    put(f, r.children);
    put(f, r.indices);
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

// Pasta do pacote do jogo, relativa ao diretorio de trabalho (como no celular).
const char* packDir() {
    static const std::string d = [] {
#ifdef __ANDROID__
        const std::string p = "assets/lodcache/mobile";
#else
        const std::string p = "assets/lodcache/desktop";
#endif
        std::error_code ec;
        return fs::is_directory(p, ec) ? p : std::string();
    }();
    return d.empty() ? nullptr : d.c_str();
}

// UV por unidade de mundo de cada textura com altura (mediana das arestas).
std::unordered_map<uint32_t, float> uvPerUnitBySlot(const std::vector<TerrainVertex>& v,
                                                    const std::vector<uint32_t>& idx) {
    std::unordered_map<uint32_t, std::vector<float>> acc;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const TerrainVertex& a = v[idx[t]];
        const TerrainVertex& b = v[idx[t + 1]];
        if (a.pbrIndex == 0 || a.texIndex == 0 || a.texIndex != b.texIndex) continue;
        const float lp = glm::length(b.position - a.position);
        if (lp <= 1e-4f) continue;
        acc[a.texIndex].push_back(glm::length(b.texCoord - a.texCoord) / lp);
    }
    std::unordered_map<uint32_t, float> out;
    for (auto& [slot, d] : acc) {
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        out[slot] = d[d.size() / 2];
    }
    return out;
}

} // namespace

bool reliefDecodeAlpha(const EtexData& e, std::vector<uint8_t>& alpha) {
    if (!e.valid() || e.width == 0 || e.height == 0) return false;
    const size_t w = e.width, h = e.height;
    const size_t mip0 = e.mipSizes.empty() ? e.payload.size() : e.mipSizes[0];
    if (e.vkFormat == VK_FORMAT_R8G8B8A8_UNORM) {
        if (mip0 < w * h * 4 || e.payload.size() < w * h * 4) return false;
        alpha.resize(w * h);
        for (size_t i = 0; i < w * h; ++i) alpha[i] = e.payload[4 * i + 3];
        return true;
    }
#ifdef ERUPTION_HAVE_BC_ENCODER
    const bool bc1 = e.vkFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || e.vkFormat == VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    const bool bc3 = e.vkFormat == VK_FORMAT_BC3_UNORM_BLOCK;
    const bool bc7 = e.vkFormat == VK_FORMAT_BC7_UNORM_BLOCK;
    if (!bc1 && !bc3 && !bc7) return false;
    const size_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = bc1 ? 8 : 16;
    if (e.payload.size() < bw * bh * bb) return false;
    alpha.assign(w * h, 255);
    uint8_t px[64];
    for (size_t by = 0; by < bh; ++by)
        for (size_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = e.payload.data() + (by * bw + bx) * bb;
            if (bc1) rgbcx::unpack_bc1(b, px);
            else if (bc3) rgbcx::unpack_bc3(b, px);
            else bc7decomp::unpack_bc7(b, reinterpret_cast<bc7decomp::color_rgba*>(px));
            for (size_t y = 0; y < 4 && by * 4 + y < h; ++y)
                for (size_t x = 0; x < 4 && bx * 4 + x < w; ++x)
                    alpha[(by * 4 + y) * w + bx * 4 + x] = px[(y * 4 + x) * 4 + 3];
        }
    return true;
#else
    return false;
#endif
}

uint64_t reliefContentHash(const void* data, size_t bytes) {
    return hashBytes(data, bytes, 14695981039346656037ULL ^ bytes);
}

void reliefClearMaps() {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_maps.clear();
}

bool reliefBakeCached(const std::vector<TerrainVertex>& vertices, const std::vector<uint32_t>& indices,
                      const ReliefParams& params, const ReliefAlphaSource& source, ReliefResult& out) {
    const auto t0 = std::chrono::steady_clock::now();
    out = {};
    if (vertices.empty() || indices.size() < 3 || params.amplitude <= 0.0f || params.spacing <= 0.0f || !source)
        return false;
    // Grade efetiva: partir ate' `spacing` da' ~5,3 x area / spacing^2
    // triangulos (teste do chao: 540 000 u^2 a 2 u -> 713 856); acima do alvo
    // a grade abre.
    double area = 0.0;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const Vec3 e1 = vertices[indices[t + 1]].position - vertices[indices[t]].position;
        const Vec3 e2 = vertices[indices[t + 2]].position - vertices[indices[t]].position;
        area += 0.5 * double(glm::length(glm::cross(e1, e2)));
    }
    const float spacing =
        std::max(params.spacing, float(std::sqrt(5.3 * area / double(std::max<size_t>(params.maxTriangles, 1)))));
    out.spacing = spacing;

    // Identidade da altura de cada textura da malha (sem decodificar).
    const std::unordered_map<uint32_t, float> uvpu = uvPerUnitBySlot(vertices, indices);
    std::unordered_map<uint32_t, uint64_t> ids;
    {
        std::vector<uint8_t> none;
        int w = 0, h = 0;
        for (const auto& kv : uvpu) {
            uint64_t id = 0;
            if (source(kv.first, false, none, w, h, id)) ids.emplace(kv.first, id);
        }
    }
    if (ids.empty()) return false;
    // Chave: o que muda o resultado, sem o numero do slot bindless (muda de
    // uma carga para outra): posicao, UV e normal de cada vertice, a altura
    // dele pela identidade do conteudo, os indices e os parametros.
    Key key;
    key.val(kVersion);
    key.val(spacing);
    key.val(params.amplitude);
    key.val(params.gain);
    key.val(uint64_t(params.maxTriangles));
    static_assert(offsetof(TerrainVertex, normal) + sizeof(Vec3) == 32, "posicao, UV e normal no inicio do vertice");
    for (const TerrainVertex& v : vertices) {
        key.add(&v, 32);
        auto it = v.pbrIndex != 0 ? ids.find(v.texIndex) : ids.end();
        key.val(it != ids.end() ? it->second : uint64_t(0));
    }
    key.add(indices.data(), indices.size() * sizeof(uint32_t));

    const size_t trisIn = indices.size() / 3;
    static const char* kPackOut = std::getenv("ERUPTION_RELIEF_PACK_OUT");
    char name[48];
    std::snprintf(name, sizeof(name), "relevo_%016llx.bin", static_cast<unsigned long long>(key.h));
    const std::string local = std::string(".cache/eruption/relevo/") + name;
    const char* pack = packDir();
    if (params.store && ((pack && readRecord(std::string(pack) + "/" + name, vertices.size(), trisIn, out.record)) ||
                         readRecord(local, vertices.size(), trisIn, out.record))) {
        if (kPackOut) writeRecord(std::string(kPackOut) + "/" + name, vertices.size(), out.record);
        out.fromCache = true;
        out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return true;
    }

    // Mediana no raio da grade: k texels entre dois vertices, raio k/2 (1 com
    // k = 2, nenhum abaixo de 1,5 - bancada de variantes,
    // i23_assado_variantes_high_medium_low.png). Depois dela, o FILTRO DO
    // NYQUIST: caixa dupla de raio k/2 (tenda de largura ~2k), sem teto. A
    // mediana para em 32 texels (custo) e preserva degrau: num chao de mapa grande
    // a 2,98 u (k ~ 90 texels) a grade carregava detalhe mais fino que ela e a
    // aresta no rasante saia em serra. A leitura passa a ser bilinear.
    std::unordered_map<uint32_t, std::shared_ptr<const HeightMap>> maps;
    for (const auto& [slot, id] : ids) {
        std::vector<uint8_t> alpha;
        int w = 0, h = 0;
        uint64_t id2 = 0;
        if (!source(slot, true, alpha, w, h, id2) || w <= 0 || h <= 0 || alpha.size() != size_t(w) * size_t(h))
            continue;
        const float k = spacing * uvpu.at(slot) * float(std::max(w, h));
        const int radius = k < 1.5f ? 0 : std::clamp(int(std::lround(k * 0.5f)), 1, 32);
        const int blur = k < 1.5f ? 0 : std::clamp(int(std::lround(k * 0.5f)), 1, std::max(w, h) / 2);
        maps.emplace(slot, heightMapFor(id, alpha, w, h, radius, blur));
    }
    if (maps.empty()) return false;

    std::vector<ReliefVertex> rv0(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const TerrainVertex& v = vertices[i];
        ReliefVertex& r = rv0[i];
        for (int c = 0; c < 3; ++c) {
            r.pos[c] = v.position[c];
            r.normal[c] = v.normal[c];
        }
        r.uv[0] = v.texCoord.x;
        r.uv[1] = v.texCoord.y;
        r.tex = v.pbrIndex != 0 ? v.texIndex : 0u;
    }
    ReliefBakeParams bp;
    bp.spacing = spacing;
    bp.amplitude = params.amplitude;
    bp.maxTriangles = params.maxTriangles * 2;
    const float gain = params.gain;
    auto height = [&](uint32_t tex, float u, float v) -> float {
        auto it = maps.find(tex);
        if (tex == 0 || it == maps.end()) return 0.5f;
        const HeightMap& m = *it->second;
        const float a = reliefSampleBilinear(m.h, m.w, m.hh, u, v);
        if (a > 0.999f) return 0.5f; // 255 = textura sem altura (heightUv)
        return std::clamp(0.5f + (a - m.mean) * gain, 0.0f, 1.0f);
    };
    // A estimativa pela area erra nas malhas de triangulo comprido (a lasca
    // so' parte a aresta longa): no teto, abre a grade 1,3x e assa de novo.
    for (int tries = 0;; ++tries) {
        std::vector<ReliefVertex> rv = rv0;
        std::vector<uint32_t> idx = indices;
        const ReliefBakeStats st = bakeRelief(rv, idx, bp, height, &out.record);
        if (!st.capped || tries == 5) {
            if (st.capped)
                ERUPTION_LOG_WARN("[RELEVO] malha de %zu tris parou no teto (%zu tris) na grade de %.2f u", trisIn,
                                  st.trianglesOut, double(bp.spacing));
            break;
        }
        bp.spacing *= 1.3f;
    }
    out.spacing = bp.spacing;
    if (!params.store) {
        out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return true;
    }
    if (!writeRecord(local, vertices.size(), out.record))
        ERUPTION_LOG_WARN("[RELEVO] cache nao gravado: %s", local.c_str());
    if (kPackOut) writeRecord(std::string(kPackOut) + "/" + name, vertices.size(), out.record);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace eruption
