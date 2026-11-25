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

} // namespace

bool TornadoField::init(ImmediateDraw& draw) {
    const auto rock = makeRock(32);
    const auto leaf = makeLeaf(12, 32);
    const auto plank = makePlank(10, 32);
    m_tex[0] = draw.addTexture("engine:tornado_rock", rock.data(), 32, 32);
    m_tex[1] = draw.addTexture("engine:tornado_leaf", leaf.data(), 12, 32);
    m_tex[2] = draw.addTexture("engine:tornado_plank", plank.data(), 10, 32);
    return m_tex[0] >= 0 && m_tex[1] >= 0 && m_tex[2] >= 0;
}

void TornadoField::submitDebris(ImmediateDraw& draw, uint32_t maxPieces, float lit) {
    m_lastPieces = 0;
    if (m_list.empty() || maxPieces == 0 || m_tex[0] < 0) return;
    ImmediateDraw::Flags flags;
    flags.blend = ImmediateDraw::Blend::Alpha;
    flags.depthTest = true;
    uint32_t withDebris = 0;
    for (const Tornado& t : m_list) withDebris += t.spec.debris ? 1u : 0u;
    if (withDebris == 0) return;
    const uint32_t each = std::max(1u, maxPieces / 3u / withDebris); // ~1/3 dos antigos
    static const Vec3 kBase[3] = {Vec3(0.42f, 0.34f, 0.26f), Vec3(0.40f, 0.38f, 0.20f), Vec3(0.62f, 0.48f, 0.30f)};
    for (const Tornado& t : m_list) {
        if (!t.spec.debris) continue;
        for (uint32_t i = 0; i < each; ++i) {
            const Piece pc = pieceAt(t, i);
            if (pc.alpha <= 0.01f) continue;
            // Perto do chao a peca se confunde com a poeira; no alto fica escura contra o ceu.
            const float dustMix = std::exp(-pc.height01 * 5.0f) * 0.55f;
            const float jitter = 0.75f + 0.5f * hash01(t.id, i, 77u);
            const Vec3 col = glm::mix(glm::mix(kBase[pc.kind], t.spec.dust * 0.8f, 0.35f), t.spec.dust * 0.8f, dustMix) * jitter * (0.30f + 0.55f * lit);
            const float w = pc.kind == 0 ? pc.size : pc.size * 0.45f;
            const float h = pc.kind == 0 ? pc.size : pc.size * 1.5f;
            draw.addSprite(m_tex[pc.kind], pc.pos, w, h, Vec4(col, pc.alpha), flags, false, pc.rot);
            ++m_lastPieces;
        }
    }
}

} // namespace eruption
