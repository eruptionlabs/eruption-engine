// Personagens da cena (NPC): sprite de 8 direções (PNG ou .spr do importador),
// posição e direção. Ficam parados ou andam até um ponto quando um script
// manda; o desenho segue as mesmas regras do sprite do jogador.
#include "core/Engine.hpp"

#include "core/Logger.hpp"
#include "core/Project.hpp"
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
    // Mesmo arquivo, mesma folha: devolve a que já está na GPU. Um elenco de
    // 15 personagens com dois sprites fazia 15 leituras e 15 envios, e um
    // mapa que criasse o elenco de uma vez estourava o relógio do script.
    auto cached = m_characterSpriteCache.find(png);
    if (cached != m_characterSpriteCache.end() && cached->second.loaded) {
        out = cached->second;
        return true;
    }

    // O .spr do importador, quando existe ao lado do PNG, tem as animações.
    LoadedSprite lido;
    std::filesystem::path spr = project::gameRef(png);
    spr.replace_extension(".spr");
    std::error_code ec;
    bool ok = std::filesystem::exists(spr, ec) && loadSpriteFromEruptSpr(spr.string(), lido);
    if (!ok) ok = loadSpriteFromPng(png, lido);
    if (!ok) return false;

    lido.texturesShared = std::make_shared<char>(0);
    m_characterSpriteCache[png] = lido;
    out = std::move(lido);
    return true;
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

// Velocidade em que a animacao de andar de uma folha e' tocada no ritmo dela
// (a mesma que walkCharacter usa quando ninguem pede um tempo).
static constexpr float kPassoNormal = 60.0f;

void Engine::walkCharacter(int index, const Vec3& target, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    if (!findPath(c.position, target, 4.0f, c.path)) {
        // Sem caminho (alvo cercado, longe demais): vai em linha reta.
        // Quando nem a origem nem o destino tem chao andavel (um palco montado
        // por script fora do mapa, por exemplo) nao existe caminho a procurar,
        // e a linha reta e' o unico comportamento que faz sentido. Ali nao e'
        // aviso: avisar a cada personagem que entra em cena so' afoga o log.
        const bool semChao = !navWalkable(c.position.x, c.position.z) && !navWalkable(target.x, target.z);
        if (!semChao)
            ERUPTION_LOG_WARN("Character %s: no path to (%.0f, %.0f); walking straight", c.name.c_str(), target.x, target.z);
        c.path = {c.position, target};
    }
    float length = 0.0f;
    for (size_t i = 1; i < c.path.size(); ++i) length += glm::length(Vec2(c.path[i].x - c.path[i - 1].x, c.path[i].z - c.path[i - 1].z));
    c.speed = seconds > 0.0f ? std::max(1.0f, length / seconds) : kPassoNormal;
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
    // O cache guarda uma referência a mais: solta depois das personagens, para
    // que a última a sair seja quem devolve os slots. Quem ficou (vizinho)
    // segue dono da folha dele.
    for (auto& [caminho, folha] : m_characterSpriteCache) {
        (void)caminho;
        cleanupSpriteTextures(folha);
    }
    m_characterSpriteCache.clear();
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
        tickCharacterFx(c, dt);
        if (c.visible && !c.removed && !hideCharactersInView) drawCharacter(c, dt);
    }
}

// Cada efeito e' um temporizador que desce; o desenho le' o estado. Nada de
// alocar, nada de RNG: o mesmo impacto sacode igual em qualquer maquina.
void Engine::tickCharacterFx(SceneCharacter& c, float dt) {
    SceneCharacter::Fx& f = c.fx;
    if (f.flashLeft > 0.0f) f.flashLeft = std::max(0.0f, f.flashLeft - dt);
    if (f.shakeLeft > 0.0f) {
        f.shakeLeft = std::max(0.0f, f.shakeLeft - dt);
        f.shakeTime += dt;
    }
    if (f.lungeLeft > 0.0f) f.lungeLeft = std::max(0.0f, f.lungeLeft - dt);
    if (f.popLeft > 0.0f) f.popLeft = std::max(0.0f, f.popLeft - dt);
    if (f.fadeLeft > 0.0f) {
        f.fadeLeft = std::max(0.0f, f.fadeLeft - dt);
        const float t = 1.0f - f.fadeLeft / std::max(1e-4f, f.fadeTotal);
        f.fade = f.fadeFrom + (f.fadeTo - f.fadeFrom) * t;
    }
}

void Engine::flashCharacter(int index, const Vec3& color, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter::Fx& f = m_characters[static_cast<size_t>(index)].fx;
    f.flashColor = color;
    f.flashTotal = f.flashLeft = std::max(0.0f, seconds);
}

void Engine::shakeCharacter(int index, float amplitude, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter::Fx& f = m_characters[static_cast<size_t>(index)].fx;
    f.shakeAmp = std::max(0.0f, amplitude);
    f.shakeTotal = f.shakeLeft = std::max(0.0f, seconds);
    f.shakeTime = 0.0f;
}

void Engine::lungeCharacter(int index, const Vec3& towards, float distance, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    const Vec3 d = towards - c.position;
    const float len = glm::length(Vec2(d.x, d.z));
    c.fx.lungeDir = len > 1e-3f ? Vec3(d.x / len, 0.0f, d.z / len) : Vec3(0.0f);
    c.fx.lungeDist = distance;
    c.fx.lungeTotal = c.fx.lungeLeft = std::max(1e-3f, seconds);
}

void Engine::popCharacter(int index, float amount, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter::Fx& f = m_characters[static_cast<size_t>(index)].fx;
    f.popScale = amount;
    f.popTotal = f.popLeft = std::max(1e-3f, seconds);
}

void Engine::fadeCharacter(int index, float to, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter::Fx& f = m_characters[static_cast<size_t>(index)].fx;
    f.fadeFrom = f.fade;
    f.fadeTo = glm::clamp(to, 0.0f, 1.0f);
    f.fadeTotal = f.fadeLeft = std::max(0.0f, seconds);
    if (f.fadeTotal <= 0.0f) f.fade = f.fadeTo;
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
    float delay = action.delay > 0 ? action.delay : 150.0f;
    // O CICLO DE PASSOS ACOMPANHA A VELOCIDADE. A folha diz o ritmo da
    // caminhada no passo normal (o mesmo de walkCharacter sem tempo pedido);
    // quem anda mais rapido que isso - um script que manda atravessar um
    // palco em meio segundo - tocava os mesmos 75 ms por quadro e PATINAVA:
    // o corpo andava cinco larguras com dois passos. Correndo, os passos
    // ficam mais curtos no tempo; devagar, mais longos (com teto, para um
    // passo lento nao virar camera lenta).
    if (moving && !named && c.speed > 0.0f)
        delay *= std::clamp(kPassoNormal / c.speed, 0.25f, 2.0f);
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

    // SUCO: deslocamento, clarao, estica e opacidade deste quadro.
    const SceneCharacter::Fx& f = c.fx;
    Vec3 fxOffset(0.0f);
    if (f.shakeLeft > 0.0f) {
        const float decay = f.shakeLeft / std::max(1e-4f, f.shakeTotal);
        const float a = f.shakeAmp * decay * decay;
        fxOffset.x += std::sin(f.shakeTime * 61.0f) * a;
        fxOffset.y += std::sin(f.shakeTime * 83.0f + 1.3f) * a * 0.5f;
    }
    if (f.lungeLeft > 0.0f) {
        // Vai no primeiro terco e volta no resto (investida, nao passo).
        const float t = 1.0f - f.lungeLeft / std::max(1e-4f, f.lungeTotal);
        const float k = t < 0.34f ? t / 0.34f : 1.0f - (t - 0.34f) / 0.66f;
        fxOffset += f.lungeDir * (f.lungeDist * k);
    }
    float fxScale = 1.0f;
    if (f.popLeft > 0.0f) {
        const float t = 1.0f - f.popLeft / std::max(1e-4f, f.popTotal);
        fxScale += f.popScale * std::sin(t * 3.14159265f);
    }
    const float flash = f.flashLeft > 0.0f ? f.flashLeft / std::max(1e-4f, f.flashTotal) : 0.0f;

    for (const auto& spr : action.frames[static_cast<size_t>(c.frame)].sprites) {
        int texIdx = spr.index + (spr.type == 1 ? static_cast<int>(ls.indexedTextureCount) : 0);
        if (texIdx < 0 || texIdx >= static_cast<int>(ls.textures.size())) continue;
        const auto& tex = ls.textures[static_cast<size_t>(texIdx)];
        const bool mirror = (spr.flags & 1) != 0;
        Sprite s;
        s.position = c.position + fxOffset;
        s.position.y += 2.0f;
        s.anchorPoint = c.position + fxOffset;
        const float w = static_cast<float>(tex.width) * kPixelToWorld * spr.scaleX * fxScale;
        const float h = static_cast<float>(tex.height) * kPixelToWorld * spr.scaleY * fxScale;
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
        // Clarao: puxa a cor na direcao da cor que o script pediu, sem
        // estourar o alfa; opacidade entra no alfa.
        if (flash > 0.0f) {
            s.tintColor = Vec4(glm::mix(Vec3(s.tintColor), f.flashColor, flash), s.tintColor.a);
        }
        s.tintColor.a *= f.fade;
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

void Engine::drawGameFx2D(float x, float y, float w, float h, float dt) {
    // Desenho 2D do jogo por cima da cena. O canvas e' do jogo (o script
    // monta os nos); o motor so' atualiza os tempos e desenha.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // O documento em CSS vem primeiro (e' a interface); os numeros e o fade
    // do UiCanvas vem por cima, porque sao efeito e tem que cobrir tudo.
    // Liga o carregador de imagem uma vez. Fica aqui, e nao na construcao,
    // porque so' depois do Vulkan de pe' faz sentido subir textura.
    static bool resolvedorLigado = false;
    if (!resolvedorLigado) {
        resolvedorLigado = true;
        m_uiDocument.setImageResolver([this](const std::string& png) { return uiImageTexture(png); });
    }
    m_uiDocument.reloadIfChanged();
    m_uiDocument.draw(dl, x, y, w, h);
    m_uiCanvas.update(dt);
    m_uiCanvas.draw(dl, x, y, w, h, m_camera.viewProjNoJitter());
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
