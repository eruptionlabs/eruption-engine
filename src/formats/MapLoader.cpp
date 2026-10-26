#include "formats/MapLoader.hpp"
#include "formats/GroundPack.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
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
        fs::path candidate = project::game("assets/data/texture") / (mapStem + "_ground.png");
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

MapFog parseMapFog(const nlohmann::json& f) {
    MapFog fog;
    fog.set = true;
    if (f.contains("color") && f["color"].is_array() && f["color"].size() >= 3)
        fog.color = Vec3(f["color"][0].get<float>(), f["color"][1].get<float>(), f["color"][2].get<float>());
    fog.start = f.value("start", fog.start);
    fog.end = std::max(f.value("end", fog.end), fog.start + 1.0f);
    fog.opacity = std::clamp(f.value("opacity", fog.opacity), 0.0f, 1.0f);
    fog.height = f.value("height", fog.height);
    fog.heightFalloff = std::max(f.value("falloff", fog.heightFalloff), 0.0f);
    return fog;
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

        if (j.contains("sky") && j["sky"].is_boolean()) map.env.hasSky = j["sky"].get<bool>();
        if (j.contains("edge_apron")) {
            const auto& a = j["edge_apron"];
            auto& p = map.edgeApronParams;
            p.enabled = a.is_object() || (a.is_boolean() && a.get<bool>());
            if (a.is_object()) {
                p.enabled = a.value("enabled", true);
                p.first = std::max(1.0f, a.value("first", p.first));
                p.bands = std::clamp(a.value("bands", p.bands), 1, 12);
                p.slope = a.value("slope", p.slope);
            }
        }
        if (j.contains("fog") && j["fog"].is_object()) {
            map.env.fog = parseMapFog(j["fog"]);
            ERUPTION_LOG_WARN("MapLoader: nevoa do .env (cor %.2f %.2f %.2f, %.0f..%.0f, opacidade %.2f)",
                              map.env.fog.color.r, map.env.fog.color.g, map.env.fog.color.b,
                              map.env.fog.start, map.env.fog.end, map.env.fog.opacity);
        }
        // "background": "fog" (cor da nevoa) ou [r, g, b] (linear, antes da
        // exposicao): o vazio abaixo do horizonte na borda do mundo.
        if (j.contains("background")) {
            const auto& b = j["background"];
            if (b.is_string() && b.get<std::string>() == "fog") {
                map.env.backgroundMode = 1;
            } else if (b.is_array() && b.size() >= 3) {
                map.env.backgroundMode = 2;
                map.env.backgroundColor = Vec3(b[0].get<float>(), b[1].get<float>(), b[2].get<float>());
            } else {
                ERUPTION_LOG_WARN("MapLoader: \"background\" do .env deve ser \"fog\" ou [r, g, b]");
            }
        }

        if (j.contains("light") && j["light"].is_object()) {
            map.env.lightAuthored = j["light"].value("authoritative", false);
            const auto& light = j["light"];
            // Direcao da LUZ (do sol para o chao; y < 0 = dia). O gerador
            // procedural grava "direction"; mapas antigos, "sunDirection".
            // So' "sunDirection" era lido, e o .env do gerador caia sempre
            // no padrao (sol a pino).
            const char* sunKey = light.contains("sunDirection") ? "sunDirection" : "direction";
            if (light.contains(sunKey) && light[sunKey].is_array() && light[sunKey].size() >= 3) {
                map.env.sunDirection = Vec3(
                    light[sunKey][0].get<float>(),
                    light[sunKey][1].get<float>(),
                    light[sunKey][2].get<float>());
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
            // A COR do ambiente sozinha nao acende nada: ela e' multiplicada
            // por uma intensidade que so' existia no slider do editor. Sem
            // esta chave o autor de um interior mexia na cor e via a mesma
            // tela preta. 0.95 e' o valor de fabrica do slider.
            if (light.contains("ambientIntensity")) {
                map.env.ambientIntensity = light["ambientIntensity"].get<float>();
            }
            // Ambiente so' vence o ciclo sob a mesma chave: os .env
            // convertidos tambem trazem `ambient`, e ligar por ele deixaria
            // os 1040 mapas com o preenchimento preso no valor do conversor.
            map.env.ambientAuthored = map.env.lightAuthored;
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
                if (e.contains("spotDir") && e["spotDir"].is_array() && e["spotDir"].size() >= 3) {
                    const Vec3 d(e["spotDir"][0].get<float>(), e["spotDir"][1].get<float>(), e["spotDir"][2].get<float>());
                    if (glm::dot(d, d) > 1e-8f) ml.spotDir = glm::normalize(d);
                    ml.spotAngle = e.value("spotAngle", 60.0f);
                }
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
                lq.shoreHeat = std::clamp(e.value("shoreHeat", 0.0f), 0.0f, 4.0f);
                lq.centerHeat = std::clamp(e.value("centerHeat", 0.0f), 0.0f, 4.0f);
                lq.flowSpeed = e.value("flowSpeed", 1.0f);
                if (e.contains("crustCoverage") && e["crustCoverage"].is_number())
                    lq.crustCoverage = std::clamp(e["crustCoverage"].get<float>(), 0.0f, 1.0f);
                if (e.contains("crustTint") && e["crustTint"].is_array() && e["crustTint"].size() >= 3)
                    lq.crustTint = Vec3(e["crustTint"][0].get<float>(), e["crustTint"][1].get<float>(),
                                        e["crustTint"][2].get<float>());
                if (e.contains("center") && e["center"].is_array() && e["center"].size() >= 2) {
                    lq.centerX = e["center"][0].get<float>();
                    lq.centerZ = e["center"][1].get<float>();
                }
                if (e.contains("shoreRadius") && e["shoreRadius"].is_array()) {
                    lq.shoreRadius.reserve(e["shoreRadius"].size());
                    for (const auto& v : e["shoreRadius"]) lq.shoreRadius.push_back(v.get<float>());
                }
                // Contorno livre: "polygon": [[x, y, z], ...] ou "polygons": [[...], ...].
                // Ponto [x, z] fica na altura `level`.
                auto readPolygon = [&lq](const nlohmann::json& pts) {
                    std::vector<Vec3> poly;
                    for (const auto& p : pts) {
                        if (!p.is_array() || p.size() < 2) continue;
                        poly.push_back(p.size() >= 3 ? Vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>())
                                                     : Vec3(p[0].get<float>(), lq.level, p[1].get<float>()));
                    }
                    if (poly.size() >= 3) lq.polygons.push_back(std::move(poly));
                };
                if (e.contains("polygon") && e["polygon"].is_array()) readPolygon(e["polygon"]);
                if (e.contains("polygons") && e["polygons"].is_array())
                    for (const auto& pg : e["polygons"])
                        if (pg.is_array()) readPolygon(pg);
                // Sem "level", o nivel do contorno e' a media das alturas (luz da
                // lava, world.liquids; a superficie segue cada ponto).
                if (!e.contains("level") && !lq.polygons.empty()) {
                    double sum = 0.0;
                    size_t cnt = 0;
                    for (const auto& pg : lq.polygons)
                        for (const Vec3& q : pg) sum += q.y, ++cnt;
                    lq.level = float(sum / double(cnt));
                }
                LoadedMap::buildLiquidContour(lq);
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

        if (j.contains("effects") && j["effects"].is_array()) {
            for (const auto& e : j["effects"]) {
                if (!e.is_object() || !e.contains("name") || !e.contains("pos")) continue;
                const nlohmann::json& pos = e["pos"];
                if (!pos.is_array() || pos.size() < 3) continue;
                LoadedMap::MapEffect fx;
                fx.name = e["name"].get<std::string>();
                fx.position = Vec3(pos[0].get<float>(), pos[1].get<float>(), pos[2].get<float>());
                if (e.contains("rot") && e["rot"].is_array() && e["rot"].size() >= 3)
                    fx.rotationDeg = Vec3(e["rot"][0].get<float>(), e["rot"][1].get<float>(), e["rot"][2].get<float>());
                if (e.contains("scale")) fx.scale = std::max(e["scale"].get<float>(), 0.001f);
                if (e.contains("enabled")) fx.enabled = e["enabled"].get<bool>();
                if (e.contains("nightOnly")) fx.nightOnly = e["nightOnly"].get<bool>();
                map.effects.push_back(fx);
            }
        }
        if (j.contains("vfxUnitsPerMeter")) map.vfxUnitsPerMeter = j["vfxUnitsPerMeter"].get<float>();

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

// Area com sinal no plano XZ (positiva = anti-horario visto de cima, com X
// para a direita e Z para baixo na tela).
static float signedAreaXZ(const std::vector<Vec3>& p) {
    float a = 0.0f;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) a += p[j].x * p[i].z - p[i].x * p[j].z;
    return 0.5f * a;
}

static bool pointInTriXZ(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    auto side = [](const Vec3& u, const Vec3& v, const Vec3& w) {
        return (v.x - u.x) * (w.z - u.z) - (v.z - u.z) * (w.x - u.x);
    };
    const float d1 = side(a, b, p), d2 = side(b, c, p), d3 = side(c, a, p);
    const bool neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f, pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(neg && pos);
}

// Orelhas (O(n^2)): contorno simples, sem buraco. Um contorno que se cruza
// sai com o que der para cortar e um aviso.
static void earClipXZ(std::vector<Vec3> poly, std::vector<Vec3>& out, const std::string& name) {
    if (poly.size() < 3) return;
    if (signedAreaXZ(poly) < 0.0f) std::reverse(poly.begin(), poly.end());
    std::vector<size_t> idx(poly.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    size_t guard = 0;
    while (idx.size() > 3 && guard++ < poly.size() * poly.size()) {
        bool cut = false;
        for (size_t k = 0; k < idx.size(); ++k) {
            const Vec3& a = poly[idx[(k + idx.size() - 1) % idx.size()]];
            const Vec3& b = poly[idx[k]];
            const Vec3& c = poly[idx[(k + 1) % idx.size()]];
            const float cross = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
            if (cross <= 0.0f) continue; // reflexo (ou colinear)
            bool inside = false;
            for (size_t m = 0; m < idx.size() && !inside; ++m) {
                const Vec3& p = poly[idx[m]];
                if (&p == &a || &p == &b || &p == &c) continue;
                inside = pointInTriXZ(p, a, b, c);
            }
            if (inside) continue;
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
            idx.erase(idx.begin() + std::ptrdiff_t(k));
            cut = true;
            break;
        }
        if (!cut) {
            ERUPTION_LOG_WARN("MapLoader: liquid '%s' outline crosses itself; %zu point(s) left out", name.c_str(),
                              idx.size());
            return;
        }
    }
    if (idx.size() == 3) {
        out.push_back(poly[idx[0]]);
        out.push_back(poly[idx[1]]);
        out.push_back(poly[idx[2]]);
    }
}

void LoadedMap::buildLiquidContour(MapLiquid& lq) {
    lq.triangles.clear();
    if (lq.polygons.empty()) return;
    for (const auto& poly : lq.polygons) earClipXZ(poly, lq.triangles, lq.name);
    Vec3 mn(1e30f), mx(-1e30f);
    for (const auto& poly : lq.polygons)
        for (const Vec3& p : poly) {
            mn = glm::min(mn, p);
            mx = glm::max(mx, p);
        }
    lq.centerX = 0.5f * (mn.x + mx.x);
    lq.centerZ = 0.5f * (mn.z + mx.z);
    lq.radius = 0.0f;
    for (const auto& poly : lq.polygons)
        for (const Vec3& p : poly) lq.radius = std::max(lq.radius, std::hypot(p.x - lq.centerX, p.z - lq.centerZ));
    lq.shoreRadius.clear();
    lq.contourLevel = lq.level;
    buildLiquidContourMesh(lq);
}

// Bordas do contorno que nao sao compartilhadas por dois poligonos (a margem
// de verdade): a junta entre dois pedacos do mesmo rio nao esquenta.
static std::vector<std::pair<Vec2, Vec2>> liquidOuterEdges(const LoadedMap::MapLiquid& lq) {
    std::vector<std::pair<Vec2, Vec2>> all;
    for (const auto& poly : lq.polygons)
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
            all.push_back({Vec2(poly[j].x, poly[j].z), Vec2(poly[i].x, poly[i].z)});
    auto same = [](Vec2 a, Vec2 b) { return glm::dot(a - b, a - b) < 1e-4f; };
    std::vector<std::pair<Vec2, Vec2>> out;
    for (size_t i = 0; i < all.size(); ++i) {
        bool shared = false;
        for (size_t k = 0; k < all.size() && !shared; ++k)
            shared = k != i && ((same(all[i].first, all[k].second) && same(all[i].second, all[k].first)) ||
                                (same(all[i].first, all[k].first) && same(all[i].second, all[k].second)));
        if (!shared) out.push_back(all[i]);
    }
    return out;
}

namespace {
// Distancia de um ponto a margem com as arestas numa grade uniforme: busca em
// aneis de celulas em volta do ponto, parando quando a melhor distancia ja'
// e' menor que a distancia ate' o anel seguinte (aresta fora dos aneis vistos
// fica pelo menos k celulas longe). O minimo e' o mesmo de testar todas.
struct EdgeGrid {
    Vec2 lo{0.0f};
    float cell = 1.0f;
    int nx = 1, nz = 1;
    std::vector<std::pair<Vec2, Vec2>> edges;
    std::vector<std::vector<uint32_t>> bins;

    EdgeGrid(std::vector<std::pair<Vec2, Vec2>> e, Vec2 mn, Vec2 mx, float cellSize) : edges(std::move(e)) {
        for (const auto& ed : edges) {
            mn = glm::min(mn, glm::min(ed.first, ed.second));
            mx = glm::max(mx, glm::max(ed.first, ed.second));
        }
        lo = mn;
        cell = std::max(cellSize, 1e-3f);
        nx = std::max(1, int(std::ceil((mx.x - mn.x) / cell)) + 1);
        nz = std::max(1, int(std::ceil((mx.y - mn.y) / cell)) + 1);
        bins.resize(size_t(nx) * size_t(nz));
        for (uint32_t i = 0; i < edges.size(); ++i) {
            const Vec2 a = glm::min(edges[i].first, edges[i].second), b = glm::max(edges[i].first, edges[i].second);
            const int x0 = cx(a.x), x1 = cx(b.x), z0 = cz(a.y), z1 = cz(b.y);
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) bins[size_t(z) * size_t(nx) + size_t(x)].push_back(i);
        }
    }
    int cx(float x) const { return std::clamp(int(std::floor((x - lo.x) / cell)), 0, nx - 1); }
    int cz(float z) const { return std::clamp(int(std::floor((z - lo.y) / cell)), 0, nz - 1); }
    float dist(Vec2 p) const {
        float best = 1e30f;
        auto test = [&](uint32_t i) {
            const auto& e = edges[i];
            const Vec2 d = e.second - e.first;
            const float len2 = glm::dot(d, d);
            const float t = len2 > 0.0f ? glm::clamp(glm::dot(p - e.first, d) / len2, 0.0f, 1.0f) : 0.0f;
            const Vec2 q = e.first + d * t;
            best = std::min(best, glm::dot(p - q, p - q));
        };
        const int px = cx(p.x), pz = cz(p.y);
        const int kMax = std::max(nx, nz);
        for (int k = 0; k <= kMax; ++k) {
            for (int z = pz - k; z <= pz + k; ++z) {
                if (z < 0 || z >= nz) continue;
                const bool edgeRow = z == pz - k || z == pz + k;
                for (int x = px - k; x <= px + k; x += (edgeRow || k == 0) ? 1 : 2 * k) {
                    if (x < 0 || x >= nx) continue;
                    for (uint32_t i : bins[size_t(z) * size_t(nx) + size_t(x)]) test(i);
                }
            }
            const float reach = float(k) * cell;
            if (best <= reach * reach) break;
        }
        return std::sqrt(best);
    }
};
} // namespace

// Malha do contorno: cada triangulo da triangulacao dividido em n x n (aresta
// ate' `maxEdge`), altura interpolada entre os cantos. O UV x leva a fracao
// "radial" que o shader usa para o calor de borda: 1 na margem, 0 no ponto
// mais longe dela. As posicoes passam pela mesma conta de giro (zero) em volta
// do centro que o desenho usa, para sair igual bit a bit ao caminho antigo.
void LoadedMap::buildLiquidContourMesh(MapLiquid& lq) {
    lq.contourMesh.reset();
    if (lq.triangles.empty()) return;
    auto mesh = std::make_shared<WaterMesh>();
    const float maxEdge = std::max(lq.radius / 24.0f, 1.0f);
    const float c = 1.0f, s = 0.0f;
    auto place = [&](Vec3 p) {
        const float dx = p.x - lq.centerX, dz = p.z - lq.centerZ;
        return Vec3(lq.centerX + dx * c - dz * s, p.y, lq.centerZ + dx * s + dz * c);
    };
    Vec2 mn(1e30f), mx(-1e30f);
    for (size_t t = 0; t + 2 < lq.triangles.size(); t += 3) {
        const Vec3 a = place(lq.triangles[t]), b = place(lq.triangles[t + 1]), cc = place(lq.triangles[t + 2]);
        auto lenXZ = [](Vec3 u, Vec3 v) { return std::hypot(u.x - v.x, u.z - v.z); };
        const float longest = std::max({lenXZ(a, b), lenXZ(b, cc), lenXZ(cc, a)});
        const int n = std::clamp(int(std::ceil(longest / maxEdge)), 1, 64);
        const uint32_t base = uint32_t(mesh->vertices.size());
        // Linha i (0..n) tem n - i + 1 pontos: P = a + (b - a) j/n + (cc - a) i/n.
        for (int i = 0; i <= n; ++i)
            for (int j = 0; j <= n - i; ++j) {
                WaterVertex v{};
                v.position = a + (b - a) * (float(j) / float(n)) + (cc - a) * (float(i) / float(n));
                mn = glm::min(mn, Vec2(v.position.x, v.position.z));
                mx = glm::max(mx, Vec2(v.position.x, v.position.z));
                mesh->vertices.push_back(v);
            }
        auto at = [&](int i, int j) { return base + uint32_t(i * (n + 1) - i * (i - 1) / 2 + j); };
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n - i; ++j) {
                mesh->indices.push_back(at(i, j));
                mesh->indices.push_back(at(i, j + 1));
                mesh->indices.push_back(at(i + 1, j));
                if (j + 1 < n - i) {
                    mesh->indices.push_back(at(i, j + 1));
                    mesh->indices.push_back(at(i + 1, j + 1));
                    mesh->indices.push_back(at(i + 1, j));
                }
            }
        mesh->waterTileCount += uint32_t(n * n);
    }
    std::vector<std::pair<Vec2, Vec2>> placed;
    for (const auto& e : liquidOuterEdges(lq)) {
        const Vec3 p0 = place(Vec3(e.first.x, 0.0f, e.first.y)), p1 = place(Vec3(e.second.x, 0.0f, e.second.y));
        placed.push_back({Vec2(p0.x, p0.z), Vec2(p1.x, p1.z)});
    }
    const EdgeGrid grid(std::move(placed), mn, mx, maxEdge);
    float far = 0.0f;
    for (WaterVertex& v : mesh->vertices) {
        v.texCoord.y = grid.dist(Vec2(v.position.x, v.position.z));
        far = std::max(far, v.texCoord.y);
    }
    for (WaterVertex& v : mesh->vertices) v.texCoord = Vec2(far > 0.0f ? 1.0f - v.texCoord.y / far : 1.0f, 0.0f);
    lq.contourMesh = std::move(mesh);
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
        // Caminhos de MAPA: saem do projeto aberto (project::game), ou da
        // pasta do repositorio quando nao ha projeto.
        std::vector<std::string> candidates = {
            project::game("assets/data/" + mapStem + ".glb").string(),
            project::game("assets/external/" + mapStem + "/" + mapStem + ".glb").string(),
            project::game("assets/external/" + mapStem + "/models/" + mapStem + ".glb").string(),
            project::game("assets/models/" + mapStem + ".glb").string(),
            mapStem + ".glb",
            glbPath + ".glb"
        };
        glbPath.clear();
        for (const auto& c : candidates) {
            if (fs::exists(c) || fs::exists(fs::path(c).replace_extension(".relevo"))) {
                glbPath = c;
                break;
            }
        }
        if (glbPath.empty()) {
            ERUPTION_LOG_WARN("MapLoader: no GLB candidate found for '%s'", m_loadingMap->mapName.c_str());
            return false;
        }
    } else if (!fs::exists(glbPath) && !fs::exists(fs::path(glbPath).replace_extension(".relevo"))) {
        ERUPTION_LOG_WARN("MapLoader: explicit GLB path not found: '%s'", glbPath.c_str());
        return false;
    }

    // Mapa exportado com o chao empacotado: so' o .relevo existe. O .glb e o
    // .ter saem do cache (GroundPack); glbPath continua sendo onde o .glb
    // estaria, que e' a chave de tudo o que mora ao lado dele (.probes,
    // pacotes assados, .nav, .env).
    std::string readPath = glbPath;
    fs::path relevoTer;
    if (!fs::exists(glbPath)) {
        const fs::path relevo = fs::path(glbPath).replace_extension(".relevo");
        fs::path cachedGlb;
        std::string error;
        if (!unpackGroundCached(relevo, cachedGlb, relevoTer, error)) {
            ERUPTION_LOG_ERROR("MapLoader: %s: %s", relevo.string().c_str(), error.c_str());
            return false;
        }
        readPath = cachedGlb.string();
        ERUPTION_LOG_WARN("MapLoader: ground from '%s' (cache %s)", relevo.string().c_str(), readPath.c_str());
    }

    ERUPTION_LOG_WARN("MapLoader: loading GLB map '%s'", glbPath.c_str());
    auto glbData = readFileBytes(readPath);
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
    fs::path terPath = relevoTer.empty() ? glbFsPath.parent_path() / (mapStem + ".ter") : relevoTer;
    fs::path navPath = glbFsPath.parent_path() / (mapStem + ".nav");
    fs::path envPath = glbFsPath.parent_path() / (mapStem + ".env");

    loadMapEnv(*m_loadingMap, envPath);
    if (m_loadingMap->edgeApronParams.enabled && !m_loadingMap->buildEdgeAprons(m_loadingMap->models.back()))
        ERUPTION_LOG_WARN("MapLoader: edge_apron sem malha de chao em grade em '%s'", glbPath.c_str());

    if (fs::exists(terPath)) {
        auto terData = readFileBytes(terPath);
        if (!terData.empty()) {
            m_loadingMap->terrain = TerrainParser::parse(terData.data(), terData.size());
            m_loadingMap->terrainFromFile = m_loadingMap->terrain.width > 0 && m_loadingMap->terrain.height > 0;
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
        // Pontual de preenchimento so' quando o .env nao manda na luz
        // (light.authoritative): ela enche a tela por igual e, num mapa com a
        // luz autorada, achatava o contraste sol/sombra (22,6% do brilho da
        // tela num mapa medido, contraste 1,40 contra ~3 sem ela).
        if (!m_loadingMap->env.lightAuthored) {
            LoadedMap::MapLight sun;
            sun.position = Vec3(500.0f, 1000.0f, 500.0f);
            sun.color = Vec3(0.15f, 0.14f, 0.13f);
            sun.intensity = 1.0f;
            sun.range = 5000.0f;
            sun.animType = LightAnimType::Static;
            m_loadingMap->lights.push_back(sun);
        }

        // Brighter directional + ambient light for GLB maps so authored PBR
        // textures are visible without relying on the daynight cycle.
        // So' quando o mapa NAO trouxe .env com bloco `light`: senao esta
        // reserva apagava o que o autor acabara de escrever.
        if (!m_loadingMap->env.lightAuthored) {
            m_loadingMap->env.sunDirection = Vec3(0.577f, -0.707f, 0.408f);
            m_loadingMap->env.sunColor = Vec3(0.25f, 0.24f, 0.22f);
            m_loadingMap->env.sunIntensity = 2.0f;
            m_loadingMap->env.ambientColor = Vec3(0.18f, 0.18f, 0.20f);
            m_loadingMap->env.ambientIntensity = 1.5f;
        }
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

namespace {

// Vertices da malha de chao na linha pos[axis] == value, em ordem pelo eixo
// `along`; no mesmo ponto fica o mais alto (uma saia repete o vertice da borda
// mais embaixo).
std::vector<uint32_t> apronSideVertices(const std::vector<Vec3>& pos, int axis, float value, int along, float eps) {
    std::vector<uint32_t> pts;
    for (uint32_t v = 0; v < pos.size(); ++v)
        if (std::abs(pos[v][axis] - value) <= eps) pts.push_back(v);
    std::sort(pts.begin(), pts.end(), [&](uint32_t a, uint32_t b) {
        if (std::abs(pos[a][along] - pos[b][along]) > eps) return pos[a][along] < pos[b][along];
        return pos[a].y > pos[b].y;
    });
    std::vector<uint32_t> out;
    for (uint32_t v : pts)
        if (out.empty() || std::abs(pos[v][along] - pos[out.back()][along]) > eps) out.push_back(v);
    return out;
}

// UV da malha de chao como plano em XZ (uv = A * (x, z, 1)), por minimos
// quadrados. Falso se a UV nao e' plana (resto acima de 1% da faixa): o
// avental entao repete a UV da borda.
bool apronPlanarUv(const ModelFileNode& g, glm::mat3x2& out) {
    if (g.texCoords.size() != g.vertices.size() || g.vertices.size() < 3) return false;
    const size_t step = std::max<size_t>(1, g.vertices.size() / 4096);
    glm::dmat3 ata(0.0);
    glm::dmat3x2 atb(0.0); // coluna j = soma de r_j * uv
    for (size_t i = 0; i < g.vertices.size(); i += step) {
        const glm::dvec3 r(g.vertices[i].x, g.vertices[i].z, 1.0);
        ata += glm::outerProduct(r, r);
        for (int j = 0; j < 3; ++j) atb[j] += r[j] * glm::dvec2(g.texCoords[i]);
    }
    if (std::abs(glm::determinant(ata)) < 1e-12) return false;
    const glm::dmat3 inv = glm::inverse(ata);
    glm::dmat3x2 a(0.0);
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) a[j] += inv[k][j] * atb[k];
    Vec2 lo(1e30f), hi(-1e30f);
    double err = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < g.vertices.size(); i += step, ++n) {
        const glm::dvec2 uv = a * glm::dvec3(g.vertices[i].x, g.vertices[i].z, 1.0);
        err += glm::dot(uv - glm::dvec2(g.texCoords[i]), uv - glm::dvec2(g.texCoords[i]));
        lo = glm::min(lo, g.texCoords[i]);
        hi = glm::max(hi, g.texCoords[i]);
    }
    const float range = std::max(hi.x - lo.x, hi.y - lo.y);
    if (range <= 0.0f || std::sqrt(err / double(n)) > 0.01 * double(range)) return false;
    out = glm::mat3x2(a);
    return true;
}

// Colunas de uma linha do avental: de `stride` em `stride`, mais a ultima
// (o canto).
std::vector<uint32_t> apronRowColumns(size_t cols, size_t stride) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cols; i += stride) out.push_back(uint32_t(i));
    if (out.back() + 1 != cols) out.push_back(uint32_t(cols - 1));
    return out;
}

// Triangulo com a face para o mesmo lado do chao (groundSign = sinal de y do
// produto vetorial de um triangulo da malha de chao).
void apronTriangle(ModelFileNode& n, uint32_t a, uint32_t b, uint32_t c, float groundSign) {
    const Vec3 e = glm::cross(n.vertices[b] - n.vertices[a], n.vertices[c] - n.vertices[a]);
    const bool flip = (e.y < 0.0f ? -1.0f : 1.0f) != groundSign;
    n.indices.insert(n.indices.end(), {a, flip ? c : b, flip ? b : c});
}

// Celula de um anel: o segmento de fora [colA, colB] (vertices outA, outB)
// ligado em leque aos vertices de dentro nas colunas colA..colB. Sem
// junta em T: a linha de dentro e' a de fora do anel anterior.
void apronFan(ModelFileNode& n, const std::vector<uint32_t>& innerCols, uint32_t innerStart, uint32_t colA,
              uint32_t colB, uint32_t outA, uint32_t outB, float groundSign) {
    const auto lo = std::lower_bound(innerCols.begin(), innerCols.end(), colA);
    const auto hi = std::upper_bound(innerCols.begin(), innerCols.end(), colB);
    const uint32_t first = innerStart + uint32_t(lo - innerCols.begin());
    const uint32_t count = uint32_t(hi - lo);
    if (count < 2) return;
    const uint32_t mid = first + (count - 1) / 2;
    for (uint32_t v = first; v < mid; ++v) apronTriangle(n, v, v + 1, outA, groundSign);
    apronTriangle(n, mid, outB, outA, groundSign);
    for (uint32_t v = mid; v + 1 < first + count; ++v) apronTriangle(n, v, v + 1, outB, groundSign);
}

template <typename T>
void apronCopyAttr(const std::vector<T>& src, size_t srcCount, uint32_t from, std::vector<T>& dst) {
    if (src.size() == srcCount) dst.push_back(src[from]);
}

} // namespace

bool LoadedMap::buildEdgeAprons(ModelFile& model) {
    // Chao: o no' de malha propria que cobre a maior area em XZ.
    int ground = -1;
    float bestArea = 0.0f;
    Vec3 lo(0.0f), hi(0.0f);
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const ModelFileNode& n = model.nodes[i];
        if (n.vertices.size() < 9 || n.skin >= 0 || n.geometryShareIndex >= 0) continue;
        Vec3 a(1e30f), b(-1e30f);
        for (const Vec3& p : n.vertices) {
            a = glm::min(a, p);
            b = glm::max(b, p);
        }
        const float area = (b.x - a.x) * (b.z - a.z);
        if (area > bestArea) {
            bestArea = area;
            ground = int(i);
            lo = a;
            hi = b;
        }
    }
    if (ground < 0) return false;
    const ModelFileNode g = model.nodes[size_t(ground)]; // copia: o push_back abaixo pode realocar
    const size_t count = g.vertices.size();
    const float eps = std::max(hi.x - lo.x, hi.z - lo.z) * 1e-4f + 1e-3f;
    glm::mat3x2 uvPlane(0.0f);
    const bool planarUv = apronPlanarUv(g, uvPlane);
    float groundSign = 1.0f;
    if (g.indices.size() >= 3) {
        const Vec3 a = g.vertices[g.indices[0]], b = g.vertices[g.indices[1]], c = g.vertices[g.indices[2]];
        groundSign = glm::cross(b - a, c - a).y < 0.0f ? -1.0f : 1.0f;
    }
    const auto& prm = edgeApronParams;
    std::vector<float> dist(size_t(prm.bands) + 1, 0.0f);
    for (int k = 1; k <= prm.bands; ++k) dist[size_t(k)] = prm.first * float((1 << k) - 1);

    struct Side { const char* tag; int axis; float value; int along; Vec2 out; };
    const Side sides[4] = {{"x0", 0, lo.x, 2, Vec2(-1.0f, 0.0f)}, {"x1", 0, hi.x, 2, Vec2(1.0f, 0.0f)},
                           {"z0", 2, lo.z, 0, Vec2(0.0f, -1.0f)}, {"z1", 2, hi.z, 0, Vec2(0.0f, 1.0f)}};
    const bool groundIsRoot = std::find(model.rootNodeNames.begin(), model.rootNodeNames.end(), g.name) !=
                              model.rootNodeNames.end();
    size_t totalVerts = 0;
    for (const Side& s : sides) {
        const std::vector<uint32_t> edge = apronSideVertices(g.vertices, s.axis, s.value, s.along, eps);
        const float alongLo = s.along == 0 ? lo.x : lo.z, alongHi = s.along == 0 ? hi.x : hi.z;
        if (edge.size() < 2) continue;
        const float span = g.vertices[edge.back()][s.along] - g.vertices[edge.front()][s.along];
        if (span < 0.5f * (alongHi - alongLo)) continue; // borda que nao e' grade (chao recortado)
        // Altura alisada ao longo do lado: media movel com janela que cresce
        // com a distancia da borda (perto segue o relevo, longe some o
        // degrau). A media do lado inteiro afundava o avental 180 u sob uma
        // crista estreita, e a crista fazia sombra num fosso.
        std::vector<double> prefix(edge.size() + 1, 0.0);
        for (size_t i = 0; i < edge.size(); ++i) prefix[i + 1] = prefix[i] + double(g.vertices[edge[i]].y);
        const float step = span / float(edge.size() - 1);
        // Cantos em meia-esquadria: o vertice do canto anda tambem para fora
        // do lado vizinho, e os quatro lados fecham sem fresta.
        Vec2 along2 = s.along == 0 ? Vec2(1.0f, 0.0f) : Vec2(0.0f, 1.0f);
        const bool startCorner = std::abs(g.vertices[edge.front()][s.along] - alongLo) <= eps;
        const bool endCorner = std::abs(g.vertices[edge.back()][s.along] - alongHi) <= eps;

        ModelFileNode n;
        n.name = std::string(kEdgeApronPrefix) + s.tag;
        n.parentName = g.parentName;
        n.parentIndex = g.parentIndex;
        n.position = g.position;
        n.rotationAxis = g.rotationAxis;
        n.rotationAngle = g.rotationAngle;
        n.scale = g.scale;
        n.offsetMatrix = g.offsetMatrix;
        n.offset = g.offset;
        n.nodeMatrix = g.nodeMatrix;
        n.renderMatrix = g.renderMatrix;
        n.textureIds = g.textureIds;
        n.textureNames = g.textureNames;
        n.pbrTextureNames = g.pbrTextureNames;
        n.emissiveTextureNames = g.emissiveTextureNames;

        // Colunas por anel: a borda em resolucao cheia e, para fora, de 2^n
        // em 2^n com o passo perto de 1/4 da largura do anel. Com todas as
        // colunas os aneis de fora viravam tiras de 5 u por 2 km, mais finas
        // que um pixel, e o G-buffer subia 4,1 ms so' de quads desperdicados.
        // Cada linha e' subconjunto da anterior; o anel liga as duas em leque.
        const size_t cols = edge.size(), rings = dist.size();
        std::vector<std::vector<uint32_t>> rowCols(rings);
        std::vector<uint32_t> rowStart(rings);
        size_t stride = 1;
        for (size_t k = 0; k < rings; ++k) {
            if (k > 0) {
                const float want = (dist[k] - dist[k - 1]) / (4.0f * std::max(step, 1e-3f));
                while (float(stride * 2) <= want && stride * 2 < cols) stride *= 2;
            }
            rowCols[k] = apronRowColumns(cols, stride);
            rowStart[k] = uint32_t(n.vertices.size());
            const float d = dist[k];
            const size_t half = size_t(0.25f * d / std::max(step, 1e-3f));
            for (const uint32_t i : rowCols[k]) {
                const uint32_t v = edge[i];
                Vec2 off = s.out;
                if (i == 0 && startCorner) off -= along2;
                if (i + 1 == cols && endCorner) off += along2;
                const Vec3 b = g.vertices[v];
                // Janela simetrica: encolhe ate' zero no canto, onde os dois
                // lados dividem o vertice da esquadria e precisam da mesma altura.
                const size_t w = std::min({half, size_t(i), cols - 1 - i});
                const size_t i0 = i - w, i1 = i + w + 1;
                const float smooth = float((prefix[i1] - prefix[i0]) / double(i1 - i0));
                const Vec3 p(b.x + off.x * d, smooth - prm.slope * d, b.z + off.y * d);
                n.vertices.push_back(k == 0 ? b : p);
                n.texCoords.push_back(planarUv ? Vec2(uvPlane * Vec3(p.x, p.z, 1.0f))
                                               : (g.texCoords.size() == count ? g.texCoords[v] : Vec2(0.0f)));
                apronCopyAttr(g.colors, count, v, n.colors);
                apronCopyAttr(g.perVertexTexIds, count, v, n.perVertexTexIds);
                apronCopyAttr(g.perVertexBlendTexIds, count, v, n.perVertexBlendTexIds);
                apronCopyAttr(g.perVertexBlendWeights, count, v, n.perVertexBlendWeights);
                apronCopyAttr(g.perVertexBlendMaskTexIds, count, v, n.perVertexBlendMaskTexIds);
                apronCopyAttr(g.perVertexBlendMaskTexIds2, count, v, n.perVertexBlendMaskTexIds2);
                for (int l = 0; l < 8; ++l)
                    apronCopyAttr(g.perVertexSplatTexIds[l], count, v, n.perVertexSplatTexIds[l]);
                apronCopyAttr(g.perVertexBlendMaskUV, count, v, n.perVertexBlendMaskUV);
                apronCopyAttr(g.perVertexEmissive, count, v, n.perVertexEmissive);
            }
        }
        // Vertice da coluna `i` na linha `k` (a coluna existe nas linhas de
        // dentro de k, pela regra de subconjunto).
        auto at = [&](size_t k, uint32_t i) {
            const auto& c = rowCols[k];
            return rowStart[k] + uint32_t(std::lower_bound(c.begin(), c.end(), i) - c.begin());
        };
        // Normal: a da borda no anel 0 (sem costura de luz); fora, pelas
        // vizinhas na linha e pela mesma coluna na linha de dentro.
        n.normals.resize(n.vertices.size(), Vec3(0.0f, 1.0f, 0.0f));
        for (size_t k = 0; k < rings; ++k)
            for (size_t j = 0; j < rowCols[k].size(); ++j) {
                const uint32_t i = rowCols[k][j], me = rowStart[k] + uint32_t(j);
                if (k == 0) {
                    if (g.normals.size() == count) n.normals[me] = g.normals[edge[i]];
                    continue;
                }
                const size_t last = rowCols[k].size() - 1;
                const Vec3 di = n.vertices[rowStart[k] + std::min(j + 1, last)] - n.vertices[rowStart[k] + (j > 0 ? j - 1 : 0)];
                const Vec3 dk = n.vertices[me] - n.vertices[at(k - 1, i)];
                Vec3 nn = glm::cross(di, dk);
                if (nn.y < 0.0f) nn = -nn;
                if (glm::dot(nn, nn) > 1e-12f) n.normals[me] = glm::normalize(nn);
            }
        for (size_t k = 1; k < rings; ++k)
            for (size_t j = 0; j + 1 < rowCols[k].size(); ++j)
                apronFan(n, rowCols[k - 1], rowStart[k - 1], rowCols[k][j], rowCols[k][j + 1], at(k, rowCols[k][j]),
                         at(k, rowCols[k][j + 1]), groundSign);
        for (const Vec3& p : n.vertices) {
            n.box.min = glm::min(n.box.min, p);
            n.box.max = glm::max(n.box.max, p);
        }
        EdgeApron info;
        info.node = n.name;
        for (int j = 0; j < 3; ++j) {
            const Vec3 b = g.vertices[edge[(cols - 1) * size_t(j + 1) / 4]];
            info.probe[j] = Vec3(b.x + s.out.x * 8.0f, b.y, b.z + s.out.y * 8.0f);
        }
        edgeAprons.push_back(info);
        totalVerts += n.vertices.size();
        if (groundIsRoot) model.rootNodeNames.push_back(n.name);
        model.nodes.push_back(std::move(n));
    }
    if (edgeAprons.empty()) return false;
    ERUPTION_LOG_WARN("MapLoader: edge_apron: %zu lados, %zu vertices, UV %s", edgeAprons.size(), totalVerts,
                      planarUv ? "plana" : "da borda");
    return true;
}

} // namespace eruption
