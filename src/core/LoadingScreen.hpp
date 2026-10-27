#pragma once

#include "core/Project.hpp"

#include <string>

struct ImDrawList;
struct ImVec2;

namespace eruption {

// Tela de carga ajustada pelo projeto (bloco "loading" do project.eruption,
// ver Project::Manifest::LoadingScreen): a barra padrao do motor com o texto,
// a escala, as cores e a posicao do jogo.
using LoadingScreenStyle = Project::Manifest::LoadingScreen;

// O estilo do projeto aberto, ou nullptr (sem projeto ou sem o bloco: a tela
// padrao, como sempre).
const LoadingScreenStyle* projectLoadingStyle();

// Barra, porcentagem e texto. Texto vazio no estilo: o mapa e a etapa.
void drawProjectLoadingBar(ImDrawList* dl, const ImVec2& display, const LoadingScreenStyle& style,
                           const std::string& mapName, const std::string& task, float progress);

} // namespace eruption
