#include "editor/Editor.hpp"
#include "script/ScriptHost.hpp"
#include "game/DefaultApplication.hpp"
#include <cstdlib>
#include <sstream>
#include "game/PlayerController.hpp"
#include "core/Engine.hpp"
#include "core/Logger.hpp"
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
    m_initialMap = loadInitialMapFromConfig();
    parseArgs(argc, argv);
}
DefaultApplication::~DefaultApplication() = default;
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
            setenv("ERUPTION_TELEMETRY_OUT", argv[++i], 1);
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
    if (!m_scripts->init(engine, "assets/scripts")) m_scripts.reset();
    if (m_editor) m_editor->setScriptHost(m_scripts.get());
    else if (m_scripts)
        engine.setGameOverlay([this] {
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
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
    // Sem editor o jogo começa direto.
    if (!m_editor && m_scripts) m_scripts->start();
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
        m_editor->update(deltaTime);
        // Scripts começam no Play e param no Stop; a pausa só congela.
        if (m_scripts && m_editor->inPlayMode() != m_scriptsPlaying) {
            m_scriptsPlaying = m_editor->inPlayMode();
            if (m_scriptsPlaying) m_scripts->start(); else m_scripts->stop();
        }
        // No editor a lógica de jogo só roda com Play (ou um passo em pausa).
        if (m_editor->gameRunning()) {
            if (m_player) m_player->update(deltaTime);
            if (m_scripts) m_scripts->update(deltaTime);
        }
        m_editor->consumeStep();
    } else {
        if (m_engine && m_player) m_player->update(deltaTime);
        if (m_scripts) m_scripts->update(deltaTime);
    }
    m_hud.update();
}
void DefaultApplication::onRender(Engine& /*engine*/) {
}
void DefaultApplication::onShutdown() {
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
