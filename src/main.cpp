#include <cstdio>
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "formats/SpriteSheet.hpp"
#include "game/DefaultApplication.hpp"
#include "utils/BcAlphaSelfTest.hpp"
#include "utils/GpuAutoSelect.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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

    // --new-project <pasta> [--project-name <nome>]: cria a pasta do jogo
    // (manifesto + subpastas de conteudo) e sai. Nao abre janela nenhuma.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--new-project" || i + 1 >= argc) continue;
        std::string name;
        for (int k = 1; k + 1 < argc; ++k)
            if (std::string(argv[k]) == "--project-name") name = argv[k + 1];
        std::string err;
        if (!eruption::Project::instance().create(argv[i + 1], name, &err)) {
            ERUPTION_LOG_FATAL("--new-project: %s", err.c_str());
            return 1;
        }
        return 0;
    }

    // --import-sprite <folha.sheet.json> [...]: corta a folha e grava o .spr
    // ao lado do PNG, sem abrir janela. E' o importador do editor em linha de
    // comando, pra folha gerada por programa. Ver formats/SpriteSheet.hpp.
    {
        std::vector<std::string> recipes;
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--import-sprite") recipes.push_back(argv[i + 1]);
        if (!recipes.empty()) {
            // O projeto resolve o caminho do PNG citado na receita.
            const std::filesystem::path dir = eruption::Project::requested(argc, argv);
            std::string err;
            if (!dir.empty() && !eruption::Project::instance().open(dir, &err))
                ERUPTION_LOG_WARN("[SPRITE] %s", err.c_str());
            int failed = 0;
            for (const std::string& r : recipes) {
                std::string sprErr;
                if (!eruption::importSpriteSheetRecipe(r, sprErr)) {
                    ERUPTION_LOG_ERROR("--import-sprite: %s", sprErr.c_str());
                    ++failed;
                }
            }
            return failed == 0 ? 0 : 1;
        }
    }

    // PROJETO DE JOGO. Antes de qualquer carga de asset: com um projeto
    // aberto, o conteudo do jogo vem da pasta dele e nao mais do
    // repositorio. Sem pedido nenhum o motor segue abrindo mapa solto.
    {
        bool explicitProject = false;
        const std::filesystem::path dir = eruption::Project::requested(argc, argv, &explicitProject);
        if (!dir.empty()) {
            std::string err;
            if (!eruption::Project::instance().open(dir, &err)) {
                if (explicitProject) {
                    ERUPTION_LOG_FATAL("--project: %s", err.c_str());
                    return 1;
                }
                ERUPTION_LOG_WARN("[PROJECT] %s; abrindo sem projeto", err.c_str());
            }
        }
    }

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
    std::string title = engine.editorMode() ? "Eruption Editor" : "Eruption Engine";
    if (eruption::Project::instance().isOpen()) {
        const std::string& game = eruption::Project::instance().manifest().name;
        title = engine.editorMode() ? game + " - Eruption Editor" : game;
    }
    if (!engine.init(winW, winH, title.c_str())) {
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
