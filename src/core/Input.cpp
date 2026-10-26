#include "core/Input.hpp"
#include <algorithm>
#include <chrono>
#include <vector>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <cstring>

namespace eruption {

void swapCssTouchRects();

GLFWwindow* Input::s_window = nullptr;
std::array<bool, 512> Input::s_keys{};
std::array<bool, 512> Input::s_keysPrev{};
std::array<bool, 512> Input::s_keysPhys{};
std::array<bool, 512> Input::s_keysVirt{};
glm::vec2 Input::s_virtStick{0.0f};
glm::vec4 Input::s_stickZone{0.0f};
Input::TouchStick Input::s_touchStick;
bool Input::s_doubleTapPending = false;
#ifdef __ANDROID__
bool Input::s_touchScreen = true;
#else
bool Input::s_touchScreen = false;
#endif
bool Input::s_doubleTap = false;
bool Input::s_pressOnCss = false;
std::array<bool, 8> Input::s_mouse{};
std::array<bool, 8> Input::s_mousePrev{};
std::array<bool, 8> Input::s_mouseClicked{};
glm::vec2 Input::s_mousePos{0.0f};
glm::vec2 Input::s_mousePosPrev{0.0f};
float Input::s_scroll = 0.0f;
float Input::s_scrollDelta = 0.0f;
std::vector<Input::TouchPoint> Input::s_touches;
bool Input::s_cursorVisible = true;
bool Input::s_cursorLocked = false;
std::array<Input::GamepadState, 4> Input::s_gamepads{};

void Input::init(GLFWwindow* window) {
    s_window = window;
    s_keys.fill(false);
    s_keysPrev.fill(false);
    s_mouse.fill(false);
    s_mousePrev.fill(false);
    s_mousePos = {0, 0};
    s_mousePosPrev = {0, 0};
    s_scroll = 0.0f;
    s_scrollDelta = 0.0f;

    glfwSetKeyCallback(window, keyCallback);
    glfwSetCharCallback(window, charCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetScrollCallback(window, scrollCallback);
}

void Input::shutdown() {
    if (s_window) {
        glfwSetKeyCallback(s_window, nullptr);
        glfwSetCharCallback(s_window, nullptr);
        glfwSetMouseButtonCallback(s_window, nullptr);
        glfwSetCursorPosCallback(s_window, nullptr);
        glfwSetScrollCallback(s_window, nullptr);
    }
    s_window = nullptr;
}

void Input::update() {
    s_keysPrev = s_keys;
    for (size_t k = 0; k < s_keys.size(); ++k) s_keys[k] = s_keysPhys[k] || s_keysVirt[k];
    s_mouseClicked.fill(false);
    s_doubleTap = s_doubleTapPending;
    s_doubleTapPending = false;
    swapCssTouchRects();
    s_mousePrev = s_mouse;
    s_mousePosPrev = s_mousePos;
    s_scrollDelta = 0.0f;

    for (int i = 0; i < 4; ++i) {
        if (glfwJoystickPresent(GLFW_JOYSTICK_1 + i)) {
            s_gamepads[i].present = true;
            std::memcpy(s_gamepads[i].buttonsPrev, s_gamepads[i].buttons, sizeof(s_gamepads[i].buttons));
            
            // O GLFW do Emscripten (API 3.2) nao tem o mapeamento de gamepad:
            // la' o controle sempre vai pelos eixos e botoes crus.
#if !defined(__EMSCRIPTEN__)
            if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + i)) {
                GLFWgamepadstate state;
                if (glfwGetGamepadState(GLFW_JOYSTICK_1 + i, &state)) {
                    for (int b = 0; b < 15; ++b) {
                        s_gamepads[i].buttons[b] = (state.buttons[b] == GLFW_PRESS);
                    }
                    for (int a = 0; a < 6; ++a) {
                        s_gamepads[i].axes[a] = state.axes[a];
                    }
                }
            } else
#endif
            {
                // Fallback to raw joystick axes/buttons for generic controllers like Lukton
                int axesCount, buttonCount;
                const float* rawAxes = glfwGetJoystickAxes(GLFW_JOYSTICK_1 + i, &axesCount);
                const uint8_t* rawButtons = glfwGetJoystickButtons(GLFW_JOYSTICK_1 + i, &buttonCount);
                
                for (int a = 0; a < 6 && a < axesCount; ++a) {
                    s_gamepads[i].axes[a] = rawAxes[a];
                }
                for (int b = 0; b < 15 && b < buttonCount; ++b) {
                    s_gamepads[i].buttons[b] = (rawButtons[b] == GLFW_PRESS);
                }
            }

            const char* name = glfwGetJoystickName(GLFW_JOYSTICK_1 + i);
            if (name) std::strncpy(s_gamepads[i].name, name, sizeof(s_gamepads[i].name) - 1);
        } else {
            s_gamepads[i].present = false;
        }
    }
}

void Input::resetDeltas() {
    s_scrollDelta = 0.0f;
    s_mousePosPrev = s_mousePos;
}

void Input::setVirtualKey(Key key, bool down) {
    const int k = static_cast<int>(key);
    if (k >= 0 && k < 512) s_keysVirt[k] = down;
}

bool Input::isKeyDown(Key key) {
    int k = static_cast<int>(key);
    return k >= 0 && k < 512 && s_keys[k];
}

bool Input::isKeyPressed(Key key) {
    int k = static_cast<int>(key);
    return k >= 0 && k < 512 && s_keys[k] && !s_keysPrev[k];
}

bool Input::isKeyReleased(Key key) {
    int k = static_cast<int>(key);
    return k >= 0 && k < 512 && !s_keys[k] && s_keysPrev[k];
}

bool Input::isMouseDown(MouseButton btn) {
    int b = static_cast<int>(btn);
    return b >= 0 && b < 8 && s_mouse[b];
}

bool Input::isMousePressed(MouseButton btn) {
    int b = static_cast<int>(btn);
    return b >= 0 && b < 8 && ((s_mouse[b] && !s_mousePrev[b]) || s_mouseClicked[b]);
}

bool Input::isMouseReleased(MouseButton btn) {
    int b = static_cast<int>(btn);
    return b >= 0 && b < 8 && !s_mouse[b] && s_mousePrev[b];
}

glm::vec2 Input::mousePosition() { return s_mousePos; }
glm::vec2 Input::mouseDelta() { return s_mousePos - s_mousePosPrev; }
float Input::mouseScroll() { return s_scroll; }
float Input::mouseScrollDelta() { return s_scrollDelta; }

bool Input::isGamepadPresent(int jid) {
    if (jid < 0 || jid >= 4) return false;
    return s_gamepads[jid].present;
}

bool Input::isGamepadButtonDown(int button, int jid) {
    if (jid < 0 || jid >= 4 || button < 0 || button >= 15) return false;
    return s_gamepads[jid].buttons[button];
}

bool Input::isGamepadButtonPressed(int button, int jid) {
    if (jid < 0 || jid >= 4 || button < 0 || button >= 15) return false;
    return s_gamepads[jid].buttons[button] && !s_gamepads[jid].buttonsPrev[button];
}

float Input::getGamepadAxis(int axis, int jid) {
    if (jid < 0 || jid >= 4 || axis < 0 || axis >= 6) return 0.0f;
    return s_gamepads[jid].axes[axis];
}

const char* Input::getGamepadName(int jid) {
    if (jid < 0 || jid >= 4) return nullptr;
    return s_gamepads[jid].name;
}

bool Input::isGamepad(int jid) {
    if (jid < 0 || jid >= 16) return false;
    return glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + jid);
}

int Input::getJoystickAxisCount(int jid) {
    int count;
    glfwGetJoystickAxes(GLFW_JOYSTICK_1 + jid, &count);
    return count;
}

int Input::getJoystickButtonCount(int jid) {
    int count;
    glfwGetJoystickButtons(GLFW_JOYSTICK_1 + jid, &count);
    return count;
}

float Input::getJoystickAxis(int axis, int jid) {
    int count;
    const float* axes = glfwGetJoystickAxes(GLFW_JOYSTICK_1 + jid, &count);
    if (axis >= 0 && axis < count) return axes[axis];
    return 0.0f;
}

bool Input::isJoystickButtonDown(int button, int jid) {
    int count;
    const uint8_t* buttons = glfwGetJoystickButtons(GLFW_JOYSTICK_1 + jid, &count);
    if (button >= 0 && button < count) return buttons[button] == GLFW_PRESS;
    return false;
}

void Input::setCursorVisible(bool visible) {
    s_cursorVisible = visible;
    if (s_window) glfwSetInputMode(s_window, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

void Input::setCursorLocked(bool locked) {
    s_cursorLocked = locked;
    if (s_window) glfwSetInputMode(s_window, GLFW_CURSOR, locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Input::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    if (key >= 0 && key < 512) {
        if (action == GLFW_PRESS) s_keysPhys[key] = true;
        else if (action == GLFW_RELEASE) s_keysPhys[key] = false;
        s_keys[key] = s_keysPhys[key] || s_keysVirt[key];
    }
}

namespace {
std::vector<glm::vec4> s_uiTouchRects; // x0, y0, x1, y1 em pixels da janela
std::vector<glm::vec4> s_cssTouchRects[2]; // [0] deste quadro, [1] do anterior
bool inRects(const std::vector<glm::vec4>& rects, float x, float y) {
    for (const glm::vec4& r : rects)
        if (x >= r.x && y >= r.y && x <= r.z && y <= r.w) return true;
    return false;
}
}
void swapCssTouchRects() {
    s_cssTouchRects[1].swap(s_cssTouchRects[0]);
    s_cssTouchRects[0].clear();
}
void Input::addCssTouchRect(float x0, float y0, float x1, float y1) {
    s_cssTouchRects[0].emplace_back(x0, y0, x1, y1);
}
bool Input::inCssTouchRect(float x, float y) {
    return inRects(s_cssTouchRects[0], x, y) || inRects(s_cssTouchRects[1], x, y);
}
bool Input::inStickZone(float x, float y) {
    const glm::vec4& z = s_stickZone;
    return z.z > z.x && x >= z.x && y >= z.y && x <= z.z && y <= z.w;
}
void Input::stickTouch(bool down, float x, float y) {
    s_touchStick.active = down;
    if (down) s_touchStick.base = s_touchStick.pos = glm::vec2(x, y);
}

void Input::clearUiTouchRects() { s_uiTouchRects.clear(); }

void Input::addUiTouchRect(float x0, float y0, float x1, float y1) { s_uiTouchRects.emplace_back(x0, y0, x1, y1); }

bool Input::inUiTouchRect(float x, float y) { return inRects(s_uiTouchRects, x, y); }

void Input::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
    // Duplo clique direito = o duplo toque do celular (com toque o botao
    // direito e' o dedo pousando; o core/TouchRouter detecta o duplo toque).
    if (!s_touchScreen && button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        static double s_last = -1.0;
        static glm::vec2 s_lastPos{0.0f};
        int w = 0, h = 0;
        glfwGetWindowSize(window, &w, &h);
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (s_last >= 0.0 && now - s_last < 0.3 && glm::length(s_mousePos - s_lastPos) < 0.05f * float(h)) {
            s_doubleTapPending = true;
            s_last = -1.0;
        } else {
            s_last = now;
            s_lastPos = s_mousePos;
        }
    }
    if (button >= 0 && button < 8) {
        if (action == GLFW_PRESS) {
            if (std::none_of(s_mouse.begin(), s_mouse.end(), [](bool b) { return b; }))
                s_pressOnCss = inCssTouchRect(s_mousePos.x, s_mousePos.y);
            s_mouse[button] = s_mouseClicked[button] = true;
            // O salto do cursor ate' onde o botao apertou nao e' arrasto: no
            // celular o dedo pousa longe de onde o anterior saiu, e o salto
            // girava a camera (um toque parado virava um tranco de 50 graus).
            s_mousePosPrev = s_mousePos;
        }
        else if (action == GLFW_RELEASE) {
            s_mouse[button] = false;
            if (std::none_of(s_mouse.begin(), s_mouse.end(), [](bool b) { return b; })) s_pressOnCss = false;
        }
    }
}

void Input::cursorPosCallback(GLFWwindow* window, double x, double y) {
    ImGui_ImplGlfw_CursorPosCallback(window, x, y);
    s_mousePos = glm::vec2(static_cast<float>(x), static_cast<float>(y));
}

void Input::scrollCallback(GLFWwindow* window, double xoff, double yoff) {
    ImGui_ImplGlfw_ScrollCallback(window, xoff, yoff);
    s_scrollDelta = static_cast<float>(yoff);
    s_scroll += s_scrollDelta;
}

void Input::charCallback(GLFWwindow* window, unsigned int c) {
    ImGui_ImplGlfw_CharCallback(window, c);
}

} // namespace eruption
