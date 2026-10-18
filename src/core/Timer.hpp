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
    // Pausa da simulação (Pause do editor): o passo e o tempo do mundo param
    // (nuvens, clima, vento, água, animações); o relógio real e o FPS seguem.
    void setPaused(bool paused) { m_paused = paused; }
    bool paused() const { return m_paused; }
    // ESCALA DE TEMPO (hit-stop, camera lenta): deltaTime() = passo x escala,
    // e para tudo que e' mundo (animacao de sprite, VFX, clima, scripts).
    // `seconds` conta no passo SEM escala (um hit-stop de 0 termina sozinho);
    // <= 0 = ate' a proxima chamada.
    void setTimeScale(float scale, float seconds = 0.0f) {
        m_timeScale = scale < 0.0f ? 0.0f : scale;
        m_timeScaleLeft = seconds;
    }
    float timeScale() const { return m_timeScale; }
    // Passo sem a escala (respeita o passo fixo de teste e a pausa): o que
    // tem de seguir andando no congelamento - clarao de tela, gravacao do som.
    float unscaledDeltaTime() const { return m_unscaledDeltaTime; }
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
    bool m_paused = false;

    float m_deltaTime = 0.0f;
    float m_unscaledDeltaTime = 0.0f;
    float m_timeScale = 1.0f;
    float m_timeScaleLeft = 0.0f;
    float m_realDeltaTime = 0.0f;
    float m_elapsed = 0.0f;
    float m_fps = 0.0f;
    uint32_t m_frameCount = 0;
    uint32_t m_framesSinceLastUpdate = 0;
};

} // namespace eruption
