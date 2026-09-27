#include "renderer/WindField.hpp"

#include <glm/gtc/constants.hpp>
#include <cmath>

namespace eruption {

namespace {

// Cheap deterministic value noise. No dependency, no table, and stable across
// runs -- important because the perf harness compares screenshots between runs.
float hash1(float n) {
    return glm::fract(std::sin(n) * 43758.5453123f);
}

float valueNoise(float x) {
    float i = std::floor(x);
    float f = x - i;
    // Smoothstep interpolation: C1 continuous, so the gust envelope has no
    // velocity discontinuity (a linear lerp makes gusts "tick" audibly in
    // motion).
    float u = f * f * (3.0f - 2.0f * f);
    return glm::mix(hash1(i), hash1(i + 1.0f), u);
}

// Two octaves is enough for a gust envelope: a slow swell plus a faster flutter.
float fbm(float x) {
    return valueNoise(x) * 0.65f + valueNoise(x * 2.7f) * 0.35f;
}

} // namespace

void WindField::update(float dt, const Vec3& prevailingDir, float targetStrength) {
    if (dt <= 0.0f) return;
    m_time += dt;

    // --- Base strength -----------------------------------------------------
    // Ease toward the weather's wind level instead of snapping. Weather
    // transitions take seconds; wind that jumps reads as a bug.
    const float kStrengthEase = 0.6f;
    m_base += (glm::clamp(targetStrength, 0.0f, 1.0f) - m_base) *
              glm::min(1.0f, kStrengthEase * dt);

    // --- Prevailing direction ---------------------------------------------
    // The authored cloud-config direction sets the heading, so clouds and wind
    // finally agree. Flatten to XZ: wind is a horizontal phenomenon at this
    // scale, and a stray Y would tilt vegetation into the ground.
    Vec2 flat(prevailingDir.x, prevailingDir.z);
    if (glm::dot(flat, flat) > 1e-8f) {
        m_dirTarget = std::atan2(flat.y, flat.x);
    }

    // Real wind wanders around its prevailing heading rather than holding a
    // perfect bearing. Amplitude scales with strength: a gale is steadier than
    // a light breeze, which is why smoke on a calm day meanders.
    const float wanderAmp = glm::mix(0.55f, 0.12f, m_base); // radians
    float wander = (fbm(m_time * 0.07f) - 0.5f) * 2.0f * wanderAmp;
    float desired = m_dirTarget + wander;

    // Take the shortest way around the circle, or the heading spins the long
    // way when it crosses +/-pi.
    float delta = desired - m_dirAngle;
    while (delta >  glm::pi<float>()) delta -= glm::two_pi<float>();
    while (delta < -glm::pi<float>()) delta += glm::two_pi<float>();
    m_dirAngle += delta * glm::min(1.0f, 0.8f * dt);

    m_direction = Vec3(std::cos(m_dirAngle), 0.0f, std::sin(m_dirAngle));

    // --- Gusts -------------------------------------------------------------
    // Gustiness rises with strength: a storm does not blow at a constant rate,
    // it comes in slugs. Centred on 1.0 so the mean matches the base strength.
    float gustAmp = glm::mix(0.15f, 0.55f, m_base);
    m_gust = 1.0f + (fbm(m_time * 0.45f) - 0.5f) * 2.0f * gustAmp;
    m_gust = glm::max(m_gust, 0.0f);

    m_strength = glm::clamp(m_base * m_gust, 0.0f, 2.0f);
}

Vec3 WindField::velocityAt(const Vec3& worldPos) const {
    // Project the position onto the wind axis and use that as a phase offset, so
    // a gust front sweeps downwind across the map instead of the whole terrain
    // pulsing in unison. ~90 m per gust cell reads well at this world scale.
    const float kGustCellMetres = 90.0f;
    float along = glm::dot(Vec3(worldPos.x, 0.0f, worldPos.z), m_direction) / kGustCellMetres;

    // Subtract time so the pattern travels WITH the wind rather than against it.
    float local = fbm(m_time * 0.45f - along);
    float amp = glm::mix(0.15f, 0.55f, m_base);
    float g = glm::max(1.0f + (local - 0.5f) * 2.0f * amp, 0.0f);

    // A little cross-wind sway, out of phase, so gusts are not perfectly
    // axis-aligned. Without it long grass all leans on exactly one line.
    Vec3 cross = Vec3(-m_direction.z, 0.0f, m_direction.x);
    float lateral = (fbm(m_time * 0.31f - along * 0.7f + 13.7f) - 0.5f) * 0.35f * m_base;

    return (m_direction * g + cross * lateral) * m_base;
}

} // namespace eruption
