#include "editor/Editor.hpp"
#include "script/ScriptHost.hpp"
#include "scene/SceneEdits.hpp"
#include "core/Platform.hpp"
#include "game/DefaultApplication.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "game/PlayerController.hpp"
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "core/Input.hpp"
#include "formats/TerrainParser.hpp"
#include <imgui.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>
#include <fstream>

namespace eruption {

static std::string loadInitialMapFromConfig() {
    try {
        std::ifstream f("data/launcher_config.json");
        if (!f) return "parana_field";
        nlohmann::json cfg;
        f >> cfg;
        if (cfg.contains("initial_map") && cfg["initial_map"].is_string()) {
            return cfg["initial_map"].get<std::string>();
        }
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("DefaultApplication: failed to read launcher_config.json: %s", e.what());
    }
    return "parana_field";
}

DefaultApplication::DefaultApplication(int argc, char** argv) {
    // Com um projeto aberto quem manda e' o manifesto: o mapa de abertura
    // dele, ou o primeiro mapa que o projeto tiver. Projeto novo e vazio
    // comeca sem mapa nenhum (o editor abre so' com a interface).
    if (project::isOpen()) {
        const Project& p = Project::instance();
        m_initialMap = p.manifest().startupMap;
        if (m_initialMap.empty()) {
            const std::vector<std::string> maps = p.maps();
            if (!maps.empty()) m_initialMap = maps.front();
        }
        applyFraming(p.manifest());
    } else {
        m_initialMap = loadInitialMapFromConfig();
        // Jogo exportado: o enquadramento vem copiado no launcher_config.
        try {
            std::ifstream f("data/launcher_config.json");
            const nlohmann::json cfg = f ? nlohmann::json::parse(f, nullptr, false) : nlohmann::json();
            if (cfg.is_object()) {
                Project::Manifest framing;
                Project::readFraming(cfg, framing);
                applyFraming(framing);
            }
        } catch (const std::exception& e) {
            ERUPTION_LOG_WARN("DefaultApplication: camera do launcher_config.json invalida: %s", e.what());
        }
    }
    parseArgs(argc, argv);
}
DefaultApplication::~DefaultApplication() = default;

// Enquadramento do jogo (blocos "spawn" e "camera"): chave que o jogo nao
// escreve fica com o padrao do motor.
void DefaultApplication::applyFraming(const Project::Manifest& mf) {
    if (mf.hasSpawn) {
        m_initialPos = Vec3(mf.spawn[0], mf.spawn[1], mf.spawn[2]);
        m_hasInitialPos = true;
    }
    if (mf.cameraFov) m_fovDeg = *mf.cameraFov;
    if (mf.cameraYaw) { m_initialYaw = *mf.cameraYaw; m_hasInitialCamera = true; }
    if (mf.cameraPitch) { m_initialPitch = *mf.cameraPitch; m_hasInitialCamera = true; }
    if (mf.cameraDistance) { m_initialDist = *mf.cameraDistance; m_hasInitialCamera = true; }
    m_keepViewInsideMap = mf.cameraKeepInsideMap;
}
void DefaultApplication::parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--auto-exit") {
            m_autoExit = true;
        } else if (arg == "--auto-test") {
            m_autoTest = true;
        } else if (arg == "--verbose") {
            Logger::setLevel(LogLevel::Debug);
        } else if (arg == "--trace") {
            Logger::setLevel(LogLevel::Trace);
        } else if (arg == "--map" && i + 1 < argc) {
            m_initialMap = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            m_screenshotPath = argv[++i];
            m_autoExit = true;
        } else if (arg == "--telemetry" && i + 1 < argc) {
            // Same switch the perf/ harness uses via env var.
            platform::setEnv("ERUPTION_TELEMETRY_OUT", argv[++i]);
        } else if (arg == "--game-telemetry" && i + 1 < argc) {
            // Eventos do jogo (telemetry.emit), nao o custo dos quadros.
            platform::setEnv("ERUPTION_GAME_TELEMETRY", argv[++i]);
        } else if (arg == "--warp-on-load" && i + 1 < argc) {
            m_warpOnLoad = argv[++i];
        } else if (arg == "--dump-frames" && i + 2 < argc) {
            i += 2;
            m_autoExit = true;
        } else if (arg == "--benchmark-water") {
            m_benchmarkWater = true;
            m_autoExit = true;
        } else if (arg == "--benchmark-thunderstorm") {
            m_benchmarkThunderstorm = true;
            m_autoExit = true;
        } else if (arg == "--benchmark-heavy-rain") {
            m_benchmarkHeavyRain = true;
            m_autoExit = true;
        } else if (arg == "--benchmark-pos" && i + 1 < argc) {
            std::string posStr = argv[++i];
            size_t c1 = posStr.find(',');
            size_t c2 = posStr.find(',', c1 + 1);
            float x = std::stof(posStr.substr(0, c1));
            float y = (c2 != std::string::npos) ? std::stof(posStr.substr(c1 + 1, c2 - c1 - 1)) : 0.0f;
            float z = (c2 != std::string::npos) ? std::stof(posStr.substr(c2 + 1)) : std::stof(posStr.substr(c1 + 1));
            m_benchmarkPos = Vec3(x, y, z);
        } else if (arg == "--benchmark-duration" && i + 1 < argc) {
            m_benchmarkDuration = std::stof(argv[++i]);
        } else if (arg == "--pos" && i + 1 < argc) {
            std::string posStr = argv[++i];
            size_t c1 = posStr.find(',');
            size_t c2 = posStr.find(',', c1 + 1);
            float x = std::stof(posStr.substr(0, c1));
            float y = (c2 != std::string::npos) ? std::stof(posStr.substr(c1 + 1, c2 - c1 - 1)) : 0.0f;
            float z = (c2 != std::string::npos) ? std::stof(posStr.substr(c2 + 1)) : std::stof(posStr.substr(c1 + 1));
            m_initialPos = Vec3(x, y, z);
            m_hasInitialPos = true;
        } else if (arg == "--cam-yaw" && i + 1 < argc) {
            m_initialYaw = std::stof(argv[++i]);
            m_hasInitialCamera = true;
        } else if (arg == "--cam-pitch" && i + 1 < argc) {
            m_initialPitch = std::stof(argv[++i]);
            m_hasInitialCamera = true;
            m_needFreeCamera = true;
        } else if (arg == "--cam-dist" && i + 1 < argc) {
            m_initialDist = std::stof(argv[++i]);
            m_hasInitialCamera = true;
        } else if (arg == "--fov" && i + 1 < argc) {
            m_fovDeg = std::stof(argv[++i]);
        } else if (arg == "--zoom" && i + 1 < argc) {
            float zoomPercent = std::stof(argv[++i]) / 100.0f;
            m_initialDist = 10.0f + (1.0f - glm::clamp(zoomPercent, 0.0f, 1.0f)) * 990.0f;
            m_hasInitialCamera = true;
        } else if (arg == "--spawn-cloud" && i + 1 < argc) {
            m_spawnCloudAlts.push_back(std::stof(argv[++i]));
        } else if (arg == "--spawn-clouds" && i + 1 < argc) {
            std::string csv = argv[++i];
            size_t start = 0;
            while (start < csv.size()) {
                size_t comma = csv.find(',', start);
                std::string tok = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!tok.empty()) m_spawnCloudAlts.push_back(std::stof(tok));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (arg == "--cloud-test-colors") {
            m_cloudTestColors = true;
        } else if (arg == "--spawn-rain") {
            // Test hook: CLI-spawned clouds carry their own rain (+ ground
            // splashes), like the UI "rain" toggle on the spawn button.
            m_spawnCloudRain = true;
        } else if (arg == "--weather" && i + 1 < argc) {
            // Test hook: force the initial weather type by name (e.g.
            // "stormy", "rainy", "cloudy").
            m_weatherTypeName = argv[++i];
        } else if (arg == "--debug-rain-occlusion") {
            m_debugRainOcclusion = true;
        } else if (arg == "--auto-cloud") {
            m_autoCloud = true;
        } else if (arg == "--no-auto-cloud") {
            m_autoCloud = false;
        } else if (arg == "--cloud-bottom" && i + 1 < argc) {
            m_cloudBoxBottom = std::stof(argv[++i]);
            m_cloudBoxSet = true;
        } else if (arg == "--cloud-thickness" && i + 1 < argc) {
            m_cloudBoxThickness = std::stof(argv[++i]);
            m_cloudBoxSet = true;
        } else if (arg == "--cloud-wind" && i + 1 < argc) {
            // Test hook: freeze/force the wind speed for deterministic runs.
            m_cloudWindSpeed = std::stof(argv[++i]);
        } else if (arg == "--cloud-radius" && i + 1 < argc) {
            // Test hook: local cloud radius X/Z (mimics the UI sliders).
            m_cloudRadius = std::stof(argv[++i]);
        } else if (arg == "--cloud-falloff" && i + 1 < argc) {
            m_cloudFalloff = std::stof(argv[++i]);
        }
    }
}
void DefaultApplication::onInit(Engine& engine) {
    m_engine = &engine;
    m_player = std::make_unique<PlayerController>(&engine);
    engine.setPlayerController(m_player.get());
    m_player->setHudRenderer(&m_hud);
    m_player->setKeepViewInsideMap(m_keepViewInsideMap);
    m_player->init();
    m_player->m_autoTest = m_autoTest;
    if (m_autoExit) engine.enableAutoExit(true);
    if (m_benchmarkWater) {
        engine.enableWaterBenchmark(true);
        engine.setBenchmarkMap(m_initialMap);
        engine.setBenchmarkPosition(m_benchmarkPos);
        engine.setBenchmarkDuration(m_benchmarkDuration);
    }
    if (m_benchmarkThunderstorm) {
        engine.enableBenchmarkThunderstorm(true);
        engine.setBenchmarkMap(m_initialMap);
        engine.setBenchmarkPosition(m_benchmarkPos);
        engine.setBenchmarkDuration(m_benchmarkDuration);
    }
    if (m_benchmarkHeavyRain) {
        engine.enableBenchmarkHeavyRain(true);
        engine.setBenchmarkMap(m_initialMap);
        engine.setBenchmarkPosition(m_benchmarkPos);
    }
    if (m_hasInitialPos) engine.setInitialPosition(m_initialPos);
    if (m_needFreeCamera) engine.camera().setDebugFreeCamera(true);
    engine.camera().setFov(m_fovDeg);
    if (m_hasInitialCamera) engine.setInitialCamera(m_initialYaw, m_initialPitch, m_initialDist);
    if (!m_spawnCloudAlts.empty()) engine.setPendingCloudSpawns(m_spawnCloudAlts, m_spawnCloudRain);
    if (!m_weatherTypeName.empty()) engine.setInitialWeatherType(m_weatherTypeName);
    if (m_cloudTestColors) engine.setCloudTestColors(true);
    if (m_debugRainOcclusion) engine.postSettings().debugRainOcclusion = true;
    engine.setAutoCloud(m_autoCloud);
    if (m_cloudBoxSet) engine.setCloudBoxOverride(m_cloudBoxBottom, m_cloudBoxThickness);
    if (m_cloudWindSpeed >= 0.0f) engine.setCloudWindOverride(m_cloudWindSpeed);
    if (m_cloudRadius >= 0.0f || m_cloudFalloff >= 0.0f) {
        float r = (m_cloudRadius >= 0.0f) ? m_cloudRadius : 60.0f;
        float f = (m_cloudFalloff >= 0.0f) ? m_cloudFalloff : 1.0f;
        engine.setLocalCloudParams(r, r * (40.0f / 60.0f), f);
    }
    if (engine.editorMode()) {
        m_editor = std::make_unique<Editor>();
        m_editor->init(engine);
        engine.setEditor(m_editor.get());
    }
    m_scripts = std::make_unique<ScriptHost>();
    if (!m_scripts->init(engine, project::game("assets/scripts").string())) m_scripts.reset();
    if (m_editor) m_editor->setScriptHost(m_scripts.get());
    else if (m_scripts)
        engine.setGameOverlay([this] {
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            if (m_engine) m_engine->drawSpeechBubbles(0.0f, 0.0f, ds.x, ds.y, ImGui::GetIO().DeltaTime);
            if (m_engine) m_engine->drawGameFx2D(0.0f, 0.0f, ds.x, ds.y, ImGui::GetIO().DeltaTime);
            if (m_scripts) m_scripts->drawMessages(0.0f, 0.0f, ds.x, ds.y);
        });
    if (!m_initialMap.empty()) {
        // For headless/screenshot runs and large single-file GLB maps, load
        // synchronously so the first rendered frame already has all geometry and
        // textures uploaded. Interactive runs still benefit from the background
        // loader for subsequent map warps.
        engine.loadMap(m_initialMap);
    }
    if (!m_warpOnLoad.empty()) {
        engine.setPendingMapWarp(m_warpOnLoad);
    }
    if (const char* seq = std::getenv("ERUPTION_TEST_MAP_SEQUENCE")) {
        std::string tok; std::istringstream ss(seq);
        while (std::getline(ss, tok, ',')) if (!tok.empty()) m_mapSequence.push_back(tok);
        const char* fr = std::getenv("ERUPTION_TEST_MAP_SEQUENCE_FRAMES");
        m_mapSequenceFrames = fr ? std::max(30, std::atoi(fr)) : 240;
        m_mapSequenceCountdown = m_mapSequenceFrames;
    }
    if (!m_screenshotPath.empty()) {
        if (!m_warpOnLoad.empty()) {
            engine.setScreenshotAfterWarp(m_screenshotPath);
        } else {
            engine.setScreenshotOnLoad(m_screenshotPath);
        }
    }
    m_hud.init(&engine, m_player.get());
    // Sem editor os scripts começam quando a cena do primeiro mapa já está
    // aplicada (onUpdate): on.start já encontra personagens e objetos acrescentados.
}
void DefaultApplication::onUpdate(float deltaTime) {
    if (m_engine && !m_mapSequence.empty() && --m_mapSequenceCountdown <= 0) {
        m_mapSequenceCountdown = m_mapSequenceFrames;
        const std::string& next = m_mapSequence[m_mapSequenceIndex++ % m_mapSequence.size()];
        ERUPTION_LOG_WARN("[MAPSEQ] carga fria #%zu: %s", m_mapSequenceIndex, next.c_str());
        m_engine->loadMap(next);
    }
    if (m_scripts) m_scripts->poll();
    if (m_editor) {
        // O editor (câmera, interface) anda no tempo real: na pausa o mundo para,
        // mas dá para continuar voando e olhando.
        m_editor->update(m_engine ? m_engine->timer().realDeltaTime() : deltaTime);
        // Scripts começam no Play e param no Stop; a pausa só congela.
        if (m_scripts && m_editor->inPlayMode() != m_scriptsPlaying) {
            m_scriptsPlaying = m_editor->inPlayMode();
            if (m_scriptsPlaying) m_scripts->start(); else m_scripts->stop();
        }
        // No editor a lógica de jogo só roda com Play (ou um passo em pausa).
        if (m_editor->gameRunning()) {
            if (m_engine) m_engine->updateCharacters(deltaTime, true);
            if (m_player) m_player->update(deltaTime);
            if (m_scripts) m_scripts->update(deltaTime);
        } else if (m_engine && m_player) {
            // Parado: personagens no lugar (animação de parado) e jogador visível.
            m_engine->updateCharacters(deltaTime, false);
            m_player->editorSprites();
        }
        m_editor->consumeStep();
    } else {
        // game.restart(): para os scripts, esquece a sessao e reabre o mapa
        // inicial; a entrada no mapa (abaixo) liga os scripts do zero.
        if (m_scripts && m_engine && m_scripts->takeRestartRequest()) {
            m_scripts->reboot();
            m_scriptsPlaying = false;
            m_engine->timer().setTimeScale(1.0f);
            m_engine->uiCanvas().clearEffects();
            m_engine->requestMap(m_initialMap, m_hasInitialPos ? &m_initialPos : nullptr);
        }
        // Porta, jogo salvo: troca de mapa pedida pelo jogo, aqui fora do frame.
        Engine::MapRequest request;
        if (m_engine && m_engine->takeMapRequest(request)) {
            m_hasMapSpawn = request.hasSpawn;
            m_mapSpawn = request.spawn;
            m_sceneEditsMap.clear(); // mesmo mapa de novo também reaplica a cena
            m_engine->loadMap(request.map);
        }
        // Edições feitas no editor (assets/scenes/<mapa>.scene.luau) valem no
        // jogo: aplicadas uma vez, depois que o jogador nasce no mapa.
        if (m_engine && m_engine->isPlayerSpawnedOnActiveMap() && m_engine->currentMapName() != m_sceneEditsMap) {
            m_sceneEditsMap = m_engine->currentMapName();
            // ERUPTION_TEST_LOAD_PHASES=1: tempo de cada passo da entrada no
            // mapa (o quadro depois da troca e' onde a cena pesa).
            static const bool kPhases = std::getenv("ERUPTION_TEST_LOAD_PHASES") != nullptr;
            auto mark = std::chrono::steady_clock::now();
            auto phase = [&mark](const char* name) {
                if (!kPhases) return;
                const auto now = std::chrono::steady_clock::now();
                ERUPTION_LOG_WARN("[MAPPHASE] %-14s %7.1f ms", name, std::chrono::duration<double, std::milli>(now - mark).count());
                mark = now;
            };
            SceneEdits edits;
            std::string error;
            if (!edits.load(SceneEdits::pathFor(m_sceneEditsMap), error)) ERUPTION_LOG_ERROR("%s", error.c_str());
            else if (!edits.empty())
                ERUPTION_LOG_WARN("Scene edits: %d change(s) applied to %s", edits.apply(*m_engine), m_sceneEditsMap.c_str());
            phase("cena");
            m_engine->loadScatter(m_sceneEditsMap); // objetos espalhados pelo pincel
            phase("scatter");
            m_engine->startWorld();                 // mapas vizinhos (mundo contínuo)
            phase("mundo");
            m_engine->setSoundEmittersActive(true); // sons da cena tocam com o jogo
            if (m_hasMapSpawn && m_player) {
                m_hasMapSpawn = false;
                const Vec3 p(m_mapSpawn.x, m_engine->sampleTerrainHeight(m_mapSpawn.x, m_mapSpawn.z), m_mapSpawn.z);
                m_player->setPos(p);
                m_engine->camera().setOrbitTarget(p);
            }
            if (m_scripts && !m_scriptsPlaying) {
                m_scripts->start();
                m_scriptsPlaying = true;
            } else if (m_scripts) {
                m_scripts->mapReady(); // mapa novo: foto e o estado de quando se saiu dele
            }
            phase("scripts");
        }
        if (m_engine) m_engine->updateCharacters(deltaTime, true);
        if (m_engine && m_player) m_player->update(deltaTime);
        // ERUPTION_TEST_PLAYER_WALK="x,z x,z ...;segundos" (teste): leva o jogador
        // pelos pontos, no chão, a partir do frame 60 (atravessar mapas emendados).
        static const char* testWalk = std::getenv("ERUPTION_TEST_PLAYER_WALK");
        static int walkFrame = 0;
        if (testWalk && m_engine && m_player && m_engine->isPlayerSpawnedOnActiveMap() && ++walkFrame >= 60) {
            static std::vector<Vec2> pts;
            static float seconds = 10.0f;
            if (pts.empty()) {
                std::string spec = testWalk;
                const size_t semi = spec.find(';');
                if (semi != std::string::npos) seconds = std::max(0.1f, std::stof(spec.substr(semi + 1)));
                std::istringstream in(spec.substr(0, semi));
                std::string p;
                while (in >> p) {
                    Vec2 v(0.0f);
                    std::sscanf(p.c_str(), "%f,%f", &v.x, &v.y);
                    pts.push_back(v);
                }
            }
            if (pts.size() >= 2) {
                const float t = std::min(1.0f, (walkFrame - 60) / (60.0f * seconds)) * float(pts.size() - 1);
                const size_t i = std::min(pts.size() - 2, size_t(t));
                const Vec2 p = glm::mix(pts[i], pts[i + 1], t - float(i));
                m_player->setPos(Vec3(p.x, m_engine->sampleTerrainHeight(p.x, p.y), p.y));
            }
        }
        if (m_scripts) m_scripts->update(deltaTime);
    }
    m_hud.update();
}
void DefaultApplication::onRender(Engine& /*engine*/) {
}
void DefaultApplication::onShutdown() {
    // Os recursos abaixo (texturas de sprite, scripts, editor) podem estar em
    // uso por frames ainda na GPU.
    if (m_engine) m_engine->vulkan().waitIdle();
    if (m_engine) m_engine->setGameOverlay(nullptr);
    if (m_scripts) {
        m_scripts->shutdown();
        if (m_editor) m_editor->setScriptHost(nullptr);
        m_scripts.reset();
    }
    if (m_editor) {
        m_editor->shutdown();
        m_editor.reset();
    }
    m_hud.shutdown();
    if (m_player && m_engine) {
        m_player->shutdown();
        m_engine->setPlayerController(nullptr);
    }
    m_engine = nullptr;
}
} 
