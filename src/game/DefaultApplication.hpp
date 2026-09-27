#pragma once
#include "math/Types.hpp"
#include <string>
#include <memory>
#include <vector>
#include "game/HudRenderer.hpp"
namespace eruption {
class Engine;
class DefaultApplication {
public:
    DefaultApplication(int argc, char** argv);
    void onInit(Engine& engine);
    void onUpdate(float deltaTime);
    void onRender(Engine& engine);
    void onShutdown();
    ~DefaultApplication();
private:
    void parseArgs(int argc, char** argv);
    Engine* m_engine = nullptr;
    std::unique_ptr<class PlayerController> m_player;
    HudRenderer m_hud;
    bool m_autoExit = false;
    bool m_autoTest = false;
    bool m_benchmarkWater = false;
    bool m_benchmarkThunderstorm = false;
    bool m_benchmarkHeavyRain = false;
    std::string m_initialMap = "parana_field";
    std::string m_screenshotPath;
    std::string m_warpOnLoad;
    // ERUPTION_TEST_MAP_SEQUENCE="vila-A,cidade-A,..." + _FRAMES=N (debug):
    // a cada N frames chama Engine::loadMap (carga FRIA, nao warp) com o
    // proximo mapa da lista, em loop. Serve pra vigiar o cache cross-mapa
    // ([ASSETCACHE] no log) e a VRAM em troca repetida no mesmo processo.
    std::vector<std::string> m_mapSequence;
    int m_mapSequenceFrames = 0;
    int m_mapSequenceCountdown = 0;
    size_t m_mapSequenceIndex = 0;
    Vec3 m_benchmarkPos = Vec3(1220.0f, 0.0f, 270.0f);
    float m_benchmarkDuration = 60.0f;
    bool m_hasInitialPos = false;
    Vec3 m_initialPos = Vec3(0.0f);
    bool m_hasInitialCamera = false;
    float m_initialYaw = -90.0f;
    float m_initialPitch = 50.0f;
    float m_initialDist = 158.5f;
    bool m_needFreeCamera = false;
    std::vector<float> m_spawnCloudAlts;
    bool m_spawnCloudRain = false;
    std::string m_weatherTypeName;
    bool m_cloudTestColors = false;
    bool m_debugRainOcclusion = false;
    bool m_autoCloud = true;
    bool m_cloudBoxSet = false;
    float m_cloudBoxBottom = 100.0f;
    float m_cloudWindSpeed = -1.0f; // >= 0: override cfg.windSpeed (test hook)
    float m_cloudRadius = -1.0f;    // >= 0: override local cloud radius X/Z (test hook)
    float m_cloudFalloff = -1.0f;   // >= 0: override local cloud falloff (test hook)
    float m_cloudBoxThickness = 800.0f;
};
} 
