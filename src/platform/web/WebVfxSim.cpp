#include "platform/web/WebVfxSim.hpp"

#include <glm/gtc/packing.hpp>
#include <glm/packing.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace eruption {

namespace {

// Bits de VfxEmitterGpu::flags.x (vfx_common.glsl, VFXF_*).
constexpr uint32_t kShapeMask = 0x7u;
constexpr uint32_t kSurface = 1u << 3;
constexpr uint32_t kFacingShift = 4u;
constexpr uint32_t kBlendShift = 6u;
constexpr uint32_t kFlipBlend = 1u << 8;
constexpr uint32_t kFlipRandom = 1u << 9;
constexpr uint32_t kCollidePlane = 1u << 10;
constexpr uint32_t kKilled = 1u << 12;
constexpr uint32_t kLowRes = 1u << 14;
constexpr uint32_t kMeshShift = 16u;
constexpr float kPi = 3.14159265f;

// Mesmo PCG hash do vfx_common.glsl: a mesma semente do pedido da' a mesma
// particula que a GPU faria (o efeito continua deterministico).
uint32_t pcg(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rand01(uint32_t seed, uint32_t salt) {
    return float(pcg(seed ^ (salt * 0x9E3779B9u)) >> 8) * (1.0f / 16777216.0f);
}

Vec3 randomUnit(uint32_t seed, uint32_t salt) {
    const float z = rand01(seed, salt) * 2.0f - 1.0f;
    const float a = rand01(seed, salt + 1u) * 2.0f * kPi;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vec3(r * std::cos(a), z, r * std::sin(a));
}

Vec3 coneDir(const Vec3& axis, float halfAngle, uint32_t seed) {
    const float cosMax = std::cos(halfAngle);
    const float z = glm::mix(cosMax, 1.0f, rand01(seed, 11u));
    const float a = rand01(seed, 12u) * 2.0f * kPi;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const Vec3 up = std::abs(axis.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 t = glm::normalize(glm::cross(up, axis));
    const Vec3 b = glm::cross(axis, t);
    return t * (r * std::cos(a)) + b * (r * std::sin(a)) + axis * z;
}

Vec3 shapePoint(const VfxEmitterGpu& e, uint32_t seed) {
    const uint32_t shape = e.flags.x & kShapeMask;
    const bool surface = (e.flags.x & kSurface) != 0u;
    const Vec3 s = Vec3(e.shape);
    if (shape == 1u) { // esfera
        const float r = surface ? 1.0f : std::cbrt(rand01(seed, 21u));
        return randomUnit(seed, 22u) * s.x * r;
    }
    if (shape == 2u) { // caixa
        Vec3 q = Vec3(rand01(seed, 23u), rand01(seed, 24u), rand01(seed, 25u)) * 2.0f - 1.0f;
        if (surface) {
            const uint32_t axis = pcg(seed ^ 26u) % 3u;
            q[int(axis)] = q[int(axis)] < 0.0f ? -1.0f : 1.0f;
        }
        return q * s;
    }
    if (shape == 3u || shape == 4u) { // cone (base) e disco: no plano XZ
        const float r = surface ? 1.0f : std::sqrt(rand01(seed, 27u));
        const float a = rand01(seed, 28u) * 2.0f * kPi;
        return Vec3(std::cos(a), 0.0f, std::sin(a)) * s.x * r;
    }
    return Vec3(0.0f);
}

// Turbulencia do vfx_sim.comp (rotacional de um potencial de senos).
Vec3 curlNoise(const Vec3& p, float t) {
    const Vec3 a = p + Vec3(0.0f, t, 0.0f);
    const Vec3 b = p * 1.7f + Vec3(t * 0.7f, 0.0f, 3.1f);
    const float dPzDy = 1.7f * std::cos(b.y), dPyDz = std::cos(a.z);
    const float dPxDz = 1.7f * std::cos(b.z), dPzDx = std::cos(a.x);
    const float dPyDx = 1.7f * std::cos(b.x), dPxDy = std::cos(a.y);
    return Vec3(dPzDy - dPyDz, dPxDz - dPzDx, dPyDx - dPxDy) * 0.5f;
}

Vec4 sampleColorLut(const VfxEmitterGpu& e, float t) {
    const float x = std::clamp(t, 0.0f, 1.0f) * 15.0f;
    const uint32_t i = uint32_t(x);
    const uint32_t j = std::min(i + 1u, 15u);
    return glm::mix(glm::unpackUnorm4x8(e.colorLut[i]), glm::unpackUnorm4x8(e.colorLut[j]), x - float(i));
}

float sizeLutAt(const VfxEmitterGpu& e, uint32_t i) {
    const glm::vec2 h = glm::unpackHalf2x16(e.sizeLut[i >> 1]);
    return (i & 1u) == 0u ? h.x : h.y;
}
float sampleSizeLut(const VfxEmitterGpu& e, float t) {
    const float x = std::clamp(t, 0.0f, 1.0f) * 15.0f;
    const uint32_t i = uint32_t(x);
    return glm::mix(sizeLutAt(e, i), sizeLutAt(e, std::min(i + 1u, 15u)), x - float(i));
}

float smoothStep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

void WebVfxSim::clear() {
    m_pos.clear();
    m_vel.clear();
    m_age.clear();
    m_life.clear();
    m_size.clear();
    m_rot.clear();
    m_angVel.clear();
    m_startFrame.clear();
    m_slot.clear();
    m_gen.clear();
    m_seed.clear();
}

void WebVfxSim::kill(size_t i) {
    const size_t last = m_age.size() - 1;
    if (i != last) {
        m_pos[i] = m_pos[last];
        m_vel[i] = m_vel[last];
        m_age[i] = m_age[last];
        m_life[i] = m_life[last];
        m_size[i] = m_size[last];
        m_rot[i] = m_rot[last];
        m_angVel[i] = m_angVel[last];
        m_startFrame[i] = m_startFrame[last];
        m_slot[i] = m_slot[last];
        m_gen[i] = m_gen[last];
        m_seed[i] = m_seed[last];
    }
    m_pos.pop_back();
    m_vel.pop_back();
    m_age.pop_back();
    m_life.pop_back();
    m_size.pop_back();
    m_rot.pop_back();
    m_angVel.pop_back();
    m_startFrame.pop_back();
    m_slot.pop_back();
    m_gen.pop_back();
    m_seed.pop_back();
}

// vfxMakeParticle (vfx_spawn.glsl), sem o grafo.
void WebVfxSim::spawnOne(const VfxEmitterGpu& e, uint32_t slot, const Vec3& origin, uint32_t seed, float preAge) {
    const glm::mat3 rot(Vec3(e.rot0), Vec3(e.rot1), Vec3(e.rot2));
    const float scale = e.rot0.w;
    const Vec3 local = shapePoint(e, seed);
    const Vec3 axis = glm::normalize(Vec3(e.dirSpread) + Vec3(0.0f, 1e-6f, 0.0f));
    const uint32_t shape = e.flags.x & kShapeMask;
    const float halfAngle = shape == 3u ? e.shape.w : e.dirSpread.w * kPi;
    const Vec3 dir = halfAngle > 1e-4f ? coneDir(axis, halfAngle, seed) : axis;
    const float speed = glm::mix(e.life.z, e.life.w, rand01(seed, 2u)) * scale;
    const Vec3 vel = rot * dir * speed + Vec3(e.velInherit) * e.velInherit.w;
    m_pos.push_back(origin + rot * (local * scale) + vel * preAge);
    m_vel.push_back(vel);
    m_age.push_back(preAge);
    m_life.push_back(glm::mix(e.life.x, e.life.y, rand01(seed, 3u)));
    m_size.push_back(glm::mix(e.sizeRot.x, e.sizeRot.y, rand01(seed, 4u)) * scale);
    m_rot.push_back(glm::mix(e.sizeRot.z, e.sizeRot.w, rand01(seed, 5u)));
    m_angVel.push_back(glm::mix(e.angVel.x, e.angVel.y, rand01(seed, 6u)));
    m_startFrame.push_back((e.flags.x & kFlipRandom) != 0u ? rand01(seed, 7u) : 0.0f);
    m_slot.push_back(slot);
    m_gen.push_back(e.flags.y & 0xFFFFu);
    m_seed.push_back(seed);
}

void WebVfxSim::step(const VfxEmitterGpu* emitters, uint32_t slotCount,
                     const std::vector<VfxSpawnRequestGpu>& requests, float dt, float time, uint32_t capacity) {
    // Nascimento (vfx_emit.comp): a k-esima do pedido nasce no ponto e no
    // instante certos do caminho do efeito neste frame.
    for (const VfxSpawnRequestGpu& req : requests) {
        if (req.slot >= slotCount) continue;
        const VfxEmitterGpu& e = emitters[req.slot];
        const Vec3 prev(e.prevPos), cur(e.posTime);
        for (uint32_t k = 0; k < req.count && m_age.size() < capacity; ++k) {
            const uint32_t seed = pcg(req.seed ^ pcg(k + 0x68E31DA4u));
            const float f = (float(k) + rand01(seed, 1u)) / float(std::max(req.count, 1u));
            spawnOne(e, req.slot, glm::mix(prev, cur, f), seed, (1.0f - f) * dt);
        }
    }

    // Simulacao (vfx_sim.comp), sem a colisao com a profundidade/grade.
    for (size_t i = 0; i < m_age.size();) {
        const uint32_t slot = m_slot[i];
        bool alive = slot < slotCount;
        if (alive) {
            const VfxEmitterGpu& e = emitters[slot];
            alive = (e.flags.y & 0xFFFFu) == m_gen[i] && (e.flags.x & kKilled) == 0u;
            m_age[i] += dt;
            alive = alive && m_age[i] < m_life[i];
            if (alive) {
                const float scale = e.rot0.w;
                Vec3 acc = Vec3(e.gravityDrag) * scale;
                if (e.noise.x != 0.0f)
                    acc += curlNoise(m_pos[i] * (e.noise.y / scale), time * e.noise.z) * e.noise.x * scale;
                Vec3 vel = m_vel[i] + acc * dt;
                vel *= 1.0f / (1.0f + e.gravityDrag.w * dt); // arrasto estavel com qualquer dt
                Vec3 next = m_pos[i] + vel * dt;
                if ((e.flags.x & kCollidePlane) != 0u && next.y < e.collide.x + 0.02f) {
                    next.y = e.collide.x + 0.02f;
                    vel.y = std::abs(vel.y) * e.noise.w;
                    vel.x *= 1.0f - e.collide.y;
                    vel.z *= 1.0f - e.collide.y;
                    m_age[i] += m_life[i] * e.collide.z;
                }
                m_pos[i] = next;
                m_vel[i] = vel;
                alive = m_age[i] < m_life[i] && std::isfinite(next.x) && std::isfinite(next.y) &&
                        std::isfinite(next.z) && std::isfinite(vel.x);
            }
        }
        if (alive) ++i;
        else kill(i);
    }
}

uint32_t WebVfxSim::buildQuads(const VfxEmitterGpu* emitters, uint32_t slotCount, const WebVfxView& view,
                               std::vector<WebVfxVertex>& verts, std::vector<uint32_t>& quadTex) {
    verts.clear();
    quadTex.clear();
    const size_t n = m_age.size();
    m_sortKey.resize(n);
    m_order.clear();
    const Vec4 wRow(view.viewProj[0][3], view.viewProj[1][3], view.viewProj[2][3], view.viewProj[3][3]);
    for (size_t i = 0; i < n; ++i) {
        if (m_slot[i] >= slotCount) continue;
        if (((emitters[m_slot[i]].flags.x >> kMeshShift) & 0xFu) != 0u) continue; // malha: sem desenho aqui
        const float w = glm::dot(wRow, Vec4(m_pos[i], 1.0f));
        if (w <= 0.0f) continue; // atras da camera
        m_sortKey[i] = w;
        m_order.push_back(uint32_t(i));
    }
    // De tras para frente, como os baldes do vfx_sort (a aditiva vai junto:
    // tudo sai num passe so', premultiplicado).
    std::sort(m_order.begin(), m_order.end(), [this](uint32_t a, uint32_t b) { return m_sortKey[a] > m_sortKey[b]; });

    verts.reserve(m_order.size() * 4);
    quadTex.reserve(m_order.size());
    const Vec3 lightDir = -glm::normalize(Vec3(view.sunDir));
    static const float kCorners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    // Duas voltas: primeiro as de meia resolucao (camada propria, como o
    // drawLowRes do desktop), depois as cheias; cada uma de tras para frente.
    uint32_t lowQuads = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i : m_order) {
            const VfxEmitterGpu& e = emitters[m_slot[i]];
            if (((e.flags.x & kLowRes) != 0u) != (pass == 0)) continue;
            const Vec3 pos = m_pos[i];
            const float viewZ = m_sortKey[i];
            const float t = std::clamp(m_age[i] / m_life[i], 0.0f, 1.0f);
            float size = m_size[i] * sampleSizeLut(e, t);
            // Teto de tamanho na tela (fumaca encostada na camera nao vira parede).
            size = std::min(size, e.render.z * std::max(viewZ, 1e-3f) / std::max(view.proj11, 1e-3f));
            const float speed = glm::length(m_vel[i]);
            const float stretchLen = e.angVel.z * speed;

            Vec4 col = sampleColorLut(e, t);
            col.r *= e.render.x;
            col.g *= e.render.x;
            col.b *= e.render.x;
            const Vec3 toCam = glm::normalize(view.camPos - pos + Vec3(0.0f, 1e-5f, 0.0f));
            if (e.render.y > 0.0f) {
                // Normal falsa entre "para a camera" e "para cima" (vfx_sim.comp).
                const Vec3 nrm = glm::normalize(toCam + Vec3(0.0f, 1.0f, 0.0f));
                const float wrap = std::clamp(glm::dot(nrm, lightDir) * 0.5f + 0.5f, 0.0f, 1.0f);
                const Vec3 lit = Vec3(view.ambient) * view.ambient.w + view.sunColor * view.sunDir.w * wrap;
                const Vec3 m = glm::mix(Vec3(1.0f), lit, e.render.y);
                col.r *= m.r;
                col.g *= m.g;
                col.b *= m.b;
            }
            col.a *= smoothStep(view.nearPlane, view.nearPlane + std::max(e.render.w * e.rot0.w, 1e-3f), viewZ);
            if (col.a <= 0.002f) continue; // o pixel descartaria (vfx_particle_frag.glsl)

            // Folha de quadros (vida, quadros/s ou sorteado fixo).
            const uint32_t cols = std::max(e.flags.w & 0xFFu, 1u);
            const uint32_t rows = std::max((e.flags.w >> 8) & 0xFFu, 1u);
            const float frames = float(cols * rows);
            const float fpos = e.flip.z > 1.5f ? std::floor(m_startFrame[i] * frames)
                                               : (e.flip.z > 0.5f ? m_age[i] * e.flip.x : t * (frames - 0.001f)) +
                                                     m_startFrame[i] * frames;
            const float f0 = std::fmod(std::floor(fpos), frames);
            const float f1 = std::fmod(f0 + 1.0f, frames);
            const float fmix = ((e.flags.x & kFlipBlend) != 0u && e.flip.z < 1.5f) ? fpos - std::floor(fpos) : 0.0f;
            const float cw = 1.0f / float(cols), ch = 1.0f / float(rows);
            const float u0 = std::fmod(f0, float(cols)) * cw, v0 = std::floor(f0 / float(cols)) * ch;
            const float u1 = std::fmod(f1, float(cols)) * cw, v1 = std::floor(f1 / float(cols)) * ch;

            const uint32_t facing = (e.flags.x >> kFacingShift) & 3u;
            const uint32_t blend = (e.flags.x >> kBlendShift) & 3u;
            const float rot = m_rot[i] + m_angVel[i] * m_age[i];
            const float cs = std::cos(rot), sn = std::sin(rot);
            const Vec3 axis = speed > 1e-5f ? m_vel[i] / speed : Vec3(0.0f, 1.0f, 0.0f);
            Vec3 side = glm::cross(axis, toCam);
            side = glm::dot(side, side) > 1e-8f ? glm::normalize(side) : view.camRight;
            const float soft = e.angVel.w * e.rot0.w;
            for (const auto& c : kCorners) {
                const float rx = c[0] * cs - c[1] * sn, ry = c[0] * sn + c[1] * cs;
                Vec3 world;
                if (facing == 1u) world = pos + side * (c[0] * size) + axis * (c[1] * (size + stretchLen));
                else if (facing == 2u) world = pos + Vec3(rx, 0.0f, ry) * size;
                else world = pos + (view.camRight * rx + view.camUp * ry) * size; // camera (e fita, aproximada)
                const float uu = c[0] * 0.5f + 0.5f, vv = 0.5f - c[1] * 0.5f;
                WebVfxVertex v;
                v.pos[0] = world.x;
                v.pos[1] = world.y;
                v.pos[2] = world.z;
                v.uv[0] = u0 + uu * cw;
                v.uv[1] = v0 + vv * ch;
                v.uv[2] = u1 + uu * cw;
                v.uv[3] = v1 + vv * ch;
                v.color[0] = col.r;
                v.color[1] = col.g;
                v.color[2] = col.b;
                v.color[3] = col.a;
                v.misc[0] = fmix;
                v.misc[1] = -1.0f; // unidade de textura: quem desenha preenche
                v.misc[2] = soft;
                v.misc[3] = viewZ;
                v.corner[0] = c[0];
                v.corner[1] = c[1];
                v.corner[2] = float(blend);
                verts.push_back(v);
            }
            quadTex.push_back(e.flags.z);
            if (pass == 0) ++lowQuads;
        }
    }
    return lowQuads;
}

} // namespace eruption
