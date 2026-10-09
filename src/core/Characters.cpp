// Personagens da cena (NPC): sprite de 8 direções (PNG ou .spr do importador),
// posição e direção. Ficam parados ou andam até um ponto quando um script
// manda; o desenho segue as mesmas regras do sprite do jogador.
#include "core/Engine.hpp"

#include "core/Logger.hpp"
#include "game/PlayerController.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace eruption {

namespace {

constexpr float kPixelToWorld = 0.15f; // mesma escala do sprite do jogador

} // namespace

bool Engine::loadCharacterSprite(const std::string& png, LoadedSprite& out) {
    // O .spr do importador, quando existe ao lado do PNG, tem as animações.
    std::filesystem::path spr(png);
    spr.replace_extension(".spr");
    std::error_code ec;
    if (std::filesystem::exists(spr, ec) && loadSpriteFromEruptSpr(spr.string(), out)) return true;
    return loadSpriteFromPng(png, out);
}

int Engine::addCharacter(const std::string& name, const std::string& sprite, const Vec3& position, float facingDeg) {
    SceneCharacter c;
    c.name = name;
    c.sprite = sprite;
    c.position = position;
    c.facing = facingDeg;
    if (!loadCharacterSprite(sprite, c.look))
        ERUPTION_LOG_WARN("Character %s: could not load sprite %s", name.c_str(), sprite.c_str());
    m_characters.push_back(std::move(c));
    return static_cast<int>(m_characters.size() - 1);
}

bool Engine::setCharacterSprite(int index, const std::string& sprite) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return false;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    LoadedSprite look;
    if (!loadCharacterSprite(sprite, look)) return false;
    cleanupSpriteTextures(c.look);
    c.look = std::move(look);
    c.sprite = sprite;
    c.frame = 0;
    c.timer = 0.0f;
    return true;
}

int Engine::findCharacter(const std::string& name) const {
    for (size_t i = 0; i < m_characters.size(); ++i)
        if (m_characters[i].name == name && !m_characters[i].removed && !m_characters[i].parked) return static_cast<int>(i);
    return -1;
}

void Engine::walkCharacter(int index, const Vec3& target, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    if (!findPath(c.position, target, 4.0f, c.path)) {
        // Sem caminho (alvo cercado, longe demais): vai em linha reta.
        ERUPTION_LOG_WARN("Character %s: no path to (%.0f, %.0f); walking straight", c.name.c_str(), target.x, target.z);
        c.path = {c.position, target};
    }
    float length = 0.0f;
    for (size_t i = 1; i < c.path.size(); ++i) length += glm::length(Vec2(c.path[i].x - c.path[i - 1].x, c.path[i].z - c.path[i - 1].z));
    c.speed = seconds > 0.0f ? std::max(1.0f, length / seconds) : 60.0f;
    c.pathIndex = 1;
    c.walking = c.path.size() >= 2;
}

void Engine::setPlayerStart(const Vec3& p) {
    m_hasPlayerStart = true;
    m_playerStart = p;
    if (m_playerController) m_playerController->setPos(p);
    m_camera.setOrbitTarget(p + Vec3(0.0f, 7.2f, 0.0f));
}

void Engine::clearCharacters(bool keepNeighbors) {
    std::vector<SceneCharacter> kept;
    for (auto& c : m_characters) {
        if (keepNeighbors && c.fromNeighbor) kept.push_back(std::move(c));
        else cleanupSpriteTextures(c.look);
    }
    m_characters = std::move(kept);
}

void Engine::updateCharacters(float dt, bool moveAlongPaths) {
    for (size_t i = 0; i < m_characters.size(); ++i) {
        SceneCharacter& c = m_characters[i];
        if (c.parked || c.removed || !moveAlongPaths) continue;
        if (c.behavior == SceneCharacter::Behavior::Follow && m_playerController) {
            // Refaz o caminho de vez em quando, se o jogador se afastou.
            const Vec3 p = m_playerController->pos();
            const float dist = glm::length(Vec2(p.x - c.position.x, p.z - c.position.z));
            c.repath -= dt;
            if (dist <= c.followDistance) {
                if (c.walking) c.walking = false;
                c.facing = glm::degrees(std::atan2(p.x - c.position.x, p.z - c.position.z));
            } else if (c.repath <= 0.0f && (!c.walking || glm::length(p - c.followTarget) > 15.0f)) {
                const Vec3 to = p + glm::normalize(Vec3(c.position.x - p.x, 0.0f, c.position.z - p.z) + Vec3(1e-4f)) *
                                        (c.followDistance * 0.8f);
                c.followTarget = p;
                walkCharacter(static_cast<int>(i), to, 0.0f);
                c.speed = std::max(c.speed, 70.0f); // alcança o passo do jogador
                c.repath = 0.4f;
            }
        } else if (c.behavior == SceneCharacter::Behavior::Patrol && !c.patrol.empty() && !c.walking) {
            if (c.waitLeft > 0.0f) {
                c.waitLeft -= dt;
            } else {
                walkCharacter(static_cast<int>(i), c.patrol[c.patrolNext % c.patrol.size()], 0.0f);
                c.patrolNext = (c.patrolNext + 1) % c.patrol.size();
                c.waitLeft = c.patrolWait;
            }
        }
    }
    for (auto& c : m_characters) {
        if (c.parked) continue;
        if (moveAlongPaths && c.walking) {
            // Segue o caminho na velocidade dele; a altura é a do chão.
            float left = c.speed * dt;
            while (left > 0.0f && c.pathIndex < c.path.size()) {
                const Vec3 to = c.path[c.pathIndex];
                const Vec2 d(to.x - c.position.x, to.z - c.position.z);
                const float dist = glm::length(d);
                if (dist > 1e-3f) c.facing = glm::degrees(std::atan2(d.x, d.y));
                if (dist <= left) {
                    c.position = to;
                    left -= dist;
                    ++c.pathIndex;
                } else {
                    c.position += Vec3(d.x, 0.0f, d.y) * (left / dist);
                    left = 0.0f;
                }
            }
            c.position.y = sampleTerrainHeight(c.position.x, c.position.z);
            if (c.pathIndex >= c.path.size()) c.walking = false;
        }
        if (c.sayTime > 0.0f) c.sayTime = std::max(0.0f, c.sayTime - dt);
        if (c.visible && !c.removed && !hideCharactersInView) drawCharacter(c, dt);
    }
}

void Engine::drawCharacter(SceneCharacter& c, float dt) {
    if (!m_playerSpawnedOnActiveMap) return;
    LoadedSprite& ls = c.look;
    if (!ls.loaded || ls.act.actions.empty()) return;

    // Direção relativa à câmera, como no jogador (0 = de frente para ela).
    const Vec3 camForward = m_camera.forward();
    float rel = glm::degrees(std::atan2(camForward.x, camForward.z)) - c.facing + 180.0f;
    rel = std::fmod(std::fmod(rel, 360.0f) + 360.0f, 360.0f);
    const int direction = static_cast<int>((rel + 22.5f) / 45.0f) % 8;
    const bool moving = c.walking;
    int actionIdx = direction + (moving ? 8 : 0);
    // Animação extra (sprite v3) por cima do parado/andando.
    const LoadedSprite::Named* named = c.action.empty() ? nullptr : ls.findNamed(c.action);
    if (!c.action.empty() && !named) c.action.clear(); // o sprite não tem essa
    if (named) actionIdx = static_cast<int>(named->first) + direction;
    if (actionIdx >= static_cast<int>(ls.act.actions.size())) actionIdx %= 8;
    if (actionIdx >= static_cast<int>(ls.act.actions.size())) actionIdx = 0;
    const auto& action = ls.act.actions[static_cast<size_t>(actionIdx)];
    if (action.frames.empty()) return;
    if (c.frame >= static_cast<int>(action.frames.size())) c.frame = 0;
    const float delay = action.delay > 0 ? action.delay : 150.0f;
    c.timer += dt * 1000.0f;
    if (c.timer >= delay) {
        c.frame = (c.frame + 1) % static_cast<int>(action.frames.size());
        c.timer = 0.0f;
        // Uma vez só: ao voltar ao primeiro quadro, termina (fica no último até lá).
        if (named && c.frame == 0 && !c.actionLoop) {
            c.action.clear();
            c.frame = static_cast<int>(action.frames.size()) - 1;
        }
    }

    const Vec3 toCam = m_camera.position() - m_camera.target();
    const float spriteYaw = std::atan2(toCam.x, toCam.z);
    for (const auto& spr : action.frames[static_cast<size_t>(c.frame)].sprites) {
        int texIdx = spr.index + (spr.type == 1 ? static_cast<int>(ls.indexedTextureCount) : 0);
        if (texIdx < 0 || texIdx >= static_cast<int>(ls.textures.size())) continue;
        const auto& tex = ls.textures[static_cast<size_t>(texIdx)];
        const bool mirror = (spr.flags & 1) != 0;
        Sprite s;
        s.position = c.position;
        s.position.y += 2.0f;
        s.anchorPoint = c.position;
        const float w = static_cast<float>(tex.width) * kPixelToWorld * spr.scaleX;
        const float h = static_cast<float>(tex.height) * kPixelToWorld * spr.scaleY;
        float xOff = static_cast<float>(spr.offsetX);
        if (mirror) xOff = -xOff;
        // Base do quadro no chão (mesma regra dos sprites PNG do jogador).
        s.texRect = Vec4(xOff * kPixelToWorld, spr.offsetY * kPixelToWorld, spriteYaw, -h * 0.5f);
        s.size = Vec2(w, h);
        s.texIndex = tex.slot;
        s.paletteIndex = 0;
        s.flags = SpriteFlags::Billboard | SpriteFlags::CastShadow | SpriteFlags::PngSprite;
        if (mirror) s.flags = s.flags | SpriteFlags::FlipX;
        s.tintColor = spr.color;
        s.sortOrder = 0.0f;
        m_spriteSystem.submitSprite(s);
    }
}

} // namespace eruption

namespace eruption {

void Engine::sayPlayer(const std::string& text, float seconds) {
    m_playerSay = text;
    m_playerSayTime = text.empty() ? 0.0f : std::max(0.1f, seconds);
}

void Engine::drawSpeechBubbles(float x, float y, float w, float h, float dt) {
    if (m_playerSayTime > 0.0f) m_playerSayTime = std::max(0.0f, m_playerSayTime - dt);
    const Mat4 vp = m_camera.viewProjNoJitter();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto bubble = [&](const Vec3& head, const std::string& text, float remaining) {
        const Vec4 c = vp * Vec4(head, 1.0f);
        if (c.w <= 1e-3f) return;
        const ImVec2 s(x + (c.x / c.w * 0.5f + 0.5f) * w, y + (c.y / c.w * 0.5f + 0.5f) * h);
        if (s.x < x || s.y < y || s.x > x + w || s.y > y + h) return;
        const float alpha = std::min(1.0f, remaining / 0.25f);
        const float wrap = std::min(w * 0.4f, 280.0f);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
        const ImVec2 p0(s.x - ts.x * 0.5f - 10.0f, s.y - ts.y - 22.0f);
        const ImVec2 p1(s.x + ts.x * 0.5f + 10.0f, s.y - 8.0f);
        const ImU32 fill = IM_COL32(250, 248, 240, int(235 * alpha)), edge = IM_COL32(40, 40, 44, int(220 * alpha));
        dl->AddRectFilled(p0, p1, fill, 8.0f);
        dl->AddRect(p0, p1, edge, 8.0f, 0, 1.5f);
        // Rabicho apontando para quem fala.
        dl->AddTriangleFilled(ImVec2(s.x - 6, p1.y - 1), ImVec2(s.x + 6, p1.y - 1), ImVec2(s.x, s.y), fill);
        dl->AddLine(ImVec2(s.x - 6, p1.y), ImVec2(s.x, s.y), edge, 1.5f);
        dl->AddLine(ImVec2(s.x + 6, p1.y), ImVec2(s.x, s.y), edge, 1.5f);
        dl->AddText(nullptr, 0.0f, ImVec2(p0.x + 10.0f, p0.y + 7.0f), IM_COL32(25, 25, 28, int(255 * alpha)), text.c_str(),
                    nullptr, wrap);
    };
    for (const auto& c : m_characters)
        if (c.sayTime > 0.0f && c.visible && !c.removed && !c.parked && !hideCharactersInView)
            bubble(c.position + Vec3(0.0f, 30.0f, 0.0f), c.say, c.sayTime);
    if (m_playerSayTime > 0.0f && m_playerController)
        bubble(m_playerController->pos() + Vec3(0.0f, 30.0f, 0.0f), m_playerSay, m_playerSayTime);
}

void Engine::followPlayer(int index, float distance) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    c.behavior = SceneCharacter::Behavior::Follow;
    c.followDistance = std::max(5.0f, distance);
    c.repath = 0.0f;
}

void Engine::patrolCharacter(int index, const std::vector<Vec3>& points, float waitSeconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size()) || points.empty()) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    c.behavior = SceneCharacter::Behavior::Patrol;
    c.patrol = points;
    c.patrolNext = 0;
    c.patrolWait = std::max(0.0f, waitSeconds);
    c.waitLeft = 0.0f;
    c.walking = false;
}

void Engine::stopCharacter(int index) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    c.behavior = SceneCharacter::Behavior::None;
    c.walking = false;
    c.patrol.clear();
}

bool Engine::playPlayerAction(const std::string& name, bool loop) {
    if (!name.empty() && (!m_playerController || !m_playerController->bodySprite().findNamed(name))) return false;
    m_playerAction = name;
    m_playerActionLoop = loop;
    m_playerActionStarted = true;
    return true;
}

bool Engine::playCharacterAction(int index, const std::string& name, bool loop) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return false;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    if (!name.empty() && !c.look.findNamed(name)) return false;
    c.action = name;
    c.actionLoop = loop;
    c.frame = 0;
    c.timer = 0.0f;
    return true;
}

} // namespace eruption
