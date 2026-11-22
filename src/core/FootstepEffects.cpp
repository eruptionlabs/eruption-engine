// Pecas genericas dos efeitos de passo (ver FootstepEffects.hpp): o chao sob
// o pe' (material e UV), e a poeira e a marca no chao tocadas com os
// parametros que o script da'.
#include "core/FootstepEffects.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "renderer/vfx/VfxSystem.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace eruption {

// ---------------------------------------------------------------- Engine

// Chao sob o pe': a instancia de piso (mapas de kit) e as malhas de chao do
// mapa; vale o triangulo cuja altura fica mais perto do pe'. O raio sai um
// pouco acima do pe' (kAcima) e aceita desnivel de kTolera.
bool Engine::footSurfaceAt(const Vec3& pos, ModelRenderer::SurfaceHit& best) {
    constexpr float kAcima = 10.0f, kTolera = 12.0f;
    const auto& insts = m_modelRenderer.getInstances();
    const auto& meshes = m_modelRenderer.getMeshes();
    if (m_footsteps.groundEpoch != m_modelRenderer.instanceEpoch()) {
        m_footsteps.groundEpoch = m_modelRenderer.instanceEpoch();
        m_footsteps.ground.clear();
        for (uint32_t i = 0; i < insts.size(); ++i)
            if (insts[i].meshIndex < meshes.size() && meshes[insts[i].meshIndex].isGround)
                m_footsteps.ground.push_back(i);
    }
    const Vec3 origin(pos.x, pos.y + kAcima, pos.z), down(0.0f, -1.0f, 0.0f);
    float bestDy = kTolera;
    bool found = false;
    auto tryInstance = [&](uint32_t i) {
        const ModelInstance& inst = insts[i];
        if (!inst.enabled || pos.x < inst.worldAabbMin.x || pos.x > inst.worldAabbMax.x ||
            pos.z < inst.worldAabbMin.z || pos.z > inst.worldAabbMax.z || inst.worldAabbMin.y > origin.y)
            return;
        ModelRenderer::SurfaceHit h;
        if (!m_modelRenderer.surfaceTextureAt(i, origin, down, h)) return;
        const float dy = std::fabs(origin.y - h.t - pos.y);
        if (dy > bestDy) return;
        bestDy = dy;
        best = std::move(h);
        found = true;
    };
    int floorInst = -1;
    bool onModel = false;
    floorHeight(pos.x, pos.z, pos.y + kAcima, &onModel, &floorInst, false);
    if (floorInst >= 0) tryInstance(uint32_t(floorInst));
    for (uint32_t i : m_footsteps.ground)
        if (int(i) != floorInst) tryInstance(i);
    return found;
}

namespace {

VfxGradient::Key gkey(float t, const Vec3& c, float a) {
    VfxGradient::Key k;
    k.t = t;
    k.color = Vec4(c, a);
    return k;
}

VfxEffectDesc buildDust(const Engine::DustParams& d, const std::string& name) {
    VfxEmitterDesc e;
    e.name = "poeira";
    e.rate = 0.0f;
    const uint32_t n = uint32_t(std::clamp(d.count, 1.0f, 64.0f));
    e.burstCount = n;
    e.capacity = n * 2u + 8u;
    e.shape = VfxShape::Disc;
    e.shapeSize = Vec3(0.06f);
    e.offset = Vec3(0.0f, 0.03f, 0.0f);
    e.lifetime = Vec2(d.life * 0.6f, d.life);
    e.speed = Vec2(0.03f, 0.2f);
    e.direction = Vec3(0.0f, 1.0f, 0.0f);
    e.spread = 0.9f;
    e.size = Vec2(d.size * 0.35f, d.size * 0.6f);
    e.sizeOverLife.keys = {Vec2(0.0f, 0.6f), Vec2(1.0f, 1.6f)};
    e.rotation = Vec2(0.0f, 360.0f);
    e.angularVelocity = Vec2(-40.0f, 40.0f);
    e.gravity = Vec3(0.0f, d.rise, 0.0f);
    e.drag = 1.2f;
    e.color.keys = {gkey(0.0f, d.color, 0.0f), gkey(0.12f, d.color, d.alpha), gkey(1.0f, d.color, 0.0f)};
    e.texture = "builtin:puff";
    e.flipCols = e.flipRows = 4;
    e.flipMode = VfxFlipMode::Random;
    e.flipBlend = false;
    e.blend = VfxBlend::Alpha;
    e.softDistance = 0.15f;
    e.nearFade = 0.2f;
    e.lit = 0.8f;
    VfxEffectDesc fx;
    fx.name = name;
    fx.duration = 0.1f;
    fx.loop = false;
    fx.emitters.push_back(e);
    return fx;
}

// Marca deitada e projetada no chao (decal do VFX): uma particula, parada, que
// some suave no fim da vida. `yawDeg` gira a ponta da textura para a direcao
// do andar.
VfxEffectDesc buildPrint(const Engine::FootprintParams& p, float yawDeg, const std::string& name) {
    VfxEmitterDesc e;
    e.name = "marca";
    e.rate = 0.0f;
    e.burstCount = 1;
    e.capacity = 4;
    e.shape = VfxShape::Point;
    e.lifetime = Vec2(p.life, p.life);
    e.speed = Vec2(0.0f, 0.0f);
    e.size = Vec2(p.size * 0.5f, p.size * 0.5f); // metade do lado do quadro
    e.rotation = Vec2(yawDeg, yawDeg);
    e.color.keys = {gkey(0.0f, p.color, p.alpha), gkey(0.7f, p.color, p.alpha), gkey(1.0f, p.color, 0.0f)};
    const std::string tex = p.texture.empty() ? "builtin:foot" : p.texture;
    e.texture = p.mirror ? "mirror:" + tex : tex;
    e.blend = VfxBlend::Alpha;
    e.decal = true;
    e.softDistance = 0.0f;
    e.nearFade = 0.0f;
    e.maxScreenSize = 1.0f;
    e.lit = 0.8f;
    VfxEffectDesc fx;
    fx.name = name;
    fx.duration = 0.1f;
    fx.loop = false;
    fx.emitters.push_back(e);
    return fx;
}

// Passado o teto de vivos do tipo, os mais velhos saem.
void trimLive(VfxSystem& vfx, FootstepEffects& fe, bool print, size_t cap) {
    size_t n = 0;
    for (const auto& l : fe.live) n += l.print == print;
    for (size_t i = 0; n >= cap && i < fe.live.size();) {
        if (fe.live[i].print != print) { ++i; continue; }
        vfx.stop(fe.live[i].id, true);
        fe.live.erase(fe.live.begin() + long(i));
        --n;
    }
}

} // namespace

namespace {
// Chave do efeito pelos parametros (arredondados): mesmo pedido, mesmo efeito.
std::string paramKey(const char* kind, const std::string& tex, const Vec3& c, float a, float b, float d, float e,
                     float f, int g) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s|%.3f,%.3f,%.3f|%.3f|%.3f|%.2f|%.3f|%.3f|%d|", kind, c.x, c.y, c.z, a, b, d, e, f,
                  g);
    return buf + tex;
}
} // namespace

// Mesmo pedido de marca/poeira muitas vezes: o efeito e' montado uma vez.
// Anel com limite fixo: passado o teto, a mais velha sai.
void Engine::spawnFootprint(const Vec3& pos, const Vec2& dir, const FootprintParams& p) {
    FootstepEffects& fe = m_footsteps;
    // Ponta da textura para a direcao do andar, em passos de 15 graus.
    const float yaw = std::atan2(-dir.x, dir.y) * 57.2957795f;
    const int bucket = int(std::lround(yaw / 15.0f));
    const std::string key =
        paramKey("m", p.texture, p.color, p.size, p.life, p.alpha, 0.0f, 0.0f, bucket * 2 + (p.mirror ? 1 : 0));
    auto it = fe.effectNames.find(key);
    if (it == fe.effectNames.end()) {
        if (fe.effectNames.size() >= FootstepEffects::kMaxVariants) {
            ERUPTION_LOG_WARN("world.footprint: variantes demais (%zu), pedido ignorado", fe.effectNames.size());
            return;
        }
        const std::string name = "passo.marca#" + std::to_string(fe.effectNames.size());
        m_vfx.setEffect(name, buildPrint(p, float(bucket) * 15.0f, name));
        it = fe.effectNames.emplace(key, name).first;
    }
    trimLive(m_vfx, fe, true, FootstepEffects::kMaxPrints);
    const uint32_t id = m_vfx.spawn(it->second, glm::translate(Mat4(1.0f), pos));
    if (id) fe.live.push_back({id, fe.clock + double(p.life) + 1.0, true});
}

void Engine::spawnDust(const Vec3& pos, const DustParams& p) {
    FootstepEffects& fe = m_footsteps;
    const std::string key = paramKey("d", "", p.color, p.size, p.count, p.life, p.rise, p.alpha, 0);
    auto it = fe.effectNames.find(key);
    if (it == fe.effectNames.end()) {
        if (fe.effectNames.size() >= FootstepEffects::kMaxVariants) {
            ERUPTION_LOG_WARN("world.dust: variantes demais (%zu), pedido ignorado", fe.effectNames.size());
            return;
        }
        const std::string name = "passo.poeira#" + std::to_string(fe.effectNames.size());
        m_vfx.setEffect(name, buildDust(p, name));
        it = fe.effectNames.emplace(key, name).first;
    }
    trimLive(m_vfx, fe, false, FootstepEffects::kMaxDust);
    const uint32_t id = m_vfx.spawn(it->second, glm::translate(Mat4(1.0f), pos));
    if (id) fe.live.push_back({id, fe.clock + double(p.life) + 1.0, false});
}

// Os passos do quadro com o chao embaixo de cada pe'. O custo (um raio por
// passo, mais waterSurfaceAt) so' existe quando um script chama isto.
const std::vector<Engine::FootstepEvent>& Engine::footstepEvents() {
    if (m_footstepEventsReady) return m_footstepEvents;
    m_footstepEventsReady = true;
    m_footstepEvents.clear();
    constexpr size_t kMaxPorQuadro = 16;
    for (size_t i = 0; i < m_characterSteps.size() && i < kMaxPorQuadro; ++i) {
        const CharacterStep& st = m_characterSteps[i];
        FootstepEvent ev;
        ev.name = st.name;
        ev.pos = st.pos;
        ev.dir = st.dir;
        ev.foot = st.foot;
        ev.speed = st.speed;
        float level = 0.0f;
        ev.inWater = waterSurfaceAt(st.pos, &level);
        ModelRenderer::SurfaceHit hit;
        if (!ev.inWater && footSurfaceAt(st.pos, hit)) {
            ev.hasGround = true;
            ev.material = hit.material.empty() ? hit.texture : hit.material;
            ev.texture = std::move(hit.texture);
            ev.node = std::move(hit.node);
            if (hit.hasUv) ev.uv = Vec2(hit.uv.x - std::floor(hit.uv.x), hit.uv.y - std::floor(hit.uv.y));
        }
        m_footstepEvents.push_back(std::move(ev));
    }
    return m_footstepEvents;
}

} // namespace eruption
