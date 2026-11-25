#include "renderer/vfx/VfxBolt.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

// xorshift32: o sorteio de um raio e' determinista pela semente do sorteio.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(s & 0xFFFFFFu) / float(0x1000000u);
    }
    float range(float a, float b) { return a + (b - a) * next(); }
};

// Direcao aleatoria perpendicular a `dir` (unitaria).
Vec3 randomPerp(Rng& r, const Vec3& dir) {
    Vec3 v(r.range(-1.0f, 1.0f), r.range(-1.0f, 1.0f), r.range(-1.0f, 1.0f));
    v -= dir * glm::dot(v, dir);
    const float l = glm::length(v);
    return l > 1e-4f ? v / l : Vec3(1.0f, 0.0f, 0.0f);
}

// Deslocamento do ponto medio: cada nivel parte cada segmento ao meio e empurra o
// meio para o lado por `jitter` * comprimento do segmento (o desvio cai por
// metade a cada nivel porque o segmento tambem cai).
std::vector<Vec3> midpointDisplace(Vec3 a, Vec3 b, float jitter, int levels, Rng& r) {
    std::vector<Vec3> pts{a, b};
    for (int lv = 0; lv < levels; ++lv) {
        std::vector<Vec3> next;
        next.reserve(pts.size() * 2);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec3 p = pts[i], q = pts[i + 1];
            const Vec3 d = q - p;
            const float len = glm::length(d);
            next.push_back(p);
            if (len > 1e-4f) {
                const Vec3 dir = d / len;
                next.push_back((p + q) * 0.5f + randomPerp(r, dir) * (len * jitter * 0.5f * r.range(0.4f, 1.0f)));
            }
        }
        next.push_back(pts.back());
        pts.swap(next);
    }
    return pts;
}

// Perfil suave (alfa) de uma fita: `across` = 0..1 atravessando a largura.
// Textura RGB branca, alfa = perfil (a cor e' do vertice).
std::vector<uint8_t> makeProfile(int w, int h, float sigma, float plateau, bool radial) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float fx = (float(x) + 0.5f) / float(w) * 2.0f - 1.0f;
            const float fy = (float(y) + 0.5f) / float(h) * 2.0f - 1.0f;
            const float r = radial ? std::sqrt(fx * fx + fy * fy) : std::fabs(fx);
            const float t = std::max(0.0f, r - plateau) / sigma;
            const float a = r >= 1.0f ? 0.0f : std::exp(-t * t);
            // borda a zero: sem corte duro no fim da fita
            const float edge = 1.0f - std::pow(std::min(r, 1.0f), 6.0f);
            px[(size_t(y) * w + x) * 4 + 3] = uint8_t(std::clamp(a * edge, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return px;
}

// Niveis de subdivisao para segmentos de ~2,5 m (230 m -> 7 niveis, 128 segmentos). Com
// 5 niveis fixos o raio de 230 m tinha segmentos de 7 m e desvios de ~1 m no fim: uma reta.
// O canal de um raio real anda em passos de dezenas de metros, com curvas de ate' ~30 graus
// a cada passo e quebras finas dentro de cada passo; a subdivisao por ponto medio com o
// desvio caindo pela metade por nivel da as duas escalas.
int levelsFor(float length) {
    return std::clamp(int(std::ceil(std::log2(std::max(length, 1.0f) / 2.5f))), 3, 9);
}

} // namespace

bool VfxBolt::init(ImmediateDraw& draw) {
    const auto halo = makeProfile(64, 4, 0.42f, 0.0f, false);
    const auto core = makeProfile(64, 4, 0.20f, 0.12f, false);
    m_texHalo = draw.addTexture("engine:bolt_halo", halo.data(), 64, 4);
    m_texCore = draw.addTexture("engine:bolt_core", core.data(), 64, 4);
    return m_texHalo >= 0 && m_texCore >= 0;
}

uint32_t VfxBolt::start(const BoltSpec& spec) {
    if (m_bolts.size() >= kMaxBolts) return 0;
    Bolt b;
    b.id = m_nextId++;
    b.spec = spec;
    b.spec.branches = std::clamp(spec.branches, 0, 12);
    b.spec.jitter = std::clamp(spec.jitter, 0.0f, 1.0f);
    b.spec.flickerMs = std::max(spec.flickerMs, 16.0f);
    b.spec.width = std::clamp(spec.width, 0.05f, 30.0f);
    roll(b);
    m_bolts.push_back(std::move(b));
    return m_bolts.back().id;
}

bool VfxBolt::stop(uint32_t id) {
    for (size_t i = 0; i < m_bolts.size(); ++i) {
        if (m_bolts[i].id == id) {
            m_bolts.erase(m_bolts.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

// Novo trajeto: tronco por deslocamento do ponto medio (5 niveis = 32 segmentos)
// e galhos que saem do tronco abaixo da nuvem, apontando para fora e para baixo,
// afinando ate' zero na ponta.
void VfxBolt::roll(Bolt& b) {
    Rng r(0xA511E9B3u ^ (b.id * 2654435761u) ^ (b.rolls * 0x9E3779B1u));
    ++b.rolls;
    b.polys.clear();
    b.gain = r.range(0.6f, 1.0f);
    const BoltSpec& s = b.spec;
    const float baseY = s.cloudBaseY;
    auto diffuseAt = [&](float y) { return s.cloudId ? glm::smoothstep(baseY, baseY + 8.0f, y) : 0.0f; };

    Poly trunk;
    const std::vector<Vec3> path = midpointDisplace(s.from, s.to, s.jitter, levelsFor(glm::length(s.to - s.from)), r);
    float total = 0.0f;
    for (size_t i = 1; i < path.size(); ++i) total += glm::length(path[i] - path[i - 1]);
    float run = 0.0f;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) run += glm::length(path[i] - path[i - 1]);
        const float t = total > 1e-4f ? run / total : 0.0f;
        trunk.pts.push_back({path[i], std::max(0.02f, s.width * (1.0f - 0.7f * t)), diffuseAt(path[i].y)});
    }
    b.polys.push_back(trunk);

    for (int k = 0; k < s.branches; ++k) {
        // Ponto de saida: entre 10% e 88% do tronco, fora da nuvem.
        size_t idx = 0;
        bool ok = false;
        for (int tries = 0; tries < 6 && !ok; ++tries) {
            idx = size_t(r.range(0.10f, 0.88f) * float(trunk.pts.size() - 1));
            ok = trunk.pts[idx].diffuse < 0.3f;
        }
        if (!ok || idx + 1 >= trunk.pts.size()) continue;
        const Vec3 start = trunk.pts[idx].p;
        const Vec3 along = glm::normalize(trunk.pts[idx + 1].p - trunk.pts[idx].p);
        const float ang = r.range(0.35f, 0.8f); // 20..46 graus do tronco: galhos abertos, nao colados nele
        Vec3 dir = along * std::cos(ang) + randomPerp(r, along) * std::sin(ang);
        dir.y -= 0.3f; // galhos tendem a descer
        dir = glm::normalize(dir);
        const float len = total * r.range(0.16f, 0.34f);
        const std::vector<Vec3> bp = midpointDisplace(start, start + dir * len, s.jitter * 1.2f, levelsFor(len), r);
        Poly br;
        br.inten = 0.8f;
        const float w0 = trunk.pts[idx].w * 0.6f;
        for (size_t i = 0; i < bp.size(); ++i) {
            const float t = float(i) / float(bp.size() - 1);
            br.pts.push_back({bp[i], std::max(0.01f, w0 * std::pow(1.0f - t, 1.0f)), 0.0f});
        }
        // Galho do galho (40%): um raio ramifica varias vezes, nao so' uma.
        if (br.pts.size() > 8 && r.next() < 0.4f) {
            const size_t j = size_t(r.range(0.3f, 0.7f) * float(br.pts.size() - 1));
            const Vec3 bd = glm::normalize(br.pts[j + 1].p - br.pts[j].p);
            Vec3 cd = bd * std::cos(0.6f) + randomPerp(r, bd) * std::sin(0.6f);
            cd.y -= 0.2f;
            cd = glm::normalize(cd);
            const float clen = len * r.range(0.3f, 0.5f);
            const std::vector<Vec3> cp = midpointDisplace(br.pts[j].p, br.pts[j].p + cd * clen, s.jitter * 1.2f, levelsFor(clen), r);
            Poly child;
            child.inten = 0.6f;
            for (size_t i = 0; i < cp.size(); ++i) {
                const float t = float(i) / float(cp.size() - 1);
                child.pts.push_back({cp[i], std::max(0.01f, br.pts[j].w * 0.7f * (1.0f - t)), 0.0f});
            }
            b.polys.push_back(std::move(child));
        }
        b.polys.push_back(std::move(br));
    }
    b.nextRoll = b.age + s.flickerMs * 0.001f;
    if (onFlash && s.cloudId)
        onFlash(s.cloudId, s.intensity * (0.8f + 1.6f * b.gain), std::max(0.12f, s.flickerMs * 0.003f));
}

float VfxBolt::envelope(const Bolt& b) const {
    const float rise = std::min(1.0f, b.age / 0.03f);
    if (b.spec.life <= 0.0f) return rise;
    return rise * (1.0f - glm::smoothstep(0.65f * b.spec.life, b.spec.life, b.age));
}

void VfxBolt::update(float dt) {
    for (size_t i = 0; i < m_bolts.size();) {
        Bolt& b = m_bolts[i];
        b.age += dt;
        if (b.spec.life > 0.0f && b.age >= b.spec.life) {
            m_bolts.erase(m_bolts.begin() + static_cast<long>(i));
            continue;
        }
        if (b.age >= b.nextRoll) roll(b);
        ++i;
    }
}

void VfxBolt::submit(ImmediateDraw& draw, const Vec3& camPos) {
    m_lastVertices = 0;
    if (m_bolts.empty() || m_texHalo < 0) return;
    ImmediateDraw::Flags flags;
    flags.blend = ImmediateDraw::Blend::Additive;
    flags.depthTest = true;
    flags.cull = false;
    // Duas camadas: halo (largo, cor do raio) e nucleo (estreito, quente). Uma
    // chamada por camada e por raio, com todos os segmentos em quads de 4 vertices.
    for (int layer = 0; layer < 2; ++layer) {
        for (const Bolt& b : m_bolts) {
            uint32_t quads = 0;
            for (const Poly& p : b.polys) quads += uint32_t(p.pts.size() > 1 ? p.pts.size() - 1 : 0);
            ImmediateDraw::Vertex* v = draw.addGeometry(layer == 0 ? m_texHalo : m_texCore,
                                                        ImmediateDraw::Topology::Quads, flags, quads * 4);
            if (!v) continue;
            const float env = envelope(b) * b.gain * b.spec.intensity;
            // Brilho limitado ANTES do bloom: com o nucleo a 7 e o halo a 2,2 a soma passava
            // de 10 no HDR e o bloom o transformava num tubo branco chapado. Nucleo ~2,5 e
            // halo ~0,9 ficam saturados so' no miolo e deixam a cor do raio no resto.
            const Vec3 hue = layer == 0 ? b.spec.color * 0.9f : b.spec.coreColor * 2.5f;
            for (const Poly& p : b.polys) {
                const size_t n = p.pts.size();
                if (n < 2) continue;
                // Lado de cada junta (a media das direcoes vizinhas, virado para a camera).
                std::vector<Vec3> side(n);
                for (size_t i = 0; i < n; ++i) {
                    const Vec3 d = p.pts[std::min(i + 1, n - 1)].p - p.pts[i > 0 ? i - 1 : 0].p;
                    Vec3 sd = glm::cross(d, camPos - p.pts[i].p);
                    const float l = glm::length(sd);
                    side[i] = l > 1e-5f ? sd / l : Vec3(1.0f, 0.0f, 0.0f);
                }
                for (size_t i = 0; i + 1 < n; ++i) {
                    float hw[2], k[2];
                    for (int e = 0; e < 2; ++e) {
                        const Pt& pt = p.pts[i + size_t(e)];
                        // Nucleo fino (~1 px na tela, nunca abaixo disso: a fita some sub-pixel
                        // a distancia) e halo ~6x o nucleo, mais fraco. A largura do script
                        // fica so' como teto do tronco perto da camera.
                        const float px = glm::length(camPos - pt.p) * 0.0012f;
                        const float core = std::max(pt.w * 0.12f, px * 0.8f);
                        if (layer == 0) {
                            hw[e] = core * (6.0f + 6.0f * pt.diffuse);
                            k[e] = (1.0f - 0.7f * pt.diffuse) * 0.5f;
                        } else {
                            hw[e] = core * (1.0f - 0.8f * pt.diffuse);
                            k[e] = (1.0f - 0.9f * pt.diffuse);
                        }
                    }
                    const Vec3 a0 = p.pts[i].p, a1 = p.pts[i + 1].p;
                    const Vec3 c[4] = {a0 - side[i] * hw[0], a1 - side[i + 1] * hw[1], a1 + side[i + 1] * hw[1],
                                       a0 + side[i] * hw[0]};
                    const float u[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                    const float vv[4] = {0.0f, 1.0f, 1.0f, 0.0f};
                    const float kk[4] = {k[0], k[1], k[1], k[0]};
                    for (int q = 0; q < 4; ++q, ++v) {
                        const float f = env * p.inten * kk[q];
                        v->x = c[q].x; v->y = c[q].y; v->z = c[q].z;
                        v->u = u[q]; v->v = vv[q];
                        v->r = hue.x * f; v->g = hue.y * f; v->b = hue.z * f; v->a = 1.0f;
                    }
                }
            }
            m_lastVertices += quads * 4;
        }
    }
}

void VfxBolt::collectLights(std::vector<PointLight>& out, size_t maxLights) const {
    for (const Bolt& b : m_bolts) {
        if (b.spec.lightRadius <= 0.0f || out.size() >= maxLights) continue;
        const float k = envelope(b) * b.gain;
        // Saida (na nuvem, ilumina a nuvem e o ar) e impacto (ilumina o chao).
        for (int e = 0; e < 2 && out.size() < maxLights; ++e) {
            PointLight l;
            l.position = e == 0 ? b.spec.from : b.spec.to + Vec3(0.0f, 1.5f, 0.0f);
            l.radius = b.spec.lightRadius;
            l.color = b.spec.color * 0.6f + Vec3(0.4f);
            l.intensity = b.spec.lightIntensity * k * (e == 0 ? 0.6f : 1.0f);
            l.steadyIntensity = l.intensity;
            // Nunca raio de luz de tela: com o padrao (true) as duas luzes do raio, no
            // meio da imagem, viravam um veu branco sobre tudo (god_rays.frag).
            l.godRays = false;
            out.push_back(l);
        }
    }
}

} // namespace eruption
