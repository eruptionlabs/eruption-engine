#pragma once
#include <vector>
#include <cstdint>
#include "math/Types.hpp"
#include <algorithm>

namespace eruption {

struct Palette {
    uint8_t colors[256 * 4]; // RGBA
};

class PaletteManager {
public:
    static std::vector<int> getSortedIndices(const Palette& pal) {
        std::vector<int> indices(256);
        for(int i=0; i<256; ++i) indices[i] = i;
        
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            float ha = getHue(pal.colors[a*4], pal.colors[a*4+1], pal.colors[a*4+2]);
            float hb = getHue(pal.colors[b*4], pal.colors[b*4+1], pal.colors[b*4+2]);
            if (std::abs(ha - hb) > 0.01f) return ha < hb;
            return pal.colors[a*4+3] < pal.colors[b*4+3];
        });
        return indices;
    }

private:
    static float getHue(uint8_t r, uint8_t g, uint8_t b) {
        float fr = r/255.f, fg = g/255.f, fb = b/255.f;
        float maxC = std::max({fr, fg, fb}), minC = std::min({fr, fg, fb});
        float delta = maxC - minC;
        if (delta == 0) return 0;
        if (maxC == fr) return 60 * std::fmod(((fg - fb) / delta), 6.f);
        if (maxC == fg) return 60 * (((fb - fr) / delta) + 2);
        return 60 * (((fr - fg) / delta) + 4);
    }
};

}
