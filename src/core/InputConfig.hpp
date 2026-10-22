#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <fstream>
#include <filesystem>

namespace eruption {

struct GamepadConfig {
    int movementAxisX = 2; // Right Stick X
    int movementAxisY = 3; // Right Stick Y
    int cameraAxisX = 0;   // Left Stick X
    int cameraAxisY = 1;   // Left Stick Y
    int cameraResetButton = 11; // L3
    
    bool invertCameraX = false;
    bool invertCameraY = false;
    float deadzone = 0.2f;
    float cameraSensitivity = 2.0f;
    float movementSensitivity = 1.0f;
    int selectedGamepadIndex = 0;
};

struct KeyboardMouseConfig {
    float wasdSpeed = 120.0f;
    float sprintMultiplier = 3.0f;
    float mouseSensitivity = 0.005f;
    float scrollSensitivity = 0.1f;
    bool invertMouseY = false;
    // Celular: graus de giro da camera por um arrasto da altura inteira da
    // tela, igual em qualquer densidade. Em pixel cru (mouseSensitivity) um
    // arrasto de 17% da largura de um aparelho 2400x1080 dava 115 graus; a
    // faixa usual dos jogos de toque e' 180-360 graus por largura.
    float touchTurnPerScreen = 120.0f;
};

class InputConfig {
public:
    GamepadConfig gamepad;
    KeyboardMouseConfig kbMouse;

    void load(const std::string& path = "data/input_config.json") {
        if (!std::filesystem::exists(path)) {
            save(path); // Save defaults
            return;
        }
        try {
            std::ifstream file(path);
            nlohmann::json j;
            file >> j;
            
            // Gamepad
            if (j.contains("gamepad")) {
                const auto& g = j["gamepad"];
                gamepad.movementAxisX = g.value("movementAxisX", gamepad.movementAxisX);
                gamepad.movementAxisY = g.value("movementAxisY", gamepad.movementAxisY);
                gamepad.cameraAxisX = g.value("cameraAxisX", gamepad.cameraAxisX);
                gamepad.cameraAxisY = g.value("cameraAxisY", gamepad.cameraAxisY);
                gamepad.invertCameraX = g.value("invertCameraX", gamepad.invertCameraX);
                gamepad.invertCameraY = g.value("invertCameraY", gamepad.invertCameraY);
                gamepad.deadzone = g.value("deadzone", gamepad.deadzone);
                gamepad.cameraSensitivity = g.value("cameraSensitivity", gamepad.cameraSensitivity);
                gamepad.movementSensitivity = g.value("movementSensitivity", gamepad.movementSensitivity);
                gamepad.selectedGamepadIndex = g.value("selectedGamepadIndex", gamepad.selectedGamepadIndex);
                gamepad.cameraResetButton = g.value("cameraResetButton", gamepad.cameraResetButton);
            }

            // Keyboard/Mouse
            if (j.contains("kbMouse")) {
                const auto& k = j["kbMouse"];
                kbMouse.wasdSpeed = k.value("wasdSpeed", kbMouse.wasdSpeed);
                kbMouse.sprintMultiplier = k.value("sprintMultiplier", kbMouse.sprintMultiplier);
                kbMouse.mouseSensitivity = k.value("mouseSensitivity", kbMouse.mouseSensitivity);
                kbMouse.scrollSensitivity = k.value("scrollSensitivity", kbMouse.scrollSensitivity);
                kbMouse.invertMouseY = k.value("invertMouseY", kbMouse.invertMouseY);
                kbMouse.touchTurnPerScreen = k.value("touchTurnPerScreen", kbMouse.touchTurnPerScreen);
            }
        } catch (...) {}
    }

    void save(const std::string& path = "data/input_config.json") {
        try {
            nlohmann::json j;
            
            // Gamepad
            j["gamepad"]["movementAxisX"] = gamepad.movementAxisX;
            j["gamepad"]["movementAxisY"] = gamepad.movementAxisY;
            j["gamepad"]["cameraAxisX"] = gamepad.cameraAxisX;
            j["gamepad"]["cameraAxisY"] = gamepad.cameraAxisY;
            j["gamepad"]["invertCameraX"] = gamepad.invertCameraX;
            j["gamepad"]["invertCameraY"] = gamepad.invertCameraY;
            j["gamepad"]["deadzone"] = gamepad.deadzone;
            j["gamepad"]["cameraSensitivity"] = gamepad.cameraSensitivity;
            j["gamepad"]["movementSensitivity"] = gamepad.movementSensitivity;
            j["gamepad"]["selectedGamepadIndex"] = gamepad.selectedGamepadIndex;
            j["gamepad"]["cameraResetButton"] = gamepad.cameraResetButton;

            // Keyboard/Mouse
            j["kbMouse"]["wasdSpeed"] = kbMouse.wasdSpeed;
            j["kbMouse"]["sprintMultiplier"] = kbMouse.sprintMultiplier;
            j["kbMouse"]["mouseSensitivity"] = kbMouse.mouseSensitivity;
            j["kbMouse"]["scrollSensitivity"] = kbMouse.scrollSensitivity;
            j["kbMouse"]["invertMouseY"] = kbMouse.invertMouseY;
            j["kbMouse"]["touchTurnPerScreen"] = kbMouse.touchTurnPerScreen;

            std::ofstream file(path);
            file << j.dump(4);
        } catch (...) {}
    }
};

}
