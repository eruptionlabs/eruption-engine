#pragma once

#include "renderer/MipMode.hpp"
#include <string>

namespace eruption {

class Engine;

class MipmapDebugMenu {
public:
    MipConfig config;

    void loadConfig();
    void saveConfig();
    void drawUI(Engine* engine);

    bool modeChanged = false; // Sinaliza para a engine que o modo mudou

private:
    static constexpr const char* CONFIG_PATH = "data/mipmap_config.json";
};

} // namespace eruption
