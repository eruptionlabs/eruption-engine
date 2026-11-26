#include "core/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

// NEVOA LOCAL (world.fog_volume): um cilindro vertical de nevoa analitica, desenhado no
// composite (post_composite.frag, fogVolume): por pixel, o comprimento do raio dentro do
// cilindro vezes a densidade. Sem volume vivo o shader sai antes (um if) e a imagem e'
// identica. Aqui fica so' o estado: nasce em `grow` s, some em `fade` s depois do fim.

uint32_t Engine::fogVolumeStart(const FogVolumeSpec& spec) {
    if (m_fogVolumes.size() >= kMaxFogVolumes) return 0;
    FogVolumeState v;
    v.id = m_fogVolumeNext++;
    v.spec = spec;
    v.spec.radius = std::max(spec.radius, 0.1f);
    v.spec.height = std::max(spec.height, 0.1f);
    v.spec.density = std::clamp(spec.density, 0.0f, 50.0f);
    v.spec.grow = std::max(spec.grow, 0.0f);
    v.spec.fade = std::max(spec.fade, 0.05f);
    m_fogVolumes.push_back(v);
    return v.id;
}

bool Engine::fogVolumeMove(uint32_t id, const Vec3& pos) {
    for (FogVolumeState& v : m_fogVolumes)
        if (v.id == id) {
            v.spec.pos = pos;
            return true;
        }
    return false;
}

bool Engine::fogVolumeStop(uint32_t id) {
    for (FogVolumeState& v : m_fogVolumes)
        if (v.id == id) {
            if (v.endAge < 0.0f) v.endAge = v.age;
            return true;
        }
    return false;
}

float Engine::fogVolumeLevel(const FogVolumeState& v) {
    const float up = v.spec.grow > 0.0f ? std::clamp(v.age / v.spec.grow, 0.0f, 1.0f) : 1.0f;
    float k = up * up * (3.0f - 2.0f * up);
    if (v.endAge >= 0.0f) {
        const float u = std::clamp((v.age - v.endAge) / v.spec.fade, 0.0f, 1.0f);
        k *= 1.0f - u * u * (3.0f - 2.0f * u);
    }
    return k;
}

void Engine::updateFogVolumes(float dt) {
    m_fogVolumeClock = std::fmod(m_fogVolumeClock + dt, 6283.0f);
    if (m_fogVolumes.empty()) return;
    for (FogVolumeState& v : m_fogVolumes) {
        v.age += dt;
        if (v.endAge < 0.0f && v.spec.duration > 0.0f && v.age >= v.spec.duration) v.endAge = v.age;
    }
    m_fogVolumes.erase(std::remove_if(m_fogVolumes.begin(), m_fogVolumes.end(),
                                      [](const FogVolumeState& v) { return v.endAge >= 0.0f && v.age - v.endAge >= v.spec.fade; }),
                       m_fogVolumes.end());
}

void Engine::fillFogVolumes(PostProcessor::PostSettings& out) const {
    PostProcessor::PostSettings::FogVolumes& f = out.fogVolumes;
    f = PostProcessor::PostSettings::FogVolumes{};
    f.time = m_fogVolumeClock;
    int n = 0;
    for (const FogVolumeState& v : m_fogVolumes) {
        if (n >= PostProcessor::PostSettings::kMaxFogVolumes) break;
        const float k = fogVolumeLevel(v);
        if (k <= 0.0f) continue;
        f.shape[n] = Vec4(v.spec.pos.x, v.spec.pos.y, v.spec.pos.z, v.spec.radius);
        f.color[n] = Vec4(v.spec.color.x, v.spec.color.y, v.spec.color.z, (v.spec.density / std::max(m_vfx.unitsPerMeter(), 1e-3f)) * k);
        f.height[n] = v.spec.height;
        ++n;
    }
    f.count = n;
}

} // namespace eruption
