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
    };

    void build();
    Element* find(const std::string& selector);
    const Element* find(const std::string& selector) const;
    // Retângulo do elemento, resolvendo o pai (uma cadeia curta; ciclo para).
    CSSRect resolveRect(const Element& e, const Vec2& area) const;

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
