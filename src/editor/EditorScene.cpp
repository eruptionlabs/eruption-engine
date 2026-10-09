// Cena como dado: guarda o estado original do mapa ao carregar, aplica o
// arquivo de edições (SceneEdits) e grava só o que mudou (Ctrl+S).
#include "editor/Editor.hpp"

#include "core/Engine.hpp"
#include "renderer/WeatherTypes.hpp"
#include "script/ScriptApi.hpp"

#include <GLFW/glfw3.h>

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
    m_copySources.clear();
    m_envEdited = false;

    SceneEdits edits;
    std::string error;
    const auto file = SceneEdits::pathFor(m_engine->currentMapName());
    if (!edits.load(file, error)) {
        log(LogLevel::Error, error);
    } else if (!edits.empty()) {
        for (const auto& c : edits.copies) m_copySources[c.name] = c.from;
        m_envEdited = edits.hasEnvironment;
        const int n = edits.apply(*m_engine);
        log(LogLevel::Info, "Scene edits loaded: " + std::to_string(n) + " change(s) from " + file.generic_string());
    }
    m_savedScene = currentEdits().serialize(m_engine->currentMapName());
    m_undo.clear();
    updateTitle(false);
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
    for (size_t i = base; i < insts.size(); ++i) {
        if (!insts[i].enabled) continue; // cópia apagada
        auto it = m_copySources.find(insts[i].name);
        if (it == m_copySources.end()) continue;
        e.copies.push_back({it->second, insts[i].name, SceneEdits::Transform::from(insts[i].transform)});
    }
    const auto& ls = m_engine->m_deferredLighting.getPointLights();
    for (size_t i = 0; i < ls.size() && i < m_original.lights.size(); ++i) {
        if (sameLight(ls[i], m_original.lights[i])) continue;
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
    m_savedScene = text;
    updateTitle(false);
    log(LogLevel::Info, "Saved " + file.generic_string());
    return true;
}

void Editor::duplicateSelection() {
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
    const bool dirty = currentEdits().serialize(m_engine->currentMapName()) != m_savedScene;
    if (dirty != m_sceneDirty) updateTitle(dirty);
}

} // namespace eruption
