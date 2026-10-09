// Pincel de objetos: espalha modelos da paleta no chão sob o pincel, com
// densidade (e distância mínima), escala e giro aleatórios; Ctrl apaga o que
// o pincel colocou. Um desfazer por traço.
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"

#include <imgui.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace eruption {

namespace {

// Gerador pequeno e repetível (mesmo traço, mesmo resultado).
struct Rng {
    uint32_t s;
    float next() {
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) / float(1u << 24);
    }
};

} // namespace

void Editor::scatterStep(float dt, bool erase) {
    TerrainBrush& b = m_brush;
    const ScatterSettings& st = m_scatterSettings;
    auto& groups = m_engine->scatterGroups();
    const Vec2 c(b.cursor.x, b.cursor.z);
    const float r2 = b.size * b.size;
    auto inBrush = [&](const Vec3& p) { return glm::dot(Vec2(p.x, p.z) - c, Vec2(p.x, p.z) - c) < r2; };
    const auto& insts = m_engine->modelRenderer().getInstances();
    auto itemPos = [&](const Engine::ScatterGroup&, const Engine::ScatterGroup::Item& it) {
        return Vec3(insts[it.first].transform[3]);
    };
    // Grupos da paleta ligados.
    std::vector<int> chosen;
    for (const auto& e : m_scatterPalette)
        if (e.on) chosen.push_back(m_engine->scatterGroup(e.file, e.node, e.prefab));
    chosen.erase(std::remove(chosen.begin(), chosen.end(), -1), chosen.end());
    if (chosen.empty()) return;

    if (erase) {
        // Apaga aos poucos (parte do que está no pincel a cada passo).
        Rng rng{m_scatterStroke.seed ^ uint32_t(m_frame * 2654435761u)};
        for (int gi : chosen) {
            auto& g = groups[size_t(gi)];
            std::vector<uint32_t> gone;
            for (uint32_t i = 0; i < g.items.size(); ++i)
                if (!g.items[i].removed && inBrush(itemPos(g, g.items[i])) && rng.next() < std::min(1.0f, dt * 6.0f))
                    gone.push_back(i);
            if (gone.empty()) continue;
            m_engine->scatterSetRemoved(gi, gone, true);
            m_scatterStroke.removed.push_back({gi, gone});
        }
        return;
    }
    // Quantos cabem no pincel pela densidade, e quantos já estão lá.
    const float area = 3.14159f * r2;
    const int target = int(st.density * area / 10000.0f);
    const float minDist = 100.0f / std::sqrt(std::max(0.01f, st.density)) * 0.6f;
    std::vector<Vec2> near;
    for (auto& g : groups)
        for (const auto& it : g.items)
            if (!it.removed && inBrush(itemPos(g, it))) near.push_back(Vec2(itemPos(g, it).x, itemPos(g, it).z));
    if (int(near.size()) >= target) return;
    Rng rng{m_scatterStroke.seed ^ uint32_t(m_frame * 2654435761u)};
    std::vector<std::vector<Mat4>> roots(chosen.size());
    int placed = 0;
    for (int attempt = 0; attempt < 24 && placed < 6 && int(near.size()) < target; ++attempt) {
        const float a = rng.next() * 6.2831853f, d = std::sqrt(rng.next()) * b.size;
        const Vec2 p = c + Vec2(std::cos(a), std::sin(a)) * d;
        bool clear = true;
        for (const Vec2& q : near)
            if (glm::dot(q - p, q - p) < minDist * minDist) { clear = false; break; }
        if (!clear) continue;
        float y = 0.0f;
        if (!floorBelow(Vec3(p.x, b.cursor.y + 500.0f, p.y), -1, y, true)) continue;
        Mat4 m = glm::translate(Mat4(1.0f), Vec3(p.x, y, p.y));
        if (st.alignToGround) {
            // Normal do chão por diferença de alturas ao redor.
            float yx = y, yz = y;
            floorBelow(Vec3(p.x + 1.0f, b.cursor.y + 500.0f, p.y), -1, yx, true);
            floorBelow(Vec3(p.x, b.cursor.y + 500.0f, p.y + 1.0f), -1, yz, true);
            const Vec3 n = glm::normalize(Vec3(y - yx, 1.0f, y - yz));
            const Vec3 axis = glm::cross(Vec3(0, 1, 0), n);
            const float ang = std::acos(std::clamp(n.y, -1.0f, 1.0f));
            if (glm::length(axis) > 1e-4f) m = glm::rotate(m, ang, glm::normalize(axis));
        }
        if (st.randomYaw) m = glm::rotate(m, rng.next() * 6.2831853f, Vec3(0, 1, 0));
        const float s = st.scaleMin + (st.scaleMax - st.scaleMin) * rng.next();
        m = glm::scale(m, Vec3(s));
        roots[size_t(rng.next() * chosen.size()) % chosen.size()].push_back(m);
        near.push_back(p);
        ++placed;
    }
    for (size_t k = 0; k < chosen.size(); ++k) {
        if (roots[k].empty()) continue;
        auto items = m_engine->scatterAdd(chosen[k], roots[k]);
        m_scatterStroke.added.push_back({chosen[k], items});
    }
}

void Editor::endScatterStroke() {
    auto added = m_scatterStroke.added;
    auto removed = m_scatterStroke.removed;
    m_scatterStroke.added.clear();
    m_scatterStroke.removed.clear();
    if (added.empty() && removed.empty()) return;
    Engine* engine = m_engine;
    bool* dirty = &m_terrainDirty;
    auto apply = [engine, dirty, added, removed](bool redo) {
        for (const auto& [g, items] : added) engine->scatterSetRemoved(g, items, !redo);
        for (const auto& [g, items] : removed) engine->scatterSetRemoved(g, items, redo);
        *dirty = true;
    };
    m_undo.push({added.empty() ? "Erase objects" : "Scatter objects", [apply] { apply(false); }, [apply] { apply(true); }});
    m_terrainDirty = true;
    updateTitle(true);
}

void Editor::drawScatterPanel() {
    // Paleta: modelos já espalhados neste mapa entram sozinhos.
    for (const auto& g : m_engine->scatterGroups()) {
        bool known = false;
        for (const auto& e : m_scatterPalette) known |= e.file == g.file && e.node == g.node && e.prefab == g.prefab;
        if (!known) {
            const std::string label = std::filesystem::path(g.prefab.empty() ? g.file : g.prefab).filename().string() +
                                      (g.node.empty() ? "" : " / " + g.node);
            m_scatterPalette.push_back({g.file, g.node, g.prefab, label, false});
        }
    }
    ImGui::TextUnformatted(tr("Palette (checked models are scattered):"));
    for (size_t i = 0; i < m_scatterPalette.size(); ++i) {
        auto& e = m_scatterPalette[i];
        ImGui::PushID(int(i));
        ImGui::Checkbox(e.label.c_str(), &e.on);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("x"))) {
            m_scatterPalette.erase(m_scatterPalette.begin() + long(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (m_scatterPalette.empty()) ImGui::TextDisabled("%s", tr("Empty. Select a .glb or prefab in the Project panel and add it."));
    const std::filesystem::path sel(m_projectSelected);
    const bool usable = sel.extension() == ".glb" || Prefab::isPrefabPath(sel);
    ImGui::BeginDisabled(!usable);
    if (ImGui::Button(tr("+ Add selected model"))) {
        std::error_code ec;
        const auto rel = std::filesystem::relative(sel, std::filesystem::current_path(), ec);
        const std::string path = (!ec && !rel.empty() && rel.native().rfind("..", 0) != 0) ? rel.generic_string()
                                                                                            : sel.string();
        ScatterPaletteEntry e;
        if (Prefab::isPrefabPath(sel)) e.prefab = path; else e.file = path;
        e.label = sel.filename().string();
        m_scatterPalette.push_back(e);
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText(tr("Rules"));
    ScatterSettings& st = m_scatterSettings;
    ImGui::DragFloat(tr("Density"), &st.density, 0.1f, 0.05f, 200.0f, "%.2f");
    ImGui::SetItemTooltip("%s", tr("Objects per 100 x 100 units. Also sets the minimum distance between them."));
    ImGui::DragFloatRange2("Scale", &st.scaleMin, &st.scaleMax, 0.01f, 0.05f, 20.0f, "%.2f");
    ImGui::Checkbox(tr("Random turn"), &st.randomYaw);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Lean with the slope"), &st.alignToGround);
    TerrainBrush& b = m_brush;
    ImGui::DragFloat(tr("Size##sc"), &b.size, 0.5f, 2.0f, 2000.0f, "%.0f");
    ImGui::SetItemTooltip("%s", tr("Radius on the ground. [ and ] change it in the Scene."));
}

} // namespace eruption
