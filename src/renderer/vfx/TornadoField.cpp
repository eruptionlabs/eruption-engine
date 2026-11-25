#include "renderer/vfx/TornadoField.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

using tornado_shape::Shape;

// Hash determinista 0..1 por (semente, indice, canal): cada destroco e cada ciclo dele
// sorteiam o mesmo valor em qualquer quadro (nada de estado por peca).
float hash01(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t n = a * 747796405u + b * 2891336453u + c * 1274126177u + 12345u;
    n = (n ^ (n >> 16)) * 2246822519u;
    n = (n ^ (n >> 13)) * 3266489917u;
    n ^= n >> 16;
    return float(n & 0xFFFFFFu) / float(0x1000000u);
}

float frac(float x) { return x - std::floor(x); }

constexpr float kEjectAt = 0.86f; // fracao do ciclo em que a peca sai da parede e voa

} // namespace

uint32_t TornadoField::start(const TornadoSpec& spec, size_t maxCount) {
    if (m_list.size() >= std::min(maxCount, kMaxTornados)) return 0;
    Tornado t;
    t.id = m_nextId++;
    t.spec = spec;
    t.spec.height = std::max(spec.height, 10.0f);
    t.spec.radiusTop = std::max(spec.radiusTop, 1.0f);
    t.spec.radiusBottom = std::clamp(spec.radiusBottom, 0.5f, t.spec.radiusTop);
    t.spec.fade = std::max(spec.fade, 0.05f);
    // Direcao da inclinacao e fase do balanco: por id, para dois tornados nao serem gemeos.
    t.seed = hash01(t.id, 7u, 3u) * 6.2831853f;
    t.lastTrail = spec.pos;
    m_list.push_back(t);
    return t.id;
}

bool TornadoField::move(uint32_t id, const Vec3& pos) {
    for (Tornado& t : m_list) {
        if (t.id == id) {
            t.spec.pos = pos;
            return true;
        }
    }
    return false;
}

bool TornadoField::stop(uint32_t id) {
    for (Tornado& t : m_list) {
        if (t.id == id) {
            if (t.endAge < 0.0f) t.endAge = t.age;
            return true;
        }
    }
    return false;
}

void TornadoField::update(float dt) {
    for (Tornado& t : m_list) {
        t.age += dt;
        if (t.endAge < 0.0f && t.spec.life > 0.0f && t.age >= t.spec.life) t.endAge = t.age;
    }
    m_list.erase(std::remove_if(m_list.begin(), m_list.end(),
                                [](const Tornado& t) { return t.endAge >= 0.0f && t.age - t.endAge >= t.spec.fade; }),
                 m_list.end());
}

float TornadoField::intensity(const Tornado& t) {
    if (t.endAge < 0.0f) return 1.0f;
    const float u = std::clamp((t.age - t.endAge) / std::max(t.spec.fade, 0.01f), 0.0f, 1.0f);
    return 1.0f - u * u * (3.0f - 2.0f * u);
}

float TornadoField::touchdown(const Tornado& t) {
    const float u = std::clamp(t.age / 6.0f, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

Shape TornadoField::shapeOf(const Tornado& t) {
    Shape s;
    s.height = t.spec.height;
    s.radiusTop = t.spec.radiusTop;
    s.radiusBottom = t.spec.radiusBottom;
    s.seed = t.seed;
    return s;
}

// Um destroco sobe pela parede em espiral (raio e velocidade angular da propria
// parede) e, ao chegar perto do topo, sai pela tangente e cai em arco. O ciclo
// recomeca no chao com outro sorteio.
TornadoField::Piece TornadoField::pieceAt(const Tornado& t, uint32_t i) {
    const Shape s = shapeOf(t);
    const float period = 8.0f + 8.0f * hash01(t.id, i, 1u);
    const float x = t.age / period + hash01(t.id, i, 2u);
    const uint32_t cycle = uint32_t(std::max(std::floor(x), 0.0f));
    const float p = frac(x);
    const uint32_t k = i * 131u + cycle * 17u;

    Piece out;
    const float pick = hash01(t.id, k, 3u);
    out.kind = pick < 0.6f ? 0 : (pick < 0.88f ? 1 : 2);
    out.size = s.height * (out.kind == 0 ? 0.0030f + 0.0050f * hash01(t.id, k, 4u)
                                         : 0.0040f + 0.0060f * hash01(t.id, k, 4u));
    if (hash01(t.id, k, 11u) < 0.08f) out.size *= 2.4f; // alguns pedacos grandes
    out.rot = hash01(t.id, k, 5u) * 6.2831853f + t.age * (1.5f + 3.0f * hash01(t.id, k, 6u));

    const float theta0 = hash01(t.id, k, 7u) * 6.2831853f;
    const float spinMul = t.spec.spin * (0.7f + 0.6f * hash01(t.id, k, 8u));
    const float wallK = 0.55f + 0.7f * hash01(t.id, k, 9u);     // onde na espessura da parede
    const float r0 = tornado_shape::skirtRadius(s) * (0.5f + 1.6f * hash01(t.id, k, 10u)); // onde nasce, na poeira

    const float u = std::min(p / kEjectAt, 1.0f);
    const float h = std::pow(u, 2.3f) * 0.62f; // concentrados no terco de baixo e na saia
    const Vec2 base(t.spec.pos.x, t.spec.pos.z);
    const float age = t.age;
    const float wallR = tornado_shape::radiusAt(s, h, age) * wallK;
    const float enter = tornado_shape::smoothstep(0.0f, 0.3f, u);
    const float radial = r0 + (wallR - r0) * enter;
    // Angulo acumulado: integral aproximada da velocidade da parede ate' esta altura.
    const float tRise = u * period * kEjectAt;
    const float omega = tornado_shape::angularSpeed(spinMul * 1.25f, h * 0.5f, wallK);
    const float theta = theta0 + omega * tRise;
    const Vec2 axis = base + tornado_shape::axisOffset(s, h, age);
    Vec2 hp = axis + radial * Vec2(std::cos(theta), std::sin(theta));
    float y = t.spec.pos.y + h * s.height;

    if (p > kEjectAt) {
        // Voo livre: velocidade tangencial e radial da parede no instante da saida.
        const float tau = (p - kEjectAt) * period;
        const Vec2 tang(-std::sin(theta), std::cos(theta));
        const Vec2 outv(std::cos(theta), std::sin(theta));
        const float vt = omega * radial * 0.55f;
        hp += (tang * vt + outv * (vt * 0.20f)) * tau;
        y -= 0.5f * s.height * 0.09f * tau * tau - s.height * 0.05f * tau;
    }
    out.pos = Vec3(hp.x, y, hp.y);
    out.height01 = h;
    const float fadeIn = tornado_shape::smoothstep(0.0f, 0.05f, p);
    const float fadeOut = 1.0f - tornado_shape::smoothstep(0.93f, 1.0f, p);
    // Longe do eixo a peca some (nao fica confete solto pelo campo).
    const float dAxis = glm::length(hp - axis);
    const float reach = tornado_shape::radiusAt(s, h, age) + tornado_shape::skirtRadius(s) * 0.5f;
    const float far = 1.0f - tornado_shape::smoothstep(1.2f, 2.6f, dAxis / std::max(reach, 1.0f));
    out.alpha = fadeIn * fadeOut * far * intensity(t) * touchdown(t);
    return out;
}

} // namespace eruption
