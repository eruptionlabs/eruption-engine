#pragma once

#include "renderer/WeatherTypes.hpp"
#include "math/Types.hpp"

#include <string>
#include <vector>

namespace eruption {

// -----------------------------------------------------------------------------
// ClimateField (IGNIS, G9)
// -----------------------------------------------------------------------------
// Until here the engine had exactly ONE weather state for the entire world: a
// single WeatherParams that every pixel and every system read. A map could be
// raining, or not raining, and that was the whole vocabulary.
//
// This makes climate a property of PLACE. The map carries regions, each one
// described by a physical signature rather than by a pile of sliders, and the
// field answers "what is the climate at this point" by blending them over a
// base (the global weather).
//
// The point of blending SIGNATURES instead of parameters is causal: halfway
// between a volcanic region and a temperate one is a physically meaningful
// state -- hot-ish, dry-ish, hazy-ish, all consistent with each other -- while
// halfway between two slider sets is just an average of symptoms that can
// easily be self-contradictory (heat shimmer in heavy rain, say).
//
// The Aristotelian point the author asked for: the volcano region is hot
// BECAUSE it is a volcano. You author that ONE fact -- thermalEnergy high,
// humidity low -- and heat shimmer, dry air, suppressed rain and fast
// evaporation all follow from it through applySignature(). You do not tune
// them independently and try to keep them consistent by hand.
class ClimateField {
public:
    struct Region {
        std::string name;
        Vec3 center = Vec3(0.0f);   // world position; XZ is what matters
        float radius = 100.0f;      // full-strength radius, metres
        float falloff = 100.0f;     // blend distance beyond the radius, metres
        float weight = 1.0f;        // 0..1 how strongly it overrides the base
        WeatherSignature signature; // the CAUSE: what this place physically is
    };

    void clear() { m_regions.clear(); }
    void addRegion(const Region& r) { m_regions.push_back(r); }
    const std::vector<Region>& regions() const { return m_regions; }
    std::vector<Region>& regions() { return m_regions; }
    bool empty() const { return m_regions.empty(); }

    // The world-wide weather, which every region blends on top of.
    void setBaseSignature(const WeatherSignature& s) { m_base = s; }
    const WeatherSignature& baseSignature() const { return m_base; }

    // Climate at a world position: the base with every region blended in by
    // its influence. Regions are applied in order, so a later region wins where
    // they overlap -- which is what you want for a small intense feature (a
    // crater) sitting inside a large mild one (a volcanic massif).
    WeatherSignature sampleAt(const Vec3& worldPos) const;

    // 0..1 influence of one region at a position. Exposed so debug UI and the
    // per-pixel upload (future) can visualise the field.
    static float influenceAt(const Region& r, const Vec3& worldPos);

    // Altitude cools the air. This is the environmental lapse rate, ~6.5 C per
    // 1000 m in the real atmosphere; scaled here by worldUnitsPerMetre since game
    // maps are not metric. Applied on top of the region blend, so a peak inside
    // a volcanic region is still cooler than its foot without anyone authoring
    // a second region for it.
    void setLapseRate(float degreesPerWorldUnit) { m_lapseRate = degreesPerWorldUnit; }
    void setReferenceAltitude(float y) { m_referenceAltitude = y; }

    // Load regions from the map's climate description. Missing file or bad JSON
    // is not fatal: the field simply stays empty and the world keeps its single
    // global climate, exactly as before.
    bool loadFromFile(const std::string& path);

private:
    std::vector<Region> m_regions;
    WeatherSignature m_base;
    float m_lapseRate = 0.0f;         // 0 = altitude does not affect temperature
    float m_referenceAltitude = 0.0f; // world Y treated as "sea level"
};

} // namespace eruption
