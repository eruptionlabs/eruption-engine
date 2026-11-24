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

// Folha (particula de ambiente): 2x2 variacoes de forma e cor - verde,
// verde-amarelo, ocre e marrom -, lanceta com nervura central e borda
// escurecida, para ler como folha mesmo com 6-10 px na tela. Cor
// premultiplicada.
VfxImage makeLeaf() {
    VfxImage img;
    img.width = img.height = 128;
    img.cols = img.rows = 2;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    const int cell = 64;
    const float cores[4][3] = {{0.38f, 0.62f, 0.22f}, {0.66f, 0.72f, 0.24f}, {0.82f, 0.56f, 0.20f}, {0.58f, 0.36f, 0.18f}};
    const float largura[4] = {0.42f, 0.34f, 0.48f, 0.38f};
    for (int c = 0; c < 4; ++c) {
        const int ox = (c % 2) * cell, oy = (c / 2) * cell;
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x) {
                const float u = (float(x) + 0.5f) / cell * 2.0f - 1.0f; // largura
                const float v = (float(y) + 0.5f) / cell * 2.0f - 1.0f; // comprimento (-1 ponta, 1 cabo)
                // Meia largura da lanceta ao longo do comprimento: zero nas pontas.
                const float t = std::clamp((v + 0.9f) / 1.75f, 0.0f, 1.0f);
                const float w = largura[c] * std::pow(std::sin(t * 3.14159265f), 0.8f) * (1.0f - 0.25f * t);
                const float d = w > 1e-3f ? std::abs(u) / w : 2.0f;
                float a = std::clamp((1.0f - d) * 6.0f, 0.0f, 1.0f);
                // Cabinho: fio de 1 px que sai da base.
                if (v > 0.82f && v < 0.98f && std::abs(u) < 0.04f) a = 1.0f;
                if (x < 1 || y < 1 || x >= cell - 1 || y >= cell - 1) a = 0.0f;
                const float n = fbm((u + 1.0f) * 4.0f, (v + 1.0f) * 4.0f, 64, 23 + c * 13, 3);
                const float nervura = std::exp(-u * u * 900.0f) * (v < 0.85f ? 0.35f : 0.0f);
                const float borda = 1.0f - 0.35f * std::clamp(d * d, 0.0f, 1.0f);
                const float k = (0.8f + 0.35f * n) * borda + nervura;
                // Cor PREMULTIPLICADA pelo alfa (use com "blend": "premultiplied", sem
                // ordenar): fora da folha a cor e' zero e o quad nao soma nada.
                put(img, ox + x, oy + y, cores[c][0] * k * a, cores[c][1] * k * a, cores[c][2] * k * a, a);
            }
    }
    return img;
}

// Marca de sola (pegada): ponta para cima (v = 0), branca com alfa; a cor vem
// do efeito. Assimetrica de proposito (a cava do arco so' de um lado), para o
// pe' esquerdo e o direito (a textura espelhada) se distinguirem.
VfxImage makeFoot() {
    VfxImage img;
    img.width = img.height = 128;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    auto ellipse = [](float x, float y, float cx, float cy, float rx, float ry) {
        const float dx = (x - cx) / rx, dy = (y - cy) / ry;
        return 1.0f - std::sqrt(dx * dx + dy * dy); // >0 dentro
    };
    for (int py = 0; py < int(img.height); ++py)
        for (int px = 0; px < int(img.width); ++px) {
            const float x = (float(px) + 0.5f) / float(img.width) * 2.0f - 1.0f;
            const float y = 1.0f - (float(py) + 0.5f) / float(img.height) * 2.0f; // +1 = ponta
            float d = ellipse(x, y, 0.04f, 0.38f, 0.42f, 0.5f);                  // ante-pe'
            d = std::max(d, ellipse(x, y, -0.02f, -0.62f, 0.3f, 0.34f));         // calcanhar
            // O meio do pe': so' uma faixa estreita do lado de fora (x > 0).
            const float ponte = ellipse(x, y, 0.2f, -0.1f, 0.2f, 0.3f);
            d = std::max(d, ponte);
            const float cava = 0.3f * std::exp(-std::pow((x + 0.5f) * 3.0f, 2.0f)) * std::exp(-std::pow((y + 0.1f) * 3.5f, 2.0f));
            const float a = std::clamp((d - cava) * 9.0f, 0.0f, 1.0f);
            const float grao = 0.88f + 0.12f * hash2(px / 3, py / 3, 7);
            put(img, px, py, grao, grao, grao, a);
        }
    return img;
}


// MARCAS DO CHAO das afeccoes (data/affects.json, "texture": "builtin:mk_*"):
// cada textura e' um ATLAS 2x2 (256x256, quadros de 128) com 4 desenhos
// diferentes do mesmo tipo; o efeito sorteia o quadro (flipMode Random) e o
// giro por instancia, entao duas marcas seguidas nunca saem iguais. Cinza
// com alfa (a cor vem do efeito: cinza escuro = miolo mais fechado). Nenhuma
// forma e' um disco: a borda e' deformada por ruido e por lobulos. O relevo
// das rachaduras e do gelo ja' vem sombreado no proprio cinza (luz de cima e
// da esquerda): e' o "normal no decal" do preset low, sem tesselacao.
float sstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float markNoise(float x, float y, int seed, float freq, int oct = 3) {
    return fbm((x + 1.0f) * freq, (y + 1.0f) * freq, 64, seed, oct);
}

constexpr int kMarkCell = 128;
constexpr int kMarkVariants = 4;

// Atlas 2x2: f(variante, x, y, r, ang, cinza, alfa), x e y em [-1, 1].
template <class F>
VfxImage makeAtlas(F f) {
    VfxImage img;
    img.width = img.height = kMarkCell * 2;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    for (int v = 0; v < kMarkVariants; ++v) {
        const int ox = (v % 2) * kMarkCell, oy = (v / 2) * kMarkCell;
        for (int py = 0; py < kMarkCell; ++py)
            for (int px = 0; px < kMarkCell; ++px) {
                const float x = (float(px) + 0.5f) / float(kMarkCell) * 2.0f - 1.0f;
                const float y = 1.0f - (float(py) + 0.5f) / float(kMarkCell) * 2.0f;
                const float r = std::sqrt(x * x + y * y);
                float g = 1.0f, a = 0.0f;
                if (r < 1.0f) f(v, x, y, r, std::atan2(y, x), g, a);
                // Borda sempre apagada: o decal nunca mostra o recorte do quadro.
                a *= 1.0f - sstep(0.86f, 1.0f, r);
                put(img, ox + px, oy + py, g, g, g, std::clamp(a, 0.0f, 1.0f));
            }
    }
    return img;
}

// Raio deformado: ruido de baixa frequencia (golfos grandes), de alta (serrilhado)
// e lobulos angulares; amp = quanto a borda foge do circulo.
float wobbleR(int v, float x, float y, float r, float ang, int seed, float amp) {
    const int s = seed + v * 101;
    const float big = markNoise(x, y, s, 2.2f, 3) - 0.5f;
    const float fine = markNoise(x, y, s + 7, 7.0f, 2) - 0.5f;
    const float lobes = std::sin(ang * float(2 + (v + seed) % 3) + float(v) * 1.7f) * 0.5f + std::sin(ang * 5.0f + float(s)) * 0.25f;
    // Lobulos so' fora do centro: no ponto o angulo nao existe (sem beliscao).
    return r + big * amp * 1.3f + fine * amp * 0.45f + lobes * amp * 0.25f * std::min(r * 3.0f, 1.0f);
}

// Linhas (raios, ramos, cristais): lista de segmentos com largura.
struct Seg {
    float ax, ay, bx, by, w;
};

uint32_t markRand(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

float markRand01(uint32_t& s) {
    return float(markRand(s) & 0xffff) / 65535.0f;
}

// Ramo que oscila: de (x, y) no angulo ang, n passos de tamanho step.
void addWobblyBranch(std::vector<Seg>& segs, uint32_t& rs, float x, float y, float ang, int n, float step, float w, float wob, float branchChance, float branchAng, int depth) {
    for (int i = 0; i < n; ++i) {
        const float t = float(i) / float(n);
        ang += (markRand01(rs) - 0.5f) * 2.0f * wob;
        const float nx = x + std::cos(ang) * step, ny = y + std::sin(ang) * step;
        segs.push_back({x, y, nx, ny, w * (1.0f - 0.65f * t)});
        if (depth > 0 && i > 0 && markRand01(rs) < branchChance) {
            const float side = markRand01(rs) < 0.5f ? -1.0f : 1.0f;
            addWobblyBranch(segs, rs, nx, ny, ang + side * branchAng * (0.7f + 0.6f * markRand01(rs)), std::max(2, (n - i) / 2), step * 0.8f, w * 0.65f, wob, branchChance * 0.6f, branchAng, depth - 1);
        }
        x = nx;
        y = ny;
    }
}

// Menor distancia (ponderada pela largura) do ponto aos segmentos: 0 = em cima.
float segsCoverage(const std::vector<Seg>& segs, float px, float py, float halo) {
    float best = 0.0f;
    for (const Seg& s : segs) {
        const float dx = s.bx - s.ax, dy = s.by - s.ay;
        const float l2 = dx * dx + dy * dy;
        float t = l2 > 1e-8f ? ((px - s.ax) * dx + (py - s.ay) * dy) / l2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float ex = px - (s.ax + dx * t), ey = py - (s.ay + dy * t);
        const float d = std::sqrt(ex * ex + ey * ey);
        const float core = sstep(s.w, s.w * 0.3f, d);
        const float soft = halo * sstep(s.w * 5.0f, s.w, d);
        best = std::max(best, std::max(core, soft));
    }
    return best;
}

// ---- fogo: chao queimado. Miolo quase carvao, chamuscado marrom ate' a borda
// irregular; o cinza e' a profundidade do escuro (a cor do efeito e' o marrom).
VfxImage makeMarkScorch() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 31, 0.55f);
        const float body = sstep(0.82f, 0.46f, d);
        const float core = sstep(0.62f, 0.05f, d);
        const float mott = markNoise(x, y, 47 + v, 6.0f, 3);
        g = std::clamp(1.0f - std::pow(core, 0.7f) * 0.93f + (mott - 0.5f) * 0.25f * (1.0f - core * 0.6f), 0.04f, 1.0f);
        // Borda esburacada: o chamusco rala em manchas antes de acabar.
        const float rim = 0.7f + 0.3f * sstep(0.35f, 0.62f, markNoise(x, y, 59 + v * 13, 9.0f, 2));
        a = body * (0.45f + 0.55f * sstep(0.78f, 0.25f, d)) * (core > 0.3f ? 1.0f : rim);
    });
}

// Brasas: pontos e fendas emissivas dentro do miolo (soma luz; apagam primeiro).
VfxImage makeMarkEmbers() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 31, 0.55f);
        const float zone = sstep(0.62f, 0.12f, d);
        // Fendas: onde um ruido de crista passa perto de 0,5.
        const float ridge = std::abs(markNoise(x, y, 203 + v * 17, 4.5f, 3) - 0.5f);
        const float crack = (1.0f - sstep(0.004f, 0.022f, ridge)) * sstep(0.7f, 0.3f, d);
        // Pontos: picos raros de ruido de alta frequencia.
        const float spot = sstep(0.74f, 0.84f, markNoise(x, y, 211 + v * 5, 13.0f, 2)) * zone;
        const float heat = std::clamp(crack * 0.9f + spot, 0.0f, 1.0f);
        a = heat * (0.4f + 0.6f * sstep(0.7f, 0.1f, d));
        g = 0.55f + 0.45f * markNoise(x, y, 217, 8.0f, 2);
    });
}

// Folhas e capim torrados em estrias curtas saindo do ponto.
VfxImage makeMarkStreak() {
    return makeAtlas([](int v, float x, float y, float, float, float& g, float& a) {
        const float n = markNoise(x, y, 61 + v * 7, 4.0f);
        const float w = 0.10f + 0.10f * markNoise(x, y, 67 + v, 3.0f, 2);
        a = sstep(w, w * 0.2f, std::abs(x - (n - 0.5f) * 0.34f)) * sstep(1.0f, 0.45f, std::abs(y)) * (0.6f + 0.4f * n);
        g = 0.7f + 0.3f * n;
    });
}

// Rachadura: fendas de ruido de crista, com labio claro e fundo escuro.
VfxImage makeMarkCrack() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 77, 0.4f);
        const float n1 = markNoise(x, y, 83 + v * 19, 3.4f, 3);
        const float n2 = markNoise(x + 0.31f, y - 0.17f, 89 + v * 23, 5.5f, 2);
        const float edge = std::min(std::abs(n1 - 0.5f), std::abs(n2 - 0.5f) * 1.3f);
        const float w = 0.05f * (1.0f - 0.45f * std::min(r, 1.0f));
        const float crack = 1.0f - sstep(0.0f, w, edge);
        const float mask = sstep(0.95f, 0.5f, d);
        const float lip = sstep(w, w * 2.6f, edge) * (1.0f - sstep(w * 2.6f, w * 4.2f, edge));
        const float dust = 0.35f * mask * sstep(0.8f, 0.2f, d) * markNoise(x, y, 97, 6.0f, 2);
        a = std::clamp(crack * mask + lip * mask * 0.45f + dust, 0.0f, 1.0f);
        const float shade = (x * -0.5f + y * 0.5f) * 0.5f + 0.5f;
        g = crack > 0.4f ? 0.12f : 0.55f + 0.4f * lip * shade;
    });
}

// Relampago: estrias finas e ramificadas saindo do ponto, miolo mais escuro.
VfxImage makeMarkBolt() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        static std::vector<Seg> segs[kMarkVariants];
        if (segs[v].empty()) {
            uint32_t rs = 9001u + uint32_t(v) * 7919u;
            const int rays = 7 + v % 3;
            for (int k = 0; k < rays; ++k) {
                const float a0 = 6.2831853f * (float(k) + markRand01(rs) * 0.6f) / float(rays);
                const float len = 0.62f + 0.3f * markRand01(rs);
                const int n = 9 + int(markRand(rs) % 3u);
                addWobblyBranch(segs[v], rs, std::cos(a0) * 0.05f, std::sin(a0) * 0.05f, a0, n, len / float(n), 0.026f, 0.38f, 0.42f, 0.75f, 2);
            }
        }
        const float line = segsCoverage(segs[v], x, y, 0.4f);
        const float d = wobbleR(v, x, y, r, ang, 91, 0.3f);
        const float core = sstep(0.3f, 0.02f, d);
        const float halo = 0.35f * sstep(0.75f, 0.2f, d) * (0.5f + 0.5f * markNoise(x, y, 99, 5.0f, 2));
        a = std::clamp(std::max(line, core * 0.95f) + halo * 0.5f, 0.0f, 1.0f);
        g = 0.12f + 0.88f * (1.0f - core) * (0.55f + 0.45f * line);
    });
}

// Molhado: o chao escurece em manchas de bordas moles (sem disco chapado).
VfxImage makeMarkPuddle() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 111, 0.6f);
        const float body = sstep(0.78f, 0.3f, d);
        // Mais escuro e fundo no meio, com veios de umidade ate' a borda.
        const float vein = markNoise(x, y, 119 + v, 5.0f, 3);
        a = body * (0.45f + 0.55f * sstep(0.7f, 0.15f, d)) * (0.75f + 0.25f * vein);
        g = 0.35f + 0.65f * sstep(0.1f, 0.7f, d) * (0.6f + 0.4f * vein);
    });
}

// Brilho molhado (reflexo): fios claros e um crescente, luz de cima.
VfxImage makeMarkSheen() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 141, 0.6f);
        const float body = sstep(0.3f, 0.5f, d) * sstep(0.85f, 0.62f, d); // so' na borda
        const float stripe = sstep(0.55f, 0.8f, markNoise(x * 1.0f, y * 2.4f, 147 + v * 5, 3.0f, 3));
        const float crescent = sstep(0.0f, 0.5f, -x * 0.6f + y * 0.8f + 0.1f);
        a = body * (0.25f + 0.75f * stripe) * (0.35f + 0.65f * crescent);
        g = 1.0f;
    });
}

// Geada: manchas de graos brancos com cristais ramificados passando da borda.
VfxImage makeMarkFrost() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        static std::vector<Seg> segs[kMarkVariants];
        if (segs[v].empty()) {
            uint32_t rs = 4242u + uint32_t(v) * 6007u;
            for (int k = 0; k < 6; ++k) {
                const float a0 = 6.2831853f * (float(k) + (markRand01(rs) - 0.5f) * 0.35f) / 6.0f;
                const float len = 0.55f + 0.38f * markRand01(rs);
                addWobblyBranch(segs[v], rs, 0.0f, 0.0f, a0, 8, len / 8.0f, 0.012f, 0.1f, 0.75f, 1.05f, 2);
            }
        }
        const float d = wobbleR(v, x, y, r, ang, 131, 0.5f);
        const float patch = sstep(0.62f, 0.2f, d);
        const float grain = sstep(0.42f, 0.66f, markNoise(x, y, 137 + v * 3, 14.0f, 3) + (0.5f - d) * 0.25f);
        const float line = segsCoverage(segs[v], x, y, 0.2f);
        const float spike = sstep(0.55f, 0.82f, r) * line;
        a = std::clamp(patch * (0.28f + 0.5f * grain) + line * 0.85f + spike * 0.2f, 0.0f, 1.0f);
        g = 0.82f + 0.18f * markNoise(x, y, 141, 9.0f, 2);
    });
}

// Mancha viscosa (veneno): gotas satelite e bordas lobuladas.
VfxImage makeMarkStain() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 151, 0.7f);
        const float base = sstep(0.7f, 0.38f, d);
        const float drops = sstep(0.56f, 0.72f, markNoise(x, y, 161 + v * 3, 9.0f, 3) + (0.62f - r) * 0.2f) * sstep(0.95f, 0.6f, r);
        a = std::clamp(base * (0.6f + 0.4f * sstep(0.6f, 0.1f, d)) + drops * 0.8f, 0.0f, 1.0f);
        g = 0.55f + 0.45f * markNoise(x, y, 167 + v, 5.0f, 2);
    });
}

// Brilho suave (flash e halo): o unico quadro simetrico, 1 desenho.
VfxImage makeMarkGlow() {
    VfxImage img;
    img.width = img.height = kMarkCell;
    img.rgba.assign(size_t(img.width) * img.height * 4, 0);
    for (int py = 0; py < kMarkCell; ++py)
        for (int px = 0; px < kMarkCell; ++px) {
            const float x = (float(px) + 0.5f) / float(kMarkCell) * 2.0f - 1.0f;
            const float y = 1.0f - (float(py) + 0.5f) / float(kMarkCell) * 2.0f;
            const float r = std::sqrt(x * x + y * y);
            const float n = markNoise(x, y, 181, 4.0f, 2);
            const float a = std::exp(-r * r * 3.2f) * sstep(1.0f, 0.7f, r + (n - 0.5f) * 0.2f);
            put(img, px, py, 1.0f, 1.0f, 1.0f, a);
        }
    return img;
}

VfxImage makeMarkMud() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 171, 0.6f);
        const float body = sstep(0.7f, 0.36f, d);
        const float drops = sstep(0.66f, 0.74f, markNoise(x, y, 181 + v * 3, 9.0f, 2) + (0.9f - r) * 0.12f) * sstep(1.0f, 0.55f, r);
        a = std::clamp(body * (0.7f + 0.3f * markNoise(x, y, 187, 5.0f, 2)) + drops * 0.7f, 0.0f, 1.0f);
        g = 0.65f + 0.35f * markNoise(x, y, 191 + v, 6.0f);
    });
}

// Sombra: tentaculos curvos saindo de um nucleo pequeno; o miolo nao e' um
// disco cheio, e a borda e' a ponta fina de cada tentaculo.
VfxImage makeMarkFade() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        static std::vector<Seg> segs[kMarkVariants];
        if (segs[v].empty()) {
            uint32_t rs = 7771u + uint32_t(v) * 4099u;
            const int arms = 8 + v % 3;
            for (int k = 0; k < arms; ++k) {
                const float a0 = 6.2831853f * (float(k) + (markRand01(rs) - 0.5f) * 0.5f) / float(arms);
                const float len = 0.55f + 0.4f * markRand01(rs);
                const int n = 10;
                addWobblyBranch(segs[v], rs, std::cos(a0) * 0.08f, std::sin(a0) * 0.08f, a0, n, len / float(n), 0.06f, 0.3f, 0.22f, 0.8f, 1);
            }
        }
        const float line = segsCoverage(segs[v], x, y, 0.12f);
        const float d = wobbleR(v, x, y, r, ang, 221, 0.35f);
        const float core = sstep(0.28f, 0.12f, d);
        a = std::clamp(std::max(line, core) * (0.8f + 0.2f * markNoise(x, y, 233 + v, 6.0f, 2)), 0.0f, 1.0f);
        g = 0.6f + 0.4f * markNoise(x, y, 239 + v, 4.0f, 2);
    });
}

// Pancada: poeira em leque irregular, raios curtos.
VfxImage makeMarkBlast() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 231, 0.55f);
        const float n = markNoise(x, y, 239 + v * 5, 3.0f);
        const float spokes = 0.45f + 0.55f * std::pow(std::abs(std::sin(ang * float(5 + v) * 0.5f + n * 5.0f)), 0.6f);
        a = std::pow(sstep(0.95f, 0.1f, d), 0.9f) * spokes;
        g = 0.55f + 0.45f * sstep(0.2f, 0.9f, r);
    });
}

// Sagrado: so' aneis quebrados e runas entre eles; o interior fica vazio
// (o chao aparece), nada de disco, raio ou halo cheio.
VfxImage makeMarkHoly() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float n = markNoise(x, y, 251 + v * 7, 3.0f, 2);
        const float cx = std::cos(ang) * 1.3f, cy = std::sin(ang) * 1.3f;
        const float rr = r + (n - 0.5f) * 0.12f + (markNoise(cx, cy, 255 + v, 3.0f, 2) - 0.5f) * 0.08f;
        const float ring1 = std::exp(-std::pow((rr - 0.34f) / 0.022f, 2.0f)) * sstep(0.42f, 0.55f, markNoise(cx, cy, 257 + v * 3, 5.0f, 3));
        const float ring2 = std::exp(-std::pow((rr - 0.80f) / 0.026f, 2.0f)) * sstep(0.40f, 0.52f, markNoise(cx, cy, 261 + v * 5, 4.0f, 3));
        // Runas: 16 celulas angulares entre os aneis, cada uma com um tracado.
        const float u = (ang + 3.14159265f) / 6.2831853f * 16.0f;
        const int cell = int(std::floor(u));
        const float la = (u - float(cell)) - 0.5f;
        const float lt = (rr - 0.46f) / 0.26f; // 0..1 entre os aneis
        const float h = hash2(cell, v, 91);
        const bool in = lt > 0.0f && lt < 1.0f && h > 0.3f;
        float rune = 0.0f;
        if (in) {
            const int kind = int(h * 10.0f) % 3;
            const float bar = sstep(0.09f, 0.05f, std::abs(la)) * sstep(0.05f, 0.1f, lt) * sstep(0.95f, 0.88f, lt);
            const float cross = sstep(0.07f, 0.04f, std::abs(lt - 0.5f)) * sstep(0.32f, 0.26f, std::abs(la));
            const float diag = sstep(0.08f, 0.05f, std::abs(la - (lt - 0.5f) * 0.5f)) * sstep(0.1f, 0.15f, lt) * sstep(0.9f, 0.85f, lt);
            rune = kind == 0 ? bar : (kind == 1 ? std::max(bar * 0.9f, cross) : diag);
        }
        a = std::clamp(ring1 + ring2 + rune * 0.9f, 0.0f, 1.0f);
        g = 1.0f;
    });
}

// Onda de choque: arco quebrado de raio ondulado e espessura variavel, nunca
// um anel fechado (cada instancia gira um dos 4 desenhos).
VfxImage makeMarkWave() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float wob = (markNoise(std::cos(ang) * 1.3f, std::sin(ang) * 1.3f, 263 + v * 9, 3.0f, 3) - 0.5f) * 0.3f;
        const float thick = 0.07f + 0.08f * markNoise(std::cos(ang) * 1.1f, std::sin(ang) * 1.1f, 269 + v * 5, 4.0f, 2);
        const float dd = (r - 0.74f + wob) / thick;
        const float gap = sstep(0.38f, 0.55f, markNoise(std::cos(ang) * 1.2f, std::sin(ang) * 1.2f, 271 + v * 3, 3.0f, 3));
        a = std::exp(-dd * dd) * gap * (0.6f + 0.4f * markNoise(x, y, 277, 8.0f, 2));
        g = 1.0f;
    });
}

// Cratera: fundo escuro irregular, labio claro erguido, raios de entulho de
// comprimentos diferentes e pedacos soltos fora (impacto).
VfxImage makeMarkCrater() {
    return makeAtlas([](int v, float x, float y, float r, float ang, float& g, float& a) {
        const float d = wobbleR(v, x, y, r, ang, 281, 0.5f);
        const float floorM = sstep(0.5f, 0.28f, d);
        const float lip = std::exp(-std::pow((d - 0.5f) / 0.09f, 2.0f));
        const float cx = std::cos(ang) * 1.6f, cy = std::sin(ang) * 1.6f;
        const float ray = sstep(0.5f, 0.7f, markNoise(cx, cy, 283 + v * 7, 6.0f, 3));
        const float reach = 0.6f + 0.38f * markNoise(cx, cy, 289 + v * 11, 3.0f, 2);
        const float rays = ray * sstep(0.42f, 0.55f, r) * sstep(reach, reach * 0.6f, r) * (0.5f + 0.5f * markNoise(x, y, 293, 10.0f, 2));
        const float chunks = sstep(0.72f, 0.82f, markNoise(x, y, 297 + v * 3, 12.0f, 2)) * sstep(0.5f, 0.6f, r) * sstep(0.95f, 0.7f, r) * (0.4f + 0.6f * ray);
        a = std::clamp(floorM * 0.95f + lip * 0.65f + rays * 0.7f + chunks, 0.0f, 1.0f);
        // Fundo escuro (cinza baixo), labio e entulho mais claros.
        g = std::clamp(0.3f + 0.7f * std::max(lip, std::max(rays, chunks)) + 0.1f * (1.0f - floorM), 0.1f, 1.0f);
    });
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
    else if (name == "builtin:leaf") out = makeLeaf();
    else if (name == "builtin:foot") out = makeFoot();
    else if (name == "builtin:mk_scorch") out = makeMarkScorch();
    else if (name == "builtin:mk_streak") out = makeMarkStreak();
    else if (name == "builtin:mk_crack") out = makeMarkCrack();
    else if (name == "builtin:mk_bolt") out = makeMarkBolt();
    else if (name == "builtin:mk_puddle") out = makeMarkPuddle();
    else if (name == "builtin:mk_frost") out = makeMarkFrost();
    else if (name == "builtin:mk_stain") out = makeMarkStain();
    else if (name == "builtin:mk_glow") out = makeMarkGlow();
    else if (name == "builtin:mk_mud") out = makeMarkMud();
    else if (name == "builtin:mk_embers") out = makeMarkEmbers();
    else if (name == "builtin:mk_fade") out = makeMarkFade();
    else if (name == "builtin:mk_blast") out = makeMarkBlast();
    else if (name == "builtin:mk_wave") out = makeMarkWave();
    else if (name == "builtin:mk_sheen") out = makeMarkSheen();
    else if (name == "builtin:mk_crater") out = makeMarkCrater();
    else if (name == "builtin:mk_holy") out = makeMarkHoly();
    else return false;
    return true;
}

const std::vector<std::string>& vfxBuiltinNames() {
    static const std::vector<std::string> names = {"builtin:puff", "builtin:flame", "builtin:spark", "builtin:ring",
                                                   "builtin:star", "builtin:leaf", "builtin:foot",
                                                   "builtin:mk_scorch", "builtin:mk_streak", "builtin:mk_crack",
                                                   "builtin:mk_bolt", "builtin:mk_puddle", "builtin:mk_frost",
                                                   "builtin:mk_stain", "builtin:mk_glow", "builtin:mk_mud",
                                                   "builtin:mk_embers", "builtin:mk_fade", "builtin:mk_blast",
                                                   "builtin:mk_wave", "builtin:mk_sheen", "builtin:mk_holy", "builtin:mk_crater"};
    return names;
}

namespace {

// Malha de trabalho indexada (posicao compartilhada entre faces vizinhas).
struct WorkMesh {
    std::vector<std::array<float, 3>> v;
    std::vector<std::array<int, 3>> f;
};

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> normalized(std::array<float, 3> a) {
    const float l = std::sqrt(dot3(a, a)) + 1e-12f;
    return {a[0] / l, a[1] / l, a[2] / l};
}

// Direcao uniforme na esfera a partir de dois sorteios.
std::array<float, 3> randomDir(uint32_t seed, int k) {
    const float z = hash2(k, int(seed), 11) * 2.0f - 1.0f;
    const float phi = hash2(k, int(seed), 12) * 6.2831853f;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(phi), z, r * std::sin(phi)};
}

WorkMesh icosphere(int subdiv) {
    const float t = 1.6180339887f;
    WorkMesh m;
    m.v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    m.f = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9},  {5, 11, 4},
           {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6},  {3, 6, 8},
           {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    for (int s = 0; s < subdiv; ++s) {
        std::vector<std::array<int, 3>> nf;
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            m.v.push_back({(m.v[a][0] + m.v[b][0]) * 0.5f, (m.v[a][1] + m.v[b][1]) * 0.5f,
                           (m.v[a][2] + m.v[b][2]) * 0.5f});
            return mid[key] = int(m.v.size()) - 1;
        };
        for (const auto& tri : m.f) {
            const int ab = midpoint(tri[0], tri[1]), bc = midpoint(tri[1], tri[2]), ca = midpoint(tri[2], tri[0]);
            nf.push_back({tri[0], ab, ca});
            nf.push_back({tri[1], bc, ab});
            nf.push_back({tri[2], ca, bc});
            nf.push_back({ab, bc, ca});
        }
        m.f = nf;
    }
    for (auto& p : m.v) p = normalized(p);
    return m;
}

// Gera os vertices finais. Normal suave dentro do angulo de vinco (faces
// vizinhas quase planas somam), quina viva acima dele: e' o que faz a face
// de fratura ler como corte e o resto como pedra gasta. A oclusao sai da
// curvatura: vizinho a frente da normal = vertice num fundo (concavo).
void emitMesh(const WorkMesh& m, float creaseDeg, const std::vector<float>& fresh, VfxMeshData& out) {
    const size_t nv = m.v.size(), nf = m.f.size();
    std::vector<std::array<float, 3>> fn(nf), fu(nf);
    std::vector<std::vector<int>> adj(nv);
    std::vector<std::vector<int>> nbr(nv);
    for (size_t i = 0; i < nf; ++i) {
        const auto& a = m.v[m.f[i][0]];
        const auto& b = m.v[m.f[i][1]];
        const auto& c = m.v[m.f[i][2]];
        const std::array<float, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const std::array<float, 3> w{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        fn[i] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        fu[i] = normalized(fn[i]);
        for (int k = 0; k < 3; ++k) {
            adj[m.f[i][k]].push_back(int(i));
            nbr[m.f[i][k]].push_back(m.f[i][(k + 1) % 3]);
        }
    }
    std::vector<float> ao(nv, 1.0f);
    for (size_t i = 0; i < nv; ++i) {
        std::array<float, 3> n{0, 0, 0};
        for (int g : adj[i])
            for (int k = 0; k < 3; ++k) n[k] += fn[g][k];
        n = normalized(n);
        float conc = 0.0f;
        for (int j : nbr[i]) {
            const std::array<float, 3> d{m.v[j][0] - m.v[i][0], m.v[j][1] - m.v[i][1], m.v[j][2] - m.v[i][2]};
            conc += dot3(n, normalized(d));
        }
        conc /= float(std::max<size_t>(nbr[i].size(), 1));
        ao[i] = std::clamp(1.0f - conc * 2.2f, 0.45f, 1.0f);
    }
    const float cosCrease = std::cos(creaseDeg * 3.14159265f / 180.0f);
    std::map<std::pair<int, std::array<float, 3>>, uint16_t> weld;
    for (size_t i = 0; i < nf; ++i) {
        for (int k = 0; k < 3; ++k) {
            const int vi = m.f[i][k];
            std::array<float, 3> n{0, 0, 0};
            for (int g : adj[vi])
                if (dot3(fu[g], fu[i]) >= cosCrease)
                    for (int c = 0; c < 3; ++c) n[c] += fn[g][c];
            n = normalized(n);
            const auto d = normalized(m.v[vi]);
            VfxMeshVertex vx{};
            for (int c = 0; c < 3; ++c) {
                vx.pos[c] = m.v[vi][c];
                vx.normal[c] = n[c];
            }
            vx.uv[0] = std::atan2(d[2], d[0]) * (0.5f / 3.14159265f) + 0.5f;
            vx.uv[1] = std::acos(std::clamp(d[1], -1.0f, 1.0f)) / 3.14159265f;
            vx.ao = ao[vi];
            vx.fresh = fresh.empty() ? 0.0f : fresh[vi];
            // Canto igual (mesma posicao e normal dentro do vinco) vira um
            // vertice so': ~metade dos vertices, o vertex shader roda menos.
            const auto key = std::make_pair(vi, std::array<float, 3>{n[0], n[1], n[2]});
            auto it = weld.find(key);
            if (it == weld.end()) {
                it = weld.emplace(key, uint16_t(out.vertices.size())).first;
                out.vertices.push_back(vx);
            }
            out.indices.push_back(it->second);
        }
    }
}

// Pedra: esfera com relevo de baixa frequencia, cortada por planos
// sorteados (as faces de fratura) e achatada. Raio maximo 1.
void makeRock(uint32_t seed, int subdiv, int cuts, float cutMin, const std::array<float, 3>& scale, float crease,
              VfxMeshData& out) {
    WorkMesh m = icosphere(subdiv);
    std::vector<float> fresh(m.v.size(), 0.0f);
    std::array<float, 3> waveDir[4];
    for (int k = 0; k < 4; ++k) waveDir[k] = randomDir(seed, 40 + k);
    for (auto& p : m.v) {
        float r = 1.0f;
        for (int k = 0; k < 4; ++k)
            r += 0.07f * std::sin(dot3(p, waveDir[k]) * (2.0f + float(k)) + hash2(k, int(seed), 13) * 6.2831853f);
        for (float& x : p) x *= r;
    }
    for (int c = 0; c < cuts; ++c) {
        const auto n = randomDir(seed, c);
        const float d = cutMin + (0.85f - cutMin) * hash2(c, int(seed), 14);
        for (size_t i = 0; i < m.v.size(); ++i) {
            const float s = dot3(m.v[i], n) - d;
            if (s <= 0.0f) continue;
            for (int k = 0; k < 3; ++k) m.v[i][k] -= s * n[k];
            fresh[i] = 1.0f;
        }
    }
    float rmax = 1e-6f;
    for (auto& p : m.v) {
        for (int k = 0; k < 3; ++k) p[k] *= scale[k];
        rmax = std::max(rmax, std::sqrt(dot3(p, p)));
    }
    for (auto& p : m.v)
        for (float& x : p) x /= rmax;
    emitMesh(m, crease, fresh, out);
}

} // namespace

bool makeVfxBuiltinMesh(const std::string& fullName, VfxMeshData& out) {
    out = VfxMeshData{};
    const bool lo = fullName.size() > 3 && fullName.compare(fullName.size() - 3, 3, "#lo") == 0;
    const std::string name = lo ? fullName.substr(0, fullName.size() - 3) : fullName;
    const int sub = lo ? 1 : 2; // subdivisoes da esfera base (320 -> 80 triangulos)
    if (name == "builtin:cube") {
        WorkMesh m;
        const float c = 0.7f;
        m.v = {{-c, -c, -c}, {c, -c, -c}, {c, c, -c}, {-c, c, -c}, {-c, -c, c}, {c, -c, c}, {c, c, c}, {-c, c, c}};
        const int q[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
        for (const auto& f : q) {
            m.f.push_back({f[0], f[1], f[2]});
            m.f.push_back({f[0], f[2], f[3]});
        }
        emitMesh(m, 10.0f, {}, out);
        for (auto& v : out.vertices) v.ao = 1.0f; // cubo nao tem fundo
    } else if (name == "builtin:shard") {
        // Estilhaco: lasca fina e comprida, quase so' face de corte.
        makeRock(23u, sub - 1, 6, 0.45f, {1.0f, 0.3f, 0.55f}, 20.0f, out);
    } else if (name == "builtin:rock") {
        // Bloco de fratura: muitos cortes fundos (faces planas grandes), o
        // resto da esfera vira quina gasta. Vinco baixo: aresta viva.
        makeRock(7u, sub, 11, 0.38f, {1.0f, 0.78f, 0.9f}, 22.0f, out);
    } else if (name == "builtin:sphere") {
        WorkMesh m = icosphere(sub);
        emitMesh(m, 90.0f, {}, out);
        for (auto& v : out.vertices) v.ao = 1.0f;
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
