#include "utils/MeshClusters.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

// Reordena `idx` (triangulos sobre `pos`, passo `stride`) por celula de uma
// grade 3D sobre a caixa da malha, em ordem de Morton, e devolve a caixa e o
// intervalo de cada celula. ~3 mil triangulos por pedaco: o bastante para o
// descarte valer, pouco o bastante para nao virar desenho demais.
std::vector<MeshCluster> buildMeshClusters(const unsigned char* pos, size_t stride, std::vector<uint32_t>& idx) {
    std::vector<MeshCluster> out;
    const size_t tris = idx.size() / 3;
    if (tris < 3) return out;
    auto P = [&](uint32_t v) { return *reinterpret_cast<const Vec3*>(pos + size_t(v) * stride); };
    Vec3 lo(1e30f), hi(-1e30f);
    for (uint32_t v : idx) { lo = glm::min(lo, P(v)); hi = glm::max(hi, P(v)); }
    const Vec3 ext = glm::max(hi - lo, Vec3(1e-3f));
    const float cells = std::clamp(float(tris) / 3000.0f, 8.0f, 512.0f);
    const float cell = std::cbrt(std::max(ext.x, 1e-3f) * std::max(ext.y, 1e-3f) * std::max(ext.z, 1e-3f) / cells);
    const glm::uvec3 n = glm::clamp(glm::uvec3(glm::ceil(ext / std::max(cell, 1e-3f))), glm::uvec3(1u), glm::uvec3(64u));
    auto spread = [](uint32_t x) {
        x &= 0x3FFu;
        x = (x | (x << 16)) & 0x030000FFu;
        x = (x | (x << 8)) & 0x0300F00Fu;
        x = (x | (x << 4)) & 0x030C30C3u;
        x = (x | (x << 2)) & 0x09249249u;
        return x;
    };
    std::vector<std::pair<uint32_t, uint32_t>> key(tris); // (Morton, triangulo)
    for (size_t t = 0; t < tris; ++t) {
        const Vec3 c = (P(idx[t * 3]) + P(idx[t * 3 + 1]) + P(idx[t * 3 + 2])) / 3.0f;
        const glm::uvec3 g = glm::min(glm::uvec3(glm::max((c - lo) / ext * Vec3(n), Vec3(0.0f))), n - 1u);
        key[t] = {spread(g.x) | (spread(g.y) << 1) | (spread(g.z) << 2), uint32_t(t)};
    }
    std::stable_sort(key.begin(), key.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint32_t> sorted(idx.size());
    for (size_t i = 0; i < tris; ++i) {
        const uint32_t t = key[i].second;
        for (int k = 0; k < 3; ++k) sorted[i * 3 + k] = idx[t * 3 + k];
        if (i == 0 || key[i].first != key[i - 1].first)
            out.push_back({Vec3(1e30f), Vec3(-1e30f), uint32_t(i * 3), 0u});
        MeshCluster& c = out.back();
        c.count += 3;
        for (int k = 0; k < 3; ++k) { c.mn = glm::min(c.mn, P(sorted[i * 3 + k])); c.mx = glm::max(c.mx, P(sorted[i * 3 + k])); }
    }
    idx.swap(sorted);
    return out;
}

} // namespace eruption
