#pragma once

#include "core/Project.hpp"

#include <string>
#include <utility>

struct ImDrawList;
struct ImVec2;

namespace eruption {

// Tela de carga ajustada pelo projeto (bloco "loading" do project.eruption,
// ver Project::Manifest::LoadingScreen): a barra padrao do motor com o texto,
// a escala, as cores e a posicao do jogo.
using LoadingScreenStyle = Project::Manifest::LoadingScreen;

// O estilo do projeto aberto, ou nullptr (sem projeto ou sem o bloco: a tela
// padrao, como sempre). No editor, sempre nullptr.
const LoadingScreenStyle* projectLoadingStyle();

// O editor, com ou sem projeto aberto, fica na tela de carga do motor; a do
// projeto e' so' do jogo (--game, jogo exportado, celular). main.cpp chama
// com o mesmo valor de Engine::setEditorMode.
void setLoadingScreenEditor(bool editor);

// O texto cortado com "..." para caber em `maxWidth` px (medida por `widthOf`).
template <class WidthOf>
std::string fitLoadingText(const std::string& text, float maxWidth, WidthOf&& widthOf) {
    if (widthOf(text.c_str()) <= maxWidth) return text;
    std::string t = text;
    while (!t.empty()) {
        // Tira um caractere UTF-8 inteiro: os bytes de continuacao e o inicial.
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
        if (!t.empty()) t.pop_back();
        if (widthOf((t + "...").c_str()) <= maxWidth) return t + "...";
    }
    return std::string();
}

// Barra, porcentagem e texto. Texto vazio no estilo: o mapa e a etapa.
void drawProjectLoadingBar(ImDrawList* dl, const ImVec2& display, const LoadingScreenStyle& style,
                           const std::string& mapName, const std::string& task, float progress);

} // namespace eruption
