#pragma once

#include "renderer/WeatherTypes.hpp"
#include "renderer/WindField.hpp"
#include "renderer/ClimateField.hpp"
#include "renderer/ClimateFuzzy.hpp"
#include "math/Types.hpp"
#include <vector>
#include <random>
#include <string>
#include <nlohmann/json.hpp>

namespace eruption {

class WeatherSystem {
public:
    void update(float dt);

    // Legacy bridge from the old 8-preset enum to the new WeatherType system.
    void applyPreset(WeatherPreset preset);

    // Main entry point: apply a weather type with a global intensity multiplier.
    void applyType(WeatherType type, float intensity = 1.0f);

    const WeatherParams& current() const { return m_current; }
    WeatherParams& target() { return m_target; }
    WeatherParams& editor() { return m_editor; }

    void setTransitionDuration(float seconds) { m_transitionDuration = glm::max(seconds, 0.1f); }
    float transitionDuration() const { return m_transitionDuration; }

    bool inTransition() const { return m_transitionTime < m_transitionDuration; }
    float transitionProgress() const { return glm::clamp(m_transitionTime / m_transitionDuration, 0.0f, 1.0f); }

    // Current active weather type.
    WeatherType currentType() const { return m_currentType; }
    void setCurrentType(WeatherType t) { m_currentType = t; }

    // Legacy preset accessor (returns Clear for any type not in the old enum).
    WeatherPreset currentPreset() const;
    void setCurrentPreset(WeatherPreset p);

    void snapToTarget();

    // Heat shimmer ground height: engine samples terrain and feeds it here.
    void setAutoHeatGroundHeight(float height);

    // Lens-drop simulation (CPU path)
    void updateLensDrops(float dt);
    const std::vector<LensDropGPU>& lensDrops() const { return m_lensDrops; }

    // Coerência atmosférica
    float sunOcclusion() const { return m_sunOcclusion; }
    float moonOcclusion() const { return m_moonOcclusion; }
    float effectiveRainIntensity() const { return m_effectiveRain; }
    float effectiveSnowIntensity() const { return m_effectiveSnow; }
    float effectiveHeatShimmer() const { return m_effectiveHeat; }

    // Molhado ACUMULADO da superficie (G4), 0..1. Diferente de
    // effectiveRainIntensity(), que e' quanta chuva esta' CAINDO agora: a rua
    // continua molhada depois que para de chover, e e' isto que a iluminacao
    // deve consumir. Molhar e' rapido; secar e' lento e depende de temperatura,
    // vento e umidade do ar.
    float surfaceWetness() const { return m_surfaceWetness; }

    // Approximate ambient temperature in Celsius from the active weather category
    // and intensity. Used by the PBR semantic material system.
    float temperatureC() const;
    // Wind (G6). These two used to be dead code returning a degenerate
    // Vec3(rainWind, 0, 0) -- always along X, never any Z, and nothing in the
    // engine called either one. They are now backed by the real WindField.
    const Vec3& windDirection() const { return m_windDirection; }
    float windStrength() const { return m_windStrength; }

    // The full field: gusts, wander, and per-position variation.
    const WindField& wind() const { return m_wind; }

    // Physical signature of the weather currently in effect (the Aristotelian
    // properties). Recomputed every frame from the live parameters, so it
    // tracks transitions instead of snapping at the end of one.
    const WeatherSignature& signature() const { return m_signature; }

    // Regional climate (G9). While the field has no regions the world behaves
    // exactly as before: one global weather state, bit-for-bit.
    // Envelopes difusos: em que regiao do espaco atmosferico cada clima vive.
    // Carregue com climateTypes().loadFromFile("data/climate_types.json").
    // Sobreposicoes de APARENCIA por tipo de clima, vindas de disco.
    //
    // Os 44 templates vivem em C++ (getWeatherTypeInfo) e continuam sendo o
    // padrao. Este arquivo so' SOBREPOE o que declarar, entao um tipo ausente
    // ou um campo ausente mantem exatamente o comportamento de antes, e o
    // autor pode afinar um clima sem recompilar nem mexer no C++.
    //
    // Separacao proposital de responsabilidades:
    //   climate_types.json  -> ONDE o clima vive no espaco atmosferico
    //                          (temperatura, umidade, vento, instabilidade)
    //   weather_types.json  -> COMO ele se parece
    //                          (nuvem, chuva, neblina, forma do streak...)
    bool loadTypeOverrides(const std::string& path);

    ClimateFuzzySet& climateTypes() { return m_climateTypes; }
    const ClimateFuzzySet& climateTypes() const { return m_climateTypes; }

    // Clima que MELHOR corresponde ao estado atmosferico corrente, junto da
    // confianca (1 = dentro do nucleo). Util para dirigir o clima pelo estado
    // do mundo em vez de escolher o tipo na mao.
    WeatherType classifyCurrent(float* outConfidence = nullptr) const {
        return m_climateTypes.classify(m_signature, outConfidence);
    }

    ClimateField& climate() { return m_climate; }
    const ClimateField& climate() const { return m_climate; }

    // Where the field is sampled. The engine feeds the player/camera anchor;
    // the resulting local signature modulates the weather the renderer sees.
    void setClimateSamplePosition(const Vec3& p) { m_climateSamplePos = p; }

    // The globally active weather, BEFORE the regional field modulates it.
    // current() is the localized result; this is what the weather UI edits.
    const WeatherParams& globalWeather() const { return m_global; }

    // Prevailing heading, fed from the authored cloud-layer config so clouds
    // and wind finally agree on which way the weather is moving. Engine calls
    // this; without it the field keeps its last heading.
    void setPrevailingWindDirection(const Vec3& dir) { m_prevailingWind = dir; }

    void setCloudOcclusionCurve(float curve) { m_cloudOcclusionCurve = curve; }
    float cloudOcclusionCurve() const { return m_cloudOcclusionCurve; }

private:
    WeatherParams m_current;
    WeatherParams m_target;
    WeatherParams m_editor;
    float m_transitionTime = 0.0f;
    float m_transitionDuration = 2.0f;
    WeatherType m_currentType = WeatherType::Clear;
    float m_autoHeatGroundHeight = 0.0f;

    // Estado de coerência calculado a cada update
    float m_sunOcclusion = 1.0f;
    float m_moonOcclusion = 1.0f;
    float m_effectiveRain = 0.0f;
    float m_effectiveSnow = 0.0f;
    float m_effectiveHeat = 0.0f;
    float m_surfaceWetness = 0.0f;
    Vec3 m_windDirection = Vec3(1.0f, 0.0f, 0.0f);
    float m_windStrength = 0.0f;
    WindField m_wind;
    Vec3 m_prevailingWind = Vec3(1.0f, 0.0f, -0.5f); // matches the cloud config default
    float m_windTarget = 0.0f;  // 0..1 wind strength the weather is asking for
    WeatherSignature m_signature;
    ClimateField m_climate;
    ClimateFuzzySet m_climateTypes;
    float m_climateReloadTimer = 0.0f;
    std::string m_overridePath;
    long long m_overrideMtime = 0;
    // Indexado por WeatherType; `has` diz se ha' sobreposicao para o tipo.
    std::vector<uint8_t> m_hasOverride;
    std::vector<nlohmann::json> m_overrides;
    void applyTypeOverride(WeatherType type, WeatherParams& p) const;
    bool reloadOverridesIfModified();
    Vec3 m_climateSamplePos = Vec3(0.0f);
    // The transition accumulator has to stay PURE. If the regional modulation
    // were written back into it, each frame would re-modulate an already
    // modulated value and the climate would compound during a transition.
    WeatherParams m_global;
    float m_cloudOcclusionCurve = 1.5f;
    float m_lightningTimer = 0.0f;

    // CPU lens-drop state
    struct LensDropCPU {
        float x = 0.0f, y = 0.0f;
        float vx = 0.0f, vy = 0.0f;
        float radius = 0.0f;
        float intensity = 0.0f;
        float life = 0.0f;       // remaining life in seconds
        float maxLife = 1.0f;    // total life in seconds
    };
    std::vector<LensDropCPU> m_lensDropState;
    std::vector<LensDropGPU> m_lensDrops;
    float m_lensDropSpawnTimer = 0.0f;
    std::mt19937 m_rng{std::random_device{}()};

    static float easeInOutCubic(float t);
    void updateCoherence();
    void updateSurfaceWetness(float dt);
    void updateLightning(float dt);
};

} // namespace eruption
