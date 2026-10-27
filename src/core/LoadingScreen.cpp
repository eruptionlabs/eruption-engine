#include "core/LoadingScreen.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace eruption {

namespace {

ImU32 color(const float c[3], float alpha = 1.0f) {
    return IM_COL32(int(c[0] * 255.0f + 0.5f), int(c[1] * 255.0f + 0.5f), int(c[2] * 255.0f + 0.5f),
                    int(alpha * 255.0f + 0.5f));
}

// Contorno: a letra repetida num circulo de raio `stroke` atras dela. So'
// as 4 diagonais deixavam buraco nas retas a partir de 2 px; 8 cobrem ate'
// 2 px e 16 o resto.
void strokedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float stroke,
                 ImU32 strokeCol) {
    if (stroke > 0.0f) {
        const int n = stroke <= 2.0f ? 8 : 16;
        for (int i = 0; i < n; ++i) {
            const float a = 6.2831853f * float(i) / float(n);
            dl->AddText(font, size, ImVec2(pos.x + std::cos(a) * stroke, pos.y + std::sin(a) * stroke), strokeCol, text);
        }
    }
    dl->AddText(font, size, pos, col, text);
}

// Listras diagonais claras sobre um preenchimento, recortadas nele.
void drawStripes(ImDrawList* dl, ImVec2 fillMin, ImVec2 fillMax, float s) {
    dl->PushClipRect(fillMin, fillMax, true);
    for (float x = fillMin.x - 10.0f * s; x < fillMax.x; x += 8.0f * s)
        dl->AddLine(ImVec2(x, fillMin.y - 2.0f * s), ImVec2(x + 8.0f * s, fillMax.y + 2.0f * s), IM_COL32(255, 255, 255, 60),
                    1.5f * s);
    dl->PopClipRect();
}

// A barra padrao do motor: trilho translucido e preenchimento listrado.
void drawStripedBar(ImDrawList* dl, ImVec2 pMin, ImVec2 pMax, float s, float progress, const LoadingScreenStyle& style) {
    const float rounding = 3.0f * s;
    dl->AddRectFilled(pMin, pMax, IM_COL32(15, 15, 15, 180), rounding);
    const float fill = (pMax.x - pMin.x) * progress;
    if (fill <= 0.0f) return;
    const ImVec2 fillMax(pMin.x + fill, pMax.y);
    dl->AddRectFilled(pMin, fillMax, color(style.barColor), rounding);
    if (style.stripes) drawStripes(dl, pMin, fillMax, s);
    dl->AddLine(ImVec2(pMin.x + 1, pMin.y + 1), ImVec2(fillMax.x - 1, pMin.y + 1), IM_COL32(255, 255, 255, 30), s);
}

// "style": "frame": moldura fina de cantos pouco arredondados, trilho
// escuro e a barra (listrada ou lisa) recuada dentro. Em escala 3: contorno
// 2, recuo 4.
void drawFramedBar(ImDrawList* dl, ImVec2 pMin, ImVec2 pMax, float s, float progress, const LoadingScreenStyle& style) {
    const float border = std::max(1.0f, std::round(2.0f / 3.0f * s));
    const float inset = border + std::max(1.0f, std::round(4.0f / 3.0f * s));
    const float radius = std::max(1.0f, s);
    dl->AddRectFilled(pMin, pMax, color(style.trackColor), radius);
    const float half = border * 0.5f;
    dl->AddRect(ImVec2(pMin.x + half, pMin.y + half), ImVec2(pMax.x - half, pMax.y - half), color(style.frameColor),
                radius, 0, border);
    const float inner = pMax.x - pMin.x - 2.0f * inset;
    if (progress <= 0.0f || inner <= 0.0f) return;
    const ImVec2 fillMin(pMin.x + inset, pMin.y + inset), fillMax(pMin.x + inset + inner * progress, pMax.y - inset);
    dl->AddRectFilled(fillMin, fillMax, color(style.barColor), std::max(0.0f, radius - inset * 0.5f));
    if (style.stripes) drawStripes(dl, fillMin, fillMax, s);
}

} // namespace

const LoadingScreenStyle* projectLoadingStyle() {
    const Project& p = Project::instance();
    if (!p.isOpen() || !p.manifest().loading) return nullptr;
    return &*p.manifest().loading;
}

void drawProjectLoadingBar(ImDrawList* dl, const ImVec2& display, const LoadingScreenStyle& style,
                           const std::string& mapName, const std::string& task, float progress) {
    // As medidas da barra padrao (220 x 6 px, 40 de margem), na escala do
    // projeto; com moldura, a caixa e' a moldura (28 px de altura em escala 3).
    const float s = style.scale;
    const float width = style.width > 0.0f ? std::round(display.x * style.width) : std::min(220.0f * s, display.x * 0.9f);
    const float height = style.frame ? std::round(28.0f / 3.0f * s) : 6.0f * s;
    const float margin = 40.0f * s;
    progress = std::clamp(progress, 0.0f, 1.0f);

    ImVec2 pMin, pMax;
    if (style.center) {
        pMin = ImVec2(std::round((display.x - width) * 0.5f), std::round(display.y * 0.5f - height * 0.5f));
    } else {
        pMin = ImVec2(display.x - margin - width, display.y - margin - height);
    }
    pMax = ImVec2(pMin.x + width, pMin.y + height);

    if (style.hasBackground) dl->AddRectFilled(ImVec2(0, 0), display, color(style.background));
    if (style.frame) drawFramedBar(dl, pMin, pMax, s, progress, style);
    else drawStripedBar(dl, pMin, pMax, s, progress, style);

    ImFont* font = ImGui::GetFont();
    const float ts = style.textScale > 0.0f ? style.textScale : s;
    const float size = ImGui::GetFontSize() * ts;
    const float stroke = style.strokeWidth < 0.0f ? std::max(1.0f, ts) : style.strokeWidth;
    const ImU32 strokeCol = color(style.strokeColor);
    const ImU32 textCol = color(style.textColor);
    const std::string pct = std::to_string(int(progress * 100.0f)) + "%";
    const ImVec2 pctSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, pct.c_str());
    strokedText(dl, font, size, ImVec2(pMax.x - pctSize.x, pMin.y - pctSize.y - 5.0f * ts), textCol, pct.c_str(), stroke, strokeCol);

    const float line = pMin.y - pctSize.y - 5.0f * ts;
    if (!style.text.empty()) {
        strokedText(dl, font, size, ImVec2(pMin.x, line), textCol, style.text.c_str(), stroke, strokeCol);
    } else if (!task.empty()) {
        strokedText(dl, font, size, ImVec2(pMin.x, line), IM_COL32(150, 150, 150, 255), task.c_str(), stroke, strokeCol);
        if (!mapName.empty())
            strokedText(dl, font, size, ImVec2(pMin.x, line - size - 2.0f * ts), textCol, mapName.c_str(), stroke, strokeCol);
    }
}

} // namespace eruption
