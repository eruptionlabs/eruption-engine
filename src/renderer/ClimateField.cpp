#include "renderer/ClimateField.hpp"
#include "core/Logger.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>

namespace eruption {

float ClimateField::influenceAt(const Region& r, const Vec3& worldPos) {
    // Distance in the XZ plane only. Climate is a property of WHERE you are on
    // the map, not how high you are standing; altitude enters separately through
    // the lapse rate, which is a different physical mechanism.
    const float dx = worldPos.x - r.center.x;
    const float dz = worldPos.z - r.center.z;
    const float dist = std::sqrt(dx * dx + dz * dz);

    if (dist <= r.radius) return glm::clamp(r.weight, 0.0f, 1.0f);

    const float falloff = glm::max(r.falloff, 1e-3f);
    // smoothstep and not a linear ramp: a linear edge puts a visible crease in
    // the climate where the gradient breaks, and any per-pixel consumer of this
    // field would show it as a hard ring on the ground.
    const float t = glm::clamp((dist - r.radius) / falloff, 0.0f, 1.0f);
    const float fade = 1.0f - (t * t * (3.0f - 2.0f * t));
    return glm::clamp(r.weight, 0.0f, 1.0f) * fade;
}

WeatherSignature ClimateField::sampleAt(const Vec3& worldPos) const {
    WeatherSignature s = m_base;

    for (const auto& r : m_regions) {
        const float w = influenceAt(r, worldPos);
        if (w > 0.001f) {
            s = lerpSignature(s, r.signature, w);
        }
    }

    if (m_lapseRate != 0.0f) {
        // Convert the blended thermal energy back to Celsius, cool it by
        // altitude, and re-normalise. Going through temperature rather than
        // scaling thermalEnergy directly keeps the lapse rate meaning what it
        // says: degrees per unit of height.
        float tempC = signatureTempToCelsius(s.thermalEnergy);
        tempC -= (worldPos.y - m_referenceAltitude) * m_lapseRate;
        s.thermalEnergy = celsiusToSignatureTemp(tempC);
    }

    return s;
}

bool ClimateField::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("[CLIMATE] %s: JSON invalido (%s); mapa fica com clima global",
                          path.c_str(), e.what());
        return false;
    }

    m_regions.clear();
    m_lapseRate = j.value("lapseRate", 0.0f);
    m_referenceAltitude = j.value("referenceAltitude", 0.0f);

    const auto regions = j.find("regions");
    if (regions == j.end() || !regions->is_array()) {
        ERUPTION_LOG_WARN("[CLIMATE] %s sem array 'regions'; mapa fica com clima global",
                          path.c_str());
        return false;
    }

    for (const auto& rj : *regions) {
        Region r;
        r.name = rj.value("name", std::string("<sem nome>"));
        if (rj.contains("center") && rj["center"].is_array() && rj["center"].size() >= 3) {
            r.center = Vec3(rj["center"][0].get<float>(),
                            rj["center"][1].get<float>(),
                            rj["center"][2].get<float>());
        }
        r.radius  = rj.value("radius", 100.0f);
        r.falloff = rj.value("falloff", 100.0f);
        r.weight  = glm::clamp(rj.value("weight", 1.0f), 0.0f, 1.0f);

        // The signature IS the authored content: you describe what the place
        // physically is, not what it should look like. Anything omitted keeps
        // the neutral default so a region can state only what makes it special
        // -- a volcano only has to say "hot and dry".
        const auto sj = rj.find("signature");
        if (sj != rj.end() && sj->is_object()) {
            WeatherSignature& g = r.signature;
            g.solar         = sj->value("solar", g.solar);
            g.cloudAmount   = sj->value("cloudAmount", g.cloudAmount);
            g.humidity      = sj->value("humidity", g.humidity);
            g.turbulence    = sj->value("turbulence", g.turbulence);
            g.thermalEnergy = sj->value("thermalEnergy", g.thermalEnergy);
            g.precipitation = sj->value("precipitation", g.precipitation);
            g.visibility    = sj->value("visibility", g.visibility);
            g.wind          = sj->value("wind", g.wind);

            const std::string pt = sj->value("precipType", std::string("none"));
            if      (pt == "rain")     g.precipType = PrecipitationType::Rain;
            else if (pt == "snow")     g.precipType = PrecipitationType::Snow;
            else if (pt == "hail")     g.precipType = PrecipitationType::Hail;
            else if (pt == "freezing") g.precipType = PrecipitationType::FreezingRain;
            else                       g.precipType = PrecipitationType::None;
        }
        m_regions.push_back(r);
    }

    ERUPTION_LOG_WARN("[CLIMATE] %s: %zu regiao(oes) carregada(s), lapseRate=%.4f",
                      path.c_str(), m_regions.size(), m_lapseRate);
    for (const auto& r : m_regions) {
        ERUPTION_LOG_WARN("[CLIMATE]   '%s' centro=(%.0f,%.0f,%.0f) raio=%.0f fade=%.0f "
                          "termica=%+.2f umid=%.2f vento=%.2f",
                          r.name.c_str(), r.center.x, r.center.y, r.center.z,
                          r.radius, r.falloff,
                          r.signature.thermalEnergy, r.signature.humidity, r.signature.wind);
    }
    return true;
}

} // namespace eruption
