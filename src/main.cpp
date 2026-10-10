#include <cstdio>
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "game/DefaultApplication.hpp"
#include "utils/BcAlphaSelfTest.hpp"
#include "utils/GpuAutoSelect.hpp"
#include <cstdlib>
#include <string>

#ifdef _WIN32
#define ERUPTION_ENVIRON _environ
#else
extern char** environ;
#define ERUPTION_ENVIRON environ
#endif

namespace eruption {
// O editor abre por padrao. O jogo sozinho vem com --game, e as execucoes
// automaticas (benchmarks, capturas, testes com ERUPTION_TEST_*) tambem
// rodam como jogo para os roteiros existentes continuarem iguais.
// --editor forca o editor mesmo nesses casos.
static bool wantsEditor(int argc, char** argv) {
#if !ERUPTION_EDITOR
    (void)argc;
    (void)argv;
    return false; // jogo exportado: sem editor
#endif
    bool automated = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--editor") return true;
        if (a == "--game") return false;
        if (a == "--auto-exit" || a == "--auto-test" || a == "--screenshot" || a == "--dump-frames" ||
            a == "--telemetry" || a.rfind("--benchmark", 0) == 0)
            automated = true;
    }
    for (char** e = ERUPTION_ENVIRON; e && *e; ++e)
        if (std::string(*e).rfind("ERUPTION_TEST_", 0) == 0) automated = true;
    return !automated;
}
} // namespace eruption

int main(int argc, char** argv) {
    eruption::Logger::init();

    // ERUPTION_TEST_BC7_ALPHA=1: teste isolado do compressor de altura, sai
    // antes de tocar GPU/janela/mapa nenhum. Ver BcAlphaSelfTest.hpp.
    if (eruption::runBcAlphaSelfTestIfRequested()) return 0;

    // Detect the best Vulkan driver/GPU before any window/Vulkan initialization.
    // This may restart the process with VK_ICD_FILENAMES set, then return true.
    eruption::GpuAutoSelect::ensureBestGpu();

    eruption::Engine engine;
    engine.setEditorMode(eruption::wantsEditor(argc, argv));
    ERUPTION_LOG_INFO("[MAIN] Engine init starting...");
    // ERUPTION_WINDOW_SIZE=<L>x<A>: tamanho da janela (capturas em alta resolução).
    int winW = 1280, winH = 720;
    if (const char* ws = std::getenv("ERUPTION_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(ws, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) {
            winW = w;
            winH = h;
        }
    }
    if (!engine.init(winW, winH, engine.editorMode() ? "Eruption Editor" : "Eruption Engine")) {
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
