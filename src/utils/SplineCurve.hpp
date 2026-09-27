#pragma once

#include <vector>
#include <algorithm>
#include <string>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace eruption {

enum class InterpolationMode {
    Linear,
    Smoothstep,
    CatmullRom,
    Step
};

struct SplineCurve {
    std::vector<float> pointsX;
    std::vector<float> pointsY;
    std::vector<InterpolationMode> modes;
    float m_maxY = 1.0f;

    SplineCurve() = default;
    SplineCurve(const std::vector<float>& x, const std::vector<float>& y, InterpolationMode defaultMode = InterpolationMode::Linear)
        : pointsX(x), pointsY(y) {
        modes.assign(x.size() > 0 ? x.size() - 1 : 0, defaultMode);
        for (float val : y) m_maxY = std::max(m_maxY, val);
    }
    SplineCurve(const std::vector<float>& x, const std::vector<float>& y, const std::vector<InterpolationMode>& segmentModes)
        : pointsX(x), pointsY(y), modes(segmentModes) {
        for (float val : y) m_maxY = std::max(m_maxY, val);
    }

    // Load from JSON point array: [{"x":0, "y":1, "interpolation":"Linear"}, ...]
    static SplineCurve fromJson(const nlohmann::json& node) {
        if (!node.is_array() || node.size() < 2) return SplineCurve();
        std::vector<float> xs, ys;
        std::vector<InterpolationMode> segmentModes;
        float maxY = 1.0f;
        
        for (size_t i = 0; i < node.size(); i++) {
            const auto& pt = node[i];
            if (pt.contains("x")) xs.push_back(pt["x"].get<float>());
            if (pt.contains("y")) {
                float y = pt["y"].get<float>();
                ys.push_back(y);
                maxY = std::max(maxY, y);
            }
            if (i < node.size() - 1 && pt.contains("interpolation")) {
                std::string modeStr = pt.value("interpolation", "Linear");
                if (modeStr == "Smoothstep") segmentModes.push_back(InterpolationMode::Smoothstep);
                else if (modeStr == "CatmullRom") segmentModes.push_back(InterpolationMode::CatmullRom);
                else if (modeStr == "Step") segmentModes.push_back(InterpolationMode::Step);
                else segmentModes.push_back(InterpolationMode::Linear);
            }
        }
        
        SplineCurve curve;
        if (xs.size() >= 2 && ys.size() >= 2) {
            if (segmentModes.size() == xs.size() - 1) curve = SplineCurve(xs, ys, segmentModes);
            else curve = SplineCurve(xs, ys, InterpolationMode::Linear);
        }
        curve.m_maxY = maxY;
        return curve;
    }

    // Serialize to JSON point array: [{"x":0, "y":1, "interpolation":"Linear"}, ...]
    nlohmann::json toJson() const {
        nlohmann::json node = nlohmann::json::array();
        const char* modeNames[] = { "Linear", "Smoothstep", "CatmullRom", "Step" };
        for (size_t i = 0; i < pointsX.size(); ++i) {
            nlohmann::json pt;
            pt["x"] = pointsX[i];
            pt["y"] = pointsY[i];
            if (i < modes.size()) {
                pt["interpolation"] = modeNames[static_cast<int>(modes[i])];
            }
            node.push_back(pt);
        }
        return node;
    }

    float evaluate(float targetX) const {
        if (pointsX.empty()) return 0.0f;
        if (targetX <= pointsX.front()) return pointsY.front();
        if (targetX >= pointsX.back()) return pointsY.back();

        for (size_t k = 0; k < pointsX.size() - 1; k++) {
            if (targetX >= pointsX[k] && targetX <= pointsX[k+1]) {
                float dist = pointsX[k+1] - pointsX[k];
                float f = (dist > 0.0001f) ? (targetX - pointsX[k]) / dist : 0.0f;
                
                switch(modes[k]) {
                    case InterpolationMode::Linear:
                        return glm::mix(pointsY[k], pointsY[k+1], f);
                    case InterpolationMode::Smoothstep:
                        return glm::mix(pointsY[k], pointsY[k+1], f * f * (3.0f - 2.0f * f));
                    case InterpolationMode::CatmullRom: {
                        float p0 = pointsY[k > 0 ? k-1 : k];
                        float p1 = pointsY[k];
                        float p2 = pointsY[k+1];
                        float p3 = pointsY[k < pointsY.size()-2 ? k+2 : k+1];
                        return 0.5f * (
                            (2.0f * p1) +
                            (-p0 + p2) * f +
                            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f * f +
                            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f * f * f
                        );
                    }
                    case InterpolationMode::Step:
                        return pointsY[k];
                    default: return pointsY[k];
                }
            }
        }
        return pointsY.back();
    }
};

} // namespace eruption
