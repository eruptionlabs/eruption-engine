// Carga de mapa do Engine: loadMap (o caminho sincrono completo - terreno,
// modelos, sprites, agua, clima, spawn do jogador), a amostragem de altura do
// terreno usada pra decidir onde o jogador nasce, e setPlayerController.
// Saiu do Engine.cpp em 2026-09-04, na quebra do arquivo.
//
// O caminho ASSINCRONO (warp com a cena ja' viva) nao esta' aqui: quem faz o
// swap e drena a fila de upload e' o MapUpload.cpp.

#include "core/Engine.hpp"
#include "utils/ReadWholeFile.hpp"
#include "utils/AssetVfs.hpp"
#include "core/Platform.hpp"
#include "core/EngineInternal.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TelemetryExporter.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/PbrDeferred.hpp"
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include "utils/ImageUtils.hpp"
#include "formats/GroundPack.hpp"
#include "formats/MapLoader.hpp"
#include "core/LoadPump.hpp"
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
#include <sstream>
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

float Engine::sampleOpenMapHeight(const LoadedMap& map, float x, float z) {
    return sampleGlbOrTerrainHeight(map, x, z);
}

float Engine::sampleTerrainHeight(float x, float z, float y, bool withRelief) const {
    const LoadedMap* map = mapAt(x, z, y); // o aberto ou um vizinho em cena
    if (!map) return 0.0f;
    // + o desnivel do chao assado, onde ele vai para a tela (celular).
    const float h = sampleGlbOrTerrainHeight(*map, x, z);
    return withRelief ? h + m_modelRenderer.reliefFloorDelta(x, z, h) : h;
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

// Assinatura da geometria para os caches que dependem dela (grade de
// sondas): numero de nos, vertices e indices, a matriz de cada no e uma
// amostra das posicoes. Igual no PC e no celular (o pacote do celular so'
// troca texturas); muda quando uma ferramenta regrava o relevo do GLB.
static void fnvMix(uint64_t& h, const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
}

// Arredonda para 1/16 de unidade antes de misturar: o mesmo GLB convertido
// num x86 e num ARM (que funde multiplica-soma) difere no ultimo bit, e o
// relevo que importa muda muito mais que isso.
static void fnvMixQuantized(uint64_t& h, const float* values, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const int32_t q = static_cast<int32_t>(std::lround(values[i] * 16.0f));
        fnvMix(h, &q, sizeof(q));
    }
}

// Chave de conteudo do mapa para caches cozidos num aparelho e lidos em outro
// (a grade de sondas do PC no celular): FNV dos bytes de todo buffer do GLB que
// nao e' imagem (malha, indices, animacao) e do relevo salvo pelo editor. O
// pacote do celular so' recodifica imagens, entao a chave bate; uma ferramenta
// que regrave o relevo muda os bytes. A geometria ja' convertida nao serve: a
// fusao de nos repetidos escolhe outro no' canonico em outra biblioteca C++.
static bool hashGlbGeometry(const std::string& glbPath, uint64_t& h) {
    // Le' so' o JSON e passa pelos trechos de geometria em blocos de 1 MB: o
    // GLB inteiro na memoria (68-200 MB) com o mapa anterior ainda carregado
    // travava a GPU do celular de 4 GB na troca de mapa, e custava 2,8 s.
    // No pacote do mapa (.emap) o .glb ja' passa pela memoria: le' inteiro de
    // la' (o arquivo solto continua em blocos).
    std::vector<uint8_t> whole;
    std::istringstream mem;
    std::ifstream file;
    bool fromMemory = vfs::inBundle(glbPath);
    if (!fromMemory && vfs::packsMounted()) { // so' num pacote .ecp do projeto
        std::error_code pec;
        fromMemory = !std::filesystem::exists(glbPath, pec);
    }
    if (fromMemory && vfs::read(glbPath, whole)) {
        mem.str(std::string(whole.begin(), whole.end()));
        whole.clear();
        whole.shrink_to_fit();
    } else {
        file.open(glbPath, std::ios::binary);
    }
    std::istream& f = file.is_open() ? static_cast<std::istream&>(file) : static_cast<std::istream&>(mem);
    uint32_t head[5] = {};
    if (!f.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
    const uint32_t jsonLen = head[3];
    std::string jsonText(jsonLen, '\0');
    uint32_t binHead[2] = {};
    if (!f.read(jsonText.data(), jsonLen) || !f.read(reinterpret_cast<char*>(binHead), sizeof(binHead))) return false;
    const std::streamoff binStart = 28 + std::streamoff(jsonLen);
    const size_t binSize = binHead[0];
    const json doc = json::parse(jsonText, nullptr, false);
    if (doc.is_discarded()) return false;
    // Fora da chave: imagens e a normal. A normal muda entre maquinas
    // (contracao FMA no ARM) e no chao desempacotado de um .relevo, que a
    // recalcula; posicao, UV e indices saem byte-iguais (mesma regra da chave
    // do LOD). Assim a grade cozida no PC vale no celular e no .relevo.
    std::vector<bool> skipView(doc.value("bufferViews", json::array()).size(), false);
    for (const json& img : doc.value("images", json::array()))
        if (img.contains("bufferView") && img["bufferView"].get<size_t>() < skipView.size())
            skipView[img["bufferView"].get<size_t>()] = true;
    const json accessors = doc.value("accessors", json::array());
    for (const json& mesh : doc.value("meshes", json::array()))
        for (const json& prim : mesh.value("primitives", json::array())) {
            const json attrs = prim.value("attributes", json::object());
            for (const char* name : {"NORMAL", "TANGENT"}) {
                if (!attrs.contains(name)) continue;
                const size_t a = attrs[name].get<size_t>();
                if (a >= accessors.size() || !accessors[a].contains("bufferView")) continue;
                const size_t v = accessors[a]["bufferView"].get<size_t>();
                if (v < skipView.size()) skipView[v] = true;
            }
        }
    std::vector<char> chunk(1 << 20);
    size_t index = 0;
    for (const json& view : doc.value("bufferViews", json::array())) {
        const size_t off = view.value("byteOffset", size_t(0)), len = view.value("byteLength", size_t(0));
        if (skipView[index++] || off + len > binSize) continue;
        f.seekg(binStart + std::streamoff(off));
        for (size_t done = 0; done < len;) {
            const size_t n = std::min(chunk.size(), len - done);
            if (!f.read(chunk.data(), std::streamsize(n))) return false;
            fnvMix(h, chunk.data(), n);
            done += n;
        }
    }
    return true;
}

static uint64_t mapContentKeyRead(const std::string& glbPath, const std::string& mapName) {
    uint64_t h = 1469598103934665603ull;
    // Chao vindo de um .relevo: le' o .glb desempacotado no cache (a mesma
    // chave do .glb original, que a normal fica de fora).
    std::filesystem::path source = glbPath;
    if (const std::string relevo = (vfs::exists(glbPath) ? std::string() : groundSourceFor(glbPath)); !relevo.empty()) {
        std::filesystem::path ter;
        std::string error;
        if (!unpackGroundCached(relevo, source, ter, error)) return 0;
    }
    if (!hashGlbGeometry(source.string(), h)) return 0;
    std::filesystem::path heights = SceneEdits::pathFor(mapName);
    heights.replace_extension();
    heights.replace_extension(".height.bin");
    std::vector<uint8_t> hraw;
    vfs::read(heights, hraw);
    if (!hraw.empty()) fnvMix(h, hraw.data(), hraw.size());
    return h;
}

// A chave le' a geometria inteira do GLB (centenas de MB no mapa grande; 962
// ms com o disco frio num mapa de cidade, na thread principal da troca). Lembrada
// por caminho + tamanho + data do GLB e do .height.bin: a thread de leitura
// do mapa (preload/warp) calcula antes e a troca so' consulta.
static uint64_t mapContentKey(const std::string& glbPath, const std::string& mapName) {
    std::filesystem::path heights = SceneEdits::pathFor(mapName);
    heights.replace_extension();
    heights.replace_extension(".height.bin");
    auto stamp = [](const std::filesystem::path& p) -> uint64_t {
        uint64_t size = 0;
        int64_t when = 0;
        if (!vfs::stat(p.string(), size, when)) return 0;
        return uint64_t(size) * 1099511628211ull ^ uint64_t(when);
    };
    const std::string relevo = (vfs::exists(glbPath) ? std::string() : groundSourceFor(glbPath));
    const uint64_t s = stamp(relevo.empty() ? glbPath : relevo) * 31u + stamp(heights);
    static std::mutex mutex;
    static std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> memo; // nome -> (selo, chave)
    const std::string id = glbPath + "|" + mapName;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = memo.find(id); it != memo.end() && it->second.first == s) return it->second.second;
    }
    const uint64_t key = mapContentKeyRead(glbPath, mapName);
    std::lock_guard<std::mutex> lock(mutex);
    memo[id] = {s, key};
    return key;
}

// Y de mundo mais baixo da geometria dos modelos do mapa (teste de agua
// soterrada). Pelos vertices quando o no ainda os tem; senao pela caixa local
// (a do dono, em geometria compartilhada), nos 8 cantos pela matriz do no.
static float glbGeometryMinY(const LoadedMap& map) {
    float minY = 3.4e38f;
    for (const auto& model : map.models)
        for (const auto& node : model.nodes) {
            if (!node.vertices.empty()) {
                for (const auto& v : node.vertices) minY = std::min(minY, (node.renderMatrix * Vec4(v, 1.0f)).y);
                continue;
            }
            const bool shared = node.geometryShareIndex >= 0 &&
                                node.geometryShareIndex < static_cast<int32_t>(model.nodes.size());
            const BoundingBox& b = shared ? model.nodes[node.geometryShareIndex].box : node.box;
            if (b.min.x > b.max.x) continue; // caixa vazia
            for (int c = 0; c < 8; ++c) {
                const Vec3 q((c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z);
                minY = std::min(minY, (node.renderMatrix * Vec4(q, 1.0f)).y);
            }
        }
    return minY;
}

static uint64_t mapGeometryKey(const std::vector<ModelAsset>& models) {
    uint64_t h = 1469598103934665603ull;
    for (const ModelAsset& model : models) {
        for (const ModelNode& node : model.nodes) {
            const uint64_t sizes[2] = {node.vertices.size(), node.indices.size()};
            fnvMix(h, sizes, sizeof(sizes));
            fnvMixQuantized(h, &node.renderMatrix[3][0], 3); // translacao do no
            for (size_t v = 0; v < node.vertices.size(); v += 61)
                fnvMixQuantized(h, &node.vertices[v].position.x, 3);
        }
    }
    return h;
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
    if (!preloaded) { // so' valem junto do mapa lido que veio com eles
        m_convertedModels.clear();
        m_convertedPrepared.clear();
        m_convertedBakes.clear();
    }
    m_fsrReset = true; // historico temporal do FSR nao vale entre mapas
    m_isInitialBoot = false; // No longer the initial boot
    TelemetryExporter::recordLoadStart(name);
    loadMapClimate(name);
    m_vulkan.waitIdle();
    loadPhase("clima+waitIdle");
    const vkc::PipelineHookScope shaderProgress(&Engine::onPipelineCompiled, this);
    clearCharacters(); // os do mapa novo vêm do scene.luau dele
    m_scriptFog = MapFog{}; // world.set_fog e' do mapa que o pediu
    m_scriptBorder = WorldBorder{}; // world.set_border tambem
    m_scriptExposure = -1.0f; // world.set_exposure tambem
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
    m_assetCache.cleanupOldTextures(&m_vulkan, &m_bindless, m_assetCacheMaps);

    // Independent clouds belong to the previous map: drop them so stale world
    // bounds / layers don't leak into the newly loaded map. The weather field
    // respawns for the new map on the next update tick.
    clearIndependentClouds();
    m_weatherFieldType = -1;

    presentLoadingScreen(name, "Loading map data", 0.1f);
    loadPhase("cache+nuvens");

    // MapLoader accepts either a plain map name (resolved to assets/external/<name>/<name>.glb)
    // or an explicit file path ending in .glb/.map.
    // Leitura numa thread (como no updateWarp) enquanto a tela de carregamento
    // segue viva; sem tela para manter, o resultado e' o mesmo.
    std::shared_ptr<LoadedMap> loadedMap = std::move(preloaded);
    if (!loadedMap) {
        auto job = platform::runAsync([name] {
            MapLoader loader;
            return loader.loadMap(name);
        });
        while (job.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) loadPump();
        loadedMap = job.get();
    }
    if (!loadedMap) {
        ERUPTION_LOG_ERROR("Failed to load map: %s", name.c_str());
        return false;
    }
    
    m_currentMap = loadedMap; 
    m_currentMapName = name;
    m_vfx.clear(); // efeitos pertencem ao mapa que saiu

    // Configure water mesh from map data
    m_shoreBuildDeferred = true; // a grade da beira sai uma vez so', no fim da carga
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
        // Chao mais baixo da geometria do .glb. O .ter que acompanha o mapa
        // pode ser mais raso que o chao de verdade: num mapa com .ter ate' -5
        // e o chao do .glb ate' -40, a agua em -28 era dada por soterrada e
        // nao aparecia em modo nenhum. A pre-carga ja' mediu antes de liberar
        // a malha (readPreparedMap); na carga direta os nos ainda a tem.
        if (loadedMap->geometryMinY >= 3.0e38f) loadedMap->geometryMinY = glbGeometryMinY(*loadedMap);
        m_mapGeometryMinY = loadedMap->geometryMinY;
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
    // O warp (e o preloadMap) ja' convertem na thread de leitura.
    if (m_convertedModels.size() == loadedMap->models.size() && !m_convertedModels.empty()) {
        genericModels = std::move(m_convertedModels);
    } else {
        m_convertedPrepared.clear();
        for (auto& model : loadedMap->models) {
            genericModels.push_back(convertModelToAssetReleasing(model));
        }
    }
    m_convertedModels.clear();

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
    presentLoadingScreen(name, "Converting models", 0.74f); // a barra anda dentro da etapa (ficava parada no 70%)
    applySavedWalk(*loadedMap);
    m_mapGeometryKey = mapGeometryKey(genericModels);
    // Nós montados na thread de leitura: valem se a geometria e' a mesma que
    // eles viram (retoque de relevo ou escala mudam a assinatura).
    if (!m_convertedPrepared.empty()) {
        if (m_convertedKey == m_mapGeometryKey) m_modelRenderer.provideMapPrepared(std::move(m_convertedPrepared));
        else ERUPTION_LOG_WARN("Engine::loadMap: geometry changed after the background read; preparing nodes here");
        m_convertedPrepared.clear();
    }
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
    {
        std::lock_guard<std::mutex> lock(m_reliefAlbedoMutex);
        m_reliefAlbedo.clear();
    }
    loadPhase("geometria/costuras");
    presentLoadingScreen(name, "Building geometry", 0.78f); // a barra anda dentro da etapa (ficava parada no 70%)
    if (m_currentMap) {
        for (auto& model : m_currentMap->models) {
            if (model.embeddedTextures.empty()) continue;
            // Lido do cache na thread do warp/preload, quando veio de la'.
            EmbeddedBakeResult bake;
            if (auto pre = m_convertedBakes.find(model.filePath); pre != m_convertedBakes.end()) {
                bake = std::move(pre->second);
                m_convertedBakes.erase(pre);
                ERUPTION_LOG_WARN("EmbeddedTextureBake: '%s' lido antes da troca (%zu texturas)",
                                  model.filePath.c_str(), bake.textures.size());
            } else {
                // Pacote grande a frio: so' o albedo agora, o PBR chega depois (PbrDeferred.hpp).
                bake = bakeEmbeddedTextures(model, static_cast<uint32_t>(m_textureMaxSize), pbrSynthesisEnabled(),
                                            !m_editorMode && PbrDeferred::enabled(&m_bindless));
            }
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
    presentLoadingScreen(name, "Uploading textures", 0.84f); // a barra anda dentro da etapa (ficava parada no 70%)
    auto _tLMM = std::chrono::steady_clock::now();
    // Cópia do chão na CPU: pincel do editor, ou pintura salva que o jogo
    // aplica nos vértices (mapas sem splat próprio).
    m_modelRenderer.setKeepGroundVertices(m_editorMode || hasSavedSplat(name) ||
                                          SceneEdits::fileMentions(name, "terrain") ||
                                          SceneEdits::fileMentions(name, "neighbors")); // bordas costuradas
    m_modelRenderer.resetResolveStats();
    // Relevo do chao em segundo plano (pollReliefJobs), como nos vizinhos do
    // mundo continuo: no fio principal ele segurava a troca de mapa 0,6-0,8 s
    // mesmo vindo do cache do relevo (441 MB num mapa de porto, 158 MB num de floresta); assim
    // ele corre junto com a montagem dos modelos da cena, que vem logo depois.
    m_modelRenderer.setReliefDeferred(true);
    m_modelRenderer.loadMapModels(std::move(genericModels), genericInstances, modelCenterX, modelCenterZ);
    m_modelRenderer.setReliefDeferred(false);
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
    presentLoadingScreen(name, "Placing models", 0.9f); // a barra anda dentro da etapa (ficava parada no 70%)
    if (hasSavedSplat(name)) beginSplatEditing(); // máscaras pintadas no editor


    ERUPTION_LOG_WARN("Engine::loadMap: ModelRenderer has %zu meshes, %zu instances, "
                      "%llu triangulos carregados (malha base x instancia, pre-cull/LOD)",
                      m_modelRenderer.meshCount(), m_modelRenderer.instanceCount(),
                      static_cast<unsigned long long>(m_modelRenderer.loadedTriangles()));
    
    presentLoadingScreen(name, "Finalizing", 0.95f);
    loadPhase("uploads+splat");
    m_vulkan.savePipelineCache();
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
    // Caixa das nuvens do clima por mapa (.env "cloud_box"): a mesma caixa do
    // --cloud-bottom/--cloud-thickness, onde as nuvens independentes do campo
    // do clima nascem (independentCloudPlaneY). P.ex. abaixo de uma cidade
    // flutuante. O mapa seguinte sem o bloco volta ao estado de antes.
    {
        if (!m_cloudBoxBaseSaved) {
            m_cloudBoxBase = {m_cloudBoxOverride, m_cloudBoxBottom, m_cloudBoxThickness, true};
            m_cloudBoxBaseSaved = true;
            m_cloudShadowsBase = m_cloudLayerRenderer.config().showCloudShadows;
        }
        m_waterMenu.config.shoreSwash = loadedMap->env.shoreSwash; // por mapa
        m_waterMenu.config.shoreSwashFoam = loadedMap->env.shoreSwashFoam;
        const MapCloudLayer& cb = loadedMap->env.cloudLayer;
        const bool antes = m_cloudBoxOverride;
        const float antesY = m_cloudBoxBottom;
        if (cb.set) {
            setCloudBoxOverride(cb.bottom, cb.thickness);
        } else {
            m_cloudBoxOverride = m_cloudBoxBase.on;
            m_cloudBoxBottom = m_cloudBoxBase.bottom;
            m_cloudBoxThickness = m_cloudBoxBase.thickness;
        }
        const int minAntes = m_mapCloudMinCount;
        m_mapCloudMinCount = cb.set ? cb.count : 0;
        m_mapCloudMinDensity = cb.density;
        m_mapCloudMinScale = cb.scale;
        // Nuvens ja' nascidas na caixa antiga ficariam na altura errada; com
        // outro minimo do mapa, o campo do clima nasce de novo.
        if (antes != m_cloudBoxOverride || antesY != m_cloudBoxBottom || minAntes != m_mapCloudMinCount) {
            clearIndependentClouds();
            m_weatherFieldType = -1;
        }
        CloudLayerRenderer::Config cfg = m_cloudLayerRenderer.config();
        cfg.showCloudShadows = cb.set ? cb.shadows : m_cloudShadowsBase;
        m_cloudLayerRenderer.setConfig(cfg);
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
        // Grade em disco ao lado do GLB do mapa: quem coze grava, quem nao
        // coze (celular) so' le'.
        const std::string probeCache = m_currentMap && !m_currentMap->models.empty()
            ? project::gameRef(m_currentMap->models.front().filePath).string() + ".probes" : std::string();
        const uint64_t contentKey = (m_irradianceProbesEnabled && !probeCache.empty())
            ? mapContentKey(probeCache.substr(0, probeCache.size() - 7), m_currentMapName) : 0;
        loadPhase("probes: chave");
        bool ready = false;
        m_probeBakedInstances = SIZE_MAX;
        if (haveBounds && m_irradianceProbesEnabled) {
            mx.y += 40.0f;
            mn.y -= 20.0f;
            if (m_irradianceProbeBake) {
                ready = m_irradianceProbes.bake(m_terrainRenderer, m_modelRenderer,
                                                m_shadowRenderer.shadowPipeline(), m_shadowRenderer.shadowLayout(),
                                                m_shadowRenderer.shadowInstPipeline(), m_shadowRenderer.shadowInstLayout(),
                                                &m_bindless, mn, mx);
                if (ready && !probeCache.empty()) m_irradianceProbes.saveCache(probeCache, mn, mx, contentKey);
                if (ready) m_probeBakedInstances = m_modelRenderer.instanceCount();
            } else if (!probeCache.empty()) {
                ready = m_irradianceProbes.loadCache(probeCache, mn, mx, contentKey);
            }
            if (ready) {
                m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                                       m_irradianceProbes.gridMin(), m_irradianceProbes.gridInvExtent(), true);
            }
        }
        if (!ready) {
            m_irradianceProbes.clear();
            m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                                   Vec3(0.0f), Vec3(0.0f), false);
        }
    }

    loadPhase("probes: bake");
    TelemetryExporter::recordLoadEnd(name);
    {
        // O chao primeiro: normal e MRAH-W (altura do relevo) das texturas de chao saem do
        // pacote antes do resto (PbrDeferred::prioritize).
        std::vector<uint32_t> groundPbr, groundNormal;
        for (const auto& [texName, slot] : m_modelRenderer.getTextureCache()) {
            if (m_modelRenderer.groundRoleForSlot(slot) == 0) continue;
            const PbrTextureSlots pbr = m_modelRenderer.pbrForTexSlot(slot);
            if (pbr.mrahw.deferred) groundPbr.push_back(pbr.mrahw.index);
            if (pbr.normal.deferred) groundNormal.push_back(pbr.normal.index);
        }
        if (!groundPbr.empty() || !groundNormal.empty()) PbrDeferred::instance().prioritize(groundPbr, groundNormal);
    }
    PbrDeferred::instance().release(); // a leitura do PBR adiado so' comeca com o mapa jogavel
    if (PbrDeferred::testOverride() == 2) PbrDeferred::instance().drainBlocking(&m_vulkan, &m_bindless);
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
    // Agua soterrada e grade da beira de novo, com a config FINAL do mapa: na
    // montagem da malha (la' em cima) o modo da beira do .env, o nivel e o
    // liquido selecionado ainda eram os do mapa anterior (ou do json), e o
    // teste podia dar a agua por soterrada ou montar a beira errada. So'
    // religar a agua pelo menu ("Force Water") refazia isso - parecia que a
    // agua precisava de um reload.
    m_shoreBuildDeferred = false;
    m_shoreAsyncOnce = true; // a grade da beira sai em segundo plano (pollShoreMask)
    cacheWaterBounds(m_shoreWaterSrc);
    m_shoreAsyncOnce = false;
    loadPhase("vfx+fim");
    // ERUPTION_TEST_DUMP_RELIEF_AMP=1 (prova, eada502): amplitude registrada
    // (ModelRenderer::m_texReliefAmp) de cada textura de CHAO deste mapa -
    // todas tem que sair 2,5 u (m_reliefBake.amplitude e' uma regra so').
    if (std::getenv("ERUPTION_TEST_DUMP_RELIEF_AMP")) {
        for (const auto& [name, slot] : m_modelRenderer.getTextureCache()) {
            if (m_modelRenderer.groundRoleForSlot(slot) != 1) continue;
            ERUPTION_LOG_WARN("RELEVO-AMP mapa='%s' textura='%s' slot=%u amplitude=%.3f", m_currentMapName.c_str(),
                              name.c_str(), slot, m_modelRenderer.reliefAmplitudeForSlot(slot));
        }
    }
    // Terreno grande: poe o BVH das malhas grandes do mapa pra construir em
    // segundo plano agora (fora de qualquer quadro de jogo), bem antes da 1a
    // consulta de piso do script - sem isso, a 1a chamada de floor_height/
    // floor_walkable numa malha grande (ex. uma montanha grande de terreno)
    // pagava a construcao inteira de uma vez e travava o quadro por >250 ms.
    m_modelRenderer.warmRaycastBvh();
    return true;
}

// Sobe para a GPU o que os resolvedores de textura/malha deixaram na fila.
void Engine::flushPendingUploads() {
    // Texturas embutidas pendentes
    if (!s_pendingUploads.empty()) {
        // Tamanho do envio (um so' para todas as texturas embutidas pendentes):
        // um envio que o driver recusa por memoria diz aqui o quanto pedia.
        double uploadMb = 0.0;
        std::vector<double> sizeMb;
        sizeMb.reserve(s_pendingUploads.size());
        for (const auto& u : s_pendingUploads) {
            uint64_t bytes = 0;
            for (uint32_t s : u.mipSizes) bytes += s;
            if (bytes == 0) bytes = static_cast<uint64_t>(u.width) * u.height * 4;
            sizeMb.push_back(static_cast<double>(bytes) / (1024.0 * 1024.0));
            uploadMb += sizeMb.back();
        }
        // Em lotes de ate' kUploadChunkMb por envio: 454 texturas (95,7 MB) num
        // envio so' foram recusadas com VK_ERROR_OUT_OF_DEVICE_MEMORY na carga de
        // um celular de teste (memoria interna do driver, nao a nossa); um lote
        // pequeno por envio mantem o que o driver segura de cada vez limitado.
        constexpr double kUploadChunkMb = 16.0;
        ERUPTION_LOG_WARN("[UPLOAD] %zu textura(s) embutida(s), %.1f MB, lotes de ate' %.0f MB", s_pendingUploads.size(),
                          uploadMb, kUploadChunkMb);
        size_t from = 0;
        while (from < s_pendingUploads.size()) {
        size_t to = from;
        double chunkMb = 0.0;
        while (to < s_pendingUploads.size() && (to == from || chunkMb + sizeMb[to] <= kUploadChunkMb)) chunkMb += sizeMb[to++];
        m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
            for (size_t k = from; k < to; ++k) {
                auto& u = s_pendingUploads[k];
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
        from = to;
        }
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
    std::vector<uint8_t> data;
    vfs::read(diskPath, data);
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
                                            int bakeMaxSize = 0, bool deferPbr = false) {
    const std::string diskPath = project::gameRef(glbPath).string();
    std::vector<uint8_t> data;
    {
        // Um fio lendo por vez no HDD. Do pacote do mapa (.emap) nao: ele ja'
        // le' em sequencia, e a trava serializava a descompressao (o mapa grande de teste com o
        // disco quente: espera do preload da cena 1566 ms com a trava).
        std::unique_lock<std::mutex> disco(diskReadMutex(), std::defer_lock);
        if (!vfs::inBundle(diskPath)) disco.lock();
        vfs::read(diskPath, data); // pacote do mapa (.emap) primeiro
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
        // O pacote fica ao lado do .glb no disco: pelo caminho relativo ao
        // projeto o preload nunca o achava. Sem pacote, assa aqui (BC + PBR,
        // fora do quadro) e grava: so' a primeira abertura paga a sintese, que
        // antes se refazia inteira a cada mapa aberto (cena de 1058 objetos:
        // 90 s parados).
        model->filePath = diskPath;
        bool ok = readEmbeddedBakeCache(*model, static_cast<uint32_t>(bakeMaxSize), pbrSynthesisEnabled(), bake, deferPbr);
        if (!ok) {
            bake = bakeEmbeddedTextures(*model, static_cast<uint32_t>(bakeMaxSize), pbrSynthesisEnabled(), deferPbr);
            ok = !bake.textures.empty();
        }
        model->filePath = glbPath;
        if (ok) {
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
        // Textura com _mrahw/_normal em disco: o PBR autoral vence a sintese (setBakedPbrProvider),
        // entao sintetizar aqui era trabalho jogado fora - no navegador (sem fios, sem cache de bloco)
        // eram ~8 s da carga de uma vila.
        bool temPbrEmDisco = false;
        if (synthesizePbr) {
            const PbrTexturePathPair par = resolvePbrTexturePaths(et.name);
            temPbrEmDisco = par.valid && !par.mrahw.empty() && !par.normal.empty();
        }
        if (synthesizePbr && !temPbrEmDisco && pbrSynthesisEnabled()) {
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
    const bool defer = !m_editorMode && PbrDeferred::enabled(&m_bindless);
    ModelPreload p;
    p.model = JobSystem::instance()
                  .submit([glbPath, maxSize, defer] { return prepareModelFile(glbPath, false, claimPreloadTexture, maxSize, defer); })
                  .share();
    p.gen = m_modelPreloadGen;
    m_modelPreloads.emplace(glbPath, std::move(p));
}

// O pacote fica ao lado do .glb no disco; o caminho do modelo é relativo ao
// projeto e, lido de outra pasta de trabalho, nunca achava o pacote.
static bool readModelBakeCache(ModelFile& model, uint32_t maxSize, EmbeddedBakeResult& out) {
    const std::string rel = model.filePath;
    model.filePath = project::gameRef(rel).string();
    const bool ok = readEmbeddedBakeCache(model, maxSize, pbrSynthesisEnabled(), out);
    model.filePath = rel;
    return ok;
}

std::shared_ptr<Engine::ReadyModel> Engine::readModelForWorld(const std::string& glbPath) {
    auto out = std::make_shared<ReadyModel>();
    out->file = glbPath;
    out->model = prepareModelFile(glbPath, false, [](const std::string&) { return false; });
    if (!out->model) return nullptr;
    // Roda na thread de leitura do vizinho: pacote velho ou ausente é
    // comprimido e gravado aqui, não decodificado no quadro do encaixe.
    if (!out->model->embeddedTextures.empty()) {
        ModelFile& m = *out->model;
        const std::string rel = m.filePath;
        m.filePath = project::gameRef(rel).string(); // o pacote fica ao lado do .glb
        out->bake = bakeEmbeddedTextures(m, embeddedTextureMaxSize(), pbrSynthesisEnabled());
        m.filePath = rel;
    }
    out->asset = convertModelToAsset(*out->model);
    out->model->nodes.clear(); // a malha segue no asset; ficam as imagens
    out->slotUse = ModelRenderer::scanSlotUse(out->asset);
    return out;
}

bool Engine::modelTextureOnGpu(const std::string& name) {
    std::string key = name;
    std::replace(key.begin(), key.end(), '/', '\\');
    if (!key.empty() && key[0] == '\\') key.erase(0, 1);
    ModelTextureGPU cached;
    return m_assetCache.getModelTexture(key, cached);
}

// O ModelFile de um .glb pedido antes por preloadModel (copia do pronto, com
// as texturas de familia ja' decodificadas por outro preload), ou so' o parse.
std::shared_ptr<ModelFile> Engine::takePreparedModelFile(const std::string& glbPath) {
    auto onGpu = [this](const std::string& name) { return modelTextureOnGpu(name); };
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
    return prepared;
}

void Engine::applyModelBake(ModelFile& model, const std::string& glbPath) {
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
                                         [&](const EmbeddedTexture& et) { return !modelTextureOnGpu(et.name); });
        // Sem preload: so' o cache, se ja' existir. O bake no quadro custava
        // ~2,8 s por textura na primeira vez (BC + sintese), muito mais
        // que a sintese direta abaixo dele (medido no warp de 17:03).
        if (!have && missing) readModelBakeCache(model, static_cast<uint32_t>(m_textureMaxSize), bake);
        for (auto& [name, tex] : bake.textures) m_embeddedBaked.emplace(name, std::move(tex));
    }
}

// Cena de muitos modelos (SceneEdits::apply): o que so' o fio principal faz
// (texturas e slots) vai aqui, um modelo por vez, e a montagem pesada (malha,
// LOD) de todos em paralelo; o addModelFromFile seguinte os acha prontos, pelo
// mesmo caminho do mundo continuo. Um a um no quadro eram 13,5 s numa cidade.
void Engine::prepareSceneModels(const std::vector<std::string>& files) {
    if (!m_currentMap) return;
    static const bool kPhases = std::getenv("ERUPTION_TEST_LOAD_PHASES") != nullptr;
    using clk = std::chrono::steady_clock;
    double msWait = 0.0, msMain = 0.0, msPbr = 0.0;
    std::vector<std::shared_ptr<ReadyModel>> batch;
    std::unordered_set<std::string> seen;
    // A tela de carga anda com a cena (e a janela segue respondendo): parada
    // no 70% por 10-20 s, a troca de mapa parecia travada.
    // O contador e' de PRONTOS (leitura em paralelo terminada), nao da posicao
    // no laco: o 1o arquivo espera enquanto todos carregam juntos, entao pela
    // posicao ficava em 1-5 e pulava de uma vez para o total.
    std::vector<std::string> todo;
    {
        std::unordered_set<std::string> uniq;
        for (const std::string& f : files)
            if (!f.empty() && !m_modelRenderer.hasAsset(f) && !m_readyModels.count(f) && uniq.insert(f).second)
                todo.push_back(f);
    }
    const size_t total = todo.size();
    size_t k = 0;
    auto prontos = [&]() {
        size_t n = 0;
        for (const std::string& f : todo) {
            const auto pre = m_modelPreloads.find(f);
            if (pre == m_modelPreloads.end() || !pre->second.model.valid() ||
                pre->second.model.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                ++n;
        }
        return std::max(n, k);
    };
    auto lastDraw = clk::now();
    auto pump = [&](bool force) {
        const auto now = clk::now();
        if (!force && now - lastDraw < std::chrono::milliseconds(100)) return;
        lastDraw = now;
        const size_t done = prontos();
        const float frac = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 1.0f;
        presentLoadingScreen(m_currentMapName,
                             "Scene objects " + std::to_string(done) + "/" + std::to_string(total),
                             // 95-99%: roda depois do "Finalizing" (95%); daqui a barra voltava a 70%.
                             0.95f + 0.04f * frac, true);
    };
    pump(true);
    m_modelRenderer.resetResolveStats();
    for (const std::string& f : files) {
        if (f.empty() || !seen.insert(f).second || m_modelRenderer.hasAsset(f) || m_readyModels.count(f)) continue;
        ++k;
        const auto t0 = clk::now();
        if (const auto pre = m_modelPreloads.find(f); pre != m_modelPreloads.end() && pre->second.model.valid())
            while (pre->second.model.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) pump(false);
        pump(false);
        const auto t1 = clk::now();
        msWait += std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::shared_ptr<ModelFile> prepared = takePreparedModelFile(f);
        if (!prepared) continue;
        auto rm = std::make_shared<ReadyModel>();
        rm->file = f;
        m_currentMap->models.push_back(std::move(*prepared));
        ModelFile& model = m_currentMap->models.back();
        applyModelBake(model, f);
        rm->asset = convertModelToAsset(model);
        batch.push_back(std::move(rm));
        msMain += std::chrono::duration<double, std::milli>(clk::now() - t1).count();
    }
    if (batch.empty()) return;
    // PBR de todas as texturas da cena lido em paralelo ANTES dos slots (o
    // mesmo prewarm dos modelos do mapa). Um a um no fio principal, a primeira
    // abertura do mapa lia ~250 PNGs frios do disco: 32-43 s parados em
    // mapas de porto, 0,2 s na segunda abertura (cache do sistema).
    {
        const auto t0 = clk::now();
        std::vector<std::string> pbrNames;
        for (const auto& rm : batch)
            for (const auto& node : rm->asset.nodes) {
                for (const auto& n : node.pbrTextureNames) if (!n.empty()) pbrNames.push_back(n);
                for (const auto& n : node.textureNames) if (!n.empty()) pbrNames.push_back(n);
            }
        pump(true);
        eruption::prewarmPbrTextureData(pbrNames, false);
        msPbr = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    }
    const auto tSlots = clk::now();
    for (auto& rm : batch) {
        rm->slotUse = ModelRenderer::scanSlotUse(rm->asset);
        rm->slots = m_modelRenderer.resolveSlots(rm->asset, rm->slotUse);
    }
    eruption::clearPbrTextureDataPrewarm(); // o que sobrou (textura ja' na GPU)
    msMain += std::chrono::duration<double, std::milli>(clk::now() - tSlots).count();
    const auto tP = clk::now();
    const ModelRenderer::LodParams lod = m_modelRenderer.lodParams();
    JobSystem::instance().parallelFor(static_cast<uint32_t>(batch.size()), [&batch, &lod](uint32_t i) {
        batch[i]->prepared = ModelRenderer::prepareAsset(batch[i]->asset, std::move(batch[i]->slots), lod);
    });
    for (auto& rm : batch) m_readyModels[rm->file] = rm;
    if (kPhases) {
        const auto& rs = m_modelRenderer.resolveStats();
        ERUPTION_LOG_WARN("[SCENEPREP] texturas novas %u em %.0f ms, PBR %u em %.0f ms", rs.tex, rs.texMs, rs.pbr, rs.pbrMs);
    }
    if (kPhases)
        ERUPTION_LOG_WARN("[SCENEPREP] %zu modelos: espera preload %.0f ms, PBR em paralelo %.0f ms, texturas+slots %.0f ms, "
                          "malhas em paralelo %.0f ms",
                          batch.size(), msWait, msPbr, msMain, std::chrono::duration<double, std::milli>(clk::now() - tP).count());
    flushPendingUploads();
}

int Engine::addModelFromFile(const std::string& glbPath, const Vec3& position, const std::string& nodeName) {
    if (!m_currentMap) return -1;
    // ERUPTION_TEST_LOAD_PHASES=1: onde vai o tempo de um modelo posto por
    // script/cena (leitura+parse, conversao+envio, uploads pendentes).
    static const bool kPhases = std::getenv("ERUPTION_TEST_LOAD_PHASES") != nullptr;
    const auto tA = std::chrono::steady_clock::now();
    auto tB = tA, tC = tA, tConv = tA;
    const bool novo = !m_modelRenderer.hasAsset(glbPath);
    ModelInstanceDesc desc;
    desc.modelPath = glbPath;
    desc.position = position;
    desc.rotation = Vec3(0.0f);
    desc.scale = Vec3(1.0f);
    uint32_t first = 0xFFFFFFFFu;
    const auto ready = m_readyModels.find(glbPath);
    if (m_modelRenderer.hasAsset(glbPath)) {
        // Já carregado (o próprio mapa, ou outro modelo já acrescentado).
        first = m_modelRenderer.addInstanceOf(glbPath, desc, nodeName);
    } else if (ready != m_readyModels.end()) {
        // Montado na thread do mundo contínuo: texturas já resolvidas, malha e LOD prontos.
        std::shared_ptr<ReadyModel> rm = ready->second;
        m_readyModels.erase(ready);
        m_modelRenderer.providePrepared(glbPath, std::move(rm->prepared));
        m_modelRenderer.setMeshLodSuspended(true);
        first = m_modelRenderer.addModel(std::move(rm->asset), desc, nodeName);
        m_modelRenderer.setMeshLodSuspended(false);
        for (ModelFile& m : m_currentMap->models)
            if (m.filePath == glbPath)
                for (EmbeddedTexture& et : m.embeddedTextures)
                    if (et.hasEncodedData()) std::vector<uint8_t>().swap(et.pixels);
    } else {
        std::shared_ptr<ModelFile> prepared = takePreparedModelFile(glbPath);
        if (!prepared) {
            ERUPTION_LOG_ERROR("addModelFromFile: not a GLB file with meshes: %s", glbPath.c_str());
            return -1;
        }
        tB = std::chrono::steady_clock::now();
        // As texturas embutidas são procuradas nos modelos do mapa aberto.
        m_currentMap->models.push_back(std::move(*prepared));
        ModelFile& model = m_currentMap->models.back();
        applyModelBake(model, glbPath);
        ModelAsset asset = convertModelToAsset(model);
        tConv = std::chrono::steady_clock::now();
        first = m_modelRenderer.addModel(std::move(asset), desc, nodeName);
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
        ERUPTION_LOG_WARN("[MODELPHASE] %s: parse %.1f, converte+envia %.1f (converte %.1f), uploads %.1f ms", glbPath.c_str(), ms(tA, tB),
                          ms(tB, tC), tConv > tB ? ms(tB, tConv) : 0.0, ms(tC, tD));
    }
    if (first == 0xFFFFFFFFu) {
        ERUPTION_LOG_ERROR("addModelFromFile: nothing to add from %s%s%s", glbPath.c_str(),
                           nodeName.empty() ? "" : " node ", nodeName.c_str());
        return -1;
    }
    return static_cast<int>(first);
}

struct Engine::PreparedMap {
    std::string name;
    std::shared_ptr<LoadedMap> map;
    std::vector<ModelAsset> models; // convertidos de map->models (que fica sem a malha)
    // Nós montados com slots provisórios, por arquivo, e a assinatura da
    // geometria que eles viram.
    std::unordered_map<std::string, ModelRenderer::PreparedAsset> prepared;
    uint64_t geometryKey = 0;
    // Texturas embutidas ja' lidas do cache, por arquivo (readEmbeddedBakeCache).
    std::unordered_map<std::string, EmbeddedBakeResult> bakes;
};

// Leitura de um mapa fora da thread principal: os arquivos (MapLoader) e a
// conversao dos modelos, que nao dependem da GPU.
std::shared_ptr<Engine::PreparedMap> Engine::readPreparedMap(const std::string& name, bool deferPbr) {
    MapLoader loader;
    auto out = std::make_shared<PreparedMap>();
    out->name = name;
    out->map = loader.loadMap(name);
    if (!out->map) return nullptr;
    // Chave de conteudo dos probes (o loadMap da troca so' consulta a memoria).
    if (!out->map->models.empty())
        mapContentKey(project::gameRef(out->map->models.front().filePath).string(), name);
    // Cache das texturas embutidas: o arquivo (centenas de MB no mapa grande)
    // era lido na troca, na thread principal (137 ms num mapa de cidade).
    for (const auto& model : out->map->models) {
        EmbeddedBakeResult bake;
        if (readEmbeddedBakeCache(model, embeddedTextureMaxSize(), pbrSynthesisEnabled(), bake, deferPbr)) {
            out->bakes.emplace(model.filePath, std::move(bake));
        } else if (!model.embeddedTextures.empty()) {
            // Sem pacote (mapa nunca aberto, ou o .glb mudou depois dele): comprime e
            // sintetiza AQUI, na thread de fundo, com o mapa antigo ainda jogavel. Antes
            // isso rodava na troca, no fio principal: 2,2 s parados num mapa de 154
            // texturas (medido pelo teste de 15 warps).
            bake = bakeEmbeddedTextures(model, embeddedTextureMaxSize(), pbrSynthesisEnabled(), deferPbr);
            if (!bake.textures.empty()) out->bakes.emplace(model.filePath, std::move(bake));
        }
    }
    out->map->geometryMinY = glbGeometryMinY(*out->map); // antes de a conversao liberar a malha
    out->models.reserve(out->map->models.size());
    for (auto& model : out->map->models) out->models.push_back(convertModelToAssetReleasing(model));
    // Vertices, solda, celulas do chao e costuras: so' CPU, sem textura
    // resolvida (o envio troca os slots provisorios pelos reais).
    out->geometryKey = mapGeometryKey(out->models);
    for (const ModelAsset& model : out->models)
        out->prepared.emplace(model.filePath, ModelRenderer::prepareAssetProxy(model));
    return out;
}

bool Engine::preloadMap(const std::string& name) {
    auto known = [this, &name] {
        return std::find(m_availableMaps.begin(), m_availableMaps.end(), name) != m_availableMaps.end();
    };
    // Mapa que chegou depois da partida (pacote baixado com o jogo rodando, no
    // navegador): relê a pasta só quando falta, então o caso comum não paga nada.
    if (!known()) populateMaps();
    if (!known()) {
        ERUPTION_LOG_WARN("Preload: map %s not found", name.c_str());
        return false;
    }
    if (name == m_currentMapName) return true;
    m_preloadWanted = name;
    updatePreload();
    // O que a cena e o scatter do destino acrescentam, lido numa thread; o
    // updatePreload dispara os preloads quando a lista chega.
    if (m_preloadListJob.valid() && m_preloadListMap != name &&
        m_preloadListJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        parkJob(m_preloadListJob); // lista de outro mapa ainda lendo: solta, nao trava a deste
    if (!m_preloadListJob.valid() && m_preloadListMap != name) {
        m_preloadListMap = name;
        m_preloadListModels.clear();
        m_preloadListJob = platform::runAsync([this, name] { return readPreloadLists(name); });
    }
    return true;
}

// Arquivos que a cena e o scatter de `name` acrescentam ao mapa. So' le':
// roda fora do fio principal.
Engine::PreloadLists Engine::readPreloadLists(const std::string& name) const {
    PreloadLists out;
    SceneEdits edits;
    std::string error;
    if (edits.load(SceneEdits::pathFor(name), error)) {
        // Camadas de terreno: a decodificacao dos PNGs era metade dos ~530 ms
        // de [SCENEPHASE] camadas na chegada (8 camadas, 24 imagens).
        out.layers = edits.terrainLayers;
        // Modelos acrescentados: o [SCENEPHASE] modelos da chegada era
        // leitura+conversao de GLB novo no fio principal (~300 ms num mapa de 3400 u).
        // Prefab fica de fora (o arquivo vem dele).
        for (const SceneEdits::Added& a : edits.added)
            if (!a.file.empty()) out.models.push_back(a.file);
    }
    // E os do scatter (pincel de objetos): o [MAPPHASE] scatter da chegada era
    // o mesmo custo, um GLB novo por grupo no fio principal.
    for (std::string& f : scatterFiles(name)) out.models.push_back(std::move(f));
    return out;
}

// Uma leitura por vez: o pedido que chega durante outra espera ela acabar;
// o resultado de um nome que ninguem mais quer e' jogado fora.
void Engine::updatePreload() {
    if (m_preloadListJob.valid() && m_preloadListJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const PreloadLists lists = m_preloadListJob.get();
        if (!lists.layers.empty()) preloadSplatLayers(lists.layers);
        for (const std::string& f : lists.models) preloadModel(f);
        m_preloadListModels = lists.models;
    }
    if (m_preloadJob.valid() && m_preloadJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::shared_ptr<PreparedMap> done = m_preloadJob.get();
        if (!done) {
            ERUPTION_LOG_ERROR("Preload: could not read map %s", m_preloadWanted.c_str());
            m_preloadWanted.clear();
        } else if (done->name == m_preloadWanted) {
            m_preloaded = std::move(done);
            ERUPTION_LOG_WARN("Preload: %s ready for the warp", m_preloaded->name.c_str());
        }
    }
    const bool haveWanted = m_preloaded && m_preloaded->name == m_preloadWanted;
    if (!m_preloadJob.valid() && !m_preloadWanted.empty() && !haveWanted) {
        m_preloaded.reset();
        const std::string map = m_preloadWanted;
        const bool defer = !m_editorMode && PbrDeferred::enabled(&m_bindless);
        m_preloadJob = platform::runAsync([map, defer] { return readPreparedMap(map, defer); });
        m_preloadJobMap = map;
        ERUPTION_LOG_WARN("Preload: reading %s in the background", map.c_str());
    }
}

// ERUPTION_TEST_PRELOAD_NEXT=1 (teste, com ERUPTION_TEST_TOUR): ao chegar num
// mapa, pre-carrega o seguinte de ERUPTION_TOUCH_MAPS, como um jogo faria com
// a proxima area.
static std::string testPreloadTarget(const std::string& current) {
    static const bool kOn = std::getenv("ERUPTION_TEST_PRELOAD_NEXT") != nullptr;
    const char* list = std::getenv("ERUPTION_TOUCH_MAPS");
    if (!kOn || !list || current.empty()) return {};
    std::vector<std::string> maps;
    std::stringstream ss(list);
    for (std::string item; std::getline(ss, item, ',');)
        if (!item.empty()) maps.push_back(item);
    const auto it = std::find(maps.begin(), maps.end(), current);
    if (it == maps.end() || maps.size() < 2) return {};
    return std::next(it) == maps.end() ? maps.front() : *std::next(it);
}

// Troca de mapa com fade: a tela escurece enquanto o mapa e' lido em segundo
// plano, a troca acontece com ela preta e depois clareia (sem o salto seco de
// um mapa para o outro). ERUPTION_WARP_FADE=0 volta a troca seca.
void Engine::beginWarpFade() {
    static const bool kOff = [] {
        const char* e = std::getenv("ERUPTION_WARP_FADE");
        return e && e[0] == '0';
    }();
    if (kOff || m_editorMode) return;
    constexpr float kFadeOutSeconds = 0.35f;
    m_uiCanvas.screenFade(Vec4(0.0f, 0.0f, 0.0f, 1.0f), m_uiCanvas.fadeAlpha(), 1.0f, kFadeOutSeconds);
    m_warpFading = true;
}

void Engine::endWarpFade() {
    if (!m_warpFading) return;
    constexpr float kFadeInSeconds = 0.5f;
    m_uiCanvas.screenFade(Vec4(0.0f, 0.0f, 0.0f, 1.0f), 1.0f, 0.0f, kFadeInSeconds);
    m_warpFading = false;
}

void Engine::reapAbandonedJobs() {
    for (size_t i = 0; i < m_abandonedJobs.size();) {
        if (m_abandonedJobs[i]()) m_abandonedJobs.erase(m_abandonedJobs.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

// O warp mais novo sempre ganha: o anterior perde o resultado e os jobs dele
// vao para o cemiterio (nao se espera um mapa pesado so' para descarta-lo).
void Engine::abandonWarp() {
    ERUPTION_LOG_WARN("Warp: %s abandoned, %s requested", m_warpJobMap.c_str(), m_pendingMapWarp.c_str());
    parkJob(m_warpJob);
    parkJob(m_warpPbrWarm);
    parkJob(m_preloadListJob);
    m_warpJobActive = false;
    m_preloadListMap.clear();
    m_preloadListModels.clear();
    m_warpPbrWarmMap.clear();
    setWorldCacheTrusted(false);
}

void Engine::updateWarp() {
    reapAbandonedJobs();
    if (m_warpJobActive && !m_pendingMapWarp.empty()) abandonWarp();
    if (!m_warpJobActive && m_pendingMapWarp.empty()) {
        const std::string next = testPreloadTarget(m_currentMapName);
        if (!next.empty() && next != m_preloadWanted && !(m_preloaded && m_preloaded->name == next)) preloadMap(next);
    }
#ifndef ERUPTION_DEV_DISABLED
    // ERUPTION_TEST_PRELOAD_WARP="A,B[,C]" (teste; fora do build final): no primeiro
    // quadro pre-carrega o mapa A e logo pede o warp para B (e, com C, 30 quadros depois para C, com o warp de B em voo), a corrida
    // da tela de login (servidor manda o personagem para outro mapa).
    static const char* const kRace = std::getenv("ERUPTION_TEST_PRELOAD_WARP");
    static bool s_raceDone = false;
    static std::string s_raceLast;
    static int s_raceFrames = 0;
    if (kRace && !s_raceDone && !m_currentMapName.empty()) {
        s_raceDone = true;
        const std::string spec = kRace;
        const size_t comma = spec.find(',');
        if (comma != std::string::npos) {
            preloadMap(spec.substr(0, comma));
            const size_t c2 = spec.find(',', comma + 1);
            m_pendingMapWarp = spec.substr(comma + 1, c2 == std::string::npos ? c2 : c2 - comma - 1);
            // "C,A,B": B (o ultimo) e' pedido s_raceFrames quadros depois, com o warp de A em voo.
            if (c2 != std::string::npos) { s_raceLast = spec.substr(c2 + 1); s_raceFrames = 30; }
        }
    }
    if (s_raceFrames > 0 && --s_raceFrames == 0) m_pendingMapWarp = s_raceLast;
#endif
    updatePreload();
    // Um pedido por vez: outro pedido durante a leitura espera ela acabar e
    // o resultado velho é jogado fora. O que o preloadMap ja' leu (ou esta'
    // lendo) para o mesmo mapa vira a leitura do warp.
    if (!m_warpJobActive && !m_pendingMapWarp.empty()) {
        m_warpJobMap = m_pendingMapWarp;
        m_pendingMapWarp.clear();
        m_warpJobActive = true;
        const std::string map = m_warpJobMap;
        const bool defer = !m_editorMode && PbrDeferred::enabled(&m_bindless);
        if (m_preloaded && m_preloaded->name == map) {
            std::promise<std::shared_ptr<PreparedMap>> ready;
            ready.set_value(std::move(m_preloaded));
            m_warpJob = ready.get_future();
            m_preloadWanted.clear();
            ERUPTION_LOG_WARN("Warp: %s already read by preload", map.c_str());
        } else if (m_preloadJob.valid() && m_preloadJobMap == map) {
            // So' adota a leitura que e' deste mapa: com a pre-carga de outro
            // mapa ainda rodando (preset lento), adotar trazia o terreno errado.
            m_warpJob = std::move(m_preloadJob);
            m_preloadWanted.clear();
            ERUPTION_LOG_WARN("Warp: waiting for the preload of %s", map.c_str());
        } else if (preloadMap(map) && m_preloadJob.valid() && m_preloadJobMap == map) {
            // Warp sem preload antes (console, world.load_map): vira um preload
            // agora, que le' o mapa E os modelos da cena dele numa thread.
            m_warpJob = std::move(m_preloadJob);
            m_preloadWanted.clear();
            ERUPTION_LOG_WARN("Warp: reading %s and its scene in the background", map.c_str());
        } else {
            // Pre-carga de outro mapa em curso: o warp le' o seu, e o pedido
            // da pre-carga (agora do mapa do warp) sai para nao reler depois.
            if (m_preloadWanted == map) m_preloadWanted.clear();
            m_warpJob = platform::runAsync([map, defer] { return readPreparedMap(map, defer); });
            ERUPTION_LOG_WARN("Warp: reading %s in the background", map.c_str());
        }
        beginWarpFade();
        return;
    }
    if (!m_warpJobActive || m_warpJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    // Modelos da cena do destino ainda no disco: espera aqui, com os quadros
    // andando. Na chegada a cena esperava por eles parada no fio principal:
    // warp a frio pelo console, um quadro de 39,7 s (mapa urbano grande).
    if (m_preloadListMap == m_warpJobMap) {
        if (m_preloadListJob.valid()) return;
        for (const std::string& f : m_preloadListModels) {
            const auto it = m_modelPreloads.find(f);
            if (it != m_modelPreloads.end() && it->second.model.valid() &&
                it->second.model.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
        }
        if (m_warpPbrWarmMap != m_warpJobMap) {
            m_warpPbrWarmMap = m_warpJobMap;
            parkJob(m_warpPbrWarm);
            std::vector<std::string> names;
            for (const std::string& f : m_preloadListModels) {
                const auto it = m_modelPreloads.find(f);
                if (it == m_modelPreloads.end() || !it->second.model.valid()) continue;
                const std::shared_ptr<ModelFile> model = it->second.model.get();
                if (!model) continue;
                for (const auto& node : model->nodes) {
                    for (const auto& n : node.pbrTextureNames) if (!n.empty()) names.push_back(n);
                    for (const auto& n : node.textureNames) if (!n.empty()) names.push_back(n);
                }
            }
            // Cena e scatter do destino lidos aqui e fixados no vfs: a chegada
            // (SceneEdits, loadScatter, startWorld) os acha na memoria.
            const std::string scenePath = SceneEdits::pathFor(m_warpJobMap).string();
            std::filesystem::path scatterPath = SceneEdits::pathFor(m_warpJobMap);
            scatterPath.replace_extension();
            scatterPath.replace_extension(".scatter.luau");
            m_warpPbrWarm = platform::runAsync([names, scenePath, scatterPath = scatterPath.string()] {
                vfs::clearPinnedText();
                for (const std::string& file : {scenePath, scatterPath}) {
                    std::string text;
                    const bool found = vfs::readText(file, text);
                    vfs::pinText(file, found, std::move(text));
                }
                warmWorldGraphCache();
                std::vector<char> buf(1 << 20);
                for (const std::string& n : names) {
                    const PbrTexturePathPair paths = resolvePbrTexturePaths(n);
                    if (!paths.valid) continue;
                    // O que a carga abre e' o .etex ao lado (o PNG e' a fonte).
                    for (const std::string& file : {paths.mrahw + ".etex", paths.normal + ".etex"}) {
                        std::ifstream in(file, std::ios::binary);
                        while (in.read(buf.data(), static_cast<std::streamsize>(buf.size()))) {}
                    }
                }
            });
            return;
        }
        if (m_warpPbrWarm.valid() && m_warpPbrWarm.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        setWorldCacheTrusted(true); // desliga no fim do startWorld
    }
    if (m_warpFading && m_uiCanvas.fading()) return; // troca so' com a tela ja' preta
    std::shared_ptr<PreparedMap> prepared = m_warpJob.get();
    m_warpJobActive = false;
    const std::string name = m_warpJobMap;
    if (!m_pendingMapWarp.empty()) return; // pediram outro mapa no meio: começa no próximo frame
    if (!prepared) {
        ERUPTION_LOG_ERROR("Warp: could not read map %s", name.c_str());
        return;
    }
    // Nenhum resultado de carga vale se o mapa dele nao e' o alvo do warp.
    if (prepared->name != name) {
        ERUPTION_LOG_ERROR("Warp: result is for %s, target is %s; discarded", prepared->name.c_str(), name.c_str());
        return;
    }
    if (m_preloaded && m_preloaded->name == name) m_preloaded.reset();
    m_convertedModels = std::move(prepared->models);
    m_convertedPrepared = std::move(prepared->prepared);
    m_convertedKey = prepared->geometryKey;
    m_convertedBakes = std::move(prepared->bakes);
    const auto t0 = std::chrono::steady_clock::now();
    const bool loaded = loadMap(name, std::move(prepared->map));
    endWarpFade();
    m_convertedBakes.clear(); // o que o mapa nao usou (arquivo sem textura embutida)
    if (!loaded) {
        m_convertedModels.clear();
        m_convertedPrepared.clear();
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (m_preloadListMap == name) { // lista usada; a proxima ida a este mapa le' de novo
        m_preloadListMap.clear();
        m_preloadListModels.clear();
    }
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

namespace eruption {
// Diagnostico da memoria que atravessa a troca de mapa (ERUPTION_TEST_MAP_SEQUENCE
// chama a cada troca): quantos itens cada cache guarda e, onde da' para somar,
// quantos MB de CPU.
void Engine::logMemoryCaches() const {
    size_t preloadPronto = 0, preloadMb = 0;
    for (const auto& kv : m_modelPreloads) {
        const auto& f = kv.second.model;
        if (f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready && f.get()) {
            ++preloadPronto;
            for (const auto& t : f.get()->embeddedTextures) preloadMb += t.pixels.size() + t.encodedData.size();
        }
    }
    size_t readyMb = 0;
    for (const auto& kv : m_readyModels)
        if (kv.second && kv.second->model)
            for (const auto& t : kv.second->model->embeddedTextures) readyMb += t.pixels.size() + t.encodedData.size();
    size_t splatMb = 0;
    for (const auto& kv : m_splatDecoded) splatMb += kv.second.pixels.size();
    ERUPTION_LOG_WARN("[MEMCACHE] preloads %zu (%zu prontos, %.0f MB img) | ready %zu (%.0f MB img) | assetCache tex %zu mesh %zu ter %zu | "
                      "convertedBakes %zu prepared %zu embeddedBaked %zu | sprites %zu | splat %zu (%.0f MB) | meshes %zu",
                      m_modelPreloads.size(), preloadPronto, preloadMb / 1048576.0, m_readyModels.size(), readyMb / 1048576.0,
                      m_assetCache.modelTextures.size(), m_assetCache.modelMeshes.size(), m_assetCache.terrainTextures.size(),
                      m_convertedBakes.size(), m_convertedPrepared.size(), m_embeddedBaked.size(),
                      m_characterSpriteCache.size(), m_splatDecoded.size(), splatMb / 1048576.0, m_modelRenderer.meshCount());
}
} // namespace eruption
