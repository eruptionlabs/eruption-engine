// Caminho dos personagens: A* na grade do chão (a mesma da caminhada), contornando
// objetos sólidos, e depois alisado (pula os pontos que se vê em linha reta).
#include "core/Engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace eruption {

namespace {

// Solido ja' recortado para a busca: a caixa crescida pelo raio e a base
// (quadrilatero convexo) do objeto, como o Engine::blockedAt os testa.
struct PathSolid {
    float minX, maxX, minZ, maxZ; // caixa em XZ crescida pelo raio
    float minY, maxY;
    Vec2 quad[4];
};

constexpr int kBucket = 16;         // celulas por lado do balde de solidos
constexpr int kMinMargin = 64;      // folga minima da janela em volta de origem e alvo
constexpr int64_t kMaxWindow = 1600 * 1600;

} // namespace

// A* em grade 8-vizinha (heuristica octil, sem cortar quina) dentro de uma
// janela em volta de origem e alvo. Antes cada celula nova chamava o
// blockedAt, que percorre TODAS as instancias solidas da cena (celulas x
// solidos), com tres mapas de hash por celula e teto fixo de 40 mil nos: num
// mapa de 3400 u com o jogo cheio de objetos uma busca longa passava de 250 ms.
// Agora os solidos da janela sao recortados uma vez e postos em baldes de
// 16x16 celulas (cada celula testa so' os do seu balde), o estado da busca e'
// vetor denso, e o teto e' o tamanho da janela.
bool Engine::findPath(const Vec3& from, const Vec3& to, float radius, std::vector<Vec3>& out) const {
    out.clear();
    const LoadedMap* map = mapAt(from.x, from.z);
    const float step = map && map->terrainFromFile && map->terrain.scale > 0.0f ? map->terrain.scale : 5.0f;
    auto cellX = [&](float x) { return int(std::lround(x / step)); };
    const int sx = cellX(from.x), sz = cellX(from.z), gx = cellX(to.x), gz = cellX(to.z);

    // Janela: retangulo de origem e alvo com folga (metade da distancia, no
    // minimo kMinMargin). Fora dela nao se anda.
    int margin = std::max(kMinMargin, std::max(std::abs(gx - sx), std::abs(gz - sz)) / 2);
    int x0, z0, w, d;
    for (;;) {
        x0 = std::min(sx, gx) - margin;
        z0 = std::min(sz, gz) - margin;
        w = std::abs(gx - sx) + 2 * margin + 1;
        d = std::abs(gz - sz) + 2 * margin + 1;
        if (int64_t(w) * d <= kMaxWindow || margin <= 8) break;
        margin /= 2;
    }
    if (int64_t(w) * d > kMaxWindow) return false;
    const size_t cells = size_t(w) * size_t(d);
    auto inWindow = [&](int x, int z) { return x >= x0 && z >= z0 && x < x0 + w && z < z0 + d; };
    auto idxOf = [&](int x, int z) { return size_t(z - z0) * size_t(w) + size_t(x - x0); };

    // Solidos que tocam a janela, nos baldes.
    const int bw = (w + kBucket - 1) / kBucket, bd = (d + kBucket - 1) / kBucket;
    std::vector<PathSolid> solids;
    std::vector<std::vector<uint32_t>> buckets(size_t(bw) * size_t(bd));
    {
        const float wx0 = (x0 - 0.5f) * step, wz0 = (z0 - 0.5f) * step;
        const float wx1 = (x0 + w + 0.5f) * step, wz1 = (z0 + d + 0.5f) * step;
        const auto& insts = m_modelRenderer.getInstances();
        for (uint32_t i = 0; i < m_solid.size() && i < insts.size(); ++i) {
            if (!m_solid[i] || !insts[i].enabled) continue;
            const ModelInstance& inst = insts[i];
            PathSolid ps;
            ps.minX = inst.worldAabbMin.x - radius;
            ps.maxX = inst.worldAabbMax.x + radius;
            ps.minZ = inst.worldAabbMin.z - radius;
            ps.maxZ = inst.worldAabbMax.z + radius;
            if (ps.maxX < wx0 || ps.minX > wx1 || ps.maxZ < wz0 || ps.minZ > wz1) continue;
            if (!m_modelRenderer.footprint(i, ps.quad)) continue;
            ps.minY = inst.worldAabbMin.y;
            ps.maxY = inst.worldAabbMax.y;
            const auto id = static_cast<uint32_t>(solids.size());
            solids.push_back(ps);
            const int bx0 = std::clamp(int(std::floor(ps.minX / step + 0.5f)) - x0, 0, w - 1) / kBucket;
            const int bx1 = std::clamp(int(std::ceil(ps.maxX / step + 0.5f)) - x0, 0, w - 1) / kBucket;
            const int bz0 = std::clamp(int(std::floor(ps.minZ / step + 0.5f)) - z0, 0, d - 1) / kBucket;
            const int bz1 = std::clamp(int(std::ceil(ps.maxZ / step + 0.5f)) - z0, 0, d - 1) / kBucket;
            for (int bz = bz0; bz <= bz1; ++bz)
                for (int bx = bx0; bx <= bx1; ++bx) buckets[size_t(bz) * size_t(bw) + size_t(bx)].push_back(id);
        }
    }
    // A mesma conta do Engine::blockedAt, so' com os solidos do balde.
    auto blocked = [&](int x, int z, const Vec3& p) {
        for (uint32_t id : buckets[size_t((z - z0) / kBucket) * size_t(bw) + size_t((x - x0) / kBucket)]) {
            const PathSolid& s = solids[id];
            if (p.y + 20.0f < s.minY || p.y > s.maxY) continue;
            if (p.x < s.minX || p.x > s.maxX || p.z < s.minZ || p.z > s.maxZ) continue;
            bool inside = true;
            for (int e = 0; e < 4 && inside; ++e) {
                const Vec2 a = s.quad[e], b = s.quad[(e + 1) % 4];
                const Vec2 edge = b - a;
                const float len = glm::length(edge);
                if (len < 1e-4f) continue;
                const Vec2 n(edge.y / len, -edge.x / len);
                if (glm::dot(Vec2(p.x, p.z) - a, n) > radius) inside = false;
            }
            if (inside) return true;
        }
        return false;
    };

    std::vector<int8_t> walk(cells, 0); // 0 nao sabe, 1 anda, -1 nao
    auto walkable = [&](int x, int z) {
        if (!inWindow(x, z)) return false;
        int8_t& v = walk[idxOf(x, z)];
        if (v == 0) {
            Vec3 p(x * step, 0.0f, z * step);
            p.y = sampleTerrainHeight(p.x, p.z);
            v = navWalkable(p.x, p.z) && !blocked(x, z, p) ? 1 : -1;
        }
        return v > 0;
    };
    // Ponto livre perto do pedido (o alvo pode estar dentro de uma parede).
    int tx = gx, tz = gz;
    if (!walkable(tx, tz)) {
        bool found = false;
        for (int r = 1; r <= 6 && !found; ++r)
            for (int dz = -r; dz <= r && !found; ++dz)
                for (int dx = -r; dx <= r && !found; ++dx)
                    if (std::max(std::abs(dx), std::abs(dz)) == r && walkable(gx + dx, gz + dz)) {
                        tx = gx + dx;
                        tz = gz + dz;
                        found = true;
                    }
        if (!found) return false;
    }
    auto h = [&](int x, int z) {
        const float dx = float(std::abs(x - tx)), dz = float(std::abs(z - tz));
        return (dx + dz) + (1.41421356f - 2.0f) * std::min(dx, dz); // octil
    };
    constexpr float kUnseen = std::numeric_limits<float>::max();
    std::vector<float> g(cells, kUnseen);
    std::vector<int32_t> came(cells, -1);
    std::vector<std::pair<float, int32_t>> open; // heap de minimo por f
    auto heapLess = [](const std::pair<float, int32_t>& a, const std::pair<float, int32_t>& b) { return a.first > b.first; };
    const size_t start = idxOf(sx, sz), target = idxOf(tx, tz);
    g[start] = 0.0f;
    open.push_back({h(sx, sz), int32_t(start)});
    bool reached = false;
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), heapLess);
        const auto [f, ni] = open.back();
        open.pop_back();
        const size_t n = size_t(ni);
        if (n == target) {
            reached = true;
            break;
        }
        const int nx = x0 + int(n % size_t(w)), nz = z0 + int(n / size_t(w));
        const float gn = g[n];
        if (f - h(nx, nz) > gn + 1e-3f) continue; // entrada velha
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                if (!walkable(nx + dx, nz + dz)) continue;
                // Diagonal nao corta quina.
                if (dx && dz && (!walkable(nx + dx, nz) || !walkable(nx, nz + dz))) continue;
                const float cost = gn + (dx && dz ? 1.41421356f : 1.0f);
                const size_t m = idxOf(nx + dx, nz + dz);
                if (g[m] <= cost) continue;
                g[m] = cost;
                came[m] = int32_t(n);
                open.push_back({cost + h(nx + dx, nz + dz), int32_t(m)});
                std::push_heap(open.begin(), open.end(), heapLess);
            }
    }
    if (!reached) return false;
    auto at = [&](size_t i) { return Vec3((x0 + int(i % size_t(w))) * step, 0.0f, (z0 + int(i / size_t(w))) * step); };
    std::vector<size_t> path{target};
    while (path.back() != start) path.push_back(size_t(came[path.back()]));
    std::reverse(path.begin(), path.end());
    // Alisa: de cada ponto, vai direto ao mais longe que se ve' sem obstaculo.
    auto clear = [&](const Vec3& a, const Vec3& b) {
        const float len = glm::length(Vec2(b.x - a.x, b.z - a.z));
        const int n = std::max(1, int(len / (step * 0.5f)));
        for (int i = 1; i < n; ++i) {
            const Vec3 p = glm::mix(a, b, float(i) / n);
            if (!walkable(cellX(p.x), cellX(p.z))) return false;
        }
        return true;
    };
    std::vector<Vec3> pts;
    pts.reserve(path.size());
    pts.push_back(from);
    for (size_t i = 1; i + 1 < path.size(); ++i) pts.push_back(at(path[i]));
    pts.push_back(tx == gx && tz == gz ? to : at(target));
    size_t i = 0;
    out.push_back(pts.front());
    while (i + 1 < pts.size()) {
        size_t j = pts.size() - 1;
        while (j > i + 1 && !clear(pts[i], pts[j])) --j;
        out.push_back(pts[j]);
        i = j;
    }
    for (auto& p : out) p.y = sampleTerrainHeight(p.x, p.z);
    return true;
}

bool Engine::characterAt(const Vec3& p, float radius, int ignore) const {
    for (size_t i = 0; i < m_characters.size(); ++i) {
        const SceneCharacter& c = m_characters[i];
        if (static_cast<int>(i) == ignore || c.removed || c.parked || !c.visible) continue;
        if (glm::length(Vec2(p.x - c.position.x, p.z - c.position.z)) < radius + 5.0f && std::abs(p.y - c.position.y) < 25.0f)
            return true;
    }
    return false;
}

} // namespace eruption
