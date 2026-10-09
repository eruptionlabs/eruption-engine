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
#include <sstream>

namespace mapgen {

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

// Pontos (fração 0..1) que valem para as regras de inclinação/altura de uma etapa.
float ruleWeight(const Result& r, int x, int z, const Step& s) {
    const float soft = float(s.get("softness", 3.0));
    float w = smoothRange(r.slopeDeg(x, z), float(s.get("slope_min", -1e9)), float(s.get("slope_max", 1e9)), soft);
    w *= smoothRange(r.h(x, z), float(s.get("height_min", -1e9)), float(s.get("height_max", 1e9)), soft);
    return w;
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
          {"coverage", 1, 0, 1, "Patchiness (1 = everywhere the rules match)"}},
         {"layer"}},
        {"scatter", "Scatter objects", "Places a model (.glb or prefab) where the rules match.",
         {{"density", 1, 0.01, 200, "Objects per 100 x 100 units"},
          {"slope_min", -1e9, 0, 90, "From this slope"},
          {"slope_max", 1e9, 0, 90, "Up to this slope"},
          {"height_min", -1e9, -1000, 3000, "From this height"},
          {"height_max", 1e9, -1000, 3000, "Up to this height"},
          {"scale_min", 0.8, 0.05, 20, "Smallest size"},
          {"scale_max", 1.2, 0.05, 20, "Largest size"}},
         {"model"}},
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
            Layer* layer = nullptr;
            const std::string tex = s.text("layer");
            if (tex.empty()) continue;
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
                    const size_t g = size_t(z) * r.w + x;
                    // A camada nova cobre as anteriores na mesma medida.
                    for (auto& other : r.layers)
                        if (&other != layer) other.weight[g] *= 1.0f - m;
                    layer->weight[g] = std::max(layer->weight[g], m);
                }
        } else if (s.type == "scatter") {
            const std::string model = s.text("model");
            if (model.empty()) continue;
            ScatterGroup g;
            g.model = model;
            const float density = float(s.get("density", 1));
            const float spacing = 100.0f / std::sqrt(std::max(density, 1e-3f));
            Rng rng{uint64_t(seed) * 977u + 5};
            const float worldSize = (r.w - 1) * r.cell;
            // Grade com sorteio dentro de cada célula: distância mínima natural.
            for (float cz = 0; cz < worldSize; cz += spacing)
                for (float cx = 0; cx < worldSize; cx += spacing) {
                    const float px = cx + rng.next() * spacing, pz = cz + rng.next() * spacing;
                    if (px > worldSize || pz > worldSize) continue; // sorteio passou da borda
                    const int gx = std::clamp(int(px / r.cell + 0.5f), 0, r.w - 1), gz = std::clamp(int(pz / r.cell + 0.5f), 0, r.d - 1);
                    const float keep = ruleWeight(r, gx, gz, s);
                    if (keep <= 0 || rng.next() > keep) {
                        rng.next();
                        rng.next();
                        continue;
                    }
                    if (r.hasWater && r.h(gx, gz) < r.waterLevel) continue;
                    const float sc = float(s.get("scale_min", 0.8)) + (float(s.get("scale_max", 1.2)) - float(s.get("scale_min", 0.8))) * rng.next();
                    // Altura bilinear no ponto exato.
                    const float fx = std::clamp(px / r.cell, 0.0f, float(r.w - 1.001f)), fz = std::clamp(pz / r.cell, 0.0f, float(r.d - 1.001f));
                    const int ix = int(fx), iz = int(fz);
                    const float u = fx - ix, v = fz - iz;
                    const float y = r.h(ix, iz) * (1 - u) * (1 - v) + r.h(ix + 1, iz) * u * (1 - v) + r.h(ix, iz + 1) * (1 - u) * v +
                                    r.h(ix + 1, iz + 1) * u * v;
                    g.items.push_back({px, y, pz, rng.next() * 360.0f, sc});
                }
            r.scatter.push_back(std::move(g));
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
                o << "        {" << num(it.x) << ", " << num(it.y) << ", " << num(it.z) << ", 0, " << num(it.yaw) << ", 0, "
                  << num(it.scale) << "},\n";
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
