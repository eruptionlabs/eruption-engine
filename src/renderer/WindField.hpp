#pragma once

#include "math/Types.hpp"

namespace eruption {

// -----------------------------------------------------------------------------
// WindField (IGNIS, G6)
// -----------------------------------------------------------------------------
// Before this, the engine had no wind. It had three unrelated things that each
// pretended to be wind:
//
//   1. WeatherSystem::windDirection()/windStrength() -- DEAD CODE. Nothing in
//      the engine ever called either one. Worse, the value was degenerate:
//      `m_windDirection = Vec3(rainWind, 0, 0)`, i.e. always along +/-X, never
//      any Z component, so it could not have described a real direction anyway.
//   2. CloudLayerRenderer::Config::windDirection/windSpeed -- the only wind that
//      actually moved anything (cloud coverage drift, rain box travel). Loaded
//      from data/cloud_layer_config.json.
//   3. WeatherParams::rainWind / snowWind -- scalar precipitation slant, with no
//      relation to either of the above.
//
// So clouds could drift north while rain slanted east. This class is the single
// source of truth: it takes the prevailing direction (from the cloud config, so
// existing authored setups keep their look) and the strength (from the weather
// signature, so an Aristotelian storm is windy *because* it is a storm), and
// produces one coherent wind that every consumer reads.
//
// Wind is not a constant vector. Real wind has a prevailing direction that
// wanders, a gust envelope that pulses, and gust fronts that travel across the
// terrain -- which is why a field of grass ripples instead of leaning. This
// models all three, cheaply and deterministically.
class WindField {
public:
    // seconds: frame delta. prevailingDir: authored direction (XZ used, need not
    // be normalized). targetStrength: 0..1 from the weather signature.
    void update(float dt, const Vec3& prevailingDir, float targetStrength);

    // Uniform wind: prevailing direction times the current gust envelope. Use
    // for things that do not care where they are (cloud drift, rain slant).
    Vec3 velocity() const { return m_direction * m_strength; }

    const Vec3& direction() const { return m_direction; } // normalized, XZ plane
    float strength() const { return m_strength; }         // 0..1, gusts included

    // Spatially varying wind at a world position. Gust fronts travel downwind,
    // so two points along the wind axis see the same gust at different times --
    // this is what makes vegetation ripple rather than sway in lockstep.
    Vec3 velocityAt(const Vec3& worldPos) const;

    // 0..1 gust envelope alone, without direction. Handy for anything that just
    // needs "how hard is it blowing right now".
    float gust() const { return m_gust; }

    // Wind drives evaporation: moving air carries the saturated boundary layer
    // away, so a wet surface dries faster. Feeds the drying model (G4).
    //
    // The coefficient was 1.5, which made wind almost irrelevant: a measurable
    // breeze changed drying time by ~2%, when in reality moving air is one of
    // the strongest terms in evaporation - it is why laundry dries on a windy
    // day and sulks on a still one. At 3.0 a full gale dries roughly four times
    // faster than dead calm, which is the right order.
    float evaporationFactor() const { return 1.0f + m_strength * 3.0f; }

private:
    Vec3  m_direction = Vec3(1.0f, 0.0f, 0.0f); // normalized, y == 0
    float m_strength  = 0.0f;                   // base * gust
    float m_base      = 0.0f;                   // smoothed target strength
    float m_gust      = 1.0f;                   // gust multiplier around 1.0
    float m_time      = 0.0f;
    float m_dirAngle  = 0.0f;                   // radians, current heading
    float m_dirTarget = 0.0f;                   // radians, heading it wanders to
};

} // namespace eruption
