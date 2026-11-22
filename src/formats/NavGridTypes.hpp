#pragma once

#include "math/Types.hpp"
#include <vector>
#include <string>

namespace eruption {

enum class TerrainType : int32_t {
    Walkable = 0,
    NonWalkable = 1,
    Water = 2,
    Unknown3 = 3,
    Snipable = 4,
    Unknown5 = 5,
    WalkableWater = 6,
};

struct NavCell {
    float height[4];
    TerrainType type;

    bool isWater() const { return type == TerrainType::Water || type == TerrainType::WalkableWater; }
    bool isWalkable() const { return type == TerrainType::Walkable || type == TerrainType::WalkableWater; }
    float avgHeight() const { return (height[0] + height[1] + height[2] + height[3]) * 0.25f; }
    float worldY() const { return -avgHeight() * 1.0f; }
};

// Alturas de canto da grade de navegacao. Arquivo .nav de versao >= 2 traz,
// por celula, as 4 alturas de canto reais do caminho andavel (escada, ponte,
// rampa); a versao 1 so tem o valor do terreno repetido nos 4 cantos e nao
// conta como altura (cornerHeights = false). Mesma convencao do .ter: o valor
// guardado e' o negativo da altura de mundo (Y = -height), em unidades de
// mundo; canto 0 = (x, z), 1 = (x+1, z), 2 = (x, z+1), 3 = (x+1, z+1).
constexpr uint32_t kNavFileVersionCornerHeights = 2;

struct NavGridFile {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<NavCell> cells;

    bool cornerHeights = false; // true: height[] das celulas sao alturas reais (.nav v2)

    const NavCell& at(uint32_t x, uint32_t z) const {
        return cells[z * width + x];
    }

    // Altura de mundo (Y) pela interpolacao bilinear dos 4 cantos da celula em
    // (u, v), coordenadas da grade em celulas (a celula gx cobre [gx, gx+1)).
    // A borda final (u == width) pertence a ultima celula. Falso (y intocado)
    // sem alturas de canto ou fora da grade.
    bool cornerHeightAt(float u, float v, float& y) const {
        if (!cornerHeights || width == 0 || height == 0 || cells.size() < size_t(width) * height) return false;
        if (!(u >= 0.0f && v >= 0.0f && u <= float(width) && v <= float(height))) return false; // tambem NaN
        const uint32_t gx = u >= float(width) ? width - 1 : uint32_t(u);
        const uint32_t gz = v >= float(height) ? height - 1 : uint32_t(v);
        const float fx = u - float(gx), fz = v - float(gz);
        const NavCell& c = at(gx, gz);
        const float h = c.height[0] * (1.0f - fx) * (1.0f - fz) + c.height[1] * fx * (1.0f - fz) +
                        c.height[2] * (1.0f - fx) * fz + c.height[3] * fx * fz;
        y = -h;
        return true;
    }
};

} // namespace eruption
