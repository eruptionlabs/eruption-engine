// Personagens da cena (NPC): sprite de 8 direções (PNG ou .spr do importador),
// posição e direção. Ficam parados ou andam até um ponto quando um script
// manda; o desenho segue as mesmas regras do sprite do jogador.
#include <chrono>
#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"

#include "core/Logger.hpp"
#include "core/Platform.hpp"
#include "core/Project.hpp"
#include "game/PlayerController.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include "core/Input.hpp"

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
        m_characterSpriteUse[png] = m_characterSpriteGen;
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
    m_characterSpriteUse[png] = m_characterSpriteGen;
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

// Velocidade padrao de walkCharacter quando ninguem pede um tempo.
static constexpr float kPassoNormal = 60.0f;
// Ritmo em que a animacao de andar de uma folha casa com o chao: ~1,1 altura
// de corpo por segundo (o passo normal de 60 num boneco de celula ~55, a
// escala em que o elenco foi desenhado).
static constexpr float kAlturasPorSegundo = 1.1f;
// `ground = true` procura o piso a partir de um degrau acima do Y da cena
// (o mesmo kStep de GroundQuery.cpp: o que se sobe andando).
static constexpr float kDegrauAoAssentar = 4.0f;
// set_position com andar: o andar segura por esta folga depois da última
// mudança de posição (um script que move a 30 Hz num quadro de 60 Hz não pisca
// entre parado e andando).
static constexpr float kFolgaDoAndar = 0.15f;
// Salto maior que isto, em alturas de corpo, num quadro é teleporte.
static constexpr float kTeleporteEmAlturas = 2.0f;

// Altura do corpo pela célula do primeiro quadro parado (de frente), na
// escala em que vai à tela; sem folha carregada, a de um boneco comum.
static float alturaDoCorpo(const Engine::LoadedSprite& ls) {
    if (ls.loaded && !ls.act.actions.empty() && !ls.act.actions.front().frames.empty()) {
        const auto& camadas = ls.act.actions.front().frames.front().sprites;
        if (!camadas.empty()) {
            const auto& spr = camadas.front();
            const int idx = spr.index + (spr.type == 1 ? static_cast<int>(ls.indexedTextureCount) : 0);
            if (idx >= 0 && idx < static_cast<int>(ls.textures.size()))
                return static_cast<float>(ls.textures[static_cast<size_t>(idx)].height) * kPixelToWorld * spr.scaleY;
        }
    }
    return kPassoNormal / kAlturasPorSegundo;
}

void Engine::placeCharacter(int index, const Vec3& position, bool walk) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    const float passo = glm::length(Vec2(position.x - c.position.x, position.z - c.position.z));
    c.position = position;
    c.walking = false;
    if (!walk || passo > kTeleporteEmAlturas * alturaDoCorpo(c.look)) {
        c.scriptStep = 0.0f;
        c.scriptWalkLeft = 0.0f;
        return;
    }
    c.scriptStep += passo;
}

void Engine::walkCharacter(int index, const Vec3& target, float seconds) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    if (!findPath(c.position, target, 4.0f, c.path)) {
        // Sem caminho (alvo cercado, longe demais): vai em linha reta.
        // Quando nem a origem nem o destino tem chao andavel (um palco montado
        // por script fora do mapa, por exemplo) nao existe caminho a procurar,
        // e a linha reta e' o unico comportamento que faz sentido. Ali nao e'
        // aviso: avisar a cada personagem que entra em cena so' afoga o log.
        const bool semChao = !navWalkable(c.position.x, c.position.z, c.position.y) &&
                             !navWalkable(target.x, target.z, target.y);
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

void Engine::setPlayerStart(const Vec3& start) {
    // No chao, como o spawn da carga (MapLoad): o Y da cena e' o do relevo de
    // quando o ponto foi marcado. Com o relevo regravado depois, o jogador
    // nascia 4 u acima do chao e so' descia no primeiro passo - a sombra dele
    // comecava longe do pe'.
    Vec3 p = start;
    if (m_currentMap && m_currentMap->isGlbMap) p.y = clampSpawnYToWater(*m_currentMap, sampleTerrainHeight(p.x, p.z));
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
    // A FOLHA FICA entre mapas: o elenco que volta (o mesmo grupo em dois
    // andares) nao relê o PNG nem reenvia a textura - eram 150 a 290 ms por
    // folha na chegada ao mapa seguinte, ~1,5 s com 9 folhas. So' sai a folha
    // sem uso ha' duas trocas (como a expiracao do cache de assets).
    ++m_characterSpriteGen;
    releaseCharacterSpriteCache(2);
}

void Engine::releaseCharacterSpriteCache(uint32_t keepGenerations) {
    // O cache guarda uma referência a mais: solta depois das personagens, para
    // que a última a sair seja quem devolve os slots. Quem ficou (vizinho)
    // segue dono da folha dele.
    for (auto it = m_characterSpriteCache.begin(); it != m_characterSpriteCache.end();) {
        const uint32_t used = m_characterSpriteUse[it->first];
        if (keepGenerations > 0 && m_characterSpriteGen - used < keepGenerations) {
            ++it;
            continue;
        }
        cleanupSpriteTextures(it->second);
        m_characterSpriteUse.erase(it->first);
        it = m_characterSpriteCache.erase(it);
    }
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
        if (c.groundPending) {
            // Procura o piso a partir de um degrau acima do Y escrito: o
            // terreno ou o topo de uma peça sólida, o mesmo chão de quem
            // anda. Um Y 0 debaixo de uma montanha cai no relevo.
            c.position.y = floorHeight(c.position.x, c.position.z, c.position.y + kDegrauAoAssentar);
            c.groundPending = false;
        }
        if (c.scriptStep > 1e-3f && dt > 0.0f) {
            // O script moveu com andar desde o último tick: a velocidade é a
            // medida, suavizada para um quadro torto não sacudir o ritmo.
            const float medida = c.scriptStep / dt;
            c.scriptSpeed = c.scriptWalkLeft > 0.0f ? glm::mix(c.scriptSpeed, medida, 0.3f) : medida;
            c.scriptWalkLeft = kFolgaDoAndar;
        } else if (c.scriptWalkLeft > 0.0f) {
            c.scriptWalkLeft = std::max(0.0f, c.scriptWalkLeft - dt);
        }
        c.scriptStep = 0.0f;
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

void Engine::scaleCharacter(int index, float scale) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    m_characters[static_cast<size_t>(index)].fx.scale = std::max(0.05f, scale);
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
    const bool scripted = !c.walking && c.scriptWalkLeft > 0.0f;
    const bool moving = c.walking || scripted;
    const float speed = scripted ? c.scriptSpeed : c.speed;
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
    // caminhada no passo natural do boneco; quem anda mais rapido que isso - um script que manda atravessar um
    // palco em meio segundo - tocava os mesmos 75 ms por quadro e PATINAVA:
    // o corpo andava cinco larguras com dois passos. Correndo, os passos
    // ficam mais curtos no tempo; devagar, mais longos (com teto, para um
    // passo lento nao virar camera lenta).
    // A referencia e' em ALTURAS DE CORPO por segundo, nao em unidades: o
    // passo de um boneco de 20 unidades e' menos da metade do de um de 45, e
    // com uma referencia fixa a caminhada natural do menor saia em camera
    // lenta. A altura e' a da celula do quadro atual, como ela vai a tela.
    if (moving && !named && speed > 0.0f) {
        float altura = 0.0f;
        const auto& camadas = action.frames[static_cast<size_t>(c.frame)].sprites;
        if (!camadas.empty()) {
            const auto& spr = camadas.front();
            const int idx = spr.index + (spr.type == 1 ? static_cast<int>(ls.indexedTextureCount) : 0);
            if (idx >= 0 && idx < static_cast<int>(ls.textures.size()))
                altura = static_cast<float>(ls.textures[static_cast<size_t>(idx)].height) * kPixelToWorld * spr.scaleY;
        }
        const float passo = altura > 1.0f ? kAlturasPorSegundo * altura : kPassoNormal;
        delay *= std::clamp(passo / speed, 0.25f, 2.0f);
    }
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
    float fxScale = f.scale;
    if (f.popLeft > 0.0f) {
        const float t = 1.0f - f.popLeft / std::max(1e-4f, f.popTotal);
        fxScale += f.popScale * std::sin(t * 3.14159265f);
    }
    const float flash = f.flashLeft > 0.0f ? f.flashLeft / std::max(1e-4f, f.flashTotal) : 0.0f;
    // Topo do quadro (ui.anchor, characters.head_screen): a parte mais alta.
    // No sprite_vert.glsl o topo da parte e' -yOff + 0,5 h - centerY, com
    // centerY = -h/2.
    float headTop = -1e9f, headSide = 0.0f;

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
        if (-s.texRect.y + h > headTop) {
            headTop = -s.texRect.y + h;
            headSide = s.texRect.x;
        }
        s.texIndex = tex.slot;
        s.paletteIndex = 0;
        s.flags = SpriteFlags::Billboard | SpriteFlags::CastShadow | SpriteFlags::PngSprite | SpriteFlags::TeamVision;
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
    if (headTop > -1e8f) {
        c.head = Vec2(headSide, headTop);
        c.headAnchor = c.position + fxOffset;
        c.headFrame = m_framesCount;
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
        // Letra pela altura da tela (3,2%), nunca menor que a do ImGui: em
        // pixels fixos o balao saia ilegivel na tela de celular (1080 linhas
        // a ~400 ppp). Margens e largura crescem na mesma proporcao.
        ImFont* font = ImGui::GetFont();
        const float size = std::max(ImGui::GetFontSize(), h * 0.032f);
        const float k = size / std::max(1.0f, ImGui::GetFontSize());
        const float wrap = std::min(w * 0.4f, 280.0f * k);
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
        const float pad = 10.0f * k;
        const ImVec2 p0(s.x - ts.x * 0.5f - pad, s.y - ts.y - 14.0f * k - 8.0f);
        const ImVec2 p1(s.x + ts.x * 0.5f + pad, s.y - 8.0f);
        const ImU32 fill = IM_COL32(250, 248, 240, int(235 * alpha)), edge = IM_COL32(40, 40, 44, int(220 * alpha));
        dl->AddRectFilled(p0, p1, fill, 8.0f);
        dl->AddRect(p0, p1, edge, 8.0f, 0, 1.5f);
        // Rabicho apontando para quem fala.
        dl->AddTriangleFilled(ImVec2(s.x - 6, p1.y - 1), ImVec2(s.x + 6, p1.y - 1), ImVec2(s.x, s.y), fill);
        dl->AddLine(ImVec2(s.x - 6, p1.y), ImVec2(s.x, s.y), edge, 1.5f);
        dl->AddLine(ImVec2(s.x + 6, p1.y), ImVec2(s.x, s.y), edge, 1.5f);
        dl->AddText(font, size, ImVec2(p0.x + pad, p0.y + 7.0f * k), IM_COL32(25, 25, 28, int(255 * alpha)), text.c_str(),
                    nullptr, wrap);
    };
    for (const auto& c : m_characters)
        if (c.sayTime > 0.0f && c.visible && !c.removed && !c.parked && !hideCharactersInView &&
            m_vision.visible(Vec2(c.position.x, c.position.z)))
            bubble(c.position + Vec3(0.0f, 30.0f, 0.0f), c.say, c.sayTime);
    if (m_playerSayTime > 0.0f && m_playerController)
        bubble(m_playerController->pos() + Vec3(0.0f, 30.0f, 0.0f), m_playerSay, m_playerSayTime);
}

bool Engine::characterHeadScreen(int index, Vec2& out) const {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return false;
    const SceneCharacter& c = m_characters[static_cast<size_t>(index)];
    if (c.headFrame + 1 < m_framesCount || m_uiArea.z <= 0.0f) return false;
    // A mesma conta do sprite_vert.glsl: no espaco de visao, direita = -x e
    // o "cima" inclinado pela spline do billboard (frameUbo.spriteTilt).
    const float pitchNorm = glm::degrees(m_camera.orbitPitch()) / 89.0f;
    const float tilt = glm::radians(m_charConfig.billboardTiltSpline.evaluate(pitchNorm));
    Vec4 v = m_camera.viewMatrix() * Vec4(c.headAnchor, 1.0f);
    v += Vec4(-c.head.x, c.head.y * std::cos(tilt), -c.head.y * std::sin(tilt), 0.0f);
    const Vec4 clip = m_camera.projNoJitter() * v;
    if (clip.w <= 1e-3f) return false;
    out = Vec2(m_uiArea.x + (clip.x / clip.w * 0.5f + 0.5f) * m_uiArea.z,
               m_uiArea.y + (clip.y / clip.w * 0.5f + 0.5f) * m_uiArea.w);
    return out.x >= m_uiArea.x && out.x <= m_uiArea.x + m_uiArea.z &&
           out.y >= m_uiArea.y && out.y <= m_uiArea.y + m_uiArea.w;
}

void Engine::setUiAnchor(const std::string& selector, const std::string& character, float dyPx) {
    for (UiAnchor& a : m_uiAnchors) {
        if (a.selector != selector) continue;
        a.character = character;
        a.dyPx = dyPx;
        return;
    }
    m_uiAnchors.push_back({selector, character, dyPx});
}

void Engine::clearUiAnchor(const std::string& selector) {
    for (size_t i = 0; i < m_uiAnchors.size(); ++i) {
        if (m_uiAnchors[i].selector != selector) continue;
        m_uiAnchors.erase(m_uiAnchors.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    m_uiDocument.clearAnchor(selector);
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
    m_uiArea = Vec4(x, y, w, h);
    // Ancoras: o ponto sai da camera deste quadro, entao a barra nao fica um
    // quadro atras do boneco ao girar a camera.
    for (const UiAnchor& a : m_uiAnchors) {
        Vec2 p(0.0f);
        const bool on = characterHeadScreen(findCharacter(a.character), p);
        m_uiDocument.setAnchor(a.selector, p.x - x, p.y - y - a.dyPx, on);
    }
    m_uiDocument.draw(dl, x, y, w, h);
    m_uiCanvas.update(dt);
    m_uiCanvas.draw(dl, x, y, w, h, m_camera.viewProjNoJitter());
}

// O cursor vem da janela; a interface desenha em coordenadas do ImGui (no
// Android o swapchain pode ser menor que a tela).
glm::vec2 Engine::uiCursor() const {
    int winW = 0, winH = 0;
    glfwGetWindowSize(m_window.handle(), &winW, &winH);
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 m = Input::mousePosition();
    return {winW > 0 ? m.x * io.DisplaySize.x / float(winW) : m.x,
            winH > 0 ? m.y * io.DisplaySize.y / float(winH) : m.y};
}

bool Engine::uiTouchRect(const std::string& selector, CSSRect& r) {
    if (selector == "minimap") {
        if (m_minimapScreenFrame + 2 < m_framesCount) return false;
        r = CSSRect{m_minimapScreen.x, m_minimapScreen.y, m_minimapScreen.z - m_minimapScreen.x,
                    m_minimapScreen.w - m_minimapScreen.y};
    } else if (selector.size() > 6 && selector.compare(selector.size() - 6, 6, ":badge") == 0) {
        // Selo: a area de toque, de pelo menos 48 dp (guia de toque do Android).
        const std::string base = selector.substr(0, selector.size() - 6);
        if (!m_uiDocument.shown(base) || m_uiArea.z <= 0.0f) return false;
        CSSRect visual;
        if (!m_uiDocument.badgeRects(base, m_uiArea.x, m_uiArea.y, m_uiArea.z, m_uiArea.w,
                                     48.0f * platform::pixelsPerDp(), visual, r)) return false;
    } else {
        if (!m_uiDocument.shown(selector) || m_uiArea.z <= 0.0f) return false;
        if (!m_uiDocument.rectOf(selector, m_uiArea.x, m_uiArea.y, m_uiArea.z, m_uiArea.w, r)) return false;
    }
    Input::addCssTouchRect(r.x, r.y, r.x + r.w, r.y + r.h);
    return true;
}

static bool dentro(const CSSRect& r, const glm::vec2& p) {
    return p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
}

bool Engine::uiHit(const std::string& selector, const CSSRect& r, const glm::vec2& p) const {
    if (!dentro(r, p)) return false;
    if (selector.size() > 6 && selector.compare(selector.size() - 6, 6, ":badge") == 0) return true;
    // Toque que comeca na area de um selo e' so' do selo: do proprio
    // elemento ou de outro desenhado por cima dele (o "+" sobre um botao).
    return !m_uiDocument.badgeTouchAt(m_uiArea.x, m_uiArea.y, m_uiArea.z, m_uiArea.w,
                                      48.0f * platform::pixelsPerDp(), Vec2(p.x, p.y));
}

bool Engine::uiClicked(const std::string& selector) {
    // O retangulo sai antes do teste do clique: publica a area tocavel a cada
    // quadro, com ou sem toque.
    CSSRect r;
    if (!uiTouchRect(selector, r)) return false;
    if (!Input::isMousePressed(MouseButton::Left)) return false;
    const glm::vec2 m = Input::mousePosition();
    if (Input::inUiTouchRect(m.x, m.y)) return false; // analogico do controle de toque
    return uiHit(selector, r, uiCursor());
}

std::string Engine::uiGraphClicked(const std::string& selector) {
    CSSRect r;
    if (!uiTouchRect(selector, r)) return {};
    if (!Input::isMousePressed(MouseButton::Left)) return {};
    const glm::vec2 m = Input::mousePosition();
    if (Input::inUiTouchRect(m.x, m.y)) return {};
    const glm::vec2 c = uiCursor();
    std::string id;
    m_uiDocument.graphNodeAt(selector, m_uiArea.x, m_uiArea.y, m_uiArea.z, m_uiArea.w,
                             48.0f * platform::pixelsPerDp(), Vec2(c.x, c.y), id);
    return id;
}

std::string Engine::uiGraphHeld(const std::string& selector, float seconds) {
    updateUiHold();
    CSSRect r;
    // Folga de 0,1 ms: a soma dos passos de 1/60 s fica um fio abaixo de 0,5.
    if (!uiTouchRect(selector, r) || !m_uiHold.active || m_uiHold.t1 - m_uiHold.t0 + 1e-4 < seconds) return {};
    std::string id;
    m_uiDocument.graphNodeAt(selector, m_uiArea.x, m_uiArea.y, m_uiArea.z, m_uiArea.w,
                             48.0f * platform::pixelsPerDp(), Vec2(m_uiHold.start.x, m_uiHold.start.y), id);
    return id;
}

void Engine::updateUiHold() {
    UiHold& h = m_uiHold;
    if (h.frame == m_framesCount) return; // uma vez por quadro, por mais chamadas que haja
    h.frame = m_framesCount;
    // O dedo da camera aperta o botao direito (core/TouchRouter); o mouse, o esquerdo.
    const MouseButton kDedo = Input::touchScreen() ? MouseButton::Right : MouseButton::Left;
    const double agora = m_uiClock;
    h.released = false;
    if (Input::isMousePressed(kDedo)) {
        const glm::vec2 m = Input::mousePosition();
        h.active = !Input::inUiTouchRect(m.x, m.y);
        h.start = h.end = uiCursor();
        h.t0 = h.t1 = agora;
    } else if (h.active) {
        h.end = uiCursor();
        h.t1 = agora;
        if (!Input::isMouseDown(kDedo)) {
            h.active = false;
            h.released = true;
        }
    }
}

float Engine::uiHeld(const std::string& selector) {
    updateUiHold();
    CSSRect r;
    if (!uiTouchRect(selector, r) || !m_uiHold.active || !uiHit(selector, r, m_uiHold.start)) return 0.0f;
    return static_cast<float>(m_uiHold.t1 - m_uiHold.t0);
}

bool Engine::uiReleased(const std::string& selector) {
    updateUiHold();
    CSSRect r;
    if (!uiTouchRect(selector, r) || !m_uiHold.released) return false;
    return uiHit(selector, r, m_uiHold.start) && dentro(r, m_uiHold.end);
}

glm::vec2 Engine::uiDragged(const std::string& selector) {
    UiDrag& d = m_uiDrag;
    if (d.frame != m_framesCount) { // uma vez por quadro, por mais chamadas que haja
        d.frame = m_framesCount;
        const glm::vec2 c = uiCursor();
        const glm::vec2 m = Input::mousePosition();
        d.delta = glm::vec2(0.0f);
        // No celular o dedo que arrasta chega como botao direito (o esquerdo e'
        // so' o toque curto, ao soltar); no PC arrasta-se com o esquerdo.
        const bool pressed = Input::isMousePressed(MouseButton::Left) || Input::isMousePressed(MouseButton::Right);
        const bool down = Input::isMouseDown(MouseButton::Left) || Input::isMouseDown(MouseButton::Right);
        if (pressed && !Input::inUiTouchRect(m.x, m.y)) {
            d = UiDrag{c, c, glm::vec2(0.0f), true, false, m_framesCount};
        } else if (d.active && down) {
            // Folga de ~1,5% da altura da tela: o dedo treme num toque parado.
            const float slop = ImGui::GetIO().DisplaySize.y * 0.015f;
            if (!d.moving && glm::length(c - d.start) > slop) { d.moving = true; d.last = d.start; }
            if (d.moving) d.delta = c - d.last;
            d.last = c;
        } else {
            d.active = false;
        }
    }
    CSSRect r;
    if (!uiTouchRect(selector, r)) return glm::vec2(0.0f);
    if (!d.moving || (d.delta.x == 0.0f && d.delta.y == 0.0f)) return glm::vec2(0.0f);
    return uiHit(selector, r, d.start) ? d.delta : glm::vec2(0.0f);
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
