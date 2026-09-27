#pragma once
// HUD da aplicacao, dirigida por CSS (data/hud/default.css).
//
// A engine desenha os widgets; o LAYOUT (posicao, tamanho, cores, bordas)
// vem do CSS e e' recarregado a quente quando o arquivo muda. Os valores
// exibidos (nome, nivel, HP, SP) sao fornecidos pela aplicacao via HudStats.
//
// Widgets reconhecidos (seletores):
//   #hero-panel     painel inferior
//   #hero-portrait  moldura do retrato (relativa ao painel)
//   #hero-level     selo de nivel (relativo ao painel)
//   #hero-hp-bar    barra de HP (relativa ao painel)
//   #hero-sp-bar    barra de SP (relativa ao painel)
//   #minimap        area do minimapa (desenhado pelo Engine)
//   #minimap-info   caixa de nome do mapa / horario
//   #overhead-hp-sp barras de HP/SP sobre o personagem (--bind-to-player)
//   #cast-bar       barra de conjuracao, abaixo das barras de HP/SP
//
// Propriedade customizada `--bind-to-player: 1` faz left/top virarem um
// deslocamento a partir da base do sprite do jogador, escalado pelo zoom.
#include "utils/CSSParser.hpp"
#include "utils/CSSLayout.hpp"
#include <imgui.h>
#include <filesystem>
#include <string>

namespace eruption {

class Engine;
class PlayerController;

struct HudStats {
    std::string name = "Hero";
    int level = 1;
    float hp = 100.0f, hpMax = 100.0f;
    float sp = 50.0f,  spMax = 50.0f;
    // Conjuracao em andamento: 0 = nenhuma (barra escondida), 0..1 = progresso.
    float castProgress = 0.0f;
    std::string castLabel;
};

class HudRenderer {
public:
    void init(Engine* engine, PlayerController* player, const std::string& cssPath = "data/hud/default.css");
    void shutdown();

    // Recarrega o CSS se o arquivo mudou (chamar uma vez por frame).
    void update();

    // Desenha o painel do heroi (chamar dentro do frame ImGui).
    void draw();

    CSSRect getMinimapRect(const ImVec2& displaySize) const;
    CSSRect getMinimapInfoRect(const ImVec2& displaySize) const;

    HudStats& stats() { return m_stats; }
    const CSSLayout& layout() const { return m_layout; }

    // Retangulo de um widget, com --bind-to-player resolvido.
    CSSRect resolveWidget(const std::string& selector, const ImVec2& displaySize,
                          const CSSRect& parent = {}, bool* found = nullptr) const;

private:
    void drawStyleBox(ImDrawList* dl, const CSSRect& r, const std::string& selector) const;
    void drawBar(ImDrawList* dl, const CSSRect& r, float ratio, ImU32 fillLow, ImU32 fillHigh,
                 const std::string& selector, const char* valueText) const;
    void reloadIfChanged();

    Engine* m_engine = nullptr;
    PlayerController* m_player = nullptr;
    std::string m_cssPath;
    CSSParser m_parser;
    CSSLayout m_layout;
    std::filesystem::file_time_type m_cssMTime{};
    bool m_loaded = false;
    HudStats m_stats;
};

} // namespace eruption
