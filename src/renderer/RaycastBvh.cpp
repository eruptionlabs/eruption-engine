#include "renderer/RaycastBvh.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace eruption {

namespace {
constexpr uint32_t kLeafMax = 8;  // poucos triangulos: a varredura linear da folha e' mais barata que descer mais
}

void RaycastBvh::build(const std::vector<ModelVertex>& vertices, const std::vector<uint32_t>& indices) {
    m_tris.clear();
    m_centroid.clear();
    m_order.clear();
    m_nodes.clear();
    const size_t triCount = indices.empty() ? vertices.size() / 3 : indices.size() / 3;
    if (triCount == 0) return;
    m_tris.reserve(triCount);
    m_centroid.reserve(triCount);
    auto addTri = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        m_tris.push_back({a, b, c});
        m_centroid.push_back((a + b + c) * (1.0f / 3.0f));
    };
    if (!indices.empty()) {
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
            if (a < vertices.size() && b < vertices.size() && c < vertices.size())
                addTri(vertices[a].position, vertices[b].position, vertices[c].position);
        }
    } else {
        for (size_t i = 0; i + 2 < vertices.size(); i += 3)
            addTri(vertices[i].position, vertices[i + 1].position, vertices[i + 2].position);
    }
    if (m_tris.empty()) return;
    m_order.resize(m_tris.size());
    for (uint32_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
    m_nodes.reserve(m_tris.size() * 2);
    buildRange(0, static_cast<uint32_t>(m_order.size()));
}

int32_t RaycastBvh::buildRange(uint32_t begin, uint32_t end) {
    Vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
    for (uint32_t i = begin; i < end; ++i) {
        const Tri& t = m_tris[m_order[i]];
        bmin = glm::min(bmin, glm::min(t.a, glm::min(t.b, t.c)));
        bmax = glm::max(bmax, glm::max(t.a, glm::max(t.b, t.c)));
    }
    const uint32_t nodeIdx = static_cast<uint32_t>(m_nodes.size());
    m_nodes.push_back(Node{});
    Node node;
    node.bmin = bmin;
    node.bmax = bmax;
    const uint32_t count = end - begin;
    if (count <= kLeafMax) {
        node.left = -1;
        node.start = begin;
        node.count = count;
        m_nodes[nodeIdx] = node;
        return static_cast<int32_t>(nodeIdx);
    }
    // Eixo mais largo da caixa; mediana dos centroides nesse eixo (nth_element:
    // O(n), sem exigir uma SAH completa - a malha e' estatica, construida uma
    // vez por asset e reusada por toda a vida do processo).
    const Vec3 ext = bmax - bmin;
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    const uint32_t mid = begin + count / 2;
    std::nth_element(m_order.begin() + begin, m_order.begin() + mid, m_order.begin() + end,
                     [&](uint32_t ia, uint32_t ib) { return m_centroid[ia][axis] < m_centroid[ib][axis]; });
    const int32_t left = buildRange(begin, mid);
    const int32_t right = buildRange(mid, end);
    node.left = left;
    node.right = right;
    node.start = 0;
    node.count = 0;
    m_nodes[nodeIdx] = node;
    return static_cast<int32_t>(nodeIdx);
}

namespace {
// Slab test: [tmin, tmax] da entrada/saida na caixa, ou false se o raio nao
// cruza (dir pode ter componentes 0; 1/0 = +-inf se comporta pela regra do
// IEEE754, que e' exatamente o slab test padrao).
bool rayBox(const Vec3& o, const Vec3& invDir, const Vec3& bmin, const Vec3& bmax, float tMaxIn, float& tMinOut) {
    float t0 = 0.0f, t1 = tMaxIn;
    for (int a = 0; a < 3; ++a) {
        float tn = (bmin[a] - o[a]) * invDir[a];
        float tf = (bmax[a] - o[a]) * invDir[a];
        if (tn > tf) std::swap(tn, tf);
        t0 = std::max(t0, tn);
        t1 = std::min(t1, tf);
        if (t0 > t1) return false;
    }
    tMinOut = t0;
    return true;
}

// Moller-Trumbore dos dois lados, igual ao ModelRenderer::raycastInstance
// original.
bool rayTri(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c, float& tOut) {
    const Vec3 e1 = b - a, e2 = c - a, pv = glm::cross(d, e2);
    const float det = glm::dot(e1, pv);
    if (std::abs(det) < 1e-12f) return false;
    const float invDet = 1.0f / det;
    const Vec3 tv = o - a;
    const float u = glm::dot(tv, pv) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 qv = glm::cross(tv, e1);
    const float v = glm::dot(d, qv) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = glm::dot(e2, qv) * invDet;
    if (t <= 0.0f) return false;
    tOut = t;
    return true;
}
}  // namespace

float RaycastBvh::raycastClosest(const Vec3& origin, const Vec3& dir) const {
    if (m_nodes.empty()) return -1.0f;
    const Vec3 invDir(dir.x != 0.0f ? 1.0f / dir.x : FLT_MAX, dir.y != 0.0f ? 1.0f / dir.y : FLT_MAX,
                      dir.z != 0.0f ? 1.0f / dir.z : FLT_MAX);
    float best = FLT_MAX;
    // Pilha fixa: a arvore e' balanceada por mediana (profundidade ~log2(N)); 64 cobre ate' malhas gigantes.
    int32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = m_nodes[static_cast<uint32_t>(stack[--sp])];
        float tEntry;
        if (!rayBox(origin, invDir, n.bmin, n.bmax, best, tEntry)) continue;
        if (n.left < 0) {
            for (uint32_t i = n.start; i < n.start + n.count; ++i) {
                const Tri& t = m_tris[m_order[i]];
                float hit;
                if (rayTri(origin, dir, t.a, t.b, t.c, hit) && hit < best) best = hit;
            }
            continue;
        }
        stack[sp++] = n.left;
        stack[sp++] = n.right;
    }
    return best == FLT_MAX ? -1.0f : best;
}

}  // namespace eruption
