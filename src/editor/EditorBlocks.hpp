#pragma once

// Desenho dos blocos de encaixar, comum às regras de jogo e às receitas do
// gerador de mapas.
#include <imgui.h>

namespace eruption {

inline ImU32 darker(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::GetColorU32(ImVec4(v.x * k, v.y * k, v.z * k, v.w));
}

// Forma do bloco: 0 chapéu (evento), 1 empilhável (ação), 2 condição (pílula).
inline void blockShape(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, int kind) {
    if (kind == 2) {
        dl->AddRectFilled(a, b, col, (b.y - a.y) * 0.5f);
        return;
    }
    const float r = 5.0f;
    if (kind == 0) {
        dl->AddRectFilled(a, b, col, 12.0f, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(ImVec2(a.x, b.y - r), b, col);
    } else {
        dl->AddRectFilled(a, b, col, r);
        // Entalhe em cima (onde o bloco de cima encaixa).
        dl->AddRectFilled(ImVec2(a.x + 12, a.y), ImVec2(a.x + 28, a.y + 4), darker(col, 0.55f), 2.0f);
    }
    // Dente embaixo (encaixa no próximo).
    dl->AddRectFilled(ImVec2(a.x + 12, b.y), ImVec2(a.x + 28, b.y + 4), col, 2.0f);
}

} // namespace eruption
