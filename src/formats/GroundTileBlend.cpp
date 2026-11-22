#include "formats/GroundTileBlend.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

namespace eruption {
namespace {

constexpr size_t kMinTiles = 256;   // abaixo disso nao e' um chao em ladrilhos
constexpr float kAlignTol = 1e-2f;  // tolerancia de alinhamento a grade, em unidades de mundo

struct TileNode {
    size_t node = 0;
    int tex = 0; // indice da textura (nome) no mapa global de nomes
    std::vector<int> vcx, vcz; // casa de cada vertice
};

struct Cell { int tex = -1; int target = -2; }; // target: -2 nao calculado, -1 nenhum

uint64_t cellKey(int x, int z) {
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
}

} // namespace

GroundTileBlendStats applyGroundTileBlend(ModelFile& model, float strength) {
    GroundTileBlendStats st;
    // ERUPTION_TEST_GROUND_TILE_BLEND=<0..1>: forca a intensidade (0 desliga) para
    // comparar antes/depois no mesmo binario.
    if (const char* e = std::getenv("ERUPTION_TEST_GROUND_TILE_BLEND")) strength = std::strtof(e, nullptr);
    if (strength <= 0.0f) return st;

    // 1) Candidatas: nos cujos triangulos cabem, cada um, numa casa quadrada
    // alinhada (bbox em xz quadrado de lado fixo, sobre a grade). Vale para
    // malha expandida (6 vertices por casa) e indexada, desde que cada vertice
    // pertenca a UMA casa (vertices por casa, nao compartilhados). Parede e caixa
    // tem bbox sem area em xz e reprovam.
    std::unordered_map<std::string, int> texIds;
    std::vector<TileNode> cand;
    float side = 0.0f, ox = 0.0f, oz = 0.0f;
    bool haveGrid = false;
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        const ModelFileNode& n = model.nodes[ni];
        if (n.geometryShareIndex >= 0 || n.vertices.empty() || n.indices.size() % 3 != 0 || n.textureNames.empty() ||
            !n.perVertexBlendWeights.empty() || !n.perVertexBlendMaskUV.empty() || !n.perVertexSplatTexIds[0].empty() ||
            n.skin >= 0 || n.morphTargets || n.position != Vec3(0.0f) || n.scale != Vec3(1.0f) ||
            n.rotationAngle != 0.0f)
            continue;
        TileNode t;
        t.node = ni;
        t.vcx.assign(n.vertices.size(), INT32_MIN);
        t.vcz.assign(n.vertices.size(), INT32_MIN);
        bool ok = true;
        for (size_t i = 0; i + 2 < n.indices.size() && ok; i += 3) {
            const uint32_t id[3] = {n.indices[i], n.indices[i + 1], n.indices[i + 2]};
            if (id[0] >= n.vertices.size() || id[1] >= n.vertices.size() || id[2] >= n.vertices.size()) { ok = false; break; }
            float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            for (int k = 0; k < 3; ++k) {
                x0 = std::min(x0, n.vertices[id[k]].x); x1 = std::max(x1, n.vertices[id[k]].x);
                z0 = std::min(z0, n.vertices[id[k]].z); z1 = std::max(z1, n.vertices[id[k]].z);
            }
            const float sx = x1 - x0, sz = z1 - z0;
            if (sx < 1e-3f || std::fabs(sx - sz) > kAlignTol) { ok = false; break; }
            for (int k = 0; k < 3; ++k) {
                const Vec3& p = n.vertices[id[k]];
                const bool xe = std::fabs(p.x - x0) < kAlignTol || std::fabs(p.x - x1) < kAlignTol;
                const bool ze = std::fabs(p.z - z0) < kAlignTol || std::fabs(p.z - z1) < kAlignTol;
                if (!xe || !ze) ok = false;
            }
            if (!haveGrid) { side = sx; ox = x0; oz = z0; haveGrid = true; }
            if (std::fabs(sx - side) > kAlignTol) { ok = false; break; }
            const float gx = (x0 - ox) / side, gz = (z0 - oz) / side;
            if (std::fabs(gx - std::round(gx)) * side > kAlignTol || std::fabs(gz - std::round(gz)) * side > kAlignTol) {
                ok = false; break;
            }
            const int cx = int(std::lround(gx)), cz = int(std::lround(gz));
            for (int k = 0; k < 3; ++k) {
                int& vx = t.vcx[id[k]]; int& vz = t.vcz[id[k]];
                if (vx != INT32_MIN && (vx != cx || vz != cz)) ok = false; // vertice dividido entre casas
                vx = cx; vz = cz;
            }
        }
        if (!ok) continue;
        const auto it = texIds.emplace(n.textureNames[0], int(texIds.size())).first;
        t.tex = it->second;
        cand.push_back(std::move(t));
    }
    if (!haveGrid || texIds.size() < 2) return st;

    // 2) Casa -> textura. Casa em dois nos (sobreposicao) nao e' chao em ladrilhos.
    std::unordered_map<uint64_t, Cell> cells;
    std::unordered_map<uint64_t, size_t> owner;
    size_t tiles = 0, dup = 0;
    for (const TileNode& t : cand) {
        const ModelFileNode& n = model.nodes[t.node];
        for (size_t i = 0; i < n.indices.size(); ++i) {
            const uint32_t v = n.indices[i];
            const uint64_t key = cellKey(t.vcx[v], t.vcz[v]);
            auto o = owner.emplace(key, t.node);
            if (o.second) { cells[key].tex = t.tex; ++tiles; }
            else if (o.first->second != t.node) ++dup;
        }
    }
    if (tiles < kMinTiles || dup * 20 > tiles * 6) return st;

    // 3) Por vertice: alvo = textura diferente mais frequente entre as quatro
    // casas do canto; peso = quantas casas a usam / 4.
    // Nome de material (PBR) de cada textura, do primeiro no que a usa.
    std::vector<std::string> texName(texIds.size()), texPbr(texIds.size());
    for (const auto& kv : texIds) texName[kv.second] = kv.first;
    for (const TileNode& t : cand) {
        const ModelFileNode& n = model.nodes[t.node];
        if (texPbr[t.tex].empty())
            texPbr[t.tex] = n.pbrTextureNames.empty() ? n.textureNames[0] : n.pbrTextureNames[0];
    }
    for (const TileNode& t : cand) {
        ModelFileNode& n = model.nodes[t.node];
        const size_t nv = n.vertices.size();
        std::vector<float> w(nv, 0.0f);
        std::vector<uint16_t> bt(nv, 0);
        std::unordered_map<int, uint16_t> local; // textura global -> indice local no no
        for (size_t v = 0; v < nv; ++v) {
            const int cx = t.vcx[v], cz = t.vcz[v];
            if (cx == INT32_MIN) continue; // vertice sem triangulo
            const Vec3& p = n.vertices[v];
            const int sx = p.x > ox + (cx + 0.5f) * side ? 1 : -1;
            const int sz = p.z > oz + (cz + 0.5f) * side ? 1 : -1;
            // O indice da textura alvo chega ao fragment como `flat` (valor do
            // vertice provocante): tem de ser UM alvo por casa, e todo vertice da
            // casa o leva mesmo com peso 0, senao o triangulo cujo vertice
            // provocante tem peso 0 perde a mistura inteira. Alvo da casa = a
            // textura diferente mais frequente entre as 8 vizinhas.
            auto tcell = cells.find(cellKey(cx, cz));
            if (tcell->second.target == -2) {
                int cnt[32] = {0}, best = -1, bestN = 0;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto it = cells.find(cellKey(cx + dx, cz + dz));
                        if (it == cells.end() || it->second.tex == t.tex || it->second.tex >= 32) continue;
                        if (++cnt[it->second.tex] > bestN) { bestN = cnt[it->second.tex]; best = it->second.tex; }
                    }
                tcell->second.target = best;
            }
            const int best = tcell->second.target;
            if (best < 0) continue;
            // Peso do canto: fracao das quatro casas em volta que usam o alvo.
            int hit = 0;
            const int dx[3] = {sx, 0, sx}, dz[3] = {0, sz, sz};
            for (int j = 0; j < 3; ++j) {
                auto it = cells.find(cellKey(cx + dx[j], cz + dz[j]));
                if (it != cells.end() && it->second.tex == best) ++hit;
            }
            auto it = local.find(best);
            if (it == local.end()) {
                // O alvo entra na lista de texturas do no (a mesma que o renderer
                // resolve em slot); o indice 0 e' a propria textura.
                const uint16_t idx = static_cast<uint16_t>(n.textureNames.size());
                n.textureNames.push_back(texName[best]);
                n.pbrTextureNames.push_back(texPbr[best]);
                n.emissiveTextureNames.push_back(std::string());
                it = local.emplace(best, idx).first;
            }
            bt[v] = it->second;
            w[v] = std::min(1.0f, strength * float(hit) / 4.0f);
            if (hit) ++st.blendedVertices;
        }
        n.perVertexBlendTexIds = std::move(bt);
        n.perVertexBlendWeights = std::move(w);
    }
    st.engaged = true;
    st.tiles = tiles;
    ERUPTION_LOG_WARN("GroundTileBlend: %zu casas de %.1f u, %zu texturas, %zu cantos misturados",
                      tiles, side, texIds.size(), st.blendedVertices);
    return st;
}

} // namespace eruption
