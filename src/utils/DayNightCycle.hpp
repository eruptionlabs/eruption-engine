#pragma once

#include "math/Types.hpp"
#include "utils/SplineCurve.hpp"
#include <nlohmann/json.hpp>

namespace eruption {

struct DayNightKeyFrame {
    float timeOfDay;
    Vec3 skyTopColor;
    Vec3 skyHorizonColor;
    float sunKelvin;        // Temperatura de cor do sol (K)
    Vec3 sunDirection;
    float sunIntensity;
    Vec3 ambientColor;
    float ambientIntensity;
    Vec3 moonColor;
    float moonIntensity;
    float fogDensity;
    Vec3 fogColor;
};

class DayNightCycle {
public:
    void init(float dayDurationSeconds = 600.0f);
    void update(float deltaTime);

    void setTimeOfDay(float t);
    void setPaused(bool paused);
    void setDayDuration(float seconds);
    void setTimeScale(float scale);
    void setSunPitchMax(float degrees);
    void setMoonPitchMax(float degrees);
    float sunPitchMax() const { return m_sunPitchMax; }
    float moonPitchMax() const { return m_moonPitchMax; }

    Vec3 getSkyTopColor() const;
    Vec3 getSkyHorizonColor() const;
    float getSunKelvin() const;
    Vec3 getSunColor() const; // Raw Kelvin color (multiply by getSunIntensity() separately)
    Vec3 getSunDirection() const;      // Direction light travels (points down/at ground)
    Vec3 getSunVisualDirection() const; // Direction TO the sun in sky (points up, opposite of light)
    Vec3 getMoonVisualDirection() const; // Direction TO the moon in sky
    float getSunIntensity() const;
    Vec3 getAmbientColor() const;
    float getAmbientIntensity() const;
    Vec3 getMoonColor() const;
    float getMoonIntensity() const;
    float getMoonPhase() const; // 0.0 = nova, 0.5 = quarto, 1.0 = cheia
    Vec3 getFogColor() const;
    float getFogDensity() const;

    float timeOfDay() const { return m_timeOfDay; }
    float currentDay() const { return m_currentDay; }
    bool isPaused() const { return m_paused; }
    float timeScale() const { return m_timeScale; }

    DayNightKeyFrame* getKeyframes() { return m_keyframes; }
    uint32_t getKeyframeCount() const { return KEYFRAME_COUNT; }

    // Visual Curves for editing
    SplineCurve sunKelvinCurve;
    SplineCurve sunIntensityCurve;
    SplineCurve ambientIntensityCurve;
    SplineCurve moonIntensityCurve;
    SplineCurve fogDensityCurve;

    // Synchronize curves from keyframes
    void syncCurvesFromKeyframes();
    // Synchronize keyframes from curves
    void syncKeyframesFromCurves();

    // Load from JSON config
    void loadFromJson(const nlohmann::json& j);

    // Utility: Helland's Kelvin to RGB
    static Vec3 kelvinToRGB(float kelvin);
    static float getSunYaw(float timeOfDay);

    // Direcao ATE o astro. extendBelowHorizon=true deixa o arco continuar por
    // baixo do chao fora da janela de dia (ceu); false clampa no horizonte,
    // que e' o que a iluminacao usa.
    Vec3 computeVisualDirection(float timeOfDay, float pitchMax,
                                bool extendBelowHorizon) const;

private:
    float m_timeOfDay = 0.25f;
    float m_currentDay = 0.0f;
    float m_dayDuration = 600.0f;
    float m_timeScale = 1.0f;
    bool m_paused = false;
    float m_sunPitchMax = 35.0f;
    float m_moonPitchMax = 25.0f;

    static constexpr uint32_t KEYFRAME_COUNT = 8;
    DayNightKeyFrame m_keyframes[KEYFRAME_COUNT];

    void initKeyframes();
    const DayNightKeyFrame& getKeyFrames(float t, float& alpha) const;
    Vec3 lerpColor(const Vec3& a, const Vec3& b, float t) const;
    float smootherstep(float t) const;
};

} // namespace eruption
