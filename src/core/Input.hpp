#pragma once

#include <cstdint>
#include <array>
#include <glm/glm.hpp>

struct GLFWwindow;

namespace eruption {

enum class Key : int32_t {
    Unknown = -1, Space = 32, Apostrophe = 39, Comma = 44, Minus = 45,
    Period = 46, Slash = 47,
    D0 = 48, D1 = 49, D2 = 50, D3 = 51, D4 = 52, D5 = 53, D6 = 54, D7 = 55, D8 = 56, D9 = 57,
    Semicolon = 59, Equal = 61,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72, I = 73,
    J = 74, K = 75, L = 76, M = 77, N = 78, O = 79, P = 80, Q = 81, R = 82,
    S = 83, T = 84, U = 85, V = 86, W = 87, X = 88, Y = 89, Z = 90,
    LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259,
    Insert = 260, Delete = 261, Right = 262, Left = 263, Down = 264, Up = 265,
    PageUp = 266, PageDown = 267, Home = 268, End = 269,
    CapsLock = 280, ScrollLock = 281, NumLock = 282,
    PrintScreen = 283, Pause = 284,
    F1 = 290, F2 = 291, F3 = 292, F4 = 293, F5 = 294, F6 = 295,
    F7 = 296, F8 = 297, F9 = 298, F10 = 299, F11 = 300, F12 = 301,
    LeftShift = 340, LeftControl = 341, LeftAlt = 342, LeftSuper = 343,
    RightShift = 344, RightControl = 345, RightAlt = 346, RightSuper = 347,
    Menu = 348,
};

enum class MouseButton : uint8_t {
    Left = 0, Right = 1, Middle = 2,
};

class Input {
public:
    static void init(GLFWwindow* window);
    // Areas da tela que sao de interface (painel de toque): o toque que
    // comeca dentro delas vai so' para o ImGui, nao gira a camera. Quem
    // desenha a interface republica as areas a cada quadro.
    static void clearUiTouchRects();
    static void addUiTouchRect(float x0, float y0, float x1, float y1);
    static bool inUiTouchRect(float x, float y);
    // Elementos CSS que o script consulta (ui.clicked, ui.held...): o dedo que
    // pousa num deles e' da interface, nunca do analogico. Valem o quadro em
    // que foram publicados e o seguinte (o toque chega antes do script rodar).
    static void addCssTouchRect(float x0, float y0, float x1, float y1);
    static bool inCssTouchRect(float x, float y);
    // ANALOGICO FLUTUANTE (celular): a interface publica a cada quadro a zona
    // em que um dedo vira analogico (largura 0 = sem analogico); o dedo que
    // pousa nela leva a base consigo e o pino segue o dedo. Os outros dedos
    // seguem livres (camera, interface).
    struct TouchStick { bool active = false; glm::vec2 base{0.0f}, pos{0.0f}; };
    static void setStickZone(const glm::vec4& zone) { s_stickZone = zone; }
    static bool inStickZone(float x, float y);
    static void stickTouch(bool down, float x, float y);
    static void stickMove(float x, float y) { s_touchStick.pos = glm::vec2(x, y); }
    static TouchStick& touchStick() { return s_touchStick; }
    // Duplo toque na area da camera (celular) ou duplo clique direito (PC):
    // verdadeiro no quadro seguinte ao gesto, por um quadro. O motor nao faz
    // nada com ele; o jogo decide (reenquadrar a camera, por exemplo).
    static void signalDoubleTap() { s_doubleTapPending = true; }
    static bool isDoubleTapped() { return s_doubleTap; }
    static void shutdown();
    static void update();
    // CONTROLE DE TOQUE (Engine::drawTouchPad): tecla virtual segurada pela
    // interface, somada a' do teclado no proximo update() - o script ve' o
    // "pressed" no quadro seguinte, como uma tecla de verdade. O analogico
    // virtual (-1..1, y para baixo) vai direto ao movimento do jogador.
    static void setVirtualKey(Key key, bool down);
    static void setVirtualStick(const glm::vec2& v) { s_virtStick = v; }
    static glm::vec2 virtualStick() { return s_virtStick; }
    // Clique sintetico (teste): cursor em (x, y) da janela e um aperto neste quadro.
    static void injectClick(float x, float y) {
        s_mousePos = glm::vec2(x, y);
        s_mouseClicked[0] = true;
    }

    static bool isKeyDown(Key key);
    static bool isKeyPressed(Key key);
    static bool isKeyReleased(Key key);

    static bool isMouseDown(MouseButton btn);
    static bool isMousePressed(MouseButton btn);
    static bool isMouseReleased(MouseButton btn);

    static glm::vec2 mousePosition();
    static glm::vec2 mouseDelta();
    static float mouseScroll();
    static float mouseScrollDelta();

    // Gamepad support
    static bool isGamepadPresent(int jid = 0);
    static bool isGamepadButtonDown(int button, int jid = 0);
    static bool isGamepadButtonPressed(int button, int jid = 0);
    static float getGamepadAxis(int axis, int jid = 0);
    static const char* getGamepadName(int jid = 0);
    static bool isGamepad(int jid = 0);

    // Raw joystick support (for non-standard controllers)
    static int getJoystickAxisCount(int jid = 0);
    static int getJoystickButtonCount(int jid = 0);
    static float getJoystickAxis(int axis, int jid = 0);
    static bool isJoystickButtonDown(int button, int jid = 0);

    static void setCursorVisible(bool visible);
    static void setCursorLocked(bool locked);

    static void resetDeltas();

private:
    static GLFWwindow* s_window;
    static std::array<bool, 512> s_keys;
    static std::array<bool, 512> s_keysPrev;
    static std::array<bool, 512> s_keysPhys; // so' o teclado
    static std::array<bool, 512> s_keysVirt; // so' o controle de toque
    static glm::vec2 s_virtStick;
    static glm::vec4 s_stickZone;
    static TouchStick s_touchStick;
    static bool s_doubleTapPending, s_doubleTap;
    static std::array<bool, 8> s_mouse;
    static std::array<bool, 8> s_mousePrev;
    // Apertou neste quadro, mesmo que ja' tenha soltado (toque curto do
    // Android e clique rapido chegam com o aperto e a soltura juntos).
    static std::array<bool, 8> s_mouseClicked;
    static glm::vec2 s_mousePos;
    static glm::vec2 s_mousePosPrev;
    static float s_scroll;
    static float s_scrollDelta;
    static bool s_cursorVisible;
    static bool s_cursorLocked;

    struct GamepadState {
        bool present = false;
        bool buttons[15]{};
        bool buttonsPrev[15]{};
        float axes[6]{};
        char name[128]{};
    };
    static std::array<GamepadState, 4> s_gamepads;

    static void keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void charCallback(GLFWwindow* w, unsigned int c);
    static void mouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* w, double x, double y);
    static void scrollCallback(GLFWwindow* w, double xoff, double yoff);
};

} // namespace eruption
