// Carga de mapa do Engine: loadMap (o caminho sincrono completo - terreno,
// modelos, sprites, agua, clima, spawn do jogador), a amostragem de altura do
// terreno usada pra decidir onde o jogador nasce, e setPlayerController.
// Saiu do Engine.cpp em 2026-09-04, na quebra do arquivo.
//
// O caminho ASSINCRONO (warp com a cena ja' viva) nao esta' aqui: quem faz o
// swap e drena a fila de upload e' o MapUpload.cpp.

#include "core/Engine.hpp"
#include "core/Platform.hpp"
#include "core/EngineInternal.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TelemetryExporter.hpp"
#include "utils/PbrTextureLoader.hpp"
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include "utils/ImageUtils.hpp"
#include "formats/MapLoader.hpp"
#include <future>
#include "formats/ModelDataConverter.hpp"
#include "scene/SceneEdits.hpp"
#include "renderer/WeatherTypes.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace eruption {

namespace {

// Texturas embutidas que um preload ja' pegou para decodificar: as familias de
// modelos repetem o mesmo nome de textura em varios arquivos, e cada uma so'
// precisa ser decodificada uma vez. Zera a cada troca de mapa.
std::mutex s_preloadClaimMutex;
std::unordered_set<std::string> s_preloadClaimed;

bool claimPreloadTexture(const std::string& name) {
    std::lock_guard<std::mutex> lock(s_preloadClaimMutex);
    return s_preloadClaimed.insert(name).second;
}

// Bake das texturas embutidas (albedo + PBR em bloco, o mesmo .etexpack dos
// modelos do mapa) lido do disco pelo preload, por arquivo .glb.
std::mutex s_preloadBakeMutex;
std::unordered_map<std::string, EmbeddedBakeResult> s_preloadBakes;

// PBR (MRAH-W + normal) sintetizado pelo preload a partir do albedo embutido,
// pela chave do albedo ('/' e sem barra inicial). A sintese no quadro era a
// maior parte dos 150-500 ms do primeiro modelo de cada familia de textura.
std::mutex s_preloadPbrMutex;
std::unordered_map<std::string, std::pair<PbrTextureData, PbrTextureData>> s_preloadPbr;

std::string pbrKey(std::string key) {
    std::replace(key.begin(), key.end(), '\\', '/');
    if (!key.empty() && key[0] == '/') key.erase(0, 1);
    return key;
}

} // namespace

static float sampleGlbTerrainHeight(const LoadedMap& map, float x, float z, float radius = 1.5f) {
    if (map.models.empty()) return 0.0f;
    bool hasTerrainNodes = false;
    for (const auto& model : map.models) {
        for (const auto& node : model.nodes) {
            if (node.name.size() >= 8 && node.name.substr(0, 8) == "terrain_") {
                hasTerrainNodes = true; break;
            }
        }
        if (hasTerrainNodes) break;
    }
    constexpr float INF = 1e9f; float minY = INF; float sumY = 0.0f; int count = 0; float radiusSq = radius * radius;
    for (const auto& model : map.models) {
        for (const auto& node : model.nodes) {
            // Nós que compartilham geometria não guardam vértices: a amostragem
            // usa a malha do dono com a matriz DESTE nó, que é exatamente o que
            // a cópia por instância produzia antes.
            const auto& geo = (node.geometryShareIndex >= 0 &&
                               node.geometryShareIndex < static_cast<int32_t>(model.nodes.size()))
                                  ? model.nodes[node.geometryShareIndex]
                                  : node;
            if (geo.vertices.empty()) continue;
            if (hasTerrainNodes && (node.name.size() < 8 || node.name.substr(0, 8) != "terrain_")) continue;
            const Mat4& m = node.renderMatrix;
            for (const auto& v : geo.vertices) {
                Vec4 world = m * Vec4(v, 1.0f);
                float dx = world.x - x; float dz = world.z - z;
                if (dx * dx + dz * dz <= radiusSq) {
                    minY = glm::min(minY, world.y); sumY += world.y; ++count;
                }
            }
        }
    }
    if (count == 0) return 0.0f;
    float avgY = sumY / count;
    if (minY < avgY - 10.0f) return avgY;
    return minY;
}

static float sampleGlbOrTerrainHeight(const LoadedMap& map, float x, float z) {
    // GLB maps that carry a companion .ter grid should use it for ground height:
    // it is fast, matches the authored terrain exactly, and is not confused by
    // nearby model geometry.  Fall back to vertex sampling for raw GLB maps
    // without a terrain file.
    if (map.isGlbMap && map.terrainFromFile) {
        return TerrainParser::getTerrainHeightAt(map.terrain, x, z);
    }
    return sampleGlbTerrainHeight(map, x, z, 1.5f);
}


void Engine::uploadVfxGround() {
    // Grade de altura do mapa aberto para a colisao das particulas (um valor
    // por vertice, a mesma interpolacao de getTerrainHeightAt).
    if (!m_currentMap || !m_currentMap->terrainFromFile) {
        m_vfx.setGroundHeights({}, 0, 0, Vec2(0.0f), 0.0f);
        return;
    }
    const TerrainFile& t = m_currentMap->terrain;
    const uint32_t w = t.width, d = t.height;
    if (w == 0 || d == 0 || t.cubes.size() < size_t(w) * d) return;
    const uint32_t vw = w + 1, vd = d + 1;
    std::vector<float> h(size_t(vw) * vd);
    for (uint32_t z = 0; z < vd; ++z)
        for (uint32_t x = 0; x < vw; ++x) {
            const uint32_t cx = std::min(x, w - 1), cz = std::min(z, d - 1);
            const int corner = (x == w ? 1 : 0) + (z == d ? 2 : 0); // SW, SE, NW, NE
            h[size_t(z) * vw + x] = -t.cubes[size_t(cz) * w + cx].height[corner];
        }
    m_vfx.setGroundHeights(h, vw, vd, Vec2(t.offsetX, t.offsetZ), t.scale > 0.0f ? t.scale : 10.0f);
}

float Engine::sampleTerrainHeight(float x, float z) const {
    const LoadedMap* map = mapAt(x, z); // o aberto ou um vizinho em cena
    if (!map) return 0.0f;
    return sampleGlbOrTerrainHeight(*map, x, z);
}

bool Engine::raycastTerrain(const Vec3& origin, const Vec3& dir, float maxT, float& hitT) const {
    if (!m_currentMap) return false;
    // O chão mais perto entre o mapa aberto e os vizinhos em cena.
    bool hit = false;
    auto test = [&](const TerrainFile& ter) {
        float t = 0.0f;
        if (raycastGrid(ter, origin, dir, hit ? hitT : maxT, t) && (!hit || t < hitT)) {
            hitT = t;
            hit = true;
        }
    };
    if (m_currentMap->terrainFromFile) test(m_currentMap->terrain);
    for (const auto& r : m_regions)
        if (r.state == WorldRegion::State::Shown && r.data) test(r.data->terrain);
    return hit;
}

bool Engine::raycastGrid(const TerrainFile& ter, const Vec3& origin, const Vec3& dir, float maxT, float& hitT) {
    // Passo de meia célula: não atravessa morro fino; bisseção refina o cruzamento.
    const float step = std::max(0.5f, ter.scale * 0.5f);
    auto above = [&](float t) {
        const Vec3 p = origin + dir * t;
        return p.y - TerrainParser::getTerrainHeightAt(ter, p.x, p.z);
    };
    float t0 = 0.0f, d0 = above(0.0f);
    if (d0 < 0.0f) return false; // começa abaixo do chão
    for (float t1 = step; t1 <= maxT; t1 += step) {
        const float d1 = above(t1);
        if (d1 <= 0.0f) {
            for (int i = 0; i < 24; ++i) {
                const float tm = 0.5f * (t0 + t1);
                if (above(tm) > 0.0f) t0 = tm; else t1 = tm;
            }
            hitT = t1;
            return true;
        }
        t0 = t1;
        d0 = d1;
    }
    return false;
}

void Engine::setPlayerController(PlayerController* pc) {
    m_playerController = pc;
    if (!m_playerController) return;

    if (!m_currentMap) {
        // Map not ready yet (background warp pending). Park the player below the
        // world so the sprite/camera don't render in the void while loading.
        m_playerController->setPos(kSafePreWarpPlayerPos);
        m_camera.setOrbitTarget(kSafePreWarpPlayerPos);
        m_playerSpawnedOnActiveMap = false;
        ERUPTION_LOG_INFO("[Engine] Player controller set before map load; parked at safe position (%.1f, %.1f, %.1f)",
                          kSafePreWarpPlayerPos.x, kSafePreWarpPlayerPos.y, kSafePreWarpPlayerPos.z);
    } else {
        m_playerSpawnedOnActiveMap = true;
    }
}

// ERUPTION_TEST_LOAD_PHASES=1 (bancada): duracao de cada fase do loadMap no
// log ("[LOADPHASE] fase ms"), para achar o que trava a troca de mapa.
static void loadPhase(const char* fase) {
    static const bool kOn = std::getenv("ERUPTION_TEST_LOAD_PHASES") != nullptr;
    if (!kOn) return;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    ERUPTION_LOG_WARN("[LOADPHASE] %-20s %7.1f ms", fase, std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
}

bool Engine::loadMap(const std::string& name, std::shared_ptr<LoadedMap> preloaded) {
    loadPhase("inicio");
    m_fsrReset = true; // historico temporal do FSR nao vale entre mapas
    m_isInitialBoot = false; // No longer the initial boot
    TelemetryExporter::recordLoadStart(name);
    loadMapClimate(name);
    m_vulkan.waitIdle();
    loadPhase("clima+waitIdle");
    clearCharacters(); // os do mapa novo vêm do scene.luau dele
    m_scriptFog = MapFog{}; // world.set_fog e' do mapa que o pediu
    clearPlayerStart();
    endSplatEditing();
    clearScatter();
    resetWorld(); // vizinhos do mapa anterior
    clearAreas();
    clearRibbons();
    clearSoundEmitters();
    m_solid.clear();
    loadPhase("limpeza");

    // GERACAO DO CACHE DE ASSET. Ate 2026-09-05 so' o warp (o antigo performMapSwap)
    // avancava a geracao e expulsava textura/malha velha do GlobalAssetCache;
    // a carga fria repetida nunca chamava nada disto, entao currentWarpId
    // ficava parado, a expiracao (Engine.cpp, lastSeenWarpId + maxAge) nunca
    // disparava e cada mapa carregado no mesmo processo deixava VkImage +
    // VmaAllocation + slot bindless presos para sempre - com 4096 slots
    // (MAX_BINDLESS_TEXTURES), o esgotamento devolve slot 0 e tudo vira a
    // textura errada, sem crash. Mesma semantica do warp: o que o mapa novo
    // reusar renova a idade; o que ficar 3 cargas sem uso sai.
    m_assetCache.incrementWarp();
    m_assetCache.cleanupOldTextures(&m_vulkan, &m_bindless, 3);
    // Modelos pedidos para outro mapa e nao usados em duas trocas saem.
    ++m_modelPreloadGen;
    for (auto it = m_modelPreloads.begin(); it != m_modelPreloads.end();)
        it = m_modelPreloadGen - it->second.gen > 2 ? m_modelPreloads.erase(it) : std::next(it);
    {
        std::lock_guard<std::mutex> lock(s_preloadClaimMutex);
        s_preloadClaimed.clear();
    }
    {
        // Bake lido por um preload que saiu acima (ou cujo modelo nunca veio).
        std::lock_guard<std::mutex> lock(s_preloadBakeMutex);
        for (auto it = s_preloadBakes.begin(); it != s_preloadBakes.end();)
            it = m_modelPreloads.count(it->first) ? std::next(it) : s_preloadBakes.erase(it);
    }
    {
        // PBR pre-sintetizado de um mapa anterior: o que foi usado ja' esta' na
        // GPU; o que nao foi, nao volta (o modelo dele saiu acima).
        std::unordered_set<std::string> keep;
        for (const auto& [path, pre] : m_modelPreloads) {
            if (pre.model.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            if (const auto& m = pre.model.get())
                for (const auto& et : m->embeddedTextures) keep.insert(pbrKey(et.name));
        }
        std::lock_guard<std::mutex> lock(s_preloadPbrMutex);
        for (auto it = s_preloadPbr.begin(); it != s_preloadPbr.end();)
            it = keep.count(it->first) ? std::next(it) : s_preloadPbr.erase(it);
    }

    // Independent clouds belong to the previous map: drop them so stale world
    // bounds / layers don't leak into the newly loaded map. The weather field
    // respawns for the new map on the next update tick.
    clearIndependentClouds();
    m_weatherFieldType = -1;

    presentLoadingScreen(name, "Loading map data", 0.1f);
    loadPhase("cache+nuvens");

    // MapLoader accepts either a plain map name (resolved to assets/external/<name>/<name>.glb)
    // or an explicit file path ending in .glb/.map.
    auto loadedMap = preloaded ? std::move(preloaded) : m_mapLoader->loadMap(name);
    if (!loadedMap) {
        ERUPTION_LOG_ERROR("Failed to load map: %s", name.c_str());
        return false;
    }
    
    m_currentMap = loadedMap; 
    m_currentMapName = name;
    m_vfx.clear(); // efeitos pertencem ao mapa que saiu

    // Configure water mesh from map data
    m_water.clearWaterMesh();
    if (loadedMap) {
        float wl, wh;
        bool fromTerrain = getMapWaterParams(*loadedMap, wl, wh, m_waterMenu.config.forceWater);
        auto wmesh = TerrainParser::generateWaterMesh(loadedMap->terrain, 0, 0,
                                                  loadedMap->terrain.width, loadedMap->terrain.height,
                                                  wl, wh, m_waterMenu.config.forceWater, !fromTerrain);
        m_mapBaseWaterLevel = wl;
        m_waterOn = !wmesh.vertices.empty();
        m_water.setWaterMesh(wmesh);
        cacheWaterBounds(wmesh);
        // MAPA SEM AGUA NAO TEM NIVEL DE AGUA. getMapWaterParams devolve 0
        // quando nao ha plano (e' a sentinela de "desligado" da geracao da
        // malha), e esse 0 seguia como nivel do mapa: o minimapa pintava de
        // agua azul tudo o que ficava abaixo de y=0 - numa caverna que desce
        // a -60, metade do mapa virava lago. Sem plano e sem malha, o nivel
        // fica bem abaixo de qualquer chao. Depois do cacheWaterBounds de
        // proposito: la' ele marcaria a agua (que nao existe) como soterrada.
        if (!fromTerrain && wmesh.vertices.empty()) m_mapBaseWaterLevel = -1.0e6f;
        syncLiquidMenuFromMap(loadedMap.get());
        setupLavaLiquid(loadedMap.get());
        ERUPTION_LOG_INFO("Water mesh: %u tiles (baseLevel=%.2f, offset=%.2f, src=%s)",
                         wmesh.waterTileCount, m_mapBaseWaterLevel, m_waterMenu.config.waterLevel, fromTerrain ? "terrain data" : "world data");
    }

    presentLoadingScreen(name, "Building terrain", 0.4f);
    loadPhase("leitura+agua");
    if (!loadedMap->isGlbMap) {
        m_terrainRenderer.buildChunks(loadedMap->terrain, &m_packManager);
    } else {
        m_terrainRenderer.clear();
        ERUPTION_LOG_INFO("Skipping terrain renderer build for GLB map '%s'", name.c_str());
    }

    presentLoadingScreen(name, "Placing models", 0.7f);
    loadPhase("terreno");
    if (loadedMap->isGlbMap) {
        // Modern GLB maps use the companion terrain file only for spawn/height reference.
        // Center the player on the authored terrain bounds, honoring offset + scale.
        m_centerX = loadedMap->terrain.offsetX + (static_cast<float>(loadedMap->terrain.width) * loadedMap->terrain.scale) * 0.5f;
        m_centerZ = loadedMap->terrain.offsetZ + (static_cast<float>(loadedMap->terrain.height) * loadedMap->terrain.scale) * 0.5f;
    } else {
        m_centerX = (float)loadedMap->terrain.width * 5.0f;
        m_centerZ = (float)loadedMap->terrain.height * 5.0f;
    }

    // Fit cloud coverage bounds to the actual map size.
    {
        Vec3 worldMin(0.0f, 0.0f, 0.0f);
        Vec3 worldMax(m_centerX * 2.0f, 2000.0f, m_centerZ * 2.0f);
        m_cloudLayerRenderer.coverageArray().setWorldBounds(worldMin, worldMax);
    }

    std::vector<ModelAsset> genericModels;
    genericModels.reserve(loadedMap->models.size());
    // A malha lida do GLB sai daqui convertida e é liberada no caminho:
    // depois da conversão o mapa só guarda as texturas embutidas e o caminho.
    for (auto& model : loadedMap->models) {
        genericModels.push_back(convertModelToAssetReleasing(model));
    }

    std::vector<ModelInstanceDesc> genericInstances;
    for (const auto& m : loadedMap->models) {
        ModelInstanceDesc d;
        d.modelPath = m.filePath;
        d.scale = Vec3(1.0f);
        d.rotation = Vec3(0.0f);
        genericInstances.push_back(d);
    }

    if (name.find("medieval_village") != std::string::npos) {
        for (auto& mod : loadedMap->models) {
            mod.box.min *= 5.0f;
            mod.box.max *= 5.0f;
        }
        for (auto& mod : genericModels) {
            for (auto& node : mod.nodes) {
                for (auto& v : node.vertices) {
                    v.position *= 5.0f;
                }
                node.aabbMin *= 5.0f;
                node.aabbMax *= 5.0f;
                node.position *= 5.0f;
                node.renderMatrix[3][0] *= 5.0f;
                node.renderMatrix[3][1] *= 5.0f;
                node.renderMatrix[3][2] *= 5.0f;
            }
            mod.boxMin *= 5.0f;
            mod.boxMax *= 5.0f;
        }
    }
    // Retoque de terreno salvo pelo editor: entra nos vértices antes de subir.
    prepareHeightEdits(*loadedMap, genericModels);
    loadPhase("conversao modelos");
    applySavedWalk(*loadedMap);
    ERUPTION_LOG_WARN("Engine::loadMap: loading %zu models, %zu instances", genericModels.size(), genericInstances.size());
    // GLB maps are authored in world space; do not add a center offset.
    float modelCenterX = loadedMap->isGlbMap ? 0.0f : m_centerX;
    float modelCenterZ = loadedMap->isGlbMap ? 0.0f : m_centerZ;
    // ------------------------------------------------------------------
    // Texturas EMBUTIDAS no GLB -> compressao de bloco (BC) com mips prontos.
    //
    // Este era o buraco da Fase 5: o .etex ja' cobria texturas de DISCO, mas
    // mapas GLB (cidade-A, parana_field) trazem TODAS as texturas dentro do
    // container - 164 PNGs / 137 Mpx no parana - e elas subiam RGBA8, ×3 com
    // os mapas PBR sintetizados. O bake abaixo comprime na primeira carga e
    // cacheia em disco (<glb>.etexpack, invalidado por tamanho+mtime do GLB,
    // teto de resolucao e conjunto de formatos da GPU).
    //
    // ACESSIBILIDADE: o cache e' artefato DERIVADO. Sem ele, ou numa GPU sem
    // BC, m_embeddedBaked fica vazio e o caminho RGBA8 abaixo roda igual.
    // ------------------------------------------------------------------
    m_embeddedBaked.clear();
    loadPhase("geometria/costuras");
    if (m_currentMap) {
        for (auto& model : m_currentMap->models) {
            if (model.embeddedTextures.empty()) continue;
            EmbeddedBakeResult bake = bakeEmbeddedTextures(
                model, static_cast<uint32_t>(m_textureMaxSize), pbrSynthesisEnabled());
            // OVERRIDE do .env tem prioridade sobre a media automatica. A
            // media e' tirada de TODAS as texturas embutidas do GLB (no
            // parana_field sao 188: arvore, folha, rocha, lava), entao ela
            // converge para um cinza quase neutro e o lado sombreado de tudo
            // recebe cinza - a maior causa isolada de "cara de 2003" segundo o
            // autor. Um latossolo roxo quer quique laranja saturado, e isso a
            // media nunca vai dar.
            const Vec3& ovr = m_currentMap ? m_currentMap->env.groundAlbedoOverride
                                           : Vec3(-1.0f);
            if (ovr.r >= 0.0f && ovr.g >= 0.0f && ovr.b >= 0.0f) {
                float lum = glm::max(0.2126f * ovr.r + 0.7152f * ovr.g + 0.0722f * ovr.b, 1e-3f);
                m_mapGroundAlbedo = ovr * (0.16f / lum);
                ERUPTION_LOG_WARN("Engine::loadMap: ground albedo do .env = (%.3f, %.3f, %.3f)",
                                  m_mapGroundAlbedo.r, m_mapGroundAlbedo.g, m_mapGroundAlbedo.b);
            } else if (bake.hasGroundAlbedo) {
                Vec3 avg(bake.groundAlbedo[0], bake.groundAlbedo[1], bake.groundAlbedo[2]);
                float lum = glm::max(0.2126f * avg.r + 0.7152f * avg.g + 0.0722f * avg.b, 1e-3f);
                m_mapGroundAlbedo = avg * (0.16f / lum);
                ERUPTION_LOG_WARN("Engine::loadMap: map ground albedo = (%.3f, %.3f, %.3f)",
                                  m_mapGroundAlbedo.r, m_mapGroundAlbedo.g, m_mapGroundAlbedo.b);
            }
            for (auto& [name, tex] : bake.textures) {
                m_embeddedBaked.emplace(name, std::move(tex));
            }
        }
    }

    // FAST PARALLEL DECODE of all embedded GLB textures
    // (so' as que o bake NAO cobriu - fallback puro RGBA8)
    std::vector<EmbeddedTexture*> allEmbedded;
    if (m_currentMap) {
        for (auto& model : m_currentMap->models) {
            for (auto& et : model.embeddedTextures) {
                if (m_embeddedBaked.count(et.name)) continue;
                if (!et.isDecoded() && et.hasEncodedData()) allEmbedded.push_back(&et);
            }
        }
    }
    const int EMBEDDED_TEXTURE_MAX_SIZE = m_textureMaxSize;
    if (!allEmbedded.empty()) {
        ERUPTION_LOG_WARN("Engine::loadMap: Parallel decoding %zu embedded textures...", allEmbedded.size());
        size_t downscaled = 0;
        std::mutex statMutex;
        eruption::JobSystem::instance().parallelFor(allEmbedded.size(), [&](uint32_t i) {
            EmbeddedTexture* et = allEmbedded[i];
            ImageData img = ImageUtils::loadFromMemoryRaw(et->encodedData.data(), et->encodedData.size());
            if (img.isValid()) {
                if (ImageUtils::downscaleRGBA(img.width, img.height, img.pixels, EMBEDDED_TEXTURE_MAX_SIZE)) {
                    std::lock_guard<std::mutex> lock(statMutex);
                    ++downscaled;
                }
                et->pixels = std::move(img.pixels);
                et->width = img.width;
                et->height = img.height;
                et->channels = img.channels;
            }
        });
        ERUPTION_LOG_WARN("Engine::loadMap: Embedded textures downscaled: %zu / %zu", downscaled, allEmbedded.size());

        // Average albedo of the map's textures -> real per-map bounce/ambient
        // ground tint (vila-A bounces green-ish grass light, parana red laterite).
        {
            double r = 0, g = 0, b = 0, n = 0;
            for (const EmbeddedTexture* et : allEmbedded) {
                if (!et->isDecoded() || et->channels < 3) continue;
                const int stride = et->channels;
                const size_t px = static_cast<size_t>(et->width) * et->height;
                for (size_t i = 0; i < px; i += 97) { // sparse sample
                    const uint8_t* p = &et->pixels[i * stride];
                    // Skip magenta colour-key and near-black borders.
                    if (p[0] > 230 && p[1] < 25 && p[2] > 230) continue;
                    if (p[0] + p[1] + p[2] < 24) continue;
                    r += p[0]; g += p[1]; b += p[2]; n += 1;
                }
            }
            if (n > 100) {
                Vec3 avg(static_cast<float>(r / n) / 255.0f,
                         static_cast<float>(g / n) / 255.0f,
                         static_cast<float>(b / n) / 255.0f);
                // Normalise to a bounce-appropriate luminance so a bright map
                // doesn't blow out the indirect term and a dark one doesn't kill it.
                float lum = glm::max(0.2126f * avg.r + 0.7152f * avg.g + 0.0722f * avg.b, 1e-3f);
                m_mapGroundAlbedo = avg * (0.16f / lum);
                ERUPTION_LOG_WARN("Engine::loadMap: map ground albedo = (%.3f, %.3f, %.3f)",
                                  m_mapGroundAlbedo.r, m_mapGroundAlbedo.g, m_mapGroundAlbedo.b);
            } else {
                m_mapGroundAlbedo = Vec3(0.08f, 0.06f, 0.04f);
            }
        }
    }

    // Decode all cooked PBR maps (_mrahw/_normal PNGs) in parallel before the
    // serial mesh loop. This was the bulk of map-load time.
    {
        auto _t = std::chrono::steady_clock::now();
        std::vector<std::string> pbrNames;
        for (const auto& m : genericModels) {
            for (const auto& node : m.nodes) {
                for (const auto& n : node.pbrTextureNames) if (!n.empty()) pbrNames.push_back(n);
                for (const auto& n : node.textureNames)    if (!n.empty()) pbrNames.push_back(n);
            }
        }
        eruption::prewarmPbrTextureData(pbrNames);
        ERUPTION_LOG_WARN("Engine::loadMap: PBR prewarm (%zu names) took %lld ms", pbrNames.size(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _t).count());
    }

    loadPhase("texturas embutidas");
    auto _tLMM = std::chrono::steady_clock::now();
    // Cópia do chão na CPU: pincel do editor, ou pintura salva que o jogo
    // aplica nos vértices (mapas sem splat próprio).
    m_modelRenderer.setKeepGroundVertices(m_editorMode || hasSavedSplat(name) ||
                                          SceneEdits::fileMentions(name, "terrain") ||
                                          SceneEdits::fileMentions(name, "neighbors")); // bordas costuradas
    m_modelRenderer.resetResolveStats();
    m_modelRenderer.loadMapModels(std::move(genericModels), genericInstances, modelCenterX, modelCenterZ);
    {
        const auto& rs = m_modelRenderer.resolveStats();
        ERUPTION_LOG_WARN("Engine::loadMap: texturas novas %u em %.0f ms, PBR %u em %.0f ms", rs.tex, rs.texMs, rs.pbr,
                          rs.pbrMs);
    }
    eruption::clearPbrTextureDataPrewarm(); // free any pre-decoded PBR data not consumed
    ERUPTION_LOG_WARN("Engine::loadMap: loadMapModels took %lld ms",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _tLMM).count());

    flushPendingUploads();
    if (!m_modelRenderer.groundMeshes().empty()) bindHeightEditToMeshes();
    loadPhase("loadMapModels");
    if (hasSavedSplat(name)) beginSplatEditing(); // máscaras pintadas no editor


    ERUPTION_LOG_WARN("Engine::loadMap: ModelRenderer has %zu meshes, %zu instances, "
                      "%llu triangulos carregados (malha base x instancia, pre-cull/LOD)",
                      m_modelRenderer.meshCount(), m_modelRenderer.instanceCount(),
                      static_cast<unsigned long long>(m_modelRenderer.loadedTriangles()));
    
    presentLoadingScreen(name, "Finalizing", 0.95f);
    loadPhase("uploads+splat");
    if (loadedMap) {
        // OFFSET DO CENTRO SO' EM MAPA LEGADO. As luzes de um mapa legado vem em
        // coordenada RELATIVA ao centro do mapa; as de um GLB (o .env ao lado)
        // vem em coordenada de MUNDO ja' pronta. Somar o centro nas duas fazia
        // as luzes do GLB irem parar FORA do mapa: no parana_field, as 12 luzes
        // de lava nasciam em (2807,2739) e eram jogadas para (4807,4739), num
        // mapa de 4000x4000 - nenhuma delas iluminava nada, e o que restava era
        // a luz presa ao jogador (autor 2026-09-06: "a luz dinamica ta muito
        // fraca"). Medido: intensidade de 0 a 27 nao mudava UM pixel.
        const Vec3 lightOrigin = loadedMap->isGlbMap ? Vec3(0.0f)
                                                 : Vec3(m_centerX, 0.0f, m_centerZ);
        std::vector<PointLight> lights;
        lights.reserve(loadedMap->lights.size());
        for (const auto& ml : loadedMap->lights) {
            PointLight pl;
            pl.position = ml.position + lightOrigin;
            pl.color = ml.color;
            pl.intensity = ml.intensity;
            pl.radius = ml.range;
            pl.animType = ml.animType;
            pl.nightOnly = ml.nightOnly;
            pl.enabled = !ml.nightOnly;
            pl.spotDir = ml.spotDir;
            pl.spotCos = std::cos(glm::radians(glm::clamp(ml.spotAngle, 1.0f, 179.0f) * 0.5f));
            if (std::getenv("ERUPTION_DEBUG_LAVA_LIGHT"))
                ERUPTION_LOG_WARN("[PLIGHT] fonte (%.0f,%.0f,%.0f) -> mundo (%.0f,%.0f,%.0f) centro (%.0f,%.0f) r=%.0f i=%.2f",
                                  ml.position.x, ml.position.y, ml.position.z,
                                  pl.position.x, pl.position.y, pl.position.z,
                                  m_centerX, m_centerZ, pl.radius, pl.intensity);
            lights.push_back(pl);
        }
        appendArtificialTestLights(m_currentMapName, m_centerX, m_centerZ, lights);
        m_deferredLighting.setPointLights(lights);
    } else {
        m_deferredLighting.setPointLights({});
    }
    // ONDE O JOGADOR NASCE.
    //
    // O centro do mapa e' um PALPITE, nao um lugar: ele so' esta' certo quando
    // o mapa por acaso tem chao no meio. O gerador ja' grava `spawn = {x,y,z}`
    // na cena (etapa "spawn" da receita) e o SceneEdits ja' le' esse campo
    // para uma struct — mas ninguem o consumia na carga do mapa, entao o
    // palpite valia sempre. Num mapa cujo miolo e' um lago de lava o
    // personagem nascia TRINTA UNIDADES ABAIXO da superficie da lava.
    //
    // A cena manda; o centro fica como reserva para mapa que nao diz nada.
    Vec3 spawnPos = Vec3(m_centerX, 0.0f, m_centerZ);
    bool spawnDaCena = false;
    {
        SceneEdits cena;
        std::string erroCena;
        if (cena.load(SceneEdits::pathFor(m_currentMapName), erroCena) && cena.hasSpawn) {
            spawnPos = cena.spawn;
            spawnDaCena = true;
        }
    }
    if (loadedMap) {
        if (loadedMap->isGlbMap) {
            // GLB maps are authored in world space. Spawn at the map center and
            // Snap spawn Y to the companion terrain grid when available, otherwise
            // fall back to sampling the authored terrain geometry.
            // GLB maps have precise authored ground; keep the character exactly on
            // the surface instead of floating 0.5 units above it.
            spawnPos.y = clampSpawnYToWater(*loadedMap,
                                            sampleGlbOrTerrainHeight(*loadedMap, spawnPos.x, spawnPos.z));
        } else {
            spawnPos.y = clampSpawnYToWater(*loadedMap,
                                            TerrainParser::getTerrainHeightAt(loadedMap->terrain, spawnPos.x, spawnPos.z));
        }
    }
    if (spawnDaCena) {
        ERUPTION_LOG_WARN("Spawn from scene file: (%.1f, %.1f, %.1f)", spawnPos.x, spawnPos.y, spawnPos.z);
    }
    // Default orbit target sits slightly above the ground so the camera looks
    // toward the horizon rather than through the floor.
    m_camera.setOrbitTarget(spawnPos + Vec3(0.0f, 7.2f, 0.0f));
    m_playerController->setPos(spawnPos);
    LightingEnvironment env;
    env.sun.direction = loadedMap->env.sunDirection;
    if (glm::length(env.sun.direction) < 0.001f) env.sun.direction = glm::normalize(Vec3(-0.5f, -1.0f, -0.3f));
    env.sun.color = loadedMap->env.sunColor;
    env.sun.intensity = loadedMap->env.sunIntensity;
    env.ambientColor = loadedMap->env.ambientColor;
    env.ambientIntensity = loadedMap->env.ambientIntensity;
    m_mapAmbientAuthored = loadedMap->env.ambientAuthored;
    m_mapAmbientColor = loadedMap->env.ambientColor;
    m_mapAmbientIntensity = loadedMap->env.ambientIntensity;
    m_mapLightAuthored = loadedMap->env.lightAuthored;
    m_mapHasSky = loadedMap->env.hasSky;
    if (!m_mapHasSky) ERUPTION_LOG_WARN("MapLoad: mapa de interior, ceu desligado pelo .env");
    m_mapSunDirection = loadedMap->env.sunDirection;
    m_mapSunColor = loadedMap->env.sunColor;
    m_mapSunIntensity = loadedMap->env.sunIntensity;
    if (m_mapAmbientAuthored) {
        ERUPTION_LOG_WARN("MapLoad: ambiente do .env = (%.3f, %.3f, %.3f), vence o ciclo dia/noite",
                          m_mapAmbientColor.r, m_mapAmbientColor.g, m_mapAmbientColor.b);
    }
    m_deferredLighting.setEnvironment(env);

    if (m_hasInitialPos) {
        Vec3 initialPos = m_initialPos;
        if (loadedMap) {
            if (loadedMap->isGlbMap) {
                // GLB maps keep authored world-space X/Z; snap Y to the companion
                // terrain grid when available, otherwise sample authored geometry.
                // Keep the character on the authored GLB ground surface.
                initialPos.y = clampSpawnYToWater(*loadedMap,
                                                  sampleGlbOrTerrainHeight(*loadedMap, initialPos.x, initialPos.z));
            } else {
                initialPos.y = clampSpawnYToWater(*loadedMap,
                                                  TerrainParser::getTerrainHeightAt(loadedMap->terrain, initialPos.x, initialPos.z));
            }
        }
        m_playerController->setPos(initialPos);
        m_camera.setOrbitTarget(m_playerController->pos());
    }

    if (m_hasInitialCamera) {
        m_camera.setDefaultOrbit(m_initialYaw, m_initialPitch, m_initialDist);
        m_camera.setOrbit(m_initialYaw, m_initialPitch, m_initialDist);
    } else {
        // Enquadramento isometrico padrao (math/Camera.hpp).
        m_camera.setDefaultOrbit(glm::radians(camera_default::kYawDeg), glm::radians(camera_default::kPitchDeg),
                                 camera_default::kDistance);
        m_camera.setOrbit(glm::radians(camera_default::kYawDeg), glm::radians(camera_default::kPitchDeg),
                          camera_default::kDistance);
    }
    m_playerController->setYaw(glm::pi<float>());
    m_playerSpawnedOnActiveMap = true;

    m_vulkan.waitIdle();
    loadPhase("spawn+ambiente+waitIdle");

    ERUPTION_LOG_WARN("Player spawned at (%.2f, %.2f, %.2f)",
                    m_playerController->pos().x,
                    m_playerController->pos().y,
                    m_playerController->pos().z);

    // Optional debug auto-spawn (off by default; enable with --auto-cloud).
    {
        CloudLayerRenderer::Config cfg = m_cloudLayerRenderer.config();
        cfg.enabled = true;
        m_cloudLayersEnabled = true;
        m_cloudLayerRenderer.setConfig(cfg);
        m_cloudLayerRenderer.coverageArray().setLayerCount(cfg.layerCount);
    }

    loadPhase("nuvens");
    // PROBES DE IRRADIANCIA (G38): bake 2.5D da visibilidade do ceu sobre o
    // (NO FIM do loadMap, depois de TODOS os uploads e do waitIdle: os buffers
    // base das malhas ainda nao estavam na GPU no ponto anterior - so' as
    // malhas com LOD, enviadas na hora, rasterizavam; chao e modelos legados
    // saiam vazios dos heightfields.)
    // AABB do terreno (+ margem vertical para copas/telhados). Terreno e
    // modelos ja' estao na GPU; o pipeline de sombra desenha os tres mapas de
    // profundidade. Sem terreno (mapa vazio) fica o fallback "ceu aberto".
    {
        Vec3 mn(1e30f), mx(-1e30f);
        for (const auto& ch : m_terrainRenderer.chunks()) {
            mn = glm::min(mn, ch.aabbMin);
            mx = glm::max(mx, ch.aabbMax);
        }
        // Mapa GLB: o terreno e' modelo - o AABB vem das instancias.
        Vec3 imn, imx;
        if (m_modelRenderer.sceneBounds(imn, imx)) {
            mn = glm::min(mn, imn);
            mx = glm::max(mx, imx);
        }
        const bool haveBounds = mx.x > mn.x && mx.z > mn.z;
        loadPhase("probes: chave");
        if (haveBounds && m_irradianceProbesEnabled) {
            mx.y += 40.0f;
            mn.y -= 20.0f;
            if (m_irradianceProbes.bake(m_terrainRenderer, m_modelRenderer,
                                        m_shadowRenderer.shadowPipeline(), m_shadowRenderer.shadowLayout(),
                                        m_shadowRenderer.shadowInstPipeline(), m_shadowRenderer.shadowInstLayout(),
                                        &m_bindless, mn, mx)) {
                m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                                       m_irradianceProbes.gridMin(), m_irradianceProbes.gridInvExtent(), true);
            }
        } else {
            m_irradianceProbes.clear();
            m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                                   Vec3(0.0f), Vec3(0.0f), false);
        }
    }

    loadPhase("probes: bake");
    TelemetryExporter::recordLoadEnd(name);
    // Estado do cache cross-mapa depois desta carga. E' o detector do
    // vazamento que existia ate 2026-09-05 (carga fria nunca expulsava nada):
    // em loop de troca de mapa, model/terrain/meshes e usedSlots tem que
    // estabilizar a partir da 4a carga; highWater e' a marca d'agua dos 4096.
    {
        const auto cs = m_assetCache.getStats();
        ERUPTION_LOG_WARN("[ASSETCACHE] gen=%u modelTex=%zu terrainTex=%zu meshes=%zu bindless_used=%u highWater=%u/%u",
                          m_assetCache.currentWarpId, cs.modelTextureCount, cs.terrainTextureCount, cs.meshCount,
                          m_bindless.usedSlots(), m_bindless.highWater(), MAX_BINDLESS_TEXTURES);
    }
    // ERUPTION_TEST_VFX="efeito@x,y,z;efeito2@x,y,z" (teste): toca efeitos
    // ao abrir o mapa, para bancada de imagem e de desempenho.
    // Efeitos do mapa (.env "effects"). Os efeitos sao descritos em metros;
    // o mapa diz quantas unidades do mundo tem um metro (padrao do motor:
    // kVfxUnitsPerMeter, medido no tamanho dos personagens e das arvores).
    m_vfx.setUnitsPerMeter(m_currentMap->vfxUnitsPerMeter > 0.0f ? m_currentMap->vfxUnitsPerMeter : kVfxUnitsPerMeter);
    uploadVfxGround();
    if (const char* u = std::getenv("ERUPTION_TEST_VFX_UNITS")) m_vfx.setUnitsPerMeter(std::strtof(u, nullptr));
    // ERUPTION_TEST_NO_VFX=1 (teste): mapa sem os efeitos do .env (imagens
    // de referencia anteriores aos efeitos continuam comparaveis).
    static const bool kNoMapVfx = std::getenv("ERUPTION_TEST_NO_VFX") != nullptr;
    m_nightEffects.clear();
    for (const auto& fx : m_currentMap->effects) {
        if (!fx.enabled || kNoMapVfx) continue;
        Mat4 t = glm::translate(Mat4(1.0f), fx.position);
        t = glm::rotate(t, glm::radians(fx.rotationDeg.y), Vec3(0, 1, 0));
        t = glm::rotate(t, glm::radians(fx.rotationDeg.x), Vec3(1, 0, 0));
        t = glm::rotate(t, glm::radians(fx.rotationDeg.z), Vec3(0, 0, 1));
        t = glm::scale(t, Vec3(fx.scale));
        if (fx.nightOnly) {
            m_nightEffects.push_back({fx.name, t});
            continue;
        }
        if (!m_vfx.spawn(fx.name, t)) ERUPTION_LOG_WARN("[VFX] efeito do mapa '%s' nao tocou", fx.name.c_str());
    }
    if (const char* tv = std::getenv("ERUPTION_TEST_VFX")) {
        std::string all = tv;
        size_t start = 0;
        while (start < all.size()) {
            size_t end = all.find(';', start);
            if (end == std::string::npos) end = all.size();
            const std::string item = all.substr(start, end - start);
            const size_t at = item.find('@');
            float x = 0, y = 0, z = 0;
            if (at != std::string::npos && std::sscanf(item.c_str() + at + 1, "%f,%f,%f", &x, &y, &z) == 3) {
                const uint32_t id = m_vfx.spawn(item.substr(0, at), glm::translate(Mat4(1.0f), Vec3(x, y, z)));
                ERUPTION_LOG_WARN("[VFX] teste: '%s' em (%.0f, %.0f, %.0f) -> instancia %u", item.substr(0, at).c_str(),
                                  x, y, z, id);
            }
            start = end + 1;
        }
    }
    loadPhase("vfx+fim");
    return true;
}

// Sobe para a GPU o que os resolvedores de textura/malha deixaram na fila.
void Engine::flushPendingUploads() {
    // Texturas embutidas pendentes
    if (!s_pendingUploads.empty()) {
        m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
            for (auto& u : s_pendingUploads) {
                m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                if (!u.mipSizes.empty()) {
                    // Textura comprimida em bloco: mips vem prontos do bake.
                    std::vector<VkBufferImageCopy> regions(u.mipSizes.size());
                    VkDeviceSize off = 0;
                    for (uint32_t m = 0; m < u.mipSizes.size(); ++m) {
                        VkBufferImageCopy& r = regions[m];
                        r = {};
                        r.bufferOffset = off;
                        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        r.imageSubresource.mipLevel = m;
                        r.imageSubresource.layerCount = 1;
                        r.imageExtent = { std::max(1u, u.width >> m), std::max(1u, u.height >> m), 1 };
                        off += u.mipSizes[m];
                    }
                    vkCmdCopyBufferToImage(cmd, u.staging, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           static_cast<uint32_t>(regions.size()), regions.data());
                    m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                    continue;
                }
                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = { u.width, u.height, 1 };
                vkCmdCopyBufferToImage(cmd, u.staging, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                if (u.mipLevels > 1) {
                    this->generateMipmaps(cmd, u.image, u.width, u.height, u.mipLevels, VK_FORMAT_R8G8B8A8_UNORM, 1);
                } else {
                    m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                }
            }
        });
        for (auto& u : s_pendingUploads) {
            vmaDestroyBuffer(m_vulkan.allocator(), u.staging, u.stagingAlloc);
        }
        s_pendingUploads.clear();
    }

    flushPbrTextureUploads(&m_vulkan);
    
    if (!s_pendingMeshes.empty()) {
        m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
            for (auto& pm : s_pendingMeshes) {
                VkBufferCopy cv{0, 0, pm.vSize};
                vkCmdCopyBuffer(cmd, pm.staging, pm.vb, 1, &cv);
                VkBufferCopy ci{pm.vSize, 0, pm.iSize};
                vkCmdCopyBuffer(cmd, pm.staging, pm.ib, 1, &ci);
            }
        });
        for (auto& pm : s_pendingMeshes) {
            vmaDestroyBuffer(m_vulkan.allocator(), pm.staging, pm.alloc);
        }
        s_pendingMeshes.clear();
    }
}

bool Engine::loadModelForPreview(const std::string& glbPath) {
    if (!m_currentMap) return false;
    if (m_modelRenderer.hasAsset(glbPath)) return true;
    // O caminho do asset continua relativo (e' chave e vai pro arquivo de
    // cena); quem abre o arquivo e' o caminho resolvido no projeto.
    const std::string diskPath = project::gameRef(glbPath).string();
    std::ifstream f(diskPath, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "glTF", 4) != 0) {
        ERUPTION_LOG_ERROR("Preview: not a GLB file: %s", glbPath.c_str());
        return false;
    }
    ModelFile model = GltfParser::parse(data.data(), data.size(), std::filesystem::path(diskPath).parent_path().string());
    if (model.nodes.empty()) {
        ERUPTION_LOG_ERROR("Preview: no meshes in %s", glbPath.c_str());
        return false;
    }
    model.filePath = glbPath;
    m_currentMap->models.push_back(model); // as texturas embutidas são procuradas aqui
    const bool ok = m_modelRenderer.loadAssetMeshes(convertModelToAsset(model));
    flushPendingUploads();
    return ok;
}

namespace {

// .glb lido e interpretado, com as texturas embutidas ja' decodificadas (RGBA8,
// lado de no maximo 1024, como o resolveModelTexture faria no envio). A
// decodificacao do PNG era 75% do tempo da chegada a um mapa com modelos
// postos por script (perf, 499 Hz, 6 s depois da troca: stbi__parse_zlib 45%,
// create_png_image_raw 16%). `parallel` reparte as texturas pelo JobSystem
// (so' do fio principal: dentro de um job o parallelFor esperaria a si mesmo).
std::shared_ptr<ModelFile> prepareModelFile(const std::string& glbPath, bool parallel,
                                            const std::function<bool(const std::string&)>& wanted,
                                            int bakeMaxSize = 0) {
    const std::string diskPath = project::gameRef(glbPath).string();
    std::vector<uint8_t> data;
    {
        std::ifstream f(diskPath, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (data.size() < 12 || std::memcmp(data.data(), "glTF", 4) != 0) return nullptr;
    auto model = std::make_shared<ModelFile>(
        GltfParser::parse(data.data(), data.size(), std::filesystem::path(diskPath).parent_path().string()));
    if (model->nodes.empty()) return nullptr;
    model->filePath = glbPath;
    // Com bake em disco, as texturas cobertas por ele nao precisam de decode.
    std::unordered_set<std::string> baked;
    if (bakeMaxSize > 0 && !model->embeddedTextures.empty()) {
        EmbeddedBakeResult bake;
        if (readEmbeddedBakeCache(*model, static_cast<uint32_t>(bakeMaxSize), pbrSynthesisEnabled(), bake)) {
            for (const auto& t : bake.textures) baked.insert(t.first);
            std::lock_guard<std::mutex> lock(s_preloadBakeMutex);
            s_preloadBakes[glbPath] = std::move(bake);
        }
    }
    constexpr int kEmbeddedMaxSize = 1024; // o mesmo teto do resolveModelTexture
    // Quem decodificar decide-se aqui, no fio que chamou (`wanted` pode olhar
    // o cache de texturas, que so' o fio principal le).
    std::vector<uint32_t> todo;
    for (uint32_t i = 0; i < model->embeddedTextures.size(); ++i) {
        const EmbeddedTexture& et = model->embeddedTextures[i];
        if (!et.isDecoded() && et.hasEncodedData() && !baked.count(et.name) && wanted(et.name)) todo.push_back(i);
    }
    const bool synthesizePbr = !parallel; // so' no job do preload, fora do quadro
    auto decode = [&model, &todo, synthesizePbr](uint32_t k) {
        EmbeddedTexture& et = model->embeddedTextures[todo[k]];
        ImageData img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(), et.encodedData.size());
        if (!img.isValid()) return; // o envio tenta de novo e avisa
        // No preload, o PBR tambem sai daqui: a mesma sintese, com os mesmos
        // pixels em resolucao cheia, que o quadro faria (findEmbeddedAlbedoPixels
        // decodifica o original); ja' roda em threads no bake do mapa.
        if (synthesizePbr && pbrSynthesisEnabled()) {
            std::pair<PbrTextureData, PbrTextureData> pbr;
            if (synthesizePbrFromPixels(img.pixels, img.width, img.height, img.channels > 0 ? img.channels : 4,
                                        pbr.first, pbr.second)) {
                std::lock_guard<std::mutex> lock(s_preloadPbrMutex);
                s_preloadPbr[pbrKey(et.name)] = std::move(pbr);
            }
        }
        ImageUtils::downscaleRGBA(img.width, img.height, img.pixels, kEmbeddedMaxSize);
        et.pixels = std::move(img.pixels);
        et.width = img.width;
        et.height = img.height;
    };
    const auto count = static_cast<uint32_t>(todo.size());
    if (parallel) JobSystem::instance().parallelFor(count, decode);
    else for (uint32_t i = 0; i < count; ++i) decode(i);
    return model;
}

} // namespace

bool takePreloadedPbr(const std::string& albedoKey, PbrTextureData& mrahw, PbrTextureData& normal) {
    std::lock_guard<std::mutex> lock(s_preloadPbrMutex);
    const auto it = s_preloadPbr.find(pbrKey(albedoKey));
    if (it == s_preloadPbr.end()) return false;
    mrahw = it->second.first; // copia: o mesmo albedo pode vir de mais de um material
    normal = it->second.second;
    return true;
}

void Engine::waitModelPreload(const std::string& glbPath) {
    const auto it = m_modelPreloads.find(glbPath);
    if (it != m_modelPreloads.end() && it->second.model.valid()) it->second.model.wait();
}

void Engine::preloadModel(const std::string& glbPath) {
    if (glbPath.empty() || m_modelPreloads.count(glbPath)) return;
    // Tambem o que ja' esta' aberto neste mapa: o asset nao passa do warp e
    // volta a ser montado no proximo, e o PBR sintetizado dele nao fica (a
    // escada do mapa de origem custava ~270 ms na chegada, com ou sem preload).
    const int maxSize = m_textureMaxSize;
    ModelPreload p;
    p.model = JobSystem::instance()
                  .submit([glbPath, maxSize] { return prepareModelFile(glbPath, false, claimPreloadTexture, maxSize); })
                  .share();
    p.gen = m_modelPreloadGen;
    m_modelPreloads.emplace(glbPath, std::move(p));
}

int Engine::addModelFromFile(const std::string& glbPath, const Vec3& position, const std::string& nodeName) {
    if (!m_currentMap) return -1;
    // ERUPTION_TEST_LOAD_PHASES=1: onde vai o tempo de um modelo posto por
    // script/cena (leitura+parse, conversao+envio, uploads pendentes).
    static const bool kPhases = std::getenv("ERUPTION_TEST_LOAD_PHASES") != nullptr;
    const auto tA = std::chrono::steady_clock::now();
    auto tB = tA, tC = tA;
    const bool novo = !m_modelRenderer.hasAsset(glbPath);
    ModelInstanceDesc desc;
    desc.modelPath = glbPath;
    desc.position = position;
    desc.rotation = Vec3(0.0f);
    desc.scale = Vec3(1.0f);
    uint32_t first = 0xFFFFFFFFu;
    if (m_modelRenderer.hasAsset(glbPath)) {
        // Já carregado (o próprio mapa, ou outro modelo já acrescentado).
        first = m_modelRenderer.addInstanceOf(glbPath, desc, nodeName);
    } else {
        auto onGpu = [this](const std::string& name) {
            std::string key = name;
            std::replace(key.begin(), key.end(), '/', '\\');
            if (!key.empty() && key[0] == '\\') key.erase(0, 1);
            ModelTextureGPU cached;
            return m_assetCache.getModelTexture(key, cached);
        };
        // Pedido antes por preloadModel: pega o pronto (ou espera o que falta).
        std::shared_ptr<ModelFile> prepared;
        // So' o pronto, e por copia: o preload pode ter sido pedido para o
        // MAPA SEGUINTE, e um spawn daqui nao pode nem esperar o job (946 ms
        // numa peca da arena) nem gastar a entrada antes do warp. Ela sai
        // sozinha depois de duas trocas de mapa.
        const auto pre = m_modelPreloads.find(glbPath);
        if (pre != m_modelPreloads.end() &&
            pre->second.model.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
            pre->second.model.get()) {
            prepared = std::make_shared<ModelFile>(*pre->second.model.get());
            // Textura da familia que outro preload decodificou: copia de la'.
            if (prepared) for (EmbeddedTexture& et : prepared->embeddedTextures) {
                if (et.isDecoded() || !et.hasEncodedData() || onGpu(et.name)) continue;
                for (auto& entry : m_modelPreloads) {
                    const ModelPreload& other = entry.second;
                    // So' os prontos: esperar um que talvez nem tenha a textura
                    // custaria mais que decodificar aqui.
                    if (other.model.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
                    const std::shared_ptr<ModelFile>& o = other.model.get();
                    if (!o) continue;
                    const auto hit = std::find_if(o->embeddedTextures.begin(), o->embeddedTextures.end(),
                                                  [&et](const EmbeddedTexture& t) { return t.name == et.name && t.isDecoded(); });
                    if (hit == o->embeddedTextures.end()) continue;
                    et.pixels = hit->pixels;
                    et.width = hit->width;
                    et.height = hit->height;
                    break;
                }
            }
        } else {
            // Textura ja' na GPU (outro arquivo da mesma familia) nao decodifica.
            // Sem preload: so' o parse aqui; as texturas vem do bake abaixo.
            prepared = prepareModelFile(glbPath, true, [](const std::string&) { return false; });
        }
        if (!prepared) {
            ERUPTION_LOG_ERROR("addModelFromFile: not a GLB file with meshes: %s", glbPath.c_str());
            return -1;
        }
        tB = std::chrono::steady_clock::now();
        // As texturas embutidas são procuradas nos modelos do mapa aberto.
        m_currentMap->models.push_back(std::move(*prepared));
        ModelFile& model = m_currentMap->models.back();
        // Texturas embutidas em bloco com o PBR pronto, como as do mapa, quando
        // o .etexpack ja' existe ao lado do .glb (o mesmo cache do mapa).
        if (!model.embeddedTextures.empty()) {
            EmbeddedBakeResult bake;
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(s_preloadBakeMutex);
                if (auto it = s_preloadBakes.find(glbPath); it != s_preloadBakes.end()) {
                    bake = it->second; // copia: a entrada fica para o mapa seguinte
                    have = true;
                }
            }
            const bool missing = std::any_of(model.embeddedTextures.begin(), model.embeddedTextures.end(),
                                             [&](const EmbeddedTexture& et) { return !onGpu(et.name); });
            // Sem preload: so' o cache, se ja' existir. O bake no quadro custava
            // ~2,8 s por textura na primeira vez (BC + sintese), muito mais
            // que a sintese direta abaixo dele (medido no warp de 17:03).
            if (!have && missing)
                readEmbeddedBakeCache(model, static_cast<uint32_t>(m_textureMaxSize), pbrSynthesisEnabled(), bake);
            for (auto& [name, tex] : bake.textures) m_embeddedBaked.emplace(name, std::move(tex));
        }
        first = m_modelRenderer.addModel(convertModelToAsset(model), desc, nodeName);
        // Ja' na GPU (o cache de texturas guarda pelo nome): fica so' o
        // arquivo codificado, para o caso de a textura ser expulsa e voltar.
        for (EmbeddedTexture& et : model.embeddedTextures)
            if (et.hasEncodedData()) std::vector<uint8_t>().swap(et.pixels);
    }
    tC = std::chrono::steady_clock::now();
    flushPendingUploads();
    if (kPhases && novo) {
        const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        const auto tD = std::chrono::steady_clock::now();
        ERUPTION_LOG_WARN("[MODELPHASE] %s: parse %.1f, converte+envia %.1f, uploads %.1f ms", glbPath.c_str(), ms(tA, tB),
                          ms(tB, tC), ms(tC, tD));
    }
    if (first == 0xFFFFFFFFu) {
        ERUPTION_LOG_ERROR("addModelFromFile: nothing to add from %s%s%s", glbPath.c_str(),
                           nodeName.empty() ? "" : " node ", nodeName.c_str());
        return -1;
    }
    return static_cast<int>(first);
}

void Engine::updateWarp() {
    // Um pedido por vez: outro pedido durante a leitura espera ela acabar e
    // o resultado velho é jogado fora.
    if (!m_warpJobActive && !m_pendingMapWarp.empty()) {
        m_warpJobMap = m_pendingMapWarp;
        m_pendingMapWarp.clear();
        m_warpJobActive = true;
        const std::string map = m_warpJobMap;
        m_warpJob = platform::runAsync([map] {
            MapLoader loader;
            return loader.loadMap(map);
        });
        ERUPTION_LOG_WARN("Warp: reading %s in the background", map.c_str());
        return;
    }
    if (!m_warpJobActive || m_warpJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::shared_ptr<LoadedMap> map = m_warpJob.get();
    m_warpJobActive = false;
    const std::string name = m_warpJobMap;
    if (!m_pendingMapWarp.empty()) return; // pediram outro mapa no meio: começa no próximo frame
    if (!map) {
        ERUPTION_LOG_ERROR("Warp: could not read map %s", name.c_str());
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!loadMap(name, std::move(map))) return;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    TelemetryExporter::recordMapSwap(name, ms);
    ERUPTION_LOG_WARN("Warp: %s in place (%.0f ms on the main thread)", name.c_str(), ms);
    m_framesAfterSwap = 0;
    // Gancho de teste: captura depois da primeira troca.
    if (m_screenshotAfterWarpPending && !m_screenshotAfterWarpPath.empty()) {
        m_screenshotOnLoad = m_screenshotAfterWarpPath;
        m_screenshotAfterWarpPath.clear();
        m_screenshotAfterWarpPending = false;
    }
}

} // namespace eruption
