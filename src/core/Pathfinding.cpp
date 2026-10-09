// Caminho dos personagens: A* na grade do chão (a mesma da caminhada), contornando
// objetos sólidos, e depois alisado (pula os pontos que se vê em linha reta).
#include "core/Engine.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace eruption {

namespace {
struct Cell {
    int x, z;
    bool operator==(const Cell& o) const { return x == o.x && z == o.z; }
};
struct CellHash {
    size_t operator()(const Cell& c) const { return std::hash<int64_t>()((int64_t(c.x) << 32) ^ uint32_t(c.z)); }
};
constexpr int kMaxExpanded = 40000; // ~ uma área de 200 x 200 células
} // namespace

bool Engine::findPath(const Vec3& from, const Vec3& to, float radius, std::vector<Vec3>& out) const {
    out.clear();
    const LoadedMap* map = mapAt(from.x, from.z);
    const float step = map && map->terrainFromFile && map->terrain.scale > 0.0f ? map->terrain.scale : 5.0f;
    std::unordered_map<Cell, int8_t, CellHash> walk; // 0 não sabe, 1 anda, -1 não
    auto at = [&](const Cell& c) { return Vec3(c.x * step, 0.0f, c.z * step); };
    auto walkable = [&](const Cell& c) {
        int8_t& w = walk[c];
        if (w == 0) {
            Vec3 p = at(c);
            p.y = sampleTerrainHeight(p.x, p.z);
            w = navWalkable(p.x, p.z) && !blockedAt(p, radius) ? 1 : -1;
        }
        return w > 0;
    };
    auto cellOf = [&](const Vec3& p) { return Cell{int(std::lround(p.x / step)), int(std::lround(p.z / step))}; };
    const Cell start = cellOf(from), goal = cellOf(to);
    // Ponto livre perto do pedido (o alvo pode estar dentro de uma parede).
    Cell target = goal;
    if (!walkable(target)) {
        bool found = false;
        for (int r = 1; r <= 6 && !found; ++r)
            for (int dz = -r; dz <= r && !found; ++dz)
                for (int dx = -r; dx <= r && !found; ++dx)
                    if (std::max(std::abs(dx), std::abs(dz)) == r && walkable({goal.x + dx, goal.z + dz})) {
                        target = {goal.x + dx, goal.z + dz};
                        found = true;
                    }
        if (!found) return false;
    }
    struct Node {
        float f;
        Cell c;
        bool operator<(const Node& o) const { return f > o.f; }
    };
    auto h = [&](const Cell& c) {
        const float dx = float(std::abs(c.x - target.x)), dz = float(std::abs(c.z - target.z));
        return (dx + dz) + (1.41421356f - 2.0f) * std::min(dx, dz); // octil
    };
    std::priority_queue<Node> open;
    std::unordered_map<Cell, float, CellHash> g;
    std::unordered_map<Cell, Cell, CellHash> came;
    g[start] = 0.0f;
    open.push({h(start), start});
    int expanded = 0;
    bool reached = false;
    while (!open.empty() && expanded < kMaxExpanded) {
        const Node n = open.top();
        open.pop();
        if (n.c == target) {
            reached = true;
            break;
        }
        const float gn = g[n.c];
        if (n.f - h(n.c) > gn + 1e-3f) continue; // entrada velha
        ++expanded;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                const Cell nb{n.c.x + dx, n.c.z + dz};
                if (!walkable(nb)) continue;
                // Diagonal não corta quina.
                if (dx && dz && (!walkable({n.c.x + dx, n.c.z}) || !walkable({n.c.x, n.c.z + dz}))) continue;
                const float cost = gn + (dx && dz ? 1.41421356f : 1.0f);
                auto it = g.find(nb);
                if (it != g.end() && it->second <= cost) continue;
                g[nb] = cost;
                came[nb] = n.c;
                open.push({cost + h(nb), nb});
            }
    }
    if (!reached) return false;
    std::vector<Cell> cells{target};
    while (!(cells.back() == start)) cells.push_back(came[cells.back()]);
    std::reverse(cells.begin(), cells.end());
    // Alisa: de cada ponto, vai direto ao mais longe que se vê sem obstáculo.
    auto clear = [&](const Vec3& a, const Vec3& b) {
        const float len = glm::length(Vec2(b.x - a.x, b.z - a.z));
        const int n = std::max(1, int(len / (step * 0.5f)));
        for (int i = 1; i < n; ++i) {
            const Vec3 p = glm::mix(a, b, float(i) / n);
            if (!walkable(cellOf(p))) return false;
        }
        return true;
    };
    std::vector<Vec3> pts;
    pts.reserve(cells.size());
    pts.push_back(from);
    for (size_t i = 1; i + 1 < cells.size(); ++i) pts.push_back(at(cells[i]));
    pts.push_back(goal == target ? to : at(target));
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
