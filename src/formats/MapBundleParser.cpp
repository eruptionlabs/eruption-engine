#include "formats/MapBundleParser.hpp"
#include "formats/GltfParser.hpp"
#include "formats/ModelFile.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
#include <cstring>
#include <algorithm>

namespace eruption {

using json = nlohmann::json;

static Vec3 readVec3(const json& j) {
    if (!j.is_array() || j.size() < 3) return Vec3(0.0f);
    return Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

static WorldFile parseWorldJson(const std::string& text) {
    WorldFile w;
    try {
        json root = json::parse(text);
        w.versionMajor = root.value("versionMajor", 0);
        w.versionMinor = root.value("versionMinor", 0);
        w.buildNumber = root.value("buildNumber", 0);

        if (root.contains("water")) {
            const auto& wtr = root["water"];
            w.water.level = wtr.value("level", 0.0f);
            w.water.type = wtr.value("type", 0);
            w.water.waveHeight = wtr.value("waveHeight", 0.2f);
            w.water.waveSpeed = wtr.value("waveSpeed", 2.0f);
            w.water.wavePitch = wtr.value("wavePitch", 50.0f);
            w.water.animSpeed = wtr.value("animSpeed", 3.0f);
        }

        if (root.contains("light")) {
            const auto& l = root["light"];
            w.light.longitude = l.value("longitude", 45);
            w.light.latitude = l.value("latitude", 45);
            w.light.diffuse = readVec3(l.value("diffuse", json::array()));
            w.light.ambient = readVec3(l.value("ambient", json::array()));
            w.light.opacity = l.value("opacity", 1.0f);
        }

        if (root.contains("ground")) {
            const auto& g = root["ground"];
            w.ground.top = g.value("top", -500);
            w.ground.bottom = g.value("bottom", 500);
            w.ground.left = g.value("left", -500);
            w.ground.right = g.value("right", 500);
        }

        if (root.contains("models") && root["models"].is_array()) {
            for (const auto& m : root["models"]) {
                WorldModel wm;
                wm.name = m.value("name", "");
                wm.modelFile = m.value("modelFile", "");
                wm.position = readVec3(m.value("position", json::array()));
                wm.rotation = readVec3(m.value("rotation", json::array()));
                wm.scale = readVec3(m.value("scale", json::array()));
                w.models.push_back(std::move(wm));
            }
        }

        if (root.contains("lights") && root["lights"].is_array()) {
            for (const auto& l : root["lights"]) {
                WorldLight wl;
                wl.name = l.value("name", "");
                wl.position = readVec3(l.value("position", json::array()));
                wl.color = readVec3(l.value("color", json::array()));
                wl.range = l.value("range", 0.0f);
                w.lights.push_back(std::move(wl));
            }
        }

        if (root.contains("sounds") && root["sounds"].is_array()) {
            for (const auto& s : root["sounds"]) {
                WorldSound ws;
                ws.name = s.value("name", "");
                ws.waveFile = s.value("waveFile", "");
                ws.position = readVec3(s.value("position", json::array()));
                ws.volume = s.value("volume", 0.0f);
                ws.width = s.value("width", 0);
                ws.height = s.value("height", 0);
                ws.range = s.value("range", 0.0f);
                ws.cycle = s.value("cycle", 0.0f);
                w.sounds.push_back(std::move(ws));
            }
        }
    } catch (const std::exception& e) {
        ERUPTION_LOG_ERROR("MapBundleParser: failed to parse world JSON: %s", e.what());
    }
    return w;
}

static NavGridFile parseNavBinary(const uint8_t* data, size_t size) {
    NavGridFile nav;
    if (size < 20) return nav;

    auto readU32 = [&](size_t offset) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, data + offset, 4);
        return v;
    };

    uint32_t magic = readU32(0);
    if (magic != *reinterpret_cast<const uint32_t*>("ERUP")) {
        ERUPTION_LOG_WARN("MapBundleParser: nav magic mismatch");
        return nav;
    }

    (void)readU32(4); // version
    nav.width = readU32(8);
    nav.height = readU32(12);
    uint32_t cellCount = readU32(16);

    size_t expected = 20 + cellCount * (4 * 4 + 4);
    if (size < expected) {
        ERUPTION_LOG_WARN("MapBundleParser: nav truncated (expected %zu, got %zu)", expected, size);
        return nav;
    }

    nav.cells.resize(cellCount);
    size_t off = 20;
    for (uint32_t i = 0; i < cellCount; ++i) {
        NavCell c;
        for (int j = 0; j < 4; ++j) {
            std::memcpy(&c.height[j], data + off, 4);
            off += 4;
        }
        int32_t type;
        std::memcpy(&type, data + off, 4);
        off += 4;
        c.type = static_cast<TerrainType>(type);
        nav.cells[i] = c;
    }
    return nav;
}

MapBundle MapBundleParser::parse(const uint8_t* data, size_t size) {
    MapBundle bundle;
    if (size < 28) {
        ERUPTION_LOG_ERROR("MapBundleParser: file too small");
        return bundle;
    }

    auto readU32 = [&](size_t offset) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, data + offset, 4);
        return v;
    };

    if (std::memcmp(data, "ERUPTMAP", 8) != 0) {
        ERUPTION_LOG_ERROR("MapBundleParser: invalid magic");
        return bundle;
    }

    (void)readU32(8); // version
    uint32_t worldSize = readU32(12);
    uint32_t navSize = readU32(16);
    uint32_t modelCount = readU32(20);
    uint32_t modelsSize = readU32(24);

    size_t headerSize = 28;
    size_t expected = headerSize + worldSize + navSize + modelsSize;
    if (size < expected) {
        ERUPTION_LOG_ERROR("MapBundleParser: truncated (expected %zu, got %zu)", expected, size);
        return bundle;
    }

    size_t off = headerSize;
    std::string worldJson(reinterpret_cast<const char*>(data + off), worldSize);
    off += worldSize;

    bundle.world = parseWorldJson(worldJson);

    if (navSize > 0) {
        bundle.nav = parseNavBinary(data + off, navSize);
        off += navSize;
    }

    for (uint32_t i = 0; i < modelCount; ++i) {
        if (off + 8 > size) break;
        uint32_t pathLen = readU32(off);
        off += 4;
        if (off + pathLen > size) break;
        std::string modelPath(reinterpret_cast<const char*>(data + off), pathLen);
        off += pathLen;

        if (off + 4 > size) break;
        uint32_t dataLen = readU32(off);
        off += 4;
        if (off + dataLen > size) break;

        // Only GLB models are supported inside map bundles.
        if (dataLen < 4 || std::memcmp(data + off, "glTF", 4) != 0) {
            ERUPTION_LOG_WARN("MapBundleParser: skipping non-GLB model '%s'", modelPath.c_str());
            off += dataLen;
            continue;
        }
        ModelFile m = GltfParser::parse(data + off, dataLen);
        if (!m.nodes.empty()) {
            m.filePath = modelPath;
            bundle.models.push_back(std::move(m));
        }
        off += dataLen;
    }

    return bundle;
}

} // namespace eruption
