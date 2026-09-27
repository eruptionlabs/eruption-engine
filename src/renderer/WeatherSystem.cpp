#include "renderer/WeatherSystem.hpp"
#include "renderer/ClimateFuzzy.hpp"

#include "core/Logger.hpp"
#include <cstdlib>
#include "renderer/WeatherTypes.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace eruption {

#define LOAD(p, name) p = j.value(#name, p)

void WeatherParams::loadFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;

    if (j.contains("weather_type") && j["weather_type"].is_string()) {
        weatherType = weatherTypeFromName(j["weather_type"].get<std::string>());
    } else if (j.contains("weather_preset") && j["weather_preset"].is_string()) {
        // legacy: old preset names map to the closest new WeatherType
        const std::string& preset = j["weather_preset"].get<std::string>();
        if (preset == "partly_cloudy") weatherType = WeatherType::PartlyCloudy;
        else if (preset == "cloudy") weatherType = WeatherType::Cloudy;
        else if (preset == "rainy") weatherType = WeatherType::Rainy;
        else if (preset == "stormy") weatherType = WeatherType::Stormy;
        else if (preset == "heatwave") weatherType = WeatherType::Heatwave;
        else if (preset == "foggy") weatherType = WeatherType::Foggy;
        else if (preset == "snowy") weatherType = WeatherType::Snowy;
        else weatherType = WeatherType::Clear;
    }
    LOAD(weatherIntensity, weather_intensity);

    LOAD(rainIntensity, rain_intensity);
    LOAD(rainSpeed, rain_speed);
    LOAD(rainWind, rain_wind);
    LOAD(rainTurbulence, rain_turbulence);
    LOAD(dropLensAmount, drop_lens_amount);
    LOAD(rainSplashIntensity, rain_splash_intensity);
    LOAD(rainSplashAmount, rain_splash_amount);
    LOAD(rainSplashOpacity, rain_splash_opacity);
    LOAD(rainSplashRadius, rain_splash_radius);
    if (j.contains("rain_splash_enabled")) {
        rainSplashEnabled = j.value("rain_splash_enabled", rainSplashEnabled);
    } else {
        // legacy: any positive manual intensity meant the feature was on
        rainSplashEnabled = (rainSplashIntensity > 0.001f);
    }
    LOAD(heightmapBlur, heightmap_blur);
    LOAD(rainShadowSoftness, rain_shadow_softness);
    LOAD(rainStreakWidth, rain_streak_width);
    LOAD(rainStreakLength, rain_streak_length);
    LOAD(rainStreakEdge, rain_streak_edge);
    LOAD(rainStreakTipFade, rain_streak_tip_fade);
    LOAD(rainStreakAlpha, rain_streak_alpha);
    LOAD(rainStreakBrightness, rain_streak_brightness);
    LOAD(rainPixelFade, rain_pixel_fade);
    LOAD(snowIntensity, snow_intensity);
    LOAD(snowSpeed, snow_speed);
    LOAD(snowSize, snow_size);
    LOAD(snowWind, snow_wind);
    LOAD(snowTurbulence, snow_turbulence);
    LOAD(snowAccumulation, snow_accumulation);
    LOAD(heatShimmer, heat_shimmer);
    LOAD(heatSpeed, heat_speed);
    LOAD(heatScale, heat_scale);
    LOAD(heatWorldHeight, heat_world_height);
    LOAD(heatWorldHeightAuto, heat_world_height_auto);
    LOAD(heatWorldHeightOffset, heat_world_height_offset);
    LOAD(fogDensity, fog_density);
    LOAD(fogStart, fog_start);
    LOAD(fogEnd, fog_end);
    LOAD(fogHeight, fog_height);
    LOAD(fogHeightFalloff, fog_height_falloff);
    LOAD(stormTint, storm_tint);
    LOAD(cloudCoverage, cloud_coverage);
    LOAD(cloudSpeed, cloud_speed);
    LOAD(cloudThickness, cloud_thickness);
    LOAD(cloudScale, cloud_scale);
    LOAD(cloudLightness, cloud_lightness);
    LOAD(cloudShade, cloud_shade);
    LOAD(cloudSoftness, cloud_softness);
    LOAD(lightningChance, lightning_chance);
    LOAD(lightningFlash, lightning_flash);
    LOAD(transitionDuration, transition_duration);
    LOAD(temperatureC, temperature_c);
    LOAD(windiness, windiness);
    LOAD(lensDropCount, lens_drop_count);
    LOAD(lensDropRadius, lens_drop_radius);
    LOAD(windDebrisIntensity, wind_debris_intensity);
    LOAD(atmosphericTint, atmospheric_tint);
    LOAD(rainbowIntensity, rainbow_intensity);
    LOAD(mirageIntensity, mirage_intensity);
    LOAD(shootingStarIntensity, shooting_star_intensity);
    LOAD(tornadoIntensity, tornado_intensity);
    LOAD(dustDevilIntensity, dust_devil_intensity);
    LOAD(auroraIntensity, aurora_intensity);
    if (j.contains("lens_drop_mode") && j["lens_drop_mode"].is_string()) {
        const std::string& m = j["lens_drop_mode"].get<std::string>();
        if (m == "cpu") lensDropMode = LensDropMode::CpuBuffer;
        else if (m == "gpu") lensDropMode = LensDropMode::GpuCompute;
        else lensDropMode = LensDropMode::Procedural;
    }
}

nlohmann::json WeatherParams::toJson() const {
    return {
        {"weather_type", weatherTypeName(weatherType)},
        {"weather_intensity", weatherIntensity},
        {"rain_intensity", rainIntensity},
        {"rain_speed", rainSpeed},
        {"rain_wind", rainWind},
        {"rain_turbulence", rainTurbulence},
        {"drop_lens_amount", dropLensAmount},
        {"rain_splash_enabled", rainSplashEnabled},
        {"rain_splash_intensity", rainSplashIntensity},
        {"rain_splash_amount", rainSplashAmount},
        {"rain_splash_opacity", rainSplashOpacity},
        {"rain_splash_radius", rainSplashRadius},
        {"heightmap_blur", heightmapBlur},
        {"rain_shadow_softness", rainShadowSoftness},
        {"rain_streak_width", rainStreakWidth},
        {"rain_streak_length", rainStreakLength},
        {"rain_streak_edge", rainStreakEdge},
        {"rain_streak_tip_fade", rainStreakTipFade},
        {"rain_streak_alpha", rainStreakAlpha},
        {"rain_streak_brightness", rainStreakBrightness},
        {"rain_pixel_fade", rainPixelFade},
        {"snow_intensity", snowIntensity},
        {"snow_speed", snowSpeed},
        {"snow_size", snowSize},
        {"snow_wind", snowWind},
        {"snow_turbulence", snowTurbulence},
        {"snow_accumulation", snowAccumulation},
        {"heat_shimmer", heatShimmer},
        {"heat_speed", heatSpeed},
        {"heat_scale", heatScale},
        {"heat_world_height", heatWorldHeight},
        {"heat_world_height_auto", heatWorldHeightAuto},
        {"heat_world_height_offset", heatWorldHeightOffset},
        {"fog_density", fogDensity},
        {"fog_start", fogStart},
        {"fog_end", fogEnd},
        {"fog_height", fogHeight},
        {"fog_height_falloff", fogHeightFalloff},
        {"storm_tint", stormTint},
        {"cloud_coverage", cloudCoverage},
        {"cloud_speed", cloudSpeed},
        {"cloud_thickness", cloudThickness},
        {"cloud_scale", cloudScale},
        {"cloud_lightness", cloudLightness},
        {"cloud_shade", cloudShade},
        {"cloud_softness", cloudSoftness},
        {"lightning_chance", lightningChance},
        {"lightning_flash", lightningFlash},
        {"transition_duration", transitionDuration},
        {"temperature_c", temperatureC},
        {"windiness", windiness},
        {"lens_drop_count", lensDropCount},
        {"lens_drop_radius", lensDropRadius},
        {"lens_drop_mode", lensDropMode == LensDropMode::CpuBuffer ? "cpu" :
                           lensDropMode == LensDropMode::GpuCompute ? "gpu" : "procedural"},
        {"wind_debris_intensity", windDebrisIntensity},
        {"atmospheric_tint", atmosphericTint},
        {"rainbow_intensity", rainbowIntensity},
        {"mirage_intensity", mirageIntensity},
        {"shooting_star_intensity", shootingStarIntensity},
        {"tornado_intensity", tornadoIntensity},
        {"dust_devil_intensity", dustDevilIntensity},
        {"aurora_intensity", auroraIntensity}
    };
}

#define LERP(f) a.f + (b.f - a.f) * t

WeatherParams WeatherParams::lerp(const WeatherParams& a, const WeatherParams& b, float t) {
    WeatherParams r;
    r.weatherType = (t >= 0.5f) ? b.weatherType : a.weatherType;
    r.weatherIntensity = LERP(weatherIntensity);
    r.rainIntensity = LERP(rainIntensity);
    r.rainSpeed = LERP(rainSpeed);
    r.rainWind = LERP(rainWind);
    r.rainTurbulence = LERP(rainTurbulence);
    r.dropLensAmount = LERP(dropLensAmount);
    r.rainSplashIntensity = LERP(rainSplashIntensity);
    r.rainSplashAmount = LERP(rainSplashAmount);
    r.rainSplashOpacity = LERP(rainSplashOpacity);
    r.rainSplashRadius = LERP(rainSplashRadius);
    // Keep enabled flag from target during transition to avoid pops
    r.rainSplashEnabled = (t >= 0.5f) ? b.rainSplashEnabled : a.rainSplashEnabled;
    r.heightmapBlur = LERP(heightmapBlur);
    r.rainShadowSoftness = LERP(rainShadowSoftness);
    r.rainStreakWidth = LERP(rainStreakWidth);
    r.rainStreakLength = LERP(rainStreakLength);
    r.rainStreakEdge = LERP(rainStreakEdge);
    r.rainStreakTipFade = LERP(rainStreakTipFade);
    r.rainStreakAlpha = LERP(rainStreakAlpha);
    r.rainStreakBrightness = LERP(rainStreakBrightness);
    r.rainPixelFade = LERP(rainPixelFade);
    r.snowIntensity = LERP(snowIntensity);
    r.snowSpeed = LERP(snowSpeed);
    r.snowSize = LERP(snowSize);
    r.snowWind = LERP(snowWind);
    r.snowTurbulence = LERP(snowTurbulence);
    r.snowAccumulation = LERP(snowAccumulation);
    r.heatShimmer = LERP(heatShimmer);
    r.heatSpeed = LERP(heatSpeed);
    r.heatScale = LERP(heatScale);
    r.heatWorldHeight = LERP(heatWorldHeight);
    r.heatWorldHeightAuto = (t >= 0.5f) ? b.heatWorldHeightAuto : a.heatWorldHeightAuto;
    r.heatWorldHeightOffset = LERP(heatWorldHeightOffset);
    r.fogDensity = LERP(fogDensity);
    r.fogStart = LERP(fogStart);
    r.fogEnd = LERP(fogEnd);
    r.fogHeight = LERP(fogHeight);
    r.fogHeightFalloff = LERP(fogHeightFalloff);
    r.stormTint = LERP(stormTint);
    r.cloudCoverage = LERP(cloudCoverage);
    r.cloudSpeed = LERP(cloudSpeed);
    r.cloudThickness = LERP(cloudThickness);
    r.cloudScale = LERP(cloudScale);
    r.cloudLightness = LERP(cloudLightness);
    r.cloudShade = LERP(cloudShade);
    r.cloudSoftness = LERP(cloudSoftness);
    r.lightningChance = LERP(lightningChance);
    r.lightningFlash = LERP(lightningFlash);
    r.transitionDuration = LERP(transitionDuration);
    r.temperatureC = LERP(temperatureC);
    // windiness usa -1 como "nao autorado". Interpolar cru misturaria o
    // sentinela com um valor real e produziria vento negativo ou fantasma no
    // meio da transicao. Se os dois lados tem valor, interpola; se so' um tem,
    // usa esse; se nenhum, continua nao autorado.
    if (a.windiness >= 0.0f && b.windiness >= 0.0f)      r.windiness = LERP(windiness);
    else if (a.windiness >= 0.0f)                        r.windiness = a.windiness;
    else                                                 r.windiness = b.windiness;
    r.lensDropCount = static_cast<int>(static_cast<float>(a.lensDropCount) +
                                       (static_cast<float>(b.lensDropCount) - static_cast<float>(a.lensDropCount)) * t);
    r.lensDropRadius = LERP(lensDropRadius);
    // Keep mode from target during transition to avoid pops
    r.lensDropMode = (t >= 0.5f) ? b.lensDropMode : a.lensDropMode;
    r.windDebrisIntensity = LERP(windDebrisIntensity);
    r.atmosphericTint = LERP(atmosphericTint);
    r.rainbowIntensity = LERP(rainbowIntensity);
    r.mirageIntensity = LERP(mirageIntensity);
    r.shootingStarIntensity = LERP(shootingStarIntensity);
    r.tornadoIntensity = LERP(tornadoIntensity);
    r.dustDevilIntensity = LERP(dustDevilIntensity);
    r.auroraIntensity = LERP(auroraIntensity);
    return r;
}

// -----------------------------------------------------------------------------
// Category / type names
// -----------------------------------------------------------------------------

const char* weatherCategoryName(WeatherCategory category) {
    switch (category) {
        case WeatherCategory::Clear:    return "Clear";
        case WeatherCategory::Cloudy:   return "Cloudy";
        case WeatherCategory::Rain:     return "Rain";
        case WeatherCategory::Snow:     return "Snow";
        case WeatherCategory::Heat:     return "Heat";
        case WeatherCategory::Fog:      return "Fog";
        case WeatherCategory::NightSky: return "Night Sky";
        case WeatherCategory::Extreme:  return "Extreme";
        default:                        return "Unknown";
    }
}

const char* weatherTypeName(WeatherType type) {
    switch (type) {
        case WeatherType::Clear:           return "clear";
        case WeatherType::PartlyCloudy:    return "partly_cloudy";
        case WeatherType::LightWind:       return "light_wind";
        case WeatherType::Pollen:          return "pollen";
        case WeatherType::SunHalo:         return "sun_halo";
        case WeatherType::GodRays:         return "god_rays";
        case WeatherType::LightPillars:    return "light_pillars";
        case WeatherType::SunDogs:         return "sun_dogs";
        case WeatherType::GreenFlash:      return "green_flash";
        case WeatherType::RainbowCloud:    return "rainbow_cloud";
        case WeatherType::Cloudy:          return "cloudy";
        case WeatherType::Overcast:        return "overcast";
        case WeatherType::Smog:            return "smog";
        case WeatherType::Haze:            return "haze";
        case WeatherType::Drizzle:         return "drizzle";
        case WeatherType::Rainy:           return "rainy";
        case WeatherType::Stormy:          return "stormy";
        case WeatherType::Thunderstorm:    return "thunderstorm";
        case WeatherType::Hail:            return "hail";
        case WeatherType::Rainbow:         return "rainbow";
        case WeatherType::FreezingRain:    return "freezing_rain";
        case WeatherType::LightSnow:       return "light_snow";
        case WeatherType::Snowy:           return "snowy";
        case WeatherType::Blizzard:        return "blizzard";
        case WeatherType::Frost:           return "frost";
        case WeatherType::Heatwave:        return "heatwave";
        case WeatherType::Sandstorm:       return "sandstorm";
        case WeatherType::DustStorm:       return "dust_storm";
        case WeatherType::DustDevil:       return "dust_devil";
        case WeatherType::Mirage:          return "mirage";
        case WeatherType::HeatHaze:        return "heat_haze";
        case WeatherType::Foggy:           return "foggy";
        case WeatherType::Mist:            return "mist";
        case WeatherType::GroundFog:       return "ground_fog";
        case WeatherType::AuroraBorealis:  return "aurora_borealis";
        case WeatherType::AuroraAustralis: return "aurora_australis";
        case WeatherType::ShootingStars:   return "shooting_stars";
        case WeatherType::MeteorShower:    return "meteor_shower";
        case WeatherType::MilkyWay:        return "milky_way";
        case WeatherType::StarryNight:     return "starry_night";
        case WeatherType::VolcanicAsh:     return "volcanic_ash";
        case WeatherType::FireSmoke:       return "fire_smoke";
        case WeatherType::Tornado:         return "tornado";
        case WeatherType::Waterspout:      return "waterspout";
        case WeatherType::Downburst:       return "downburst";
        default:                           return "clear";
    }
}

WeatherType weatherTypeFromName(const std::string& name) {
    for (uint32_t i = 0; i < WeatherTypeCount; ++i) {
        if (name == weatherTypeName(static_cast<WeatherType>(i))) {
            return static_cast<WeatherType>(i);
        }
    }
    // Legacy fallback for the old config field names
    if (name == "partly_cloudy") return WeatherType::PartlyCloudy;
    if (name == "cloudy") return WeatherType::Cloudy;
    if (name == "rainy") return WeatherType::Rainy;
    if (name == "stormy") return WeatherType::Stormy;
    if (name == "heatwave") return WeatherType::Heatwave;
    if (name == "foggy") return WeatherType::Foggy;
    if (name == "snowy") return WeatherType::Snowy;
    return WeatherType::Clear;
}

WeatherCategory weatherTypeCategory(WeatherType type) {
    switch (type) {
        case WeatherType::Clear:
        case WeatherType::PartlyCloudy:
        case WeatherType::LightWind:
        case WeatherType::Pollen:
        case WeatherType::SunHalo:
        case WeatherType::GodRays:
        case WeatherType::LightPillars:
        case WeatherType::SunDogs:
        case WeatherType::GreenFlash:
        case WeatherType::RainbowCloud:
            return WeatherCategory::Clear;
        case WeatherType::Cloudy:
        case WeatherType::Overcast:
        case WeatherType::Smog:
        case WeatherType::Haze:
            return WeatherCategory::Cloudy;
        case WeatherType::Drizzle:
        case WeatherType::Rainy:
        case WeatherType::Stormy:
        case WeatherType::Thunderstorm:
        case WeatherType::Hail:
        case WeatherType::Rainbow:
        case WeatherType::FreezingRain:
            return WeatherCategory::Rain;
        case WeatherType::LightSnow:
        case WeatherType::Snowy:
        case WeatherType::Blizzard:
        case WeatherType::Frost:
            return WeatherCategory::Snow;
        case WeatherType::Heatwave:
        case WeatherType::Sandstorm:
        case WeatherType::DustStorm:
        case WeatherType::DustDevil:
        case WeatherType::Mirage:
        case WeatherType::HeatHaze:
            return WeatherCategory::Heat;
        case WeatherType::Foggy:
        case WeatherType::Mist:
        case WeatherType::GroundFog:
            return WeatherCategory::Fog;
        case WeatherType::AuroraBorealis:
        case WeatherType::AuroraAustralis:
        case WeatherType::ShootingStars:
        case WeatherType::MeteorShower:
        case WeatherType::MilkyWay:
        case WeatherType::StarryNight:
            return WeatherCategory::NightSky;
        case WeatherType::VolcanicAsh:
        case WeatherType::FireSmoke:
        case WeatherType::Tornado:
        case WeatherType::Waterspout:
        case WeatherType::Downburst:
            return WeatherCategory::Extreme;
        default:
            return WeatherCategory::Clear;
    }
}

bool isWeatherTypeDayOnly(WeatherType type) {
    switch (type) {
        case WeatherType::SunHalo:
        case WeatherType::GodRays:
        case WeatherType::LightPillars:
        case WeatherType::SunDogs:
        case WeatherType::GreenFlash:
        case WeatherType::RainbowCloud:
        case WeatherType::Heatwave:
        case WeatherType::Sandstorm:
        case WeatherType::DustStorm:
        case WeatherType::DustDevil:
        case WeatherType::Mirage:
        case WeatherType::HeatHaze:
        case WeatherType::Rainbow:
            return true;
        default:
            return false;
    }
}

bool isWeatherTypeNightOnly(WeatherType type) {
    switch (type) {
        case WeatherType::AuroraBorealis:
        case WeatherType::AuroraAustralis:
        case WeatherType::ShootingStars:
        case WeatherType::MeteorShower:
        case WeatherType::MilkyWay:
        case WeatherType::StarryNight:
        case WeatherType::LightPillars: // light pillars need artificial lights at night usually, but keep flexible
            return true;
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
// Weather type templates
// -----------------------------------------------------------------------------

static WeatherParams makeBaseClear() {
    WeatherParams p;
    p.weatherType = WeatherType::Clear;
    p.weatherIntensity = 1.0f;
    p.rainIntensity = 0.0f;
    p.dropLensAmount = 0.0f;
    p.snowIntensity = 0.0f;
    p.heatShimmer = 0.0f;
    p.fogDensity = 0.0f;
    p.stormTint = 0.0f;
    p.cloudCoverage = 0.0f;
    p.cloudThickness = 0.2f;
    p.cloudSpeed = 0.02f;
    p.cloudScale = 1.5f;
    p.cloudLightness = 1.15f;
    p.cloudShade = 0.35f;
    p.cloudSoftness = 0.55f;
    p.lightningChance = 0.0f;
    p.windDebrisIntensity = 0.0f;
    p.atmosphericTint = 0.0f;
    return p;
}

// Choose sensible cloud-layer look defaults for a weather category.
static void applyCategoryCloudLook(WeatherParams& p, WeatherCategory category) {
    switch (category) {
        case WeatherCategory::Clear:
            p.cloudScale = 1.5f; p.cloudLightness = 1.15f; p.cloudShade = 0.35f; p.cloudSoftness = 0.55f;
            break;
        case WeatherCategory::Cloudy:
            p.cloudScale = 1.2f; p.cloudLightness = 1.0f; p.cloudShade = 0.55f; p.cloudSoftness = 0.65f;
            break;
        case WeatherCategory::Rain:
            p.cloudScale = 1.0f; p.cloudLightness = 0.75f; p.cloudShade = 0.75f; p.cloudSoftness = 0.45f;
            break;
        case WeatherCategory::Snow:
            p.cloudScale = 1.1f; p.cloudLightness = 1.05f; p.cloudShade = 0.50f; p.cloudSoftness = 0.70f;
            break;
        case WeatherCategory::Heat:
            p.cloudScale = 2.0f; p.cloudLightness = 1.25f; p.cloudShade = 0.25f; p.cloudSoftness = 0.80f;
            break;
        case WeatherCategory::Fog:
            p.cloudScale = 1.3f; p.cloudLightness = 0.90f; p.cloudShade = 0.60f; p.cloudSoftness = 0.85f;
            break;
        case WeatherCategory::NightSky:
            p.cloudScale = 1.4f; p.cloudLightness = 0.85f; p.cloudShade = 0.65f; p.cloudSoftness = 0.60f;
            break;
        case WeatherCategory::Extreme:
            p.cloudScale = 0.9f; p.cloudLightness = 0.60f; p.cloudShade = 0.85f; p.cloudSoftness = 0.35f;
            break;
        default:
            break;
    }
}

// Map a weather category to an approximate ambient temperature in Celsius.
// Intensity pushes the temperature toward the category extreme.
static float categoryTemperature(WeatherCategory cat, float intensity) {
    float base = 20.0f;
    switch (cat) {
        case WeatherCategory::Clear:     base = 25.0f; break;
        case WeatherCategory::Cloudy:    base = 18.0f; break;
        case WeatherCategory::Rain:      base = 12.0f; break;
        case WeatherCategory::Snow:      base = -8.0f; break;
        case WeatherCategory::Heat:      base = 38.0f; break;
        case WeatherCategory::Fog:       base = 10.0f; break;
        case WeatherCategory::NightSky:  base = 5.0f;  break;
        case WeatherCategory::Extreme:   base = 0.0f;  break;
        default:                         base = 20.0f; break;
    }
    return base + (intensity - 0.5f) * 10.0f;
}

WeatherParams scaleWeatherParams(const WeatherParams& base, float intensity) {
    WeatherParams p = base;
    p.weatherIntensity = intensity;
    // Scale the dominant effects by intensity while preserving ratios
    p.rainIntensity *= intensity;
    p.snowIntensity *= intensity;
    p.heatShimmer *= intensity;
    p.fogDensity *= intensity;
    p.stormTint *= intensity;
    p.lightningChance *= intensity;
    p.windDebrisIntensity *= intensity;
    p.atmosphericTint *= intensity;
    p.rainbowIntensity *= intensity;
    p.mirageIntensity *= intensity;
    p.shootingStarIntensity *= intensity;
    p.tornadoIntensity *= intensity;
    p.dustDevilIntensity *= intensity;
    p.auroraIntensity *= intensity;
    p.dropLensAmount *= intensity;
    // Temperature follows the weather category so the PBR semantic system can
    // react causally: rain wets, freezing freezes, heat dries/evaporates.
    p.temperatureC = categoryTemperature(weatherTypeCategory(p.weatherType), intensity);
    applyCategoryCloudLook(p, weatherTypeCategory(p.weatherType));
    return p;
}

// Helper: per-type rain drop shape. Every precip type sets its own streak
// preset here so the shape follows the weather (rainy = fine sharp threads,
// drizzle = hair-fine threads, storm = fat ropes, hail = short hard pellets).
// UI sliders remain a live override on top of the active type's preset.
static void setStreakShape(WeatherParams& p, float w, float l, float e, float tip,
                           float alpha, float bright, float pixelFade) {
    p.rainStreakWidth = w; p.rainStreakLength = l; p.rainStreakEdge = e;
    p.rainStreakTipFade = tip; p.rainStreakAlpha = alpha;
    p.rainStreakBrightness = bright; p.rainPixelFade = pixelFade;
}

// -----------------------------------------------------------------------------
// Assinatura fisica (categorias aristotelicas)
// -----------------------------------------------------------------------------
// `WeatherSignature` existia em WeatherTypes.hpp desde sempre, com um
// comentario prometendo que o WeatherSystem "deriva os parametros de rendering
// dessas propriedades causais em vez de hardcodar cada slider para cada tipo".
// Isso NUNCA foi implementado: o construtor de WeatherTypeInfo nem recebia o
// campo e nada no projeto atribuia um. A struct ficou zerada em todas as 44
// entradas da tabela, e portanto inutil.
//
// As tres funcoes abaixo fecham o ciclo:
//
//   deriveSignature  params -> assinatura   (le a fisica dos templates que ja'
//                                            existem; 1 funcao em vez de 44
//                                            assinaturas escritas a mao, que
//                                            sairiam do sincronismo no primeiro
//                                            ajuste de slider)
//   applySignature   assinatura -> params   (a direcao ARISTOTELICA: a causa
//                                            produz o efeito. E' o que permite
//                                            uma REGIAO do mapa ter clima
//                                            proprio: o vulcao e' quente
//                                            PORQUE e' vulcao, e dai saem calor,
//                                            ar seco e evaporacao rapida)
//   lerpSignature    mistura duas           (o clima regional interpola CAUSAS,
//                                            nao sliders: meio caminho entre
//                                            "vulcanico" e "temperado" e' um
//                                            estado fisico com sentido, ao passo
//                                            que a media de dois conjuntos de
//                                            sliders e' so' media de sintomas)

WeatherSignature deriveSignature(const WeatherParams& p) {
    WeatherSignature s;

    s.cloudAmount = glm::clamp(p.cloudCoverage, 0.0f, 1.0f);

    // Insolacao: o que sobra do sol depois da nuvem e da neblina. A nuvem pesa
    // mais quanto mais espessa; neblina fina espalha a luz sem apagar o sol.
    const float cloudBlock = s.cloudAmount * glm::mix(0.55f, 1.0f, glm::clamp(p.cloudThickness, 0.0f, 1.0f));
    s.solar = glm::clamp((1.0f - cloudBlock) * (1.0f - p.fogDensity * 0.6f), 0.0f, 1.0f);

    // Precipitacao e o tipo dela. Chuva e neve nunca coexistem nos templates,
    // entao a mais forte manda; a temperatura decide se e' liquida ou solida.
    const float rain = glm::clamp(p.rainIntensity, 0.0f, 1.0f);
    const float snow = glm::clamp(p.snowIntensity, 0.0f, 1.0f);
    s.precipitation = glm::max(rain, snow);
    if (s.precipitation <= 0.001f) {
        s.precipType = PrecipitationType::None;
    } else if (snow > rain) {
        s.precipType = PrecipitationType::Snow;
    } else if (p.temperatureC <= 0.0f) {
        s.precipType = PrecipitationType::FreezingRain;
    } else {
        s.precipType = PrecipitationType::Rain;
    }

    // Umidade: nuvem, precipitacao e neblina sao todas manifestacoes de agua no
    // ar. Calor seco (heatShimmer sem precipitacao) empurra para baixo - e'
    // justamente o caso do deserto e do vulcao.
    s.humidity = glm::clamp(s.cloudAmount * 0.35f
                          + s.precipitation * 0.5f
                          + p.fogDensity * 0.45f
                          - p.heatShimmer * 0.3f,
                          0.0f, 1.0f);

    // Instabilidade atmosferica. A agitacao da chuva/neve so' conta na medida em
    // que ha' chuva/neve caindo: `rainTurbulence` e `snowTurbulence` tem default
    // NAO-ZERO (0.2 e 0.5), entao soma-los cru dava turbulencia num dia limpo.
    const float precipTurb = (p.rainTurbulence * rain + p.snowTurbulence * snow) * 0.35f;
    s.turbulence = glm::clamp(p.stormTint * 0.5f
                            + precipTurb
                            + p.lightningChance * 0.6f
                            + p.tornadoIntensity * 0.9f
                            + p.dustDevilIntensity * 0.4f,
                            0.0f, 1.0f);

    // Energia termica: desvio do temperado (20 C), normalizado para -1..1 numa
    // faixa de +/-30 C. Negativo = frio, positivo = calor.
    s.thermalEnergy = celsiusToSignatureTemp(p.temperatureC);

    // Visibilidade: neblina domina, precipitacao e detritos completam.
    s.visibility = glm::clamp(1.0f
                            - p.fogDensity * 0.85f
                            - s.precipitation * 0.25f
                            - p.windDebrisIntensity * 0.3f
                            - p.atmosphericTint * 0.2f,
                            0.0f, 1.0f);

    // Vento. Mesma derivacao que o WindField consumia solta em updateCoherence;
    // agora mora aqui, que e' o lugar dela.
    //
    // `rainWind` e `snowWind` descrevem a INCLINACAO da precipitacao, e tem
    // default nao-zero (0.2 e 0.3). Soma-los sem porta dava vento 0,45 num dia
    // Clear sem uma gota caindo — o clima limpo herdava o vento de uma chuva
    // que nao existe. Agora eles so' contam proporcionalmente ao que de fato
    // esta' caindo.
    const float precipSlant = (glm::abs(p.rainWind) * rain + glm::abs(p.snowWind) * snow) * 0.6f;

    // Brisa de fundo: ar completamente parado nao existe ao ar livre, e sem
    // isso vegetacao e nuvem congelariam num dia limpo.
    const float baseBreeze = 0.12f;

    if (p.windiness >= 0.0f) {
        // Vento AUTORADO: o clima declara quanto venta, e nao se adivinha mais
        // dos sintomas. E' a fonte preferencial.
        s.wind = glm::clamp(p.windiness, 0.0f, 1.0f);
    } else {
        // Sem valor autorado (config antiga), infere dos sintomas como antes.
        s.wind = glm::clamp(baseBreeze
                          + precipSlant
                          + p.stormTint * 0.5f
                          + precipTurb
                          + p.windDebrisIntensity * 0.4f
                          + p.tornadoIntensity * 0.9f
                          + p.dustDevilIntensity * 0.3f,
                          0.0f, 1.0f);
    }
    return s;
}

void applySignature(const WeatherSignature& sig, WeatherParams& p) {
    // Causa -> efeito. Cada linha aqui e' uma consequencia fisica da assinatura,
    // nao um slider solto. Uma regiao descrita so' por uma assinatura ganha um
    // conjunto de parametros coerente entre si de graca.
    p.cloudCoverage = glm::clamp(sig.cloudAmount, 0.0f, 1.0f);
    p.cloudThickness = glm::clamp(0.25f + sig.cloudAmount * 0.5f + sig.turbulence * 0.25f, 0.0f, 1.0f);

    // Temperatura sai direto da energia termica: e' a MESMA grandeza.
    p.temperatureC = signatureTempToCelsius(sig.thermalEnergy);

    // Precipitacao vai para o canal certo conforme o tipo. Nunca os dois: agua
    // caindo e' liquida ou solida, nao as duas.
    p.rainIntensity = 0.0f;
    p.snowIntensity = 0.0f;
    switch (sig.precipType) {
        case PrecipitationType::Snow:
            p.snowIntensity = sig.precipitation;
            break;
        case PrecipitationType::Rain:
        case PrecipitationType::FreezingRain:
        case PrecipitationType::Hail:
            p.rainIntensity = sig.precipitation;
            break;
        case PrecipitationType::None:
            break;
    }

    // Calor visivel: so' aparece com energia termica positiva, e ar umido o
    // mata (a ondulacao de calor precisa de ar seco sobre solo quente).
    const float heat = glm::max(sig.thermalEnergy, 0.0f);
    p.heatShimmer = glm::clamp(heat * (1.0f - sig.humidity), 0.0f, 1.0f);

    // Visibilidade perdida vira neblina.
    p.fogDensity = glm::clamp((1.0f - sig.visibility) * 0.85f, 0.0f, 1.0f);

    // Instabilidade vira tempestade e raio.
    p.stormTint = glm::clamp(sig.turbulence * 0.9f, 0.0f, 1.0f);
    p.lightningChance = glm::clamp((sig.turbulence - 0.55f) / 0.45f, 0.0f, 1.0f) * 0.3f;

    // Vento move nuvem, inclina precipitacao e levanta detrito.
    p.cloudSpeed = 0.02f + sig.wind * 0.30f;
    p.rainWind = sig.wind;
    p.snowWind = sig.wind;
    p.rainTurbulence = glm::clamp(sig.turbulence, 0.0f, 1.0f);
    p.snowTurbulence = glm::clamp(sig.turbulence, 0.0f, 1.0f);
    p.windDebrisIntensity = glm::clamp(sig.wind * (1.0f - sig.humidity), 0.0f, 1.0f);
}

WeatherSignature lerpSignature(const WeatherSignature& a, const WeatherSignature& b, float t) {
    t = glm::clamp(t, 0.0f, 1.0f);
    WeatherSignature s;
    s.solar         = glm::mix(a.solar, b.solar, t);
    s.cloudAmount   = glm::mix(a.cloudAmount, b.cloudAmount, t);
    s.humidity      = glm::mix(a.humidity, b.humidity, t);
    s.turbulence    = glm::mix(a.turbulence, b.turbulence, t);
    s.thermalEnergy = glm::mix(a.thermalEnergy, b.thermalEnergy, t);
    s.precipitation = glm::mix(a.precipitation, b.precipitation, t);
    s.visibility    = glm::mix(a.visibility, b.visibility, t);
    s.wind          = glm::mix(a.wind, b.wind, t);
    // Tipo de precipitacao e' categorico, nao interpola: fica o do lado
    // dominante. Meio caminho entre chuva e neve nao e' meia-chuva, e' a que
    // estiver mais forte ali.
    const bool takeB = (t > 0.5f && b.precipType != PrecipitationType::None) ||
                       a.precipType == PrecipitationType::None;
    s.precipType = takeB ? b.precipType : a.precipType;
    return s;
}

const WeatherTypeInfo& getWeatherTypeInfo(WeatherType type) {
    static const WeatherTypeInfo table[WeatherTypeCount] = {
        // Clear
        {"Clear",           WeatherCategory::Clear,    false, false, makeBaseClear()},
        {"Partly Cloudy",   WeatherCategory::Clear,    false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::PartlyCloudy;
            p.cloudCoverage = 0.40f; p.cloudThickness = 0.35f; p.cloudSpeed = 0.04f; return p;}()},
        {"Light Wind",      WeatherCategory::Clear,    false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::LightWind;
            p.cloudCoverage = 0.25f; p.windDebrisIntensity = 0.25f; return p;}()},
        {"Pollen",          WeatherCategory::Clear,    false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Pollen;
            p.cloudCoverage = 0.10f; p.windDebrisIntensity = 0.45f; p.fogDensity = 0.05f; return p;}()},
        {"Sun Halo",        WeatherCategory::Clear,    true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::SunHalo;
            p.cloudCoverage = 0.25f; p.cloudThickness = 0.3f; return p;}()},
        {"God Rays",        WeatherCategory::Clear,    true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::GodRays;
            p.cloudCoverage = 0.30f; p.cloudThickness = 0.35f; return p;}()},
        {"Light Pillars",   WeatherCategory::Clear,    false, true,  [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::LightPillars;
            p.cloudCoverage = 0.10f; p.fogDensity = 0.05f; return p;}()},
        {"Sun Dogs",        WeatherCategory::Clear,    true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::SunDogs;
            p.cloudCoverage = 0.20f; return p;}()},
        {"Green Flash",     WeatherCategory::Clear,    true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::GreenFlash;
            p.cloudCoverage = 0.05f; return p;}()},
        {"Rainbow Cloud",   WeatherCategory::Clear,    true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::RainbowCloud;
            p.cloudCoverage = 0.45f; p.cloudThickness = 0.4f; return p;}()},

        // Cloudy
        {"Cloudy",          WeatherCategory::Cloudy,   false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Cloudy;
            p.cloudCoverage = 0.70f; p.cloudThickness = 0.60f; p.cloudSpeed = 0.05f;
            p.fogDensity = 0.05f; return p;}()},
        {"Overcast",        WeatherCategory::Cloudy,   false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Overcast;
            p.cloudCoverage = 0.92f; p.cloudThickness = 0.75f; p.cloudSpeed = 0.06f;
            p.fogDensity = 0.10f; return p;}()},
        {"Smog",            WeatherCategory::Cloudy,   false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Smog;
            p.cloudCoverage = 0.55f; p.cloudThickness = 0.45f;
            p.fogDensity = 0.25f; p.fogHeightFalloff = 0.008f;
            p.atmosphericTint = 0.40f; return p;}()},
        {"Haze",            WeatherCategory::Cloudy,   false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Haze;
            p.cloudCoverage = 0.35f; p.fogDensity = 0.15f;
            p.fogHeightFalloff = 0.006f; return p;}()},

        // Rain
        {"Drizzle",         WeatherCategory::Rain,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Drizzle;
            p.rainIntensity = 0.25f; p.rainSpeed = 0.9f; p.rainWind = 0.1f;
            p.dropLensAmount = 0.10f; p.lensDropMode = LensDropMode::Procedural;
            p.fogDensity = 0.10f; p.cloudCoverage = 1.0f; p.cloudThickness = 0.55f;
            p.stormTint = 0.15f;
            p.rainSplashEnabled = true;
            p.rainSplashAmount = 0.10f; p.rainSplashOpacity = 0.25f;
            // Splash/crown 25% em todos os climas (author feedback 2026-08-10).
            p.rainCrownGain = 0.25f;
            // Garoa: fios finos, curtos e suaves. Pixel fade baixo (1px):
            // com 1.5px os fios de 0.01m ficam sub-pixel já a ~10m e o fade
            // zerava o alpha — garoa invisível. Width 0.20 e alpha/brightness
            // 4.0/3.0 (era 0.14/2.8/2.5): a cena do drizzle é clara (pouco
            // stormTint/fog) e o fio de 1cm ficava sub-pixel (<1px) e lavado
            // contra o fundo claro — garoa invisível mesmo caindo das nuvens
            // (author feedback 2026-08-09). Continua fina, mas lê na tela.
            setStreakShape(p, 0.20f, 5.0f, 4.0f, 1.5f, 4.0f, 3.0f, 1.0f); return p;}()},
        {"Rainy",           WeatherCategory::Rain,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Rainy;
            p.rainIntensity = 0.70f; p.rainSpeed = 1.4f; p.rainWind = 0.4f; p.rainTurbulence = 0.25f;
            p.dropLensAmount = 0.30f; p.lensDropMode = LensDropMode::CpuBuffer; p.lensDropCount = 512;
            p.rainSplashEnabled = true;
            p.rainSplashAmount = 0.25f; p.rainSplashOpacity = 0.25f;
            p.rainCrownGain = 0.25f;
            p.fogDensity = 0.18f; p.fogHeightFalloff = 0.015f;
            p.stormTint = 0.40f; p.cloudCoverage = 1.0f; p.cloudThickness = 0.75f; p.cloudSpeed = 0.12f;
            p.lightningChance = 0.08f;
            // Chuva: fios finos e nítidos (calibrado pelo autor). O baseline
            // antigo (0.5/12/1/0.5) tinha width 0.04m com borda suave — gota
            // gorda que virava "bola" brilhante, principalmente com pitch alto
            // (streak vertical encurta na projeção e sobra o blob da largura).
            setStreakShape(p, 0.15f, 6.0f, 5.0f, 1.3f, 2.0f, 2.5f, 0.0f); return p;}()},
        {"Stormy",          WeatherCategory::Rain,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Stormy;
            p.rainIntensity = 0.95f; p.rainSpeed = 1.9f; p.rainWind = 0.7f; p.rainTurbulence = 0.45f;
            p.dropLensAmount = 0.60f; p.lensDropMode = LensDropMode::CpuBuffer; p.lensDropCount = 1024;
            p.rainSplashEnabled = true;
            p.rainSplashAmount = 0.45f; p.rainSplashOpacity = 0.25f;
            p.rainCrownGain = 0.25f;
            p.fogDensity = 0.35f; p.fogHeightFalloff = 0.02f;
            p.stormTint = 0.90f; p.cloudCoverage = 1.0f; p.cloudThickness = 0.90f; p.cloudSpeed = 0.22f;
            p.lightningChance = 0.35f;
            // Tempestade: um pouco ABAIXO da thunderstorm (a mais pesada de
            // todas, author feedback 2026-08-10): mesma família de cordas,
            // ~5% mais finas/curtas e um tick menos brilhantes.
            setStreakShape(p, 0.24f, 4.8f, 0.8f, 0.4f, 1.5f, 3.1f, 0.0f); return p;}()},
        {"Thunderstorm",    WeatherCategory::Rain,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Thunderstorm;
            p.rainIntensity = 0.85f; p.rainSpeed = 1.7f; p.rainWind = 0.6f; p.rainTurbulence = 0.50f;
            p.dropLensAmount = 0.45f; p.lensDropMode = LensDropMode::CpuBuffer; p.lensDropCount = 768;
            p.rainSplashEnabled = true;
            p.rainSplashAmount = 0.40f; p.rainSplashOpacity = 0.25f;
            // Splash/crown 25% em todos os climas (author feedback 2026-08-10).
            p.rainCrownGain = 0.25f;
            // NEVOA: a densidade e' a maior dos climas de chuva (0,25 contra
            // 0,18 do Rainy) mas o fogHeightFalloff tinha ficado no padrao da
            // struct (0,01) - era o UNICO preset de chuva sem a linha, e por
            // isso a nevoa da tempestade subia mais alto que a do proprio
            // nevoeiro (Foggy usa 0,02). Resultado: parede branca que come a
            // cena inteira ("uma fog fodida, ai nao da pra ver nada", autor
            // 2026-09-06). Com 0,032 a nevoa fica presa ao chao, o volume
            // continua pesado e o detalhe da cena volta (medido: contraste
            // local: 1,76 -> 3,36, quase o dobro).
            p.fogDensity = 0.25f; p.fogHeightFalloff = 0.032f;
            p.stormTint = 0.75f; p.cloudCoverage = 1.0f; p.cloudThickness = 0.85f; p.cloudSpeed = 0.40f;
            p.lightningChance = 0.70f;
            // Temporal com relâmpago: gota mais pesada de todos os climas
            // (valores ditados pelo autor): largura 0.25, comprimento 5,
            // alpha 1.5; brightness 3.2 lê sob o flash.
            setStreakShape(p, 0.25f, 5.0f, 0.8f, 0.4f, 1.5f, 3.2f, 0.0f); return p;}()},
        {"Hail",            WeatherCategory::Rain,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Hail;
            p.rainIntensity = 0.80f; p.rainSpeed = 2.2f; p.rainWind = 0.5f; p.rainTurbulence = 0.35f;
            p.dropLensAmount = 0.20f;
            p.rainSplashEnabled = true;
            p.rainSplashAmount = 0.30f; p.rainSplashOpacity = 0.25f;
            p.rainCrownGain = 0.25f;
            p.fogDensity = 0.15f;
            p.stormTint = 0.50f; p.cloudCoverage = 1.0f; p.cloudThickness = 0.70f;
            p.lightningChance = 0.20f;
            // Granizo: pelota curta e grossa — length 2.0 (era 4.0) pra ficar
            // redondinha em vez de streak longo. Alpha/brightness 1.8/2.5:
            // brilho intermediário entre o original (3.0/3.5, saturava em
            // blob branco) e o corte seco (1.2/1.5) — o shader ainda aplica
            // um boost fixo de 1.6x no hail (author feedback 2026-08-10).
            setStreakShape(p, 0.60f, 2.0f, 1.5f, 0.5f, 1.5f, 2.0f, 0.0f); return p;}()},
        {"Rainbow",         WeatherCategory::Rain,     true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Rainbow;
            p.rainIntensity = 0.0f; p.fogDensity = 0.05f;
            p.cloudCoverage = 0.40f; p.cloudThickness = 0.35f;
            p.rainbowIntensity = 1.0f; return p;}()},
        {"Freezing Rain",   WeatherCategory::Rain,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::FreezingRain;
            p.rainIntensity = 0.55f; p.rainSpeed = 1.1f; p.rainWind = 0.3f;
            p.dropLensAmount = 0.25f;
            p.rainSplashEnabled = true;
            p.rainSplashAmount = 0.20f; p.rainSplashOpacity = 0.25f;
            p.rainCrownGain = 0.25f;
            p.fogDensity = 0.20f; p.fogHeightFalloff = 0.012f;
            p.snowIntensity = 0.10f;
            p.cloudCoverage = 1.0f; p.cloudThickness = 0.65f;
            p.stormTint = 0.25f;
            // Chuva congelante: agulhas finas de gelo, médias e brilhantes.
            setStreakShape(p, 0.25f, 7.0f, 2.5f, 1.0f, 2.0f, 3.0f, 1.0f); return p;}()},

        // Snow
        {"Light Snow",      WeatherCategory::Snow,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::LightSnow;
            // Garoa de neve: pouca partícula e look suave (author feedback
            // 2026-08-10 — estava "caindo muito forte").
            p.snowIntensity = 0.22f; p.snowSpeed = 0.4f; p.snowSize = 0.8f; p.snowWind = 0.2f;
            p.fogDensity = 0.08f; p.cloudCoverage = 1.0f; p.cloudThickness = 0.45f;
            // Neve renderiza como pelota estilo hail (ver particle_render.frag);
            // look.x/.y viram alpha/brilho da pelota — gradação por clima.
            setStreakShape(p, 0.60f, 2.0f, 1.5f, 0.5f, 0.8f, 0.8f, 0.0f);
            return p;}()},
        {"Snowy",           WeatherCategory::Snow,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Snowy;
            p.snowIntensity = 0.85f; p.snowSpeed = 0.7f; p.snowSize = 1.3f; p.snowWind = 0.5f; p.snowTurbulence = 0.55f;
            p.dropLensAmount = 0.05f;
            p.fogDensity = 0.22f; p.fogHeightFalloff = 0.012f;
            p.cloudCoverage = 1.0f; p.cloudThickness = 0.70f; p.cloudSpeed = 0.06f;
            setStreakShape(p, 0.60f, 2.0f, 1.5f, 0.5f, 1.5f, 1.4f, 0.0f);
            return p;}()},
        {"Blizzard",        WeatherCategory::Snow,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Blizzard;
            p.snowIntensity = 1.00f; p.snowSpeed = 1.6f; p.snowSize = 1.6f; p.snowWind = 1.0f; p.snowTurbulence = 0.85f;
            p.fogDensity = 0.55f; p.fogHeightFalloff = 0.025f;
            p.stormTint = 0.30f; p.cloudCoverage = 1.0f; p.cloudThickness = 0.90f; p.cloudSpeed = 0.30f;
            // Sem debris/dust na tela (author request 2026-08-10): a força do
            // blizzard vem da pelota de neve MAIS intensa que o hail (2.5/2.3
            // vs 1.5/2.0) + vento/turbulência máximos, não do véu de poeira.
            setStreakShape(p, 0.60f, 2.0f, 1.5f, 0.5f, 2.5f, 2.3f, 0.0f);
            return p;}()},
        {"Frost",           WeatherCategory::Snow,     false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Frost;
            p.snowIntensity = 0.10f; p.snowSize = 0.6f;
            p.fogDensity = 0.05f;
            p.cloudCoverage = 0.8f; p.cloudThickness = 0.30f;
            setStreakShape(p, 0.60f, 2.0f, 1.5f, 0.5f, 0.6f, 0.7f, 0.0f);
            return p;}()},

        // Heat
        {"Heatwave",        WeatherCategory::Heat,     true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Heatwave;
            p.heatShimmer = 0.80f; p.heatSpeed = 1.2f; p.heatScale = 1.3f;
            p.cloudCoverage = 0.05f; p.cloudThickness = 0.15f; p.cloudSpeed = 0.02f;
            p.mirageIntensity = 0.15f; return p;}()},
        {"Sandstorm",       WeatherCategory::Heat,     true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Sandstorm;
            p.fogDensity = 0.70f; p.fogStart = 5.0f; p.fogEnd = 120.0f; p.fogHeightFalloff = 0.03f;
            p.stormTint = 0.75f; p.cloudCoverage = 0.20f; p.cloudSpeed = 0.40f;
            p.windDebrisIntensity = 0.90f; p.atmosphericTint = 0.80f;
            p.rainWind = 0.8f; return p;}()},
        {"Dust Storm",      WeatherCategory::Heat,     true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::DustStorm;
            p.fogDensity = 0.55f; p.fogStart = 8.0f; p.fogEnd = 180.0f; p.fogHeightFalloff = 0.025f;
            p.stormTint = 0.55f; p.cloudCoverage = 0.25f; p.cloudSpeed = 0.35f;
            p.windDebrisIntensity = 0.70f; p.atmosphericTint = 0.60f;
            p.rainWind = 0.6f; return p;}()},
        {"Dust Devil",      WeatherCategory::Heat,     true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::DustDevil;
            p.fogDensity = 0.15f; p.fogHeightFalloff = 0.04f;
            p.cloudCoverage = 0.10f;
            p.windDebrisIntensity = 0.50f; p.atmosphericTint = 0.30f;
            p.dustDevilIntensity = 1.0f;
            p.rainWind = 0.5f; return p;}()},
        {"Mirage",          WeatherCategory::Heat,     true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Mirage;
            p.heatShimmer = 0.60f; p.heatSpeed = 0.8f; p.heatScale = 1.6f;
            p.cloudCoverage = 0.02f;
            p.mirageIntensity = 1.0f; return p;}()},
        {"Heat Haze",       WeatherCategory::Heat,     true,  false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::HeatHaze;
            p.heatShimmer = 0.35f; p.heatSpeed = 1.0f; p.heatScale = 1.2f;
            p.cloudCoverage = 0.10f;
            p.mirageIntensity = 0.25f; return p;}()},

        // Fog
        {"Foggy",           WeatherCategory::Fog,      false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Foggy;
            p.fogDensity = 0.65f; p.fogStart = 10.0f; p.fogEnd = 250.0f; p.fogHeightFalloff = 0.025f;
            p.cloudCoverage = 0.55f; p.cloudThickness = 0.50f; return p;}()},
        {"Mist",            WeatherCategory::Fog,      false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Mist;
            p.fogDensity = 0.25f; p.fogStart = 20.0f; p.fogEnd = 400.0f; p.fogHeightFalloff = 0.015f;
            p.cloudCoverage = 0.30f; p.cloudThickness = 0.35f; return p;}()},
        {"Ground Fog",      WeatherCategory::Fog,      false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::GroundFog;
            p.fogDensity = 0.45f; p.fogStart = 5.0f; p.fogEnd = 200.0f; p.fogHeight = -2.0f; p.fogHeightFalloff = 0.04f;
            p.cloudCoverage = 0.25f; p.cloudThickness = 0.30f; return p;}()},

        // Night Sky
        {"Aurora Borealis", WeatherCategory::NightSky, false, true,  [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::AuroraBorealis;
            p.cloudCoverage = 0.20f; p.cloudThickness = 0.20f;
            p.auroraIntensity = 1.0f; return p;}()},
        {"Aurora Australis",WeatherCategory::NightSky, false, true,  [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::AuroraAustralis;
            p.cloudCoverage = 0.20f; p.cloudThickness = 0.20f;
            p.auroraIntensity = 1.0f; return p;}()},
        {"Shooting Stars",  WeatherCategory::NightSky, false, true,  [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::ShootingStars;
            p.cloudCoverage = 0.10f;
            p.shootingStarIntensity = 0.6f; return p;}()},
        {"Meteor Shower",   WeatherCategory::NightSky, false, true,  [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::MeteorShower;
            p.cloudCoverage = 0.05f;
            p.shootingStarIntensity = 1.0f; return p;}()},
        {"Milky Way",       WeatherCategory::NightSky, false, true,  [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::MilkyWay;
            p.cloudCoverage = 0.05f; return p;}()},
        {"Starry Night",    WeatherCategory::NightSky, false, true,  [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::StarryNight;
            p.cloudCoverage = 0.05f; return p;}()},

        // Extreme
        {"Volcanic Ash",    WeatherCategory::Extreme,  false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::VolcanicAsh;
            p.fogDensity = 0.60f; p.fogStart = 8.0f; p.fogEnd = 220.0f; p.fogHeightFalloff = 0.02f;
            p.stormTint = 0.35f; p.cloudCoverage = 0.70f; p.cloudThickness = 0.60f; p.cloudSpeed = 0.10f;
            p.windDebrisIntensity = 0.40f; p.atmosphericTint = 0.70f;
            p.rainWind = 0.4f; return p;}()},
        {"Fire Smoke",      WeatherCategory::Extreme,  false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::FireSmoke;
            p.fogDensity = 0.40f; p.fogStart = 15.0f; p.fogEnd = 350.0f; p.fogHeightFalloff = 0.015f;
            p.stormTint = 0.20f; p.cloudCoverage = 0.50f; p.cloudThickness = 0.45f;
            p.atmosphericTint = 0.50f; return p;}()},
        {"Tornado",         WeatherCategory::Extreme,  false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Tornado;
            p.fogDensity = 0.30f; p.fogHeightFalloff = 0.03f;
            p.stormTint = 0.55f; p.cloudCoverage = 0.90f; p.cloudThickness = 0.85f; p.cloudSpeed = 0.50f;
            p.windDebrisIntensity = 0.95f; p.lightningChance = 0.25f;
            p.tornadoIntensity = 1.0f;
            p.rainWind = 0.9f; return p;}()},
        {"Waterspout",      WeatherCategory::Extreme,  false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Waterspout;
            p.fogDensity = 0.25f; p.fogHeightFalloff = 0.025f;
            p.stormTint = 0.40f; p.cloudCoverage = 0.70f; p.cloudThickness = 0.65f; p.cloudSpeed = 0.40f;
            p.windDebrisIntensity = 0.60f;
            p.tornadoIntensity = 0.6f; return p;}()},
        {"Downburst",       WeatherCategory::Extreme,  false, false, [](){
            WeatherParams p = makeBaseClear(); p.weatherType = WeatherType::Downburst;
            p.rainIntensity = 0.60f; p.rainSpeed = 2.0f; p.rainWind = 0.9f; p.rainTurbulence = 0.55f;
            p.dropLensAmount = 0.30f; p.rainSplashEnabled = true;
            p.rainSplashAmount = 0.50f; p.rainSplashOpacity = 0.25f;
            p.rainCrownGain = 0.25f;
            p.fogDensity = 0.30f;
            p.stormTint = 0.45f; p.cloudCoverage = 0.85f; p.cloudThickness = 0.80f; p.cloudSpeed = 0.35f;
            p.windDebrisIntensity = 0.80f; p.lightningChance = 0.30f;
            p.tornadoIntensity = 0.7f; return p;}()},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == WeatherTypeCount, "WeatherTypeInfo table mismatch");
    return table[static_cast<uint32_t>(type)];
}

// -----------------------------------------------------------------------------
// WeatherSystem
// -----------------------------------------------------------------------------

float WeatherSystem::easeInOutCubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

void WeatherSystem::update(float dt) {
    // The transition runs on m_global, which is the PURE global weather. The
    // regional field modulates a copy of it further down. Modulating the
    // accumulator itself would feed each frame's climate back into the next
    // frame's starting point and compound it over the transition.
    if (m_transitionTime < m_transitionDuration) {
        m_transitionTime += dt;
        float t = glm::clamp(m_transitionTime / m_transitionDuration, 0.0f, 1.0f);
        m_global = WeatherParams::lerp(m_global, m_target, easeInOutCubic(t));
    } else {
        m_global = m_target;
    }
    m_current = m_global;

    // Regional climate (G9). With no regions authored this is a no-op and the
    // world keeps a single global weather state, exactly as before.
    if (!m_climate.empty()) {
        m_climate.setBaseSignature(deriveSignature(m_global));
        const WeatherSignature local = m_climate.sampleAt(m_climateSamplePos);
        // applySignature only writes the PHYSICAL parameters (cloud,
        // temperature, precipitation, heat, fog, storm, wind). The artistic
        // tuning of the active template -- streak shape, splash radius, lens
        // drops -- is left untouched, so a region changes the weather without
        // discarding how the author made it look.
        applySignature(local, m_current);
    }

    // Auto ground-height for heat shimmer: snap all states so the value tracks
    // the terrain without producing a transition every time the player moves.
    if (m_current.heatWorldHeightAuto) {
        float desired = m_autoHeatGroundHeight + m_current.heatWorldHeightOffset;
        m_current.heatWorldHeight = desired;
        m_target.heatWorldHeight = desired;
        m_editor.heatWorldHeight = desired;
    }

    updateCoherence();

    // Wind (G6). Driven after updateCoherence so it sees this frame's target.
    // The prevailing heading comes from the authored cloud-layer config, so the
    // clouds, the rain slant and the wind all finally point the same way.
    m_wind.update(dt, m_prevailingWind, m_windTarget);
    m_windDirection = m_wind.direction();
    m_windStrength = m_wind.strength();

    // Hot-reload dos envelopes climaticos. `stat` uma vez por segundo, nao por
    // frame: o autor edita o JSON e ve o efeito sem reiniciar, e a validacao de
    // nucleo sobreposto roda de novo a cada recarga.
    m_climateReloadTimer += dt;
    if (m_climateReloadTimer >= 1.0f) {
        m_climateReloadTimer = 0.0f;
        reloadOverridesIfModified();
        if (m_climateTypes.reloadIfModified()) {
            // Reaplica o tipo corrente para o estado atmosferico acompanhar os
            // valores novos na hora.
            applyType(m_currentType, m_current.weatherIntensity);
        }
    }

    updateSurfaceWetness(dt);
    updateLightning(dt);
    updateLensDrops(dt);
}

void WeatherSystem::snapToTarget() {
    m_transitionTime = m_transitionDuration;
    // m_global too, not just m_current: update() rebuilds m_current from
    // m_global every frame, so setting only m_current here would be discarded
    // on the very next tick and the snap would silently not happen.
    m_global = m_target;
    m_current = m_target;
    updateCoherence();
}

void WeatherSystem::updateCoherence() {
    const WeatherParams& p = m_current;

    // --- Oclusão solar/lunar pelas nuvens ---
    float coverageFactor = glm::pow(p.cloudCoverage, 1.2f) * p.cloudThickness * m_cloudOcclusionCurve;
    m_sunOcclusion = 1.0f - glm::clamp(coverageFactor, 0.0f, 1.0f);
    m_moonOcclusion = 1.0f - glm::clamp(coverageFactor * 1.3f, 0.0f, 1.0f);

    // Tempestade esconde quase tudo
    float stormFactor = p.stormTint * 1.2f;
    m_sunOcclusion *= 1.0f - glm::clamp(stormFactor, 0.0f, 0.95f);
    m_moonOcclusion *= 1.0f - glm::clamp(stormFactor, 0.0f, 0.98f);

    // --- Precipitação exige nuvens ---
    bool canRain = p.cloudCoverage > 0.55f && p.cloudThickness > 0.4f;
    m_effectiveRain = canRain ? p.rainIntensity : 0.0f;

    bool canSnow = p.cloudCoverage > 0.60f && p.cloudThickness > 0.45f;
    m_effectiveSnow = canSnow ? p.snowIntensity : 0.0f;

    // --- Calor só quando céu limpo e sem precipitação ---
    float heatValidity = (1.0f - p.cloudCoverage) *
                         (1.0f - m_effectiveRain) *
                         (1.0f - m_effectiveSnow) *
                         (1.0f - glm::clamp(p.fogDensity * 5.0f, 0.0f, 1.0f));
    m_effectiveHeat = p.heatShimmer * glm::clamp(heatValidity, 0.0f, 1.0f);

    // --- Neve e chuva não coexistem: neve vence quando ambos são fortes ---
    if (m_effectiveSnow > 0.3f && m_effectiveRain > 0.3f) {
        if (m_effectiveSnow >= m_effectiveRain) {
            m_effectiveRain = 0.0f;
        } else {
            m_effectiveSnow = 0.0f;
        }
    }

    // --- Alvo de forca do vento (G6) ---
    // Antes daqui saia `m_windDirection = Vec3(rainWind, 0, 0)`: sempre no eixo
    // X, sem componente Z, incapaz de descrever uma direcao — e ninguem lia.
    // A direcao agora vem do WindField; a INTENSIDADE vem da assinatura fisica
    // do clima corrente, que e' a propriedade aristotelica certa para isso.
    // (Ate' a assinatura ser populada, isto era uma soma solta de sintomas
    // repetida aqui; agora mora em deriveSignature e ha' uma copia so'.)
    // p is m_current, i.e. already localized by the regional field, so the wind
    // a region asks for is the wind that actually blows there.
    m_signature = deriveSignature(p);
    m_windTarget = m_signature.wind;
}

bool WeatherSystem::loadTypeOverrides(const std::string& path) {
    m_overridePath = path;
    std::ifstream f(path);
    if (!f.is_open()) return false;

    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e) {
        ERUPTION_LOG_WARN("[CLIMA] %s: JSON invalido (%s); templates de C++ mantidos",
                          path.c_str(), e.what());
        return false;
    }
    { struct stat st{}; if (stat(path.c_str(), &st) == 0) m_overrideMtime = st.st_mtime; }

    const auto types = j.find("types");
    if (types == j.end() || !types->is_object()) {
        ERUPTION_LOG_WARN("[CLIMA] %s sem objeto 'types'", path.c_str());
        return false;
    }

    m_hasOverride.assign(WeatherTypeCount, 0);
    m_overrides.assign(WeatherTypeCount, nlohmann::json::object());

    int n = 0;
    for (auto it = types->begin(); it != types->end(); ++it) {
        const WeatherType t = weatherTypeFromName(it.key());
        // Round-trip: weatherTypeFromName devolve Clear para nome desconhecido,
        // entao so' comparar com Count deixaria erro de digitacao passar como
        // se fosse "clear". Ja' aconteceu uma vez com climate_types.json.
        if (std::string(weatherTypeName(t)) != it.key()) {
            ERUPTION_LOG_WARN("[CLIMA] tipo desconhecido '%s' ignorado "
                              "(nomes sao snake_case, ex: partly_cloudy)", it.key().c_str());
            continue;
        }
        if (!it.value().is_object()) continue;
        m_overrides[static_cast<uint32_t>(t)] = it.value();
        m_hasOverride[static_cast<uint32_t>(t)] = 1;
        ++n;
    }
    ERUPTION_LOG_WARN("[CLIMA] %s: %d tipo(s) com sobreposicao de aparencia", path.c_str(), n);

    // Reaplica o tipo corrente para o ajuste valer na hora, sem reiniciar.
    applyType(m_currentType, m_current.weatherIntensity);
    return true;
}

void WeatherSystem::applyTypeOverride(WeatherType type, WeatherParams& p) const {
    const uint32_t i = static_cast<uint32_t>(type);
    if (i >= m_hasOverride.size() || !m_hasOverride[i]) return;
    // loadFromJson so' toca o que o objeto declara: campo ausente mantem o
    // valor que ja' estava, entao a sobreposicao e' aditiva e nao destrutiva.
    p.loadFromJson(m_overrides[i]);
}

bool WeatherSystem::reloadOverridesIfModified() {
    if (m_overridePath.empty()) return false;
    struct stat st{};
    if (stat(m_overridePath.c_str(), &st) != 0) return false;
    if (static_cast<long long>(st.st_mtime) == m_overrideMtime) return false;
    ERUPTION_LOG_WARN("[CLIMA] %s mudou, recarregando", m_overridePath.c_str());
    return loadTypeOverrides(m_overridePath);
}

void WeatherSystem::updateSurfaceWetness(float dt) {
    if (dt <= 0.0f) return;

    // Antes disto o molhado era instantaneo nos dois sentidos: a iluminacao lia
    // `effectiveRainIntensity`, que e' quanta chuva esta' CAINDO. A chuva
    // parava e no mesmo frame o chao estava seco - o que ninguem nunca viu
    // acontecer. Molhar e secar sao processos com constantes de tempo MUITO
    // diferentes, e e' essa assimetria que faz a chuva parecer real.

    const float falling = glm::clamp(m_effectiveRain, 0.0f, 1.0f);

    if (falling > m_surfaceWetness) {
        // MOLHAR: rapido. Uma superficie exposta satura em poucos segundos -
        // o limite e' quanta agua a superficie comporta, nao a taxa de chegada.
        const float kWetPerSecond = 0.5f;
        m_surfaceWetness += (falling - m_surfaceWetness)
                          * glm::min(1.0f, kWetPerSecond * dt * 3.0f);
    } else {
        // SECAR: lento, e por evaporacao - logo governado pelo estado do ar,
        // nao pelo da superficie. Tres fatores, todos com sentido fisico:
        //
        //  - TEMPERATURA: quanto mais quente, mais rapido. Abaixo de zero a
        //    agua nao evapora como liquido (vira gelo), entao o termo zera e o
        //    molhado FICA - que e' o comportamento certo para uma superficie
        //    congelada e o gancho natural para o gelo depois (G5).
        //  - VENTO: ar em movimento leva embora a camada limite saturada
        //    junto da superficie. Sem vento a evaporacao se auto-limita.
        //    WindField::evaporationFactor() ja' existia e ninguem consumia.
        //  - UMIDADE: ar ja' saturado nao aceita mais vapor. Sob ceu fechado e
        //    umido a poca demora; no deserto some. Vem da assinatura fisica.
        const float tempC = m_current.temperatureC;
        const float thermal = glm::clamp(tempC / 30.0f, 0.0f, 1.5f);
        const float wind = m_wind.evaporationFactor();
        const float airCanAccept = 1.0f - glm::clamp(m_signature.humidity, 0.0f, 1.0f) * 0.8f;

        // Calibrado por teste (tests/test_surface_wetness.cpp), NAO por
        // estimativa: o valor anterior (0.035) secava a superficie em ~15 s, e
        // o comentario afirmava um minuto. Asfalto molhado nao seca em quinze
        // segundos. 0.014 da' ~60 s a 20 C com ar seco e calmo, e ~40 s num
        // dia limpo de 30 C - a ordem de grandeza certa para uma pancada.
        const float kBaseDryPerSecond = 0.014f;
        const float dry = kBaseDryPerSecond * thermal * wind * airCanAccept;
        m_surfaceWetness -= dry * dt;
    }

    m_surfaceWetness = glm::clamp(m_surfaceWetness, 0.0f, 1.0f);
}

void WeatherSystem::updateLightning(float dt) {
    m_lightningTimer -= dt;
    if (m_lightningTimer <= 0.0f) {
        m_current.lightningFlash = 0.0f;
        float chance = m_current.lightningChance * m_current.stormTint;
        if (chance > 0.0f && (rand() / float(RAND_MAX)) < chance * dt) {
            m_current.lightningFlash = 1.0f;
            m_lightningTimer = 0.15f + (rand() / float(RAND_MAX)) * 0.25f;
        }
    } else {
        m_current.lightningFlash = glm::max(0.0f, m_current.lightningFlash - dt * 8.0f);
    }
}

void WeatherSystem::applyPreset(WeatherPreset preset) {
    // Legacy bridge: old enum values map to the closest new WeatherType.
    switch (preset) {
        case WeatherPreset::PartlyCloudy: applyType(WeatherType::PartlyCloudy); break;
        case WeatherPreset::Cloudy:       applyType(WeatherType::Cloudy);       break;
        case WeatherPreset::Rainy:        applyType(WeatherType::Rainy);        break;
        case WeatherPreset::Stormy:       applyType(WeatherType::Stormy);       break;
        case WeatherPreset::Heatwave:     applyType(WeatherType::Heatwave);     break;
        case WeatherPreset::Foggy:        applyType(WeatherType::Foggy);        break;
        case WeatherPreset::Snowy:        applyType(WeatherType::Snowy);        break;
        default:                          applyType(WeatherType::Clear);        break;
    }
}

void WeatherSystem::applyType(WeatherType type, float intensity) {
    m_currentType = type;
    const auto& info = getWeatherTypeInfo(type);
    m_target = scaleWeatherParams(info.baseParams, intensity);
    m_target.weatherType = type;
    m_target.weatherIntensity = intensity;

    // ESCOLHER UM CLIMA MUDA O ESTADO ATMOSFERICO INTEIRO.
    //
    // O template de cada tipo autora a aparencia (nuvem, chuva, neblina), mas
    // nao dizia nada sobre TEMPERATURA e VENTO - quase todos ficavam nos 20 C
    // e num vento adivinhado dos sintomas. O resultado e' que escolher
    // "nevasca" nao esfriava o mundo e escolher "onda de calor" nao o
    // esquentava: o clima mudava de cara sem mudar de estado.
    //
    // O envelope de data/climate_types.json diz em que regiao do espaco
    // atmosferico o tipo vive; o CENTRO dessa regiao e' o estado que o
    // representa. Escolher o tipo leva o mundo para la', e a partir dai
    // temperatura, vento, secagem de superficie e o clima regional passam a
    // ser coerentes com a escolha.
    if (const ClimateEnvelope* env = m_climateTypes.find(type)) {
        const WeatherSignature c = env->centroid();
        m_target.temperatureC = signatureTempToCelsius(c.thermalEnergy);
        m_target.windiness = c.wind;
        // Interpolar a intensidade global em direcao ao temperado: uma
        // "meia nevasca" nao deve ser tao fria quanto uma inteira.
        const float k = glm::clamp(intensity, 0.0f, 1.0f);
        m_target.temperatureC = glm::mix(20.0f, m_target.temperatureC, k);
        m_target.windiness = glm::mix(0.12f, m_target.windiness, k);
    }

    // A sobreposicao de disco entra POR ULTIMO, depois do template de C++ e
    // depois do estado atmosferico do envelope: quem edita o JSON tem a
    // palavra final, inclusive sobre temperatura e vento, se quiser.
    applyTypeOverride(type, m_target);

    m_editor = m_target;
    m_transitionTime = 0.0f;

    // ERUPTION_TEST_WEATHER_SIGNATURE=1 (debug): imprime a assinatura fisica do
    // tipo aplicado. Serve para conferir que as propriedades aristotelicas
    // batem com o clima (tempestade = turbulencia alta, onda de calor =
    // termica alta e umidade baixa) sem precisar abrir a UI.
    if (std::getenv("ERUPTION_TEST_WEATHER_SIGNATURE")) {
        const WeatherSignature sg = deriveSignature(m_target);
        static const char* kPrecip[] = {"none", "rain", "snow", "hail", "freezing"};
        ERUPTION_LOG_WARN("[SIGNATURE] %-16s solar=%.2f nuvem=%.2f umid=%.2f turb=%.2f "
                          "termica=%+.2f precip=%.2f(%s) visib=%.2f vento=%.2f",
                          info.displayName, sg.solar, sg.cloudAmount, sg.humidity,
                          sg.turbulence, sg.thermalEnergy, sg.precipitation,
                          kPrecip[static_cast<int>(sg.precipType)], sg.visibility, sg.wind);
    }
}

WeatherPreset WeatherSystem::currentPreset() const {
    switch (m_currentType) {
        case WeatherType::PartlyCloudy: return WeatherPreset::PartlyCloudy;
        case WeatherType::Cloudy:       return WeatherPreset::Cloudy;
        case WeatherType::Rainy:        return WeatherPreset::Rainy;
        case WeatherType::Stormy:       return WeatherPreset::Stormy;
        case WeatherType::Heatwave:     return WeatherPreset::Heatwave;
        case WeatherType::Foggy:        return WeatherPreset::Foggy;
        case WeatherType::Snowy:        return WeatherPreset::Snowy;
        default:                        return WeatherPreset::Clear;
    }
}

void WeatherSystem::setCurrentPreset(WeatherPreset p) {
    applyPreset(p);
}

void WeatherSystem::setAutoHeatGroundHeight(float height) {
    m_autoHeatGroundHeight = height;
}

void WeatherSystem::updateLensDrops(float dt) {
    const WeatherParams& p = m_current;
    // Lens drops disabled (author request 2026-08-09): skip the CPU drip sim
    // entirely regardless of preset/slider values.
    float amount = 0.0f;
    if (amount <= 0.001f || p.lensDropMode == LensDropMode::Procedural) {
        m_lensDrops.clear();
        return;
    }

    int targetCount = glm::clamp(p.lensDropCount, 64, 16384);
    if (static_cast<int>(m_lensDropState.size()) != targetCount) {
        m_lensDropState.assign(targetCount, LensDropCPU{});
        m_lensDrops.assign(targetCount, LensDropGPU{});
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        for (auto& d : m_lensDropState) {
            d.x = u01(m_rng);
            d.y = u01(m_rng);
            d.radius = (0.008f + u01(m_rng) * 0.022f) * p.lensDropRadius;
            d.intensity = 0.3f + u01(m_rng) * 0.7f;
            d.maxLife = 2.0f + u01(m_rng) * 4.0f;
            d.life = u01(m_rng) * d.maxLife;
        }
    }

    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    float storm = p.stormTint;
    float windX = p.rainWind * 0.03f * (1.0f + storm);
    float gravity = 0.08f + storm * 0.12f + amount * 0.10f;

    for (size_t i = 0; i < m_lensDropState.size(); ++i) {
        auto& s = m_lensDropState[i];
        s.vy += gravity * dt;
        s.vx = glm::mix(s.vx, windX, dt * 2.0f);
        s.x += s.vx * dt;
        s.y += s.vy * dt;
        s.life -= dt;

        // Respawn if off-screen or dead (UV: y=0 top, y=1 bottom)
        if (s.y > 1.05f || s.life <= 0.0f || s.x < -0.05f || s.x > 1.05f) {
            s.x = u01(m_rng);
            s.y = -0.1f - u01(m_rng) * 0.1f;
            s.vx = windX * (0.8f + u01(m_rng) * 0.4f);
            s.vy = 0.0f;
            s.radius = (0.008f + u01(m_rng) * 0.020f * (0.6f + 0.4f * amount)) * p.lensDropRadius;
            s.intensity = 0.3f + u01(m_rng) * 0.7f;
            s.maxLife = 2.0f + u01(m_rng) * 4.0f;
            s.life = s.maxLife;
        }

        // Simple merging simulation: larger drops fall faster and absorb smaller neighbors
        if ((i & 7u) == 0u && amount > 0.4f) {
            size_t other = static_cast<size_t>(u01(m_rng) * m_lensDropState.size()) % m_lensDropState.size();
            if (other != i) {
                auto& o = m_lensDropState[other];
                float dx = s.x - o.x;
                float dy = s.y - o.y;
                float dist = glm::sqrt(dx * dx + dy * dy);
                float mergeDist = s.radius + o.radius;
                if (dist < mergeDist && s.radius >= o.radius) {
                    float area = s.radius * s.radius + o.radius * o.radius * 0.5f;
                    // Cap the merged radius: without it a drop that keeps
                    // eating neighbours grows to 0.2+ UV within one lifetime
                    // (a 400+ px refracting blob that warps/darkens a whole
                    // screen region instead of a small trickling bead).
                    s.radius = glm::min(glm::sqrt(area), 0.045f * p.lensDropRadius);
                    s.intensity = glm::min(1.0f, s.intensity + o.intensity * 0.3f);
                    s.vy += o.vy * 0.2f;
                    o.radius = 0.001f;
                    o.intensity = 0.0f;
                }
            }
        }

        // Write GPU layout
        m_lensDrops[i].posX = s.x;
        m_lensDrops[i].posY = s.y;
        m_lensDrops[i].radius = s.radius;
        m_lensDrops[i].intensity = s.intensity;
    }
}

float WeatherSystem::temperatureC() const {
    return m_current.temperatureC;
}

} // namespace eruption
