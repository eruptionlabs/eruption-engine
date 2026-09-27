#pragma once

#include "math/Types.hpp"
#include "renderer/WeatherTypes.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace eruption {

using json = nlohmann::json;

enum class SkyBackendType {
    Procedural,
    CloudLayers,
    Volumetric,
    // CloudFluff kept for legacy config compatibility only.
    CloudFluff,
    Count
};

struct ProceduralSkyParams {
    float starDensity = 0.7f;
    float starTwinkleSpeed = 2.5f;
    float cloudCoverage = 0.35f;
    float cloudSpeed = 0.03f;
    float cloudScale = 1.5f;
    float cloudLightness = 1.0f;
    float cloudShade = 0.55f;
    float cloudSoftness = 0.5f;
    float cloudThickness = 0.5f;
    float stormTint = 0.0f;
    int sunRayCount = 12;
    float moonPhaseOffset = 0.12f;
    float sunSize = 1.0f;
    float moonSize = 1.0f;
    float sunLimbDarkening = 0.6f;
    float sunHaloIntensity = 0.35f;
    float sunHaloRays = 4.0f;
    float sunHaloSize = 1.0f;
    float moonPhase = 0.5f;
    bool moonPhaseAuto = true;
    bool enableStars = true;
    bool enableClouds = true;
    bool debugShowCloudsOnly = false;
    WeatherType weatherType = WeatherType::Clear;
    WeatherParams weather;
};

struct CloudFluffParams {
    bool enabled = true;
    std::string tier = "medium";  // mobile, medium, high
    int maxClouds = 128;
    float displacementScale = 1.0f;
    float noiseTexScale = 0.05f;
    int shadowMapSize = 512;
    float windSpeed = 0.25f;
    Vec3 windDirection = Vec3(1.0f, 0.0f, -0.5f);
    Vec3 scale = Vec3(60.0f, 20.0f, 60.0f);
    float coverage = 0.8f;
    float opacity = 2.0f;
    float weatherReactionSpeed = 0.5f;
    Vec3 worldBoundsMin = Vec3(-3000.0f, 0.0f, -3000.0f);
    Vec3 worldBoundsMax = Vec3(3000.0f, 2000.0f, 3000.0f);
    float heightMin = 80.0f;
    float heightMax = 400.0f;
    float distanceMin = 150.0f;
    float distanceMax = 800.0f;
    bool castShadows = false;
    float cloudShadowIntensity = 0.5f;      // 0..1 darkness of cloud shadows
    int cloudShadowMapSize = 1024;          // cloud shadow map resolution
    bool enablePrecipSource = true;
    bool autoPreset = true; // apply a CloudFluff preset when weather type changes

    // 2D cloud coverage map settings.
    std::string coverageMapBackend = "gpu"; // "cpu" or "gpu"
    uint32_t coverageMapSize = 1024;        // texture size (power of two)
    uint32_t coverageMapOctaves = 3;        // 0 = disabled, 1..5 = noise octaves
    float coverageMapCoverage = 0.5f;       // 0..1 threshold/coverage
    uint32_t coverageMapSeed = 12345u;      // noise seed

    // Spawn distribution tuning.
    float spawnInViewChance = 0.65f;        // probability of spawning inside the camera view cone
    float verticalSpreadAngle = 30.0f;      // half-angle (degrees) of the vertical cone spread
};

struct VolumetricParams {
    bool enabled = false;
    int quality = 1;
};

class SkyConfig {
public:
    SkyBackendType backend = SkyBackendType::Procedural;
    ProceduralSkyParams procedural;
    CloudFluffParams cloudfluff;
    VolumetricParams volumetric;

    void loadFromJson(const json& j);
    json toJson() const;

    static SkyBackendType parseBackend(const std::string& name);
    static const char* backendName(SkyBackendType type);
    static const char* qualityName(int quality);
    static WeatherType parseWeatherType(const std::string& name);
    static const char* weatherTypeName(WeatherType type);
};

} // namespace eruption
