#include "utils/PbrSynthStrength.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace eruption {

namespace {

std::atomic<float> g_globalStrength{1.0f};

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Cores distintas, sem o preto de fundo (r+g+b < 24: atlas de recortes, janela
// pintada). Bitset de 2^24 bits (2 MB), uma passada.
int countColors(const uint8_t* px, int w, int h, int ch) {
    std::vector<uint64_t> seen(size_t(1) << 18, 0);
    int n = 0;
    const size_t total = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < total; ++i) {
        const uint8_t* p = px + i * ch;
        if (int(p[0]) + int(p[1]) + int(p[2]) < 24) continue;
        const uint32_t key = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        uint64_t& word = seen[key >> 6];
        const uint64_t bit = uint64_t(1) << (key & 63);
        if (!(word & bit)) { word |= bit; ++n; }
    }
    return n;
}

// Constantes da curva (medicao em PbrSynthStrength.hpp).
constexpr float kPaletteLo = 400.0f, kPaletteHi = 1500.0f; // cores: pintada <= 254, foto >= 2086
constexpr float kPaintedFloor = 0.10f;   // pintada pequena nao zera: sobra um grao
constexpr float kPaintedLo = 256.0f, kPaintedHi = 1024.0f; // lado: pintada ampliada ate' aqui
constexpr float kTinyLo = 32.0f, kTinyHi = 96.0f;

} // namespace

float pbrAutoStrength(const uint8_t* pixels, int width, int height, int channels, PbrSynthSignals* signals) {
    PbrSynthSignals s;
    s.side = std::min(width, height);
    if (!pixels || width <= 0 || height <= 0 || channels < 3) {
        s.strength = 1.0f;
        if (signals) *signals = s;
        return 1.0f;
    }
    s.colors = countColors(pixels, width, height, channels);
    s.photo = smoothstep(kPaletteLo, kPaletteHi, static_cast<float>(s.colors));
    const float painted = kPaintedFloor + (1.0f - kPaintedFloor) * smoothstep(kPaintedLo, kPaintedHi, static_cast<float>(s.side));
    s.resolution = smoothstep(kTinyLo, kTinyHi, static_cast<float>(s.side));
    s.strength = s.resolution * (painted + (1.0f - painted) * s.photo);
    if (signals) *signals = s;
    return s.strength;
}

void setPbrSynthesisStrength(float strength) { g_globalStrength = std::clamp(strength, 0.0f, 1.0f); }
float pbrSynthesisStrength() { return g_globalStrength; }

float PbrSidecar::forMaterial(const std::string& name) const {
    for (const auto& kv : materials)
        if (kv.first == name) return kv.second;
    return strength;
}

PbrSidecar parsePbrSidecar(const std::string& jsonText) {
    PbrSidecar out;
    const nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, false);
    if (!j.is_object()) return out;
    out.present = true;
    if (j.contains("strength") && j["strength"].is_number())
        out.strength = std::clamp(j["strength"].get<float>(), 0.0f, 1.0f);
    if (j.contains("materials") && j["materials"].is_object()) {
        for (auto it = j["materials"].begin(); it != j["materials"].end(); ++it)
            if (it.value().is_number())
                out.materials.emplace_back(it.key(), std::clamp(it.value().get<float>(), 0.0f, 1.0f));
    }
    // FNV-1a do que vale (valores, nao os espacos do arquivo), na ordem do JSON.
    uint32_t h = 2166136261u;
    auto mix = [&](const std::string& t) { for (unsigned char c : t) h = (h ^ c) * 16777619u; };
    mix("s" + std::to_string(int(std::lround(out.strength * 1000.0f))));
    for (const auto& kv : out.materials) mix(kv.first + "=" + std::to_string(int(std::lround(kv.second * 1000.0f))));
    out.hash = h ? h : 1u;
    return out;
}

} // namespace eruption
