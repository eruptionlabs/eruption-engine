#include "formats/MapBundleParser.hpp"
#include "formats/TerrainParser.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

static std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

int main(int argc, char** argv) {
    std::string mapName = (argc > 1) ? argv[1] : "parana_field";

    fs::path bundlePath = fs::path("assets") / "data" / (mapName + ".map");
    fs::path terrainPath = fs::path("assets") / "data" / "terrain" / (mapName + ".ter");

    auto bundleData = readBytes(bundlePath);
    if (bundleData.empty()) {
        std::cerr << "Failed to read " << bundlePath << "\n";
        return 1;
    }

    eruption::MapBundle bundle = eruption::MapBundleParser::parse(bundleData);
    std::cout << "World version: " << (int)bundle.world.versionMajor << "."
              << (int)bundle.world.versionMinor << "\n";
    std::cout << "Models: " << bundle.models.size() << "\n";
    std::cout << "Nav: " << bundle.nav.width << "x" << bundle.nav.height
              << " (" << bundle.nav.cells.size() << " cells)\n";

    if (bundle.world.versionMajor == 0 && bundle.world.versionMinor == 0) {
        std::cerr << "Parse failed\n";
        return 1;
    }

    if (fs::exists(terrainPath)) {
        auto terData = readBytes(terrainPath);
        if (!terData.empty()) {
            auto ter = eruption::TerrainParser::parse(terData);
            std::cout << "Terrain: " << ter.width << "x" << ter.height << "\n";
        }
    }

    std::cout << "OK\n";
    return 0;
}
