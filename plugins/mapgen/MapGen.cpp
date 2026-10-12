#include "MapGen.hpp"

#include "formats/TerrainParser.hpp"
#include "utils/ImageUtils.hpp"

#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <queue>
#include <sstream>

namespace mapgen {

using eruption::Vec2;

namespace {

namespace fs = std::filesystem;

constexpr float kPi = 3.14159265f;

// ---------------------------------------------------------------- ruído
// Ruído de gradiente 2D (gradiente por hash do canto, interpolação quíntica).

uint32_t hash2(int x, int z, uint32_t seed) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

float gradDot(int ix, int iz, float fx, float fz, uint32_t seed) {
    const float a = float(hash2(ix, iz, seed) & 0xffffu) / 65536.0f * 2.0f * kPi;
    return std::cos(a) * fx + std::sin(a) * fz;
}

float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

float gradientNoise(float x, float z, uint32_t seed) {
    const int ix = int(std::floor(x)), iz = int(std::floor(z));
    const float fx = x - ix, fz = z - iz;
    const float u = fade(fx), v = fade(fz);
    const float n00 = gradDot(ix, iz, fx, fz, seed), n10 = gradDot(ix + 1, iz, fx - 1, fz, seed);
    const float n01 = gradDot(ix, iz + 1, fx, fz - 1, seed), n11 = gradDot(ix + 1, iz + 1, fx - 1, fz - 1, seed);
    return (n00 + (n10 - n00) * u) + ((n01 + (n11 - n01) * u) - (n00 + (n10 - n00) * u)) * v; // ~[-0.7, 0.7]
}

float fbm(float x, float z, int octaves, float persistence, float lacunarity, uint32_t seed) {
    float sum = 0, amp = 1, norm = 0, f = 1;
    for (int o = 0; o < octaves; ++o) {
        sum += gradientNoise(x * f, z * f, seed + uint32_t(o) * 101u) * amp;
        norm += amp;
        amp *= persistence;
        f *= lacunarity;
    }
    return sum / std::max(norm, 1e-6f) * 1.4f; // ~[-1, 1]
}

float ridged(float x, float z, int octaves, uint32_t seed) {
    float sum = 0, amp = 1, norm = 0, f = 1;
    for (int o = 0; o < octaves; ++o) {
        const float n = 1.0f - std::abs(gradientNoise(x * f, z * f, seed + uint32_t(o) * 131u) * 1.4f);
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5f;
        f *= 2.0f;
    }
    return sum / std::max(norm, 1e-6f); // [0, 1]
}

struct Rng {
    uint64_t s;
    float next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return float(s >> 40) / float(1ull << 24);
    }
};

float smoothRange(float v, float lo, float hi, float soft) {
    // 1 dentro de [lo, hi], caindo suave fora (soft = largura da borda).
    float w = 1.0f;
    if (lo > -1e8f) w *= std::clamp((v - lo) / std::max(soft, 1e-4f) + 0.5f, 0.0f, 1.0f);
    if (hi < 1e8f) w *= std::clamp((hi - v) / std::max(soft, 1e-4f) + 0.5f, 0.0f, 1.0f);
    return w;
}

// ---------------------------------------------------------------- erosão
// Gotas de chuva escorrem pelo terreno: aceleram na descida, levam sedimento
// proporcional à velocidade e à água, depositam quando desaceleram ou sobem.

void erode(Result& r, int drops, float erodeRate, float depositRate, float inertia, uint32_t seed,
           const std::function<bool(float)>& tick) {
    const int w = r.w, d = r.d;
    auto heightAndGrad = [&](float px, float pz, float& gx, float& gz) {
        const int x = std::clamp(int(px), 0, w - 2), z = std::clamp(int(pz), 0, d - 2);
        const float u = px - x, v = pz - z;
        const float h00 = r.h(x, z), h10 = r.h(x + 1, z), h01 = r.h(x, z + 1), h11 = r.h(x + 1, z + 1);
        gx = (h10 - h00) * (1 - v) + (h11 - h01) * v;
        gz = (h01 - h00) * (1 - u) + (h11 - h10) * u;
        return h00 * (1 - u) * (1 - v) + h10 * u * (1 - v) + h01 * (1 - u) * v + h11 * u * v;
    };
    auto deposit = [&](float px, float pz, float amount) {
        const int x = std::clamp(int(px), 0, w - 2), z = std::clamp(int(pz), 0, d - 2);
        const float u = px - x, v = pz - z;
        r.height[size_t(z) * w + x] += amount * (1 - u) * (1 - v);
        r.height[size_t(z) * w + x + 1] += amount * u * (1 - v);
        r.height[size_t(z + 1) * w + x] += amount * (1 - u) * v;
        r.height[size_t(z + 1) * w + x + 1] += amount * u * v;
    };
    Rng rng{seed * 2654435761ull + 17};
    const float capacityFactor = 4.0f, minCapacity = 0.01f, evaporate = 0.02f, gravity = 4.0f;
    for (int i = 0; i < drops; ++i) {
        if ((i & 4095) == 0 && tick && !tick(float(i) / drops)) return;
        float px = rng.next() * (w - 1), pz = rng.next() * (d - 1);
        float dx = 0, dz = 0, speed = 1, water = 1, sediment = 0;
        for (int life = 0; life < 64; ++life) {
            float gx, gz;
            const float h = heightAndGrad(px, pz, gx, gz);
            dx = dx * inertia - gx * (1 - inertia);
            dz = dz * inertia - gz * (1 - inertia);
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-6f) break;
            dx /= len;
            dz /= len;
            const float ox = px, oz = pz;
            px += dx;
            pz += dz;
            if (px < 0 || pz < 0 || px >= w - 1 || pz >= d - 1) break;
            float ngx, ngz;
            const float dh = heightAndGrad(px, pz, ngx, ngz) - h;
            const float capacity = std::max(-dh * speed * water * capacityFactor, minCapacity);
            if (sediment > capacity || dh > 0) {
                const float amount = dh > 0 ? std::min(dh, sediment) : (sediment - capacity) * depositRate;
                sediment -= amount;
                deposit(ox, oz, amount);
            } else {
                const float amount = std::min((capacity - sediment) * erodeRate, -dh);
                deposit(ox, oz, -amount);
                sediment += amount;
            }
            speed = std::sqrt(std::max(0.0f, speed * speed - dh * gravity));
            water *= 1.0f - evaporate;
        }
    }
}

void smooth(Result& r, int radius, int iterations, float amount) {
    std::vector<float> tmp(r.height.size());
    for (int it = 0; it < iterations; ++it) {
        for (int z = 0; z < r.d; ++z)
            for (int x = 0; x < r.w; ++x) {
                float sum = 0;
                int n = 0;
                for (int dz = -radius; dz <= radius; ++dz)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int sx = std::clamp(x + dx, 0, r.w - 1), sz = std::clamp(z + dz, 0, r.d - 1);
                        sum += r.h(sx, sz);
                        ++n;
                    }
                tmp[size_t(z) * r.w + x] = r.h(x, z) + (sum / n - r.h(x, z)) * amount;
            }
        r.height.swap(tmp);
    }
}

// ------------------------------------------------------- "parece desenhado"
//
// O gerador produz forma EXATA: o `tube` abre corredor de largura constante
// com parede lisa, a `crater` abre circunferência, o `smooth` deixa rampa de
// derivada contínua. Nada disso existe em rocha. O olho reconhece a regra
// antes de reconhecer o lugar, e o mapa lê como desenho técnico.
//
// A correção NÃO é sujar tudo por igual — isso só troca "liso demais" por
// "barulhento demais" e apaga a forma que foi desenhada de propósito. A
// correção é MEDIR onde a superfície está regular demais e gastar a
// deformação ali.
//
// Regularidade = quanto a superfície se parece com o próprio borrão dela.
// Rocha de verdade tem detalhe em toda escala: tirar a média não muda quase
// nada, porque o que sobra é tão grande quanto o que saiu. Já uma rampa
// lisa, uma parede de tubo ou um piso aplanado SÃO a média deles mesmos — o
// resíduo é zero. Duas escalas de resíduo, normalizadas pelo relevo do
// próprio mapa, dão um número por ponto: 1 = desenhado, 0 = já é rocha.
std::vector<float> regularidade(const Result& r) {
    const size_t n = r.height.size();
    std::vector<float> b1(n), b2(n), reg(n);

    auto borrar = [&](const std::vector<float>& src, std::vector<float>& dst, int raio) {
        std::vector<float> tmp(src.size());
        const float inv = 1.0f / float(2 * raio + 1);
        for (int z = 0; z < r.d; ++z)                       // separável: horizontal
            for (int x = 0; x < r.w; ++x) {
                float soma = 0;
                for (int d = -raio; d <= raio; ++d)
                    soma += src[size_t(z) * r.w + std::clamp(x + d, 0, r.w - 1)];
                tmp[size_t(z) * r.w + x] = soma * inv;
            }
        for (int z = 0; z < r.d; ++z)                       // ... e vertical
            for (int x = 0; x < r.w; ++x) {
                float soma = 0;
                for (int d = -raio; d <= raio; ++d)
                    soma += tmp[size_t(std::clamp(z + d, 0, r.d - 1)) * r.w + x];
                dst[size_t(z) * r.w + x] = soma * inv;
            }
    };
    borrar(r.height, b1, 2);
    borrar(b1, b2, 6);

    // Referência: o desvio padrão do relevo do mapa. Sem normalizar, o mesmo
    // limiar valeria para uma montanha e para uma mesa.
    double soma = 0, soma2 = 0;
    for (float h : r.height) { soma += h; soma2 += double(h) * h; }
    const double media = soma / double(n);
    const float ref = std::max(1e-3f, float(std::sqrt(std::max(0.0, soma2 / double(n) - media * media))));

    for (size_t i = 0; i < n; ++i) {
        const float fino = std::abs(r.height[i] - b1[i]);
        const float medio = std::abs(b1[i] - b2[i]);
        // 0.045 e 0.085 do desvio do mapa: abaixo disso a superfície não tem
        // detalhe próprio naquela escala. Os dois pesos somam 1.
        const float t = 0.62f * std::clamp(fino / (ref * 0.045f), 0.0f, 1.0f)
                      + 0.38f * std::clamp(medio / (ref * 0.085f), 0.0f, 1.0f);
        reg[i] = 1.0f - t;
    }
    return reg;
}

// Deforma o DOMÍNIO: em vez de somar ruído na altura (que só encrespa a
// superfície e mantém a planta), lê a altura de um ponto deslocado. Corredor
// reto vira corredor que serpenteia, circunferência vira mancha, parede
// paralela deixa de ser paralela — a PLANTA deixa de ser desenhada, que é o
// que o olho estava reconhecendo. O deslocamento é proporcional à
// regularidade, então o que já era rocha não se mexe.
void deformarDominio(Result& r, const std::vector<float>& reg, const std::vector<float>& porta,
                     float amplitudeCelulas, float escalaMundo, uint32_t seed) {
    const std::vector<float> fonte = r.height;
    const float ox = r.originX, oz = r.originZ, c = r.cell;
    for (int z = 0; z < r.d; ++z)
        for (int x = 0; x < r.w; ++x) {
            const size_t i = size_t(z) * r.w + x;
            const float a = amplitudeCelulas * reg[i] * porta[i];
            if (a < 1e-3f) continue;
            const float wx = (ox + x * c) / escalaMundo, wz = (oz + z * c) / escalaMundo;
            const float dx = fbm(wx, wz, 3, 0.5f, 2.0f, seed) * a;
            const float dz = fbm(wx + 31.7f, wz - 17.3f, 3, 0.5f, 2.0f, seed + 101u) * a;
            const float fx = std::clamp(x + dx, 0.0f, float(r.w - 1.001f));
            const float fz = std::clamp(z + dz, 0.0f, float(r.d - 1.001f));
            const int ix = int(fx), iz = int(fz);
            const float u = fx - ix, v = fz - iz;
            r.height[i] = fonte[size_t(iz) * r.w + ix] * (1 - u) * (1 - v) +
                          fonte[size_t(iz) * r.w + ix + 1] * u * (1 - v) +
                          fonte[size_t(iz + 1) * r.w + ix] * (1 - u) * v +
                          fonte[size_t(iz + 1) * r.w + ix + 1] * u * v;
        }
}

// Erosão TÉRMICA (talude): onde o degrau entre vizinhos passa do ângulo de
// repouso, material desce. É o que transforma rampa lisa de derivada
// contínua em encosta facetada com pé de entulho — a diferença entre uma
// superfície calculada e uma superfície que desabou. A erosão por gotas que
// já existe aqui faz vale e canal; esta faz TALUDE, que é outro fenômeno.
void talude(Result& r, const std::vector<float>& reg, const std::vector<float>& porta,
            float anguloGraus, int passes, float taxa) {
    const float limite = std::tan(anguloGraus * kPi / 180.0f) * r.cell;
    std::vector<float> delta(r.height.size());
    const int viz[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int p = 0; p < passes; ++p) {
        std::fill(delta.begin(), delta.end(), 0.0f);
        for (int z = 1; z < r.d - 1; ++z)
            for (int x = 1; x < r.w - 1; ++x) {
                const size_t i = size_t(z) * r.w + x;
                const float peso = reg[i] * porta[i];
                if (peso < 1e-3f) continue;
                for (const auto& v : viz) {
                    const size_t j = size_t(z + v[1]) * r.w + (x + v[0]);
                    const float diag = (v[0] && v[1]) ? 1.41421356f : 1.0f;
                    const float d = r.height[i] - r.height[j] - limite * diag;
                    if (d <= 0) continue;
                    const float mover = d * taxa * 0.125f * peso;
                    delta[i] -= mover;
                    delta[j] += mover;
                }
            }
        for (size_t i = 0; i < r.height.size(); ++i) r.height[i] += delta[i];
    }
}

// Pontos (fração 0..1) que valem para as regras de inclinação/altura de uma etapa.
float ruleWeight(const Result& r, int x, int z, const Step& s) {
    const float soft = float(s.get("softness", 3.0));
    float w = smoothRange(r.slopeDeg(x, z), float(s.get("slope_min", -1e9)), float(s.get("slope_max", 1e9)), soft);
    w *= smoothRange(r.h(x, z), float(s.get("height_min", -1e9)), float(s.get("height_max", 1e9)), soft);
    // Bioma: um ruído bem largo divide o mapa em regiões; a etapa vale entre
    // region_min e region_max dele (mesmo region_seed = mesmo mapa de regiões).
    if (s.num.count("region_scale")) {
        const float scale = std::max(10.0f, float(s.get("region_scale", 600)));
        const uint32_t seed = uint32_t(s.get("region_seed", 1)) * 7919u + 13u;
        const float n = fbm((r.originX + x * r.cell) / scale, (r.originZ + z * r.cell) / scale, 3, 0.5f, 2.0f, seed) * 0.5f + 0.5f;
        w *= smoothRange(n, float(s.get("region_min", 0)), float(s.get("region_max", 1)), 0.04f);
    }
    return w;
}

// Pinta `tex` com peso m no ponto g (a camada nova cobre as anteriores).
bool paintAt(Result& r, const std::string& tex, size_t g, float m, std::string& error) {
    Layer* layer = nullptr;
    for (auto& l : r.layers)
        if (l.texture == tex) layer = &l;
    if (!layer) {
        if (r.layers.size() >= 8) {
            error = "At most 8 different paint layers.";
            return false;
        }
        r.layers.push_back({tex, std::vector<float>(r.height.size(), 0.0f)});
        layer = &r.layers.back();
    }
    for (auto& other : r.layers)
        if (&other != layer) other.weight[g] *= 1.0f - m;
    layer->weight[g] = std::max(layer->weight[g], m);
    return true;
}

// Cava/aplaina uma faixa ao longo de `path` (pontos da grade): dentro da
// metade da largura o chão vai para `bedAt(i)` (+ perfil), na margem se mistura.
template <typename Profile>
bool carveStrip(Result& r, const std::vector<Vec2>& path, const std::vector<float>& bed, float halfWidth, Profile profile,
                const std::string& tex, std::string& error) {
    const int reach = int(std::ceil(halfWidth * 2.0f)) + 1;
    std::vector<float> best(r.height.size(), FLT_MAX), target(r.height.size(), 0.0f);
    for (size_t i = 0; i < path.size(); ++i) {
        const int cx = int(std::lround(path[i].x)), cz = int(std::lround(path[i].y));
        for (int dz = -reach; dz <= reach; ++dz)
            for (int dx = -reach; dx <= reach; ++dx) {
                const int x = cx + dx, z = cz + dz;
                if (x < 0 || z < 0 || x >= r.w || z >= r.d) continue;
                const float t = glm::length(Vec2(x, z) - path[i]) / std::max(0.5f, halfWidth);
                const size_t g = size_t(z) * r.w + x;
                if (t < best[g]) {
                    best[g] = t;
                    target[g] = bed[i] + profile(std::min(t, 1.0f));
                }
            }
    }
    if (r.reserved.size() != r.height.size()) r.reserved.assign(r.height.size(), 0);
    for (size_t g = 0; g < r.height.size(); ++g) {
        const float t = best[g];
        if (t >= 2.0f) continue;
        const float k = t <= 1.0f ? 1.0f : 1.0f - (t - 1.0f) * (t - 1.0f) * (3.0f - 2.0f * (t - 1.0f));
        r.height[g] += (target[g] - r.height[g]) * k;
        if (t <= 1.0f) r.reserved[g] = 1;
        if (!tex.empty() && t < 1.3f && !paintAt(r, tex, g, std::clamp((1.3f - t) / 0.3f, 0.0f, 1.0f), error)) return false;
    }
    return true;
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

std::string num(double v) {
    char b[32];
    if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e12) std::snprintf(b, sizeof(b), "%.0f", v);
    else std::snprintf(b, sizeof(b), "%.4g", v);
    return b;
}

} // namespace

float Result::slopeDeg(int x, int z) const {
    const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1), zd = std::max(0, z - 1), zu = std::min(d - 1, z + 1);
    const float dx = (h(xr, z) - h(xl, z)) / (float(xr - xl) * cell);
    const float dz = (h(x, zu) - h(x, zd)) / (float(zu - zd) * cell);
    return std::atan(std::sqrt(dx * dx + dz * dz)) * 180.0f / kPi;
}

const std::vector<StepInfo>& stepCatalog() {
    static const std::vector<StepInfo> k = {
        {"noise", "Hills (noise)", "Rolling hills from layered noise.",
         {{"scale", 300, 10, 5000, "Size of the hills (world units)"},
          {"height", 80, -1000, 2000, "How tall"},
          {"octaves", 5, 1, 10, "Detail layers"},
          {"persistence", 0.5, 0.1, 0.9, "Roughness of the detail"}},
         {}},
        {"ridges", "Mountains (ridges)", "Sharp mountain ridges.",
         {{"scale", 500, 10, 5000, "Size of the ridges"}, {"height", 150, 0, 3000, "How tall"}, {"octaves", 4, 1, 8, "Detail"}},
         {}},
        {"tube", "Lava tube (canyon)", "Carves a winding corridor into the rock: a lava tube seen from above.",
         {{"axis", 0, 0, 1, "0 = runs west-east, 1 = north-south"},
          {"center", 0.5, 0.0, 1.0, "Where it crosses the map (fraction)"},
          {"width", 110, 20, 1200, "Floor width (world units)"},
          {"depth", 90, 0, 1000, "How deep it cuts"},
          {"wall", 0.55, 0.0, 1.0, "0 = open trench, 1 = steep walls"},
          {"wander", 150, 0, 2000, "How much it snakes sideways"},
          {"wander_scale", 600, 50, 5000, "Length of each bend"},
          {"flat", 0.7, 0.0, 1.0, "How flat the floor is"},
          {"width_var", 0.0, 0.0, 1.0, "Width varies along it (0 = constant)"},
          {"pinch", 0.0, 0.0, 1.0, "Floor rises where it narrows"},
          {"from", 0.0, 0.0, 1.0, "Starts at this fraction of the map"},
          {"to", 1.0, 0.0, 1.0, "Ends here"},
          {"level", 0, -3000, 3000, "Carve DOWN TO this height instead of by `depth` (0 = off)"}},
         {}},
        {"crater", "Crater (volcano)", "A ring of rock around a pit, in steps: the inside of a volcano.",
         {{"radius", 0.34, 0.05, 0.8, "Mouth radius (fraction of the map)"},
          {"rim", 180, 0, 2000, "How tall the wall around it is"},
          {"depth", 260, 0, 3000, "How deep the pit goes"},
          {"floors", 3, 1, 12, "Steps down the inside wall (the floors)"},
          {"level", 0, -3000, 3000, "Pit floor AT this height, flat (0 = off, follows the rock)"},
          {"floor_flat", 0.55, 0.0, 0.95, "How flat each step is (0 = ramp)"},
          {"rough", 0.35, 0.0, 1.0, "Rock noise on the wall"},
          {"warp", 0.0, 0.0, 1.0, "Bends the WHOLE pit out of round (fraction of the radius)"},
          {"warp_scale", 400, 80, 4000, "Size of each bulge (world units)"},
          {"center_x", 0.5, 0.0, 1.0, "Mouth centre, across"},
          {"center_z", 0.5, 0.0, 1.0, "Mouth centre, along"}},
         {}},
        {"island", "Island", "Lowers the edges so the land sits in the middle.",
         {{"radius", 0.42, 0.05, 0.7, "Land radius (fraction of the map)"}, {"depth", 60, 0, 1000, "How much the edges sink"}},
         {}},
        {"erosion", "Erosion (rain)", "Rain carves valleys and leaves sediment at the bottom.",
         {{"drops", 60000, 0, 2000000, "Rain drops"},
          {"strength", 0.3, 0.01, 1, "How much each drop digs"},
          {"deposit", 0.3, 0.01, 1, "How fast mud settles"},
          {"inertia", 0.05, 0, 0.9, "Drops keep their direction"}},
         {}},
        {"smooth", "Smooth", "Softens sharp edges.",
         {{"radius", 1, 1, 6, "Neighbourhood"}, {"iterations", 1, 1, 20, "Passes"}, {"amount", 0.6, 0, 1, "Strength"}},
         {}},
        {"terrace", "Terraces", "Steps like rice fields or mesas.",
         {{"steps", 8, 2, 64, "Number of steps"}, {"sharpness", 0.6, 0, 1, "0 = soft ramps, 1 = flat steps"}},
         {}},
        {"raise", "Raise / lower", "Adds a height to the whole map.", {{"amount", 0, -2000, 2000, "Units"}}, {}},
        {"water", "Water level", "Water surface; ground below it becomes lakes and sea.", {{"level", 0, -1000, 3000, "Height"}}, {}},
        {"paint", "Paint layer", "Paints a ground texture where the rules match.",
         {{"slope_min", -1e9, 0, 90, "From this slope (degrees)"},
          {"slope_max", 1e9, 0, 90, "Up to this slope"},
          {"height_min", -1e9, -1000, 3000, "From this height"},
          {"height_max", 1e9, -1000, 3000, "Up to this height"},
          {"softness", 3, 0.1, 50, "Blend at the borders"},
          {"coverage", 1, 0, 1, "Patchiness (1 = everywhere the rules match)"},
          {"region_scale", 1e9, 50, 5000, "Biome: size of the regions (turn on to use)"},
          {"region_min", 0, 0, 1, "Biome: from this part of the region noise"},
          {"region_max", 1, 0, 1, "Biome: up to this part"},
          {"region_seed", 1, 1, 100, "Biome: same number = same regions"}},
         {"layer"}},
        {"realistic", "Realistic (break the geometry)",
         "Finds where the ground still looks DRAWN (smooth ramps, constant-width corridors, "
         "round craters) and spends the deformation there. Also makes every LATER scatter step "
         "behave like nature: clustered, power-law sizes, bedded into the slope. Put it after "
         "the shapes are carved and before the scatter steps. Man-made pieces opt out with "
         "natural = 0 on their own scatter step.",
         {{"strength", 1, 0, 3, "Overall amount"},
          {"scale", 110, 10, 2000, "Size of the meander (world units)"},
          {"detail", 1, 0, 3, "Micro relief added to the smooth parts"},
          {"talus", 1, 0, 6, "Scree: slopes collapse to the angle of repose"},
          {"repose", 55, 10, 85, "Angle of repose (degrees). 55 = rock; 34 = sand/scree"},
          {"tilt", 14, 0, 60, "Later scatter: how far a piece may lean (degrees)"},
          {"clump", 1, 0, 3, "Later scatter: how much it clusters"},
          {"sink", 0.25, 0, 3, "Later scatter: how deep it beds in (model units)"},
          {"size_power", 2.2, 1, 6, "Later scatter: 1 = uniform sizes, higher = many small few big"},
          {"slope_min", -1e9, 0, 90, "From this slope (degrees)"},
          {"slope_max", 1e9, 0, 90, "Up to this slope"},
          {"height_min", -1e9, -1000, 3000, "From this height"},
          {"height_max", 1e9, -1000, 3000, "Up to this height"},
          {"softness", 3, 0.1, 50, "Blend at the borders"},
          {"region_scale", 1e9, 50, 5000, "Biome: size of the regions (turn on to use)"},
          {"region_min", 0, 0, 1, "Biome: from this part of the region noise"},
          {"region_max", 1, 0, 1, "Biome: up to this part"},
          {"region_seed", 1, 1, 100, "Biome: same number = same regions"}},
         {}},
        {"scatter", "Scatter objects", "Places a model (.glb or prefab) where the rules match.",
         {{"density", 1, 0.01, 200, "Objects per 100 x 100 units"},
          {"slope_min", -1e9, 0, 90, "From this slope"},
          {"slope_max", 1e9, 0, 90, "Up to this slope"},
          {"height_min", -1e9, -1000, 3000, "From this height"},
          {"height_max", 1e9, -1000, 3000, "Up to this height"},
          {"scale_min", 0.8, 0.05, 20, "Smallest size"},
          {"scale_max", 1.2, 0.05, 20, "Largest size"},
          {"natural", 1, 0, 1, "0 = man-made: a previous 'realistic' step does NOT touch it"},
          {"region_scale", 1e9, 50, 5000, "Biome: size of the regions (turn on to use)"},
          {"region_min", 0, 0, 1, "Biome: from this part of the region noise"},
          {"region_max", 1, 0, 1, "Biome: up to this part"},
          {"region_seed", 1, 1, 100, "Biome: same number = same regions"}},
         {"model"}},
        {"river", "River", "Carves a riverbed downhill from the highest point (or x, z) to the water or the border.",
         {{"width", 24, 4, 200, "Width (world units)"},
          {"depth", 6, 0.5, 60, "How deep the bed goes"},
          {"meander", 0.6, 0, 3, "How much it winds"},
          {"x", 0.5, 0, 1, "Start across (optional)"},
          {"z", 0.5, 0, 1, "Start along (optional)"}},
         {"layer"}},
        {"road", "Road", "Flattest way between two points, levelled and painted.",
         {{"from_x", 0, 0, 1, "Start across"}, {"from_z", 0.5, 0, 1, "Start along"},
          {"to_x", 1, 0, 1, "End across"}, {"to_z", 0.5, 0, 1, "End along"},
          {"width", 16, 2, 100, "Width (world units)"}, {"avoid_slopes", 4, 0, 50, "How much it goes around hills"}},
         {"layer"}},
        {"spawn", "Player start", "Where the player begins (fractions of the map).",
         {{"x", 0.5, 0, 1, "Across"}, {"z", 0.5, 0, 1, "Along"}},
         {}},
        {"walk", "Walkable slopes", "Steeper ground than this blocks walking.", {{"max_slope", 40, 5, 89, "Degrees"}}, {}},
    };
    return k;
}

const StepInfo* findStep(const std::string& type) {
    for (const auto& s : stepCatalog())
        if (type == s.type) return &s;
    return nullptr;
}

// ---------------------------------------------------------------- receita

bool Recipe::load(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "Recipe not found: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), error);
}

bool Recipe::parse(const std::string& source, std::string& error) {
    *this = Recipe{};
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_sandbox(L);
    size_t size = 0;
    char* bc = luau_compile(source.data(), source.size(), nullptr, &size);
    const bool ok = luau_load(L, "@recipe", bc, size, 0) == 0 && lua_pcall(L, 0, 1, 0) == 0;
    std::free(bc);
    if (!ok || !lua_istable(L, -1)) {
        error = ok ? "The recipe must return a table." : lua_tostring(L, -1);
        lua_close(L);
        return false;
    }
    const int root = lua_gettop(L);
    auto field = [&](const char* k, double def) {
        lua_getfield(L, root, k);
        const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
        lua_pop(L, 1);
        return v;
    };
    this->size = float(field("size", 1000));
    cell = float(field("cell", 8));
    seed = uint32_t(field("seed", 1));
    lua_getfield(L, root, "origin");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        originX = float(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 2);
        originZ = float(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, root, "steps");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                Step s;
                lua_rawgeti(L, -1, 1);
                if (lua_isstring(L, -1)) s.type = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        const std::string k = lua_tostring(L, -2);
                        if (k == "enabled") s.enabled = lua_toboolean(L, -1) != 0;
                        else if (lua_type(L, -1) == LUA_TNUMBER) s.num[k] = lua_tonumber(L, -1);
                        else if (lua_type(L, -1) == LUA_TSTRING) s.str[k] = lua_tostring(L, -1);
                    }
                    lua_pop(L, 1);
                }
                if (!s.type.empty()) steps.push_back(std::move(s));
            }
            lua_pop(L, 1);
        }
    }
    lua_close(L);
    if (this->size < 64 || cell < 1 || this->size / cell > 1023) {
        error = "size / cell must give between 8 and 1023 grid points.";
        return false;
    }
    return true;
}

std::string Recipe::serialize() const {
    std::ostringstream o;
    o << "-- Map recipe: steps run top to bottom (Map Generator in the editor).\n"
         "return {\n    size = " << num(size) << ", cell = " << num(cell) << ", seed = " << seed;
    if (originX != 0.0f || originZ != 0.0f) o << ", origin = {" << num(originX) << ", " << num(originZ) << "}";
    o << ",\n    steps = {\n";
    for (const auto& s : steps) {
        o << "        { " << quote(s.type);
        if (!s.enabled) o << ", enabled = false";
        for (const auto& [k, v] : s.str) o << ", " << k << " = " << quote(v);
        for (const auto& [k, v] : s.num) o << ", " << k << " = " << num(v);
        o << " },\n";
    }
    o << "    },\n}\n";
    return o.str();
}

// ---------------------------------------------------------------- execução

bool run(const Recipe& recipe, int resolution, Result& r, std::string& error,
         const std::function<bool(float, const char*)>& progress) {
    r = Result{};
    const int full = int(std::round(recipe.size / recipe.cell)) + 1;
    const int n = resolution > 0 ? std::min(resolution, full) : full;
    // Uma célula a mais em volta: inclinação, pintura e normais da borda veem o
    // relevo do outro lado (o mapa vizinho com origin ao lado vê o mesmo). Sai no fim.
    constexpr int A = 1;
    r.w = r.d = n + 2 * A;
    r.cell = recipe.size / float(n - 1);
    r.height.assign(size_t(r.w) * r.d, 0.0f);
    const float scaleToWorld = r.cell;
    const float ox = recipe.originX - A * r.cell, oz = recipe.originZ - A * r.cell;
    r.originX = ox;
    r.originZ = oz;
    r.reserved.assign(r.height.size(), 0);
    // Ligado pela etapa "realistic" e lido pelas etapas de scatter que vierem
    // DEPOIS dela. É por ordem, e não por bandeira global, porque num mesmo
    // mapa convivem a encosta (natural) e a mina escorada por gente
    // (construída): a construída é escrita antes, ou pede natural = 0.
    struct Natureza {
        bool ligado = false;
        float tilt = 0.0f, clump = 0.0f, sink = 0.0f, sizePower = 1.0f;
        uint32_t seed = 0;
    } nat;
    int stepIndex = 0;
    for (const Step& s : recipe.steps) {
        const float base = float(stepIndex++) / std::max<size_t>(1, recipe.steps.size());
        if (!s.enabled) continue;
        const StepInfo* info = findStep(s.type);
        if (!info) {
            error = "Unknown step \"" + s.type + "\".";
            return false;
        }
        if (progress && !progress(base, info->label)) return false;
        const uint32_t seed = recipe.seed * 7919u + uint32_t(stepIndex) * 104729u;
        if (s.type == "noise" || s.type == "ridges") {
            const float scale = float(s.get("scale", s.type == "noise" ? 300 : 500));
            const float height = float(s.get("height", s.type == "noise" ? 80 : 150));
            const int oct = int(s.get("octaves", s.type == "noise" ? 5 : 4));
            const float pers = float(s.get("persistence", 0.5));
            for (int z = 0; z < r.d; ++z)
                for (int x = 0; x < r.w; ++x) {
                    const float wx = (ox + x * scaleToWorld) / scale, wz = (oz + z * scaleToWorld) / scale;
                    const float n = s.type == "noise" ? fbm(wx, wz, oct, pers, 2.0f, seed) * 0.5f + 0.5f : ridged(wx, wz, oct, seed);
                    r.height[size_t(z) * r.w + x] += n * height;
                }
        } else if (s.type == "tube") {
            // TUBO DE LAVA. Um corredor que serpenteia e e' cavado na rocha,
            // com piso quase plano e parede subindo dos dois lados. E' a
            // forma real de um tubo de lava visto de cima, e o unico jeito de
            // ter CORREDOR num gerador que so' sabe mexer em altura.
            const bool eixoZ = s.get("axis", 0) >= 0.5;
            const float centro = float(s.get("center", 0.5));
            const float largura = float(s.get("width", 110));
            const float fundo = float(s.get("depth", 90));
            const float parede = float(s.get("wall", 0.55));
            const float serpente = float(s.get("wander", 150));
            const float escalaSerp = float(s.get("wander_scale", 600));
            const float plano = float(s.get("flat", 0.7));
            // LARGURA VARIAVEL. Sem isto o tubo e' uma vala de largura
            // constante: por construcao ele so' pode sair retangulo, e visto
            // de cima o mapa inteiro parece uma caixa cavada. Um tubo de lava
            // real aperta e alarga ao longo do curso, e e' esse estrangulamento
            // que da' ritmo ao corredor - sala, gargalo, sala.
            const float varia = float(s.get("width_var", 0.0));
            // Onde aperta, o chao tambem SOBE um pouco: gargalo de tubo nao e'
            // so' mais estreito, e' mais raso, porque a lava deixou mais crosta.
            const float aperta = float(s.get("pinch", 0.0));
            const float de = float(s.get("from", 0.0)), ate = float(s.get("to", 1.0));
            // COTA ABSOLUTA. Por `depth` o tubo cava um tanto a partir do que
            // ja' esta ali, entao dois tubos que se cruzam cavam duas vezes e
            // o cruzamento vira poco. Com `level` o piso de todos eles para
            // na mesma altura e a planta pode ser uma REDE de corredores, que
            // e' o que uma caverna de dungeon e'. 0 mantem o comportamento
            // antigo (ninguem cava ate' a cota zero de proposito).
            const bool porCota = s.has("level");
            const float cota = float(s.get("level", 0.0));
            const float lado = recipe.size;
            for (int z = 0; z < r.d; ++z)
                for (int x = 0; x < r.w; ++x) {
                    // u = ao longo do tubo, v = atravessado nele (0..1).
                    const float fx = float(x - A) / (n - 1), fz = float(z - A) / (n - 1);
                    const float u = eixoZ ? fz : fx;
                    const float v = eixoZ ? fx : fz;
                    if (u < de || u > ate) continue;
                    // A linha do meio serpenteia com ruido ao longo do tubo.
                    const float desvio = serpente * fbm(u * lado / escalaSerp, 7.3f, 3, 0.5f, 2.0f, seed) / lado;
                    const float dist = std::abs(v - centro - desvio) * lado;  // unidades de mundo
                    // Ruido proprio (semente e frequencia diferentes do
                    // serpenteio) para o tubo nao afinar sempre na curva.
                    const float sopro = fbm(u * lado / std::max(60.0f, escalaSerp * 0.55f), 31.7f, 3, 0.5f, 2.0f, seed + 911u);
                    const float fator = 1.0f - varia * 0.5f * (1.0f - sopro);
                    const float meia = largura * 0.5f * std::max(0.25f, fator);
                    if (dist > meia * (1.0f + parede * 2.0f)) continue;
                    // Piso plano no miolo; parede subindo ate' a borda.
                    float t;
                    if (dist <= meia * plano) {
                        t = 1.0f;                                  // chao do tubo
                    } else {
                        const float k = std::clamp((dist - meia * plano) / std::max(1.0f, meia * (1.0f + parede * 2.0f) - meia * plano), 0.0f, 1.0f);
                        t = 1.0f - k * k * (3.0f - 2.0f * k);      // sobe suave ate' a rocha
                    }
                    // Ponta do tubo: fecha nas extremidades em vez de cortar reto.
                    const float borda = std::min(u - de, ate - u);
                    const float fecha = std::clamp(borda / 0.06f, 0.0f, 1.0f);
                    float& h = r.height[size_t(z) * r.w + x];
                    // Pelo gargalo o piso sobe um pouco tambem na cota fixa.
                    const float sobe = fundo * aperta * (1.0f - std::max(0.25f, fator));
                    const float alvo = porCota ? std::min(h, cota + sobe)
                                               : h - fundo * (1.0f - aperta * (1.0f - std::max(0.25f, fator)));
                    h = h + (alvo - h) * t * fecha;
                }
        } else if (s.type == "crater") {
            // CRATERA. De fora para dentro: encosta subindo ate' a borda,
            // depois a parede caindo em DEGRAUS ate' o fundo chato. Cada
            // degrau e' um andar da dungeon - e' o que faz um vulcao por
            // dentro ser jogavel a pe' em vez de um funil liso.
            const float radius = float(s.get("radius", 0.34));
            const float rim = float(s.get("rim", 180));
            const float depth = float(s.get("depth", 260));
            const int floors = std::max(1, int(s.get("floors", 3)));
            const float flat = float(s.get("floor_flat", 0.55));
            const float rough = float(s.get("rough", 0.35));
            // DEFORMACAO DE DOMINIO, que e' outra coisa que `rough`.
            //
            // `rough` multiplica a distancia por ruido do ANGULO, com teto de
            // 12% do raio: a boca sai um oval amassado e, pior, a deformacao
            // e' a mesma em toda distancia — os degraus continuam curvas de
            // nivel CONCENTRICAS, que e' o que o olho le como compasso. Com
            // 0.62 de rough o lago de lava da dungeon continuou redondo na
            // foto.
            //
            // Aqui o PONTO DE CONSULTA anda antes de medir a distancia, como
            // na etapa `realistic`. O deslocamento depende da posicao (nao do
            // angulo), entao cada degrau entorta por conta propria: aparecem
            // baia, peninsula e istmo, e a linha d'agua deixa de ter raio
            // unico. A escala do bojo e' em unidades de MUNDO, porque o que
            // importa e' o tamanho do bojo contra o tamanho do boneco.
            const float warp = float(s.get("warp", 0.0));
            const float warpScale = std::max(1.0f, float(s.get("warp_scale", 400.0)));
            const float cx = float(s.get("center_x", 0.5)), cz = float(s.get("center_z", 0.5));
            // COTA ABSOLUTA do fundo, como no tubo. Sem ela o degrau de baixo
            // herda toda a ondulacao da rocha de base: num poco de 280 com
            // ruido de 180 o "fundo" varia mais que a profundidade, e um lago
            // posto na cota media aparece so' no meio, com o resto do leito
            // acima da agua. Com `level` o fundo e' chato de verdade e cada
            // degrau acima dele sobe depth/floors.
            const bool porCota = s.has("level");
            const float cota = float(s.get("level", 0.0));
            for (int z = 0; z < r.d; ++z)
                for (int x = 0; x < r.w; ++x) {
                    float dx = float(x - A) / (n - 1) - cx, dz = float(z - A) / (n - 1) - cz;
                    if (warp > 0.0f) {
                        // O deslocamento MORRE PERTO DO CENTRO. Sem isso ele
                        // puxa o ponto de distancia zero para fora de baixo
                        // do centro, o fundo pedido nunca e' alcancado e a
                        // cratera perde o patamar de baixo: medido num poco
                        // de 120 de raio, o fundo subiu de -180 para -149 e
                        // virou um V de 59 graus. `warp` tem de entortar a
                        // BOCA, nao roubar a profundidade.
                        const float d0 = std::sqrt(dx * dx + dz * dz) / std::max(1e-4f, radius);
                        const float k = std::clamp(d0 / 0.45f, 0.0f, 1.0f);
                        // Em FRACAO do mapa, como dx/dz: warp e' fracao do raio.
                        const float wx = (r.originX + x * r.cell) / warpScale;
                        const float wz = (r.originZ + z * r.cell) / warpScale;
                        const float a = warp * radius * k * k * (3.0f - 2.0f * k);
                        dx += a * fbm(wx, wz, 3, 0.5f, 2.0f, seed + 7u);
                        dz += a * fbm(wx + 19.3f, wz - 7.1f, 3, 0.5f, 2.0f, seed + 71u);
                    }
                    float dist = std::sqrt(dx * dx + dz * dz);
                    // Borda irregular: a boca nao e' um circulo de compasso.
                    if (rough > 0.0f) {
                        const float ang = std::atan2(dz, dx);
                        dist *= 1.0f + rough * 0.12f * fbm(std::cos(ang) * 2.0f, std::sin(ang) * 2.0f, 3, 0.5f, 2.0f, seed);
                    }
                    float& h = r.height[size_t(z) * r.w + x];
                    if (dist >= radius) {
                        // Fora: encosta do cone, alta na boca e caindo para longe.
                        const float t = std::clamp((dist - radius) / std::max(0.05f, 0.5f - radius), 0.0f, 1.0f);
                        h += rim * (1.0f - t) * (1.0f - t);
                        continue;
                    }
                    // Dentro: 0 na borda, 1 no centro.
                    const float t = 1.0f - dist / std::max(1e-4f, radius);
                    const float passo = t * float(floors);
                    const float degrau = std::floor(passo);
                    const float dentro = passo - degrau;
                    // `flat` = quanto do degrau e' patamar; o resto e' rampa.
                    const float rampa = flat >= 0.999f ? 0.0f
                                      : std::clamp((dentro - flat) / (1.0f - flat), 0.0f, 1.0f);
                    const float nivel = (degrau + rampa * rampa * (3.0f - 2.0f * rampa)) / float(floors);
                    if (porCota) {
                        // `nivel` chega a 1 so' NO PONTO central: usado como
                        // esta', o fundo pedido virava um cone com a cota so'
                        // no miolo, e um lago posto nela aparecia do tamanho
                        // de uma poca. Aqui o que conta e' o DEGRAU: o de
                        // dentro inteiro fica na cota, o de fora na boca.
                        const float plano = std::min(
                            1.0f, (degrau + rampa * rampa * (3.0f - 2.0f * rampa)) /
                                      std::max(1.0f, float(floors - 1)));
                        // Junto da boca mistura com a rocha que ja' estava
                        // ali, senao o degrau de cima vira um corte reto.
                        const float alvo = cota + (1.0f - plano) * depth;
                        const float k = std::clamp(t / 0.18f, 0.0f, 1.0f);
                        h += (alvo - h) * k * k * (3.0f - 2.0f * k);
                        continue;
                    }
                    h += rim - (rim + depth) * nivel;
                }
        } else if (s.type == "island") {
            const float radius = float(s.get("radius", 0.42)), depth = float(s.get("depth", 60));
            for (int z = 0; z < r.d; ++z)
                for (int x = 0; x < r.w; ++x) {
                    const float dx = float(x - A) / (n - 1) - 0.5f, dz = float(z - A) / (n - 1) - 0.5f;
                    const float t = std::clamp((std::sqrt(dx * dx + dz * dz) - radius) / 0.12f, 0.0f, 1.0f);
                    r.height[size_t(z) * r.w + x] -= t * t * (3 - 2 * t) * depth;
                }
        } else if (s.type == "erosion") {
            // Gotas na proporção da grade (a prévia em baixa resolução usa menos).
            const float frac = float(r.w * r.d) / float(full * full);
            erode(r, int(s.get("drops", 60000) * frac), float(s.get("strength", 0.3)), float(s.get("deposit", 0.3)),
                  float(s.get("inertia", 0.05)), seed, [&](float t) {
                      return !progress || progress(base + t / std::max<size_t>(1, recipe.steps.size()), info->label);
                  });
        } else if (s.type == "smooth") {
            smooth(r, int(s.get("radius", 1)), int(s.get("iterations", 1)), float(s.get("amount", 0.6)));
        } else if (s.type == "terrace") {
            const float steps = float(s.get("steps", 8)), sharp = float(s.get("sharpness", 0.6));
            const auto [lo, hi] = std::minmax_element(r.height.begin(), r.height.end());
            const float h0 = *lo, range = std::max(1e-3f, *hi - *lo);
            for (float& h : r.height) {
                const float t = (h - h0) / range * steps;
                const float fl = std::floor(t), f = t - fl;
                const float k = std::pow(f, 1.0f + sharp * 8.0f); // plano e subida curta
                h = h0 + (fl + k) / steps * range;
            }
        } else if (s.type == "raise") {
            for (float& h : r.height) h += float(s.get("amount", 0));
        } else if (s.type == "water") {
            r.hasWater = true;
            r.waterLevel = float(s.get("level", 0));
        } else if (s.type == "paint") {
            const std::string tex = s.text("layer");
            if (tex.empty()) continue;
            const float coverage = float(s.get("coverage", 1));
            for (int z = 0; z < r.d; ++z)
                for (int x = 0; x < r.w; ++x) {
                    float m = ruleWeight(r, x, z, s);
                    if (coverage < 1.0f) {
                        const float n = fbm((ox + x * scaleToWorld) / 80.0f, (oz + z * scaleToWorld) / 80.0f, 3, 0.5f, 2.0f, seed) *
                                            0.5f + 0.5f;
                        m *= std::clamp((n - (1.0f - coverage)) * 6.0f, 0.0f, 1.0f);
                    }
                    if (m <= 0) continue;
                    if (!paintAt(r, tex, size_t(z) * r.w + x, m, error)) return false;
                }
        } else if (s.type == "realistic") {
            const float forca = float(s.get("strength", 1));
            // Porta: as mesmas regras de inclinação/altura/região das outras
            // etapas. É assim que isto vira "aplica nesta região" — e é assim
            // que o que foi CONSTRUÍDO fica de fora, porque quem constrói
            // escolhe onde.
            std::vector<float> porta(r.height.size(), 1.0f);
            for (int z = 0; z < r.d; ++z)
                for (int x = 0; x < r.w; ++x) {
                    const size_t i = size_t(z) * r.w + x;
                    // Leito de rio e estrada são traçado, não acidente: ficam.
                    porta[i] = r.reserved[i] ? 0.0f : ruleWeight(r, x, z, s);
                }
            if (forca > 1e-3f) {
                const std::vector<float> reg = regularidade(r);
                double quanto = 0;
                for (size_t i = 0; i < reg.size(); ++i) quanto += double(reg[i] * porta[i]);
                const float parte = float(quanto / double(reg.size()));

                const float escala = float(s.get("scale", 110));
                // Amplitude em CÉLULAS. Um quarto da onda: desloca o bastante
                // para a planta deixar de ser reconhecível e pouco o bastante
                // para não fechar corredor (o mais estreito aqui tem 140
                // unidades contra ~27 de deslocamento máximo).
                deformarDominio(r, reg, porta, forca * escala * 0.25f / r.cell, escala, seed);

                const int passes = int(s.get("talus", 1) * 12);
                if (passes > 0)
                    talude(r, reg, porta, float(s.get("repose", 55)), passes, std::min(1.0f, forca));

                // Relevo SÓ no que estava liso.
                //
                // AMPLITUDE EM UNIDADES DE MUNDO, não em células da grade. A
                // primeira volta disto amarrou a amplitude à célula e saiu
                // com ~1 unidade de relevo num personagem de 42 — 2% dele,
                // ou seja, nada. O relevo tem de ser medido contra o que
                // anda em cima dele: a maior onda vale 6,5% da ondulação
                // pedida (130 -> 8,5 unidades, um quinto de personagem), e
                // cada oitava seguinte vale metade, que é a proporção de uma
                // superfície fraturada de verdade.
                //
                // O piso da onda continua amarrado à célula: onda de menos
                // de 2,5 células vira serra de um triângulo e o mapa de
                // sombra responde com mancha preta de borda reta.
                const float det = float(s.get("detail", 1)) * forca;
                if (det > 1e-3f) {
                    const std::vector<float> reg2 = regularidade(r);
                    for (int z = 0; z < r.d; ++z)
                        for (int x = 0; x < r.w; ++x) {
                            const size_t i = size_t(z) * r.w + x;
                            const float peso = reg2[i] * porta[i];
                            if (peso < 1e-3f) continue;
                            const float wx = ox + x * scaleToWorld, wz = oz + z * scaleToWorld;
                            float soma = 0.0f, amp = escala * 0.065f, onda = escala * 0.45f;
                            for (int o = 0; o < 4 && onda > r.cell * 2.5f; ++o) {
                                soma += amp * fbm(wx / onda, wz / onda, 2, 0.5f, 2.0f, seed + 7u * uint32_t(o));
                                amp *= 0.5f;
                                onda *= 0.45f;
                            }
                            r.height[i] += soma * det * peso;
                        }
                }
                char msg[160];
                std::snprintf(msg, sizeof(msg), "realistic: %.0f%% do mapa estava geometrico", parte * 100.0f);
                if (progress) progress(base, msg);
            }
            nat.ligado = true;
            nat.tilt = float(s.get("tilt", 14));
            nat.clump = float(s.get("clump", 1));
            nat.sink = float(s.get("sink", 0.25));
            nat.sizePower = std::max(1.0f, float(s.get("size_power", 2.2)));
            nat.seed = seed;
        } else if (s.type == "scatter") {
            const std::string model = s.text("model");
            if (model.empty()) continue;
            ScatterGroup g;
            g.model = model;
            const float density = float(s.get("density", 1));
            Rng rng{uint64_t(seed) * 977u + 5};
            const float worldSize = (r.w - 1) * r.cell;
            const float escMin = float(s.get("scale_min", 0.8)), escMax = float(s.get("scale_max", 1.2));
            // A etapa "realistic" anterior vale aqui, a menos que esta peça
            // seja construída (natural = 0): poste escorado por gente fica de
            // pé, alinhado e do tamanho que foi cortado.
            const bool natural = nat.ligado && s.get("natural", 1) > 0.5;
            const float ondaClump = 170.0f;
            const float expClump = natural ? 1.0f + 2.0f * nat.clump : 1.0f;

            // Agrupamento muda a densidade média, então ela é medida numa
            // amostra grossa e devolvida na ACEITAÇÃO (não no espaçamento:
            // compensar nos dois lugares multiplicou a contagem por 1/média
            // e o primeiro mapa saiu com 3614 peças em vez de 725).
            float medioClump = 1.0f;
            if (natural && nat.clump > 1e-3f) {
                double soma = 0;
                for (int i = 0; i < 48; ++i)
                    for (int j = 0; j < 48; ++j) {
                        const float cx = ox + worldSize * (i + 0.5f) / 48.0f;
                        const float cz = oz + worldSize * (j + 0.5f) / 48.0f;
                        const float c = std::clamp(fbm(cx / ondaClump, cz / ondaClump, 3, 0.5f, 2.0f, nat.seed + 303u) * 0.5f + 0.5f, 0.0f, 1.0f);
                        soma += std::pow(double(c), double(expClump));
                    }
                medioClump = std::max(0.05f, float(soma / (48.0 * 48.0)));
            }
            const float spacing = 100.0f / std::sqrt(std::max(density, 1e-3f));

            for (float cz = 0; cz < worldSize; cz += spacing)
                for (float cx = 0; cx < worldSize; cx += spacing) {
                    // SORTEIO QUE PASSA DA CÉLULA. O código antigo sorteava um
                    // ponto DENTRO de cada célula de uma grade regular: isso é
                    // um reticulado com tremor, e em densidade alta o olho lê a
                    // fileira. Espalhando por 1,8 célula a partir de -0,4 as
                    // células se sobrepõem e a fileira some. Em peça construída
                    // o reticulado FICA, porque quem assenta trilho assenta em
                    // linha.
                    const float j0 = natural ? -0.4f : 0.0f, j1 = natural ? 1.8f : 1.0f;
                    const float px = cx + (j0 + rng.next() * j1) * spacing;
                    const float pz = cz + (j0 + rng.next() * j1) * spacing;
                    if (px < 0 || pz < 0 || px > worldSize || pz > worldSize) continue;
                    const int gx = std::clamp(int(px / r.cell + 0.5f), 0, r.w - 1), gz = std::clamp(int(pz / r.cell + 0.5f), 0, r.d - 1);
                    float keep = ruleWeight(r, gx, gz, s);
                    if (natural && nat.clump > 1e-3f) {
                        // Natureza não espalha por igual: ela ajunta. Pedra
                        // vem em campo de pedra, e entre dois campos o chão
                        // fica limpo.
                        const float c = std::clamp(fbm((ox + px) / ondaClump, (oz + pz) / ondaClump, 3, 0.5f, 2.0f, nat.seed + 303u) * 0.5f + 0.5f, 0.0f, 1.0f);
                        keep *= std::pow(c, expClump) / medioClump;
                    }
                    if (keep <= 0 || rng.next() > keep) {
                        rng.next();
                        rng.next();
                        continue;
                    }
                    if (r.hasWater && r.h(gx, gz) < r.waterLevel) continue;
                    if (r.reserved[size_t(gz) * r.w + gx]) continue; // leito de rio, estrada
                    // TAMANHO POR LEI DE POTÊNCIA. Sorteio uniforme dá tantas
                    // pedras grandes quanto pequenas, e é por isso que campo
                    // de entulho gerado lê como "bolinhas do mesmo tamanho".
                    // Fragmentação real dá muitas pequenas e poucas grandes.
                    const float t = natural ? std::pow(rng.next(), nat.sizePower) : rng.next();
                    const float sc = escMin + (escMax - escMin) * t;
                    // Altura bilinear no ponto exato.
                    const float fx = std::clamp(px / r.cell, 0.0f, float(r.w - 1.001f)), fz = std::clamp(pz / r.cell, 0.0f, float(r.d - 1.001f));
                    const int ix = int(fx), iz = int(fz);
                    const float u = fx - ix, v = fz - iz;
                    float y = r.h(ix, iz) * (1 - u) * (1 - v) + r.h(ix + 1, iz) * u * (1 - v) + r.h(ix, iz + 1) * (1 - u) * v +
                              r.h(ix + 1, iz + 1) * u * v;
                    float pitch = 0.0f, roll = 0.0f;
                    if (natural) {
                        // ASSENTA NO TALUDE. A peça de pé em chão inclinado é
                        // o sinal mais rápido de "isto foi colocado por um
                        // programa". O eixo de cima acompanha a normal do
                        // terreno (0,6 dela: pedra acomoda, não cola), e por
                        // cima vai um desvio sorteado.
                        const float dhx = (r.h(std::min(gx + 1, r.w - 1), gz) - r.h(std::max(gx - 1, 0), gz)) / (2.0f * r.cell);
                        const float dhz = (r.h(gx, std::min(gz + 1, r.d - 1)) - r.h(gx, std::max(gz - 1, 0))) / (2.0f * r.cell);
                        const float g180 = 180.0f / kPi;
                        roll = std::atan(dhx) * g180 * 0.6f + (rng.next() * 2.0f - 1.0f) * nat.tilt;
                        pitch = -std::atan(dhz) * g180 * 0.6f + (rng.next() * 2.0f - 1.0f) * nat.tilt;
                        y -= nat.sink * sc;   // enterra: rocha solta no chão está meio coberta
                    }
                    g.items.push_back({px, y, pz, rng.next() * 360.0f, sc, pitch, roll});
                }
            r.scatter.push_back(std::move(g));
        } else if (s.type == "river") {
            // Do ponto pedido (ou do mais alto) pela descida mais forte, com curvas
            // de ruído, até a água ou a borda; o leito só desce.
            const float halfW = float(s.get("width", 24)) * 0.5f / r.cell;
            const float depth = float(s.get("depth", 6));
            const float meander = float(s.get("meander", 0.6));
            int x = 0, z = 0;
            if (s.num.count("x") && s.num.count("z")) {
                x = A + std::clamp(int(float(s.get("x", 0.5)) * (n - 1)), 0, n - 1);
                z = A + std::clamp(int(float(s.get("z", 0.5)) * (n - 1)), 0, n - 1);
            } else {
                // O mais alto do miolo (um quinto de margem): rio longo, não um fiapo na borda.
                const int margin = std::max(2, n / 5);
                float top = -FLT_MAX;
                for (int zz = A + margin; zz < r.d - A - margin; ++zz)
                    for (int xx = A + margin; xx < r.w - A - margin; ++xx)
                        if (r.h(xx, zz) > top) { top = r.h(xx, zz); x = xx; z = zz; }
            }
            // Menor subida até a borda ou a água (A* com alvo em qualquer um deles):
            // nunca prende num buraco. O ruído no custo faz as curvas.
            const size_t N = r.height.size();
            // Destino: a água, se o mapa tem lago; senão a borda.
            bool anyWater = false;
            if (r.hasWater)
                for (float hh : r.height)
                    if (hh < r.waterLevel - 1.0f) { anyWater = true; break; }
            auto isGoal = [&](int xx, int zz) {
                if (anyWater) return r.h(xx, zz) < r.waterLevel - 1.0f;
                return xx <= A || zz <= A || xx >= r.w - 1 - A || zz >= r.d - 1 - A;
            };
            auto toBorder = [&](int xx, int zz) {
                return anyWater ? 0.0f : float(std::min({xx - A, zz - A, r.w - 1 - A - xx, r.d - 1 - A - zz}));
            };
            std::vector<float> g(N, FLT_MAX);
            std::vector<int> came(N, -1);
            using QItem = std::pair<float, int>;
            std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> open;
            g[size_t(z) * r.w + x] = 0.0f;
            open.push({toBorder(x, z), z * r.w + x});
            int goal = -1;
            while (!open.empty()) {
                const auto [f, cur] = open.top();
                open.pop();
                const int cx = cur % r.w, cz = cur / r.w;
                if (f - toBorder(cx, cz) > g[size_t(cur)] + 1e-3f) continue;
                if (isGoal(cx, cz)) {
                    goal = cur;
                    break;
                }
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dz) continue;
                        const int nx = cx + dx, nz = cz + dz;
                        if (nx < 0 || nz < 0 || nx >= r.w || nz >= r.d) continue;
                        const float run = dx && dz ? 1.41421356f : 1.0f;
                        const float rise = r.h(nx, nz) - r.h(cx, cz);
                        const float wx = (ox + nx * scaleToWorld) / 140.0f, wz = (oz + nz * scaleToWorld) / 140.0f;
                        const float wiggle = 1.0f + meander * (gradientNoise(wx, wz, seed) + 0.7f);
                        const float cost = g[size_t(cur)] + run * wiggle + std::max(0.0f, rise) * 3.0f;
                        const size_t ni = size_t(nz) * r.w + nx;
                        if (cost >= g[ni]) continue;
                        g[ni] = cost;
                        came[ni] = cur;
                        open.push({cost + toBorder(nx, nz), int(ni)});
                    }
            }
            std::vector<Vec2> path;
            for (int c = goal; c >= 0; c = came[size_t(c)]) path.push_back(Vec2(float(c % r.w), float(c / r.w)));
            std::reverse(path.begin(), path.end());
            // Curvas mais redondas.
            for (int pass = 0; pass < 2; ++pass)
                for (size_t i = 1; i + 1 < path.size(); ++i) path[i] = (path[i - 1] + path[i] * 2.0f + path[i + 1]) * 0.25f;
            if (path.size() < 3) continue;
            std::vector<float> bed(path.size());
            for (size_t i = 0; i < path.size(); ++i) {
                const float hh = r.h(int(std::lround(path[i].x)), int(std::lround(path[i].y))) - depth;
                bed[i] = i ? std::min(hh, bed[i - 1]) : hh;
            }
            if (!carveStrip(r, path, bed, std::max(0.6f, halfW), [&](float t) { return depth * t * t; }, s.text("layer"), error))
                return false;
        } else if (s.type == "road") {
            // Caminho mais plano entre dois pontos (A* pesando a subida), aplainado.
            const float halfW = float(s.get("width", 16)) * 0.5f / r.cell;
            const float climb = float(s.get("avoid_slopes", 4));
            auto cellAt = [&](const char* kx, const char* kz, float dx, float dz) {
                return glm::ivec2(A + std::clamp(int(float(s.get(kx, dx)) * (n - 1)), 0, n - 1),
                                  A + std::clamp(int(float(s.get(kz, dz)) * (n - 1)), 0, n - 1));
            };
            const glm::ivec2 from = cellAt("from_x", "from_z", 0.0f, 0.5f), to = cellAt("to_x", "to_z", 1.0f, 0.5f);
            const size_t N = r.height.size();
            std::vector<float> g(N, FLT_MAX);
            std::vector<int> came(N, -1);
            using QItem = std::pair<float, int>;
            std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> open;
            auto idx = [&](int xx, int zz) { return zz * r.w + xx; };
            auto heur = [&](int xx, int zz) { return glm::length(Vec2(xx - to.x, zz - to.y)); };
            g[idx(from.x, from.y)] = 0.0f;
            open.push({heur(from.x, from.y), idx(from.x, from.y)});
            while (!open.empty()) {
                const auto [f, cur] = open.top();
                open.pop();
                const int cx = cur % r.w, cz = cur / r.w;
                if (cx == to.x && cz == to.y) break;
                if (f - heur(cx, cz) > g[size_t(cur)] + 1e-3f) continue;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dz) continue;
                        const int nx = cx + dx, nz = cz + dz;
                        if (nx < 0 || nz < 0 || nx >= r.w || nz >= r.d) continue;
                        const float run = dx && dz ? 1.41421356f : 1.0f;
                        const float rise = std::abs(r.h(nx, nz) - r.h(cx, cz)) / (run * r.cell);
                        float cost = g[size_t(cur)] + run * (1.0f + climb * rise * rise * 4.0f);
                        if (r.hasWater && r.h(nx, nz) < r.waterLevel) cost += 50.0f; // não atravessa lago à toa
                        const size_t ni = size_t(idx(nx, nz));
                        if (cost >= g[ni]) continue;
                        g[ni] = cost;
                        came[ni] = cur;
                        open.push({cost + heur(nx, nz), int(ni)});
                    }
            }
            std::vector<Vec2> path;
            for (int c = idx(to.x, to.y); c >= 0; c = came[size_t(c)]) {
                path.push_back(Vec2(float(c % r.w), float(c / r.w)));
                if (c == idx(from.x, from.y)) break;
            }
            if (path.size() < 2) continue;
            std::reverse(path.begin(), path.end());
            // Curvas mais suaves e altura da estrada pela média do caminho.
            for (int pass = 0; pass < 3; ++pass)
                for (size_t i = 1; i + 1 < path.size(); ++i) path[i] = (path[i - 1] + path[i] * 2.0f + path[i + 1]) * 0.25f;
            std::vector<float> raw(path.size()), bed(path.size());
            for (size_t i = 0; i < path.size(); ++i) raw[i] = r.h(int(std::lround(path[i].x)), int(std::lround(path[i].y)));
            for (size_t i = 0; i < path.size(); ++i) {
                float sum = 0.0f;
                int cnt = 0;
                for (int k = -4; k <= 4; ++k) {
                    const long j = long(i) + k;
                    if (j < 0 || j >= long(path.size())) continue;
                    sum += raw[size_t(j)];
                    ++cnt;
                }
                bed[i] = sum / float(cnt);
            }
            if (!carveStrip(r, path, bed, std::max(0.6f, halfW), [](float) { return 0.0f; }, s.text("layer"), error)) return false;
        } else if (s.type == "spawn") {
            const int gx = A + std::clamp(int(float(s.get("x", 0.5)) * (n - 1)), 0, n - 1);
            const int gz = A + std::clamp(int(float(s.get("z", 0.5)) * (n - 1)), 0, n - 1);
            r.hasSpawn = true;
            r.spawn[0] = (gx - A) * r.cell;
            r.spawn[1] = r.h(gx, gz);
            r.spawn[2] = (gz - A) * r.cell;
        } else if (s.type == "walk") {
            r.maxWalkSlope = float(s.get("max_slope", 40));
        }
    }
    // Tira a faixa extra.
    {
        auto crop = [&](std::vector<float>& v) {
            std::vector<float> out(size_t(n) * n);
            for (int z = 0; z < n; ++z)
                for (int x = 0; x < n; ++x) out[size_t(z) * n + x] = v[size_t(z + A) * r.w + x + A];
            v.swap(out);
        };
        // A pintura do mapa já viu a faixa; a caminhada (inclinação) é feita na gravação.
        crop(r.height);
        for (auto& l : r.layers) crop(l.weight);
        const float lim = (n - 1) * r.cell;
        for (auto& g : r.scatter) {
            std::vector<ScatterGroup::Item> kept;
            for (auto it : g.items) {
                it.x -= A * r.cell;
                it.z -= A * r.cell;
                if (it.x >= 0 && it.z >= 0 && it.x <= lim && it.z <= lim) kept.push_back(it);
            }
            g.items.swap(kept);
        }
        r.w = r.d = n;
    }
    if (progress) progress(1.0f, "Done");
    return true;
}

// ---------------------------------------------------------------- gravação

namespace {

template <typename T>
void append(std::vector<uint8_t>& out, const std::vector<T>& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(v.data());
    out.insert(out.end(), p, p + v.size() * sizeof(T));
}

bool writeGroundGlb(const Result& r, const fs::path& file, std::string& error) {
    std::vector<float> pos, nrm, uv;
    for (int z = 0; z < r.d; ++z)
        for (int x = 0; x < r.w; ++x) {
            pos.insert(pos.end(), {x * r.cell, r.h(x, z), z * r.cell});
            const int xl = std::max(0, x - 1), xr = std::min(r.w - 1, x + 1), zd = std::max(0, z - 1), zu = std::min(r.d - 1, z + 1);
            const float dx = (r.h(xr, z) - r.h(xl, z)) / ((xr - xl) * r.cell), dz = (r.h(x, zu) - r.h(x, zd)) / ((zu - zd) * r.cell);
            const float inv = 1.0f / std::sqrt(dx * dx + 1 + dz * dz);
            nrm.insert(nrm.end(), {-dx * inv, inv, -dz * inv});
            uv.insert(uv.end(), {x * r.cell / 32.0f, z * r.cell / 32.0f});
        }
    std::vector<uint32_t> idx;
    for (int z = 0; z + 1 < r.d; ++z)
        for (int x = 0; x + 1 < r.w; ++x) {
            const uint32_t a = uint32_t(z * r.w + x), b = a + 1, c = a + uint32_t(r.w), d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    std::vector<uint8_t> bin;
    const size_t oP = bin.size(); append(bin, pos);
    const size_t oN = bin.size(); append(bin, nrm);
    const size_t oU = bin.size(); append(bin, uv);
    const size_t oI = bin.size(); append(bin, idx);
    const auto [lo, hi] = std::minmax_element(r.height.begin(), r.height.end());
    using nlohmann::json;
    const size_t nv = size_t(r.w) * r.d;
    json j;
    j["asset"] = {{"version", "2.0"}, {"generator", "Eruption Map Generator"}};
    j["scene"] = 0;
    j["scenes"] = json::array({{{"nodes", {0}}}});
    j["nodes"] = json::array({{{"name", "Ground"}, {"mesh", 0}}});
    j["meshes"] = json::array({{{"name", "Ground"}, {"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}}, {"indices", 3}, {"material", 0}}})}}});
    j["materials"] = json::array({{{"name", "eruption_ground"}, {"pbrMetallicRoughness", {{"baseColorFactor", {0.30, 0.34, 0.27, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.9}}}}});
    j["buffers"] = json::array({{{"byteLength", bin.size()}}});
    j["bufferViews"] = json::array({
        {{"buffer", 0}, {"byteOffset", oP}, {"byteLength", pos.size() * 4}, {"target", 34962}},
        {{"buffer", 0}, {"byteOffset", oN}, {"byteLength", nrm.size() * 4}, {"target", 34962}},
        {{"buffer", 0}, {"byteOffset", oU}, {"byteLength", uv.size() * 4}, {"target", 34962}},
        {{"buffer", 0}, {"byteOffset", oI}, {"byteLength", idx.size() * 4}, {"target", 34963}},
    });
    j["accessors"] = json::array({
        {{"bufferView", 0}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"},
         {"min", {0.0, *lo, 0.0}}, {"max", {(r.w - 1) * r.cell, *hi, (r.d - 1) * r.cell}}},
        {{"bufferView", 1}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"}},
        {{"bufferView", 2}, {"componentType", 5126}, {"count", nv}, {"type", "VEC2"}},
        {{"bufferView", 3}, {"componentType", 5125}, {"count", idx.size()}, {"type", "SCALAR"}},
    });
    std::string text = j.dump();
    while (text.size() % 4) text.push_back(' ');
    auto u32 = [](std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); };
    std::vector<uint8_t> glb;
    u32(glb, 0x46546C67);
    u32(glb, 2);
    u32(glb, uint32_t(12 + 8 + text.size() + 8 + bin.size()));
    u32(glb, uint32_t(text.size()));
    u32(glb, 0x4E4F534A);
    glb.insert(glb.end(), text.begin(), text.end());
    u32(glb, uint32_t(bin.size()));
    u32(glb, 0x004E4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    std::ofstream f(file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    if (!f) {
        error = "Could not write " + file.string();
        return false;
    }
    return true;
}

} // namespace

bool writeMap(const Result& r, const std::string& projectRoot, const std::string& name, std::string& error) {
    const fs::path root(projectRoot);
    const fs::path dir = root / "assets" / "external" / name;
    const fs::path scenes = root / "assets" / "scenes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::create_directories(scenes, ec);
    if (!writeGroundGlb(r, dir / (name + ".glb"), error)) return false;
    // Grade de altura: um ponto por vértice do chão (o .ter guarda negativo).
    eruption::TerrainFile ter;
    ter.version = 2;
    ter.width = uint32_t(r.w);
    ter.height = uint32_t(r.d);
    ter.scale = r.cell;
    ter.cubes.resize(size_t(r.w) * r.d);
    for (int z = 0; z < r.d; ++z)
        for (int x = 0; x < r.w; ++x) {
            const int x1 = std::min(x + 1, r.w - 1), z1 = std::min(z + 1, r.d - 1);
            ter.cubes[size_t(z) * r.w + x] = {{-r.h(x, z), -r.h(x1, z), -r.h(x, z1), -r.h(x1, z1)}, -1, -1, -1};
        }
    if (!eruption::TerrainParser::writeEruptGndBin(ter, (dir / (name + ".ter")).string())) {
        error = "Could not write the height grid.";
        return false;
    }
    std::ofstream(dir / (name + ".env")) << "{\n \"light\": {\n  \"direction\": [0.47, -0.82, 0.33]\n }\n}\n";
    // Arquivos ao lado da cena: começa do zero (o mapa é novo).
    for (const char* ext : {".scene.luau", ".height.bin", ".splat0.png", ".splat1.png", ".scatter.luau", ".walk.png"})
        fs::remove(scenes / (name + ext), ec);
    // Camadas: duas máscaras RGBA (8 camadas), um texel por ponto da grade.
    if (!r.layers.empty()) {
        for (int m = 0; m < 2; ++m) {
            std::vector<uint8_t> px(size_t(r.w) * r.d * 4, 0);
            for (size_t l = size_t(m) * 4; l < std::min(r.layers.size(), size_t(m) * 4 + 4); ++l)
                for (size_t g = 0; g < r.height.size(); ++g)
                    px[g * 4 + (l % 4)] = uint8_t(std::lround(std::clamp(r.layers[l].weight[g], 0.0f, 1.0f) * 255.0f));
            eruption::ImageUtils::writePNG((scenes / (name + ".splat" + std::to_string(m) + ".png")).string(), r.w, r.d, 4, px.data());
        }
    }
    // Cena: camadas, água e início.
    std::ostringstream sc;
    sc << "-- Edits of the map \"" << name << "\", written by the editor (Ctrl+S).\n"
          "-- Rotation in degrees. Names that no longer exist in the map are ignored.\nreturn {\n";
    if (r.hasSpawn) sc << "    spawn = {" << num(r.spawn[0]) << ", " << num(r.spawn[1]) << ", " << num(r.spawn[2]) << "},\n";
    if (!r.layers.empty()) {
        sc << "    terrain = { layers = {";
        for (size_t l = 0; l < r.layers.size(); ++l) sc << (l ? ", " : " ") << quote(r.layers[l].texture);
        sc << " } },\n";
    }
    if (r.hasWater) sc << "    environment = { time = 12, paused = false, water = " << num(r.waterLevel) << " },\n";
    sc << "}\n";
    std::ofstream(scenes / (name + ".scene.luau")) << sc.str();
    // Objetos espalhados (mesmo formato do pincel de objetos).
    if (!r.scatter.empty()) {
        std::ostringstream o;
        o << "-- Objects painted with the Objects brush of the editor: {x, y, z, rot x, rot y, rot z, scale}.\nreturn {\n";
        for (const auto& g : r.scatter) {
            const bool prefab = g.model.size() > 12 && g.model.compare(g.model.size() - 12, 12, ".prefab.luau") == 0;
            o << "    { " << (prefab ? "prefab = " : "file = ") << quote(g.model) << ", items = {\n";
            for (const auto& it : g.items)
                o << "        {" << num(it.x) << ", " << num(it.y) << ", " << num(it.z) << ", " << num(it.pitch) << ", "
                  << num(it.yaw) << ", " << num(it.roll) << ", " << num(it.scale) << "},\n";
            o << "    } },\n";
        }
        o << "}\n";
        std::ofstream(scenes / (name + ".scatter.luau")) << o.str();
    }
    // Caminhada: bloqueia encosta íngreme e água funda.
    if (r.maxWalkSlope > 0 || r.hasWater) {
        std::vector<uint8_t> px(size_t(r.w) * r.d * 4, 0);
        bool any = false;
        for (int z = 0; z < r.d; ++z)
            for (int x = 0; x < r.w; ++x) {
                const bool blocked = (r.maxWalkSlope > 0 && r.slopeDeg(x, z) > r.maxWalkSlope) ||
                                     (r.hasWater && r.h(x, z) < r.waterLevel - 3.0f);
                const size_t g = size_t(z) * r.w + x;
                px[g * 4] = px[g * 4 + 1] = px[g * 4 + 2] = blocked ? 255 : 0;
                px[g * 4 + 3] = 255;
                any |= blocked;
            }
        if (any) eruption::ImageUtils::writePNG((scenes / (name + ".walk.png")).string(), r.w, r.d, 4, px.data());
    }
    return true;
}

} // namespace mapgen
