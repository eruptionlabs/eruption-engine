#include "formats/NavGridParser.hpp"
#include "core/Logger.hpp"

namespace eruption {

NavGridFile NavGridParser::parse(const uint8_t* data, size_t size) {
    BinaryReader reader(data, size);
    NavGridFile nav;

    if (size < 14) {
        ERUPTION_LOG_WARN("NavGridParser: file too small (%zu bytes)", size);
        return nav;
    }

    // Expected magic: ERUPTNAV
    char magic[8];
    std::memcpy(magic, data, 8);
    if (std::memcmp(magic, "ERUPTNAV", 8) != 0) {
        ERUPTION_LOG_ERROR("NavGridParser: unsupported format (expected ERUPTNAV).");
        return nav;
    }
    reader.seek(8);

    nav.width  = reader.readU32();
    nav.height = reader.readU32();

    if (nav.width == 0 || nav.height == 0 || nav.width > 10000 || nav.height > 10000) {
        ERUPTION_LOG_WARN("NavGridParser: invalid dimensions: %ux%u", nav.width, nav.height);
        nav.width = 0;
        nav.height = 0;
        return nav;
    }

    uint32_t cellCount = nav.width * nav.height;
    if (reader.remaining() < cellCount * 20) {
        ERUPTION_LOG_WARN("NavGridParser: data too small for %u cells", cellCount);
        return nav;
    }

    nav.cells.resize(cellCount);
    for (auto& cell : nav.cells) {
        cell.height[0] = reader.readFloat();
        cell.height[1] = reader.readFloat();
        cell.height[2] = reader.readFloat();
        cell.height[3] = reader.readFloat();
        cell.type = static_cast<TerrainType>(reader.readI32());
    }

    ERUPTION_LOG_INFO("NavGridParser: parsed %dx%d (%u cells)", nav.width, nav.height, cellCount);
    return nav;
}

} // namespace eruption
