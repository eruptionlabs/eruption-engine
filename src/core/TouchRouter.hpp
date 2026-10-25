#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace eruption {

// Roteador de toque, o mesmo no celular (platform/android) e no navegador
// (core/EngineWeb.cpp). Cada dedo fica com o papel da area onde pousou, ate'
// sair:
// - INTERFACE: painel do ImGui (controle de toque) -> so' o ImGui;
// - ANALOGICO: zona do analogico flutuante (Input::inStickZone), fora de
//   elemento CSS tocavel -> a base nasce ali e o pino segue o dedo;
// - CAMERA: o resto. O primeiro dedo da camera e' o "mouse" (botao direito
//   arrasta e gira; toque curto vira clique esquerdo); o segundo faz a pinca.
// Um dedo nunca interfere no outro: o analogico anda enquanto o outro gira.
class TouchRouter {
public:
    // Para onde vao os eventos: o mouse do motor, o ImGui e a altura da tela.
    struct Sink {
        std::function<void(double x, double y)> cursor;  // cursor do motor
        std::function<void(int button, int action)> button; // botao do motor (GLFW_PRESS/RELEASE)
        std::function<void(double dy)> scroll;           // rolagem do motor (zoom)
        std::function<void(float x, float y)> uiCursor;  // cursor do ImGui
        std::function<void(int action)> uiButton;        // botao esquerdo do ImGui
        std::function<int()> windowHeight;
    };

    explicit TouchRouter(Sink sink) : m_sink(std::move(sink)) {}

    // Tempos pela hora do evento (ns): num quadro lento, aperto e soltura
    // chegam no mesmo lote e a hora do quadro diria que o toque durou zero.
    void down(int id, float x, float y, int64_t tNs);
    void move(int id, float x, float y);
    void up(int id, float x, float y, int64_t tNs, bool cancelled);
    void releaseAll();

private:
    enum class Role { Free, Interface, Stick, Camera, Ignored };
    struct Finger {
        int id = -1;
        Role role = Role::Free;
        float sx = 0.0f, sy = 0.0f, x = 0.0f, y = 0.0f;
        int64_t t0 = 0;
        bool moved = false;
    };
    static constexpr int64_t kShortTapNs = 300'000'000;

    int find(int id) const;
    bool hasRole(Role r) const;
    void button(int b, int action);

    Sink m_sink;
    std::array<Finger, 10> m_fingers;
    int m_camera = -1, m_pinch = -1;
    float m_pinchDist = 0.0f;
    int64_t m_lastTap = 0;
    float m_lastTapX = -1e9f, m_lastTapY = -1e9f;
    int m_buttons[3] = {0, 0, 0}; // estado mandado ao motor (sem repetir PRESS/RELEASE)
};

} // namespace eruption
