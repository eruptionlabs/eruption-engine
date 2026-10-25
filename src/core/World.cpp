// Mundo contínuo: mapas vizinhos emendados ao mapa aberto.
//
// Cada mapa declara os vizinhos no próprio scene.luau
//   neighbors = { { map = "floresta", offset = {1000, 0, 0} } }
// e a ligação vale nos dois sentidos. A partir do mapa aberto o motor monta o
// grafo (vizinhos dos vizinhos, com o deslocamento somado) e, conforme o
// jogador (ou a câmera do editor) chega perto da borda de um deles:
//   - lê o mapa numa thread (GLB, grade, caminhada, retoques, cena);
//   - encaixa no frame: modelos deslocados, objetos acrescentados e espalhados,
//     personagens, luzes, água e camadas pintadas;
//   - quando ele se afasta, tira de cena (instâncias desligadas, personagens
//     parados, luz e água fora) e devolve na volta sem ler de novo.
// O mapa aberto continua sendo o que o editor edita e salva; os vizinhos
// aparecem travados, para dar contexto.
#include "core/Engine.hpp"
#include "core/Platform.hpp"

#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "formats/MapLoader.hpp"
#include "formats/ModelDataConverter.hpp"
#include "formats/TerrainParser.hpp"
#include "game/PlayerController.hpp"
#include "scene/SceneEdits.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>

namespace eruption {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxRegions = 64;
constexpr int kMaxJobs = 2;

// Textura embutida "#imagem" passa a "#<mapa>|imagem": GLBs diferentes usam os
// mesmos nomes e o cache de texturas é por nome.
void namespaceEmbedded(ModelFile& model, const std::string& map) {
    auto rename = [&](std::string& n) {
        if (!n.empty() && n[0] == '#') n = "#" + map + "|" + n.substr(1);
    };
    for (auto& et : model.embeddedTextures) rename(et.name);
    for (auto& t : model.textures) rename(t);
    for (auto& node : model.nodes)
        for (auto& t : node.textureNames) rename(t);
}

bool inside(const Vec2& lo, const Vec2& hi, float x, float z) {
    return x >= lo.x && z >= lo.y && x <= hi.x && z <= hi.y;
}

float distanceToArea(const Vec2& lo, const Vec2& hi, float x, float z) {
    const float dx = std::max({lo.x - x, 0.0f, x - hi.x});
    const float dz = std::max({lo.y - z, 0.0f, z - hi.y});
    return std::sqrt(dx * dx + dz * dz);
}

bool groundArea(const TerrainFile& t, Vec2& lo, Vec2& hi) {
    if (t.width < 2 || t.height < 2) return false;
    const float s = t.scale > 0.0f ? t.scale : 10.0f;
    lo = Vec2(t.offsetX, t.offsetZ);
    hi = lo + Vec2(float(t.width - 1) * s, float(t.height - 1) * s);
    return true;
}

} // namespace

struct Engine::WorldRegionLoad {
    std::shared_ptr<LoadedMap> map;
    std::vector<ModelAsset> models;
    // Montagem das malhas fora da thread principal: quais texturas cada nó
    // usa (thread de leitura), os slots delas (principal) e a malha pronta.
    std::vector<std::vector<ModelRenderer::NodeSlotUse>> slotUse;
    std::vector<std::vector<ModelRenderer::NodeSlots>> slots;
    std::vector<ModelRenderer::PreparedAsset> prepared;
    SceneEdits edits;
    std::string error;
};

bool Engine::mapGroundArea(const std::string& map, Vec2& lo, Vec2& hi) {
    // Só o cabeçalho do .ter (o mesmo lugar onde o MapLoader procura).
    for (const fs::path& p : {project::game("assets/external") / map / (map + ".ter"),
                              project::game("assets/data") / (map + ".ter"),
                              project::game("assets/models") / (map + ".ter")}) {
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        char magic[11] = {};
        uint32_t version = 0, w = 0, d = 0;
        float scale = 0.0f, ox = 0.0f, oz = 0.0f;
        f.read(magic, 11);
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&w), 4);
        f.read(reinterpret_cast<char*>(&d), 4);
        f.read(reinterpret_cast<char*>(&scale), 4);
        if (version >= 2) {
            f.read(reinterpret_cast<char*>(&ox), 4);
            f.read(reinterpret_cast<char*>(&oz), 4);
        }
        if (!f || std::string(magic, 11) != "ERUPTGNDBIN") continue;
        TerrainFile t;
        t.width = w;
        t.height = d;
        t.scale = scale;
        t.offsetX = ox;
        t.offsetZ = oz;
        return groundArea(t, lo, hi);
    }
    return false;
}

void Engine::setWorldNeighbors(std::vector<WorldNeighbor> neighbors) {
    m_worldNeighbors = std::move(neighbors);
    if (m_worldStarted) restartWorld();
}

void Engine::resetWorld() {
    // Troca de mapa: o renderizador já foi limpo; só sobra o que é nosso.
    for (auto& r : m_regions) {
        if (r.job.valid()) r.job.wait();
        releaseRegionSplat(r);
    }
    m_regions.clear();
    m_worldNeighbors.clear();
    m_worldStarted = false;
    m_deferredLighting.setExtraPointLights({});
}

void Engine::restartWorld() {
    for (auto& r : m_regions)
        if (r.state == WorldRegion::State::Shown) showRegion(r, false);
    // Os que já estão no mundo continuam valendo se o vizinho é o mesmo.
    std::vector<WorldRegion> old = std::move(m_regions);
    m_regions.clear();
    buildWorldGraph();
    for (auto& r : m_regions)
        for (auto& o : old)
            if (!o.map.empty() && o.map == r.map && o.offset == r.offset && o.state == WorldRegion::State::Hidden) {
                r = std::move(o);
                o.map.clear();
            }
    for (auto& o : old) {
        if (o.map.empty()) continue;
        if (o.job.valid()) o.job.wait();
        // Mapa que saiu do mundo: instâncias ficam desligadas para sempre.
        for (auto& c : m_characters)
            if (std::find(o.characters.begin(), o.characters.end(), c.name) != o.characters.end()) c.removed = true;
        releaseRegionSplat(o);
    }
}

void Engine::startWorld() {
    m_worldNeighbors.clear();
    SceneEdits e;
    std::string error;
    if (e.load(SceneEdits::pathFor(m_currentMapName), error))
        m_worldNeighbors = e.neighbors;
    if (m_worldStarted) restartWorld(); // arquivo de cena relido
    else buildWorldGraph();
}

void Engine::buildWorldGraph() {
    m_worldStarted = true;
    m_regions.clear();
    if (m_currentMapName.empty()) return;
    // Vizinhos de cada mapa: os do próprio arquivo e os que apontam para ele.
    std::map<std::string, std::vector<WorldNeighbor>> links;
    auto addLink = [&](const std::string& a, const std::string& b, const Vec3& off) {
        if (a == b) return;
        auto& v = links[a];
        for (const auto& n : v)
            if (n.map == b) return;
        v.push_back({b, off});
    };
    std::error_code ec;
    const fs::path dir = SceneEdits::pathFor("x").parent_path();
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it) {
        const std::string fn = it->path().filename().string();
        const std::string suffix = ".scene.luau";
        if (fn.size() <= suffix.size() || fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const std::string map = fn.substr(0, fn.size() - suffix.size());
        if (map == m_currentMapName) continue; // o aberto vale pelo que está na memória
        SceneEdits e;
        std::string error;
        if (!SceneEdits::fileMentions(map, "neighbors") || !e.load(it->path(), error)) continue;
        for (const auto& n : e.neighbors) {
            addLink(map, n.map, n.offset);
            addLink(n.map, map, -n.offset);
        }
    }
    for (const auto& n : m_worldNeighbors) {
        addLink(m_currentMapName, n.map, n.offset);
        addLink(n.map, m_currentMapName, -n.offset);
    }
    // Do aberto para fora, somando os deslocamentos.
    std::map<std::string, Vec3> placed{{m_currentMapName, Vec3(0.0f)}};
    std::deque<std::string> queue{m_currentMapName};
    while (!queue.empty() && m_regions.size() < kMaxRegions) {
        const std::string at = queue.front();
        queue.pop_front();
        for (const auto& n : links[at]) {
            if (placed.count(n.map)) continue;
            const Vec3 off = placed[at] + n.offset;
            placed[n.map] = off;
            queue.push_back(n.map);
            WorldRegion r;
            r.map = n.map;
            r.offset = off;
            if (!mapGroundArea(n.map, r.boundsMin, r.boundsMax)) {
                ERUPTION_LOG_WARN("World: neighbor map %s has no height grid (.ter); left out", n.map.c_str());
                continue;
            }
            r.boundsMin += Vec2(off.x, off.z);
            r.boundsMax += Vec2(off.x, off.z);
            m_regions.push_back(std::move(r));
        }
    }
    if (!m_regions.empty())
        ERUPTION_LOG_WARN("World: %zu neighbor map(s) around %s", m_regions.size(), m_currentMapName.c_str());
}

void Engine::updateWorld() {
    if (!m_worldStarted || m_regions.empty() || !m_currentMap) return;
    // Perto de quem: do jogador no jogo; da câmera no editor.
    Vec3 focus = m_camera.position();
    if (m_playerController && m_playerSpawnedOnActiveMap && !m_editorMode) focus = m_playerController->pos();
    int jobs = 0;
    for (const auto& r : m_regions) jobs += r.state == WorldRegion::State::Loading;
    // ERUPTION_TEST_WORLD_RELEASE_SECONDS (teste): encurta o tempo até liberar.
    static const char* testRelease = std::getenv("ERUPTION_TEST_WORLD_RELEASE_SECONDS");
    const float releaseAfter = testRelease ? float(std::atof(testRelease)) : worldReleaseSeconds;
    bool attachedThisFrame = false;
    for (auto& r : m_regions) {
        const float dist = distanceToArea(r.boundsMin, r.boundsMax, focus.x, focus.z);
        const bool near = dist < worldLoadDistance;
        const bool far = dist > worldLoadDistance * 1.5f + 50.0f;
        switch (r.state) {
        case WorldRegion::State::Idle:
            if (near && jobs < kMaxJobs) {
                const std::string map = r.map;
                const Vec3 off = r.offset;
                r.job = platform::runAsync([map, off] {
                    auto out = std::make_shared<WorldRegionLoad>();
                    MapLoader loader;
                    out->map = loader.loadMap(map);
                    if (!out->map || !out->map->isGlbMap || !out->map->terrainFromFile) {
                        out->error = "only maps with a height grid (.ter) can be neighbors";
                        out->map.reset();
                        return out;
                    }
                    for (auto& m : out->map->models) {
                        namespaceEmbedded(m, map);
                        out->models.push_back(convertModelToAsset(m));
                        out->slotUse.push_back(ModelRenderer::scanSlotUse(out->models.back()));
                    }
                    HeightEdit h;
                    applyHeightDelta(*out->map, out->models, h);
                    applySavedWalk(*out->map);
                    // Grade no lugar do mundo (a altura guardada é negativa).
                    TerrainFile& t = out->map->terrain;
                    t.offsetX += off.x;
                    t.offsetZ += off.z;
                    if (off.y != 0.0f)
                        for (auto& c : t.cubes)
                            for (float& hh : c.height) hh -= off.y;
                    std::string error;
                    if (!out->edits.load(SceneEdits::pathFor(map), error)) ERUPTION_LOG_ERROR("%s", error.c_str());
                    return out;
                });
                r.state = WorldRegion::State::Loading;
                r.slotsResolved = false;
                ++jobs;
            }
            break;
        case WorldRegion::State::Loading:
            if (!attachedThisFrame && r.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                std::shared_ptr<WorldRegionLoad> load = r.job.get();
                if (!load || !load->map) {
                    ERUPTION_LOG_ERROR("World: neighbor map %s: %s", r.map.c_str(),
                                       load ? load->error.c_str() : "load failed");
                    r.state = WorldRegion::State::Failed;
                    break;
                }
                attachedThisFrame = true; // um passo pesado por frame
                if (!r.slotsResolved) {
                    // Lido: texturas viram slots aqui e a malha (vértices,
                    // solda, células, costuras, LOD) é montada em outra thread.
                    resolveRegionSlots(*load);
                    const ModelRenderer::LodParams lod = m_modelRenderer.lodParams();
                    r.job = platform::runAsync([load, lod] {
                        for (size_t i = 0; i < load->models.size() && i < load->slots.size(); ++i)
                            load->prepared.push_back(
                                ModelRenderer::prepareAsset(load->models[i], std::move(load->slots[i]), lod));
                        return load;
                    });
                    r.slotsResolved = true;
                    break;
                }
                attachRegion(r, *load);
                if (far) showRegion(r, false);
            }
            break;
        case WorldRegion::State::Shown:
            if (far) showRegion(r, false);
            break;
        case WorldRegion::State::Hidden:
            if (near) {
                showRegion(r, true);
            } else if (std::chrono::steady_clock::now() - r.hiddenAt > std::chrono::duration<float>(releaseAfter)) {
                releaseRegion(r); // longe há tempo: devolve a memória (lê de novo se voltar)
            }
            break;
        case WorldRegion::State::Failed:
            break;
        }
    }
}

void Engine::resolveRegionSlots(WorldRegionLoad& load) {
    // Texturas embutidas: procuradas nos modelos do mapa aberto (só as
    // imagens; a malha não precisa ficar).
    for (auto& m : load.map->models) {
        ModelFile images;
        images.filePath = m.filePath;
        images.embeddedTextures = std::move(m.embeddedTextures);
        m.nodes.clear();
        if (!images.embeddedTextures.empty() && m_currentMap) m_currentMap->models.push_back(std::move(images));
    }
    for (size_t i = 0; i < load.models.size() && i < load.slotUse.size(); ++i)
        load.slots.push_back(m_modelRenderer.resolveSlots(load.models[i], load.slotUse[i]));
}

void Engine::attachRegion(WorldRegion& r, WorldRegionLoad& load) {
    const auto t0 = std::chrono::steady_clock::now();
    r.data = load.map;
    ModelRenderer& mr = m_modelRenderer;
    const SceneEdits& e = load.edits;
    const Mat4 place = glm::translate(Mat4(1.0f), r.offset);
    const std::string prefix = r.map + ":";
    r.firstInstance = static_cast<uint32_t>(mr.instanceCount());

    // Modelos do mapa, deslocados; a malha chega montada (resolveRegionSlots).
    const bool keep = mr.keepGroundVertices();
    mr.setKeepGroundVertices(true); // camadas e bordas costuradas mexem nos vértices
    mr.setMeshLodSuspended(true);   // o LOD veio da thread; simplificar aqui travava o frame
    for (size_t i = 0; i < load.models.size(); ++i) {
        const std::string file = load.models[i].filePath;
        if (i < load.prepared.size()) mr.providePrepared(file, std::move(load.prepared[i]));
        ModelInstanceDesc desc;
        desc.modelPath = file;
        desc.position = r.offset;
        desc.rotation = Vec3(0.0f);
        desc.scale = Vec3(1.0f);
        mr.addModel(std::move(load.models[i]), desc);
        r.modelFiles.push_back(file);
    }
    flushPendingUploads();
    mr.setMeshLodSuspended(false);
    mr.setKeepGroundVertices(keep);
    const auto tModels = std::chrono::steady_clock::now();
    const uint32_t mapEnd = static_cast<uint32_t>(mr.instanceCount());
    for (uint32_t i = r.firstInstance; i < mapEnd; ++i) mr.setInstanceName(i, prefix + mr.getInstances()[i].name);
    auto findInRegion = [&](const std::string& name) -> int {
        const auto& insts = mr.getInstances();
        for (uint32_t i = r.firstInstance; i < insts.size(); ++i)
            if (insts[i].name == prefix + name) return static_cast<int>(i);
        return -1;
    };

    // Edições da cena do vizinho, deslocadas.
    for (const auto& a : e.added) {
        std::string file = a.file, node = a.node;
        Mat4 inner(1.0f);
        if (!a.prefab.empty()) {
            Prefab p;
            std::string error;
            if (!p.load(a.prefab, error)) continue;
            file = p.file;
            node = p.node;
            inner = p.matrix();
        }
        SceneEdits::AddedInstance out;
        SceneEdits::addModel(*this, file, node, prefix + a.name, place * a.transform.matrix(), inner, out);
    }
    for (const auto& ob : e.objects) {
        const int idx = findInRegion(ob.name);
        if (idx < 0) continue;
        if (ob.hasTransform) mr.setInstanceTransform(static_cast<uint32_t>(idx), place * ob.transform.matrix());
        if (ob.hidden) mr.setInstanceEnabled(static_cast<uint32_t>(idx), false);
    }
    for (const auto& c : e.copies) {
        const int src = findInRegion(c.from);
        if (src >= 0) mr.duplicateInstance(static_cast<uint32_t>(src), prefix + c.name, place * c.transform.matrix());
    }
    const auto tScene = std::chrono::steady_clock::now();
    // Objetos espalhados: lista à parte (o pincel do editor é do mapa aberto).
    {
        std::vector<ScatterGroup> mine;
        std::swap(m_scatter, mine);
        loadScatter(r.map, place);
        std::swap(m_scatter, mine);
    }
    const auto tScatter = std::chrono::steady_clock::now();
    buildRegionSplat(r, e.terrainLayers);
    const auto tSplat = std::chrono::steady_clock::now();
    r.endInstance = static_cast<uint32_t>(mr.instanceCount());

    // Luzes do mapa (GLB: coordenada de mundo do próprio mapa) e as da cena.
    r.lights.clear();
    for (const auto& ml : r.data->lights) {
        PointLight pl;
        pl.position = ml.position + r.offset;
        pl.color = ml.color;
        pl.intensity = ml.intensity;
        pl.radius = ml.range;
        pl.animType = ml.animType;
        pl.nightOnly = ml.nightOnly;
        pl.enabled = !ml.nightOnly;
        pl.spotDir = ml.spotDir;
        pl.spotCos = std::cos(glm::radians(glm::clamp(ml.spotAngle, 1.0f, 179.0f) * 0.5f));
        r.lights.push_back(pl);
    }
    for (const auto& l : e.lights) {
        if (l.index < 1 || l.index > 1024) continue;
        if (l.index > static_cast<int>(r.lights.size())) r.lights.resize(static_cast<size_t>(l.index));
        PointLight& p = r.lights[static_cast<size_t>(l.index - 1)];
        p.position = l.position + r.offset;
        p.color = l.color;
        p.intensity = l.intensity;
        p.radius = l.range;
        p.enabled = l.enabled;
    }
    r.characters.clear();
    for (const auto& c : e.characters) {
        const std::string name = prefix + c.name;
        if (findCharacter(name) >= 0) continue;
        const int idx = addCharacter(name, c.sprite, c.position + r.offset, c.facing);
        m_characters[static_cast<size_t>(idx)].visible = !c.hidden;
        m_characters[static_cast<size_t>(idx)].fromNeighbor = true;
        m_characters[static_cast<size_t>(idx)].ground = m_characters[static_cast<size_t>(idx)].groundPending = c.ground;
        m_characters[static_cast<size_t>(idx)].groundFromY = c.position.y + r.offset.y;
        r.characters.push_back(name);
    }
    r.areas.clear();
    for (const auto& a : e.areas) {
        const std::string name = prefix + a.name;
        if (findArea(name) >= 0) continue;
        const int idx = addArea(name, a.position + r.offset, a.size, a.rotation);
        m_areas[static_cast<size_t>(idx)].fromNeighbor = true;
        r.areas.push_back(name);
    }
    for (const auto& ob : e.objects) {
        const int idx = ob.solid ? findInRegion(ob.name) : -1;
        if (idx >= 0) setSolid(static_cast<uint32_t>(idx), true);
    }
    r.sounds.clear();
    for (const auto& snd : e.sounds) {
        const std::string name = prefix + snd.name;
        if (findSoundEmitter(name) >= 0) continue;
        const int idx = addSoundEmitter(name, snd.file, snd.position + r.offset, snd.volume, snd.range);
        m_soundEmitters[static_cast<size_t>(idx)].fromNeighbor = true;
        r.sounds.push_back(name);
    }
    r.hasWater = e.hasWater;
    r.waterLevel = e.waterLevel + r.offset.y;
    r.state = WorldRegion::State::Shown;
    ++m_worldVersion;
    const auto tMisc = std::chrono::steady_clock::now();
    stitchGroundEdges();
    const auto tStitch = std::chrono::steady_clock::now();
    rebuildWorldLights();
    if (r.hasWater) rebuildWorldWater();
    const auto tEnd = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    ERUPTION_LOG_WARN("World: %s in place at (%.0f, %.0f, %.0f): %u instances, %zu characters, %.0f ms "
                      "(models %.1f, scene %.1f, scatter %.1f, splat %.1f, rest %.1f, seams %.1f, lights/water %.1f)",
                      r.map.c_str(), r.offset.x, r.offset.y, r.offset.z, r.endInstance - r.firstInstance,
                      r.characters.size(), ms(t0, tEnd), ms(t0, tModels), ms(tModels, tScene), ms(tScene, tScatter),
                      ms(tScatter, tSplat), ms(tSplat, tMisc), ms(tMisc, tStitch), ms(tStitch, tEnd));
}

void Engine::showRegion(WorldRegion& r, bool show) {
    ModelRenderer& mr = m_modelRenderer;
    std::vector<uint32_t> on;
    if (show) {
        for (uint32_t i = r.firstInstance; i < r.endInstance; ++i)
            if (i - r.firstInstance < r.enabled.size() && r.enabled[i - r.firstInstance]) on.push_back(i);
        mr.setInstancesEnabled(on, true);
    } else {
        // Guarda quem estava ligado (um script pode ter apagado algum).
        r.enabled.assign(r.endInstance - r.firstInstance, 0);
        const auto& insts = mr.getInstances();
        for (uint32_t i = r.firstInstance; i < r.endInstance && i < insts.size(); ++i) {
            r.enabled[i - r.firstInstance] = insts[i].enabled ? 1 : 0;
            if (insts[i].enabled) on.push_back(i);
        }
        mr.setInstancesEnabled(on, false);
    }
    for (auto& c : m_characters)
        if (c.fromNeighbor && std::find(r.characters.begin(), r.characters.end(), c.name) != r.characters.end())
            c.parked = !show;
    for (auto& a : m_areas)
        if (a.fromNeighbor && std::find(r.areas.begin(), r.areas.end(), a.name) != r.areas.end()) a.parked = !show;
    for (auto& snd : m_soundEmitters)
        if (snd.fromNeighbor && std::find(r.sounds.begin(), r.sounds.end(), snd.name) != r.sounds.end()) snd.parked = !show;
    r.state = show ? WorldRegion::State::Shown : WorldRegion::State::Hidden;
    if (!show) r.hiddenAt = std::chrono::steady_clock::now();
    ++m_worldVersion;
    rebuildWorldLights();
    if (r.hasWater) rebuildWorldWater();
    ERUPTION_LOG_WARN("World: %s %s", r.map.c_str(), show ? "back in place" : "out of range");
}

namespace {
struct WorldGround {
    const TerrainFile* ter;
    uint32_t first, end;
};
// Qual chão tem o ponto (e a altura dele); -1 = fora de todos.
int groundAt(const std::vector<WorldGround>& grounds, float x, float z, float& h) {
    for (size_t i = 0; i < grounds.size(); ++i) {
        Vec2 lo, hi;
        if (groundArea(*grounds[i].ter, lo, hi) && inside(lo, hi, x, z)) {
            h = TerrainParser::getTerrainHeightAt(*grounds[i].ter, x, z);
            return static_cast<int>(i);
        }
    }
    return -1;
}
} // namespace

// Normais da borda do chão olhando a altura do outro lado (cada mapa calculou
// as suas sem o vizinho: aparecia uma linha de luz na emenda).
void Engine::stitchGroundEdges() {
    std::vector<WorldGround> grounds;
    if (m_currentMap && m_currentMap->terrainFromFile)
        grounds.push_back({&m_currentMap->terrain, 0, static_cast<uint32_t>(m_modelRenderer.instanceCount())});
    for (const auto& r : m_regions)
        if (r.state == WorldRegion::State::Shown && r.data) grounds.push_back({&r.data->terrain, r.firstInstance, r.endInstance});
    if (grounds.size() < 2) return;
    const auto& insts = m_modelRenderer.getInstances();
    std::vector<uint32_t> done;
    size_t patched = 0;
    for (const auto& g : grounds) {
        Vec2 lo, hi;
        if (!groundArea(*g.ter, lo, hi)) continue;
        const float s = g.ter->scale > 0.0f ? g.ter->scale : 10.0f;
        for (uint32_t i = g.first; i < g.end && i < insts.size(); ++i) {
            if (isRegionInstance(i) && g.first == 0) continue; // do mapa aberto, só as dele
            const uint32_t mesh = insts[i].meshIndex;
            if (!m_modelRenderer.isGroundMesh(mesh) || std::find(done.begin(), done.end(), mesh) != done.end()) continue;
            auto* verts = m_modelRenderer.groundVertices(mesh);
            if (!verts) continue;
            done.push_back(mesh);
            const Mat4& m = insts[i].transform;
            const glm::mat3 toLocalN = glm::transpose(glm::mat3(m));
            std::vector<uint32_t> changed;
            for (uint32_t k = 0; k < verts->size(); ++k) {
                TerrainVertex& v = (*verts)[k];
                const Vec3 wp = Vec3(m * Vec4(v.position, 1.0f));
                const bool edge = wp.x < lo.x + s * 0.5f || wp.x > hi.x - s * 0.5f || wp.z < lo.y + s * 0.5f ||
                                  wp.z > hi.y - s * 0.5f;
                if (!edge) continue;
                // Diferença pelo mundo: do outro lado da borda vale a grade do vizinho.
                bool crossed = false;
                auto slope = [&](float ax, float az, float bx, float bz) {
                    float h0 = 0.0f, h1 = 0.0f;
                    const int g0 = groundAt(grounds, ax, az, h0), g1 = groundAt(grounds, bx, bz, h1);
                    const int own = static_cast<int>(&g - grounds.data());
                    crossed |= (g0 >= 0 && g0 != own) || (g1 >= 0 && g1 != own);
                    if (g0 >= 0 && g1 >= 0) return (h1 - h0) / (2.0f * s);
                    if (g1 >= 0) return (h1 - wp.y) / s;
                    if (g0 >= 0) return (wp.y - h0) / s;
                    return 0.0f;
                };
                const float dx = slope(wp.x - s, wp.z, wp.x + s, wp.z);
                const float dz = slope(wp.x, wp.z - s, wp.x, wp.z + s);
                if (!crossed) continue; // borda de fora do mundo: fica como veio
                const Vec3 n = glm::normalize(Vec3(-dx, 1.0f, -dz));
                v.normal = glm::normalize(toLocalN * n);
                changed.push_back(k);
            }
            if (!changed.empty()) {
                m_modelRenderer.patchGroundVertices(mesh, changed);
                patched += changed.size();
            }
        }
    }
    if (patched) {
        ERUPTION_LOG_INFO("World: %zu border vertices stitched", patched);
    }
}

void Engine::releaseRegion(WorldRegion& r) {
    ModelRenderer& mr = m_modelRenderer;
    // Instâncias do vizinho (mapa, acrescentados, espalhados): mortas e sem nome.
    std::vector<uint32_t> idx;
    for (uint32_t i = r.firstInstance; i < r.endInstance && i < mr.instanceCount(); ++i) {
        idx.push_back(i);
        mr.setInstanceName(i, std::string());
        setSolid(i, false);
    }
    for (auto& a : m_areas)
        if (a.fromNeighbor && std::find(r.areas.begin(), r.areas.end(), a.name) != r.areas.end()) a.removed = true;
    r.areas.clear();
    for (auto& snd : m_soundEmitters)
        if (snd.fromNeighbor && std::find(r.sounds.begin(), r.sounds.end(), snd.name) != r.sounds.end()) snd.removed = true;
    r.sounds.clear();
    mr.setInstancesEnabled(idx, false);
    // Malhas do .glb do mapa (só dele) e as imagens embutidas guardadas para ele.
    size_t meshes = 0;
    for (const auto& path : r.modelFiles) {
        for (const auto& key : mr.releaseAsset(path)) {
            for (ModelMeshGPU base : m_assetCache.takeModelMeshes(key)) {
                ++meshes;
                m_vulkan.deferFrameCleanup([this, base]() mutable { base.destroyBuffers(&m_vulkan); });
            }
        }
        if (m_currentMap) {
            auto& models = m_currentMap->models;
            models.erase(std::remove_if(models.begin(), models.end(),
                                        [&](const ModelFile& m) { return m.filePath == path && m.nodes.empty(); }),
                         models.end());
        }
    }
    for (auto& c : m_characters)
        if (c.fromNeighbor && std::find(r.characters.begin(), r.characters.end(), c.name) != r.characters.end()) {
            cleanupSpriteTextures(c.look);
            c.removed = true;
        }
    // Imagens embutidas no .glb do vizinho ("#<mapa>|..."): só ele as usa.
    const std::string embeddedPrefix = "#" + r.map + "|";
    std::vector<ModelTextureGPU> embedded = m_assetCache.takeModelTextures(embeddedPrefix);
    mr.forgetTextures(embeddedPrefix);
    for (const auto& t : embedded) {
        if (t.bindlessSlot) m_bindless.freeSlot(t.bindlessSlot);
        if (t.pbrSlot) m_bindless.freeSlot(t.pbrSlot);
        if (t.normalSlot) m_bindless.freeSlot(t.normalSlot);
    }
    if (!embedded.empty()) {
        VkDevice dev = m_vulkan.device();
        VmaAllocator allocator = m_vulkan.allocator();
        m_vulkan.deferSlotImageCleanup([dev, allocator, embedded] {
            for (const auto& t : embedded) {
                if (t.view) vkDestroyImageView(dev, t.view, nullptr);
                if (t.image) vmaDestroyImage(allocator, t.image, t.alloc);
                if (t.pbrView) vkDestroyImageView(dev, t.pbrView, nullptr);
                if (t.pbrImage) vmaDestroyImage(allocator, t.pbrImage, t.pbrAlloc);
                if (t.normalView) vkDestroyImageView(dev, t.normalView, nullptr);
                if (t.normalImage) vmaDestroyImage(allocator, t.normalImage, t.normalAlloc);
            }
        });
    }
    m_bindless.flushUpdates(); // slots dos sprites e das imagens de volta à textura padrão antes de destruir
    releaseRegionSplat(r);
    for (auto& t : r.splatMask) t = TextureResource{};
    ERUPTION_LOG_WARN("World: %s released (%zu meshes, %zu embedded images, %zu instances)", r.map.c_str(), meshes,
                      embedded.size(), idx.size());
    r.data.reset();
    r.enabled.clear();
    r.lights.clear();
    r.characters.clear();
    r.modelFiles.clear();
    r.firstInstance = r.endInstance = 0;
    r.state = WorldRegion::State::Idle;
    ++m_worldVersion;
}

void Engine::rebuildWorldLights() {
    std::vector<PointLight> extra;
    for (const auto& r : m_regions)
        if (r.state == WorldRegion::State::Shown) extra.insert(extra.end(), r.lights.begin(), r.lights.end());
    m_deferredLighting.setExtraPointLights(std::move(extra));
}

void Engine::rebuildWorldWater() {
    if (!m_currentMap) return;
    // Uma malha só: a água do mapa aberto (se tem) e a de cada vizinho em cena.
    WaterMesh all;
    auto append = [&](const TerrainFile& t, float level, float dx, float dz) {
        WaterMesh m = TerrainParser::generateWaterMesh(t, 0, 0, t.width, t.height, level, 0.0f, true, false);
        const uint32_t base = static_cast<uint32_t>(all.vertices.size());
        for (auto& v : m.vertices) {
            v.position.x += dx;
            v.position.z += dz;
            all.vertices.push_back(v);
        }
        for (uint32_t i : m.indices) all.indices.push_back(base + i);
        all.waterTileCount += m.waterTileCount;
    };
    bool any = false;
    if (m_waterOn) {
        append(m_currentMap->terrain, m_mapBaseWaterLevel, 0.0f, 0.0f);
        any = true;
    }
    for (const auto& r : m_regions) {
        if (r.state != WorldRegion::State::Shown || !r.hasWater || !r.data) continue;
        // A malha sai em coordenada da grade; a grade do vizinho já está deslocada.
        append(r.data->terrain, r.waterLevel, r.data->terrain.offsetX, r.data->terrain.offsetZ);
        if (!any) m_mapBaseWaterLevel = r.waterLevel; // nível usado no sombreamento
        any = true;
    }
    m_vulkan.waitIdle();
    if (all.vertices.empty()) {
        if (!m_waterOn) m_water.clearWaterMesh();
        return;
    }
    m_water.setWaterMesh(all);
    cacheWaterBounds(all);
}

const char* Engine::worldStateName(WorldRegion::State s) {
    switch (s) {
    case Engine::WorldRegion::State::Idle: return "far";
    case Engine::WorldRegion::State::Loading: return "loading";
    case Engine::WorldRegion::State::Shown: return "shown";
    case Engine::WorldRegion::State::Hidden: return "hidden";
    case Engine::WorldRegion::State::Failed: return "failed";
    }
    return "";
}

std::vector<Engine::WorldRegionInfo> Engine::worldRegions() const {
    std::vector<WorldRegionInfo> out;
    for (const auto& r : m_regions) out.push_back({r.map, r.offset, r.boundsMin, r.boundsMax, worldStateName(r.state)});
    return out;
}

bool Engine::isRegionInstance(uint32_t index) const {
    for (const auto& r : m_regions)
        if (r.endInstance > r.firstInstance && index >= r.firstInstance && index < r.endInstance) return true;
    return false;
}

const LoadedMap* Engine::mapAt(float x, float z) const {
    if (!m_currentMap) return nullptr;
    Vec2 lo, hi;
    if (!m_currentMap->terrainFromFile || !groundArea(m_currentMap->terrain, lo, hi) || inside(lo, hi, x, z))
        return m_currentMap.get();
    for (const auto& r : m_regions)
        if (r.state == WorldRegion::State::Shown && r.data && inside(r.boundsMin, r.boundsMax, x, z)) return r.data.get();
    return m_currentMap.get();
}

bool Engine::worldBounds(Vec2& outMin, Vec2& outMax) const {
    if (!m_currentMap) return false;
    // Mapa aberto: o limite de sempre da câmera, [0, lado x 10].
    outMin = Vec2(0.0f);
    outMax = Vec2(float(m_currentMap->terrain.width) * 10.0f, float(m_currentMap->terrain.height) * 10.0f);
    for (const auto& r : m_regions)
        if (r.state == WorldRegion::State::Shown) {
            outMin = glm::min(outMin, r.boundsMin);
            outMax = glm::max(outMax, r.boundsMax);
        }
    return true;
}

} // namespace eruption
