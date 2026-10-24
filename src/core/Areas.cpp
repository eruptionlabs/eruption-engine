// Áreas de gatilho (caixas nomeadas que os scripts observam) e objetos
// sólidos (o jogador não atravessa o pé deles).
#include "core/Engine.hpp"
#include "core/Logger.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdlib>
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
    for (uint32_t i = 0; i < m_solid.size() && i < insts.size(); ++i) {
        if (!m_solid[i] || !insts[i].enabled) continue;
        const ModelInstance& inst = insts[i];
        // Corpo do jogador (p.y até +20) contra a altura do objeto.
        if (p.y + 20.0f < inst.worldAabbMin.y || p.y > inst.worldAabbMax.y) continue;
        if (p.x < inst.worldAabbMin.x - radius || p.x > inst.worldAabbMax.x + radius ||
            p.z < inst.worldAabbMin.z - radius || p.z > inst.worldAabbMax.z + radius)
            continue;
        if (footprintContains(i, Vec2(p.x, p.z), radius)) return true;
    }
    return false;
}

bool Engine::footprintContains(uint32_t instance, const Vec2& p, float radius) const {
    Vec2 c[4];
    if (!m_modelRenderer.footprint(instance, c)) return false;
    // Ponto dentro do quadrilátero (convexo) crescido pelo raio.
    for (int e = 0; e < 4; ++e) {
        const Vec2 a = c[e], b = c[(e + 1) % 4];
        const Vec2 edge = b - a;
        const float len = glm::length(edge);
        if (len < 1e-4f) continue;
        const Vec2 n(edge.y / len, -edge.x / len); // normal para fora (cantos no sentido certo)
        if (glm::dot(p - a, n) > radius) return false;
    }
    return true;
}

int Engine::addSoundEmitter(const std::string& name, const std::string& file, const Vec3& position, float volume,
                            float range) {
    SoundEmitter e;
    e.name = name;
    e.file = file;
    e.position = position;
    e.volume = volume;
    e.range = std::max(1.0f, range);
    m_soundEmitters.push_back(std::move(e));
    return static_cast<int>(m_soundEmitters.size() - 1);
}

int Engine::findSoundEmitter(const std::string& name) const {
    for (size_t i = 0; i < m_soundEmitters.size(); ++i)
        if (m_soundEmitters[i].name == name && !m_soundEmitters[i].removed) return static_cast<int>(i);
    return -1;
}

void Engine::clearSoundEmitters(bool keepNeighbors) {
    for (auto& e : m_soundEmitters)
        if ((!keepNeighbors || !e.fromNeighbor) && e.playing) m_audio.stop(e.playing, 0.3f);
    if (!keepNeighbors) {
        m_soundEmitters.clear();
        return;
    }
    m_soundEmitters.erase(std::remove_if(m_soundEmitters.begin(), m_soundEmitters.end(),
                                         [](const SoundEmitter& e) { return !e.fromNeighbor; }),
                          m_soundEmitters.end());
}

void Engine::updateSoundEmitters() {
    // Toca quem está perto do ouvinte; longe (ou parado o jogo), solta o som.
    const Vec3 ear = m_camera.position();
    for (auto& e : m_soundEmitters) {
        const bool want = m_soundEmittersActive && !e.removed && !e.parked && !e.file.empty() &&
                          glm::length(e.position - ear) < e.range * 1.2f;
        static const bool debug = std::getenv("ERUPTION_DEBUG_SOUNDS") != nullptr;
        if (want && (!e.playing || !m_audio.playing(e.playing))) {
            e.playing = m_audio.playAt(e.file, e.position, e.volume, true, e.range);
            if (debug) ERUPTION_LOG_WARN("[SOUND] %s starts (%s)", e.name.c_str(), e.file.c_str());
        } else if (!want && e.playing) {
            m_audio.stop(e.playing, 0.5f);
            e.playing = 0;
            if (debug) ERUPTION_LOG_WARN("[SOUND] %s stops", e.name.c_str());
        }
    }
}

} // namespace eruption
