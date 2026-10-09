// Personagens da cena (NPC): sprite de 8 direções (PNG ou .spr do importador),
// posição e direção. Ficam parados ou andam até um ponto quando um script
// manda; o desenho segue as mesmas regras do sprite do jogador.
#include "core/Engine.hpp"

#include "core/Logger.hpp"
#include "game/PlayerController.hpp"

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
        if (m_characters[i].name == name && !m_characters[i].removed) return static_cast<int>(i);
    return -1;
}

void Engine::walkCharacter(int index, const Vec3& target, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    c.walkFrom = c.position;
    c.walkTo = target;
    c.walkTime = 0.0f;
    c.walkDuration = std::max(0.01f, seconds);
    c.walking = true;
}

void Engine::setPlayerStart(const Vec3& p) {
    m_hasPlayerStart = true;
    m_playerStart = p;
    if (m_playerController) m_playerController->setPos(p);
    m_camera.setOrbitTarget(p + Vec3(0.0f, 7.2f, 0.0f));
}

void Engine::clearCharacters() {
    for (auto& c : m_characters) cleanupSpriteTextures(c.look);
    m_characters.clear();
}

void Engine::updateCharacters(float dt, bool moveAlongPaths) {
    for (auto& c : m_characters) {
        if (moveAlongPaths && c.walking) {
            c.walkTime += dt;
            const float t = std::min(1.0f, c.walkTime / c.walkDuration);
            const Vec3 d = c.walkTo - c.walkFrom;
            if (std::abs(d.x) + std::abs(d.z) > 1e-3f) c.facing = glm::degrees(std::atan2(d.x, d.z));
            c.position = c.walkFrom + d * t;
            if (t >= 1.0f) c.walking = false;
        }
        if (c.visible && !c.removed) drawCharacter(c, dt);
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
