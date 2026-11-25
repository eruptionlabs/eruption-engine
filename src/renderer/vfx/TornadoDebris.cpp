// Destrocos do tornado: texturas procedurais (pedra, folha, tabua) e a entrega dos sprites
// do quadro ao ImmediateDraw. Separado de TornadoField.cpp para a logica do tornado
// (estado, fade, caminho dos destrocos) rodar nos testes sem Vulkan.
#include "renderer/vfx/TornadoField.hpp"

#include "renderer/ImmediateDraw.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

float hash01(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t n = a * 747796405u + b * 2891336453u + c * 1274126177u + 12345u;
    n = (n ^ (n >> 16)) * 2246822519u;
    n = (n ^ (n >> 13)) * 3266489917u;
    n ^= n >> 16;
    return float(n & 0xFFFFFFu) / float(0x1000000u);
}

// Pedra: contorno irregular com sombreado (o alfa da a forma; o brilho fica no RGB).
std::vector<uint8_t> makeRock(int n) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 255);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float fx = (float(x) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float fy = (float(y) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float r = std::sqrt(fx * fx + fy * fy);
            const float th = std::atan2(fy, fx);
            const float edge = 0.72f + 0.14f * std::sin(3.0f * th + 1.0f) + 0.09f * std::sin(5.0f * th + 2.3f) +
                               0.05f * std::sin(9.0f * th);
            const float a = std::clamp((edge - r) * 12.0f, 0.0f, 1.0f);
            const float shade = std::clamp(0.62f + 0.38f * (-fx * 0.6f - fy * 0.8f) / std::max(edge, 0.2f), 0.25f, 1.0f);
            const float grain = 0.88f + 0.12f * std::sin(fx * 23.0f) * std::sin(fy * 19.0f);
            const uint8_t c = uint8_t(std::clamp(shade * grain, 0.0f, 1.0f) * 255.0f);
            uint8_t* p = &px[(size_t(y) * n + x) * 4];
            p[0] = p[1] = p[2] = c;
            p[3] = uint8_t(a * 255.0f + 0.5f);
        }
    }
    return px;
}

// Folha / pedaco de palha: lamina pontuda de 1 x 4 com nervura.
std::vector<uint8_t> makeLeaf(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float fx = (float(x) + 0.5f) / float(w) * 2.0f - 1.0f;
            const float fy = (float(y) + 0.5f) / float(h) * 2.0f - 1.0f;
            const float half = std::pow(std::max(1.0f - fy * fy, 0.0f), 0.7f) * 0.9f;
            const float a = std::clamp((half - std::fabs(fx)) * 10.0f, 0.0f, 1.0f);
            const float vein = 1.0f - 0.35f * std::exp(-fx * fx * 40.0f);
            const uint8_t c = uint8_t(std::clamp(vein * (0.8f + 0.2f * fy), 0.0f, 1.0f) * 255.0f);
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = p[1] = p[2] = c;
            p[3] = uint8_t(a * 255.0f + 0.5f);
        }
    }
    return px;
}

// Tabua: retangulo com veios e ponta quebrada.
std::vector<uint8_t> makePlank(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float fx = (float(x) + 0.5f) / float(w) * 2.0f - 1.0f;
            const float fy = (float(y) + 0.5f) / float(h) * 2.0f - 1.0f;
            const float broken = 0.92f - 0.25f * std::max(0.0f, fx * 0.9f + 0.4f) * (fy > 0.55f ? 1.0f : 0.0f);
            const float a = std::clamp((0.85f - std::fabs(fx)) * 10.0f, 0.0f, 1.0f) *
                            std::clamp((broken - std::fabs(fy)) * 10.0f, 0.0f, 1.0f);
            const float grain = 0.86f + 0.14f * std::sin(fx * 5.0f + std::sin(fy * 3.0f) * 1.3f);
            const uint8_t c = uint8_t(std::clamp(grain, 0.0f, 1.0f) * 255.0f);
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = p[1] = p[2] = c;
            p[3] = uint8_t(a * 255.0f + 0.5f);
        }
    }
    return px;
}

// Floco / ponto macio: disco com borda suave (neve, poeira fina).
std::vector<uint8_t> makeFlake(int n) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 255);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float fx = (float(x) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float fy = (float(y) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float r = std::sqrt(fx * fx + fy * fy);
            const float a = std::clamp((0.95f - r) / 0.55f, 0.0f, 1.0f);
            uint8_t* p = &px[(size_t(y) * n + x) * 4];
            p[0] = p[1] = p[2] = uint8_t(255.0f * (0.80f + 0.20f * (1.0f - r)));
            p[3] = uint8_t(a * a * 255.0f + 0.5f);
        }
    }
    return px;
}

// Cristal de gelo: estrela de seis pontas fina.
std::vector<uint8_t> makeCrystal(int n) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 255);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float fx = (float(x) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float fy = (float(y) + 0.5f) / float(n) * 2.0f - 1.0f;
            const float r = std::sqrt(fx * fx + fy * fy);
            const float th = std::atan2(fy, fx);
            const float arm = std::pow(std::max(std::cos(3.0f * th), 0.0f), 6.0f); // 6 pontas
            const float reach = 0.92f * arm + 0.22f;
            const float a = std::clamp((reach - r) * 7.0f, 0.0f, 1.0f);
            uint8_t* p = &px[(size_t(y) * n + x) * 4];
            p[0] = p[1] = p[2] = uint8_t(255.0f * (0.75f + 0.25f * (1.0f - r)));
            p[3] = uint8_t(a * 255.0f + 0.5f);
        }
    }
    return px;
}

} // namespace

bool TornadoField::init(ImmediateDraw& draw) {
    const auto rock = makeRock(32);
    const auto leaf = makeLeaf(12, 32);
    const auto plank = makePlank(10, 32);
    m_tex[0] = draw.addTexture("engine:tornado_rock", rock.data(), 32, 32);
    m_tex[1] = draw.addTexture("engine:tornado_leaf", leaf.data(), 12, 32);
    m_tex[2] = draw.addTexture("engine:tornado_plank", plank.data(), 10, 32);
    const auto flake = makeFlake(16);
    const auto crystal = makeCrystal(24);
    m_tex[3] = draw.addTexture("engine:tornado_flake", flake.data(), 16, 16);
    m_tex[4] = draw.addTexture("engine:tornado_crystal", crystal.data(), 24, 24);
    return m_tex[0] >= 0 && m_tex[1] >= 0 && m_tex[2] >= 0 && m_tex[3] >= 0 && m_tex[4] >= 0;
}

void TornadoField::submitDebris(ImmediateDraw& draw, uint32_t maxPieces, float lit) {
    m_lastPieces = 0;
    if (m_list.empty() || maxPieces == 0 || m_tex[0] < 0) return;
    ImmediateDraw::Flags flags;
    flags.blend = ImmediateDraw::Blend::Alpha;
    flags.depthTest = true;
    uint32_t withDebris = 0;
    for (const Tornado& t : m_list) withDebris += t.spec.debris ? 1u : 0u;
    // Neve fina em volta: flocos em queda por caminho analitico (sem estado), num cilindro em
    // torno do pe; ate' `kSnowfallMax` por tornado, so' para quem pediu `snowfall`.
    for (const Tornado& t : m_list) {
        if (t.spec.snowfall <= 0.0f) continue;
        const uint32_t n = uint32_t(float(kSnowfallMax) * t.spec.snowfall);
        const float reach = std::max(t.spec.radiusTop * 3.0f, t.spec.height * 0.25f);
        const float top = std::max(t.spec.height * 0.45f, 4.0f);
        const float flake = std::max(t.spec.height * 0.0016f, 0.05f);
        for (uint32_t i = 0; i < n; ++i) {
            const float a = hash01(t.id, i, 201u) * 6.2831853f;
            const float rad = reach * std::sqrt(hash01(t.id, i, 202u));
            const float fall = top * (0.35f + 0.25f * hash01(t.id, i, 203u)); // altura por segundo
            float yy = hash01(t.id, i, 204u) * top - t.age * fall;
            yy -= std::floor(yy / top) * top;
            const float swirl = a + t.age * (0.15f + 0.25f * hash01(t.id, i, 205u)) / (0.3f + rad / reach);
            const Vec3 pos(t.spec.pos.x + rad * std::cos(swirl), t.spec.pos.y + yy, t.spec.pos.z + rad * std::sin(swirl));
            const float edge = std::min(yy / (top * 0.1f), (top - yy) / (top * 0.1f));
            const float alpha = std::clamp(edge, 0.0f, 1.0f) * intensity(t) * 0.85f;
            if (alpha <= 0.01f) continue;
            const float sz = flake * (0.6f + 0.8f * hash01(t.id, i, 206u));
            draw.addSprite(m_tex[3], pos, sz, sz, Vec4(0.9f, 0.95f, 1.0f, alpha) * Vec4(Vec3(0.65f + 0.45f * lit), 1.0f), flags, false, 0.0f);
            ++m_lastPieces;
        }
    }
    if (withDebris == 0) return;
    const uint32_t each = std::max(1u, maxPieces / 3u / withDebris); // ~1/3 dos antigos
    static const Vec3 kBase[3] = {Vec3(0.42f, 0.34f, 0.26f), Vec3(0.40f, 0.38f, 0.20f), Vec3(0.62f, 0.48f, 0.30f)};
    static const Vec3 kLeaf[3] = {Vec3(0.34f, 0.40f, 0.16f), Vec3(0.46f, 0.34f, 0.14f), Vec3(0.30f, 0.30f, 0.12f)};
    static const Vec3 kSnow = Vec3(0.86f, 0.93f, 1.0f);
    for (const Tornado& t : m_list) {
        if (!t.spec.debris) continue;
        // Tornado pequeno: menos pecas (a quantidade acompanha o porte; 200 de altura = todas).
        uint32_t count = t.spec.height >= 60.0f ? each : std::max(6u, uint32_t(float(each) * std::max(t.spec.height / 60.0f, 0.15f)));
        if (t.spec.debrisKind == TornadoDebrisKind::Snow) count *= 2; // flocos sao pequenos: mais deles
        for (uint32_t i = 0; i < count; ++i) {
            const Piece pc = pieceAt(t, i);
            if (pc.alpha <= 0.01f) continue;
            // Perto do chao a peca se confunde com a poeira; no alto fica escura contra o ceu.
            const float dustMix = std::exp(-pc.height01 * 5.0f) * 0.55f;
            const float jitter = 0.75f + 0.5f * hash01(t.id, i, 77u);
            Vec3 col;
            if (pc.kind >= 3) {
                // Neve: branco-azulado claro (nao escurece como a terra); poeira fina: a cor da poeira.
                const bool snow = t.spec.debrisKind == TornadoDebrisKind::Snow;
                col = (snow ? kSnow : t.spec.dust) * (0.65f + 0.45f * lit) * (0.85f + 0.3f * hash01(t.id, i, 77u));
            } else if (t.spec.debrisKind == TornadoDebrisKind::Leaves) {
                col = kLeaf[(i + t.id) % 3] * jitter * (0.30f + 0.65f * lit);
            } else {
                col = glm::mix(glm::mix(kBase[pc.kind], t.spec.dust * 0.8f, 0.35f), t.spec.dust * 0.8f, dustMix) * jitter * (0.30f + 0.55f * lit);
            }
            const bool rounded = pc.kind == 0 || pc.kind >= 3;
            const float w = rounded ? pc.size : pc.size * 0.45f;
            const float h = rounded ? pc.size : pc.size * 1.5f;
            draw.addSprite(m_tex[pc.kind], pc.pos, w, h, Vec4(col, pc.alpha), flags, false, pc.rot);
            ++m_lastPieces;
        }
    }
}

} // namespace eruption
