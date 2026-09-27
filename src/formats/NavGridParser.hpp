#pragma once

#include "utils/BinaryReader.hpp"
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

struct NavGridFile {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<NavCell> cells;

    const NavCell& at(uint32_t x, uint32_t z) const {
        return cells[z * width + x];
    }
};

class NavGridParser {
public:
    static NavGridFile parse(const uint8_t* data, size_t size);
    static NavGridFile parse(const std::vector<uint8_t>& data) {
        return parse(data.data(), data.size());
    }
};

} // namespace eruption
