#include <cstdio>
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "formats/FlatMap.hpp"
#if ERUPTION_EDITOR
#include "../plugins/mapgen/MapGen.hpp"
#endif
#include "formats/SpriteSheet.hpp"
#include "game/DefaultApplication.hpp"
#include "utils/BcAlphaSelfTest.hpp"
#include "utils/GpuAutoSelect.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#ifdef ERUPTION_MODULE_COMPAT
#include "motor/AppComCompat.hpp"
#endif

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

// No Android quem chama e' a NativeActivity (src/platform/android/AndroidMain.cpp).
int eruptionMain(int argc, char** argv) {
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

#if ERUPTION_EDITOR
    // --gen-map <receita.mapgen.luau> --map-name <nome>: roda o gerador
    // procedural (o mesmo do editor) e grava o mapa no projeto, sem janela.
    // E' assim que um mapa de relevo nasce por programa.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--gen-map") continue;
        const std::filesystem::path dir = eruption::Project::requested(argc, argv);
        std::string err;
        if (!dir.empty() && !eruption::Project::instance().open(dir, &err)) {
            ERUPTION_LOG_FATAL("--gen-map: %s", err.c_str());
            return 1;
        }
        std::string nome;
        for (int k = 1; k + 1 < argc; ++k)
            if (std::string(argv[k]) == "--map-name") nome = argv[k + 1];
        const std::filesystem::path receita = eruption::project::gameRef(argv[i + 1]);
        if (nome.empty()) nome = receita.stem().stem().string();

        mapgen::Recipe r;
        if (!r.load(receita.string(), err)) {
            ERUPTION_LOG_FATAL("--gen-map: %s", err.c_str());
            return 1;
        }
        mapgen::Result res;
        const auto t0 = std::chrono::steady_clock::now();
        int ultimo = -1;
        auto andamento = [&](float t, const char* etapa) {
            const int pct = int(t * 100.0f) / 10 * 10;
            if (pct != ultimo) {
                ultimo = pct;
                ERUPTION_LOG_WARN("[MAPGEN] %3d%% %s", pct, etapa ? etapa : "");
            }
            return true;
        };
        if (!mapgen::run(r, 0, res, err, andamento)) {
            ERUPTION_LOG_FATAL("--gen-map: %s", err.c_str());
            return 1;
        }
        const std::string raiz = eruption::project::isOpen() ? eruption::project::root().string()
                                                             : eruption::project::engineRoot().string();
        if (!mapgen::writeMap(res, raiz, nome, err)) {
            ERUPTION_LOG_FATAL("--gen-map: %s", err.c_str());
            return 1;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ERUPTION_LOG_WARN("[MAPGEN] %s: grade %dx%d, %zu camada(s), %zu grupo(s) de objeto em %.0f ms", nome.c_str(),
                          res.w, res.d, res.layers.size(), res.scatter.size(), ms);
        return 0;
    }
#endif

    // --new-map <nome> [--map-size <lado>] [--map-color r,g,b]: cria o mapa
    // vazio (chao plano,
    // grade de altura e ambiente) dentro do projeto e sai. Mesmo codigo do
    // File > New Map do editor. Ver formats/FlatMap.hpp.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--new-map") continue;
        const std::filesystem::path dir = eruption::Project::requested(argc, argv);
        std::string err;
        if (!dir.empty() && !eruption::Project::instance().open(dir, &err)) {
            ERUPTION_LOG_FATAL("--new-map: %s", err.c_str());
            return 1;
        }
        eruption::FlatMap::Options o;
        // A flag sem valor tem que ser lida ate o ultimo argumento: parando em
        // argc - 1 ela e' ignorada quando vem no fim da linha de comando.
        for (int k = 1; k < argc; ++k) {
            const std::string flag = argv[k];
            if (flag == "--map-size" && k + 1 < argc) o.size = static_cast<float>(std::atof(argv[k + 1]));
            // --map-color r,g,b (0..1): a mesma cor de chao do File > New Map.
            if (flag == "--map-color" && k + 1 < argc) {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                if (std::sscanf(argv[k + 1], "%f,%f,%f", &r, &g, &b) == 3) o.groundColor = eruption::Vec3(r, g, b);
            }
            if (flag == "--map-overwrite") o.overwrite = true;
        }
        const std::string name = argv[i + 1];
        const std::filesystem::path mapDir = eruption::project::game("assets/external") / name;
        if (!eruption::FlatMap::create(mapDir, name, o, err)) {
            ERUPTION_LOG_FATAL("--new-map: %s", err.c_str());
            return 1;
        }
        ERUPTION_LOG_WARN("[MAPA] %s criado em %s (%.0f x %.0f unidades)", name.c_str(),
                          mapDir.string().c_str(), double(o.size), double(o.size));
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
#if !defined(__EMSCRIPTEN__) // no navegador nao ha' driver para escolher
    eruption::GpuAutoSelect::ensureBestGpu();
#endif

#if defined(__EMSCRIPTEN__)
    // O laco do navegador sai de main desenrolando a pilha (emscripten_set_main_loop):
    // motor e aplicacao vivem fora dela, senao os destrutores rodam no meio do jogo.
    static eruption::Engine engine;
#else
    eruption::Engine engine;
#endif
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

#if defined(__EMSCRIPTEN__)
    static eruption::DefaultApplication app(argc, argv);
#else
    eruption::DefaultApplication app(argc, argv);
#endif
    ERUPTION_LOG_INFO("[MAIN] Running app...");
#ifdef ERUPTION_MODULE_COMPAT
    // Plugins de outros motores (compat/): so' carrega algo com ERUPTION_COMPAT_PLUGINS.
    eruption::compat::AppComCompat<eruption::DefaultApplication> appCompat(app);
    engine.run(appCompat);
#else
    engine.run(app);
#endif
    ERUPTION_LOG_INFO("[MAIN] App finished, exiting");
    return 0;
}

#ifndef __ANDROID__
int main(int argc, char** argv) { return eruptionMain(argc, argv); }
#endif
