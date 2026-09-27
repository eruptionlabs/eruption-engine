#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>

namespace eruption {

enum class WeatherCategory {
    Clear,
    Cloudy,
    Rain,
    Snow,
    Heat,
    Fog,
    NightSky,
    Extreme,
    Count
};

const char* weatherCategoryName(WeatherCategory category);

// Legacy 8-preset enum. Kept for backwards compatibility with old configs / call sites.
enum class WeatherPreset {
    Clear, PartlyCloudy, Cloudy, Rainy, Stormy, Heatwave, Foggy, Snowy
};

enum class WeatherType : uint32_t {
    // Clear
    Clear = 0,
    PartlyCloudy,
    LightWind,
    Pollen,
    SunHalo,
    GodRays,
    LightPillars,
    SunDogs,
    GreenFlash,
    RainbowCloud,
    // Cloudy
    Cloudy,
    Overcast,
    Smog,
    Haze,
    // Rain
    Drizzle,
    Rainy,
    Stormy,
    Thunderstorm,
    Hail,
    Rainbow,
    FreezingRain,
    // Snow
    LightSnow,
    Snowy,
    Blizzard,
    Frost,
    // Heat
    Heatwave,
    Sandstorm,
    DustStorm,
    DustDevil,
    Mirage,
    HeatHaze,
    // Fog
    Foggy,
    Mist,
    GroundFog,
    // NightSky
    AuroraBorealis,
    AuroraAustralis,
    ShootingStars,
    MeteorShower,
    MilkyWay,
    StarryNight,
    // Extreme
    VolcanicAsh,
    FireSmoke,
    Tornado,
    Waterspout,
    Downburst,

    Count
};

constexpr uint32_t WeatherTypeCount = static_cast<uint32_t>(WeatherType::Count);

const char* weatherTypeName(WeatherType type);
WeatherType weatherTypeFromName(const std::string& name);
WeatherCategory weatherTypeCategory(WeatherType type);
bool isWeatherTypeDayOnly(WeatherType type);
bool isWeatherTypeNightOnly(WeatherType type);

// Forward declaration; implementation lives in WeatherSystem.cpp
struct WeatherTypeInfo;
const WeatherTypeInfo& getWeatherTypeInfo(WeatherType type);

enum class WeatherTier {
    Mobile,
    Medium,
    High,
    Count
};

enum class LensDropMode {
    Procedural,   // pure fragment-shader fake drops
    CpuBuffer,    // CPU-simulated drops uploaded to SSBO
    GpuCompute,   // GPU compute-simulated drops
    Count
};

struct WeatherParams {
    // --- Type & intensity ---
    WeatherType weatherType = WeatherType::Clear;
    float weatherIntensity = 1.0f;     // 0..1 global multiplier over the type template

    // --- Chuva ---
    float rainIntensity = 0.0f;        // 0..1 densidade
    float rainSpeed = 1.0f;            // multiplicador de velocidade terminal
    float rainWind = 0.2f;             // componente horizontal XZ normalizado
    float rainTurbulence = 0.2f;       // 0..1 agitação
    float dropLensAmount = 0.0f;       // gotas na lente (screen-space)
    bool rainSplashEnabled = false;    // habilita splash de contato proporcional à chuva
    float rainSplashIntensity = 0.0f;  // [legacy/debug] splash manual; preferir effectiveRainSplashIntensity()
    float rainSplashAmount = 1.0f;     // 0..1 multiplicador global da quantidade de splash
    float rainSplashOpacity = 1.0f;    // 0..1 transparência/opacidade do splash
    float rainSplashRadius = 1.0f;     // multiplicador de raio do splash (0.5 deixava o anel subpixel)
    float rainCrownGain = 1.0f;        // multiplicador de intensidade do crown procedural (overlay screen-space); 1.0 = neutro
    float heightmapBlur = 0.5f;        // 0..1 suavização do heightmap de sombra de chuva
    float rainShadowSoftness = 2.0f;   // largura da transição suave da máscara de sombra (metros)

    // --- Rain streak shape tuning (anti-shimmer). Hail/freezing rain scale
    // proportionally from these so the sliders move all precip together.
    float rainStreakWidth = 0.5f;      // largura da gota (widthScale do streak)
    float rainStreakLength = 12.0f;    // comprimento do streak (lengthScale)
    float rainStreakEdge = 1.0f;       // nitidez da borda lateral (falloff uv.x; maior = mais fina)
    float rainStreakTipFade = 0.5f;    // fade nas pontas (falloff uv.y)
    float rainStreakAlpha = 2.0f;      // ganho de alpha do streak
    float rainStreakBrightness = 2.5f; // ganho de cor do streak
    float rainPixelFade = 0.0f;        // >0: fade de alpha quando a largura projetada cai abaixo de N pixels (0 = off)

    float effectiveRainSplashIntensity() const {
        return rainSplashEnabled ? rainIntensity * 0.05f : 0.0f;
    }
    LensDropMode lensDropMode = LensDropMode::Procedural;
    int lensDropCount = 2048;          // active drops for CPU/GPU modes
    float lensDropRadius = 1.0f;       // global radius multiplier for lens drops

    // --- Neve ---
    float snowIntensity = 0.0f;        // 0..1 densidade
    float snowSpeed = 0.5f;            // multiplicador de velocidade de queda
    float snowSize = 1.0f;             // multiplicador de tamanho
    float snowWind = 0.3f;             // componente horizontal XZ normalizado
    float snowTurbulence = 0.5f;       // 0..1 turbulência
    float snowAccumulation = 0.0f;     // 0..1 (High tier)

    // --- Calor ---
    float heatShimmer = 0.0f;          // 0..1 intensidade global
    float heatSpeed = 1.0f;            // velocidade da ondulação
    float heatScale = 1.0f;            // escala do noise
    float heatWorldHeight = 0.0f;      // altura do chão para origem do calor (auto + offset)
    bool heatWorldHeightAuto = true;   // quando true, heatWorldHeight é atualizado do terreno
    float heatWorldHeightOffset = 0.0f;// offset artístico sobre a altura do terreno

    // --- Neblina ---
    float fogDensity = 0.0f;           // 0..1
    float fogStart = 50.0f;            // m
    float fogEnd = 1000.0f;            // m
    float fogHeight = 0.0f;            // base da neblina (m)
    float fogHeightFalloff = 0.01f;    // atenuação vertical

    // --- Nuvens ---
    float cloudCoverage = 0.0f;        // 0..1
    float cloudSpeed = 0.03f;          // multiplicador de velocidade
    float cloudThickness = 0.5f;       // 0..1
    float stormTint = 0.0f;            // 0..1

    // Visual tuning of the active CloudLayerRenderer.
    float cloudScale = 1.5f;           // procedural detail / puff scale
    float cloudLightness = 1.0f;       // brighten lit parts
    float cloudShade = 0.55f;          // darken shadowed parts
    float cloudSoftness = 0.5f;        // edge softness

    // --- Trovão ---
    float lightningChance = 0.0f;      // 0..1 chance por segundo de flash
    float lightningFlash = 0.0f;       // 0..1 estado atual do flash

    // --- Debris / tintes atmosféricos ---
    float windDebrisIntensity = 0.0f;  // 0..1 folhas/pólen/poeira no vento
    float atmosphericTint = 0.0f;      // 0..1 força do tinte atmosférico (areia/cinza/fogo)

    // --- Specialized overlay effects (sliders so any weather can borrow them) ---
    float rainbowIntensity = 0.0f;     // 0..1 rainbow arc strength
    float mirageIntensity = 0.0f;      // 0..1 distant-object reflection / heat mirage
    float shootingStarIntensity = 0.0f;// 0..1 shooting-star streaks
    float tornadoIntensity = 0.0f;     // 0..1 tornado / waterspout / downburst vortex
    float dustDevilIntensity = 0.0f;   // 0..1 small swirling dust column
    float auroraIntensity = 0.0f;      // 0..1 aurora curtains (night only)

    // --- Transição ---
    float transitionDuration = 2.0f;   // segundos

    // --- Estado climatico explicito ---
    // Temperatura e vento sao PROPRIEDADES DO CLIMA, nao consequencias dele.
    // Antes, a forca do vento era inferida dos sintomas (inclinacao da chuva,
    // tinte de tempestade, detritos) porque nenhum campo a declarava: um dia
    // limpo e ventando era indistinguivel de um dia limpo e parado, e "vento"
    // nao existia como coisa que se possa autorar. Agora existe, e o envelope
    // climatico (data/climate_types.json) define os dois por tipo.
    float temperatureC = 20.0f;        // temperatura ambiente da superficie, em Celsius
    float windiness = -1.0f;           // 0..1 forca do vento; <0 = derivar dos sintomas
                                       // (compatibilidade com config antiga)

    void loadFromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
    static WeatherParams lerp(const WeatherParams& a, const WeatherParams& b, float t);
};

// Scale a weather template by a global intensity while preserving its character.
WeatherParams scaleWeatherParams(const WeatherParams& base, float intensity);

enum class PrecipitationType : uint8_t {
    None,
    Rain,
    Snow,
    Hail,
    FreezingRain
};

// Semantic physical signature of a weather type. The WeatherSystem derives
// concrete rendering parameters from these causally related properties instead
// of hardcoding every slider for every named type (Aristotelian category system).
struct WeatherSignature {
    float solar = 1.0f;          // 0..1 insolation
    float cloudAmount = 0.0f;    // 0..1 cloud coverage
    float humidity = 0.0f;       // 0..1 atmospheric humidity
    float turbulence = 0.0f;     // 0..1 atmospheric instability
    float thermalEnergy = 0.0f;  // -1..1 deviation from temperate
    float precipitation = 0.0f;  // 0..1 intensity of falling precipitation
    PrecipitationType precipType = PrecipitationType::None;
    float visibility = 1.0f;     // 0..1 clear=1, fog/smog/dust low
    float wind = 0.0f;           // 0..1 horizontal wind strength
};

// Conversao entre a energia termica da assinatura (-1..1) e Celsius.
//
// O mapeamento e' ASSIMETRICO de proposito, porque a atmosfera e'. Antes era
// `20 + t*30`, linear e simetrico, o que dava -10 C no extremo artico: uma
// nevasca no polo saia mais quente que uma geada de inverno europeu, e o
// termo de congelamento do molhado (que so' age abaixo de zero) quase nunca
// disparava. Massa de ar artica real fica na casa dos -40 C; calor extremo
// chega perto de 50 C. O centro (t=0) e' o temperado, 20 C.
inline float signatureTempToCelsius(float t) {
    t = (t < -1.0f) ? -1.0f : ((t > 1.0f) ? 1.0f : t);
    return (t < 0.0f) ? (20.0f + t * 60.0f)   // t=-1 -> -40 C (artico)
                      : (20.0f + t * 30.0f);  // t=+1 -> +50 C (calor extremo)
}

inline float celsiusToSignatureTemp(float c) {
    const float t = (c < 20.0f) ? ((c - 20.0f) / 60.0f) : ((c - 20.0f) / 30.0f);
    return (t < -1.0f) ? -1.0f : ((t > 1.0f) ? 1.0f : t);
}

// Read the physical signature out of a concrete parameter set. The 44 weather
// templates already encode the physics in their sliders (cloudCoverage,
// rainIntensity, fogDensity, temperatureC, stormTint...), so the signature is
// derived from them instead of being hand-authored 44 times. One function, and
// it can never drift out of sync with the template it describes.
WeatherSignature deriveSignature(const WeatherParams& p);

// The Aristotelian direction: cause -> effect. Given a signature (a REGION is
// hot and dry because it is a volcano), push the concrete rendering parameters
// that follow from it. This is what lets a place on the map carry weather of
// its own instead of the world having one global state.
void applySignature(const WeatherSignature& sig, WeatherParams& p);

// Blend two signatures. Regional climate interpolates SIGNATURES, not sliders:
// halfway between "volcanic" and "temperate" is a physically meaningful state,
// whereas halfway between two slider sets is just an average of symptoms.
WeatherSignature lerpSignature(const WeatherSignature& a, const WeatherSignature& b, float t);

struct WeatherTypeInfo {
    const char* displayName;
    WeatherCategory category;
    bool dayOnly;
    bool nightOnly;
    WeatherSignature signature;
    WeatherParams baseParams;

    // `signature` is declared before `baseParams`, so it is initialized first;
    // deriveSignature reads the CONSTRUCTOR ARGUMENT, not the member, so there
    // is no initialization-order trap here.
    WeatherTypeInfo(const char* name, WeatherCategory cat, bool day, bool night,
                    const WeatherParams& params)
        : displayName(name), category(cat), dayOnly(day), nightOnly(night),
          signature(deriveSignature(params)), baseParams(params) {}
};

// GPU layout for a single lens drop. Matches CPU and compute paths.
struct LensDropGPU {
    float posX = 0.0f;     // screen x 0..1
    float posY = 0.0f;     // screen y 0..1
    float radius = 0.0f;   // screen-space radius
    float intensity = 0.0f;// 0..1 brightness/refraction strength
};
static_assert(sizeof(LensDropGPU) == 16, "LensDropGPU must be 16 bytes");

} // namespace eruption
