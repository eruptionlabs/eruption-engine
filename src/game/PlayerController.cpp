#include "game/PlayerController.hpp"
#include "game/HudRenderer.hpp"
#include "core/Input.hpp"
#include "renderer/skymap/SkyConfig.hpp"
#include "formats/TerrainParser.hpp"
#include "core/Logger.hpp"
#include "renderer/SpritePickerUI.hpp"
#include "renderer/PostProcessor.hpp"
#include "renderer/WeatherRenderer.hpp"
#include "renderer/WeatherSystem.hpp"
#include <imgui.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <unordered_map>
namespace eruption {
PlayerController::PlayerController(Engine* engine)
    : m_engine(engine)
{
}
void PlayerController::init() {
    if (!m_engine) return;
    m_bodyPaletteSlot = m_engine->spriteRenderer().addPalette(m_engine->charConfig().bodyPalette.colors);
    m_hairPaletteSlot = m_engine->spriteRenderer().addPalette(m_engine->charConfig().hairPalette.colors);
    m_palettesInitialized = true;
    // ERUPTION_TEST_FREE_CAM=1 (debug): start with free camera active — prevents
    // the orbit camera from following the player sprite and keeps the pose
    // set by ERUPTION_TEST_CAM_POSE perfectly still for headless screenshot runs.
    if (std::getenv("ERUPTION_TEST_FREE_CAM")) {
        m_freeCameraToggle = true;
        m_freeCamera = true;
        if (m_engine) m_engine->camera().setDebugFreeCamera(true);
    }
}
void PlayerController::shutdown() {
    if (!m_engine) return;
    m_engine->cleanupSpriteTextures(m_bodySprite);
    m_engine->cleanupSpriteTextures(m_hairSprite);
}
void PlayerController::update(float dt) {
    if (dt <= 0.0f || !m_engine) return;
    // ERUPTION_TEST_PC_MARKS=1 (debug): per-sub-phase timing, printed once a
    // second. onUpdate dominates the frame on some maps and the engine-side
    // marks cannot see inside it.
    static const bool pcMarks = std::getenv("ERUPTION_TEST_PC_MARKS") != nullptr;
    using _clk = std::chrono::steady_clock;
    using _ms = std::chrono::duration<float, std::milli>;
    auto _p = _clk::now();
    float t_water = 0, t_cfg = 0, t_ui = 0, t_move = 0, t_cam = 0, t_world = 0, t_auto = 0, t_spr = 0;
    auto _lap = [&](float& out) { if (pcMarks) { auto n = _clk::now(); out = _ms(n - _p).count(); _p = n; } };

    updateWater(dt);      _lap(t_water);
    updateConfigs(dt);    _lap(t_cfg);
    ImGuiIO& io = ImGui::GetIO();
    bool captureMouse = io.WantCaptureMouse;
    bool captureKeyboard = io.WantCaptureKeyboard;
    updateUI(dt);         _lap(t_ui);
    updateMovement(dt);   _lap(t_move);
    updateCamera(dt, captureMouse, captureKeyboard); _lap(t_cam);
    updateWorld(dt);      _lap(t_world);
    updateAutoTest(dt);   _lap(t_auto);
    updatePlayerSprites(dt); _lap(t_spr);

    if (pcMarks) {
        static float acc = 0.0f;
        acc += dt;
        if (acc >= 1.0f) {
            acc = 0.0f;
            ERUPTION_LOG_WARN("[PC] water=%.2f cfg=%.2f ui=%.2f move=%.2f cam=%.2f world=%.2f auto=%.2f sprites=%.2f ms",
                              t_water, t_cfg, t_ui, t_move, t_cam, t_world, t_auto, t_spr);
        }
    }
}
void PlayerController::updateWater(float dt) {
    static float foamTestTimer = 0.0f;
    foamTestTimer += dt;
    if (foamTestTimer >= 2.0f) {
        foamTestTimer = 0.0f;
        Vec3 target = m_engine->camera().target();
        if (m_engine->renderEffect(7)->isEnabled() && m_engine->water().hasWater()) {
            m_engine->water().addContactFoam(target, 3.0f, 1.0f);
        }
    }
    if (m_engine->waterMenu().requestGenerateBlueNoise) {
        m_engine->waterMenu().requestGenerateBlueNoise = false;
        m_engine->water().generateBlueNoiseTexture(512);
    }
    if (m_engine->waterMenu().requestLoadTexture && !m_engine->waterMenu().config.waterTexturePath.empty()) {
        m_engine->waterMenu().requestLoadTexture = false;
        m_engine->water().loadWaterTextureFromFile(m_engine->waterMenu().config.waterTexturePath.c_str());
    }
    if (m_engine->waterMenu().requestReloadShaders) {
        m_engine->waterMenu().requestReloadShaders = false;
        m_engine->water().reloadShaders();
    }
    static Vec3 s_lastCharPos = m_pos;
    if (m_engine->water().hasWater() && m_engine->renderEffect(7)->isEnabled()) {
        float waterSurface = m_engine->mapBaseWaterLevel() + m_engine->waterMenu().config.waterLevel;
        if (m_pos.y <= waterSurface + 0.5f) {
            float moved = glm::distance(Vec2(m_pos.x, m_pos.z), Vec2(s_lastCharPos.x, s_lastCharPos.z));
            if (moved > 0.1f) {
                m_engine->water().addContactFoamTrail(s_lastCharPos, m_pos, 2.5f, 1.0f);
            }
        }
    }
    s_lastCharPos = m_pos;
    if (m_warpClickDebounceTimer > 0.0f) {
        m_warpClickDebounceTimer -= dt;
        if (m_warpClickDebounceTimer <= 0.0f && !m_warpDebounceTarget.empty()) {
            m_engine->setPendingMapWarp(m_warpDebounceTarget);
            m_warpDebounceTarget.clear();
        }
    }
}
void PlayerController::updateConfigs(float /*dt*/) {
    if (!Input::isKeyPressed(Key::F5)) return;
    bool anyReloaded = false;
    if (m_engine->shadowConfig().reloadIfModified()) {
        auto& shadowSettings = m_engine->shadowRenderer().settings();
        shadowSettings.loadFromJson(m_engine->shadowConfig().root());
        if (shadowSettings.atlasSize > 0) {
            m_engine->shadowRenderer().resizeAtlas(
                static_cast<uint32_t>(shadowSettings.atlasSize));
        }
        ERUPTION_LOG_INFO("Shadow config hot-reloaded from data/shadows.json");
        anyReloaded = true;
    }
    if (m_engine->graphicsConfig().reloadIfModified()) {
        m_engine->applyGraphicsPreset();
        ERUPTION_LOG_INFO("Graphics preset hot-reloaded from data/graphics.json");
        anyReloaded = true;
    }
    if (m_engine->postConfig().reloadIfModified()) {
        m_engine->postSettings().loadFromJson(m_engine->postConfig().root());
        ERUPTION_LOG_INFO("Post-process config hot-reloaded from data/postprocess.json");
        anyReloaded = true;
    }
    if (m_engine->dayNightConfig().reloadIfModified()) {
        m_engine->dayNightCycle().loadFromJson(m_engine->dayNightConfig().root());
        float yawSnaps[8];
        float timeSnaps[8];
        for (uint32_t i = 0; i < m_engine->dayNightCycle().getKeyframeCount(); i++) {
            timeSnaps[i] = m_engine->dayNightCycle().getKeyframes()[i].timeOfDay;
            yawSnaps[i] = DayNightCycle::getSunYaw(timeSnaps[i]);
        }
        m_engine->shadowRenderer().setShadowSnaps(yawSnaps, timeSnaps, m_engine->dayNightCycle().getKeyframeCount());
        ERUPTION_LOG_INFO("Day/Night config hot-reloaded from data/daynight.json");
        anyReloaded = true;
    }
    if (m_engine->dioramaConfig().reloadIfModified()) {
        if (m_engine->dioramaConfig().root().contains("default_preset") && m_engine->dioramaConfig().root().contains("presets")) {
            std::string presetName = m_engine->dioramaConfig().root()["default_preset"];
            const auto& presets = m_engine->dioramaConfig().root()["presets"];
            if (presets.contains(presetName)) {
                m_engine->lookConfig().loadFromJson(presets[presetName]);
                m_engine->lookConfig().name = presetName;
            }
        }
        ERUPTION_LOG_INFO("Diorama look config hot-reloaded from data/diorama.json");
        anyReloaded = true;
    }
    if (m_engine->skyboxConfig().reloadIfModified()) {
        eruption::SkyConfig skyConfig;
        skyConfig.loadFromJson(m_engine->skyboxConfig().root());
        m_engine->skyboxSystem().reloadConfig(skyConfig);
        // Re-bind sky texture in case the backend changed and produced a new output image.
        m_engine->water().setSkyTexture(m_engine->skyboxSystem().outputView(), m_engine->skyboxSystem().outputSampler());
        ERUPTION_LOG_INFO("Skybox config hot-reloaded from data/skybox_config.json");
        anyReloaded = true;
    }
    if (!anyReloaded) {
        ERUPTION_LOG_INFO("F5 pressed — no config files modified.");
    }
}
void PlayerController::updateUI(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    bool captureMouse = io.WantCaptureMouse;
    bool captureKeyboard = io.WantCaptureKeyboard;
    if (!captureKeyboard) {
        if (Input::isKeyPressed(Key::F1)) m_engine->showObjectMenu() = !m_engine->showObjectMenu();
        if (Input::isKeyPressed(Key::F2)) m_engine->showEffectsMenu() = !m_engine->showEffectsMenu();
        if (Input::isKeyPressed(Key::F3)) m_engine->showStatsMenu() = !m_engine->showStatsMenu();
        if (Input::isKeyPressed(Key::F4)) m_engine->showDebugOverlay() = !m_engine->showDebugOverlay();
        if (Input::isKeyPressed(Key::F9)) m_engine->showABTestMenu() = !m_engine->showABTestMenu();
        if (Input::isKeyPressed(Key::F8)) m_engine->toggleWireframe();
        if (Input::isKeyPressed(Key::F6)) m_engine->showSpritePicker() = !m_engine->showSpritePicker();
        if (Input::isKeyPressed(Key::F12)) m_engine->showResourceManager() = !m_engine->showResourceManager();
        // F10: liga/desliga a luz presa ao sprite E abre o menu de cor dela.
        // Um toque so' pra' os dois: quem aperta quer VER a luz e ajustar.
        if (Input::isKeyPressed(Key::F10)) {
            m_engine->playerLight().enabled = !m_engine->playerLight().enabled;
            m_engine->showPlayerLightMenu() = m_engine->playerLight().enabled;
        }
        if (Input::isKeyPressed(Key::Escape) && m_engine->showConsole()) m_engine->showConsole() = false;
        if (Input::isKeyPressed(Key::Enter)) m_engine->showConsole() = !m_engine->showConsole();
    }
    (void)captureMouse; 
    // O contador de FPS saiu daqui para Engine::run(). Motivo: update()
    // comeca com `if (dt <= 0.0f) return;`, entao todo frame de dt zero era
    // desenhado e NAO contado, e o FPS reportado ficava abaixo do real.
    // Contar quadro e' trabalho do laco, nao da logica de jogo.
}
void PlayerController::updateMovement(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    float currentMoveSpeed = m_engine->charConfig().moveSpeed;
    if (!m_engine->showConsole() && !io.WantTextInput && Input::isKeyDown(Key::LeftShift))
        currentMoveSpeed *= m_engine->inputConfig().kbMouse.sprintMultiplier;
    float panSpeed = currentMoveSpeed * dt;
    if (!m_freeCamera) panSpeed *= 0.5f;
    m_actualMoveSpeed = currentMoveSpeed;
    static std::string lastGpadName = "";
    int activeGpad = m_engine->inputConfig().gamepad.selectedGamepadIndex;
    if (Input::isGamepadPresent(activeGpad)) {
        std::string curName = Input::getGamepadName(activeGpad);
        if (curName != lastGpadName) {
            std::string lowerName = curName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            if (lowerName.find("lukton") != std::string::npos || lowerName.find("generic") != std::string::npos || lowerName.find("ps2") != std::string::npos) {
                ERUPTION_LOG_INFO("Lukton/PS2 Controller detected! Applying automatic mapping: Left Stick = Camera, Right Stick = Walk.");
                m_engine->inputConfig().gamepad.cameraAxisX = 0;
                m_engine->inputConfig().gamepad.cameraAxisY = 1;
                m_engine->inputConfig().gamepad.movementAxisX = 2;
                m_engine->inputConfig().gamepad.movementAxisY = 3;
                m_engine->inputConfig().save("data/input_config.json");
            }
            lastGpadName = curName;
        }
    }
    Vec3 camForward = m_engine->camera().forward();
    camForward.y = 0.0f;
    if (glm::length(camForward) > 0.001f) camForward = glm::normalize(camForward);
    Vec3 camRight = m_engine->camera().right();
    camRight.y = 0.0f;
    if (glm::length(camRight) > 0.001f) camRight = glm::normalize(camRight);
    if (!m_engine->showConsole() && !io.WantTextInput) {
        Vec3 move(0.0f);
        bool canMove = true;
        if (canMove) {
            if (Input::isKeyDown(Key::W)) move += camForward;
            if (Input::isKeyDown(Key::S)) move -= camForward;
            if (Input::isKeyDown(Key::A)) move -= camRight;
            if (Input::isKeyDown(Key::D)) move += camRight;
        }
        if (Input::isGamepadPresent(activeGpad)) {
            float gCamX = Input::getGamepadAxis(m_engine->inputConfig().gamepad.cameraAxisX, activeGpad);
            float gCamY = Input::getGamepadAxis(m_engine->inputConfig().gamepad.cameraAxisY, activeGpad);
            float gMoveX = Input::getGamepadAxis(m_engine->inputConfig().gamepad.movementAxisX, activeGpad);
            float gMoveY = Input::getGamepadAxis(m_engine->inputConfig().gamepad.movementAxisY, activeGpad);
            auto applyDeadzone = [&](float val) {
                if (std::abs(val) < m_engine->inputConfig().gamepad.deadzone) return 0.0f;
                return (val - (val > 0 ? m_engine->inputConfig().gamepad.deadzone : -m_engine->inputConfig().gamepad.deadzone)) / (1.0f - m_engine->inputConfig().gamepad.deadzone);
            };
            gCamX = applyDeadzone(gCamX);
            gCamY = applyDeadzone(gCamY);
            gMoveX = applyDeadzone(gMoveX);
            gMoveY = applyDeadzone(gMoveY);
            if (m_engine->inputConfig().gamepad.invertCameraX) gCamX = -gCamX;
            if (m_engine->inputConfig().gamepad.invertCameraY) gCamY = -gCamY;
            if (std::abs(gCamX) > 0.01f) m_engine->camera().orbitYaw(gCamX * dt * m_engine->inputConfig().gamepad.cameraSensitivity);
            if (std::abs(gCamY) > 0.01f) m_engine->camera().orbitPitch(-gCamY * dt * m_engine->inputConfig().gamepad.cameraSensitivity);
            move += camRight * gMoveX * m_engine->inputConfig().gamepad.movementSensitivity;
            move += camForward * -gMoveY * m_engine->inputConfig().gamepad.movementSensitivity;
            if (Input::isGamepadButtonPressed(m_engine->inputConfig().gamepad.cameraResetButton, activeGpad))
                m_engine->camera().resetOrbit();
        }
        m_isMoving = false;
        if (glm::length(move) > 0.001f) {
            m_isMoving = true;
            if (!m_tacticalView) m_yaw = std::atan2(move.x, move.z);
            move = glm::normalize(move) * panSpeed;
            if (!m_freeCamera) {
                if (m_snapToGat && m_engine->currentMap() && !m_tacticalView) {
                    Vec3 nextPos = m_pos + move;
                    float gatCellSize = 5.0f;
                    auto isWalkable = [&](const Vec3& p) {
                        int gx = (int)(p.x / gatCellSize);
                        int gz = (int)(p.z / gatCellSize);
                        if (gx >= 0 && gz >= 0 && gx < (int)m_engine->currentMap()->navGrid.width && gz < (int)m_engine->currentMap()->navGrid.height)
                            return m_engine->currentMap()->navGrid.at(gx, gz).isWalkable();
                        return false;
                    };
                    Vec3 finalPos = m_pos;
                    if (isWalkable(Vec3(nextPos.x, m_pos.y, m_pos.z))) finalPos.x = nextPos.x;
                    if (isWalkable(Vec3(finalPos.x, m_pos.y, nextPos.z))) finalPos.z = nextPos.z;
                    if (m_engine->currentMap()->isGlbMap) {
                        finalPos.y = m_engine->sampleTerrainHeight(finalPos.x, finalPos.z);
                    } else {
                        int fgx = (int)(finalPos.x / gatCellSize);
                        int fgz = (int)(finalPos.z / gatCellSize);
                        if (fgx >= 0 && fgz >= 0 && fgx < (int)m_engine->currentMap()->navGrid.width && fgz < (int)m_engine->currentMap()->navGrid.height)
                            finalPos.y = m_engine->currentMap()->navGrid.at(fgx, fgz).worldY();
                    }
                    m_pos = finalPos;
                } else {
                    m_pos += move;
                }
            } else {
                m_engine->camera().moveTarget(move);
            }
        }
    }
}
void PlayerController::updateCamera(float dt, bool captureMouse, bool captureKeyboard) {
    bool middleHeld = Input::isMouseDown(MouseButton::Middle);
    if (middleHeld && !m_freeCameraHeld) {
        m_freeCameraHeld = true;
    } else if (!middleHeld && m_freeCameraHeld) {
        m_freeCameraHeld = false;
    }
    m_freeCamera = m_freeCameraToggle || m_freeCameraHeld;
    m_engine->camera().setDebugFreeCamera(m_freeCamera);
    if (!m_freeCamera) {
        if (!m_engine->isPlayerSpawnedOnActiveMap()) {
            // Map swap hasn't happened yet; keep the camera parked at the safe
            // pre-warp position so it doesn't chase the player into the void.
            return;
        }
        Vec3 targetPos = m_pos;
        targetPos.y += m_spriteHeight * 0.5f;
        Vec3 target = m_engine->camera().target();
        target += (targetPos - target) * glm::clamp(m_cameraSmoothFactor * dt, 0.0f, 1.0f);
        if (m_engine->currentMap()) {
            float limitX = (float)m_engine->currentMap()->terrain.width * 10.0f;
            float limitZ = (float)m_engine->currentMap()->terrain.height * 10.0f;
            target.x = glm::clamp(target.x, 0.0f, limitX);
            target.z = glm::clamp(target.z, 0.0f, limitZ);
        }
        m_engine->camera().setOrbitTarget(target);
    }
    float scroll = Input::mouseScrollDelta();
    if (!captureMouse && scroll != 0.0f) {
        if (m_tacticalView) {
            float dist = m_engine->camera().orbitDistance();
            int level = 0;
            if (dist > 550.0f) level = 1;
            if (dist > 850.0f) level = 2;
            if (scroll > 0) {
                if (level > 0) {
                    level--;
                    float newDist = (level == 0) ? 400.0f : 700.0f;
                    m_engine->camera().setOrbit(m_engine->camera().orbitYaw(), m_engine->camera().orbitPitch(), newDist);
                }
            } else if (scroll < 0) {
                if (level < 2) {
                    level++;
                    float newDist = (level == 2) ? 1000.0f : 700.0f;
                    m_engine->camera().setOrbit(m_engine->camera().orbitYaw(), m_engine->camera().orbitPitch(), newDist);
                }
            }
        } else {
            m_engine->camera().zoom(1.0f + scroll * m_engine->inputConfig().kbMouse.scrollSensitivity);
        }
    }
    if (!m_engine->showConsole() && !ImGui::GetIO().WantTextInput) {
        if (Input::isKeyPressed(Key::F7)) {
            m_tacticalView = !m_tacticalView;
            if (m_tacticalView) {
                m_preTacticalYaw = m_engine->camera().orbitYaw();
                m_preTacticalPitch = m_engine->camera().orbitPitch();
                m_preTacticalDist = m_engine->camera().orbitDistance();
                // A visao tatica QUER passar dos 70 graus do jogo normal, entao
                // SOBE o teto de pitch em vez de furar o clamp com setOrbit.
                // Sem isto a camera ficava em 89 mas o arrasto so' aceitava ate'
                // 70, e o primeiro movimento do mouse dava um salto de 19 graus
                // (o "jump" reproduzido no parana_demo, 2026-09-04). Com o teto
                // levantado, arrastar o pitch de 10 a 89 aqui e' continuo.
                m_engine->camera().setPitchLimits(glm::radians(10.0f), glm::radians(89.0f));
                m_engine->camera().setOrbit(m_engine->camera().orbitYaw(), glm::radians(89.0f), 1000.0f);
            } else {
                // Volta a faixa do jogo ANTES de restaurar a pose: setOrbit
                // clampa, entao a pose pre-tatica (que ja' estava na faixa)
                // volta intacta e nada fica fora dos limites.
                m_engine->camera().setPitchLimits(glm::radians(10.0f), glm::radians(70.0f));
                m_engine->camera().setOrbit(m_preTacticalYaw, m_preTacticalPitch, m_preTacticalDist);
            }
        }
        if (Input::isKeyPressed(Key::Space)) {
            m_freeCameraToggle = !m_freeCameraToggle;
        }
    }
    Vec3 camForward = m_engine->camera().forward();
    camForward.y = 0.0f;
    if (glm::length(camForward) > 0.001f) camForward = glm::normalize(camForward);
    Vec3 camRight = m_engine->camera().right();
    camRight.y = 0.0f;
    if (glm::length(camRight) > 0.001f) camRight = glm::normalize(camRight);
    if (!captureMouse) {
        Vec2 md = Input::mouseDelta();
        if (Input::isMouseDown(MouseButton::Left)) {
            if (Input::isKeyDown(Key::LeftAlt)) m_engine->camera().orbitPitch(md.y * m_engine->inputConfig().kbMouse.mouseSensitivity);
            else m_engine->camera().orbitYaw(md.x * m_engine->inputConfig().kbMouse.mouseSensitivity);
        }
        if (Input::isMouseDown(MouseButton::Right)) {
            m_engine->camera().orbitYaw(md.x * m_engine->inputConfig().kbMouse.mouseSensitivity);
            float my = m_engine->inputConfig().kbMouse.invertMouseY ? md.y : -md.y;
            m_engine->camera().orbitPitch(my * m_engine->inputConfig().kbMouse.mouseSensitivity);
        }
        if (Input::isMouseDown(MouseButton::Middle)) {
            float dragSpeed = m_engine->camera().position().y * 0.002f;
            m_engine->camera().moveTarget(-camRight * md.x * dragSpeed + camForward * md.y * dragSpeed);
        }
    }
    if (Input::isKeyPressed(Key::Home)) m_engine->camera().resetOrbit();
    (void)captureKeyboard; 
}
void PlayerController::updateWorld(float dt) {
    static const bool pcMarks = std::getenv("ERUPTION_TEST_PC_MARKS") != nullptr;
    using _clk = std::chrono::steady_clock;
    using _ms = std::chrono::duration<float, std::milli>;
    auto _p = _clk::now();
    float w_day = 0, w_anim = 0, w_spr = 0, w_chunk = 0;
    auto _lap = [&](float& out) { if (pcMarks) { auto n = _clk::now(); out = _ms(n - _p).count(); _p = n; } };

    m_engine->dayNightCycle().update(dt);
    m_engine->updateNightLights();
    _lap(w_day);
    m_engine->modelRenderer().updateAnimations(m_engine->timer().elapsed() * 1000.0f);
    _lap(w_anim);
    m_engine->spriteSystem().processSprites(m_engine->camera().frustum(), m_engine->camera().position(), m_engine->camera().forward());
    _lap(w_spr);
    // Contador p/ F3/telemetria: reusa o resultado do culling AVX2 do passe
    // de terreno (1 frame atras - estatistica, nao decisao de render). A
    // versao anterior re-varria TODOS os chunks (AoS 72B) com teste escalar
    // por frame, duplicando o que cullChunks ja' fez vetorizado.
    // (Varredura de hot paths 2026-09-02.)
    m_engine->visibleChunks() = static_cast<int>(
        m_engine->terrainRenderer().visibleChunkIndices().size());
    _lap(w_chunk);
    if (pcMarks) {
        static float wacc = 0.0f;
        wacc += dt;
        if (wacc >= 1.0f) {
            wacc = 0.0f;
            ERUPTION_LOG_WARN("[PC-WORLD] daynight=%.2f modelAnim=%.2f sprites=%.2f chunkCull=%.2f ms",
                              w_day, w_anim, w_spr, w_chunk);
        }
    }
    m_engine->debugLineRenderer().clearLines();
    m_engine->overlayLineRenderer().clearLines();
    if (m_engine->debugModelPivots() && m_engine->currentMap() && !m_engine->autoExitEnabled()) {
        const auto& insts = m_engine->modelRenderer().getInstances();
        const auto& frustum = m_engine->camera().frustum();
        Vec3 camPos = m_engine->camera().position();
        float proximityRadius = 150.0f;
        for (const auto& inst : insts) {
            if (!inst.enabled) continue;
            if (!frustum.intersectsAABB(inst.worldAabbMin, inst.worldAabbMax)) continue;
            float dist = glm::distance(camPos, inst.worldCenter);
            if (dist > proximityRadius) continue;
            Vec3 pivotPos = Vec3(inst.transform[3]);
            float groundY = TerrainParser::getTerrainHeightAt(m_engine->currentMap()->terrain, pivotPos.x, pivotPos.z);
            float heightDiff = pivotPos.y - groundY;
            m_engine->debugLineRenderer().addLine(pivotPos, Vec3(pivotPos.x, groundY, pivotPos.z), Vec3(1.0f, 0.0f, 0.0f));
            m_engine->debugLineRenderer().addBox(pivotPos - Vec3(0.5f), pivotPos + Vec3(0.5f), Vec3(1.0f, 1.0f, 0.0f));
            if (std::abs(heightDiff) > 0.01f) {
                static std::unordered_map<std::string, float> s_lastLoggedDiff;
                if (s_lastLoggedDiff[inst.name] != heightDiff) {
                    ERUPTION_LOG_INFO("[HEIGHT_DEBUG] Model: %s | PivotY: %.3f | GroundY: %.3f | Diff: %.3f",
                                     inst.name.c_str(), pivotPos.y, groundY, heightDiff);
                    s_lastLoggedDiff[inst.name] = heightDiff;
                }
            }
        }
    }
    if (m_engine->postSettings().debugRainRegions) {
        auto& wr = m_engine->postProcessor().weatherRenderer();
        const auto& followers = wr.rainFollowers();
        const auto& weather = m_engine->weatherSystem().current();
        if (!followers.empty()) {
            for (const auto& f : followers) {
                float boxHeight = glm::max(f.boxCenter.y * 0.95f, 60.0f);
                Vec3 boxMin(f.boxCenter.x - f.boxSizeXZ.x * 0.5f, 0.0f, f.boxCenter.z - f.boxSizeXZ.y * 0.5f);
                Vec3 boxMax(f.boxCenter.x + f.boxSizeXZ.x * 0.5f, boxHeight, f.boxCenter.z + f.boxSizeXZ.y * 0.5f);
                // Rain region box (Cyan)
                m_engine->debugLineRenderer().addBox(boxMin, boxMax, Vec3(0.0f, 1.0f, 1.0f));
            }
        } else if (weather.rainIntensity > 0.001f) {
            Vec3 camPos = m_engine->camera().position();
            float boxHeight = glm::max(wr.cloudBottom() * 0.95f, 60.0f);
            Vec3 boxMin(camPos.x - 40.0f, 0.0f, camPos.z - 40.0f);
            Vec3 boxMax(camPos.x + 40.0f, boxHeight, camPos.z + 40.0f);
            m_engine->debugLineRenderer().addBox(boxMin, boxMax, Vec3(0.0f, 1.0f, 0.0f));
        }
    }
    if (m_engine->postSettings().cocEnableAdaptiveFocal && m_engine->postSettings().cocEnableZoomMapping) {
        float obstruction = m_engine->computeObstruction();
        float zoomPercent = 1.0f - (m_engine->camera().orbitDistance() - 10.0f) / (1000.0f - 10.0f);
        float baseFocal = m_engine->postSettings().cocFocalCurve.evaluate(zoomPercent);
        float targetOffset = -baseFocal * obstruction;
        float speed = glm::clamp(m_engine->postSettings().cocAdaptiveFocalDamping * dt * 60.0f, 0.0f, 1.0f);
        m_adaptiveFocalOffset += (targetOffset - m_adaptiveFocalOffset) * speed;
    } else {
        m_adaptiveFocalOffset = 0.0f;
    }
}
void PlayerController::updateAutoTest(float dt) {
    static bool s_autoTestFrozen = false;
    if (!m_autoTest) return;
    m_autoTestTimer += dt;
    if (!s_autoTestFrozen) {
        float angle = m_engine->timer().elapsed() * 2.0f;
        float speed = 50.0f * dt;
        Vec3 autoMove(std::sin(angle) * speed, 0.0f, std::cos(angle) * speed);
        m_pos += autoMove;
        m_engine->camera().setOrbitTarget(m_pos);
        m_isMoving = true;
    }
    if (m_autoTestTimer > 10.0f && !s_autoTestFrozen) {
        static const char* forceWarp = std::getenv("ERUPTION_TEST_WARP_TARGET");
        if (forceWarp || m_engine->availableMaps().size() > 1) {
            m_autoTestTimer = 0.0f;
            static bool firstWarp = true;
            std::string nextMap;
            if (forceWarp) {
                nextMap = forceWarp;
            } else if (firstWarp) {
                nextMap = "parana_field";
                firstWarp = false;
            } else {
                m_autoTestMapIndex = (m_autoTestMapIndex + 1) % m_engine->availableMaps().size();
                nextMap = m_engine->availableMaps()[m_autoTestMapIndex];
            }
            ERUPTION_LOG_INFO("Auto-test: warping to %s", nextMap.c_str());
            m_engine->setPendingMapWarp(nextMap);
            s_autoTestFrozen = true;
        }
    }
}
void PlayerController::renderMapSelector() {
    if (!m_engine) return;
    static char mfilt[64] = "";
    ImGui::InputText("Search Maps", mfilt, sizeof(mfilt));
    if (m_warpClickDebounceTimer > 0.0f) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
            "Warping to %s in %.1fs...", m_warpDebounceTarget.c_str(), m_warpClickDebounceTimer);
    }
    if (ImGui::BeginChild("MapList", ImVec2(0, 150), true)) {
        for (const auto& mapName : m_engine->availableMaps()) {
            if (mfilt[0] != '\0' && mapName.find(mfilt) == std::string::npos) continue;
            bool isSelected = (m_engine->currentMapName() == mapName);
            if (m_warpDebounceTarget == mapName) isSelected = true;
            if (ImGui::Selectable(mapName.c_str(), isSelected)) {
                m_warpClickDebounceTimer = 0.05f;
                m_warpDebounceTarget = mapName;
            }
        }
        ImGui::EndChild();
    }
}
void PlayerController::renderImGui() {
    if (!m_engine) return;
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0, 1, 0.5f, 1), "Camera Smooth Settings");
    ImGui::SliderFloat("Walk Smoothness", &m_cameraSmoothFactor, 1.0f, 50.0f, "%.1f");
    ImGui::Checkbox("Free Camera (Space)", &m_freeCameraToggle);
    ImGui::Checkbox("Tactical View (F7)", &m_tacticalView);
    ImGui::Checkbox("Snap to NavGrid", &m_snapToGat);
}

void PlayerController::renderApplicationHUD() {
    if (m_hud) m_hud->draw();
}

void PlayerController::renderMinimapMarkers() const {
}

eruption::CSSRect PlayerController::getMinimapRect(const ImVec2& displaySize) const {
    if (m_hud) return m_hud->getMinimapRect(displaySize);
    return {};
}

eruption::CSSRect PlayerController::getMinimapInfoRect(const ImVec2& displaySize) const {
    if (m_hud) return m_hud->getMinimapInfoRect(displaySize);
    return {};
}

static Palette s_lastBodyPal;
static Palette s_lastHairPal;
void PlayerController::updatePlayerSprites(float dt) {
    bool jobChanged = m_engine->charConfig().job != m_lastJob;
    bool hairChanged = m_engine->charConfig().hair != m_lastHair;
    bool genderChanged = m_engine->charConfig().gender != m_lastGender;
    if ((jobChanged || genderChanged) && !m_engine->charConfig().job.empty()) {
        loadBodySprite();
    }
    if ((hairChanged || genderChanged) && !m_engine->charConfig().hair.empty()) {
        loadHairSprite();
    }
    m_lastGender = m_engine->charConfig().gender;
    updatePalettes();
    // ERUPTION_TEST_HIDE_PLAYER=1: don't submit the character sprite (clean
    // screenshots of terrain/lighting without the player billboard in the shot).
    static const bool s_hidePlayer = std::getenv("ERUPTION_TEST_HIDE_PLAYER") != nullptr;
    if (s_hidePlayer) return;
    int bodySprites = 0, hairSprites = 0;
    m_engine->renderSpritePart(m_bodySprite, m_bodyPaletteSlot, m_bodyFrame, m_bodyTimer, dt, bodySprites, true);
    m_engine->renderSpritePart(m_hairSprite, m_hairPaletteSlot, m_hairFrame, m_hairTimer, dt, hairSprites, false);
    if (bodySprites > 0 || hairSprites > 0) {
        ERUPTION_LOG_INFO("Submitted %d body + %d hair sprites at pos (%.1f, %.1f, %.1f)", bodySprites, hairSprites, m_pos.x, m_pos.y, m_pos.z);
    }
}
void PlayerController::loadBodySprite() {
    m_engine->cleanupSpriteTextures(m_bodySprite);
    const std::string& configJob = m_engine->charConfig().job;

    // Only PNG/ERUPTSPR assets are supported.
    // If the config points to a PNG, try it; otherwise fall back to the
    // bundled default sprite so the player is always visible.
    if (configJob.size() > 4 &&
        (configJob.compare(configJob.size() - 4, 4, ".png") == 0 ||
         configJob.compare(configJob.size() - 4, 4, ".PNG") == 0)) {
        std::string sprPath = configJob.substr(0, configJob.size() - 4) + ".spr";
        if (std::ifstream(sprPath, std::ios::binary)) {
            ERUPTION_LOG_WARN("Loading body sprite from ERUPTSPR: %s", sprPath.c_str());
            if (m_engine->loadSpriteFromEruptSpr(sprPath, m_bodySprite)) {
                m_bodyFrame = 0;
                m_bodyTimer = 0.0f;
                m_lastJob = configJob;
                m_engine->calculateMaxCharHeight();
                return;
            }
        }
        ERUPTION_LOG_WARN("Loading body sprite from PNG: %s", configJob.c_str());
        if (m_engine->loadSpriteFromPng(configJob, m_bodySprite)) {
            m_bodyFrame = 0;
            m_bodyTimer = 0.0f;
            m_lastJob = configJob;
            m_engine->calculateMaxCharHeight();
            return;
        }
        ERUPTION_LOG_WARN("Failed to load PNG body sprite: %s, falling back to default.png", configJob.c_str());
    }

    if (m_engine->loadSpriteFromPng("assets/sprites/default.png", m_bodySprite)) {
        m_bodyFrame = 0;
        m_bodyTimer = 0.0f;
        m_lastJob = configJob;
        m_engine->calculateMaxCharHeight();
    }
}
void PlayerController::loadHairSprite() {
    m_engine->cleanupSpriteTextures(m_hairSprite);
    // Hair is not a separate asset anymore; mark as not loaded so the renderer
    // only draws the body/default sprite.
    m_hairSprite.loaded = false;
    m_lastHair = m_engine->charConfig().hair;
}
void PlayerController::updatePalettes() {
    if (!m_palettesInitialized) return;
    if (m_bodySprite.loaded && m_hairSprite.loaded) {
        if (memcmp(m_engine->charConfig().bodyPalette.colors, s_lastBodyPal.colors, 1024) != 0) {
            memcpy(s_lastBodyPal.colors, m_engine->charConfig().bodyPalette.colors, 1024);
            loadBodySprite();
        }
        if (memcmp(m_engine->charConfig().hairPalette.colors, s_lastHairPal.colors, 1024) != 0) {
            memcpy(s_lastHairPal.colors, m_engine->charConfig().hairPalette.colors, 1024);
            loadHairSprite();
        }
    }
}

Vec2 PlayerController::getSpriteScreenBase(const Camera& camera, float screenW, float screenH) const {
    if (!m_engine) return Vec2(-1.0f);

    const Engine::LoadedSprite& ls = m_bodySprite;
    if (!ls.loaded || ls.act.actions.empty()) return Vec2(-1.0f);


    // Use the CURRENT idle direction's offset so the widget tracks the sprite
    // horizontally (which varies with facing) without bobbing from walk frames.
    int direction = 0;
    if (!m_tacticalView) {
        Vec3 camForward = camera.forward();
        float camAngle = glm::degrees(std::atan2(camForward.x, camForward.z));
        float charAngle = glm::degrees(m_yaw);
        float roAngle = camAngle - charAngle + 180.0f;
        while (roAngle < 0.0f) roAngle += 360.0f;
        while (roAngle >= 360.0f) roAngle -= 360.0f;
        direction = static_cast<int>((roAngle + 22.5f) / 45.0f) % 8;
    }

    int actionIdx = direction;
    if (m_tacticalView) actionIdx = 4 * 8 + direction;
    if (actionIdx >= (int)ls.act.actions.size()) actionIdx %= 8;
    const auto& action = ls.act.actions[actionIdx];
    if (action.frames.empty()) return Vec2(-1.0f);
    const auto& frame = action.frames[0];
    if (frame.sprites.empty()) return Vec2(-1.0f);

    const auto& spr = frame.sprites[0];
    int texIdx = spr.index;
    if (spr.type == 1) texIdx += (int)ls.indexedTextureCount;
    if (texIdx < 0 || texIdx >= (int)ls.textures.size()) return Vec2(-1.0f);

    float centerY = m_spriteHeight * 0.4f;

    // Use the visual center of the sprite as the bind anchor, matching
    // Caldera PLAYER_POS (the sprite preview is drawn centered there, ignoring
    // per-direction ACT horizontal offsets so CSS offsets remain predictable).
    Vec3 anchorWorld = m_pos;
    anchorWorld.y += centerY;

    Vec4 clip = camera.viewProjNoJitter() * Vec4(anchorWorld, 1.0f);

    if (clip.w <= 0.0f) return Vec2(-1.0f);

    Vec3 ndc = Vec3(clip) / clip.w;
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
        return Vec2(-1.0f);

    return Vec2(
        (ndc.x * 0.5f + 0.5f) * screenW,
        (ndc.y * 0.5f + 0.5f) * screenH
    );
}
} 
