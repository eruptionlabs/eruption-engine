#include "core/Input.hpp"
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <cstring>

namespace eruption {

GLFWwindow* Input::s_window = nullptr;
std::array<bool, 512> Input::s_keys{};
std::array<bool, 512> Input::s_keysPrev{};
std::array<bool, 8> Input::s_mouse{};
std::array<bool, 8> Input::s_mousePrev{};
glm::vec2 Input::s_mousePos{0.0f};
glm::vec2 Input::s_mousePosPrev{0.0f};
float Input::s_scroll = 0.0f;
float Input::s_scrollDelta = 0.0f;
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
    s_mousePrev = s_mouse;
    s_mousePosPrev = s_mousePos;
    s_scrollDelta = 0.0f;

    for (int i = 0; i < 4; ++i) {
        if (glfwJoystickPresent(GLFW_JOYSTICK_1 + i)) {
            s_gamepads[i].present = true;
            std::memcpy(s_gamepads[i].buttonsPrev, s_gamepads[i].buttons, sizeof(s_gamepads[i].buttons));
            
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
            } else {
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
    return b >= 0 && b < 8 && s_mouse[b] && !s_mousePrev[b];
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
        if (action == GLFW_PRESS) s_keys[key] = true;
        else if (action == GLFW_RELEASE) s_keys[key] = false;
    }
}

void Input::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
    if (button >= 0 && button < 8) {
        if (action == GLFW_PRESS) s_mouse[button] = true;
        else if (action == GLFW_RELEASE) s_mouse[button] = false;
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
