#include "renderer/vfx/VfxBuiltins.hpp"

#include <algorithm>
#include <map>
#include <array>
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
                const float n2 = fbm((u + 1.0f) * 5.0f, (v - phase * 2.0f) * 8.0f + 3.0f, 64, 19, 3);
                // Silhueta de gota (base redonda, ponta fina), torta pelo
                // ruido; a borda e' corroida por um segundo ruido mais fino,
                // entao nao sobra contorno reto.
                const float cx = u + (n - 0.5f) * 0.7f * v;
                const float width = 0.62f * std::pow(std::max(1.0f - v, 0.0f), 0.65f) + 0.03f;
                const float base = std::clamp((v - 0.02f) * 5.0f, 0.0f, 1.0f);
                float body = 1.0f - std::abs(cx) / width;
                body = std::clamp(body * 1.6f - (1.0f - n2) * 0.55f, 0.0f, 1.0f);
                float a = body * base * std::clamp((1.0f - v) * 1.8f, 0.0f, 1.0f);
                a = std::clamp(a * (0.75f + 0.5f * n), 0.0f, 1.0f);
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

void computeVfxCutout(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t cols, uint32_t rows, float out[8]) {
    const float s = 0.70710678f;
    const float nx[8] = {1, s, 0, -s, -1, -s, 0, s};
    const float ny[8] = {0, s, 1, s, 0, -s, -1, -s};
    for (int k = 0; k < 8; ++k) out[k] = -1e9f;
    cols = std::max(cols, 1u);
    rows = std::max(rows, 1u);
    const uint32_t cw = w / cols, ch = h / rows;
    bool any = false;
    if (cw > 0 && ch > 0) {
        for (uint32_t y = 0; y < rows * ch; ++y)
            for (uint32_t x = 0; x < cols * cw; ++x) {
                if (rgba[(size_t(y) * w + x) * 4 + 3] < 8) continue; // ~3% de alfa
                // Centro do texel no quadro, em [-1,1], com folga de meio texel.
                const float px = ((float(x % cw) + 0.5f) / float(cw)) * 2.0f - 1.0f;
                const float py = 1.0f - ((float(y % ch) + 0.5f) / float(ch)) * 2.0f;
                const float pad = 1.0f / float(std::min(cw, ch));
                for (int k = 0; k < 8; ++k) out[k] = std::max(out[k], px * nx[k] + py * ny[k] + pad);
                any = true;
            }
    }
    // Sem conteudo (ou quadro vazio): quad inteiro.
    for (int k = 0; k < 8; ++k) {
        const float full = (k & 1) ? 1.41421356f : 1.0f;
        out[k] = any ? std::clamp(out[k], 0.05f, full) : full;
    }
}

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

namespace {

// Faces planas (normal por face): bom para pedra e estilhaco lidos de longe.
void addTri(VfxMeshData& m, const float a[3], const float b[3], const float c[3]) {
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-12f;
    for (float& x : n) x /= l;
    const float* p[3] = {a, b, c};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    for (int k = 0; k < 3; ++k) {
        VfxMeshVertex vx{};
        for (int j = 0; j < 3; ++j) {
            vx.pos[j] = p[k][j];
            vx.normal[j] = n[j];
        }
        vx.uv[0] = uv[k][0];
        vx.uv[1] = uv[k][1];
        m.indices.push_back(uint16_t(m.vertices.size()));
        m.vertices.push_back(vx);
    }
}

void icosphere(int subdiv, float jitter, uint32_t seed, VfxMeshData& m) {
    const float t = 1.6180339887f;
    std::vector<std::array<float, 3>> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<int, 3>> f = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                         {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                         {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                         {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    for (int s = 0; s < subdiv; ++s) {
        std::vector<std::array<int, 3>> nf;
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            v.push_back({(v[a][0] + v[b][0]) * 0.5f, (v[a][1] + v[b][1]) * 0.5f, (v[a][2] + v[b][2]) * 0.5f});
            return mid[key] = int(v.size()) - 1;
        };
        for (const auto& tri : f) {
            const int ab = midpoint(tri[0], tri[1]), bc = midpoint(tri[1], tri[2]), ca = midpoint(tri[2], tri[0]);
            nf.push_back({tri[0], ab, ca});
            nf.push_back({tri[1], bc, ab});
            nf.push_back({tri[2], ca, bc});
            nf.push_back({ab, bc, ca});
        }
        f = nf;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        const float l = std::sqrt(v[i][0] * v[i][0] + v[i][1] * v[i][1] + v[i][2] * v[i][2]);
        const float r = 1.0f + jitter * (hash2(int(i), int(seed), 3) - 0.5f) * 2.0f;
        for (float& x : v[i]) x = x / l * r;
    }
    for (const auto& tri : f) addTri(m, v[tri[0]].data(), v[tri[1]].data(), v[tri[2]].data());
}

} // namespace

bool makeVfxBuiltinMesh(const std::string& name, VfxMeshData& out) {
    out = VfxMeshData{};
    if (name == "builtin:cube") {
        const float c[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                               {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
        const int q[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
        for (const auto& f : q) {
            float a[3], b[3], cc[3], d[3];
            for (int j = 0; j < 3; ++j) {
                a[j] = c[f[0]][j] * 0.7f;
                b[j] = c[f[1]][j] * 0.7f;
                cc[j] = c[f[2]][j] * 0.7f;
                d[j] = c[f[3]][j] * 0.7f;
            }
            addTri(out, a, b, cc);
            addTri(out, a, cc, d);
        }
    } else if (name == "builtin:shard") {
        // Estilhaco: octaedro esticado e torto.
        const float v[6][3] = {{0.0f, 1.3f, 0.1f}, {0.0f, -1.1f, -0.1f}, {0.55f, 0.1f, 0.0f},
                               {-0.45f, 0.0f, 0.1f}, {0.05f, 0.2f, 0.35f}, {0.0f, -0.1f, -0.3f}};
        const int t[8][3] = {{0, 2, 4}, {0, 4, 3}, {0, 3, 5}, {0, 5, 2}, {1, 4, 2}, {1, 3, 4}, {1, 5, 3}, {1, 2, 5}};
        for (const auto& tri : t) addTri(out, v[tri[0]], v[tri[1]], v[tri[2]]);
    } else if (name == "builtin:rock") {
        icosphere(1, 0.35f, 7u, out);
    } else if (name == "builtin:sphere") {
        icosphere(2, 0.0f, 0u, out);
    } else {
        return false;
    }
    return true;
}

const std::vector<std::string>& vfxBuiltinMeshNames() {
    static const std::vector<std::string> names = {"builtin:rock", "builtin:shard", "builtin:cube", "builtin:sphere"};
    return names;
}

} // namespace eruption
