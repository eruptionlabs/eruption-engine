#include "formats/MapLoader.hpp"
#include "core/Logger.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;
namespace eruption {

std::shared_ptr<LoadedMap> MapLoader::loadMap(const std::string& mapName) {
    if (!beginLoad(mapName)) return nullptr;
    if (!loadPhaseTextures()) return nullptr;
    if (!loadPhaseGeometry()) return nullptr;
    if (!loadPhaseModels()) return nullptr;
    return finalizeLoad();
}

bool MapLoader::beginLoad(const std::string& mapName) {
    m_loadingMapName = mapName;
    m_loadingMap = std::make_shared<LoadedMap>();
    m_loadingMap->mapName = mapName;
    m_loadProgress = 0.0f;

    ERUPTION_LOG_INFO("MapLoader: begin load '%s'", m_loadingMapName.c_str());
    return true;
}

bool MapLoader::loadPhaseTextures() {
    if (!m_loadingMap) return false;
    m_loadProgress = 0.25f;
    return true;
}

static std::vector<uint8_t> readFileBytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static std::string pickGroundTexture(const std::string& mapStem) {
    if (!mapStem.empty()) {
        fs::path candidate = fs::path("assets") / "data" / "texture" / (mapStem + "_ground.png");
        if (fs::exists(candidate)) {
            return candidate.filename().string();
        }
    }
    return "hidden_alley_ground.png";
}

static TerrainFile buildFlatTerrain(uint32_t width, uint32_t height, float scale,
                                    const std::string& groundTexture,
                                    float offsetX = 0.0f, float offsetZ = 0.0f,
                                    float baseHeight = 0.0f) {
    TerrainFile terrain;
    terrain.version = 0x0107;
    terrain.width = width;
    terrain.height = height;
    terrain.scale = scale;
    terrain.offsetX = offsetX;
    terrain.offsetZ = offsetZ;
    // Note: ERUPTGNDBIN v2 stores offsetX/offsetZ explicitly.

    TerrainSurface surf;
    for (int i = 0; i < 4; ++i) {
        surf.u[i] = (i == 1 || i == 2) ? 1.0f : 0.0f;
        surf.v[i] = (i == 2 || i == 3) ? 1.0f : 0.0f;
    }
    surf.textureId = 0;
    surf.lightmapId = 0xFFFF;
    surf.color = 0xFFFFFFFF;
    terrain.surfaces.push_back(surf);
    terrain.textures.push_back(groundTexture);

    uint32_t count = width * height;
    terrain.cubes.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TerrainCube cube;
        for (int j = 0; j < 4; ++j) cube.height[j] = baseHeight;
        cube.surfaceTop = 0;
        cube.surfaceNorth = -1;
        cube.surfaceEast = -1;
        terrain.cubes.push_back(cube);
    }

    TerrainParser::computeSmoothNormals(terrain);
    TerrainParser::computeSmoothColors(terrain);
    return terrain;
}

static void loadMapEnv(LoadedMap& map, const fs::path& envPath) {
    if (!fs::exists(envPath)) return;

    std::ifstream f(envPath);
    if (!f) {
        ERUPTION_LOG_WARN("MapLoader: failed to open env '%s'", envPath.string().c_str());
        return;
    }

    try {
        nlohmann::json j;
        f >> j;

        if (j.contains("light") && j["light"].is_object()) {
            const auto& light = j["light"];
            if (light.contains("sunDirection") && light["sunDirection"].is_array() && light["sunDirection"].size() >= 3) {
                map.env.sunDirection = Vec3(
                    light["sunDirection"][0].get<float>(),
                    light["sunDirection"][1].get<float>(),
                    light["sunDirection"][2].get<float>());
            }
            // groundAlbedo: cor do quique do chao, em 0..1. Autoritativo -
            // quando presente, ignora a media automatica das texturas.
            if (light.contains("groundAlbedo") && light["groundAlbedo"].is_array() &&
                light["groundAlbedo"].size() >= 3) {
                map.env.groundAlbedoOverride = Vec3(
                    light["groundAlbedo"][0].get<float>(),
                    light["groundAlbedo"][1].get<float>(),
                    light["groundAlbedo"][2].get<float>());
                ERUPTION_LOG_WARN("MapLoader: groundAlbedo do .env = (%.3f, %.3f, %.3f)",
                                  map.env.groundAlbedoOverride.r,
                                  map.env.groundAlbedoOverride.g,
                                  map.env.groundAlbedoOverride.b);
            }
            if (light.contains("diffuse") && light["diffuse"].is_array() && light["diffuse"].size() >= 3) {
                map.env.sunColor = Vec3(
                    light["diffuse"][0].get<float>(),
                    light["diffuse"][1].get<float>(),
                    light["diffuse"][2].get<float>());
            }
            if (light.contains("opacity")) {
                map.env.sunIntensity = light["opacity"].get<float>();
            }
            if (light.contains("ambient") && light["ambient"].is_array() && light["ambient"].size() >= 3) {
                map.env.ambientColor = Vec3(
                    light["ambient"][0].get<float>(),
                    light["ambient"][1].get<float>(),
                    light["ambient"][2].get<float>());
            }
        }

        if (j.contains("water") && j["water"].is_object()) {
            const auto& water = j["water"];
            TerrainWaterPlane wp{};
            if (water.contains("level")) wp.level = water["level"].get<float>();
            if (water.contains("type")) wp.type = water["type"].get<uint32_t>();
            if (water.contains("waveHeight")) wp.waveHeight = water["waveHeight"].get<float>();
            if (water.contains("waveSpeed")) wp.waveSpeed = water["waveSpeed"].get<float>();
            if (water.contains("wavePitch")) wp.wavePitch = water["wavePitch"].get<float>();
            if (water.contains("animSpeed")) wp.textureCycling = water["animSpeed"].get<uint32_t>();
            if (map.waterPlanes.empty()) {
                map.waterPlanes.push_back(wp);
            } else {
                // Update first plane if already present from .ter
                map.waterPlanes[0] = wp;
            }
        }

        // Optional per-map authored point lights (props with their own light
        // source - lanterns, braziers, etc). Source-agnostic: this file only
        // knows the data shape, not what kind of prop or game produced it.
        if (j.contains("pointLights") && j["pointLights"].is_array()) {
            static const std::unordered_map<std::string, LightAnimType> kAnimTypes = {
                {"static", LightAnimType::Static}, {"pulse", LightAnimType::Pulse},
                {"flicker", LightAnimType::Flicker}, {"torch", LightAnimType::Torch},
            };
            for (const auto& e : j["pointLights"]) {
                if (!e.is_object() || !e.contains("position")) continue;
                const auto& p = e["position"];
                if (!p.is_array() || p.size() < 3) continue;
                LoadedMap::MapLight ml;
                ml.position = Vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
                if (e.contains("color") && e["color"].is_array() && e["color"].size() >= 3) {
                    ml.color = Vec3(e["color"][0].get<float>(), e["color"][1].get<float>(), e["color"][2].get<float>());
                }
                if (e.contains("intensity")) ml.intensity = e["intensity"].get<float>();
                if (e.contains("range")) ml.range = e["range"].get<float>();
                if (e.contains("nightOnly")) ml.nightOnly = e["nightOnly"].get<bool>();
                if (e.contains("animType") && e["animType"].is_string()) {
                    auto it = kAnimTypes.find(e["animType"].get<std::string>());
                    if (it != kAnimTypes.end()) ml.animType = it->second;
                }
                map.lights.push_back(ml);
            }
            ERUPTION_LOG_WARN("MapLoader: loaded %zu authored point light(s) from '%s'",
                              j["pointLights"].size(), envPath.string().c_str());
        }

        // Emissores de fumaça (chave opcional "smoke"). Ausente = mapa sem
        // fumaça; todo .env antigo carrega exatamente como antes. Campos
        // ausentes usam o default de SmokeEmitter.
        if (j.contains("liquids") && j["liquids"].is_array()) {
            for (const auto& e : j["liquids"]) {
                if (!e.is_object()) continue;
                LoadedMap::MapLiquid lq;
                lq.name = e.value("name", std::string());
                const std::string kind = e.value("kind", std::string("water"));
                lq.kind = (kind == "lava") ? 1 : 0;
                lq.level = e.value("level", 0.0f);
                lq.radius = e.value("radius", 0.0f);
                lq.emissive = e.value("emissive", 0.0f);
                lq.flowSpeed = e.value("flowSpeed", 1.0f);
                if (e.contains("center") && e["center"].is_array() && e["center"].size() >= 2) {
                    lq.centerX = e["center"][0].get<float>();
                    lq.centerZ = e["center"][1].get<float>();
                }
                if (e.contains("shoreRadius") && e["shoreRadius"].is_array()) {
                    lq.shoreRadius.reserve(e["shoreRadius"].size());
                    for (const auto& v : e["shoreRadius"]) lq.shoreRadius.push_back(v.get<float>());
                }
                map.liquids.push_back(lq);
            }
            ERUPTION_LOG_WARN("MapLoader: loaded %zu liquid surface(s) from '%s'",
                              map.liquids.size(), envPath.string().c_str());

            // LAVA VIRA FONTE DE LUZ. Ate' aqui `emissive` so' alimentava o
            // brilho da PROPRIA superficie da lava (lavaSettings.emissiveStrength
            // em Render.cpp): a lava brilhava, mas nao iluminava a parede da
            // cratera nem a pedra ao lado - a coisa mais obviamente errada numa
            // cena vulcanica. A tubulacao pra' consertar ja' existia por inteiro
            // (esta mesma lista `map.lights` alimenta o passe de point light);
            // faltava so' GERAR a luz a partir do liquido.
            //
            // Dedup contra luz AUTORAL: parana_demo ja' tem 18 point lights
            // autorais em volta da caldeira (centro pulsando + anel em flicker,
            // cor 1.0/0.42/0.13, intensity 3.2 = exatamente o `emissive` 3.2 do
            // liquido). Se o autor ja' iluminou aquela lava, nao empilhamos uma
            // segunda luz em cima. So' sintetiza pra' lava sem nenhuma luz a
            // menos de (raio + 40) do centro - em parana_field sao 15 de 15.
            {
                size_t synthesized = 0;
                for (const auto& lq : map.liquids) {
                    if (lq.kind != 1 || lq.emissive <= 0.0f || lq.radius <= 0.0f) continue;
                    const float dedupR = lq.radius + 40.0f;
                    bool authored = false;
                    for (const auto& ml : map.lights) {
                        const float dx = ml.position.x - lq.centerX;
                        const float dz = ml.position.z - lq.centerZ;
                        if (dx * dx + dz * dz < dedupR * dedupR) { authored = true; break; }
                    }
                    if (authored) continue;
                    LoadedMap::MapLight ml;
                    // Um pouco acima do nivel do liquido, senao a luz nasce
                    // DENTRO da superficie e a metade de baixo do raio e'
                    // desperdicada iluminando lava por baixo.
                    ml.position = Vec3(lq.centerX, lq.level + 8.0f, lq.centerZ);
                    // Cor: a mesma convencao das luzes autorais de lava do
                    // projeto (laranja de magma, nao amarelo de lampada).
                    ml.color = Vec3(1.0f, 0.42f, 0.13f);
                    // emissive == intensity na convencao autoral (3.2 == 3.2).
                    ml.intensity = lq.emissive;
                    // Raio ~1.8x o raio da poca, calibrado na luz central autoral
                    // de parana_demo (raio 167 -> range 300): alcanca a parede da
                    // cratera sem vazar pro campo em volta.
                    // ALCANCE MINIMO. 1,8x o raio funciona para o LAGO de lava
                    // (raio 180 -> 324 u), mas as bocas do flanco tem raio 18-26
                    // e sairiam com 32-47 u de alcance: a luz morre antes de sair
                    // da propria poca e a cena em volta fica no escuro (autor,
                    // 2026-09-06: "a luz dinamica ta muito fraca, a fonte de luz
                    // nao ta tao forte"). O piso poe a boca pequena iluminando a
                    // rocha em volta; a intensidade sobe junto pela raiz do quanto
                    // o alcance foi esticado, senao espalhar o mesmo fluxo por uma
                    // esfera maior deixa tudo mais fraco ainda.
                    // ERUPTION_TEST_LAVA_LIGHT="alcanceMin,multIntensidade" (bancada).
                    static const Vec2 kLavaTune = [] {
                        float mn = 110.0f, mul = 1.0f;
                        if (const char* e = std::getenv("ERUPTION_TEST_LAVA_LIGHT"))
                            std::sscanf(e, "%f,%f", &mn, &mul);
                        return Vec2(mn, mul);
                    }();
                    const float baseRange = lq.radius * 1.8f;
                    ml.range = std::max(baseRange, kLavaTune.x);
                    ml.intensity *= kLavaTune.y * std::sqrt(std::max(ml.range / std::max(baseRange, 1.0f), 1.0f));
                    ml.animType = LightAnimType::Pulse; // magma respira, nao tremula como tocha
                    ml.nightOnly = false;               // lava nao apaga de dia
                    if (std::getenv("ERUPTION_DEBUG_LAVA_LIGHT"))
                        ERUPTION_LOG_WARN("[LAVALIGHT] raio %.0f -> alcance %.0f, intensidade %.2f em (%.0f,%.0f)", lq.radius, ml.range, ml.intensity, lq.centerX, lq.centerZ);
                    map.lights.push_back(ml);
                    ++synthesized;
                }
                if (synthesized > 0) {
                    ERUPTION_LOG_WARN("MapLoader: sintetizou %zu point light(s) de lava (liquidos sem luz autoral por perto)",
                                      synthesized);
                }
            }
        }

        if (j.contains("smoke") && j["smoke"].is_array()) {
            for (const auto& e : j["smoke"]) {
                if (!e.is_object()) continue;
                const nlohmann::json pos = e.contains("pos") ? e["pos"]
                                         : (e.contains("position") ? e["position"] : nlohmann::json());
                if (!pos.is_array() || pos.size() < 3) continue;
                SmokeEmitter sm;
                sm.position = Vec3(pos[0].get<float>(), pos[1].get<float>(), pos[2].get<float>());
                if (e.contains("radius"))     sm.radius     = e["radius"].get<float>();
                if (e.contains("thickness"))  sm.height     = e["thickness"].get<float>();
                if (e.contains("height"))     sm.height     = e["height"].get<float>();
                if (e.contains("rise"))       sm.rise       = e["rise"].get<float>();
                if (e.contains("rate"))       sm.rate       = e["rate"].get<float>();
                if (e.contains("density"))    sm.density    = e["density"].get<float>();
                if (e.contains("spread"))     sm.spread     = e["spread"].get<float>();
                if (e.contains("wind"))       sm.windScale  = e["wind"].get<float>();
                if (e.contains("turbulence")) sm.turbulence = e["turbulence"].get<float>();
                if (e.contains("glow"))       sm.glow       = e["glow"].get<float>();
                if (e.contains("fire"))       sm.fire       = e["fire"].get<float>();
                if (e.contains("enabled"))    sm.enabled    = e["enabled"].get<bool>();
                if (e.contains("color") && e["color"].is_array() && e["color"].size() >= 3) {
                    sm.color = Vec3(e["color"][0].get<float>(), e["color"][1].get<float>(),
                                    e["color"][2].get<float>());
                }
                // Saneamento: valores degenerados matariam o ray-march.
                sm.radius     = std::max(sm.radius, 0.5f);
                sm.height     = std::max(sm.height, 1.0f);
                sm.rise       = std::max(sm.rise, 0.1f);
                sm.rate       = glm::clamp(sm.rate, 0.0f, 8.0f);
                sm.density    = glm::clamp(sm.density, 0.0f, 4.0f);
                sm.spread     = glm::clamp(sm.spread, 0.0f, 12.0f);
                sm.windScale  = glm::clamp(sm.windScale, 0.0f, 8.0f);
                sm.turbulence = glm::clamp(sm.turbulence, 0.001f, 0.5f);
                sm.glow       = glm::clamp(sm.glow, 0.0f, 8.0f);
                map.smokeEmitters.push_back(sm);
            }
            ERUPTION_LOG_WARN("MapLoader: loaded %zu smoke emitter(s) from '%s'",
                              map.smokeEmitters.size(), envPath.string().c_str());
        }

        ERUPTION_LOG_WARN("MapLoader: loaded companion env '%s' sunDir=(%.3f,%.3f,%.3f)",
                          envPath.string().c_str(), map.env.sunDirection.x, map.env.sunDirection.y, map.env.sunDirection.z);
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("MapLoader: failed to parse env '%s': %s", envPath.string().c_str(), e.what());
    }
}

static NavGridFile buildEmptyNavGrid(uint32_t width, uint32_t height) {
    NavGridFile navGrid;
    navGrid.width = width;
    navGrid.height = height;
    navGrid.cells.resize(width * height);
    for (auto& cell : navGrid.cells) {
        for (int i = 0; i < 4; ++i) cell.height[i] = 0.0f;
        cell.type = TerrainType::Walkable;
    }
    return navGrid;
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
        ERUPTION_LOG_WARN("MapLoader: nav magic mismatch");
        return nav;
    }

    (void)readU32(4); // version
    nav.width = readU32(8);
    nav.height = readU32(12);
    uint32_t cellCount = readU32(16);

    size_t expected = 20 + cellCount * (4 * 4 + 4);
    if (size < expected) {
        ERUPTION_LOG_WARN("MapLoader: nav truncated (expected %zu, got %zu)", expected, size);
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

bool MapLoader::loadFromGlb() {
    if (!m_loadingMap) return false;

    ERUPTION_LOG_WARN("MapLoader: loadFromGlb() invoked for '%s'", m_loadingMap->mapName.c_str());

    std::string glbPath = m_loadingMap->mapName;
    std::replace(glbPath.begin(), glbPath.end(), '\\', '/');

    // Derive a plain stem from the map path (strip "data/" prefix if present).
    std::string mapStem = glbPath;
    if (mapStem.size() >= 5 && mapStem.substr(0, 5) == "data/") {
        mapStem = mapStem.substr(5);
    }

    if (glbPath.size() < 4 || glbPath.substr(glbPath.size() - 4) != ".glb") {
        std::vector<std::string> candidates = {
            "assets/data/" + mapStem + ".glb",
            "assets/external/" + mapStem + "/" + mapStem + ".glb",
            "assets/external/" + mapStem + "/models/" + mapStem + ".glb",
            "assets/models/" + mapStem + ".glb",
            mapStem + ".glb",
            glbPath + ".glb"
        };
        glbPath.clear();
        for (const auto& c : candidates) {
            if (fs::exists(c)) {
                glbPath = c;
                break;
            }
        }
        if (glbPath.empty()) {
            ERUPTION_LOG_WARN("MapLoader: no GLB candidate found for '%s'", m_loadingMap->mapName.c_str());
            return false;
        }
    } else if (!fs::exists(glbPath)) {
        ERUPTION_LOG_WARN("MapLoader: explicit GLB path not found: '%s'", glbPath.c_str());
        return false;
    }

    ERUPTION_LOG_WARN("MapLoader: loading GLB map '%s'", glbPath.c_str());
    auto glbData = readFileBytes(glbPath);
    if (glbData.size() < 12 || std::memcmp(glbData.data(), "glTF", 4) != 0) {
        ERUPTION_LOG_ERROR("MapLoader: invalid GLB: %s", glbPath.c_str());
        return false;
    }

    ModelFile model = GltfParser::parse(glbData.data(), glbData.size(),
                                       fs::path(glbPath).parent_path().string());
    if (model.nodes.empty()) {
        ERUPTION_LOG_ERROR("MapLoader: failed to parse GLB: %s", glbPath.c_str());
        return false;
    }
    model.filePath = glbPath;

    // GLB maps are authored in world space; keep authored coordinates.
    model.skipMainOffset = true;
    m_loadingMap->models.push_back(std::move(model));
    m_loadingMap->rawDataSize += glbData.size();

    // GLB maps are authored in world space; keep authored coordinates.
    m_loadingMap->isGlbMap = true;

    // Try to load companion terrain (.ter), nav (.nav) and env (.env) files.
    // .env carries the authored light/water settings so shadows keep the
    // authored direction instead of the hard-coded GLB fallback.
    fs::path glbFsPath(glbPath);
    fs::path terPath = glbFsPath.parent_path() / (mapStem + ".ter");
    fs::path navPath = glbFsPath.parent_path() / (mapStem + ".nav");
    fs::path envPath = glbFsPath.parent_path() / (mapStem + ".env");

    loadMapEnv(*m_loadingMap, envPath);

    if (fs::exists(terPath)) {
        auto terData = readFileBytes(terPath);
        if (!terData.empty()) {
            m_loadingMap->terrain = TerrainParser::parse(terData.data(), terData.size());
            m_loadingMap->waterPlanes = m_loadingMap->terrain.waterPlanes;
            ERUPTION_LOG_WARN("MapLoader: loaded companion terrain '%s' (%ux%u) waterPlanes=%zu",
                              terPath.string().c_str(), m_loadingMap->terrain.width, m_loadingMap->terrain.height,
                              m_loadingMap->waterPlanes.size());
        }
    }

    if (fs::exists(navPath)) {
        auto navData = readFileBytes(navPath);
        if (!navData.empty()) {
            m_loadingMap->navGrid = parseNavBinary(navData.data(), navData.size());
            ERUPTION_LOG_WARN("MapLoader: loaded companion nav '%s' (%ux%u)",
                              navPath.string().c_str(), m_loadingMap->navGrid.width, m_loadingMap->navGrid.height);
        }
    }

    if (m_loadingMap->terrain.width == 0 || m_loadingMap->terrain.height == 0) {
        const float scale = 10.0f;
        const float margin = 20.0f;
        const auto& box = m_loadingMap->models.back().box;
        float worldMinX = box.min.x - margin;
        float worldMaxX = box.max.x + margin;
        float worldMinZ = box.min.z - margin;
        float worldMaxZ = box.max.z + margin;
        uint32_t terrainW = static_cast<uint32_t>(std::ceil((worldMaxX - worldMinX) / scale));
        uint32_t terrainH = static_cast<uint32_t>(std::ceil((worldMaxZ - worldMinZ) / scale));
        if (terrainW < 8) terrainW = 8;
        if (terrainH < 8) terrainH = 8;

        float groundClearance = 20.0f;
        float terrainBaseH = -(box.min.y - groundClearance);
        if (terrainBaseH < 2.0f) terrainBaseH = 2.0f;

        // Convention (see TerrainParser::parse / MapLoad.cpp): offsetX/Z is
        // the world position of grid cell (0,0), i.e. the terrain's MIN corner, not
        // the center. World-space cell x sits at offsetX + x*scale, so the terrain
        // center used for spawn is offsetX + width*scale*0.5. Passing -center here
        // (as this used to) put the "center" at -center + halfExtent, which is the
        // mirror of the real center across the origin — for a map authored away
        // from world (0,0) (any real bench_* scene) that lands the spawn near the
        // scene's edge or entirely outside its bounds. Use the min corner instead.
        std::string groundTex = pickGroundTexture(mapStem);
        m_loadingMap->terrain = buildFlatTerrain(terrainW, terrainH, scale, groundTex, worldMinX, worldMinZ, terrainBaseH);
        ERUPTION_LOG_WARN("MapLoader: GLB map loaded (%zu nodes, terrain base y=%.2f) [flat fallback]",
                          m_loadingMap->models.back().nodes.size(), -terrainBaseH);
    }

    if (m_loadingMap->navGrid.width == 0 || m_loadingMap->navGrid.height == 0) {
        m_loadingMap->navGrid = buildEmptyNavGrid(m_loadingMap->terrain.width, m_loadingMap->terrain.height);
    }

    return true;
}

bool MapLoader::loadPhaseGeometry() {
    if (!m_loadingMap) return false;

    if (!loadFromGlb()) {
        ERUPTION_LOG_ERROR("MapLoader: failed to load GLB map '%s'", m_loadingMapName.c_str());
        return false;
    }

    loadTerrainChunks();
    m_loadProgress = 0.6f;
    return true;
}

bool MapLoader::loadPhaseModels() {
    if (!m_loadingMap) return false;

    // GLB maps contain exactly one model with all nodes already in world space.
    ERUPTION_LOG_INFO("MapLoader: using %zu embedded model(s) from GLB",
                      static_cast<size_t>(m_loadingMap->models.size()));

    // Default directional light for GLB maps. Values are tuned to match the
    // look of legacy maps without overriding the daynight cycle.
    {
        LoadedMap::MapLight sun;
        sun.position = Vec3(500.0f, 1000.0f, 500.0f);
        sun.color = Vec3(0.15f, 0.14f, 0.13f);
        sun.intensity = 1.0f;
        sun.range = 5000.0f;
        sun.animType = LightAnimType::Static;
        m_loadingMap->lights.push_back(sun);

        // Brighter directional + ambient light for GLB maps so authored PBR
        // textures are visible without relying on the daynight cycle.
        m_loadingMap->env.sunDirection = Vec3(0.577f, -0.707f, 0.408f);
        m_loadingMap->env.sunColor = Vec3(0.25f, 0.24f, 0.22f);
        m_loadingMap->env.sunIntensity = 2.0f;
        m_loadingMap->env.ambientColor = Vec3(0.18f, 0.18f, 0.20f);
        m_loadingMap->env.ambientIntensity = 1.5f;
    }

    // Extract terrain point lights/heuristics (mostly empty for generated flat terrain).
    auto terrainLights = TerrainParser::extractPointLights(m_loadingMap->terrain);
    for (const auto& gl : terrainLights) {
        LoadedMap::MapLight ml;
        ml.position = gl.worldPos;
        ml.color = gl.color;
        ml.intensity = gl.intensity;
        ml.range = gl.radius;
        ml.animType = gl.animType;
        m_loadingMap->lights.push_back(ml);
    }

    m_loadProgress = 0.9f;
    return true;
}

std::shared_ptr<LoadedMap> MapLoader::finalizeLoad() {
    if (!m_loadingMap) return nullptr;
    m_loadProgress = 1.0f;
    size_t parsedSize = m_loadingMap->rawDataSize * 2;
    if (parsedSize > 0) {
        PROFILE_RAM_ALLOC(ProfilerCategory::FileCache, m_loadingMap->rawDataSize);
        PROFILE_RAM_ALLOC(ProfilerCategory::LoadingIO, parsedSize - m_loadingMap->rawDataSize);
    }
    ERUPTION_LOG_INFO("MapLoader: map '%s' loaded (%d models, %d lights, %d chunks), raw=%zu parsed≈%zu",
                     m_loadingMap->mapName.c_str(),
                     static_cast<int>(m_loadingMap->models.size()),
                     static_cast<int>(m_loadingMap->lights.size()),
                     static_cast<int>(m_loadingMap->terrainChunks.size()),
                     m_loadingMap->rawDataSize, parsedSize);
    return m_loadingMap;
}

void MapLoader::loadTerrainChunks() {
    if (m_loadingMap->terrain.width == 0 || m_loadingMap->terrain.height == 0) return;

    if (m_loadingMap->isGlbMap) {
        ERUPTION_LOG_INFO("MapLoader: skipping terrain chunk generation for GLB map '%s'",
                          m_loadingMap->mapName.c_str());
        return;
    }

    TerrainMesh mesh = TerrainParser::generateMesh(m_loadingMap->terrain, 0, 0,
                                                m_loadingMap->terrain.width,
                                                m_loadingMap->terrain.height);
    if (!mesh.vertices.empty()) {
        m_loadingMap->terrainChunks.push_back(std::move(mesh));
    }
}

} // namespace eruption
