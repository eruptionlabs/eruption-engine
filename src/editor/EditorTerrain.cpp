// Ferramenta Terrain: pincel de altura (subir/baixar, suavizar, aplainar) com
// anel de prévia no chão e um desfazer por traço. Os dados ficam no motor
// (core/TerrainEditing.cpp); aqui é só o mouse, o painel e o desfazer.
#include "editor/Editor.hpp"

#include "core/Engine.hpp"
#include "editor/EditorTheme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

const char* kModeNames[] = {"Raise / Lower", "Smooth", "Flatten"};

} // namespace

void Editor::terrainBrushInput(float x, float y, float w, float h) {
    TerrainBrush& b = m_brush;
    const ImGuiIO& io = ImGui::GetIO();
    b.hasCursor = false;
    if (m_play != PlayState::Editing || m_tool != Tool::Terrain) return;
    // Tamanho com [ e ] (como em programas de pintura).
    if (m_viewportHovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) b.size = std::max(2.0f, b.size / 1.2f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) b.size = std::min(2000.0f, b.size * 1.2f);
    }
    const ImVec2 m = io.MousePos;
    Vec3 at;
    if (m_viewportHovered && groundPointAt((m.x - x) / w, (m.y - y) / h, at)) {
        b.cursor = at;
        b.hasCursor = true;
    }
    const bool canPaint = m_engine->terrainEditable();
    if (!b.stroking && canPaint && b.hasCursor && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        b.stroking = true;
        b.snapshot = m_engine->heightEdit().delta;
        b.rect[0] = b.rect[1] = INT32_MAX;
        b.rect[2] = b.rect[3] = INT32_MIN;
        b.flattenTo = at.y; // aplainar: altura onde o traço começou
    }
    if (b.stroking && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (b.hasCursor) {
            // Ctrl inverte subir/baixar; Shift suaviza enquanto segurado.
            Engine::SculptMode mode = Engine::SculptMode::Raise;
            if (io.KeyShift || b.mode == 1) mode = Engine::SculptMode::Smooth;
            else if (b.mode == 2) mode = Engine::SculptMode::Flatten;
            else if (io.KeyCtrl) mode = Engine::SculptMode::Lower;
            int r[4];
            if (m_engine->sculptTerrain(mode, Vec2(b.cursor.x, b.cursor.z), b.size, b.strength, b.falloff, b.flattenTo,
                                        std::min(io.DeltaTime, 0.05f), r)) {
                b.rect[0] = std::min(b.rect[0], r[0]);
                b.rect[1] = std::min(b.rect[1], r[1]);
                b.rect[2] = std::max(b.rect[2], r[2]);
                b.rect[3] = std::max(b.rect[3], r[3]);
            }
        }
    } else if (b.stroking) {
        endTerrainStroke();
    }
}

void Editor::endTerrainStroke() {
    TerrainBrush& b = m_brush;
    b.stroking = false;
    if (b.rect[0] > b.rect[2]) return; // nada tocado
    const int x0 = b.rect[0], z0 = b.rect[1], x1 = b.rect[2], z1 = b.rect[3];
    const uint32_t w = m_engine->heightEdit().w;
    std::vector<float> before;
    before.reserve(size_t(x1 - x0 + 1) * (z1 - z0 + 1));
    for (int z = z0; z <= z1; ++z)
        for (int xx = x0; xx <= x1; ++xx) before.push_back(b.snapshot[size_t(z) * w + xx]);
    std::vector<float> after = m_engine->heightDeltaRegion(x0, z0, x1, z1);
    b.snapshot.clear();
    b.snapshot.shrink_to_fit();
    Engine* engine = m_engine;
    bool* dirty = &m_terrainDirty;
    auto apply = [engine, dirty, x0, z0, x1, z1](const std::vector<float>& v) {
        engine->setHeightDeltaRegion(x0, z0, x1, z1, v);
        *dirty = true;
    };
    m_undo.push({"Terrain stroke", [apply, before] { apply(before); }, [apply, after] { apply(after); }});
    m_terrainDirty = true;
    updateTitle(true);
}

void Editor::drawTerrainBrush(float x, float y, float w, float h) {
    const TerrainBrush& b = m_brush;
    if (m_tool != Tool::Terrain || !b.hasCursor || m_play != PlayState::Editing) return;
    const Mat4 vp = m_engine->camera().viewProjNoJitter();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto ring = [&](float radius, ImU32 col, float thick) {
        constexpr int kSegs = 64;
        ImVec2 pts[kSegs];
        int n = 0;
        for (int i = 0; i < kSegs; ++i) {
            const float a = float(i) / kSegs * 6.2831853f;
            const float px = b.cursor.x + std::cos(a) * radius, pz = b.cursor.z + std::sin(a) * radius;
            // Segue o relevo para o anel não sumir dentro de morro.
            const float py = m_engine->terrainEditable() ? m_engine->sampleTerrainHeight(px, pz) : b.cursor.y;
            const Vec4 c = vp * Vec4(px, py + 0.3f, pz, 1.0f);
            if (c.w <= 1e-3f) return;
            pts[n++] = ImVec2(x + (c.x / c.w * 0.5f + 0.5f) * w, y + (c.y / c.w * 0.5f + 0.5f) * h);
        }
        dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, thick);
    };
    const bool can = m_engine->terrainEditable();
    const ImU32 col = can ? ImGui::GetColorU32(kAccent) : IM_COL32(150, 150, 150, 200);
    ring(b.size, col, 2.0f);
    ring(b.size * std::clamp(1.0f - b.falloff, 0.05f, 1.0f), (col & 0x00FFFFFF) | 0x80000000, 1.0f);
}

void Editor::drawTerrainPanel() {
    TerrainBrush& b = m_brush;
    ImGui::SeparatorText("Terrain brush");
    if (!m_engine->terrainEditable()) {
        ImGui::TextWrapped("This map has no editable ground. Maps made with File > New Map, or with a height "
                           "grid (.ter) that matches the ground mesh, can be sculpted.");
        return;
    }
    for (int i = 0; i < IM_ARRAYSIZE(kModeNames); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kModeNames[i], b.mode == i)) b.mode = i;
    }
    ImGui::DragFloat("Size", &b.size, 0.5f, 2.0f, 2000.0f, "%.0f");
    ImGui::SetItemTooltip("Radius on the ground. [ and ] change it in the Scene.");
    ImGui::SliderFloat("Strength", &b.strength, 1.0f, 200.0f, "%.0f");
    ImGui::SliderFloat("Falloff", &b.falloff, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("0 = hard edge, 1 = fades from the center.");
    ImGui::Spacing();
    ImGui::TextDisabled("Left drag: paint.  Ctrl: lower.  Shift: smooth.");
    ImGui::TextDisabled("Flatten keeps the height where the stroke starts.");
    ImGui::TextDisabled("Ctrl+S saves the terrain with the scene.");
}

} // namespace eruption
