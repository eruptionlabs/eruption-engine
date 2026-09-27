#pragma once
#include "utils/CSSLayout.hpp"
#include "math/Types.hpp"
#include "core/Engine.hpp"
#include <string>
namespace eruption {
class HudRenderer;
class PlayerController {
public:
    explicit PlayerController(Engine* engine = nullptr);
    void init();
    void update(float dt);
    void renderImGui();
    void renderApplicationHUD();
    void renderMinimapMarkers() const;
    // Retorna a área do minimapa definida pelo CSS (#minimap), se houver.
    eruption::CSSRect getMinimapRect(const ImVec2& displaySize) const;
    eruption::CSSRect getMinimapInfoRect(const ImVec2& displaySize) const;
    void renderMapSelector();
    void setHudRenderer(HudRenderer* hud) { m_hud = hud; }
    void shutdown();

    // Retorna a posição na tela da base do sprite do player,
    // replicando a transformação view-space billboard do sprite.vert
    // para que widgets bind-to-player fiquem literalmente colados no sprite.
    Vec2 getSpriteScreenBase(const Camera& camera, float screenW, float screenH) const;

    const Vec3& pos() const { return m_pos; }
    void setPos(const Vec3& p) { m_pos = p; }
    bool isMoving() const { return m_isMoving; }
    float yaw() const { return m_yaw; }
    void setYaw(float yaw) { m_yaw = yaw; }
    float spriteHeight() const { return m_spriteHeight; }
    float actualMoveSpeed() const { return m_actualMoveSpeed; }
    float adaptiveFocalOffset() const { return m_adaptiveFocalOffset; }
    bool freeCamera() const { return m_freeCamera; }
    bool tacticalView() const { return m_tacticalView; }
    void setTacticalView(bool v) { m_tacticalView = v; }
    bool snapToGat() const { return m_snapToGat; }
    float cameraSmoothFactor() const { return m_cameraSmoothFactor; }
    const Engine::LoadedSprite& bodySprite() const { return m_bodySprite; }
    const Engine::LoadedSprite& hairSprite() const { return m_hairSprite; }
    uint32_t bodyPaletteSlot() const { return m_bodyPaletteSlot; }
    uint32_t hairPaletteSlot() const { return m_hairPaletteSlot; }
    bool palettesInitialized() const { return m_palettesInitialized; }
    const Vec2& bodyAttachPoint() const { return m_bodyAttachPoint; }
    float warpClickDebounceTimer() const { return m_warpClickDebounceTimer; }
    const std::string& warpDebounceTarget() const { return m_warpDebounceTarget; }
    Engine::LoadedSprite& bodySpriteRef() { return m_bodySprite; }
    Engine::LoadedSprite& hairSpriteRef() { return m_hairSprite; }
    float& spriteHeightRef() { return m_spriteHeight; }
    Vec2& bodyAttachPointRef() { return m_bodyAttachPoint; }
    bool m_autoTest = false;
private:
    void updateWater(float dt);
    void updateConfigs(float dt);
    void updateUI(float dt);
    void updateMovement(float dt);
    void updateCamera(float dt, bool captureMouse, bool captureKeyboard);
    void updateWorld(float dt);
    void updateAutoTest(float dt);
    void updatePlayerSprites(float dt);
    void loadBodySprite();
    void loadHairSprite();
    void updatePalettes();
    Engine* m_engine = nullptr;
    Vec3 m_pos = Vec3(0.0f);
    bool m_isMoving = false;
    float m_yaw = 0.0f;
    float m_spriteHeight = 2.0f;
    HudRenderer* m_hud = nullptr;
    float m_actualMoveSpeed = 0.0f;
    float m_adaptiveFocalOffset = 0.0f;
    bool m_freeCamera = false;
    bool m_freeCameraToggle = false;
    bool m_freeCameraHeld = false;
    bool m_tacticalView = false;
    bool m_snapToGat = true;
    float m_preTacticalYaw = 0.0f;
    float m_preTacticalPitch = glm::radians(50.0f);
    float m_preTacticalDist = 158.5f;
    float m_cameraSmoothFactor = 10.0f;
    float m_autoTestTimer = 0.0f;
    size_t m_autoTestMapIndex = 0;
    float m_warpClickDebounceTimer = 0.0f;
    std::string m_warpDebounceTarget;
    Engine::LoadedSprite m_bodySprite;
    Engine::LoadedSprite m_hairSprite;
    uint32_t m_bodyPaletteSlot = 0;
    uint32_t m_hairPaletteSlot = 0;
    bool m_palettesInitialized = false;
    int m_bodyAction = 0;
    int m_bodyFrame = 0;
    float m_bodyTimer = 0.0f;
    int m_hairAction = 0;
    int m_hairFrame = 0;
    float m_hairTimer = 0.0f;
    Vec2 m_bodyAttachPoint = Vec2(0.0f);
    std::string m_lastJob;
    std::string m_lastHair;
    int m_lastGender = -1;
};
} 
