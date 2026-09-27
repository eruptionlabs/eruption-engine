#pragma once

#include <imgui.h>
#include "utils/SplineCurve.hpp"
#include <string>
#include <cstdio>

namespace eruption {

class SplineEditorUI {
public:
    // Legacy draw with X normalized to 0..1 and displayed as percent.
    static void draw(const char* label, SplineCurve& curve, float minY, float maxY, float currentX, float currentY, bool isAperture = false, const char* xLabel = "Zoom %") {
        draw(label, curve, 0.0f, 1.0f, minY, maxY, currentX, currentY, isAperture, xLabel, true);
    }

    // Flexible draw with real X range. X values are still stored normalized 0..1 in the curve,
    // but the UI displays and edits them in [minX, maxX] units.
    static void draw(const char* label, SplineCurve& curve, float minX, float maxX, float minY, float maxY, float currentXReal, float currentY, bool isAperture = false, const char* xLabel = "X", bool displayAsPercent = false) {
        ImGui::PushID(label);
        ImGui::Text("%s", label);
        
        // Use curve's own m_maxY if it's larger than provided maxY to ensure points are visible
        float effectiveMaxY = std::max(maxY, curve.m_maxY);
        
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImVec2(ImGui::GetContentRegionAvail().x, 120.0f);
        
        ImGui::Dummy(canvas_size);
        
        draw_list->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(30, 30, 30, 255));
        draw_list->AddRect(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(100, 100, 100, 255));

        auto to_canvas = [&](float x, float y) {
            float normY = (effectiveMaxY - minY > 0.0001f) ? (y - minY) / (effectiveMaxY - minY) : 0.5f;
            return ImVec2(canvas_pos.x + x * canvas_size.x, 
                          canvas_pos.y + (1.0f - normY) * canvas_size.y);
        };

        const char* interpNames[] = { "Linear", "Smoothstep", "Catmull-Rom", "Step" };

        // Draw curve
        for (int i = 0; i < 100; i++) {
            float t0 = (float)i / 100.0f;
            float t1 = (float)(i + 1) / 100.0f;
            draw_list->AddLine(to_canvas(t0, curve.evaluate(t0)), 
                               to_canvas(t1, curve.evaluate(t1)), 
                               isAperture ? IM_COL32(0, 200, 255, 255) : IM_COL32(0, 255, 100, 255), 2.0f);
        }

        // Draggable points
        for (size_t i = 0; i < curve.pointsX.size(); i++) {
            ImVec2 p_pos = to_canvas(curve.pointsX[i], curve.pointsY[i]);
            
            ImGui::PushID((int)i);
            ImGui::SetCursorScreenPos(ImVec2(p_pos.x - 10, p_pos.y - 10));
            ImGui::InvisibleButton("dot", ImVec2(20, 20));
            
            bool hovered = ImGui::IsItemHovered();
            bool active = ImGui::IsItemActive();
            
            if (active && ImGui::IsMouseDown(0)) {
                ImVec2 mouse = ImGui::GetIO().MousePos;
                float tx = (mouse.x - canvas_pos.x) / canvas_size.x;
                float ty = 1.0f - (mouse.y - canvas_pos.y) / canvas_size.y;
                
                float minX = (i > 0) ? curve.pointsX[i-1] + 0.001f : 0.0f;
                float maxX = (i < curve.pointsX.size() - 1) ? curve.pointsX[i+1] - 0.001f : 1.0f;
                
                curve.pointsX[i] = std::max(minX, std::min(maxX, tx));
                curve.pointsY[i] = minY + std::max(0.0f, std::min(1.0f, ty)) * (effectiveMaxY - minY);
                
                // Update m_maxY if point dragged higher
                curve.m_maxY = std::max(curve.m_maxY, curve.pointsY[i]);
            }

            ImU32 col = (active || hovered) ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 120, 255, 255);
            draw_list->AddCircleFilled(p_pos, 6.0f, col);
            draw_list->AddCircle(p_pos, 7.0f, IM_COL32(255, 255, 255, 200));
            ImGui::PopID();
        }

        float xRange = maxX - minX;

        // Current value indicator (Red Dot)
        float currentXNorm = (xRange > 0.0001f) ? (currentXReal - minX) / xRange : 0.0f;
        draw_list->AddCircleFilled(to_canvas(currentXNorm, currentY), 6.0f, IM_COL32(255, 0, 0, 255));

        // Inputs below
        ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x, canvas_pos.y + canvas_size.y + 5));
        for (size_t i = 0; i < curve.pointsX.size(); i++) {
            ImGui::PushID((int)i);
            ImGui::BeginGroup();

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.16f);
            float realX = curve.pointsX[i] * xRange + minX;
            if (ImGui::DragFloat(xLabel, &realX, 0.1f, minX, maxX, displayAsPercent ? "%.0f%%" : "%.1f")) {
                float minXN = (i > 0) ? curve.pointsX[i-1] + 0.001f : 0.0f;
                float maxXN = (i < curve.pointsX.size() - 1) ? curve.pointsX[i+1] - 0.001f : 1.0f;
                float realMinX = minXN * xRange + minX;
                float realMaxX = maxXN * xRange + minX;
                float clampedRealX = std::max(realMinX, std::min(realMaxX, realX));
                curve.pointsX[i] = (clampedRealX - minX) / xRange;
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.20f);
            float vVal = curve.pointsY[i];
            if (ImGui::DragFloat(isAperture ? "Apt %" : "Val", &vVal, 0.001f, minY, effectiveMaxY, "%.2f")) {
                curve.pointsY[i] = vVal;
                curve.m_maxY = std::max(curve.m_maxY, curve.pointsY[i]);
            }

            if (i < curve.modes.size()) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.32f);
                int curM = (int)curve.modes[i];
                if (ImGui::Combo("Transition", &curM, interpNames, 4)) {
                    curve.modes[i] = (InterpolationMode)curM;
                }
            }

            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
};

} // namespace eruption
