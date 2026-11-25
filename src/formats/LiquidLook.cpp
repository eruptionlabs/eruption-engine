#include "formats/LiquidLook.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace eruption {

namespace {

void readColor(const nlohmann::json& e, const char* key, bool& has, Vec3& out) {
    if (!e.contains(key) || !e[key].is_array() || e[key].size() < 3) return;
    for (int i = 0; i < 3; ++i)
        if (!e[key][i].is_number()) return;
    out = Vec3(e[key][0].get<float>(), e[key][1].get<float>(), e[key][2].get<float>());
    has = true;
}

void readNumber(const nlohmann::json& e, const char* key, float lo, float hi, float& out) {
    if (e.contains(key) && e[key].is_number()) out = std::clamp(e[key].get<float>(), lo, hi);
}

} // namespace

WaterLook parseWaterLook(const nlohmann::json& e) {
    WaterLook lk;
    readColor(e, "deepColor", lk.hasDeep, lk.deep);
    readColor(e, "shallowColor", lk.hasShallow, lk.shallow);
    readColor(e, "scatterColor", lk.hasScatter, lk.scatter);
    readNumber(e, "transparency", 0.0f, 1.0f, lk.transparency);
    readNumber(e, "refraction", 0.0f, 0.5f, lk.refraction);
    readNumber(e, "reflectivity", 0.0f, 1.0f, lk.reflectivity);
    readNumber(e, "roughness", 0.0f, 1.0f, lk.roughness);
    readNumber(e, "waveAmplitude", 0.0f, 4.0f, lk.waveAmplitude);
    readNumber(e, "waveFrequency", 0.01f, 8.0f, lk.waveFrequency);
    readNumber(e, "waveSpeed", 0.0f, 8.0f, lk.waveSpeed);
    readNumber(e, "clarity", 0.1f, 200.0f, lk.clarity);
    readNumber(e, "shoreFoam", 0.0f, 8.0f, lk.shoreFoam);
    readNumber(e, "caustics", 0.0f, 4.0f, lk.caustics);
    if (e.contains("foam") && e["foam"].is_boolean()) lk.foam = e["foam"].get<bool>() ? 1 : 0;
    return lk;
}

} // namespace eruption
