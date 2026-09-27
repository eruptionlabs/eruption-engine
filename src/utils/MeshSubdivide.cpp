#include "utils/MeshSubdivide.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace eruption {

namespace {

TerrainVertex lerpVertex(const TerrainVertex& a, const TerrainVertex& b) {
    TerrainVertex v = a; // herda todos os índices/flags do primeiro
    v.position = (a.position + b.position) * 0.5f;
    v.texCoord = (a.texCoord + b.texCoord) * 0.5f;
    v.normal = a.normal + b.normal;
    float len = glm::length(v.normal);
    v.normal = (len > 1e-6f) ? (v.normal / len) : a.normal;
    v.blendWeight = (a.blendWeight + b.blendWeight) * 0.5f;
    v.blendMaskUV = (a.blendMaskUV + b.blendMaskUV) * 0.5f;
    // Cor por vértice é iluminação cozida do mapa: interpolar evita degrau.
    auto ch = [](uint32_t c, int s) { return float((c >> s) & 0xFF); };
    uint32_t mixed = 0;
    for (int s = 0; s < 32; s += 8) {
        uint32_t m = static_cast<uint32_t>((ch(a.color, s) + ch(b.color, s)) * 0.5f);
        mixed |= (std::min(m, 255u) << s);
    }
    v.color = mixed;
    return v;
}

uint64_t edgeKey(uint32_t a, uint32_t b) {
    return (a < b) ? ((uint64_t(a) << 32) | b) : ((uint64_t(b) << 32) | a);
}

} // namespace

float medianEdgeLength(const std::vector<TerrainVertex>& vertices,
                       const std::vector<uint32_t>& indices) {
    if (indices.size() < 3) return 0.0f;
    std::vector<float> lens;
    lens.reserve(indices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vec3& p0 = vertices[indices[i]].position;
        const Vec3& p1 = vertices[indices[i + 1]].position;
        const Vec3& p2 = vertices[indices[i + 2]].position;
        lens.push_back(glm::length(p1 - p0));
        lens.push_back(glm::length(p2 - p1));
        lens.push_back(glm::length(p0 - p2));
    }
    if (lens.empty()) return 0.0f;
    std::nth_element(lens.begin(), lens.begin() + lens.size() / 2, lens.end());
    return lens[lens.size() / 2];
}

SubdivideResult subdivideMesh(const std::vector<TerrainVertex>& vertices,
                              const std::vector<uint32_t>& indices,
                              const SubdivideParams& params) {
    SubdivideResult out;
    if (vertices.empty() || indices.size() < 3) return out;

    std::vector<TerrainVertex> verts = vertices;
    std::vector<uint32_t> idx = indices;

    for (int level = 0; level < params.maxLevels; ++level) {
        // Só divide o triângulo que ainda está grosso: malha já densa fica
        // como está (subdivisão adaptativa, não uniforme).
        bool anySplit = false;
        std::vector<uint32_t> nextIdx;
        nextIdx.reserve(idx.size() * 4);
        std::unordered_map<uint64_t, uint32_t> midpoints;
        midpoints.reserve(idx.size());

        auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
            const uint64_t k = edgeKey(a, b);
            auto it = midpoints.find(k);
            if (it != midpoints.end()) return it->second;
            verts.push_back(lerpVertex(verts[a], verts[b]));
            uint32_t id = static_cast<uint32_t>(verts.size() - 1);
            midpoints.emplace(k, id);
            return id;
        };

        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const uint32_t i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
            const Vec3& p0 = verts[i0].position;
            const Vec3& p1 = verts[i1].position;
            const Vec3& p2 = verts[i2].position;
            const float e0 = glm::length(p1 - p0);
            const float e1 = glm::length(p2 - p1);
            const float e2 = glm::length(p0 - p2);
            if (std::max({e0, e1, e2}) <= params.targetEdge) {
                nextIdx.insert(nextIdx.end(), {i0, i1, i2});
                continue;
            }
            anySplit = true;
            // 1-para-4: os midpoints são compartilhados entre triângulos
            // vizinhos pelo mapa de arestas, então não abre fenda (T-junction).
            const uint32_t m01 = midpoint(i0, i1);
            const uint32_t m12 = midpoint(i1, i2);
            const uint32_t m20 = midpoint(i2, i0);
            nextIdx.insert(nextIdx.end(), {i0, m01, m20,
                                           m01, i1, m12,
                                           m20, m12, i2,
                                           m01, m12, m20});
        }
        if (!anySplit) break;
        if (nextIdx.size() / 3 > params.maxOutTriangles) return out; // estourou
        idx.swap(nextIdx);
    }

    if (verts.size() == vertices.size()) return out; // nada mudou

    // Passa-baixa nas POSIÇÕES: a forma grande fica, o ruído de alta
    // frequência sai (é ele que vira moiré na sombra). Média dos vizinhos
    // por aresta, peso = lowPassStrength.
    const float w = std::clamp(params.lowPassStrength, 0.0f, 1.0f);
    if (w > 0.0f) {
        std::vector<Vec3> acc(verts.size(), Vec3(0.0f));
        std::vector<uint32_t> cnt(verts.size(), 0);
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const uint32_t t[3] = {idx[i], idx[i + 1], idx[i + 2]};
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    if (a == b) continue;
                    acc[t[a]] += verts[t[b]].position;
                    cnt[t[a]]++;
                }
            }
        }
        for (size_t i = 0; i < verts.size(); ++i) {
            if (cnt[i] == 0) continue;
            const Vec3 avg = acc[i] / static_cast<float>(cnt[i]);
            verts[i].position = glm::mix(verts[i].position, avg, w);
        }
    }

    out.vertices = std::move(verts);
    out.indices = std::move(idx);
    out.valid = true;
    return out;
}

} // namespace eruption
