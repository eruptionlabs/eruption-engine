#include "renderer/vfx/CloudPrecip.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

float frac(float x) { return x - std::floor(x); }

float frand(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return float(s & 0xFFFFFFu) / float(0x1000000u);
}

// Particulas por m2 de area com rate = 1 (visual: flocos esparsos, chuva mais densa).
constexpr float kSnowPerM2 = 0.1f;
constexpr float kNearFadeStart = 25.0f; // m: abaixo disso o floco ja' sumiu (vira disco de DOF)
constexpr float kNearFadeEnd = 60.0f;
constexpr float kRainPerM2 = 0.18f;
constexpr float kSnowSpeed = 4.0f;   // m/s
constexpr float kRainSpeed = 38.0f;  // m/s
constexpr float kRainLength = 4.5f;  // m

std::vector<uint8_t> makeDot(int n) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 255);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float fx = (float(x) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float fy = (float(y) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float r = std::sqrt(fx * fx + fy * fy);
            const float a = r >= 1.0f ? 0.0f : std::exp(-(r * 2.0f) * (r * 2.0f)) * (1.0f - std::pow(r, 6.0f));
            px[(size_t(y) * n + x) * 4 + 3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return px;
}

// Floco: cristal de 6 bracos. Atlas 2x2 (n px por celula) com 4 variacoes: haste fina com
// ramos, haste grossa com ramos curtos, placa hexagonal com miolo, estrela de ramos longos.
// O alfa leva a forma; a cor vem do vertice (ja' iluminada pela cena).
float crystalAlpha(int variant, float fx, float fy) {
    const float r = std::sqrt(fx * fx + fy * fy);
    if (r >= 1.0f) return 0.0f;
    constexpr float kSixth = 1.04719755f; // 60 graus
    float th = std::atan2(fy, fx);
    // Dobra o angulo para o braco mais proximo (periodo de 60 graus, braco em 0).
    float t = std::fmod(th + 6.2831853f * 2.0f, kSixth);
    if (t > 0.5f * kSixth) t -= kSixth;
    const float along = r * std::cos(t), perp = std::fabs(r * std::sin(t));
    static const float kWidth[4] = {0.055f, 0.10f, 0.07f, 0.045f};
    static const float kBranchAt[4] = {0.45f, 0.35f, 0.62f, 0.30f};
    static const float kBranchLen[4] = {0.30f, 0.22f, 0.20f, 0.45f};
    const float w = kWidth[variant];
    float d = perp - w * (1.0f - 0.35f * along); // <0 dentro da haste
    if (along > 0.98f) d = std::max(d, along - 0.98f);
    // Ramos a +-60 graus da haste, comecando em kBranchAt.
    const float bs = along - kBranchAt[variant];
    if (bs > 0.0f && bs < kBranchLen[variant]) {
        const float bd = std::fabs(perp - bs * 1.2f) - w * 0.75f;
        d = std::min(d, bd);
    }
    if (variant == 2) d = std::min(d, r - 0.30f); // placa central
    d = std::min(d, r - 0.12f);                   // miolo
    return std::clamp(0.5f - d / 0.06f, 0.0f, 1.0f) * std::clamp((1.0f - r) / 0.08f, 0.0f, 1.0f);
}

std::vector<uint8_t> makeFlakeAtlas(int n) {
    const int w = n * 2;
    std::vector<uint8_t> px(size_t(w) * w * 4, 255);
    for (int y = 0; y < w; ++y) {
        for (int x = 0; x < w; ++x) {
            const int cell = (y / n) * 2 + (x / n);
            const float fx = (float(x % n) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float fy = (float(y % n) + 0.5f) / float(n) * 2.0f - 1.0f;
            px[(size_t(y) * w + x) * 4 + 3] = uint8_t(crystalAlpha(cell, fx, fy) * 255.0f + 0.5f);
        }
    }
    return px;
}

std::vector<uint8_t> makeStreak(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float fx = (float(x) + 0.5f) / float(w) * 2.0f - 1.0f; // atravessa
            const float fy = (float(y) + 0.5f) / float(h);               // ao longo: cabeca em 1
            const float a = std::exp(-(fx * 1.8f) * (fx * 1.8f)) * std::pow(fy, 1.5f) * (1.0f - std::pow(std::fabs(fx), 6.0f));
            px[(size_t(y) * w + x) * 4 + 3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return px;
}

} // namespace

bool CloudPrecip::init(ImmediateDraw& draw) {
    const auto dot = makeDot(32);
    const auto streak = makeStreak(8, 32);
    m_texDot = draw.addTexture("engine:precip_dot", dot.data(), 32, 32);
    m_texStreak = draw.addTexture("engine:precip_streak", streak.data(), 8, 32);
    const auto flake = makeFlakeAtlas(48);
    m_texFlake = draw.addTexture("engine:precip_crystal", flake.data(), 96, 96);
    return m_texDot >= 0 && m_texStreak >= 0 && m_texFlake >= 0;
}

uint32_t CloudPrecip::particleCount() const {
    uint32_t n = 0;
    for (const auto& kv : m_states) n += uint32_t(kv.second.parts.size());
    return n;
}

void CloudPrecip::update(float dt, const std::vector<Params>& emitters, const GroundFn& ground) {
    // Emissores que sumiram da lista saem do mapa.
    for (auto it = m_states.begin(); it != m_states.end();) {
        const bool alive = std::any_of(emitters.begin(), emitters.end(), [&](const Params& p) { return p.id == it->first; });
        it = alive ? std::next(it) : m_states.erase(it);
    }
    m_active.clear();
    uint32_t budget = kMaxTotal;
    for (const Params& p : emitters) {
        State& st = m_states[p.id];
        if (st.rng == 1) st.rng = 0x9E3779B9u ^ (p.id * 2246822519u);
        const float perM2 = p.snow ? kSnowPerM2 : kRainPerM2;
        const float area = 3.14159265f * p.radius * p.radius;
        // Coluna alta (nuvem no ceu): mais particulas, senao 230 m de queda ficam vazios.
        const float colH = std::clamp((p.baseY - (ground ? ground(p.center.x, p.center.y) : 0.0f)) / 40.0f, 1.0f, 4.0f);
        uint32_t want = uint32_t(std::clamp(perM2 * area * colH * std::clamp(p.rate, 0.0f, 3.0f), 0.0f, float(kMaxPerEmitter)));
        want = std::min(want, budget);
        budget -= want;
        st.time += dt;
        st.last = p;
        st.groundY = ground ? ground(p.center.x, p.center.y) : 0.0f;
        const float column = std::max(p.baseY - st.groundY, 1.0f);
        auto spawn = [&](Particle& q, bool anywhere) {
            const float a = frand(st.rng) * 6.2831853f;
            const float r = std::sqrt(frand(st.rng)) * p.radius;
            q.ox = std::cos(a) * r;
            q.oz = std::sin(a) * r;
            // Na criacao, uniforme na coluna; depois de cair, volta ao topo (a base).
            q.y = anywhere ? st.groundY + frand(st.rng) * column : p.baseY - frand(st.rng) * 2.0f;
            q.phase = frand(st.rng) * 6.2831853f;
            // Tamanhos bem variados (maioria miuda, alguns grandes): o floco uniforme lia como bokeh.
            { const float u = frand(st.rng); q.size = p.snow ? 0.08f + 0.6f * u * u : 1.0f; }
        };
        if (st.parts.size() > want) st.parts.resize(want);
        while (st.parts.size() < want) {
            Particle q;
            spawn(q, true);
            st.parts.push_back(q);
        }
        const float speed = p.snow ? kSnowSpeed : kRainSpeed;
        for (Particle& q : st.parts) {
            q.y -= speed * dt * (p.snow ? (0.7f + 0.3f * std::sin(q.phase * 3.0f)) : 1.0f);
            if (p.snow) {
                // Vento do emissor (rajada lenta, direcao propria por id) mais o
                // balanco de cada floco: o floco anda de lado e oscila, nao cai reto.
                const float ang = float(p.id) * 1.7f + std::sin(st.time * 0.23f) * 0.6f;
                const float gust = 1.6f + 0.9f * std::sin(st.time * 0.61f + float(p.id));
                q.ox += (std::cos(ang) * gust + std::sin(st.time * 1.3f + q.phase) * 1.4f) * dt;
                q.oz += (std::sin(ang) * gust + std::cos(st.time * 1.1f + q.phase * 1.3f) * 1.4f) * dt;
            } else {
                q.ox += 1.2f * dt; // chuva inclinada pelo vento
            }
            // O vento arrasta para fora do disco: o que passa da borda reentra no topo. A
            // coluna e' um cilindro reto de raio `radius`, sem velocidade radial.
            if (q.ox * q.ox + q.oz * q.oz > p.radius * p.radius) { spawn(q, false); continue; }
            const float gy = ground ? ground(p.center.x + q.ox, p.center.y + q.oz) : st.groundY;
            if (q.y < gy || q.y > p.baseY + 2.0f) spawn(q, false);
        }
        m_active.push_back(p.id);
    }
}

void CloudPrecip::submit(ImmediateDraw& draw, const Vec3& camPos, const Vec3& camRight, const Vec3& camUp,
                         const Vec3& snowLit) {
    m_lastVertices = 0;
    if (m_active.empty() || m_texDot < 0) return;
    ImmediateDraw::Flags flags;
    flags.blend = ImmediateDraw::Blend::Additive;
    flags.depthTest = true;
    // Flocos: opacos (mistura por alfa, cor ja' iluminada), sem emissao. Aditivo + 1,5 de
    // brilho os fazia faiscas no bloom.
    ImmediateDraw::Flags flakeFlags = flags;
    flakeFlags.blend = ImmediateDraw::Blend::Alpha;
    // Chuva: tambem por mistura de alfa, nao aditiva. Gota d'agua vista em contraluz
    // sob nuvem escura e' cinza-azulada e fosca; o aditivo a fazia luminosa, mais clara
    // que a propria nuvem.
    const float litF = glm::clamp(snowLit.x / 0.95f, 0.15f, 1.0f); // nivel de luz da cena (0..1)
    for (uint32_t id : m_active) {
        auto it = m_states.find(id);
        if (it == m_states.end() || it->second.parts.empty()) continue;
        const State& st = it->second;
        const Params& p = st.last;
        const uint32_t n = uint32_t(st.parts.size());
        ImmediateDraw::Vertex* v = draw.addGeometry(p.snow ? m_texFlake : m_texStreak, ImmediateDraw::Topology::Quads,
                                                    flakeFlags, n * 4);
        if (!v) continue;
        const Vec3 hue = p.snow ? snowLit : Vec3(0.20f, 0.26f, 0.34f) * (0.45f + 0.55f * litF);
        // Tamanho minimo em pixels: a 375 u um floco de 1 m some (menos de 1 px).
        // Cresce com a distancia da camera ao centro da coluna (~3,5 px a 45 graus de fov).
        const float distC = glm::length(Vec3(p.center.x, 0.5f * (p.baseY + st.groundY), p.center.y) - camPos);
        const float pxWorld = distC * 0.0012f;
        // Vento do emissor (mesma formula da simulacao): alonga e inclina o floco na tela.
        const float wAng = float(p.id) * 1.7f + std::sin(st.time * 0.23f) * 0.6f;
        const float wGust = 1.6f + 0.9f * std::sin(st.time * 0.61f + float(p.id));
        const Vec3 vel(std::cos(wAng) * wGust, p.snow ? -kSnowSpeed : -kRainSpeed, std::sin(wAng) * wGust);
        const float vx = glm::dot(vel, camRight), vy = glm::dot(vel, camUp);
        const float vl = std::max(std::sqrt(vx * vx + vy * vy), 1e-4f);
        const Vec3 velR = (camRight * vx + camUp * vy) / vl; // direcao do movimento no plano da tela
        const Vec3 velP = glm::cross(glm::cross(camRight, camUp), velR);
        // Alongamento pela velocidade na tela (medido por olho: 1 + 0,06/(m/s), ate' 1,5).
        const float stretch = 1.0f + std::min(1.0f, vl * 0.12f); // ate' 2x: o vento risca o floco
        for (const Particle& q : st.parts) {
            const Vec3 pos(p.center.x + q.ox, q.y, p.center.y + q.oz);
            // Some perto da base (sai de dentro da nuvem) e no chao.
            float fade = glm::smoothstep(p.baseY - 0.0f, p.baseY - 8.0f, q.y) * glm::smoothstep(st.groundY, st.groundY + 2.0f, q.y);
            Vec3 ax, ay; // eixos do quad: ax atravessa, ay vai ao longo
            float hw, hh;
            if (p.snow) {
                // Flocos muito perto da camera saem do plano de foco e o DOF os faz discos
                // borrados: somem antes de chegar la.
                // Distancia absoluta (m) e nao relativa a coluna: o corte nao pode depender
                // da perspectiva (recortava a coluna em cone). A camera de jogo (centenas de
                // metros) nunca entra nessa rampa; so' quem passa perto de um floco.
                fade *= glm::smoothstep(kNearFadeStart, kNearFadeEnd, glm::length(pos - camPos));
                // Tamanho em pixels varia por floco (fase); menor que antes (era 0,4..1,2 px de 0,12%).
                // Medido na camera de jogo (~375 u): com 0,3..0,85 o floco tinha 1 a 4 px e virava ponto
                // redondo; de 0,9 a 1,7 (4 a 8 px de largura) a forma do cristal aparece. Cresce com a
                // distancia como o resto, entao de perto nao vira o disco de DOF (o corte de perto fica).
                const float pxv = 0.90f + 0.80f * (q.phase / 6.2831853f);
                hw = std::max(q.size * 0.4f, pxWorld * pxv);
                hh = hw * stretch;
                // Balanco: o floco gira em torno do eixo de queda (+-1 rad), cada um com fase propria.
                const float fl = std::sin(st.time * (1.4f + 0.9f * frac(q.phase * 3.1f)) + q.phase) * 1.0f;
                const float cs = std::cos(fl), sn = std::sin(fl);
                ay = velR * cs + velP * sn;
                ax = velP * cs - velR * sn;
            } else {
                ay = Vec3(0.0f, 1.0f, 0.0f);
                Vec3 sd = glm::cross(ay, camPos - pos);
                const float l = glm::length(sd);
                ax = l > 1e-5f ? sd / l : camRight;
                hw = std::max(0.05f, pxWorld * 0.6f); hh = std::max(kRainLength, pxWorld * 9.0f) * 0.5f;
            }
            const float f = fade * p.alpha;
            const Vec3 c[4] = {pos - ax * hw - ay * hh, pos + ax * hw - ay * hh, pos + ax * hw + ay * hh, pos - ax * hw + ay * hh};
            const float u[4] = {0.0f, 1.0f, 1.0f, 0.0f};
            // Variante do cristal (celula do atlas 2x2); chuva usa a textura inteira.
            const int var = p.snow ? int(frac(q.phase * 7.31f) * 4.0f) & 3 : 0;
            const float u0 = p.snow ? float(var & 1) * 0.5f : 0.0f, v0 = p.snow ? float(var >> 1) * 0.5f : 0.0f;
            const float us = p.snow ? 0.5f : 1.0f;
            // Fita da gota: cauda em v = 0 (alto), cabeca em v = 1 (baixo).
            for (int k = 0; k < 4; ++k, ++v) {
                v->x = c[k].x; v->y = c[k].y; v->z = c[k].z;
                v->u = u0 + u[k] * us;
                v->v = v0 + (k < 2 ? 1.0f : 0.0f) * us;
                v->r = hue.x; v->g = hue.y; v->b = hue.z;
                v->a = p.snow ? f : f * 0.6f; // chuva translucida
            }
        }
        m_lastVertices += n * 4;
    }
}

} // namespace eruption
