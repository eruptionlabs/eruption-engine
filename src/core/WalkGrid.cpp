// Grade de caminhada: onde o jogador pode andar. Em mapa com grade de altura
// (.ter) a grade de caminhada usa as mesmas células (passo e origem do .ter);
// o editor pinta células bloqueadas e o resultado fica em
// assets/scenes/<mapa>.walk.png (branco = bloqueado).
#include "core/Engine.hpp"

#include "core/Logger.hpp"
#include "core/JobSystem.hpp"
#include "formats/TerrainParser.hpp"
#include "formats/MapLoader.hpp"
#include "scene/SceneEdits.hpp"
#include "utils/ImageUtils.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

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

// Ponto do mundo no espaço do mapa da grade antiga (célula de 5 u a partir da
// origem do mapa). Vizinho do mundo contínuo: o lugar dele (placeX/placeZ) e o
// giro; o mapa aberto tem lugar 0 e não gira. No vizinho girado o ponto anda
// um nada para +x/+z do mundo antes de girar: em cima da borda de uma célula
// fica a célula do lado + do mundo, como no mapa sem giro. Sem isso, girado
// 180°, a linha da emenda (z inteiro, onde a busca de caminho amostra) caía na
// célula do outro lado, a parede da borda do mapa (26/10).
Vec2 legacyLocal(const TerrainFile& t, float x, float z) {
    constexpr float kTie = 1e-3f;
    return t.rotated() ? t.toMap(x + kTie, z + kTie) : Vec2(x - t.placeX, z - t.placeZ);
}

// Célula da grade de caminhada sob (x, z) e o passo dela.
bool cellOf(const LoadedMap& m, float x, float z, int& gx, int& gz) {
    if (followsTerrain(m)) {
        const TerrainFile& t = m.terrain;
        const Vec2 p = t.toMap(x, z); // vizinho girado: a grade é a do mapa
        gx = int(std::floor((p.x - t.offsetX) / t.scale + 0.5f));
        gz = int(std::floor((p.y - t.offsetZ) / t.scale + 0.5f));
    } else {
        const Vec2 p = legacyLocal(m.terrain, x, z);
        gx = int(std::floor(p.x / 5.0f)); // grade antiga: célula de 5 unidades a partir da origem
        gz = int(std::floor(p.y / 5.0f));
    }
    return gx >= 0 && gz >= 0 && gx < int(m.navGrid.width) && gz < int(m.navGrid.height);
}

// Centro da célula em mundo (o inverso de cellOf).
Vec2 cellCenter(const LoadedMap& m, int gx, int gz) {
    if (followsTerrain(m)) {
        const TerrainFile& t = m.terrain;
        return t.toWorld(t.offsetX + gx * t.scale, t.offsetZ + gz * t.scale);
    }
    const TerrainFile& t = m.terrain;
    const Vec2 p((gx + 0.5f) * 5.0f, (gz + 0.5f) * 5.0f);
    return t.rotated() ? t.toWorld(p.x, p.y) : p + Vec2(t.placeX, t.placeZ);
}

float cellCost(const LoadedMap& m, int gx, int gz) {
    return m.walkCost.empty() ? 1.0f : m.walkCost[size_t(gz) * m.navGrid.width + size_t(gx)];
}

bool cellWalkable(const LoadedMap& m, int gx, int gz) {
    return m.navGrid.at(uint32_t(gx), uint32_t(gz)).isWalkable() && std::isfinite(cellCost(m, gx, gz));
}

} // namespace

bool Engine::navFollowsTerrain() const { return m_currentMap && followsTerrain(*m_currentMap); }

bool Engine::navCell(float x, float z, int& gx, int& gz) const {
    return m_currentMap && cellOf(*m_currentMap, x, z, gx, gz);
}

bool Engine::navWalkable(float x, float z, float y) const {
    const LoadedMap* map = mapAt(x, z, y);
    int gx = 0, gz = 0;
    if (map && map != m_currentMap.get()) {
        // Vizinho do mundo contínuo: a grade dele, a do .ter ou a antiga de 5 u,
        // no lugar do vizinho. Até 26/10 a antiga valia "anda em tudo", e o jogo
        // mantinha uma grade própria para não atravessar paredes nos vizinhos.
        if (map->navGrid.width == 0) return true;
        if (cellOf(*map, x, z, gx, gz) && cellWalkable(*map, gx, gz)) return true;
        return seamWalkable(map, x, z);
    }
    if (navCell(x, z, gx, gz) && cellWalkable(*m_currentMap, gx, gz)) return true;
    return map && seamWalkable(map, x, z);
}

// Emenda do mundo contínuo: os mapas vizinhos se sobrepõem por uma célula, e a
// célula de sobra de cada um é a parede da borda dele. Outro mapa em cena com o
// mesmo chão (até um degrau) no ponto também deixa passar.
bool Engine::seamWalkable(const LoadedMap* map, float x, float z) const {
    constexpr float kSeamStep = 4.0f; // o degrau da caminhada (GroundQuery)
    const float h = TerrainParser::getTerrainHeightAt(map->terrain, x, z);
    const LoadedMap* others[4];
    const int n = mapsAt(x, z, others, 4);
    for (int k = 0; k < n; ++k) {
        const LoadedMap& o = *others[k];
        if (&o == map || o.navGrid.width == 0) continue;
        if (std::abs(TerrainParser::getTerrainHeightAt(o.terrain, x, z) - h) > kSeamStep) continue;
        int gx = 0, gz = 0;
        if (cellOf(o, x, z, gx, gz) && cellWalkable(o, gx, gz)) return true;
    }
    return false;
}

int Engine::setWalkCost(float x, float z, float radius, float cost) {
    // O mapa sob o ponto (o atual ou um vizinho em cena); mapAt devolve const
    // só para as consultas.
    auto* map = const_cast<LoadedMap*>(mapAt(x, z));
    int cx = 0, cz = 0;
    if (!map || !cellOf(*map, x, z, cx, cz)) return 0;
    cost = std::max(cost, 1.0f);
    NavGridFile& nav = map->navGrid;
    if (map->walkCost.empty()) {
        if (cost == 1.0f) return 0;
        map->walkCost.assign(nav.cells.size(), 1.0f);
    }
    const float step = followsTerrain(*map) ? map->terrain.scale : 5.0f;
    const int r = radius > 0.0f ? int(std::ceil(radius / step)) : 0;
    int changed = 0;
    for (int gz = std::max(0, cz - r); gz <= std::min(int(nav.height) - 1, cz + r); ++gz)
        for (int gx = std::max(0, cx - r); gx <= std::min(int(nav.width) - 1, cx + r); ++gx) {
            if (r > 0 && glm::length(cellCenter(*map, gx, gz) - Vec2(x, z)) > radius) continue;
            float& c = map->walkCost[size_t(gz) * nav.width + size_t(gx)];
            changed += c != cost;
            c = cost;
        }
    return changed;
}

float Engine::walkCost(float x, float z) const {
    const LoadedMap* map = mapAt(x, z);
    int gx = 0, gz = 0;
    if (!map || !cellOf(*map, x, z, gx, gz)) return 1.0f;
    return cellCost(*map, gx, gz);
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
    // Por linha, em paralelo, lendo a grade do .ter direto: e' o que o
    // sampleTerrainHeight devolve dentro do mapa (mapa com grade que segue o
    // terreno). So' a borda, onde a amostra vizinha pode cair num mapa ao
    // lado, passa pelo sampleTerrainHeight. Num mapa de 851x851 o laco de
    // 2,9 milhoes de amostras pelo caminho geral custava ~450 ms.
    const uint32_t w = nav.width, d = nav.height;
    std::vector<int> perRow(d, 0);
    JobSystem::instance().parallelFor(d, [&](uint32_t z) {
        const bool edgeRow = z == 0 || z + 1 >= d;
        int count = 0;
        for (uint32_t x = 0; x < w; ++x) {
            const float px = t.offsetX + x * t.scale, pz = t.offsetZ + z * t.scale;
            const bool edge = edgeRow || x == 0 || x + 1 >= w;
            auto height = [&](float hx, float hz) {
                return edge ? sampleTerrainHeight(hx, hz) : TerrainParser::getTerrainHeightAt(t, hx, hz);
            };
            const float dx = (height(px + t.scale, pz) - height(px - t.scale, pz)) / (2 * t.scale);
            const float dz = (height(px, pz + t.scale) - height(px, pz - t.scale)) / (2 * t.scale);
            if (std::sqrt(dx * dx + dz * dz) > limit) {
                nav.cells[size_t(z) * w + x].type = TerrainType::NonWalkable;
                ++count;
            }
        }
        perRow[z] = count;
    });
    int n = 0;
    for (int c : perRow) n += c;
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
