// Cena como dado: guarda o estado original do mapa ao carregar, aplica o
// arquivo de edições (SceneEdits) e grava só o que mudou (Ctrl+S).
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"

#include <imgui.h>
#include "renderer/WeatherTypes.hpp"
#include "script/ScriptApi.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <fstream>

namespace eruption {

namespace {

bool sameMatrix(const Mat4& a, const Mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-4f * std::max(1.0f, std::abs(b[c][r]))) return false;
    return true;
}

bool sameLight(const PointLight& a, const PointLight& b) {
    return a.position == b.position && a.color == b.color && a.intensity == b.intensity && a.radius == b.radius &&
           a.enabled == b.enabled;
}

} // namespace

void Editor::onMapReady() {
    const auto& insts = m_engine->modelRenderer().getInstances();
    m_original = MapOriginal{};
    m_original.count = insts.size();
    m_original.transforms.reserve(insts.size());
    m_original.enabled.reserve(insts.size());
    for (const auto& i : insts) {
        m_original.transforms.push_back(i.transform);
        m_original.enabled.push_back(i.enabled);
    }
    m_original.lights = m_engine->deferredLighting().getPointLights();
    m_engine->loadScatter(m_engine->currentMapName());
    applySceneFile();
    applyLayerVisibility();
    ++m_mapReadyCount;
}

void Editor::applySceneFile() {
    m_copySources.clear();
    m_added.clear();
    m_envEdited = false;
    m_waterEdited = false;

    SceneEdits edits;
    std::string error;
    const auto file = SceneEdits::pathFor(m_engine->currentMapName());
    if (!edits.load(file, error)) {
        log(LogLevel::Error, error);
    } else if (!edits.empty()) {
        for (const auto& c : edits.copies) m_copySources[c.name] = c.from;
        m_envEdited = edits.hasEnvironment;
        m_waterEdited = edits.hasWater;
        const int n = edits.apply(*m_engine, &m_added);
        log(LogLevel::Info, "Scene edits loaded: " + std::to_string(n) + " change(s) from " + file.generic_string());
    }
    // Mapas vizinhos (mundo contínuo) do arquivo: antes da foto do "salvo".
    m_engine->startWorld();
    m_savedScene = currentEdits().serialize(m_engine->currentMapName());
    m_sceneFileTime = sceneFileTime();
    m_terrainDirty = false;
    m_undo.clear();
    updateTitle(false);
}

std::filesystem::file_time_type Editor::sceneFileTime() const {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(SceneEdits::pathFor(m_engine->currentMapName()), ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

void Editor::reloadSceneFile() {
    // Volta o mapa ao estado de quando carregou e aplica o arquivo de novo.
    ModelRenderer& mr = m_engine->modelRenderer();
    const auto& insts = mr.getInstances();
    const size_t base = std::min(m_original.count, insts.size());
    for (size_t i = 0; i < base; ++i) {
        const uint32_t idx = static_cast<uint32_t>(i);
        if (!sameMatrix(insts[i].transform, m_original.transforms[i])) mr.setInstanceTransform(idx, m_original.transforms[i]);
        if (insts[i].enabled != m_original.enabled[i]) mr.setInstanceEnabled(idx, m_original.enabled[i]);
    }
    // Cópias e modelos acrescentados: apagados e sem nome (o arquivo os cria de novo).
    for (size_t i = base; i < insts.size(); ++i) {
        const uint32_t idx = static_cast<uint32_t>(i);
        if (m_engine->isRegionInstance(idx)) continue; // mapa vizinho: o mundo cuida
        if (insts[i].enabled) mr.setInstanceEnabled(idx, false);
        mr.setInstanceName(idx, std::string());
    }
    m_engine->m_deferredLighting.getPointLights() = m_original.lights;
    m_engine->clearCharacters(true); // vêm todos do arquivo (os dos vizinhos ficam)
    m_engine->clearAreas(true);
    m_engine->clearSoundEmitters(true);
    for (uint32_t i = 0; i < insts.size(); ++i)
        if (!m_engine->isRegionInstance(i)) m_engine->setSolid(i, false);
    m_engine->clearPlayerStart();
    select({});
    applySceneFile();
    log(LogLevel::Info, "Scene file reloaded.");
}

void Editor::watchSceneFile() {
    // Uma vez por segundo: o arquivo pode ter sido mudado no VS Code ou por uma IA.
    if (m_frame % 60 != 0 || m_engine->currentMapName().empty() || m_play != PlayState::Editing) return;
    const auto t = sceneFileTime();
    if (t == m_sceneFileTime) return;
    m_sceneFileTime = t;
    if (m_sceneDirty) {
        log(LogLevel::Warning, "The scene file changed on disk, but this map has unsaved changes. "
                               "File > Reload Scene discards them and loads the file.");
        return;
    }
    reloadSceneFile();
}

SceneEdits Editor::currentEdits() const {
    SceneEdits e;
    const auto& insts = m_engine->modelRenderer().getInstances();
    const size_t base = std::min(m_original.count, insts.size());
    for (size_t i = 0; i < base; ++i) {
        const bool moved = !sameMatrix(insts[i].transform, m_original.transforms[i]);
        const bool hidden = m_original.enabled[i] && !insts[i].enabled;
        if (!moved && !hidden) continue;
        SceneEdits::Object o;
        o.name = insts[i].name;
        o.hidden = hidden;
        o.hasTransform = moved;
        if (moved) o.transform = SceneEdits::Transform::from(insts[i].transform);
        e.objects.push_back(std::move(o));
    }
    for (const auto& a : m_added) {
        // Raiz a partir de um nó visível; todos apagados = modelo removido.
        int k0 = -1;
        for (uint32_t k = 0; k < a.count && k0 < 0; ++k)
            if (a.first + k < insts.size() && insts[a.first + k].enabled) k0 = static_cast<int>(k);
        if (k0 < 0) continue;
        const Mat4 root = insts[a.first + k0].transform * glm::inverse(a.local[k0]);
        SceneEdits::Added out;
        out.file = a.prefab.empty() ? a.file : std::string();
        out.node = a.prefab.empty() ? a.node : std::string();
        out.prefab = a.prefab;
        out.name = a.name;
        out.transform = SceneEdits::Transform::from(root);
        e.added.push_back(std::move(out));
        for (uint32_t k = 0; k < a.count; ++k) {
            const ModelInstance& inst = insts[a.first + k];
            const bool hidden = !inst.enabled;
            const bool moved = !hidden && !sameMatrix(inst.transform, root * a.local[k]);
            if (!hidden && !moved) continue;
            SceneEdits::Object o;
            o.name = inst.name;
            o.hidden = hidden;
            o.hasTransform = moved;
            if (moved) o.transform = SceneEdits::Transform::from(inst.transform);
            e.objects.push_back(std::move(o));
        }
    }
    for (size_t i = base; i < insts.size(); ++i) {
        if (!insts[i].enabled) continue; // cópia apagada
        auto it = m_copySources.find(insts[i].name);
        if (it == m_copySources.end()) continue;
        e.copies.push_back({it->second, insts[i].name, SceneEdits::Transform::from(insts[i].transform)});
    }
    for (const auto& c : m_engine->characters()) {
        if (c.removed || c.fromNeighbor) continue;
        e.characters.push_back({c.name, c.sprite, c.position, c.facing, !c.visible});
    }
    // Sólidos: marca no objeto (o do mapa, o acrescentado ou a cópia), pelo nome.
    for (uint32_t i = 0; i < insts.size(); ++i) {
        if (!m_engine->isSolid(i) || !insts[i].enabled || insts[i].name.empty() || m_engine->isRegionInstance(i) ||
            m_engine->isScatterInstance(i))
            continue;
        auto it = std::find_if(e.objects.begin(), e.objects.end(), [&](const SceneEdits::Object& o) { return o.name == insts[i].name; });
        if (it != e.objects.end()) {
            it->solid = true;
        } else {
            SceneEdits::Object o;
            o.name = insts[i].name;
            o.solid = true;
            e.objects.push_back(std::move(o));
        }
    }
    for (const auto& a : m_engine->areas())
        if (!a.removed && !a.fromNeighbor) e.areas.push_back({a.name, a.center, a.size, a.yaw});
    for (const auto& snd : m_engine->soundEmitters())
        if (!snd.removed && !snd.fromNeighbor) e.sounds.push_back({snd.name, snd.file, snd.position, snd.volume, snd.range});
    e.terrainLayers = m_engine->splatLayerFiles();
    e.neighbors = m_engine->worldNeighbors();
    if (m_engine->hasPlayerStart()) {
        e.hasSpawn = true;
        e.spawn = m_engine->playerStart();
    }
    const auto& ls = m_engine->m_deferredLighting.getPointLights();
    for (size_t i = 0; i < ls.size(); ++i) {
        const bool fromMap = i < m_original.lights.size();
        if (fromMap ? sameLight(ls[i], m_original.lights[i]) : !ls[i].enabled) continue; // luz nova apagada = removida
        SceneEdits::Light l;
        l.index = static_cast<int>(i) + 1;
        l.position = ls[i].position;
        l.color = ls[i].color;
        l.intensity = ls[i].intensity;
        l.range = ls[i].radius;
        l.enabled = ls[i].enabled;
        e.lights.push_back(l);
    }
    if (m_envEdited) {
        e.hasEnvironment = true;
        e.time = m_engine->dayNightCycle().timeOfDay() * 24.0f;
        e.paused = m_engine->dayNightCycle().isPaused();
        e.weather = weatherTypeName(m_engine->weatherSystem().currentType());
        if (m_waterEdited && m_engine->waterEnabled()) {
            e.hasWater = true;
            e.waterLevel = m_engine->waterLevel();
        }
    }
    return e;
}

bool Editor::saveScene() {
    const std::string& map = m_engine->currentMapName();
    if (map.empty()) return false;
    if (m_play != PlayState::Editing) {
        log(LogLevel::Warning, "Stop the game before saving: changes made while playing are thrown away.");
        return false;
    }
    const std::string text = currentEdits().serialize(map);
    const auto file = SceneEdits::pathFor(map);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << text;
    std::string terrainError;
    if (!m_engine->saveHeightDelta(terrainError)) log(LogLevel::Error, terrainError);
    if (!m_engine->saveSplat(terrainError)) log(LogLevel::Error, terrainError);
    if (!m_engine->saveScatter(terrainError)) log(LogLevel::Error, terrainError);
    if (!m_engine->saveWalk(terrainError)) log(LogLevel::Error, terrainError);
    m_terrainDirty = false;
    m_savedScene = text;
    m_sceneFileTime = sceneFileTime();
    updateTitle(false);
    log(LogLevel::Info, "Saved " + file.generic_string());
    return true;
}

void Editor::duplicateSelection() {
    const auto list = selectionList();
    if (list.size() > 1) {
        // Duplica os modelos do grupo; a seleção passa a ser as cópias.
        std::vector<Selection> copies;
        m_undo.beginGroup("Duplicate " + std::to_string(list.size()) + " objects");
        for (const auto& s : list) {
            if (s.kind != SelectionKind::Model) continue;
            m_selection = s;
            m_extraSelection.clear();
            duplicateOne();
            copies.push_back(m_selection);
        }
        m_undo.endGroup();
        select({});
        for (const auto& c : copies) toggleSelect(c);
        return;
    }
    duplicateOne();
}

void Editor::duplicateOne() {
    if (m_selection.kind != SelectionKind::Model || !selectionHasTransform()) return;
    ModelRenderer& mr = m_engine->modelRenderer();
    const uint32_t src = static_cast<uint32_t>(m_selection.index);
    const ModelInstance& s = mr.getInstances()[src];
    // Nome único: <origem>_copia<n>.
    const std::string root = m_copySources.count(s.name) ? m_copySources.at(s.name) : s.name;
    std::string name;
    for (int n = 1;; ++n) {
        name = root + "_copy" + std::to_string(n);
        if (scriptFindModel(name) < 0) break;
    }
    // Um pouco ao lado, para a cópia não nascer escondida dentro da original.
    Mat4 m = s.transform;
    const float shift = std::max(1.0f, (s.worldAabbMax.x - s.worldAabbMin.x) * 0.6f);
    m[3].x += shift;
    const uint32_t idx = mr.duplicateInstance(src, name, m);
    m_copySources[name] = root;
    select({SelectionKind::Model, static_cast<int>(idx)});
    Engine* engine = m_engine;
    auto setEnabled = [engine, idx](bool on) { engine->modelRenderer().setInstanceEnabled(idx, on); };
    m_undo.push({"Duplicate " + name, [setEnabled] { setEnabled(false); }, [setEnabled] { setEnabled(true); }});
}

namespace {

// Caminho relativo à pasta do projeto quando o arquivo está dentro dela, para
// o scene.luau continuar valendo em outra máquina.
std::string projectPath(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path rel = fs::relative(path, fs::current_path(), ec);
    if (!ec && !rel.empty() && rel.native().rfind("..", 0) != 0) return rel.generic_string();
    return path;
}

} // namespace

void Editor::dropModel(const std::string& path, const Vec3& position) {
    namespace fs = std::filesystem;
    std::string file = projectPath(path), node, prefab;
    Mat4 inner(1.0f);
    std::string stem = fs::path(path).stem().string();
    if (Prefab::isPrefabPath(path)) {
        Prefab p;
        std::string error;
        if (!p.load(path, error)) {
            log(LogLevel::Error, error);
            return;
        }
        prefab = file;
        file = p.file;
        node = p.node;
        inner = p.matrix();
        stem = fs::path(stem).stem().string(); // "x.prefab" -> "x"
    }
    // Nome livre: nem "<nome>" nem "<nome>/<nó>" em uso.
    const auto& insts = m_engine->modelRenderer().getInstances();
    auto taken = [&insts](const std::string& n) {
        for (const auto& i : insts)
            if (i.name == n || i.name.rfind(n + "/", 0) == 0) return true;
        return false;
    };
    std::string name;
    for (int n = 1;; ++n) {
        name = stem + "_" + std::to_string(n);
        if (!taken(name)) break;
    }
    SceneEdits::AddedInstance added;
    if (!SceneEdits::addModel(*m_engine, file, node, name, glm::translate(Mat4(1.0f), position), inner, added)) {
        log(LogLevel::Error, "Could not add " + file + " (see the log above).");
        return;
    }
    added.node = node;
    added.prefab = prefab;
    m_added.push_back(added);
    select({SelectionKind::Model, static_cast<int>(added.first)});
    Engine* engine = m_engine;
    const uint32_t first = added.first, count = added.count;
    auto setEnabled = [engine, first, count](bool on) {
        for (uint32_t k = 0; k < count; ++k) engine->modelRenderer().setInstanceEnabled(first + k, on);
    };
    m_undo.push({"Add " + name, [setEnabled] { setEnabled(false); }, [setEnabled] { setEnabled(true); }});
    log(LogLevel::Info, "Added " + name + " (" + (prefab.empty() ? file : prefab) + ")");
}

void Editor::addPointLight() {
    Vec3 at;
    if (!groundPointAt(0.5f, 0.5f, at)) at = m_engine->camera().target();
    auto& lights = m_engine->m_deferredLighting.getPointLights();
    PointLight l;
    l.position = at + Vec3(0.0f, 15.0f, 0.0f);
    l.color = Vec3(1.0f, 0.85f, 0.6f);
    l.intensity = 3.0f;
    l.radius = 60.0f;
    lights.push_back(l);
    const int idx = static_cast<int>(lights.size() - 1);
    select({SelectionKind::Light, idx});
    Engine* engine = m_engine;
    auto setEnabled = [engine, idx](bool on) {
        auto& ls = engine->m_deferredLighting.getPointLights();
        if (idx < static_cast<int>(ls.size())) ls[static_cast<size_t>(idx)].enabled = on;
    };
    m_undo.push({"Add light", [setEnabled] { setEnabled(false); }, [setEnabled] { setEnabled(true); }});
}

void Editor::addTriggerArea() {
    Vec3 at;
    if (!groundPointAt(0.5f, 0.5f, at)) at = m_engine->camera().target();
    std::string name;
    for (int i = 1; name.empty() || m_engine->findArea(name) >= 0; ++i) name = "area_" + std::to_string(i);
    const Vec3 size(40.0f, 30.0f, 40.0f);
    const int idx = m_engine->addArea(name, at + Vec3(0.0f, size.y * 0.5f, 0.0f), size, 0.0f);
    select({SelectionKind::Area, idx});
    Engine* engine = m_engine;
    auto setRemoved = [engine, idx](bool removed) {
        auto& as = engine->areas();
        if (idx < static_cast<int>(as.size())) as[static_cast<size_t>(idx)].removed = removed;
    };
    m_undo.push({"Add area", [setRemoved] { setRemoved(true); }, [setRemoved] { setRemoved(false); }});
}

void Editor::addSoundEmitter() {
    Vec3 at;
    if (!groundPointAt(0.5f, 0.5f, at)) at = m_engine->camera().target();
    std::string name;
    for (int i = 1; name.empty() || m_engine->findSoundEmitter(name) >= 0; ++i) name = "sound_" + std::to_string(i);
    // Um som selecionado no Projeto já vira o arquivo do emissor.
    const std::filesystem::path sel(m_projectSelected);
    std::string file;
    if (sel.extension() == ".wav" || sel.extension() == ".ogg" || sel.extension() == ".mp3" || sel.extension() == ".flac") {
        std::error_code ec;
        const auto rel = std::filesystem::relative(sel, std::filesystem::current_path(), ec);
        file = (!ec && !rel.empty() && rel.native().rfind("..", 0) != 0) ? rel.generic_string() : sel.string();
    }
    const int idx = m_engine->addSoundEmitter(name, file, at + Vec3(0.0f, 5.0f, 0.0f), 1.0f, 200.0f);
    select({SelectionKind::Sound, idx});
    Engine* engine = m_engine;
    auto setRemoved = [engine, idx](bool removed) {
        auto& ss = engine->soundEmitters();
        if (idx < static_cast<int>(ss.size())) ss[static_cast<size_t>(idx)].removed = removed;
    };
    m_undo.push({"Add sound", [setRemoved] { setRemoved(true); }, [setRemoved] { setRemoved(false); }});
}

void Editor::setPlayerStartHere() {
    Vec3 at;
    if (!groundPointAt(0.5f, 0.5f, at)) at = m_engine->camera().target();
    const bool had = m_engine->hasPlayerStart();
    const Vec3 before = m_engine->playerStart();
    Engine* engine = m_engine;
    auto apply = [engine](bool has, const Vec3& p) {
        if (has) engine->movePlayerStart(p); else engine->clearPlayerStart();
    };
    apply(true, at);
    select({SelectionKind::Spawn, 0});
    m_undo.push({"Player start", [apply, had, before] { apply(had, before); }, [apply, at] { apply(true, at); }});
}

void Editor::dropCharacter(const std::string& png, const Vec3& position) {
    namespace fs = std::filesystem;
    const std::string stem = fs::path(png).stem().string();
    std::string name;
    for (int n = 1;; ++n) {
        name = stem + "_" + std::to_string(n);
        if (m_engine->findCharacter(name) < 0) break;
    }
    // De frente para a câmera.
    const Vec3 toCam = m_engine->camera().position() - position;
    const float facing = glm::degrees(std::atan2(toCam.x, toCam.z));
    const int idx = m_engine->addCharacter(name, projectPath(png), position, facing);
    select({SelectionKind::Character, idx});
    Engine* engine = m_engine;
    auto setRemoved = [engine, idx](bool removed) {
        auto& cs = engine->characters();
        if (idx < static_cast<int>(cs.size())) cs[static_cast<size_t>(idx)].removed = removed;
    };
    m_undo.push({"Add " + name, [setRemoved] { setRemoved(true); }, [setRemoved] { setRemoved(false); }});
    log(LogLevel::Info, "Added character " + name + ". Rules and scripts find it by this name.");
}

bool Editor::savePrefab(const std::string& path) {
    if (m_selection.kind != SelectionKind::Model || !selectionHasTransform()) return false;
    ModelRenderer& mr = m_engine->modelRenderer();
    const uint32_t idx = static_cast<uint32_t>(m_selection.index);
    const ModelInstance& inst = mr.getInstances()[idx];
    Prefab p;
    // Matriz do nó como o motor o cria, a menos de translação: a rotação e a
    // escala do objeto relativas a ela viram as do prefab (a posição é de cada cópia).
    Mat4 created(1.0f);
    const SceneEdits::AddedInstance* from = nullptr;
    for (const auto& a : m_added)
        if (idx >= a.first && idx < a.first + a.count) from = &a;
    if (from) {
        p.file = from->file;
        p.node = from->count == 1 ? from->node : inst.name.substr(inst.name.rfind('/') + 1);
        created = glm::inverse(from->inner) * from->local[idx - from->first];
    } else if (idx < m_original.count) {
        p.file = inst.assetPath;
        p.node = inst.name;
        created = m_original.transforms[idx];
    } else {
        log(LogLevel::Warning, "Copies can't become prefabs yet: make the prefab from the original object.");
        return false;
    }
    const SceneEdits::Transform delta = SceneEdits::Transform::from(inst.transform * glm::inverse(created));
    p.rotation = delta.rotation;
    p.scale = delta.scale;
    p.file = projectPath(p.file);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream(path, std::ios::binary) << p.serialize();
    log(LogLevel::Info, "Saved prefab " + path + ". Drag it from the Project panel into a scene.");
    return true;
}

void Editor::drawPrefabDialog() {
    if (m_prefabOpen) {
        ImGui::OpenPopup("Save as Prefab");
        m_prefabOpen = false;
        std::string base = selectionName(m_selection);
        base = base.substr(base.rfind('/') + 1);
        for (char& c : base)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
        std::snprintf(m_prefabName, sizeof(m_prefabName), "%s", base.c_str());
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Save as Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText(tr("Name"), m_prefabName, sizeof(m_prefabName), ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string path = std::string("assets/prefabs/") + m_prefabName + ".prefab.luau";
    ImGui::TextDisabled("%s", path.c_str());
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", tr("Replaces the existing prefab."));
    ImGui::Separator();
    ImGui::BeginDisabled(m_prefabName[0] == '\0');
    const bool save = ImGui::Button(tr("Save"), ImVec2(120, 0)) || (enter && m_prefabName[0]);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (save && savePrefab(path)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::updateTitle(bool dirty) {
    const std::string& map = m_engine->currentMapName();
    std::string title = "Eruption Editor";
    if (!map.empty()) title += " - " + map + (dirty ? " *" : "");
    glfwSetWindowTitle(m_engine->m_window.handle(), title.c_str());
    m_sceneDirty = dirty;
}

void Editor::checkSceneDirty() {
    // A comparação percorre todas as instâncias: duas vezes por segundo basta.
    if (m_frame % 30 != 0 || m_engine->currentMapName().empty() || m_play != PlayState::Editing) return;
    const bool dirty = m_terrainDirty || currentEdits().serialize(m_engine->currentMapName()) != m_savedScene;
    if (dirty != m_sceneDirty) updateTitle(dirty);
}

} // namespace eruption
