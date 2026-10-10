#include "core/Timer.hpp"

#include <algorithm>

#include <cstdlib>

namespace eruption {

Timer::Timer() {
    if (const char* e = std::getenv("ERUPTION_TEST_FIXED_DT")) {
        m_fixedDt = std::strtof(e, nullptr);
        if (!(m_fixedDt > 0.0f)) m_fixedDt = 0.0f;
    }
    reset();
}

void Timer::reset() {
    m_startTime = Clock::now();
    m_lastFrame = m_startTime;
    m_lastFpsUpdate = m_startTime;
    m_deltaTime = 0.0f;
    m_realDeltaTime = 0.0f;
    m_elapsed = 0.0f;
    m_fps = 0.0f;
    m_frameCount = 0;
    m_framesSinceLastUpdate = 0;
}

void Timer::tick() {
    TimePoint now = Clock::now();
    m_realDeltaTime = std::chrono::duration<float>(now - m_lastFrame).count();
    if (m_paused) {
        m_deltaTime = 0.0f; // o tempo do mundo fica parado
    } else if (m_fixedDt > 0.0f) {
        // Passo fixo: frame N sempre corresponde ao mesmo instante de simulação.
        m_deltaTime = m_fixedDt;
        m_elapsed += m_fixedDt;
    } else {
        // Acumulado (não "agora - início"): voltando da pausa, o tempo segue de
        // onde parou em vez de saltar.
        m_deltaTime = std::min(std::chrono::duration<float>(now - m_lastFrame).count(), 0.25f);
        m_elapsed += m_deltaTime;
    }
    m_lastFrame = now;

    ++m_frameCount;
    ++m_framesSinceLastUpdate;

    auto fpsDelta = std::chrono::duration<float>(now - m_lastFpsUpdate).count();
    if (fpsDelta >= 1.0f) {
        m_fps = static_cast<float>(m_framesSinceLastUpdate) / fpsDelta;
        m_framesSinceLastUpdate = 0;
        m_lastFpsUpdate = now;
    }
}

} // namespace eruption
