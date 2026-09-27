#pragma once

#include <imgui.h>
#include <cstddef>
#include <string>
#include <vector>

namespace ImGuiEx {

struct PieChartSlice {
    std::string label;
    float value;
    ImU32 color;
};

// Draws a pie chart at the current cursor position.
// Returns true if drawn successfully.
// highlightIndex: slice to keep full color; others are desaturated.
bool PieChart(const char* id, float radius, const std::vector<PieChartSlice>& slices, int highlightIndex = -1);

// Desaturate an ImU32 color by given factor (0.0 = keep original, 1.0 = full gray)
ImU32 DesaturateColor(ImU32 col, float factor);

} // namespace ImGuiEx
