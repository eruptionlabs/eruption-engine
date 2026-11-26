#include "core/GroundVegetation.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace eruption::ground {

int vegChannelForName(const std::string& name) {
    if (name == "scorch") return 0;
    if (name == "frost" || name == "ice_water") return 1;
    if (name == "wet") return 2;
    if (name == "snow") return 3;
    return -1;
}

bool VegetationTable::loadFromJson(const std::string& text) {
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return false;
    const auto it = root.find("vegetation");
    if (it == root.end() || !it->is_object()) return true; // sem a chave: nada a sobrepor
    for (auto kv = it->begin(); kv != it->end(); ++kv) {
        const int c = vegChannelForName(kv.key());
        if (c < 0 || !kv.value().is_object()) continue;
        // "ice_water" so' vale se "frost" nao existe: o canal e' o mesmo.
        if (kv.key() == "ice_water" && it->contains("frost")) continue;
        const nlohmann::json& j = kv.value();
        VegEffect e = m_ch[c];
        e.ampScale = std::clamp(j.value("sway_amp_scale", e.ampScale), 0.0f, 4.0f);
        e.freqScale = std::clamp(j.value("sway_freq_scale", e.freqScale), 0.1f, 4.0f);
        e.densityBoost = std::clamp(j.value("density_boost", e.densityBoost), -0.5f, 0.95f);
        e.albedoLift = std::clamp(j.value("albedo_lift", e.albedoLift), 0.0f, 1.0f);
        m_ch[c] = e;
    }
    return true;
}

VegEffect VegetationTable::blend(const float cover[kVegChannels]) const {
    VegEffect r;
    for (int c = 0; c < kVegChannels; ++c) {
        const float k = std::clamp(cover[c], 0.0f, 1.0f);
        if (k <= 0.0f) continue;
        const VegEffect& e = m_ch[c];
        r.ampScale *= 1.0f + (e.ampScale - 1.0f) * k;
        r.freqScale *= 1.0f + (e.freqScale - 1.0f) * k;
        r.densityBoost += e.densityBoost * k;
        r.albedoLift += e.albedoLift * k;
    }
    r.densityBoost = std::clamp(r.densityBoost, -0.5f, 0.95f);
    r.albedoLift = std::clamp(r.albedoLift, 0.0f, 1.0f);
    return r;
}

bool VegetationTable::neutral() const {
    for (const VegEffect& e : m_ch)
        if (e.ampScale != 1.0f || e.freqScale != 1.0f || e.densityBoost != 0.0f || e.albedoLift != 0.0f) return false;
    return true;
}

} // namespace eruption::ground
