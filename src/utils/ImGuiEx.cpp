#include "utils/ImGuiEx.hpp"
#include <imgui_internal.h>
#include <cmath>

#ifndef IM_PI
#define IM_PI 3.14159265358979323846f
#endif

namespace ImGuiEx {

ImU32 DesaturateColor(ImU32 col, float factor) {
    if (factor <= 0.0f) return col;
    if (factor >= 1.0f) factor = 1.0f;
    int r = (col >> IM_COL32_R_SHIFT) & 0xFF;
    int g = (col >> IM_COL32_G_SHIFT) & 0xFF;
    int b = (col >> IM_COL32_B_SHIFT) & 0xFF;
    int a = (col >> IM_COL32_A_SHIFT) & 0xFF;
    float lum = r * 0.299f + g * 0.587f + b * 0.114f;
    int nr = static_cast<int>(r + (lum - r) * factor);
    int ng = static_cast<int>(g + (lum - g) * factor);
    int nb = static_cast<int>(b + (lum - b) * factor);
    return IM_COL32(
        nr < 0 ? 0 : (nr > 255 ? 255 : nr),
        ng < 0 ? 0 : (ng > 255 ? 255 : ng),
        nb < 0 ? 0 : (nb > 255 ? 255 : nb),
        a);
}

bool PieChart(const char* id, float radius, const std::vector<PieChartSlice>& slices, int highlightIndex) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + radius * 2.0f, pos.y + radius * 2.0f));

    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, window->GetID(id))) return false;

    ImDrawList* draw_list = window->DrawList;
    ImVec2 center = ImVec2(pos.x + radius, pos.y + radius);

    float total = 0.0f;
    for (const auto& slice : slices) {
        if (slice.value > 0.0f) total += slice.value;
    }

    if (total <= 0.0f) {
        draw_list->AddCircleFilled(center, radius, IM_COL32(100, 100, 100, 255));
        return true;
    }

    float angle_start = -IM_PI / 2.0f; // Start at top
    int idx = 0;
    for (const auto& slice : slices) {
        if (slice.value <= 0.0f) { idx++; continue; }

        float fraction = slice.value / total;
        float angle_step = fraction * 2.0f * IM_PI;
        float angle_end = angle_start + angle_step;

        ImU32 drawColor = slice.color;
        if (highlightIndex >= 0 && idx != highlightIndex) {
            drawColor = DesaturateColor(slice.color, 0.75f);
        }

        // If it's the whole circle
        if (fraction >= 0.999f) {
            draw_list->AddCircleFilled(center, radius, drawColor);
        } else {
            draw_list->PathLineTo(center);
            draw_list->PathArcTo(center, radius, angle_start, angle_end, 32);
            draw_list->PathFillConvex(drawColor);
        }

        angle_start = angle_end;
        idx++;
    }

    return true;
}

} // namespace ImGuiEx
