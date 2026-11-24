// Cola das afeccoes do lugar (ver core/Affects.hpp): descobre a superficie sob
// o ponto, escolhe a reacao na tabela data/affects.json e a monta com as pecas
// que o motor ja' tem: decal e particulas do VFX, molhado (Wetness.cpp) e a
// rajada de vento lida pelo shader de vegetacao. Nada por elemento no codigo.
//
// Custo: uma afeccao e' UMA instancia de VFX (todos os emissores dela), o
// efeito e' montado uma vez por variante (reacao, qualidade, faixa de forca,
// duracao arredondada) e reaproveitado; o resto e' um anel de ids.
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "renderer/vfx/VfxSystem.hpp"
#include "utils/AssetVfs.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace eruption {

using affects::Block;
using affects::DecalBlock;
using affects::ParticleBlock;
using affects::RaiseBlock;
using affects::Reaction;

namespace {

constexpr size_t kMaxVariants = 1024; // efeitos distintos montados (cada um e' uns KB)

// Forca em 3 faixas: a variante do efeito leva o alfa e a contagem prontos, e
// pedidos parecidos dividem o mesmo efeito.
int strengthBucket(float s) { return s < 0.34f ? 0 : (s < 0.67f ? 1 : 2); }
float strengthOf(int bucket) {
    static const float k[3] = {0.35f, 0.65f, 1.0f};
    return k[std::clamp(bucket, 0, 2)];
}

// Duracao arredondada em degraus de 25%: 20 s e 22 s dividem o efeito.
int durationBucket(float seconds) {
    return int(std::lround(std::log(std::max(seconds, 0.1f)) / std::log(1.25f)));
}
float durationOf(int bucket) { return std::pow(1.25f, float(bucket)); }

VfxGradient::Key gkey(float t, const Vec3& c, float a) {
    VfxGradient::Key k;
    k.t = t;
    k.color = Vec4(c, a);
    return k;
}

Vec3 mix3(const float a[3], const float b[3], float t) {
    return Vec3(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t);
}

// Contagem: forca e preset mexem; o que existe no bloco nunca zera por conta
// do arredondamento (a menos que o preset zere as particulas).
uint32_t scaledCount(uint32_t n, float strength, float particles) {
    if (n == 0 || particles <= 0.0f) return 0;
    return std::max(1u, uint32_t(std::lround(float(n) * (0.5f + 0.5f * strength) * particles)));
}

VfxEmitterDesc buildDecal(const DecalBlock& d, float strength, float dur, float yawDeg, bool hasYaw) {
    VfxEmitterDesc e;
    e.name = "marca";
    e.rate = 0.0f;
    e.burstCount = d.count;
    e.capacity = d.count + 2u;
    if (d.scatter > 0.0f) {
        e.shape = VfxShape::Disc;
        e.shapeSize = Vec3(d.scatter);
    } else {
        e.shape = VfxShape::Point;
    }
    e.startDelay = d.delay * dur;
    const float life = std::max(d.life * dur, 0.2f);
    e.lifetime = Vec2(life * 0.92f, life);
    e.speed = Vec2(0.0f);
    e.size = Vec2(d.size * (1.0f - d.sizeJitter), d.size * (1.0f + d.sizeJitter));
    e.sizeOverLife.keys = {Vec2(0.0f, d.sizeStart), Vec2(1.0f, d.sizeEnd)};
    if (d.directional && hasYaw) e.rotation = Vec2(yawDeg, yawDeg);
    else e.rotation = Vec2(0.0f, 360.0f);
    // Sobe rapido, segura e some suave; a cor anda da inicial para a final
    // (brasa que esfria) pelo caminho todo.
    const float a = d.alpha * ((1.0f - d.strengthAlpha) + d.strengthAlpha * strength);
    const float tIn = std::clamp(d.fadeIn, 0.0f, 0.95f);
    const float tHold = std::clamp(std::max(d.hold, tIn + 0.01f), 0.02f, 0.98f);
    e.color.keys = {gkey(0.0f, mix3(d.color, d.colorEnd, 0.0f), 0.0f), gkey(tIn, mix3(d.color, d.colorEnd, tIn), a),
                    gkey(tHold, mix3(d.color, d.colorEnd, tHold), a), gkey(1.0f, mix3(d.color, d.colorEnd, 1.0f), 0.0f)};
    if (tIn <= 0.0f) e.color.keys.erase(e.color.keys.begin()); // nasce ja' no alfa cheio (clarao)
    e.texture = d.texture;
    // Atlas de desenhos: um quadro sorteado por marca, sem animar nem misturar.
    if (d.atlas > 1) {
        e.flipCols = e.flipRows = d.atlas;
        e.flipMode = VfxFlipMode::Random;
        e.flipRandomStart = true;
        e.flipBlend = false;
    }
    e.blend = d.additive ? VfxBlend::Additive : VfxBlend::Alpha;
    e.decal = true;
    e.softDistance = 0.0f;
    e.nearFade = 0.0f;
    e.maxScreenSize = 1.0f;
    e.lit = d.additive ? 0.0f : d.lit;
    return e;
}

VfxEmitterDesc buildParticles(const ParticleBlock& p, float strength, float particles, float dur) {
    VfxEmitterDesc e;
    e.name = p.mesh.empty() ? "particulas" : "pedrinhas";
    const bool timed = p.rate > 0.0f;
    e.rate = timed ? p.rate * (0.5f + 0.5f * strength) * std::max(particles, 0.25f) : 0.0f;
    e.burstCount = scaledCount(p.count, strength, particles);
    e.duration = timed ? std::max(p.duration, 0.05f) * dur : 0.0f;
    e.startDelay = p.burstTime * dur;
    const float lifeMax = p.life[1];
    e.capacity = std::max(8u, e.burstCount + (timed ? uint32_t(std::ceil(e.rate * lifeMax)) : 0u) + 4u);
    if (p.areaRadius > 0.0f) {
        e.shape = VfxShape::Disc;
        e.shapeSize = Vec3(p.areaRadius);
    } else {
        e.shape = VfxShape::Point;
    }
    e.offset = Vec3(0.0f, 0.02f, 0.0f);
    e.lifetime = Vec2(p.life[0], p.life[1]);
    e.speed = Vec2(p.speed[0], p.speed[1]);
    e.direction = Vec3(0.0f, 1.0f, 0.0f);
    // "outward": o jorro sai para os lados (hemisferio) em vez de subir em cone.
    e.spread = p.outward ? std::max(p.spread, 0.5f) : p.spread;
    e.size = Vec2(p.size[0], p.size[1]);
    e.sizeOverLife.keys = {Vec2(0.0f, 0.6f), Vec2(1.0f, p.sizeEnd)};
    e.rotation = Vec2(0.0f, 360.0f);
    e.angularVelocity = Vec2(-40.0f, 40.0f);
    e.gravity = Vec3(0.0f, p.gravity, 0.0f);
    e.drag = p.drag;
    const float a = p.alpha * (0.6f + 0.4f * strength);
    const Vec3 c0(p.color[0], p.color[1], p.color[2]);
    const Vec3 c1(p.colorEnd[0], p.colorEnd[1], p.colorEnd[2]);
    e.color.keys = {gkey(0.0f, c0, 0.0f), gkey(0.12f, c0, a), gkey(0.55f, mix3(p.color, p.colorEnd, 0.55f), a * 0.8f),
                    gkey(1.0f, c1, 0.0f)};
    if (!p.mesh.empty()) {
        e.mesh = p.mesh;
        e.color.keys = {gkey(0.0f, c0, 1.0f), gkey(1.0f, c1, 1.0f)};
        e.sizeOverLife.keys = {Vec2(0.0f, 1.0f), Vec2(0.8f, 1.0f), Vec2(1.0f, 0.0f)};
        e.collideDepth = p.collide;
        e.bounce = 0.3f;
        e.friction = 0.4f;
    } else {
        e.texture = p.texture;
        e.blend = p.additive ? VfxBlend::Additive : VfxBlend::Alpha;
        if (p.texture == "builtin:puff") {
            e.flipCols = e.flipRows = 4;
            e.flipMode = VfxFlipMode::Random;
            e.flipBlend = false;
        } else if (p.texture == "builtin:leaf") {
            e.flipCols = e.flipRows = 2;
            e.flipMode = VfxFlipMode::Random;
            e.flipBlend = false;
        }
        e.softDistance = 0.15f;
        e.nearFade = 0.2f;
        e.lit = p.additive ? 0.0f : p.lit;
        e.collideDepth = p.collide;
    }
    return e;
}

// Relevo que sobe e assenta: pedacos de malha enterrados no chao que crescem
// e voltam a afundar. So' nos presets de qualidade alta (geometria de verdade,
// com silhueta e sombra, no lugar da ilusao do decal).
VfxEmitterDesc buildRaise(const RaiseBlock& r, float strength, float particles, float dur) {
    VfxEmitterDesc e;
    e.name = "relevo";
    e.rate = 0.0f;
    e.burstCount = std::max(1u, scaledCount(r.count, strength, std::max(particles, 0.5f)));
    e.capacity = e.burstCount + 2u;
    if (r.ring > 0.0f) {
        e.shape = VfxShape::Disc;
        e.shapeSize = Vec3(r.ring);
        e.surfaceOnly = true;
    } else {
        e.shape = VfxShape::Point;
    }
    // Enterrado: o topo do pedaco (raio ~ size) fica `rise` acima do chao, e ele
    // cresce dali em vez de aparecer solto no ar.
    e.offset = Vec3(0.0f, std::min(r.rise - r.size, 0.0f), 0.0f);
    const float life = std::max(r.life * dur, 0.5f);
    e.lifetime = Vec2(life * 0.9f, life);
    e.speed = Vec2(0.0f);
    e.size = Vec2(r.size * 0.8f, r.size * 1.2f) * (0.7f + 0.3f * strength);
    const float up = std::clamp(r.riseTime, 0.01f, 0.9f);
    // A altura que o relevo atinge e' a do proprio pedaco: sobe rapido ao
    // tamanho cheio e assenta devagar ate' sumir no chao.
    e.sizeOverLife.keys = {Vec2(0.0f, 0.04f), Vec2(up, 1.0f), Vec2(up + (1.0f - up) * 0.4f, 0.8f), Vec2(1.0f, 0.0f)};
    e.rotation = Vec2(0.0f, 360.0f);
    e.mesh = r.mesh;
    e.color.keys = {gkey(0.0f, Vec3(r.color[0], r.color[1], r.color[2]), 1.0f),
                    gkey(1.0f, Vec3(r.color[0], r.color[1], r.color[2]), 1.0f)};
    return e;
}

// Do que a reacao precisa em fracao da duracao: ate' quando ela esta' viva.
float reactionExtent(const Reaction& r, float dur) {
    float ext = 0.0f;
    for (const Block& b : r.blocks) {
        switch (b.type) {
        case Block::Type::Decal: ext = std::max(ext, b.decal.delay + b.decal.life); break;
        case Block::Type::Raise: ext = std::max(ext, b.raise.life); break;
        case Block::Type::Wet: ext = std::max(ext, b.wet.dry); break;
        case Block::Type::Particles: {
            const float t = b.particles.burstTime + (b.particles.rate > 0.0f ? b.particles.duration : 0.0f);
            ext = std::max(ext, t + b.particles.life[1] / std::max(dur, 0.1f));
            break;
        }
        case Block::Type::Wind: break;
        }
    }
    return std::max(ext, 0.05f);
}

bool inQuality(int lo, int hi, int level) { return level >= lo && level <= hi; }

} // namespace

// ------------------------------------------------------------ tabela

void Engine::loadAffectTable() {
    if (m_affectTableLoaded) return;
    m_affectTableLoaded = true;
    // O projeto do jogo vence (arquivo inteiro, sem mescla); senao o do motor.
    const std::filesystem::path candidates[2] = {project::game("data/affects.json"), project::engine("data/affects.json")};
    for (const auto& c : candidates) {
        std::string text, err;
        if (!vfs::exists(c) || !vfs::readText(c.string(), text)) continue;
        affects::Table t;
        if (!t.parse(text, &err)) {
            ERUPTION_LOG_WARN("[AFECCAO] %s: %s", c.string().c_str(), err.c_str());
            continue;
        }
        m_affectTable = std::move(t);
        ERUPTION_LOG_INFO("[AFECCAO] tabela %s: %zu elementos, %zu reacoes", c.string().c_str(),
                          m_affectTable.elements().size(), m_affectTable.reactionCount());
        return;
    }
    ERUPTION_LOG_WARN("[AFECCAO] sem data/affects.json: world.affect nao faz nada");
}

const affects::Table& Engine::affectTable() {
    loadAffectTable();
    return m_affectTable;
}

void Engine::affectApplyPreset(const json& p) {
    affects::Quality& q = m_affectQuality;
    q.level = std::clamp(p.value("affect_quality", 0), 0, 2);
    q.maxAffects = uint32_t(std::clamp(p.value("affect_max", 48), 1, int(affects::kHardMaxAffects)));
    q.maxGusts = uint32_t(std::clamp(p.value("wind_gust_max", 4), 1, int(affects::kMaxGusts)));
    q.particles = std::clamp(p.value("affect_particles", 0.5f), 0.0f, 1.5f);
}

void Engine::affectsOnMapLoaded() {
    m_affectRing.clear();
    m_gustRing.clear();
    m_affectEffects.clear();
}

// -------------------------------------------------------- superficie

// Superficie do lugar: agua, regiao de textura, nome do material/textura/no' do
// chao pelas dicas da tabela, ou a camada pintada de maior peso no terreno.
std::string Engine::affectSurfaceAt(const Vec3& pos) {
    loadAffectTable();
    float level = 0.0f;
    if (waterSurfaceAt(pos, &level)) return "water";
    ModelRenderer::SurfaceHit hit;
    if (footSurfaceAt(pos, hit)) {
        if (hit.hasUv) {
            const Vec2 uv(hit.uv.x - std::floor(hit.uv.x), hit.uv.y - std::floor(hit.uv.y));
            const std::string s = m_affectTable.surfaceFromRegion(regionSurfaceAt(hit.texture, uv));
            if (!s.empty()) return s;
        }
        for (const std::string* name : {&hit.material, &hit.texture, &hit.node}) {
            const std::string s = m_affectTable.surfaceFromName(*name);
            if (s != "default") return s;
        }
    }
    // Terreno de grade: a camada pintada que mais pesa (SplatEdit).
    const SplatEdit& sp = m_splatEdit;
    if (sp.active && sp.w > 0 && sp.d > 0 && sp.weights.size() >= size_t(sp.w) * sp.d * 8 && m_heightEdit.scale > 0.0f) {
        const int ix = std::clamp(int(std::lround((pos.x - m_heightEdit.offX) / m_heightEdit.scale)), 0, int(sp.w) - 1);
        const int iz = std::clamp(int(std::lround((pos.z - m_heightEdit.offZ) / m_heightEdit.scale)), 0, int(sp.d) - 1);
        const float* w = &sp.weights[(size_t(iz) * sp.w + size_t(ix)) * 8];
        int best = -1;
        float bestW = 0.0f;
        for (int l = 0; l < 8; ++l)
            if (w[l] > bestW) { bestW = w[l]; best = l; }
        if (best >= 0) {
            const std::string& name = sp.layerFiles[best].empty() ? sp.layerNames[best] : sp.layerFiles[best];
            return m_affectTable.surfaceFromName(name);
        }
    }
    return "default";
}

// ---------------------------------------------------------- pedidos

uint32_t Engine::affect(const AffectRequest& r) {
    loadAffectTable();
    if (m_affectTable.empty() || !m_affectTable.hasElement(r.kind)) {
        ERUPTION_LOG_WARN("world.affect: elemento desconhecido '%s'", r.kind.c_str());
        return 0;
    }
    // Sem raio pedido: 3 m do mapa (o jogo mede em metros, o motor em unidades).
    const float radius = std::clamp(r.radius > 0.0f ? r.radius : 3.0f * m_vfx.unitsPerMeter(), 0.05f, 2000.0f);
    const float strength = std::clamp(r.strength, 0.0f, 1.0f);
    const float duration = std::clamp(r.duration, 0.1f, 3600.0f);
    float y = floorHeight(r.x, r.z, FLT_MAX);
    Vec3 pos(r.x, y, r.z);
    const std::string surface = affectSurfaceAt(pos);
    float level = 0.0f;
    if (surface == "water" && waterSurfaceAt(pos, &level)) pos.y = level;
    const Reaction* reaction = m_affectTable.select(r.kind, surface);
    if (!reaction) {
        ERUPTION_LOG_WARN("world.affect: '%s' sem reacao em '%s'", r.kind.c_str(), surface.c_str());
        return 0;
    }

    const affects::Quality& q = m_affectQuality;
    affects::LiveAffect live;
    if (++m_affectNextId == 0) ++m_affectNextId;
    live.id = m_affectNextId;
    live.born = m_affectClock;
    live.expire = m_affectClock + double(duration) * double(reactionExtent(*reaction, duration));
    live.element = r.kind;
    live.surface = surface;
    live.x = r.x;
    live.z = r.z;

    // Efeito do VFX (decal, particulas, relevo) montado por variante.
    const int sb = strengthBucket(strength), db = durationBucket(duration);
    const float sv = strengthOf(sb), dv = durationOf(db);
    const bool useDir = reaction->usesDir() && r.hasDir;
    const float yaw = useDir ? std::atan2(-r.dir.x, r.dir.y) * 57.2957795f : 0.0f;
    const int yawBucket = int(std::lround(yaw / 15.0f));
    char key[160];
    std::snprintf(key, sizeof(key), "%s|q%d|s%d|d%d|y%d|p%d", reaction->name.c_str(), q.level, sb, db,
                  useDir ? yawBucket : 0, int(std::lround(q.particles * 10.0f)));
    auto it = m_affectEffects.find(key);
    if (it == m_affectEffects.end()) {
        VfxEffectDesc fx;
        fx.duration = 0.1f;
        fx.loop = false;
        float emitUntil = 0.0f; // emissores com taxa precisam do efeito aberto ate' acabarem
        for (const Block& b : reaction->blocks) {
            if (b.type == Block::Type::Decal && inQuality(b.decal.minQuality, b.decal.maxQuality, q.level)) {
                fx.emitters.push_back(buildDecal(b.decal, sv, dv, float(yawBucket) * 15.0f, useDir));
            } else if (b.type == Block::Type::Particles && inQuality(b.particles.minQuality, b.particles.maxQuality, q.level)) {
                VfxEmitterDesc e = buildParticles(b.particles, sv, q.particles, dv);
                if (e.burstCount == 0 && e.rate <= 0.0f) continue;
                if (e.rate > 0.0f) emitUntil = std::max(emitUntil, e.startDelay + e.duration);
                fx.emitters.push_back(std::move(e));
            } else if (b.type == Block::Type::Raise && inQuality(b.raise.minQuality, b.raise.maxQuality, q.level)) {
                fx.emitters.push_back(buildRaise(b.raise, sv, q.particles, dv));
            }
        }
        fx.duration = std::max(fx.duration, emitUntil + 0.1f);
        std::string name;
        if (!fx.emitters.empty()) {
            if (m_affectEffects.size() >= kMaxVariants) {
                ERUPTION_LOG_WARN("world.affect: variantes demais (%zu), so' molhado e vento deste pedido", m_affectEffects.size());
            } else {
                name = "afecc." + reaction->name + "#" + std::to_string(m_affectEffects.size());
                fx.name = name;
                m_vfx.setEffect(name, fx);
            }
        }
        it = m_affectEffects.emplace(key, name).first;
    }
    if (!it->second.empty()) {
        const float s = radius / std::max(m_vfx.unitsPerMeter(), 1e-4f);
        const uint32_t id = m_vfx.spawn(it->second, glm::translate(Mat4(1.0f), pos) * glm::scale(Mat4(1.0f), Vec3(s)));
        if (id) live.vfx.push_back(id);
    }

    // Molhado e vento da reacao.
    for (const Block& b : reaction->blocks) {
        if (b.type == Block::Type::Wet) {
            // Lama seca mais devagar que poca limpa.
            const float dry = duration * b.wet.dry * (b.wet.mud ? 1.6f : 1.0f);
            wetArea(pos, radius * b.wet.radius, b.wet.amount * (0.5f + 0.5f * strength), dry);
        } else if (b.type == Block::Type::Wind) {
            const float wd = std::max(b.wind.durationMin, b.wind.duration * duration);
            live.gusts.push_back(windBurst(r.x, r.z, radius * b.wind.radius, b.wind.strength * strength, wd));
        }
    }

    // Teto: passou, a mais velha (e as particulas dela) sai.
    for (const affects::LiveAffect& old : m_affectRing.add(live, q.maxAffects)) {
        for (uint32_t v : old.vfx) m_vfx.stop(v, true);
        for (uint32_t g : old.gusts) m_gustRing.stop(g);
    }
#if ERUPTION_DEV
    // ERUPTION_TEST_AFFECT_LOG=1 (teste): uma linha por pedido com a superficie
    // achada e a reacao escolhida (o nivel de log padrao esconde o INFO).
    static const bool kLog = std::getenv("ERUPTION_TEST_AFFECT_LOG") != nullptr;
    if (kLog) {
        ModelRenderer::SurfaceHit dbg;
        const bool hit = footSurfaceAt(pos, dbg);
        ERUPTION_LOG_WARN("[AFECCAO] #%u %s sobre %s (%.0f, %.0f) raio %.1f forca %.2f duracao %.1fs -> %s | chao: %s | %s | %s",
                          live.id, r.kind.c_str(), surface.c_str(), r.x, r.z, radius, strength, duration,
                          reaction->name.c_str(), hit ? dbg.material.c_str() : "-", hit ? dbg.texture.c_str() : "-",
                          hit ? dbg.node.c_str() : "-");
    }
#endif
    return live.id;
}

bool Engine::affectStop(uint32_t id) {
    affects::LiveAffect a;
    if (m_affectRing.stop(id, &a)) {
        for (uint32_t v : a.vfx) m_vfx.stop(v, true);
        for (uint32_t g : a.gusts) m_gustRing.stop(g);
        return true;
    }
    return m_gustRing.stop(id);
}

uint32_t Engine::windBurst(float x, float z, float radius, float strength, float duration) {
    if (++m_affectNextId == 0) ++m_affectNextId;
    const uint32_t id = m_affectNextId;
    m_gustRing.add(id, x, z, std::clamp(radius > 0.0f ? radius : 3.0f * m_vfx.unitsPerMeter(), 0.05f, 2000.0f), std::clamp(strength, 0.0f, 1.0f),
                   std::clamp(duration, 0.05f, 600.0f), m_affectQuality.maxGusts);
    return id;
}

void Engine::updateAffects(float dt) {
    m_affectClock += double(dt);
    // As particulas e marcas do VFX acabam sozinhas; aqui so' sai do anel o
    // que ja' passou do prazo (o teto conta afeccoes visivelmente vivas).
    (void)m_affectRing.tick(m_affectClock);
    m_gustRing.tick(dt);
}

void Engine::fillGusts(FrameUBO& ubo) const {
    affects::GustPacked packed[affects::kMaxGusts];
    const uint32_t n = m_gustRing.pack(packed);
    ubo.gustInfo = Vec4(float(n), 0.0f, 0.0f, 0.0f);
    for (uint32_t i = 0; i < affects::kMaxGusts; ++i) {
        const bool on = i < n;
        ubo.gusts[2 * i] = on ? Vec4(packed[i].a[0], packed[i].a[1], packed[i].a[2], packed[i].a[3]) : Vec4(0.0f);
        ubo.gusts[2 * i + 1] = on ? Vec4(packed[i].b[0], packed[i].b[1], packed[i].b[2], packed[i].b[3]) : Vec4(0.0f);
    }
}

} // namespace eruption
