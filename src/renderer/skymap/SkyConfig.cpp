#include "renderer/skymap/SkyConfig.hpp"
#include <glm/gtc/type_ptr.hpp>

namespace eruption {

static Vec3 readVec3(const json& j, const Vec3& defaultValue) {
    if (!j.is_array() || j.size() < 3) return defaultValue;
    try {
        return Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    } catch (...) {
        return defaultValue;
    }
}

void SkyConfig::loadFromJson(const json& j) {
    if (!j.is_object()) return;

    if (j.contains("backend")) {
        backend = parseBackend(j["backend"].get<std::string>());
    }

    if (j.contains("procedural") && j["procedural"].is_object()) {
        const auto& p = j["procedural"];
        procedural.starDensity = p.value("star_density", procedural.starDensity);
        procedural.starTwinkleSpeed = p.value("star_twinkle_speed", procedural.starTwinkleSpeed);
        procedural.cloudCoverage = p.value("cloud_coverage", procedural.cloudCoverage);
        procedural.cloudSpeed = p.value("cloud_speed", procedural.cloudSpeed);
        procedural.cloudScale = p.value("cloud_scale", procedural.cloudScale);
        procedural.cloudLightness = p.value("cloud_lightness", procedural.cloudLightness);
        procedural.cloudShade = p.value("cloud_shade", procedural.cloudShade);
        procedural.cloudSoftness = p.value("cloud_softness", procedural.cloudSoftness);
        procedural.cloudThickness = p.value("cloud_thickness", procedural.cloudThickness);
        procedural.stormTint = p.value("storm_tint", procedural.stormTint);
        procedural.sunRayCount = p.value("sun_ray_count", procedural.sunRayCount);
        procedural.moonPhaseOffset = p.value("moon_phase_offset", procedural.moonPhaseOffset);
        procedural.sunSize = p.value("sun_size", procedural.sunSize);
        procedural.moonSize = p.value("moon_size", procedural.moonSize);
        procedural.sunLimbDarkening = p.value("sun_limb_darkening", procedural.sunLimbDarkening);
        procedural.sunHaloIntensity = p.value("sun_halo_intensity", procedural.sunHaloIntensity);
        procedural.sunHaloRays = p.value("sun_halo_rays", procedural.sunHaloRays);
        procedural.sunHaloSize = p.value("sun_halo_size", procedural.sunHaloSize);
        procedural.moonPhase = p.value("moon_phase", procedural.moonPhase);
        procedural.moonPhaseAuto = p.value("moon_phase_auto", procedural.moonPhaseAuto);
        procedural.enableStars = p.value("enable_stars", procedural.enableStars);
        procedural.enableClouds = p.value("enable_clouds", procedural.enableClouds);
        procedural.debugShowCloudsOnly = p.value("debug_show_clouds_only", procedural.debugShowCloudsOnly);
        if (p.contains("weather_type")) {
            procedural.weatherType = parseWeatherType(p["weather_type"].get<std::string>());
        } else if (p.contains("weather_preset")) {
            procedural.weatherType = parseWeatherType(p["weather_preset"].get<std::string>());
        }
        if (p.contains("weather") && p["weather"].is_object()) {
            procedural.weather.loadFromJson(p["weather"]);
        }
    }

    if (j.contains("cloudfluff") && j["cloudfluff"].is_object()) {
        const auto& c = j["cloudfluff"];
        cloudfluff.enabled = c.value("enabled", cloudfluff.enabled);
        cloudfluff.tier = c.value("tier", cloudfluff.tier);
        cloudfluff.maxClouds = c.value("max_clouds", cloudfluff.maxClouds);
        cloudfluff.displacementScale = c.value("displacement_scale", cloudfluff.displacementScale);
        cloudfluff.noiseTexScale = c.value("noise_tex_scale", cloudfluff.noiseTexScale);
        cloudfluff.shadowMapSize = c.value("shadow_map_size", cloudfluff.shadowMapSize);
        cloudfluff.windSpeed = c.value("wind_speed", cloudfluff.windSpeed);
        cloudfluff.windDirection = readVec3(c.value("wind_direction", json()), cloudfluff.windDirection);
        cloudfluff.scale = readVec3(c.value("scale", json()), cloudfluff.scale);
        cloudfluff.coverage = c.value("coverage", cloudfluff.coverage);
        cloudfluff.opacity = c.value("opacity", cloudfluff.opacity);
        cloudfluff.weatherReactionSpeed = c.value("weather_reaction_speed", cloudfluff.weatherReactionSpeed);
        cloudfluff.worldBoundsMin = readVec3(c.value("world_bounds_min", json()), cloudfluff.worldBoundsMin);
        cloudfluff.worldBoundsMax = readVec3(c.value("world_bounds_max", json()), cloudfluff.worldBoundsMax);
        cloudfluff.heightMin = c.value("height_min", cloudfluff.heightMin);
        cloudfluff.heightMax = c.value("height_max", cloudfluff.heightMax);
        cloudfluff.distanceMin = c.value("distance_min", cloudfluff.distanceMin);
        cloudfluff.distanceMax = c.value("distance_max", cloudfluff.distanceMax);
        cloudfluff.castShadows = c.value("cast_shadows", cloudfluff.castShadows);
        cloudfluff.cloudShadowIntensity = c.value("cloud_shadow_intensity", cloudfluff.cloudShadowIntensity);
        cloudfluff.cloudShadowMapSize = c.value("cloud_shadow_map_size", cloudfluff.cloudShadowMapSize);
        cloudfluff.enablePrecipSource = c.value("enable_precip_source", cloudfluff.enablePrecipSource);
        cloudfluff.autoPreset = c.value("auto_preset", cloudfluff.autoPreset);
        cloudfluff.coverageMapBackend = c.value("coverage_map_backend", cloudfluff.coverageMapBackend);
        cloudfluff.coverageMapSize = c.value("coverage_map_size", cloudfluff.coverageMapSize);
        cloudfluff.coverageMapOctaves = c.value("coverage_map_octaves", cloudfluff.coverageMapOctaves);
        cloudfluff.coverageMapCoverage = c.value("coverage_map_coverage", cloudfluff.coverageMapCoverage);
        cloudfluff.coverageMapSeed = c.value("coverage_map_seed", cloudfluff.coverageMapSeed);
        cloudfluff.spawnInViewChance = c.value("spawn_in_view_chance", cloudfluff.spawnInViewChance);
        cloudfluff.verticalSpreadAngle = c.value("vertical_spread_angle", cloudfluff.verticalSpreadAngle);
    }

    if (j.contains("volumetric") && j["volumetric"].is_object()) {
        const auto& v = j["volumetric"];
        volumetric.enabled = v.value("enabled", volumetric.enabled);
        std::string q = v.value("quality", "medium");
        if (q == "low") volumetric.quality = 0;
        else if (q == "high") volumetric.quality = 2;
        else volumetric.quality = 1;
    }
}

SkyBackendType SkyConfig::parseBackend(const std::string& name) {
    if (name == "cloudlayers") return SkyBackendType::CloudLayers;
    if (name == "volumetric") return SkyBackendType::Volumetric;
    if (name == "cloudfluff") return SkyBackendType::CloudFluff;
    return SkyBackendType::Procedural;
}

const char* SkyConfig::backendName(SkyBackendType type) {
    switch (type) {
        case SkyBackendType::Procedural: return "procedural";
        case SkyBackendType::CloudLayers: return "cloudlayers";
        case SkyBackendType::Volumetric: return "volumetric";
        case SkyBackendType::CloudFluff: return "cloudfluff";
        default: return "unknown";
    }
}

const char* SkyConfig::qualityName(int quality) {
    switch (quality) {
        case 0: return "low";
        case 2: return "high";
        default: return "medium";
    }
}

WeatherType SkyConfig::parseWeatherType(const std::string& name) {
    return eruption::weatherTypeFromName(name);
}

const char* SkyConfig::weatherTypeName(WeatherType type) {
    return eruption::weatherTypeName(type);
}

json SkyConfig::toJson() const {
    json j;
    j["backend"] = backendName(backend);

    j["procedural"] = {
        {"star_density", procedural.starDensity},
        {"star_twinkle_speed", procedural.starTwinkleSpeed},
        {"cloud_coverage", procedural.cloudCoverage},
        {"cloud_speed", procedural.cloudSpeed},
        {"cloud_scale", procedural.cloudScale},
        {"cloud_lightness", procedural.cloudLightness},
        {"cloud_shade", procedural.cloudShade},
        {"cloud_softness", procedural.cloudSoftness},
        {"cloud_thickness", procedural.cloudThickness},
        {"storm_tint", procedural.stormTint},
        {"sun_ray_count", procedural.sunRayCount},
        {"moon_phase_offset", procedural.moonPhaseOffset},
        {"sun_size", procedural.sunSize},
        {"moon_size", procedural.moonSize},
        {"sun_limb_darkening", procedural.sunLimbDarkening},
        {"sun_halo_intensity", procedural.sunHaloIntensity},
        {"sun_halo_rays", procedural.sunHaloRays},
        {"sun_halo_size", procedural.sunHaloSize},
        {"moon_phase", procedural.moonPhase},
        {"moon_phase_auto", procedural.moonPhaseAuto},
        {"enable_stars", procedural.enableStars},
        {"enable_clouds", procedural.enableClouds},
        {"debug_show_clouds_only", procedural.debugShowCloudsOnly},
        {"weather_type", weatherTypeName(procedural.weatherType)},
        {"weather", procedural.weather.toJson()}
    };

    j["cloudfluff"] = {
        {"enabled", cloudfluff.enabled},
        {"tier", cloudfluff.tier},
        {"max_clouds", cloudfluff.maxClouds},
        {"displacement_scale", cloudfluff.displacementScale},
        {"noise_tex_scale", cloudfluff.noiseTexScale},
        {"shadow_map_size", cloudfluff.shadowMapSize},
        {"wind_speed", cloudfluff.windSpeed},
        {"wind_direction", {cloudfluff.windDirection.x, cloudfluff.windDirection.y, cloudfluff.windDirection.z}},
        {"scale", {cloudfluff.scale.x, cloudfluff.scale.y, cloudfluff.scale.z}},
        {"coverage", cloudfluff.coverage},
        {"opacity", cloudfluff.opacity},
        {"weather_reaction_speed", cloudfluff.weatherReactionSpeed},
        {"world_bounds_min", {cloudfluff.worldBoundsMin.x, cloudfluff.worldBoundsMin.y, cloudfluff.worldBoundsMin.z}},
        {"world_bounds_max", {cloudfluff.worldBoundsMax.x, cloudfluff.worldBoundsMax.y, cloudfluff.worldBoundsMax.z}},
        {"height_min", cloudfluff.heightMin},
        {"height_max", cloudfluff.heightMax},
        {"distance_min", cloudfluff.distanceMin},
        {"distance_max", cloudfluff.distanceMax},
        {"cast_shadows", cloudfluff.castShadows},
        {"cloud_shadow_intensity", cloudfluff.cloudShadowIntensity},
        {"cloud_shadow_map_size", cloudfluff.cloudShadowMapSize},
        {"enable_precip_source", cloudfluff.enablePrecipSource},
        {"auto_preset", cloudfluff.autoPreset},
        {"coverage_map_backend", cloudfluff.coverageMapBackend},
        {"coverage_map_size", cloudfluff.coverageMapSize},
        {"coverage_map_octaves", cloudfluff.coverageMapOctaves},
        {"coverage_map_coverage", cloudfluff.coverageMapCoverage},
        {"coverage_map_seed", cloudfluff.coverageMapSeed},
        {"spawn_in_view_chance", cloudfluff.spawnInViewChance},
        {"vertical_spread_angle", cloudfluff.verticalSpreadAngle}
    };

    j["volumetric"] = {
        {"enabled", volumetric.enabled},
        {"quality", qualityName(volumetric.quality)}
    };

    return j;
}

} // namespace eruption
