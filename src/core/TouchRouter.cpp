#include "core/TouchRouter.hpp"

#include "core/Input.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

namespace eruption {

int TouchRouter::find(int id) const {
    for (size_t i = 0; i < m_fingers.size(); ++i)
        if (m_fingers[i].role != Role::Free && m_fingers[i].id == id) return static_cast<int>(i);
    return -1;
}

bool TouchRouter::hasRole(Role r) const {
    for (const Finger& f : m_fingers)
        if (f.role == r) return true;
    return false;
}

// Dedos da camera e os ignorados (o terceiro dedo, o que sobra da pinca):
// o jogo decide o que fazer com eles. Interface e controle de toque ficam de fora.
void TouchRouter::publish() const {
    std::vector<Input::TouchPoint> pts;
    for (const Finger& f : m_fingers)
        if (f.role == Role::Camera || f.role == Role::Ignored) pts.push_back({f.id, f.x, f.y});
    Input::setTouches(std::move(pts));
}

void TouchRouter::button(int b, int action) {
    if (m_buttons[b] == action) return;
    m_buttons[b] = action;
    if (m_sink.button) m_sink.button(b, action);
}

void TouchRouter::click(float x, float y) {
    if (m_sink.cursor) m_sink.cursor(x, y);
    button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
}

void TouchRouter::releaseAll() {
    for (Finger& f : m_fingers) f = Finger{};
    m_camera = m_pinch = -1;
    Input::stickTouch(false, 0.0f, 0.0f);
    button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
    publish();
}

void TouchRouter::down(int id, float x, float y, int64_t tNs) {
    Input::markTouchScreen();
    int slot = -1;
    for (size_t i = 0; i < m_fingers.size(); ++i)
        if (m_fingers[i].role == Role::Free) { slot = static_cast<int>(i); break; }
    if (slot < 0) return;
    Finger& f = m_fingers[static_cast<size_t>(slot)];
    f = Finger{id, Role::Ignored, x, y, x, y, tNs, false};
    if (Input::inUiTouchRect(x, y) && !hasRole(Role::Interface)) {
        f.role = Role::Interface;
        if (m_sink.uiCursor) m_sink.uiCursor(x, y);
        if (m_sink.uiButton) m_sink.uiButton(GLFW_PRESS);
    } else if (Input::inCssTouchRect(x, y)) {
        // Botao do CSS: o toque e' dele. Zera o duplo toque (dois toques no
        // ataque reenquadravam a camera). Um segundo dedo noutro botao fica
        // so' em input.touches: o "mouse" e' um so'.
        m_lastTapX = m_lastTapY = -1e9f;
        if (!hasRole(Role::Button)) {
            f.role = Role::Button;
            if (m_sink.cursor) m_sink.cursor(x, y);
            button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
        }
    } else if (Input::inStickZone(x, y) && !hasRole(Role::Stick)) {
        f.role = Role::Stick;
        Input::stickTouch(true, x, y);
    } else if (m_camera < 0) {
        f.role = Role::Camera;
        m_camera = slot;
    } else if (m_pinch < 0) {
        // Segundo dedo na camera: so' pinca (zoom). Para de girar e o primeiro
        // dedo nao vira mais toque curto.
        f.role = Role::Camera;
        m_pinch = slot;
        Finger& c = m_fingers[static_cast<size_t>(m_camera)];
        c.moved = true;
        m_pinchDist = std::hypot(x - c.x, y - c.y);
    }
    publish();
}

void TouchRouter::move(int id, float x, float y) {
    const int slot = find(id);
    if (slot < 0) return;
    Finger& f = m_fingers[static_cast<size_t>(slot)];
    const float dx = x - f.x, dy = y - f.y;
    f.x = x;
    f.y = y;
    switch (f.role) {
        case Role::Interface:
            if (m_sink.uiCursor) m_sink.uiCursor(x, y);
            break;
        case Role::Button:
            if (std::hypot(x - f.sx, y - f.sy) > 12.0f) f.moved = true;
            if (m_sink.cursor) m_sink.cursor(x, y);
            break;
        case Role::Stick:
            Input::stickMove(x, y);
            break;
        case Role::Camera:
            if (m_camera >= 0 && m_pinch >= 0) {
                const Finger& a = m_fingers[static_cast<size_t>(m_camera)];
                const Finger& b = m_fingers[static_cast<size_t>(m_pinch)];
                const float dist = std::hypot(b.x - a.x, b.y - a.y);
                // Rolagem pelo LOG da razao da pinca (o motor faz zoom(1 +
                // rolagem * 0,1) com a soma do quadro). Os dois dedos andando
                // juntos chegam como dois eventos no mesmo quadro: com o log
                // eles se cancelam exatamente; com a razao linear sobrava
                // zoom num arrasto paralelo (250 -> 166 em 200 px).
                if (m_pinchDist > 1.0f && dist > 1.0f && m_sink.scroll)
                    m_sink.scroll(static_cast<double>(std::log(dist / m_pinchDist) / 0.1f));
                m_pinchDist = dist;
            } else if (slot == m_camera) {
                if (std::hypot(x - f.sx, y - f.sy) > 12.0f) f.moved = true;
                Input::addTouchOrbit(dx, dy);
            }
            break;
        default:
            break;
    }
    publish();
}

void TouchRouter::up(int id, float x, float y, int64_t tNs, bool cancelled) {
    const int slot = find(id);
    if (slot < 0) return;
    Finger& f = m_fingers[static_cast<size_t>(slot)];
    if (f.role == Role::Interface) {
        if (m_sink.uiCursor) m_sink.uiCursor(x, y);
        if (m_sink.uiButton) m_sink.uiButton(GLFW_RELEASE);
    } else if (f.role == Role::Button) {
        if (m_sink.cursor) m_sink.cursor(x, y);
        button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
        if (!cancelled && !f.moved && tNs - f.t0 < kShortTapNs) click(x, y);
    } else if (f.role == Role::Stick) {
        Input::stickTouch(false, x, y);
    } else if (slot == m_pinch || (slot == m_camera && m_pinch >= 0)) {
        // Fim da pinca: o dedo que fica nao volta a girar ate' sair e pousar de novo.
        const int other = slot == m_pinch ? m_camera : m_pinch;
        if (other >= 0) m_fingers[static_cast<size_t>(other)].role = Role::Ignored;
        m_camera = m_pinch = -1;
    } else if (slot == m_camera) {
        // Com um dedo no botao, o cursor e' dele: o toque curto no mundo nao
        // o puxa (ui.dragged leria o salto como arrasto).
        if (!cancelled && !f.moved && tNs - f.t0 < kShortTapNs && !hasRole(Role::Button)) {
            click(x, y);
            // Duplo toque: dois toques curtos, o segundo pousando menos de
            // 300 ms depois de o primeiro sair (a conta do GestureDetector do
            // Android), a menos de 5% da altura da tela um do outro.
            const int h = m_sink.windowHeight ? m_sink.windowHeight() : 0;
            const float near = 0.05f * static_cast<float>(std::max(1, h));
            if (f.t0 - m_lastTap < kShortTapNs && std::hypot(x - m_lastTapX, y - m_lastTapY) < near) {
                Input::signalDoubleTap();
                m_lastTapX = m_lastTapY = -1e9f;
            } else {
                m_lastTap = tNs;
                m_lastTapX = x;
                m_lastTapY = y;
            }
        }
        m_camera = -1;
    }
    f = Finger{};
    publish();
}

} // namespace eruption
