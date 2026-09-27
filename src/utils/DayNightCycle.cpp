#include "utils/DayNightCycle.hpp"

namespace eruption {

void DayNightCycle::init(float dayDurationSeconds) {
    m_dayDuration = dayDurationSeconds;
    initKeyframes();
    syncCurvesFromKeyframes();
}

void DayNightCycle::initKeyframes() {
    m_keyframes[0] = {0.00f, Vec3(0.02f, 0.02f, 0.05f), Vec3(0.05f, 0.05f, 0.1f), 8000.0f, Vec3(0.3f, -0.5f, 0.8f), 0.00f, Vec3(0.1f, 0.1f, 0.2f), 0.15f, Vec3(0.7f, 0.7f, 0.9f), 0.3f, 0.02f, Vec3(0.05f, 0.05f, 0.1f)};
    m_keyframes[1] = {0.20f, Vec3(0.1f, 0.1f, 0.3f), Vec3(0.8f, 0.4f, 0.2f), 3500.0f, Vec3(0.5f, -0.3f, 0.8f), 0.15f, Vec3(0.3f, 0.25f, 0.4f), 0.3f, Vec3(0.7f, 0.7f, 0.9f), 0.4f, 0.015f, Vec3(0.3f, 0.2f, 0.3f)};
    m_keyframes[2] = {0.25f, Vec3(0.3f, 0.4f, 0.7f), Vec3(1.0f, 0.7f, 0.4f), 4500.0f, Vec3(0.6f, -0.1f, 0.8f), 0.60f, Vec3(0.4f, 0.4f, 0.5f), 0.4f, Vec3(0.7f, 0.7f, 0.9f), 0.3f, 0.01f, Vec3(0.5f, 0.4f, 0.3f)};
    m_keyframes[3] = {0.35f, Vec3(0.4f, 0.6f, 0.9f), Vec3(0.7f, 0.8f, 0.9f), 5500.0f, Vec3(0.4f, 0.2f, 0.9f), 1.0f, Vec3(0.5f, 0.5f, 0.6f), 0.5f, Vec3(0.7f, 0.7f, 0.9f), 0.2f, 0.008f, Vec3(0.6f, 0.7f, 0.8f)};
    m_keyframes[4] = {0.50f, Vec3(0.3f, 0.5f, 1.0f), Vec3(0.6f, 0.8f, 1.0f), 5500.0f, Vec3(0.3f, 0.5f, 0.8f), 1.0f, Vec3(0.6f, 0.6f, 0.7f), 0.6f, Vec3(0.7f, 0.7f, 0.9f), 0.1f, 0.005f, Vec3(0.6f, 0.7f, 0.9f)};
    m_keyframes[5] = {0.65f, Vec3(0.3f, 0.45f, 0.8f), Vec3(0.7f, 0.7f, 0.9f), 5000.0f, Vec3(0.2f, 0.3f, 0.9f), 0.80f, Vec3(0.5f, 0.5f, 0.6f), 0.5f, Vec3(0.7f, 0.7f, 0.9f), 0.15f, 0.008f, Vec3(0.6f, 0.6f, 0.8f)};
    m_keyframes[6] = {0.75f, Vec3(0.2f, 0.2f, 0.5f), Vec3(1.0f, 0.5f, 0.2f), 3200.0f, Vec3(0.5f, -0.2f, 0.8f), 0.40f, Vec3(0.4f, 0.3f, 0.4f), 0.35f, Vec3(0.7f, 0.7f, 0.9f), 0.3f, 0.012f, Vec3(0.5f, 0.3f, 0.2f)};
    m_keyframes[7] = {0.80f, Vec3(0.02f, 0.02f, 0.15f), Vec3(0.4f, 0.15f, 0.25f), 2500.0f, Vec3(0.4f, -0.4f, 0.8f), 0.00f, Vec3(0.2f, 0.15f, 0.3f), 0.5f, Vec3(0.8f, 0.8f, 1.0f), 0.45f, 0.018f, Vec3(0.15f, 0.1f, 0.2f)};
}

void DayNightCycle::syncCurvesFromKeyframes() {
    std::vector<float> times;
    std::vector<float> kelvins, sunInt, ambInt, moonInt, fogDens;
    for (uint32_t i = 0; i < KEYFRAME_COUNT; i++) {
        times.push_back(m_keyframes[i].timeOfDay);
        kelvins.push_back(m_keyframes[i].sunKelvin);
        sunInt.push_back(m_keyframes[i].sunIntensity);
        ambInt.push_back(m_keyframes[i].ambientIntensity);
        moonInt.push_back(m_keyframes[i].moonIntensity);
        fogDens.push_back(m_keyframes[i].fogDensity);
    }
    sunKelvinCurve = SplineCurve(times, kelvins, InterpolationMode::Smoothstep);
    sunIntensityCurve = SplineCurve(times, sunInt, InterpolationMode::Smoothstep);
    ambientIntensityCurve = SplineCurve(times, ambInt, InterpolationMode::Smoothstep);
    moonIntensityCurve = SplineCurve(times, moonInt, InterpolationMode::Smoothstep);
    fogDensityCurve = SplineCurve(times, fogDens, InterpolationMode::Smoothstep);
}

void DayNightCycle::syncKeyframesFromCurves() {
    for (uint32_t i = 0; i < KEYFRAME_COUNT; i++) {
        if (i < sunKelvinCurve.pointsX.size()) m_keyframes[i].timeOfDay = sunKelvinCurve.pointsX[i];
        if (i < sunKelvinCurve.pointsY.size()) m_keyframes[i].sunKelvin = sunKelvinCurve.pointsY[i];
        if (i < sunIntensityCurve.pointsY.size()) m_keyframes[i].sunIntensity = sunIntensityCurve.pointsY[i];
        if (i < ambientIntensityCurve.pointsY.size()) m_keyframes[i].ambientIntensity = ambientIntensityCurve.pointsY[i];
        if (i < moonIntensityCurve.pointsY.size()) m_keyframes[i].moonIntensity = moonIntensityCurve.pointsY[i];
        if (i < fogDensityCurve.pointsY.size()) m_keyframes[i].fogDensity = fogDensityCurve.pointsY[i];
        
        if (i < sunIntensityCurve.pointsX.size()) sunIntensityCurve.pointsX[i] = m_keyframes[i].timeOfDay;
        if (i < ambientIntensityCurve.pointsX.size()) ambientIntensityCurve.pointsX[i] = m_keyframes[i].timeOfDay;
        if (i < moonIntensityCurve.pointsX.size()) moonIntensityCurve.pointsX[i] = m_keyframes[i].timeOfDay;
        if (i < fogDensityCurve.pointsX.size()) fogDensityCurve.pointsX[i] = m_keyframes[i].timeOfDay;
    }
}

void DayNightCycle::update(float deltaTime) {
    if (m_paused) return;
    m_timeOfDay += (deltaTime * m_timeScale) / m_dayDuration;
    if (m_timeOfDay >= 1.0f) {
        m_timeOfDay -= 1.0f;
        m_currentDay += 1.0f;
    }
}

void DayNightCycle::setTimeOfDay(float t) {
    m_timeOfDay = glm::clamp(t, 0.0f, 1.0f);
}

void DayNightCycle::setPaused(bool paused) {
    m_paused = paused;
}

void DayNightCycle::setDayDuration(float seconds) {
    m_dayDuration = seconds;
}

void DayNightCycle::setTimeScale(float scale) {
    m_timeScale = glm::clamp(scale, 0.0f, 10.0f);
}

const DayNightKeyFrame& DayNightCycle::getKeyFrames(float t, float& alpha) const {
    uint32_t idx = 0;
    for (uint32_t i = 0; i < KEYFRAME_COUNT - 1; i++) {
        if (t >= m_keyframes[i].timeOfDay && t < m_keyframes[i + 1].timeOfDay) {
            idx = i;
            break;
        }
    }
    if (t >= m_keyframes[KEYFRAME_COUNT - 1].timeOfDay) {
        idx = KEYFRAME_COUNT - 1;
        alpha = (t - m_keyframes[idx].timeOfDay) / (1.0f + m_keyframes[0].timeOfDay - m_keyframes[idx].timeOfDay);
        return m_keyframes[idx];
    }
    alpha = (t - m_keyframes[idx].timeOfDay) / (m_keyframes[idx + 1].timeOfDay - m_keyframes[idx].timeOfDay);
    return m_keyframes[idx];
}

Vec3 DayNightCycle::lerpColor(const Vec3& a, const Vec3& b, float t) const {
    return glm::mix(a, b, t);
}

float DayNightCycle::smootherstep(float t) const {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

Vec3 DayNightCycle::kelvinToRGB(float kelvin) {
    float t = glm::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    Vec3 c;
    c.r = (t <= 66.0f) ? 1.0f : glm::clamp(1.292936f * std::pow(t - 60.0f, -0.1332047592f), 0.0f, 1.0f);
    c.g = (t <= 66.0f) ? glm::clamp(0.3900815788f * std::log(t) - 0.6318414438f, 0.0f, 1.0f)
                       : glm::clamp(1.12989086f * std::pow(t - 60.0f, -0.0755148492f), 0.0f, 1.0f);
    c.b = (t >= 66.0f) ? 1.0f : (t <= 19.0f) ? 0.0f
           : glm::clamp(0.5432067891f * std::log(t - 10.0f) - 1.19625408914f, 0.0f, 1.0f);
    return c;
}

Vec3 DayNightCycle::getSkyTopColor() const {
    float alpha; const auto& k = getKeyFrames(m_timeOfDay, alpha);
    uint32_t nextIdx = (&k - m_keyframes + 1) % KEYFRAME_COUNT;
    return lerpColor(k.skyTopColor, m_keyframes[nextIdx].skyTopColor, alpha);
}

Vec3 DayNightCycle::getSkyHorizonColor() const {
    float alpha; const auto& k = getKeyFrames(m_timeOfDay, alpha);
    uint32_t nextIdx = (&k - m_keyframes + 1) % KEYFRAME_COUNT;
    return lerpColor(k.skyHorizonColor, m_keyframes[nextIdx].skyHorizonColor, alpha);
}

float DayNightCycle::getSunKelvin() const {
    return sunKelvinCurve.evaluate(m_timeOfDay);
}

Vec3 DayNightCycle::getSunColor() const {
    // Returns the raw Kelvin color; callers multiply by getSunIntensity() so
    // intensity is applied exactly once (was previously doubled).
    return kelvinToRGB(getSunKelvin());
}

// Direcao ATE o astro (aponta para cima quando ele esta' no ceu).
//
// O arco de dia vai de 0,20 (nasce, SE) a 0,80 (se poe, NW). Fora dessa janela
// o codigo antigo CLAMPAVA o dayProgress, e o resultado era que o astro ficava
// estacionado EXATAMENTE na linha do horizonte a noite inteira, com pitch 0.
// O skybox entao desenhava disco, coroa e raios de difracao a meia-noite -
// "puta ta' um sol de noite" (autor, 2026-09-06) - e sunHeight ficava preso em
// 0, o que mantinha dayBlend em 0,36 e as estrelas em 33% de opacidade no meio
// da noite.
//
// extendBelowHorizon=true continua o arco POR BAIXO do chao nos 0,40 de dia que
// sobram: o yaw segue de NW de volta para SE e o pitch espelha para negativo,
// fechando o circulo sem descontinuidade (no crepusculo o pitch passa por zero
// vindo de cima, na alvorada volta por zero subindo). E' o que o ceu precisa.
//
// extendBelowHorizon=false mantem o arco clampado do jeito que sempre foi - a
// ILUMINACAO e as sombras continuam com o sol parado no horizonte a noite, que
// e' o comportamento em cima do qual a cena noturna e a demo estao ajustadas.
Vec3 DayNightCycle::computeVisualDirection(float timeOfDay, float pitchMax,
                                           bool extendBelowHorizon) const {
    const float visibleStart = 0.20f;
    const float visibleEnd = 0.80f;
    const float yawStart = glm::radians(45.0f);    // SE
    const float yawSweep = glm::radians(180.0f);   // ate' NW
    const float PI = 3.14159265f;

    float t = timeOfDay - std::floor(timeOfDay);   // 0..1
    float yaw;
    float pitch;

    if (!extendBelowHorizon || (t >= visibleStart && t <= visibleEnd)) {
        float dayProgress = (t - visibleStart) / (visibleEnd - visibleStart);
        dayProgress = glm::clamp(dayProgress, 0.0f, 1.0f);
        yaw = yawStart + yawSweep * dayProgress;
        pitch = pitchMax * std::sin(dayProgress * PI);
    } else {
        // Noite: 0,40 de dia entre o poente e o nascente, do outro lado.
        const float nightSpan = 1.0f - (visibleEnd - visibleStart);
        float q = (t > visibleEnd) ? (t - visibleEnd) / nightSpan
                                   : (t + (1.0f - visibleEnd)) / nightSpan;
        yaw = yawStart + yawSweep * (1.0f + q);     // NW -> SE, fechando a volta
        pitch = -pitchMax * std::sin(q * PI);       // mais fundo a meia-noite
    }

    Vec3 dir;
    dir.x = std::cos(pitch) * std::cos(yaw);
    dir.y = std::sin(pitch);
    dir.z = std::cos(pitch) * std::sin(yaw);

    return dir;
}

Vec3 DayNightCycle::getSunDirection() const {
    // Direction the light TRAVELS (opposite to the visual sun, points down toward ground)
    // — used for lighting & shadows. Arco CLAMPADO de proposito: mexer nele
    // mudaria a direcao da luz e das sombras a noite.
    return -computeVisualDirection(m_timeOfDay, glm::radians(m_sunPitchMax), false);
}

Vec3 DayNightCycle::getSunVisualDirection() const {
    // Direction TO the sun in the sky (points up) — used for skybox rendering
    return computeVisualDirection(m_timeOfDay, glm::radians(m_sunPitchMax), true);
}

Vec3 DayNightCycle::getMoonVisualDirection() const {
    // Direction TO the moon in the sky (points up) — moon is 180° offset from sun
    float moonTime = m_timeOfDay + 0.5f;
    if (moonTime > 1.0f) moonTime -= 1.0f;
    return computeVisualDirection(moonTime, glm::radians(m_moonPitchMax), true);
}

void DayNightCycle::setSunPitchMax(float degrees) {
    m_sunPitchMax = glm::clamp(degrees, 5.0f, 89.0f);
}

void DayNightCycle::setMoonPitchMax(float degrees) {
    m_moonPitchMax = glm::clamp(degrees, 5.0f, 89.0f);
}

float DayNightCycle::getSunYaw(float timeOfDay) {
    float visibleStart = 0.20f;
    float visibleEnd = 0.80f;
    float yawStart = glm::radians(45.0f);   // SE
    float yawEnd   = glm::radians(225.0f);  // NW
    
    if (timeOfDay < visibleStart || timeOfDay > visibleEnd) {
        // Night: moon follows same arc
        float moonTime = (timeOfDay + 0.5f);
        if (moonTime > 1.0f) moonTime -= 1.0f;
        float moonAngle = (moonTime - visibleStart) / (visibleEnd - visibleStart);
        moonAngle = glm::clamp(moonAngle, 0.0f, 1.0f);
        return glm::mix(yawStart, yawEnd, moonAngle);
    }
    
    float dayProgress = (timeOfDay - visibleStart) / (visibleEnd - visibleStart);
    dayProgress = glm::clamp(dayProgress, 0.0f, 1.0f);
    return glm::mix(yawStart, yawEnd, dayProgress);
}

float DayNightCycle::getSunIntensity() const {
    return sunIntensityCurve.evaluate(m_timeOfDay);
}

Vec3 DayNightCycle::getAmbientColor() const {
    float alpha; const auto& k = getKeyFrames(m_timeOfDay, alpha);
    uint32_t nextIdx = (&k - m_keyframes + 1) % KEYFRAME_COUNT;
    return lerpColor(k.ambientColor, m_keyframes[nextIdx].ambientColor, alpha);
}

float DayNightCycle::getAmbientIntensity() const {
    return ambientIntensityCurve.evaluate(m_timeOfDay);
}

Vec3 DayNightCycle::getMoonColor() const {
    float alpha; const auto& k = getKeyFrames(m_timeOfDay, alpha);
    uint32_t nextIdx = (&k - m_keyframes + 1) % KEYFRAME_COUNT;
    return lerpColor(k.moonColor, m_keyframes[nextIdx].moonColor, alpha);
}

float DayNightCycle::getMoonIntensity() const {
    return moonIntensityCurve.evaluate(m_timeOfDay);
}

float DayNightCycle::getMoonPhase() const {
    // Full moon at midnight (timeOfDay=0), new moon at noon (timeOfDay=0.5)
    float angle = (m_timeOfDay + 0.5f) * 2.0f * 3.14159265f;
    return 0.5f * (1.0f - std::cos(angle));
}

Vec3 DayNightCycle::getFogColor() const {
    float alpha; const auto& k = getKeyFrames(m_timeOfDay, alpha);
    uint32_t nextIdx = (&k - m_keyframes + 1) % KEYFRAME_COUNT;
    return lerpColor(k.fogColor, m_keyframes[nextIdx].fogColor, alpha);
}

float DayNightCycle::getFogDensity() const {
    return fogDensityCurve.evaluate(m_timeOfDay);
}

void DayNightCycle::loadFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    m_dayDuration = j.value("day_duration", m_dayDuration);
    m_timeOfDay = j.value("initial_time", m_timeOfDay);
    m_timeScale = j.value("time_scale", m_timeScale);
    m_sunPitchMax = j.value("sun_pitch_max", m_sunPitchMax);
    m_moonPitchMax = j.value("moon_pitch_max", m_moonPitchMax);

    if (j.contains("keyframes") && j["keyframes"].is_array() && j["keyframes"].size() == KEYFRAME_COUNT) {
        for (uint32_t i = 0; i < KEYFRAME_COUNT; i++) {
            const auto& k = j["keyframes"][i];
            m_keyframes[i].timeOfDay = k.value("time", m_keyframes[i].timeOfDay);
            if (k.contains("sky_top") && k["sky_top"].is_array() && k["sky_top"].size() >= 3)
                m_keyframes[i].skyTopColor = Vec3(k["sky_top"][0], k["sky_top"][1], k["sky_top"][2]);
            if (k.contains("sky_horizon") && k["sky_horizon"].is_array() && k["sky_horizon"].size() >= 3)
                m_keyframes[i].skyHorizonColor = Vec3(k["sky_horizon"][0], k["sky_horizon"][1], k["sky_horizon"][2]);
            m_keyframes[i].sunKelvin = k.value("sun_kelvin", m_keyframes[i].sunKelvin);
            if (k.contains("sun_direction") && k["sun_direction"].is_array() && k["sun_direction"].size() >= 3)
                m_keyframes[i].sunDirection = Vec3(k["sun_direction"][0], k["sun_direction"][1], k["sun_direction"][2]);
            m_keyframes[i].sunIntensity = k.value("sun_intensity", m_keyframes[i].sunIntensity);
            if (k.contains("ambient_color") && k["ambient_color"].is_array() && k["ambient_color"].size() >= 3)
                m_keyframes[i].ambientColor = Vec3(k["ambient_color"][0], k["ambient_color"][1], k["ambient_color"][2]);
            m_keyframes[i].ambientIntensity = k.value("ambient_intensity", m_keyframes[i].ambientIntensity);
            if (k.contains("moon_color") && k["moon_color"].is_array() && k["moon_color"].size() >= 3)
                m_keyframes[i].moonColor = Vec3(k["moon_color"][0], k["moon_color"][1], k["moon_color"][2]);
            m_keyframes[i].moonIntensity = k.value("moon_intensity", m_keyframes[i].moonIntensity);
            m_keyframes[i].fogDensity = k.value("fog_density", m_keyframes[i].fogDensity);
            if (k.contains("fog_color") && k["fog_color"].is_array() && k["fog_color"].size() >= 3)
                m_keyframes[i].fogColor = Vec3(k["fog_color"][0], k["fog_color"][1], k["fog_color"][2]);
        }
    }

    if (j.contains("curves")) {
        const auto& curves = j["curves"];
        if (curves.contains("sun_kelvin") && curves["sun_kelvin"].contains("points"))
            sunKelvinCurve = SplineCurve::fromJson(curves["sun_kelvin"]["points"]);
        if (curves.contains("sun_intensity") && curves["sun_intensity"].contains("points"))
            sunIntensityCurve = SplineCurve::fromJson(curves["sun_intensity"]["points"]);
        if (curves.contains("ambient_intensity") && curves["ambient_intensity"].contains("points"))
            ambientIntensityCurve = SplineCurve::fromJson(curves["ambient_intensity"]["points"]);
        if (curves.contains("moon_intensity") && curves["moon_intensity"].contains("points"))
            moonIntensityCurve = SplineCurve::fromJson(curves["moon_intensity"]["points"]);
        if (curves.contains("fog_density") && curves["fog_density"].contains("points"))
            fogDensityCurve = SplineCurve::fromJson(curves["fog_density"]["points"]);
    }
}

} // namespace eruption
