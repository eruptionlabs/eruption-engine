// Grade de caminhada: onde o jogador pode andar. Em mapa com grade de altura
// (.ter) a grade de caminhada usa as mesmas células (passo e origem do .ter);
// o editor pinta células bloqueadas e o resultado fica em
// assets/scenes/<mapa>.walk.png (branco = bloqueado).
#include "core/Engine.hpp"

#include "core/Logger.hpp"
#include "formats/MapLoader.hpp"
#include "scene/SceneEdits.hpp"
#include "utils/ImageUtils.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace eruption {

namespace {

namespace fs = std::filesystem;

fs::path walkPathFor(const std::string& map) {
    fs::path p = SceneEdits::pathFor(map);
    p.replace_extension();
    p.replace_extension(".walk.png");
    return p;
}

bool followsTerrain(const LoadedMap& m) {
    return m.isGlbMap && m.terrainFromFile && m.navGrid.width == m.terrain.width && m.navGrid.height == m.terrain.height;
}

} // namespace

bool Engine::navFollowsTerrain() const { return m_currentMap && followsTerrain(*m_currentMap); }

bool Engine::navCell(float x, float z, int& gx, int& gz) const {
    if (!m_currentMap) return false;
    const NavGridFile& nav = m_currentMap->navGrid;
    if (followsTerrain(*m_currentMap)) {
        const TerrainFile& t = m_currentMap->terrain;
        gx = int(std::floor((x - t.offsetX) / t.scale + 0.5f));
        gz = int(std::floor((z - t.offsetZ) / t.scale + 0.5f));
    } else {
        gx = int(x / 5.0f); // grade antiga: célula de 5 unidades a partir da origem
        gz = int(z / 5.0f);
    }
    return gx >= 0 && gz >= 0 && gx < int(nav.width) && gz < int(nav.height);
}

bool Engine::navWalkable(float x, float z) const {
    int gx = 0, gz = 0;
    return navCell(x, z, gx, gz) && m_currentMap->navGrid.at(uint32_t(gx), uint32_t(gz)).isWalkable();
}

void Engine::applySavedWalk(LoadedMap& map) {
    if (!followsTerrain(map)) return;
    std::ifstream f(walkPathFor(map.mapName), std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ImageData img = ImageUtils::loadFromMemoryRaw(bytes.data(), bytes.size());
    if (!img.isValid() || uint32_t(img.width) != map.navGrid.width || uint32_t(img.height) != map.navGrid.height) {
        ERUPTION_LOG_WARN("Walk grid for %s doesn't match the map; ignored", map.mapName.c_str());
        return;
    }
    size_t blocked = 0;
    for (size_t i = 0; i < map.navGrid.cells.size(); ++i) {
        const bool b = img.pixels[i * 4] > 127;
        map.navGrid.cells[i].type = b ? TerrainType::NonWalkable : TerrainType::Walkable;
        blocked += b;
    }
    ERUPTION_LOG_WARN("Walk grid: %zu blocked cells (%s)", blocked, walkPathFor(map.mapName).string().c_str());
}

bool Engine::paintWalk(const Vec2& center, float radius, bool block, int rect[4]) {
    if (!navFollowsTerrain()) return false;
    const TerrainFile& t = m_currentMap->terrain;
    NavGridFile& nav = m_currentMap->navGrid;
    const int x0 = std::max(0, int(std::floor((center.x - radius - t.offsetX) / t.scale)));
    const int z0 = std::max(0, int(std::floor((center.y - radius - t.offsetZ) / t.scale)));
    const int x1 = std::min(int(nav.width) - 1, int(std::ceil((center.x + radius - t.offsetX) / t.scale)));
    const int z1 = std::min(int(nav.height) - 1, int(std::ceil((center.y + radius - t.offsetZ) / t.scale)));
    if (x0 > x1 || z0 > z1) return false;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const Vec2 p(t.offsetX + x * t.scale, t.offsetZ + z * t.scale);
            if (glm::length(p - center) > radius) continue;
            nav.cells[size_t(z) * nav.width + x].type = block ? TerrainType::NonWalkable : TerrainType::Walkable;
        }
    rect[0] = x0;
    rect[1] = z0;
    rect[2] = x1;
    rect[3] = z1;
    return true;
}

std::vector<uint8_t> Engine::walkRegion(int x0, int z0, int x1, int z1) const {
    std::vector<uint8_t> out;
    const NavGridFile& nav = m_currentMap->navGrid;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) out.push_back(nav.at(uint32_t(x), uint32_t(z)).isWalkable() ? 0 : 1);
    return out;
}

void Engine::setWalkRegion(int x0, int z0, int x1, int z1, const std::vector<uint8_t>& blocked) {
    if (!m_currentMap) return;
    NavGridFile& nav = m_currentMap->navGrid;
    size_t k = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            nav.cells[size_t(z) * nav.width + x].type = blocked[k++] ? TerrainType::NonWalkable : TerrainType::Walkable;
}

int Engine::blockSteepSlopes(float maxDegrees) {
    if (!navFollowsTerrain()) return 0;
    const TerrainFile& t = m_currentMap->terrain;
    NavGridFile& nav = m_currentMap->navGrid;
    const float limit = std::tan(glm::radians(std::clamp(maxDegrees, 1.0f, 89.0f)));
    int n = 0;
    for (uint32_t z = 0; z < nav.height; ++z)
        for (uint32_t x = 0; x < nav.width; ++x) {
            const float px = t.offsetX + x * t.scale, pz = t.offsetZ + z * t.scale;
            const float dx = (sampleTerrainHeight(px + t.scale, pz) - sampleTerrainHeight(px - t.scale, pz)) / (2 * t.scale);
            const float dz = (sampleTerrainHeight(px, pz + t.scale) - sampleTerrainHeight(px, pz - t.scale)) / (2 * t.scale);
            if (std::sqrt(dx * dx + dz * dz) > limit) {
                nav.cells[size_t(z) * nav.width + x].type = TerrainType::NonWalkable;
                ++n;
            }
        }
    return n;
}

bool Engine::saveWalk(std::string& error) const {
    if (!navFollowsTerrain()) return true;
    const NavGridFile& nav = m_currentMap->navGrid;
    const fs::path path = walkPathFor(m_currentMapName);
    std::vector<uint8_t> px(size_t(nav.width) * nav.height * 4, 0);
    bool any = false;
    for (size_t i = 0; i < nav.cells.size(); ++i) {
        const uint8_t v = nav.cells[i].isWalkable() ? 0 : 255;
        any |= v != 0;
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
        px[i * 4 + 3] = 255;
    }
    std::error_code ec;
    if (!any) {
        fs::remove(path, ec);
        return true;
    }
    fs::create_directories(path.parent_path(), ec);
    if (!ImageUtils::writePNG(path.string(), int(nav.width), int(nav.height), 4, px.data())) {
        error = "Could not write " + path.generic_string();
        return false;
    }
    return true;
}

void Engine::setWaterLevel(bool enabled, float level) {
    if (!m_currentMap) return;
    m_vulkan.waitIdle();
    if (!enabled) {
        m_water.clearWaterMesh();
        m_waterOn = false;
        return;
    }
    auto mesh = TerrainParser::generateWaterMesh(m_currentMap->terrain, 0, 0, m_currentMap->terrain.width,
                                                 m_currentMap->terrain.height, level, 0.0f, true, false);
    m_water.setWaterMesh(mesh);
    cacheWaterBounds(mesh);
    m_mapBaseWaterLevel = level;
    m_waterOn = !mesh.vertices.empty();
}

} // namespace eruption
