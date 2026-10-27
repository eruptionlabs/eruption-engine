// Caminho dos personagens: A* na grade do chão (a mesma da caminhada), contornando
// objetos sólidos e pesando o custo por célula do jogo, e depois alisado (pula
// os pontos que se vê em linha reta).
#include "core/Engine.hpp"
#include "core/PathClearance.hpp"

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
// Subida aceita de uma celula para a vizinha, em passos da grade: a rampa de
// entrada de uma caverna medida em 26/10 sobe 14 u em 10 u.
constexpr float kClimb = 2.0f;

} // namespace

// A* em grade 8-vizinha (heuristica octil, sem cortar quina) dentro de uma
// janela em volta de origem e alvo. Antes cada celula nova chamava o
// blockedAt, que percorre TODAS as instancias solidas da cena (celulas x
// solidos), com tres mapas de hash por celula e teto fixo de 40 mil nos: num
// mapa de 3400 u com o jogo cheio de objetos uma busca longa passava de 250 ms.
// Agora os solidos da janela sao recortados uma vez e postos em baldes de
// 16x16 celulas (cada celula testa so' os do seu balde), o estado da busca e'
// vetor denso, e o teto e' o tamanho da janela.
// wallClearance > 0: cada celula do caminho e cada trecho alisado (amostrado a
// cada quarto de celula) fica a essa distancia de qualquer celula que nao
// anda (parede, rampa bloqueada, solido). Sem ela, a grade so' testa o
// centro da celula e o alisamento corta a quina para fora do chao.
bool Engine::findPath(const Vec3& from, const Vec3& to, float radius, std::vector<Vec3>& out,
                      float wallClearance) const {
    out.clear();
    // Mapas empilhados: a busca fica no andar de quem sai (a altura da origem).
    const LoadedMap* map = mapAt(from.x, from.z, from.y);
    // As celulas da busca sao as da grade de caminhada do mapa (passo e
    // centro); o resto da conta fica no referencial dela (mundo - origem).
    Vec2 org;
    float step = navCellFrame(map, org);
    // Com folga, meia celula: as celulas da busca sao quartos das da grade (o
    // chao continua exato) e o caminho acha a passagem que so' cabe fora do
    // centro de uma celula (16 u livres para folga 8 numa grade de 5 u).
    if (wallClearance > 0.0f) {
        step *= 0.5f;
        org -= Vec2(0.5f * step);
    }
    auto cellX = [&](float x) { return int(std::lround((x - org.x) / step)); };
    auto cellZ = [&](float z) { return int(std::lround((z - org.y) / step)); };
    const int sx = cellX(from.x), sz = cellZ(from.z), gx = cellX(to.x), gz = cellZ(to.z);

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
        const float wx0 = org.x + (x0 - 0.5f) * step, wz0 = org.y + (z0 - 0.5f) * step;
        const float wx1 = org.x + (x0 + w + 0.5f) * step, wz1 = org.y + (z0 + d + 0.5f) * step;
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
            const int bx0 = std::clamp(int(std::floor((ps.minX - org.x) / step + 0.5f)) - x0, 0, w - 1) / kBucket;
            const int bx1 = std::clamp(int(std::ceil((ps.maxX - org.x) / step + 0.5f)) - x0, 0, w - 1) / kBucket;
            const int bz0 = std::clamp(int(std::floor((ps.minZ - org.y) / step + 0.5f)) - z0, 0, d - 1) / kBucket;
            const int bz1 = std::clamp(int(std::ceil((ps.maxZ - org.y) / step + 0.5f)) - z0, 0, d - 1) / kBucket;
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

    // Chao de cada celula perguntado na altura de quem chega nela (fromY, o
    // chao da celula de onde vem), nao na da origem: numa rampa a altura da
    // origem fica para tras e, entre mapas empilhados (caverna sob a
    // montanha), mapAt escolhia o mapa de baixo no meio da subida e a busca
    // morria na emenda.
    // Com folga, a celula so' anda se o quadrado inteiro dela anda: a grade de
    // 5 u antiga comeca na origem (celula [5i, 5i+5)) e a busca centra a celula
    // em 5i, entao o centro cai na divisa de duas celulas da grade e a metade
    // de la' podia ser parede (o caminho passava rente a ela). Os quatro
    // pontos a um quarto de celula do centro caem um em cada metade.
    const bool roomy = wallClearance > 0.0f;
    std::vector<int8_t> walk(cells, 0); // 0 nao sabe, 1 anda, -1 nao
    std::vector<float> floorY(cells, from.y);
    auto walkable = [&](int x, int z, float fromY) {
        if (!inWindow(x, z)) return false;
        const size_t i = idxOf(x, z);
        int8_t& v = walk[i];
        if (v == 0) {
            const float y = fromY + kClimb * step;
            Vec3 p(org.x + x * step, 0.0f, org.y + z * step);
            p.y = sampleTerrainHeight(p.x, p.z, y);
            floorY[i] = p.y;
            bool ok = navWalkable(p.x, p.z, y) && !blocked(x, z, p);
            const float q = step * 0.25f;
            for (int k = 0; k < 4 && ok && roomy; ++k)
                ok = navWalkable(p.x + (k & 1 ? q : -q), p.z + (k & 2 ? q : -q), y);
            v = ok ? 1 : -1;
        }
        return v > 0;
    };
    // Celula que anda com a folga de parede (sem folga = walkable). Perto da
    // origem (folga + uma celula) a folga nao vale: quem nasce colado na
    // parede sai dela; antes, todo vizinho da origem ficava sem folga e a
    // busca morria ali (grupo nascido a 2 u da beira de um corredor).
    const float escape = wallClearance + step;
    auto nearFrom = [&](float px, float pz) { return glm::length(Vec2(px - from.x, pz - from.z)) <= escape; };
    std::vector<int8_t> room(roomy ? cells : 0, 0);
    auto passable = [&](int x, int z, float fromY) {
        if (!walkable(x, z, fromY)) return false;
        if (!roomy || nearFrom(org.x + x * step, org.y + z * step)) return true;
        int8_t& v = room[idxOf(x, z)];
        if (v == 0) {
            auto base = [&](int cx, int cz) { return walkable(cx, cz, fromY); };
            v = pointHasClearance(x * step, z * step, step, wallClearance, base) ? 1 : -1;
        }
        return v > 0;
    };
    // Custo da celula escrito pelo jogo (setWalkCost; >= 1, entao a
    // heuristica octil continua admissivel). 0 = ainda nao lido.
    std::vector<float> cellCost(cells, 0.0f);
    auto costAt = [&](int x, int z) {
        float& c = cellCost[idxOf(x, z)];
        if (c == 0.0f) c = walkCost(org.x + x * step, org.y + z * step);
        return c;
    };
    // Ponto livre perto do pedido (o alvo pode estar dentro de uma parede).
    int tx = gx, tz = gz;
    if (!passable(tx, tz, to.y)) {
        bool found = false;
        const int reach = 6 + int(std::ceil(wallClearance / step));
        for (int r = 1; r <= reach && !found; ++r)
            for (int dz = -r; dz <= r && !found; ++dz)
                for (int dx = -r; dx <= r && !found; ++dx)
                    if (std::max(std::abs(dx), std::abs(dz)) == r && passable(gx + dx, gz + dz, to.y)) {
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
    auto moveClear = [&](int ax, int az, int bx, int bz, float y) {
        auto base = [&](int cx, int cz) { return walkable(cx, cz, y); };
        return segmentHasClearance(ax * step, az * step, bx * step, bz * step, step, wallClearance, base);
    };
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
        const float yn = floorY[n];
        if (f - h(nx, nz) > gn + 1e-3f) continue; // entrada velha
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                if (!passable(nx + dx, nz + dz, yn)) continue;
                // Com folga, o passo entre dois centros livres e' amostrado
                // (no meio dele a parede pode chegar mais perto que nas
                // pontas) e isso ja' cuida da quina: a regra da diagonal
                // fechava a passagem que so' cabe na diagonal (16 u entre a
                // quina de uma parede e a de uma pedra, folga 8). A saida da
                // origem fica livre (quem esta' colado na parede sai dela).
                const bool fromFree = roomy && !nearFrom(org.x + nx * step, org.y + nz * step);
                if (fromFree) {
                    if (!moveClear(nx, nz, nx + dx, nz + dz, yn)) continue;
                } else if (dx && dz && (!passable(nx + dx, nz, yn) || !passable(nx, nz + dz, yn))) {
                    continue; // diagonal nao corta quina
                }
                const float cost = gn + (dx && dz ? 1.41421356f : 1.0f) * costAt(nx + dx, nz + dz);
                const size_t m = idxOf(nx + dx, nz + dz);
                if (g[m] <= cost) continue;
                g[m] = cost;
                came[m] = int32_t(n);
                open.push_back({cost + h(nx + dx, nz + dz), int32_t(m)});
                std::push_heap(open.begin(), open.end(), heapLess);
            }
    }
    if (!reached) return false;
    auto at = [&](size_t i) {
        return Vec3(org.x + (x0 + int(i % size_t(w))) * step, floorY[i], org.y + (z0 + int(i / size_t(w))) * step);
    };
    std::vector<size_t> path{target};
    while (path.back() != start) path.push_back(size_t(came[path.back()]));
    std::reverse(path.begin(), path.end());
    // Alisa: de cada ponto, vai direto ao mais longe que se ve' sem obstaculo.
    auto clear = [&](const Vec3& a, const Vec3& b) {
        if (roomy) {
            const float y = std::max(a.y, b.y);
            auto base = [&](int cx, int cz) { return walkable(cx, cz, y); };
            const float len = glm::length(Vec2(b.x - a.x, b.z - a.z));
            const int n = std::max(1, int(std::ceil(len / (step * kClearanceSample))));
            for (int i = 1; i < n; ++i) {
                const Vec3 p = glm::mix(a, b, float(i) / n);
                if (!nearFrom(p.x, p.z) && !pointHasClearance(p.x - org.x, p.z - org.y, step, wallClearance, base))
                    return false;
            }
        }
        const float len = glm::length(Vec2(b.x - a.x, b.z - a.z));
        const int n = std::max(1, int(len / (step * 0.5f)));
        for (int i = 1; i < n; ++i) {
            const Vec3 p = glm::mix(a, b, float(i) / n);
            // Atalho nao corta celula cara (o A* contornou ou pagou por ela).
            const int cx = cellX(p.x), cz = cellZ(p.z);
            if (!walkable(cx, cz, p.y) || costAt(cx, cz) > 1.0f) return false;
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
    for (auto& p : out) p.y = sampleTerrainHeight(p.x, p.z, p.y + kClimb * step);
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
