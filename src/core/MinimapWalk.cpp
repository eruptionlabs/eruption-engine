#include "core/MinimapWalk.hpp"

#include <nlohmann/json.hpp>
#include <fstream>

namespace eruption {

bool parseHexColor(const std::string& text, uint8_t out[3]) {
    size_t i = (!text.empty() && text[0] == '#') ? 1 : 0;
    if (text.size() - i != 6) return false;
    uint8_t v[3];
    for (int c = 0; c < 3; ++c) {
        int byte = 0;
        for (int k = 0; k < 2; ++k) {
            const char ch = text[i + size_t(c) * 2 + size_t(k)];
            int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else return false;
            byte = byte * 16 + d;
        }
        v[c] = uint8_t(byte);
    }
    for (int c = 0; c < 3; ++c) out[c] = v[c];
    return true;
}

MinimapWalkConfig MinimapWalkConfig::load(const std::string& path) {
    MinimapWalkConfig cfg;
    std::ifstream f(path);
    if (!f) return cfg;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return cfg;
    auto color = [&](const char* key, uint8_t out[3]) {
        auto it = j.find(key);
        if (it != j.end() && it->is_string()) parseHexColor(it->get<std::string>(), out);
    };
    auto en = j.find("minimap_walk_enabled");
    if (en != j.end() && en->is_boolean()) cfg.enabled = en->get<bool>();
    color("minimap_walk_color", cfg.walk);
    color("minimap_block_color", cfg.block);
    auto st = j.find("minimap_walk_style");
    if (st != j.end() && st->is_string()) cfg.edge = (st->get<std::string>() == "edge");
    return cfg;
}

void bakeWalkMinimap(const MinimapWalkConfig& cfg, uint32_t res,
                     const std::function<int(float, float)>& probe, std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> kind(size_t(res) * res, 0);
    for (uint32_t y = 0; y < res; ++y)
        for (uint32_t x = 0; x < res; ++x)
            kind[size_t(y) * res + x] = uint8_t(probe((float(x) + 0.5f) / float(res), (float(y) + 0.5f) / float(res)));
    rgba.assign(size_t(res) * res * 4, 0);
    for (uint32_t y = 0; y < res; ++y)
        for (uint32_t x = 0; x < res; ++x) {
            const uint8_t k = kind[size_t(y) * res + x];
            if (k == 0) continue;
            uint8_t* p = &rgba[(size_t(y) * res + x) * 4];
            const uint8_t* c = (k == 1) ? cfg.walk : cfg.block;
            bool dark = false;
            if (cfg.edge && k == 2) {
                static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int n = 0; n < 4; ++n) {
                    const int nx = int(x) + dx[n], ny = int(y) + dy[n];
                    if (nx >= 0 && ny >= 0 && nx < int(res) && ny < int(res) && kind[size_t(ny) * res + size_t(nx)] == 1) dark = true;
                }
            }
            for (int ch = 0; ch < 3; ++ch) p[ch] = dark ? uint8_t(c[ch] * 6 / 10) : c[ch];
            p[3] = 255;
        }
}

} // namespace eruption
