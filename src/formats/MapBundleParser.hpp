#pragma once

#include "formats/MapInfo.hpp"
#include "formats/NavGridParser.hpp"
#include "formats/ModelFile.hpp"
#include <vector>
#include <string>
#include <cstdint>

namespace eruption {

// Loads a unified .map bundle (world JSON + nav binary + embedded model sources).
// The terrain (.ter) is kept in a separate file and loaded through TerrainParser.
struct MapBundle {
    WorldFile world;
    NavGridFile nav;
    std::vector<ModelFile> models;
};

class MapBundleParser {
public:
    static MapBundle parse(const uint8_t* data, size_t size);
    static MapBundle parse(const std::vector<uint8_t>& data) {
        return parse(data.data(), data.size());
    }
};

} // namespace eruption
