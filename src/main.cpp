#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "game/DefaultApplication.hpp"
#include "utils/GpuAutoSelect.hpp"
#include <string>

int main(int argc, char** argv) {
    eruption::Logger::init();

    // Detect the best Vulkan driver/GPU before any window/Vulkan initialization.
    // This may restart the process with VK_ICD_FILENAMES set, then return true.
    eruption::GpuAutoSelect::ensureBestGpu();

    eruption::Engine engine;
    ERUPTION_LOG_INFO("[MAIN] Engine init starting...");
    if (!engine.init(1280, 720, "Eruption Engine")) {
        ERUPTION_LOG_FATAL("Engine initialization failed");
        return 1;
    }
    ERUPTION_LOG_INFO("[MAIN] Engine init succeeded");

    eruption::DefaultApplication app(argc, argv);
    ERUPTION_LOG_INFO("[MAIN] Running app...");
    engine.run(app);
    ERUPTION_LOG_INFO("[MAIN] App finished, exiting");
    return 0;
}
