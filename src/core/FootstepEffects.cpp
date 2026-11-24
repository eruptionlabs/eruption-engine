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
#include <cstdlib>

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
// `owner` (so' do rastro): o teto vale para os vivos desse dono.
void trimLive(VfxSystem& vfx, FootstepEffects& fe, uint8_t pool, size_t cap, const std::string* owner = nullptr) {
    auto mine = [&](const FootstepEffects::Live& l) { return l.pool == pool && (!owner || l.owner == *owner); };
    size_t n = 0;
    for (const auto& l : fe.live) n += mine(l);
    for (size_t i = 0; n >= cap && i < fe.live.size();) {
        if (!mine(fe.live[i])) { ++i; continue; }
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
void Engine::spawnFootprint(const Vec3& pos, const Vec2& dirIn, const FootprintParams& pIn) {
    FootstepEffects& fe = m_footsteps;
    FootprintParams p = pIn;
    Vec2 dir = dirIn;
    float step = 15.0f; // a ponta da textura gira em passos de 15 graus
    if (!p.shape.empty()) {
        // Forma de contato: o tamanho do VFX e' o lado do quadrado da mascara
        // (em metros); `size` dado pede essa largura de marca.
        auto m = fe.masks.find(p.shape);
        if (m != fe.masks.end()) {
            const float side = m->second.side / m_vfx.unitsPerMeter();
            const float width = std::max(m->second.width, 1e-3f) / m_vfx.unitsPerMeter();
            p.size = p.sizeGiven ? side * p.size / width : side;
            p.texture = p.shape;
            if (!p.colorGiven) p.color = Vec3(0.0f); // so' escurece: o chao guarda o proprio tom
        }
    }
    const float yaw = std::atan2(-dir.x, dir.y) * 57.2957795f;
    const int bucket = int(std::lround(yaw / step));
    const std::string key =
        paramKey("m", p.texture, p.color, p.size, p.life, p.alpha, 0.0f, 0.0f, bucket * 2 + (p.mirror ? 1 : 0));
    auto it = fe.effectNames.find(key);
    if (it == fe.effectNames.end()) {
        if (fe.effectNames.size() >= FootstepEffects::kMaxVariants) {
            ERUPTION_LOG_WARN("world.footprint: variantes demais (%zu), pedido ignorado", fe.effectNames.size());
            return;
        }
        const std::string name = "passo.marca#" + std::to_string(fe.effectNames.size());
        m_vfx.setEffect(name, buildPrint(p, float(bucket) * step, name));
        it = fe.effectNames.emplace(key, name).first;
    }
    const auto mk = p.shape.empty() ? fe.masks.end() : fe.masks.find(p.shape);
    const bool trail = mk != fe.masks.end() && mk->second.trail;
    if (trail) {
        trimLive(m_vfx, fe, FootstepEffects::kTrail, FootstepEffects::kMaxTrailPerOwner, &p.owner);
        trimLive(m_vfx, fe, FootstepEffects::kTrail, FootstepEffects::kMaxTrail);
    } else {
        trimLive(m_vfx, fe, FootstepEffects::kPrint, FootstepEffects::kMaxPrints);
    }
    const uint32_t id = m_vfx.spawn(it->second, glm::translate(Mat4(1.0f), pos));
    if (id) fe.live.push_back({id, fe.clock + double(p.life) + 1.0, trail ? FootstepEffects::kTrail : FootstepEffects::kPrint, p.owner});
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
    trimLive(m_vfx, fe, FootstepEffects::kDust, FootstepEffects::kMaxDust);
    const uint32_t id = m_vfx.spawn(it->second, glm::translate(Mat4(1.0f), pos));
    if (id) fe.live.push_back({id, fe.clock + double(p.life) + 1.0, FootstepEffects::kDust, {}});
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
        ev.contact = st.contact;
        ev.shape = st.shape;
        ev.width = st.width / m_vfx.unitsPerMeter(); // metros, como o `size` do world.footprint
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

// ---------------------------------------------------- passos pelos pixels

namespace {

} // namespace

// O ponto do chao (plano da ancora) que um ponto do billboard cobre na tela:
// `sx` = lado, para a direita da tela, `up` = altura acima da ancora, pela mesma
// conta do sprite_vert.glsl (ver billboardPixel em Characters.cpp). Sem chao
// na linha de visao (camera rasante), a propria ancora.
Vec3 Engine::groundUnderSpritePoint(const Vec3& anchor, float sx, float up) const {
    const float pitchNorm = glm::degrees(m_camera.orbitPitch()) / 89.0f;
    const float tilt = glm::radians(m_charConfig.billboardTiltSpline.evaluate(pitchNorm));
    Vec4 v = m_camera.viewMatrix() * Vec4(anchor, 1.0f);
    v += Vec4(sx, up * std::cos(tilt), -up * std::sin(tilt), 0.0f);
    const Vec3 world = Vec3(glm::inverse(m_camera.viewMatrix()) * v);
    const Vec3 eye = m_camera.position();
    const Vec3 ray = world - eye;
    if (ray.y > -1e-4f) return anchor;
    const float t = (anchor.y - eye.y) / ray.y;
    return t > 0.0f ? eye + ray * t : anchor;
}

// Registra o passo no quadro: o pe' alterna, e a agua ganha o anel.
void Engine::pushCharacterStep(const SceneCharacter& c, const Vec3& pos, int contactKind, const std::string& shape,
                               float width) {
    CharacterStep s;
    s.name = c.name;
    s.dir = c.stepDir;
    s.pos = pos;
    s.foot = c.stepFoot;
    s.speed = c.stepSpeed;
    s.contact = contactKind;
    s.shape = shape;
    s.width = width;
    float nivel = 0.0f;
    if (waterSurfaceAt(s.pos, &nivel)) m_water.addStepRipple(s.pos.x, s.pos.z, m_timer.elapsed(), 1.0f);
    m_characterSteps.push_back(std::move(s));
}

namespace {

// A silhueta de contato de uma acao do sprite: as camadas de cada quadro, ja'
// no mundo (o X do billboard cresce para a ESQUERDA da tela, ver drawCharacter).
contact::Action buildContactAction(const Engine::LoadedSprite& ls, int actionIdx) {
    const auto& action = ls.act.actions[size_t(actionIdx)];
    std::vector<std::vector<contact::Layer>> frames;
    frames.reserve(action.frames.size());
    for (const auto& fr : action.frames) {
        std::vector<contact::Layer> layers;
        for (const auto& spr : fr.sprites) {
            const int idx = spr.index + (spr.type == 1 ? int(ls.indexedTextureCount) : 0);
            if (idx < 0 || idx >= int(ls.textures.size()) || !ls.textures[size_t(idx)].contact) continue;
            const bool mirror = (spr.flags & 1) != 0;
            contact::Layer l;
            l.rows = ls.textures[size_t(idx)].contact.get();
            l.sx = -(mirror ? -float(spr.offsetX) : float(spr.offsetX)) * contact::kPixelToWorld;
            l.up0 = -float(spr.offsetY) * contact::kPixelToWorld;
            l.pw = contact::kPixelToWorld * spr.scaleX;
            l.ph = contact::kPixelToWorld * spr.scaleY;
            l.mirror = mirror;
            layers.push_back(l);
        }
        frames.push_back(std::move(layers));
    }
    return contact::build(frames);
}

} // namespace

// O PASSO PELOS PIXELS. A acao que anda tem a silhueta de contato de cada
// quadro (SpriteContact.hpp, lida do alfa); a cada quadro mostrado:
//  - uma mancha que aparece ou desce ate' o chao e' um pe' que pisou: sai um
//    passo com a forma dela (bipede: dois pes; quadrupede: as patas);
//  - se o ciclo inteiro nunca tem mancha nova (cobra, lesma: o corpo desliza
//    no chao), o passo e' um arrasto: a faixa inteira, a cada trecho andado.
// Sprite sem pixel no chao (ou sem sprite) segue na passada (trackCharacterStep).
void Engine::trackContactSteps(SceneCharacter& c, int actionIdx, int shownFrame, bool moving, const Vec3& anchor) {
    constexpr int kMaxFrames = 64;
    c.contactDriven = false;
    c.strideShape.clear();
    c.strideWidth = 0.0f;
#if ERUPTION_DEV
    // ERUPTION_TEST_STRIDE_STEPS=1 (teste): so' a passada, como antes do passo
    // pelos pixels, para comparar a marca e o custo.
    static const bool kStrideOnly = std::getenv("ERUPTION_TEST_STRIDE_STEPS") != nullptr;
    if (kStrideOnly) return;
#endif
    LoadedSprite& ls = c.look;
    if (!moving || actionIdx < 0 || actionIdx >= int(ls.act.actions.size())) {
        c.contactAction = -1;
        return;
    }
    auto& slot = ls.contactActions[actionIdx];
    if (!slot) {
        // As 8 direcoes de andar valem um tipo so': o que desliza (cobra) ou
        // pisa (bipede, quadrupede, gelatina que salta) e' do sprite, nao de
        // uma direcao: visto de frente os pes de um bipede quase nao se
        // mexem na folha e pareceriam deslizar. Vale a mediana das direcoes.
        std::vector<int> dirs;
        for (int a = 8; a < 16 && a < int(ls.act.actions.size()); ++a) dirs.push_back(a);
        if (actionIdx < 8 || actionIdx >= 16 || dirs.size() < 8) dirs = {actionIdx};
        std::vector<int> slides;
        for (int a : dirs) {
            auto& sa = ls.contactActions[a];
            if (!sa) sa = std::make_shared<contact::Action>(buildContactAction(ls, a));
            slides.push_back(sa->slideFrames);
        }
        std::sort(slides.begin(), slides.end());
        const bool drag = slides[slides.size() / 2] <= 1;
        for (int a : dirs) contact::setDrag(*ls.contactActions[a], drag);
        // O tipo vale para o sprite todo: o da maioria das direcoes (empate:
        // sola, pata, mancha, rastro).
        int votes[4] = {0, 0, 0, 0};
        for (int a : dirs) ++votes[int(ls.contactActions[a]->shape)];
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (votes[k] > votes[best]) best = k;
        for (int a : dirs) contact::setFamily(*ls.contactActions[a], contact::Shape(best));
        // A FORMA, das 8 direcoes juntas: cada uma mostra o pe' de um lado.
        // Direcao d = quem anda virado 45*d graus da camera (0 = de frente
        // para ela, ver drawCharacter); o eixo da direita da tela visto por ele
        // e' o eixo daquela vista. Referencial da marca (do decal): frente F,
        // direita (F.z, -F.x).
        const Vec3 camF = m_camera.forward();
        const Mat4& view = m_camera.viewMatrix();
        Vec2 camR(view[0][0], view[2][0]);
        camR = glm::length(camR) > 1e-5f ? glm::normalize(camR) : Vec2(1.0f, 0.0f);
        const float camYaw = std::atan2(camF.x, camF.z);
        std::vector<contact::View> views;
        for (int a : dirs) {
            contact::View v;
            if (!contact::viewProfile(*ls.contactActions[a], v.prof)) continue;
            const float facing = camYaw + glm::radians(180.0f - 45.0f * float(a % 8));
            const Vec2 F(std::sin(facing), std::cos(facing)), R(F.y, -F.x);
            v.au = glm::dot(camR, R);
            v.av = glm::dot(camR, F);
#if ERUPTION_DEV
            if (std::getenv("ERUPTION_TEST_CONTACT_TRACE")) {
                int on = 0;
                for (float q : v.prof) on += q > 0.1f;
                ERUPTION_LOG_WARN("[CONTATO] %s vista %d eixo=(%.2f,%.2f) largura=%.2f", c.name.c_str(), a % 8, v.au, v.av,
                                  float(on) * contact::kCell);
            }
#endif
            views.push_back(std::move(v));
        }
        std::string printId;
        contact::Mask print;
        if (!views.empty()) {
            print = contact::hullMask(views, ls.contactActions[dirs[0]]->drag);
            if (m_footsteps.masks.find(print.id) != m_footsteps.masks.end() ||
                m_footsteps.masks.size() < FootstepEffects::kMaxMasks) {
                printId = print.id;
                m_footsteps.masks.emplace(print.id, print);
            }
        }
        for (int a : dirs) {
            contact::Action& da = *ls.contactActions[a];
            da.printId = printId;
            da.printWidth = print.width;
            da.printLength = print.length;
        }
#if ERUPTION_DEV
        // ERUPTION_TEST_CONTACT_TRACE=1 (teste): as direcoes de andar do
        // sprite e, por quadro da acao em uso, o chao e o que pisou.
        static const bool kTrace = std::getenv("ERUPTION_TEST_CONTACT_TRACE") != nullptr;
        if (kTrace)
            for (int a : dirs) {
                const auto& act = ls.contactActions[a];
                int strikeFrames = 0;
                for (const auto& fr : act->frames) strikeFrames += fr.strikes.empty() ? 0 : 1;
                ERUPTION_LOG_WARN("[CONTATO] %s acao=%d quadros=%zu arrasto=%d quadros_com_pisada=%d deslize=%d", c.name.c_str(), a,
                                  act->frames.size(), act->drag ? 1 : 0, strikeFrames, act->slideFrames);
                if (a != actionIdx) continue;
                for (size_t i = 0; i < act->frames.size(); ++i) {
                    const contact::Frame& fr = act->frames[i];
                    std::string t;
                    for (const auto& b : fr.strikes) {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), " PISA[%.2f..%.2f]", fr.x0 + float(b.c0) * contact::kCell,
                                      fr.x0 + float(b.c1 + 1) * contact::kCell);
                        t += buf;
                    }
                    ERUPTION_LOG_WARN("[CONTATO]   quadro %zu baixo=%.2f%s", i, fr.lowest, t.c_str());
                }
            }
        if (kTrace) {
            ERUPTION_LOG_WARN("[CONTATO] %s marca %s vistas=%zu %dx%d lado=%.2f largura=%.2f comprimento=%.2f", c.name.c_str(),
                              printId.c_str(), views.size(), print.size, print.size, print.side, print.width, print.length);
            if (const char* dir = std::getenv("ERUPTION_TEST_CONTACT_DUMP"); dir && !printId.empty()) {
                // ERUPTION_TEST_CONTACT_DUMP=<pasta> (teste): a mascara como PGM.
                const std::string path = std::string(dir) + "/" + c.name + "_" + printId.substr(8) + "_mask.pgm";
                if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                    std::fprintf(fp, "P5 %d %d 255\n", print.size, print.size);
                    for (int i = 0; i < print.size * print.size; ++i) std::fputc(print.rgba[size_t(i) * 4 + 3], fp);
                    std::fclose(fp);
                }
            }
        }
#endif
    }
    contact::Action& act = *slot;
    const int n = int(act.frames.size());
    if (!act.any || n == 0 || shownFrame < 0 || shownFrame >= n) {
        c.contactAction = -1;
        return;
    }
    // Direcao em que nao se acham 2 pisadas por ciclo (visto de frente os pes
    // quase nao se mexem na folha): passada, mas com a marca do sprite.
    if (act.strideFallback) {
        c.strideShape = act.printId;
        c.strideWidth = act.printWidth;
        c.contactAction = -1;
        return;
    }
    c.contactDriven = true;
    if (c.contactAction != actionIdx || c.contactFrame < 0 || c.contactFrame >= n) {
        c.contactAction = actionIdx;
        c.contactFrame = shownFrame;
        c.dragOdometer = 0.0f;
        return;
    }
    // O ponto do chao que o pixel mais baixo da mancha cobre na tela (o pe'
    // do desenho fica abaixo ou acima da ancora): o centro da marca.
    auto stamp = [&](const contact::Frame& fr, const contact::Blob& b, int kind) {
        Vec3 pos = groundUnderSpritePoint(anchor, b.sxCenter, fr.lowest);
        pos.y = floorHeight(pos.x, pos.z, anchor.y + 10.0f); // o chao de verdade ali, nao o plano da ancora
        pushCharacterStep(c, pos, kind, act.printId, act.printWidth);
    };
    const bool moved = c.stepDist > 1e-4f;
    // Do quadro anterior ao de agora, um a um: o ciclo pode pular quadros num
    // intervalo longo, e o pe' que pisou no meio nao pode se perder.
    int f = c.contactFrame;
    for (int guard = 0; f != shownFrame && guard < kMaxFrames; ++guard) {
        f = (f + 1) % n;
        if (!moved || act.drag) continue;
        for (const contact::Blob& b : act.frames[size_t(f)].strikes) {
            stamp(act.frames[size_t(f)], b, 1);
            c.stepFoot ^= 1;
        }
    }
    c.contactFrame = shownFrame;
    if (act.drag && moved) {
        contact::Frame& fr = act.frames[size_t(shownFrame)];
        if (fr.any) {
            c.dragOdometer += c.stepDist;
            // Um carimbo a cada 0,4 do comprimento da marca: os vizinhos se
            // cobrem e fecham uma faixa sem emenda.
            const float interval = std::max(0.4f * act.printLength, 0.3f);
            if (c.dragOdometer >= interval) {
                c.dragOdometer = std::fmod(c.dragOdometer, interval);
                stamp(fr, fr.all, 2);
            }
        }
    }
}

} // namespace eruption
