// Chao molhado (ver Engine.hpp, setWetness/wetArea): o molhado global da
// chuva, que encharca rapido e seca pelo clima, e as areas molhadas pelo
// script, que secam no tempo dado. O shader (lighting/wet_cover.glsl) so'
// molha pela chuva onde ela chega.
#include "core/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

void Engine::setWetness(float target, float seconds) {
    m_wetForced = target < 0.0f ? -1.0f : std::min(target, 1.0f);
    m_wetForcedSeconds = std::max(seconds, 0.0f);
}

void Engine::wetArea(const Vec3& pos, float radius, float amount, float drySeconds) {
    if (radius <= 0.0f || amount <= 0.0f) return;
    WetArea a{Vec2(pos.x, pos.z), radius, std::min(amount, 1.0f), std::max(drySeconds, 0.1f), 0.0f};
    // Cheio (o UBO leva kWetAreas): sai a que esta' mais seca agora.
    if (m_wetAreas.size() >= DeferredLighting::kWetAreas) {
        auto left = [](const WetArea& w) { return w.amount * (1.0f - w.age / w.dry); };
        auto weakest = std::min_element(m_wetAreas.begin(), m_wetAreas.end(),
                                        [&](const WetArea& x, const WetArea& y) { return left(x) < left(y); });
        if (left(*weakest) > a.amount) return;
        *weakest = a;
        return;
    }
    m_wetAreas.push_back(a);
}

void Engine::updateWetness(float dt) {
    // Mapa novo: as areas e a grade de cobertura eram do outro.
    const WeatherRenderer& wr = m_postProcessor.weatherRenderer();
    if (m_wetMap != m_currentMap.get()) {
        m_wetMap = m_currentMap.get();
        m_wetAreas.clear();
        m_wetCoverSerial = wr.heightmapSerial() + 1;
    }
    if (m_classic.on) {
        // Classico: sem PBR molhado, nem o que ja' estava.
        m_groundWet = 0.0f;
        m_wetAreas.clear();
    } else if (m_wetForced >= 0.0f) {
        // O script manda: anda ate' o alvo na velocidade pedida (0 = ja').
        const float step = m_wetForcedSeconds > 0.0f ? dt / m_wetForcedSeconds : 1.0f;
        if (m_groundWet < m_wetForced) m_groundWet = std::min(m_wetForced, m_groundWet + step);
        else m_groundWet = std::max(m_wetForced, m_groundWet - step);
    } else {
        // Clima: encharca em ~6 s com a chuva; parada, seca pelo ar
        // (calor, vento e umidade: WeatherSystem::dryingRate, ~60 s a 20 C).
        // Abaixo de zero nao seca, e o shader ja' nao molha (vira gelo).
        const float rain = m_weatherSystem.current().rainIntensity;
        const float alvo = rain > 0.02f ? std::min(1.0f, 0.4f + rain) : 0.0f;
        if (alvo > m_groundWet) m_groundWet += (alvo - m_groundWet) * (1.0f - std::exp(-dt / 6.0f));
        else m_groundWet = std::max(alvo, m_groundWet - m_weatherSystem.dryingRate() * dt);
    }
    // As areas secam linear no tempo de cada uma.
    std::vector<Vec4> areas;
    areas.reserve(m_wetAreas.size());
    for (WetArea& a : m_wetAreas) a.age += dt;
    m_wetAreas.erase(std::remove_if(m_wetAreas.begin(), m_wetAreas.end(), [](const WetArea& a) { return a.age >= a.dry; }),
                     m_wetAreas.end());
    for (const WetArea& a : m_wetAreas) areas.emplace_back(a.xz.x, a.xz.y, a.radius, a.amount * (1.0f - a.age / a.dry));
    m_deferredLighting.setWetAreas(areas);
    // Onde a chuva chega: a grade de cima da chuva (do quadro anterior; o
    // passe dela roda depois da iluminacao). Sem ela (celular, sem chuva
    // ainda neste mapa), molha tudo.
    if (wr.heightmapSerial() >= m_wetCoverSerial && wr.heightmapWritten() && m_postSettings.weatherTier != WeatherTier::Mobile)
        m_deferredLighting.setRainCover(wr.heightmapView(), wr.heightmapOrigin(), wr.heightmapWorldSize(),
                                        m_weatherSystem.current().rainShadowSoftness);
    else
        m_deferredLighting.setRainCover(VK_NULL_HANDLE, Vec2(0.0f), 0.0f, 0.0f);
}

} // namespace eruption
