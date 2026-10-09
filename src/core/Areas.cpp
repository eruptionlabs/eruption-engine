// Áreas de gatilho (caixas nomeadas que os scripts observam) e objetos
// sólidos (o jogador não atravessa o pé deles).
#include "core/Engine.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace eruption {

Mat4 Engine::TriggerArea::matrix() const {
    Mat4 m = glm::translate(Mat4(1.0f), center);
    m = glm::rotate(m, glm::radians(yaw), Vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(m, size);
}

void Engine::TriggerArea::setMatrix(const Mat4& m) {
    center = Vec3(m[3]);
    size = Vec3(glm::length(Vec3(m[0])), glm::length(Vec3(m[1])), glm::length(Vec3(m[2])));
    size = glm::max(size, Vec3(0.5f));
    const Vec3 x = Vec3(m[0]) / size.x;
    yaw = glm::degrees(std::atan2(-x.z, x.x));
    if (std::abs(yaw) < 1e-3f) yaw = 0.0f; // sem "-0" no Inspetor
}

bool Engine::TriggerArea::contains(const Vec3& p) const {
    // No espaço da caixa (centro na origem, sem giro).
    const float a = glm::radians(yaw);
    const Vec3 d = p - center;
    const float lx = d.x * std::cos(a) - d.z * std::sin(a);
    const float lz = d.x * std::sin(a) + d.z * std::cos(a);
    return std::abs(lx) <= size.x * 0.5f && std::abs(lz) <= size.z * 0.5f && std::abs(d.y) <= size.y * 0.5f;
}

int Engine::addArea(const std::string& name, const Vec3& center, const Vec3& size, float yawDeg) {
    TriggerArea a;
    a.name = name;
    a.center = center;
    a.size = size;
    a.yaw = yawDeg;
    m_areas.push_back(std::move(a));
    return static_cast<int>(m_areas.size() - 1);
}

int Engine::findArea(const std::string& name) const {
    for (size_t i = 0; i < m_areas.size(); ++i)
        if (m_areas[i].name == name && !m_areas[i].removed && !m_areas[i].parked) return static_cast<int>(i);
    return -1;
}

void Engine::clearAreas(bool keepNeighbors) {
    if (!keepNeighbors) {
        m_areas.clear();
        return;
    }
    m_areas.erase(std::remove_if(m_areas.begin(), m_areas.end(), [](const TriggerArea& a) { return !a.fromNeighbor; }),
                  m_areas.end());
}

void Engine::setSolid(uint32_t instance, bool solid) {
    if (instance >= m_solid.size()) {
        if (!solid) return;
        m_solid.resize(instance + 1, 0);
    }
    m_solid[instance] = solid ? 1 : 0;
}

bool Engine::isSolid(uint32_t instance) const { return instance < m_solid.size() && m_solid[instance]; }

bool Engine::blockedAt(const Vec3& p, float radius) const {
    const auto& insts = m_modelRenderer.getInstances();
    Vec2 c[4];
    for (uint32_t i = 0; i < m_solid.size() && i < insts.size(); ++i) {
        if (!m_solid[i] || !insts[i].enabled) continue;
        const ModelInstance& inst = insts[i];
        // Corpo do jogador (p.y até +20) contra a altura do objeto.
        if (p.y + 20.0f < inst.worldAabbMin.y || p.y > inst.worldAabbMax.y) continue;
        if (p.x < inst.worldAabbMin.x - radius || p.x > inst.worldAabbMax.x + radius ||
            p.z < inst.worldAabbMin.z - radius || p.z > inst.worldAabbMax.z + radius)
            continue;
        if (!m_modelRenderer.footprint(i, c)) continue;
        // Ponto dentro do quadrilátero (convexo) crescido pelo raio.
        bool inside = true;
        for (int e = 0; e < 4 && inside; ++e) {
            const Vec2 a = c[e], b = c[(e + 1) % 4];
            const Vec2 edge = b - a;
            const float len = glm::length(edge);
            if (len < 1e-4f) continue;
            const Vec2 n(edge.y / len, -edge.x / len); // normal para fora (cantos no sentido certo)
            if (glm::dot(Vec2(p.x, p.z) - a, n) > radius) inside = false;
        }
        if (inside) return true;
    }
    return false;
}

} // namespace eruption
