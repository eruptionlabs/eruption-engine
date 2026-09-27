#pragma once

#include <chrono>

namespace eruption {

class Timer {
public:
    Timer();

    void reset();
    void tick();

    float deltaTime() const { return m_deltaTime; }
    // Delta de RELOGIO DE PAREDE, sempre - mesmo com passo fixo ligado.
    // Qualquer coisa que MEÇA performance tem que usar este, nunca deltaTime():
    // com ERUPTION_TEST_FIXED_DT, 1/deltaTime() da' o passo fixo cravado
    // (60,0 FPS) e esconde o numero real. Ja' aconteceu duas vezes nesta base.
    float realDeltaTime() const { return m_realDeltaTime; }
    float elapsed() const { return m_elapsed; }
    float fps() const { return m_fps; }
    uint32_t frameCount() const { return m_frameCount; }

private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    TimePoint m_startTime;
    TimePoint m_lastFrame;
    TimePoint m_lastFpsUpdate;

    // ERUPTION_TEST_FIXED_DT=<segundos> (debug): avança o tempo de simulação
    // num passo fixo por frame em vez do relógio de parede. Sem isso, o frame N
    // cai num instante de simulação diferente a cada execução (o FPS varia), e
    // duas execuções idênticas divergem: medimos RMS 42 entre dois runs iguais,
    // 14x acima do limiar de regressão visual (3.0). O FPS medido continua
    // vindo do relógio real, então a telemetria de performance não é afetada.
    float m_fixedDt = 0.0f;

    float m_deltaTime = 0.0f;
    float m_realDeltaTime = 0.0f;
    float m_elapsed = 0.0f;
    float m_fps = 0.0f;
    uint32_t m_frameCount = 0;
    uint32_t m_framesSinceLastUpdate = 0;
};

} // namespace eruption
