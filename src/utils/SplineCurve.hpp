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

    // DEBUG DESTA SPLINE (cada spline tem o seu, independente das outras). Ligado,
    // o editor pinta cada bolinha com a cor do SEU PONTO e o traco entre dois
    // pontos como degrade da cor de um para a do proximo; se a spline muda algo
    // no mundo, o mapa tinge a cena com essa mesma cor, na posicao x da entrada.
    // Nao e' salvo no JSON nem copiado de preset: e' so' de quem esta' olhando.
    bool debug = false;

    // Paleta fixa dos pontos, na ordem do dono. A cor vem do INDICE do ponto na
    // spline (ponto 0 = azul, 1 = roxo...), entao e' a mesma em toda spline e em
    // todo quadro. Passada a oitava volta ao azul: com 8 cores distintas o
    // vizinho nunca repete a cor (o 7o e' laranja, o 8o volta azul).
    static constexpr int kDebugPaletteSize = 8;
    static glm::vec3 debugPointColor(size_t index) {
        static const glm::vec3 kPalette[kDebugPaletteSize] = {
            {0.20f, 0.45f, 1.00f},  // azul
            {0.65f, 0.30f, 1.00f},  // roxo
            {1.00f, 0.15f, 0.15f},  // vermelho
            {0.15f, 0.85f, 0.25f},  // verde
            {1.00f, 0.92f, 0.15f},  // amarelo
            {1.00f, 0.20f, 0.90f},  // magenta
            {0.15f, 0.92f, 0.96f},  // ciano
            {1.00f, 0.58f, 0.12f},  // laranja
        };
        return kPalette[index % kDebugPaletteSize];
    }

    // Cor do debug na posicao x do eixo de entrada: degrade linear entre as cores
    // dos dois pontos vizinhos (antes do primeiro e depois do ultimo, a do ponto).
    glm::vec3 debugColorAt(float x) const {
        if (pointsX.empty()) return debugPointColor(0);
        if (x <= pointsX.front()) return debugPointColor(0);
        for (size_t k = 0; k + 1 < pointsX.size(); ++k) {
            if (x <= pointsX[k + 1]) {
                const float d = pointsX[k + 1] - pointsX[k];
                const float f = d > 0.0001f ? (x - pointsX[k]) / d : 0.0f;
                return glm::mix(debugPointColor(k), debugPointColor(k + 1), f);
            }
        }
        return debugPointColor(pointsX.size() - 1);
    }

    // Maior valor (y) dos pontos: o que normaliza a intensidade do tingimento.
    float debugMaxValue() const {
        float m = 0.0001f;
        for (float y : pointsY) m = std::max(m, y);
        return m;
    }

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
