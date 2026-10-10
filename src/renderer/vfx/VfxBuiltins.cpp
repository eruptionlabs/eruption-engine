#include "renderer/vfx/VfxBuiltins.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace eruption {

namespace {

// Ruido de valor 2D com periodo (tileable), suave (interpolacao quintica).
float hash2(int x, int y, int seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}

float smooth(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

float valueNoise(float x, float y, int period, int seed) {
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    const float fx = smooth(x - float(xi)), fy = smooth(y - float(yi));
    auto at = [&](int a, int b) { return hash2(((a % period) + period) % period, ((b % period) + period) % period, seed); };
    const float a = at(xi, yi), b = at(xi + 1, yi), c = at(xi, yi + 1), d = at(xi + 1, yi + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

float fbm(float x, float y, int period, int seed, int octaves) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += valueNoise(x, y, period, seed + o * 17) * amp;
        norm += amp;
        x *= 2.0f;
        y *= 2.0f;
        period *= 2;
        amp *= 0.5f;
    }
    return sum / norm;
}

uint8_t u8(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

void put(VfxImage& img, int x, int y, float r, float g, float b, float a) {
    uint8_t* p = &img.rgba[(size_t(y) * img.width + size_t(x)) * 4];
    p[0] = u8(r);
    p[1] = u8(g);
    p[2] = u8(b);
    p[3] = u8(a);
}

// Fumaca: 4x4 bolinhas de ruido diferentes (variedade sem animar).
VfxImage makePuff() {
    VfxImage img;
    img.width = img.height = 512;
    img.cols = img.rows = 4;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    const int cell = 128;
    for (int c = 0; c < 16; ++c) {
        const int ox = (c % 4) * cell, oy = (c / 4) * cell;
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x) {
                const float u = (float(x) + 0.5f) / cell * 2.0f - 1.0f;
                const float v = (float(y) + 0.5f) / cell * 2.0f - 1.0f;
                const float r = std::sqrt(u * u + v * v);
                const float n = fbm((u + 1.0f) * 3.0f, (v + 1.0f) * 3.0f, 64, 11 + c * 31, 5);
                // Borda irregular: o raio efetivo varia com o ruido.
                const float edge = 0.62f + 0.38f * n;
                float a = std::clamp(1.0f - r / edge, 0.0f, 1.0f);
                a = a * a * (0.55f + 0.45f * n) * 1.3f;
                // Borda de 2 px vazia: o filtro bilinear nao puxa o vizinho.
                if (x < 2 || y < 2 || x >= cell - 2 || y >= cell - 2) a = 0.0f;
                const float shade = 0.75f + 0.25f * n; // relevo leve
                put(img, ox + x, oy + y, shade, shade, shade, a);
            }
    }
    return img;
}

// Chama: 4x4 quadros de uma lingua de fogo subindo (ruido rolando para cima).
VfxImage makeFlame() {
    VfxImage img;
    img.width = img.height = 512;
    img.cols = img.rows = 4;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    const int cell = 128;
    for (int f = 0; f < 16; ++f) {
        const int ox = (f % 4) * cell, oy = (f / 4) * cell;
        const float phase = float(f) / 16.0f;
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x) {
                const float u = (float(x) + 0.5f) / cell * 2.0f - 1.0f;   // -1..1
                const float v = 1.0f - (float(y) + 0.5f) / cell;          // 0 embaixo, 1 em cima
                const float n = fbm((u + 1.0f) * 2.0f, (v - phase) * 4.0f + 8.0f, 32, 7, 4);
                // Silhueta: larga embaixo, afina em cima, deformada pelo ruido.
                const float width = (1.0f - v) * 0.75f + 0.08f;
                const float dx = std::abs(u + (n - 0.5f) * 0.6f * v) / width;
                float a = std::clamp(1.0f - dx, 0.0f, 1.0f) * std::clamp((1.0f - v) * 1.6f, 0.0f, 1.0f);
                a *= std::clamp(v * 6.0f, 0.0f, 1.0f) * (0.6f + 0.8f * n);
                a = std::clamp(a * 1.4f, 0.0f, 1.0f);
                if (x < 2 || y < 2 || x >= cell - 2 || y >= cell - 2) a = 0.0f;
                // Mais quente no meio (branco-amarelo) e mais frio na borda.
                const float heat = std::clamp(a * 1.2f, 0.0f, 1.0f);
                put(img, ox + x, oy + y, 1.0f, 0.55f + 0.45f * heat, 0.2f + 0.6f * heat * heat, a);
            }
    }
    return img;
}

// Faisca / brilho: nucleo forte com halo (bom esticado na velocidade).
VfxImage makeSpark() {
    VfxImage img;
    img.width = img.height = 64;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float u = (float(x) + 0.5f) / 32.0f - 1.0f, v = (float(y) + 0.5f) / 32.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);
            const float core = std::exp(-r * r * 40.0f);
            const float halo = std::exp(-r * 4.0f) * 0.45f;
            const float a = std::clamp(core + halo, 0.0f, 1.0f) * (r < 0.98f ? 1.0f : 0.0f);
            put(img, x, y, 1.0f, 1.0f, 1.0f, a);
        }
    return img;
}

// Anel (onda de choque, deitado no chao).
VfxImage makeRing() {
    VfxImage img;
    img.width = img.height = 256;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const float u = (float(x) + 0.5f) / 128.0f - 1.0f, v = (float(y) + 0.5f) / 128.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);
            const float d = (r - 0.8f) / 0.12f;
            const float n = fbm((u + 1.0f) * 6.0f, (v + 1.0f) * 6.0f, 64, 5, 3);
            const float a = std::exp(-d * d) * (0.7f + 0.3f * n) * (r < 0.99f ? 1.0f : 0.0f);
            put(img, x, y, 1.0f, 1.0f, 1.0f, a);
        }
    return img;
}

// Estrela de 4 pontas (brilho magico).
VfxImage makeStar() {
    VfxImage img;
    img.width = img.height = 128;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const float u = (float(x) + 0.5f) / 64.0f - 1.0f, v = (float(y) + 0.5f) / 64.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);
            const float rays = std::exp(-std::abs(u) * 30.0f) * std::exp(-std::abs(v) * 2.5f) +
                               std::exp(-std::abs(v) * 30.0f) * std::exp(-std::abs(u) * 2.5f);
            const float core = std::exp(-r * r * 30.0f);
            const float a = std::clamp(rays + core, 0.0f, 1.0f) * std::clamp(1.0f - r, 0.0f, 1.0f) * 1.0f;
            put(img, x, y, 1.0f, 1.0f, 1.0f, a);
        }
    return img;
}

} // namespace

bool makeVfxBuiltin(const std::string& name, VfxImage& out) {
    if (name == "builtin:puff") out = makePuff();
    else if (name == "builtin:flame") out = makeFlame();
    else if (name == "builtin:spark") out = makeSpark();
    else if (name == "builtin:ring") out = makeRing();
    else if (name == "builtin:star") out = makeStar();
    else return false;
    return true;
}

const std::vector<std::string>& vfxBuiltinNames() {
    static const std::vector<std::string> names = {"builtin:puff", "builtin:flame", "builtin:spark", "builtin:ring",
                                                   "builtin:star"};
    return names;
}

} // namespace eruption
