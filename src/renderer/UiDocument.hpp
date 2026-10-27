#pragma once

// DOCUMENTO DE INTERFACE EM CSS — a interface do jogo, descrita em arquivo.
//
// O motor já falava CSS, mas só para a HUD do jogador: game/HudRenderer
// desenha uma lista FECHADA de widgets (#hero-panel, #hero-hp-bar, #minimap)
// e nada mais. Um jogo precisa de menu, listas, ficha de personagem, caixa
// de diálogo, tela de título — e nenhuma lista fechada cobre isso.
//
// Aqui a lista é o PRÓPRIO ARQUIVO: cada regra de id vira um elemento, e o
// tipo sai da propriedade `--widget` (rect, text, bar, image). O script só
// preenche valor (texto, proporção da barra, visível ou não); onde fica, de
// que cor e de que tamanho é decisão do .css — recarregado a quente quando o
// arquivo muda, e editável no Caldera.
//
// É o que torna a interface modificável sem compilar: trocar um arquivo de
// texto muda a cara do jogo.
//
// INTERFACE COM LISTA. Uma ficha por personagem, um slot por turno na fila:
// os elementos saem repetidos, e escrever cada um inteiro levou uma interface
// de quatro fichas a 760 linhas em que quase tudo era
// cópia — mudar a cor da barra de vida eram quatro edições que tinham de
// combinar. `--class` resolve isso no parser (ver utils/CSSParser.cpp):
//
//     .barra-hp { --widget: bar; width: 60%; --fill-color: #d84040; ... }
//     #hp-1 { --class: barra-hp; --parent: #ficha-1; }
//     #hp-2 { --class: barra-hp; --parent: #ficha-2; }
//
// O id continua vencendo a classe, como em CSS. O mesmo arquivo depois da
// conversão: 417 linhas, os mesmos 81 elementos.
//
// O documento NÃO desenha mundo nenhum: é 2D puro sobre a área de jogo.
// Efeito que precisa de projeção (número subindo do mundo) é do UiCanvas.

#include <functional>
#include "math/Types.hpp"
#include "utils/CSSLayout.hpp"
#include "utils/CSSParser.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawList;

namespace eruption {

class UiDocument {
public:
    enum class Widget : uint8_t { Rect, Text, Bar, Image };

    // Carrega (ou recarrega) o arquivo. Falso + motivo em *error.
    bool load(const std::filesystem::path& cssPath, std::string* error = nullptr);
    // Relê o arquivo se ele mudou no disco. Chamar uma vez por frame.
    void reloadIfChanged();
    void clear();

    bool loaded() const { return m_loaded; }
    const std::filesystem::path& path() const { return m_path; }
    size_t elementCount() const { return m_elements.size(); }
    bool has(const std::string& selector) const { return find(selector) != nullptr; }
    // Desenhado agora, como em draw(): o elemento e todos os pais (--parent)
    // visiveis, e nenhum ancorado fora da tela.
    bool shown(const std::string& selector) const {
        const Element* e = find(selector);
        return e && m_loaded && !chainHidden(*e);
    }

    // Valores que o jogo preenche. Selector com ou sem "#" - os dois servem.
    void setText(const std::string& selector, const std::string& text);
    // Troca o arquivo de um widget de imagem em tempo de execucao. O CSS ja'
    // sabia desenhar imagem (--widget: image, --image: "..."), mas o arquivo
    // era fixo no arquivo de estilo: uma ficha de personagem ou um retrato
    // numa lista, que mudam conforme quem esta' em cena, nao tinham como
    // ser escritos pelo script.
    void setImage(const std::string& selector, const std::string& png);
    void setValue(const std::string& selector, float value01);
    void setVisible(const std::string& selector, bool visible);
    // Cor viva por cima da do arquivo. `which`: 0 = fundo/trilho/texto,
    // 1 = preenchimento da barra ou borda. Alfa negativo mantém o do CSS.
    void setColor(const std::string& selector, int which, const Vec4& color);
    // Posicao e tamanho por script, em % do pai (ou da tela, sem pai), como
    // left/top/width/height do CSS; tamanho negativo mantem o do arquivo.
    // clearPlace devolve o elemento ao CSS.
    void setPlace(const std::string& selector, float leftPct, float topPct, float widthPct, float heightPct);
    void clearPlace(const std::string& selector);
    // Centro do elemento num ponto da area (pixels), posto a cada quadro por
    // quem segue algo no mundo (ui.anchor). `onScreen` falso esconde o
    // elemento e os filhos; os filhos seguem o pai pela cadeia de --parent.
    void setAnchor(const std::string& selector, float x, float y, bool onScreen);
    void clearAnchor(const std::string& selector);
    // Retrato (ui.portrait): partes de um quadro de sprite desenhadas dentro
    // do elemento, postas a cada quadro. `rect` em fracao da vista (x0, y0,
    // x1, y1; pode sair dela, o elemento recorta); a vista de proporcao
    // `aspect` (largura/altura) cabe inteira no elemento, centrada.
    struct PortraitPart {
        void* tex = nullptr;
        Vec4 rect{0.0f};
        Vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
        uint32_t tint = 0xFFFFFFFFu; // ImU32
    };
    void setPortrait(const std::string& selector, std::vector<PortraitPart> parts, float aspect);
    void clearPortrait(const std::string& selector);
    // Selo sobre o canto de cima a' direita do elemento ("+", um numero, um
    // icone): texto, ou imagem quando `image` nao e' vazia. Some com o
    // elemento. O toque nele e' dele (seletor "#id:badge"), nao do elemento.
    void setBadge(const std::string& selector, const std::string& text, const std::string& image);
    void clearBadge(const std::string& selector);
    bool hasBadge(const std::string& selector) const {
        const Element* e = find(selector);
        return e && e->hasBadge;
    }
    // Circulo desenhado e area de toque (quadrado de lado >= minTouch, no
    // mesmo centro), em pixels da tela. Falso sem selo.
    bool badgeRects(const std::string& selector, float x, float y, float w, float h, float minTouch,
                    CSSRect& visual, CSSRect& touch) const;
    // Algum selo desenhado agora tem a area de toque sobre `p` (pixels da tela)?
    bool badgeTouchAt(float x, float y, float w, float h, float minTouch, const Vec2& p) const;
    // Grafo de nos sobre o elemento (ui.graph): cada no com icone (PNG) ou
    // texto, e ligacoes para outros nos por id. `pos` em fracao do retangulo
    // do elemento; sem ela (< 0), camadas pela profundidade a partir das
    // raizes (nos onde nenhuma ligacao chega), de cima para baixo, ou da
    // esquerda para a direita com --graph-direction: right.
    struct GraphNode {
        std::string id, text, image;
        Vec2 pos{-1.0f};
        bool hasColor = false;
        Vec4 color{1.0f};
        std::vector<std::string> links;
    };
    void setGraph(const std::string& selector, std::vector<GraphNode> nodes);
    void clearGraph(const std::string& selector);
    // No sob o ponto p (pixels da tela): o de centro mais perto, a ate' o
    // maior entre o raio desenhado e minTouch/2. Falso se nenhum.
    bool graphNodeAt(const std::string& selector, float x, float y, float w, float h, float minTouch, const Vec2& p,
                     std::string& id) const;
    // Centro de um no em pixels da tela (teste de toque).
    bool graphNodeCenter(const std::string& selector, const std::string& id, float x, float y, float w, float h,
                         Vec2& center) const;
    // Recorte da imagem em coordenadas de textura (0..1): uma janela de uma
    // imagem maior (minimapa, creditos rolando). clearImageRect volta ao todo.
    void setImageRect(const std::string& selector, float u0, float v0, float u1, float v1);
    void clearImageRect(const std::string& selector);
    // Volta tudo o que o script mexeu para o que o arquivo diz.
    void resetOverrides();

    // Retângulo resolvido de um elemento, em pixels da área (útil para
    // ancorar efeito de mundo em cima de um widget).
    bool rectOf(const std::string& selector, float x, float y, float w, float h, CSSRect& out) const;

    void draw(ImDrawList* dl, float x, float y, float w, float h) const;

    // Resolvedor de imagem: recebe o caminho do PNG e devolve o handle de
    // textura do ImGui (nullptr se nao der). O documento de UI nao fala com
    // o Vulkan - quem sobe a textura e' o Engine, que injeta esta funcao.
    // Sem ela o widget de imagem continua sendo so' a moldura com o nome do
    // arquivo, que era o unico comportamento que existia.
    void setImageResolver(std::function<void*(const std::string&)> fn) { m_imageResolver = std::move(fn); }

private:
    struct Element {
        std::string selector;   // com "#"
        std::string parent;     // vazio = relativo à tela
        Widget widget = Widget::Rect;
        int z = 0;
        bool visibleCss = true;
        std::string textCss;
        float valueCss = 1.0f;
        std::string image;
        uint8_t align = 0;      // 0 esquerda, 1 centro, 2 direita
        float fontSize = 0.03f; // fração da altura da área
        // O que o script mandou (vence o CSS enquanto estiver posto).
        bool hasText = false, hasValue = false, hasVisible = false;
        std::string text;
        float value = 1.0f;
        bool visible = true;
        bool hasColor[2] = {false, false};
        Vec4 color[2] = {Vec4(1.0f), Vec4(1.0f)};
        bool hasPlace = false;
        Vec4 place{0.0f};             // left, top, width, height em fracao do pai
        Vec4 uv{0.0f, 0.0f, 1.0f, 1.0f}; // recorte da imagem (u0, v0, u1, v1)
        uint32_t tintCss = 0xFFFFFFFFu;  // --image-tint (ImU32); ui.color(sel, cor, 0) troca
        // --bar-shape: arc. Graus como no CSS (0 = em cima, sentido horario);
        // espessura em fracao do menor lado do retangulo.
        bool arc = false;
        float arcStart = -70.0f, arcEnd = 70.0f, arcThickness = 0.15f;
        bool hasAnchor = false, anchorOn = false;
        Vec2 anchor{0.0f};            // centro em pixels da area
        bool hasBadge = false;
        std::string badgeText, badgeImage;
        std::vector<PortraitPart> portrait;
        float portraitAspect = 1.0f;
        // Grafo: nos, posicao de cada um (0..1; auto = da camada) e as
        // ligacoes ja' em indices, resolvidos uma vez em setGraph.
        std::vector<GraphNode> graph;
        std::vector<Vec2> graphAt;
        std::vector<uint8_t> graphAuto;
        std::vector<std::pair<uint16_t, uint16_t>> graphEdges;
        // Forma e efeito (lidos em build, nao por quadro):
        // transform: rotate(N deg|rad|turn) em torno do centro, sentido horario;
        // clip-path: polygon(...) em rect e bar;
        // border-radius com um raio por canto (com um so' fica o caminho antigo);
        // --glow: <cor> <raio> ou box-shadow: dx dy desfoque [espalhar] <cor>.
        float rotate = 0.0f;
        std::vector<CSSLayout::PolyPoint> clip;
        bool corners = false;
        float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        uint8_t radiiPct = 0;
        uint32_t glowColor = 0;
        // Numeros do --glow (raio) ou do box-shadow (dx dy desfoque espalhar)
        // com a unidade de cada um (0 px, 1 vw, 2 vh, 3 vmin, 4 vmax): o
        // tamanho de tela so' se sabe no desenho.
        bool glowShadow = false;
        float glowNum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        uint8_t glowUnit[4] = {0, 0, 0, 0};
    };
    // O elemento ou um pai esta' escondido (ui.visible ou CSS), ou ancorado
    // fora da tela. Esconder o pai esconde o grupo inteiro.
    bool chainHidden(const Element& e) const;
    CSSRect badgeVisual(const Element& e, const Vec2& area) const;
    float graphRadius(const Element& e, const Vec2& area) const;
    bool graphSideways(const Element& e) const;
    static Vec2 graphPoint(const Element& e, size_t i, const CSSRect& r, bool sideways);
    void drawGraph(ImDrawList* dl, const Element& e, const CSSRect& r, const Vec2& area, float alpha) const;
    static void drawPortrait(ImDrawList* dl, const Element& e, const CSSRect& r, float alpha);
    void drawBadge(ImDrawList* dl, const Element& e, const CSSRect& c) const;
    // Rect e bar com clip-path ou cantos diferentes: o contorno vira poligono,
    // a barra enche recortada pelo valor.
    void drawShaped(ImDrawList* dl, const Element& e, const CSSRect& r, const Vec2& area, float alpha, ImU32 bg,
                    ImU32 edge) const;

    void build();
    static void drawArcBar(ImDrawList* dl, const CSSRect& r, const Element& e, float value, uint32_t rail, uint32_t fill);
    Element* find(const std::string& selector);
    const Element* find(const std::string& selector) const;
    // Retângulo do elemento, resolvendo o pai (uma cadeia curta; ciclo para).
    CSSRect resolveRect(const Element& e, const Vec2& area) const;
    // O retangulo do proprio elemento dentro do pai: o do CSS, ou o posto por
    // setPlace. A cadeia de pais passa por aqui, entao os filhos acompanham.
    CSSRect ownRect(const Element& e, const ImVec2& display, const CSSRect& parent) const;

    CSSParser m_parser;
    CSSLayout m_layout{&m_parser};
    std::filesystem::path m_path;
    std::filesystem::file_time_type m_mtime{};
    bool m_loaded = false;
    std::vector<Element> m_elements;               // já em ordem de desenho (z)
    std::unordered_map<std::string, size_t> m_byId; // "#id" -> índice

    std::function<void*(const std::string&)> m_imageResolver;
};

} // namespace eruption
