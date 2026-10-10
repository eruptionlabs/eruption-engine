#pragma once
#include "utils/CSSParser.hpp"
#include <imgui.h>
#include <cstdint>
#include <string>

namespace eruption {

struct CSSRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    ImVec2 min() const { return ImVec2(x, y); }
    ImVec2 max() const { return ImVec2(x + w, y + h); }
    ImVec2 center() const { return ImVec2(x + w * 0.5f, y + h * 0.5f); }
};

class CSSLayout {
public:
    explicit CSSLayout(const CSSParser* parser = nullptr);

    void setParser(const CSSParser* parser) { m_parser = parser; }
    const CSSParser* parser() const { return m_parser; }

    // Resolve posicao/tamanho de um elemento pelo seletor.
    CSSRect resolve(const std::string& selector, const ImVec2& displaySize,
                    const CSSRect& parent = {}, bool* found = nullptr) const;

    // Helpers de leitura de propriedades.
    bool displayVisible(const std::string& selector) const;
    ImU32 backgroundColor(const std::string& selector, ImU32 fallback = 0) const;
    ImU32 borderColor(const std::string& selector, ImU32 fallback = 0) const;
    ImU32 textColor(const std::string& selector, ImU32 fallback = IM_COL32(255,255,255,255)) const;
    float borderWidth(const std::string& selector, float fallback = 1.0f) const;
    float borderRadius(const std::string& selector, float fallback = 0.0f) const;
    float opacity(const std::string& selector, float fallback = 1.0f) const;
    int zIndex(const std::string& selector, int fallback = 0) const;

    // DESENHO dos widgets de um .css. Fica aqui, e nao em quem desenha, pra
    // existir UMA implementacao: a HUD do jogador (game/HudRenderer) e a
    // interface do jogo (renderer/UiDocument) pintam igual, e quem mexer na
    // borda arredondada conserta nos dois.
    //
    // `fallback*` e' o que vale quando a regra nao diz nada; `alphaMul`
    // multiplica a opacidade de tudo (fade de painel).
    void drawBox(ImDrawList* dl, const CSSRect& r, const std::string& selector, float alphaMul = 1.0f,
                 ImU32 fallbackBg = 0, ImU32 fallbackBorder = 0, float fallbackBorderWidth = 0.0f,
                 float fallbackRadius = 0.0f, ImU32 bgOverride = 0, ImU32 borderOverride = 0) const;
    // Barra: moldura + preenchimento proporcional (degrade de `fillLow` a
    // `fillHigh`) + texto opcional no centro.
    void drawBar(ImDrawList* dl, const CSSRect& r, const std::string& selector, float ratio, ImU32 fillLow,
                 ImU32 fillHigh, const char* valueText = nullptr, float textSize = 0.0f, float alphaMul = 1.0f,
                 ImU32 fallbackBg = 0, ImU32 fallbackBorder = 0, float fallbackBorderWidth = 0.0f,
                 float fallbackRadius = 0.0f) const;
    // Texto com sombra de 1 px. `align`: 0 esquerda, 1 centro, 2 direita.
    // `size` em pixels; 0 usa a fonte atual do ImGui.
    static void drawLabel(ImDrawList* dl, float x, float y, float size, ImU32 color, const char* text,
                          uint8_t align = 0);

    // Leitura crua de propriedade customizada (ex: clip-path).
    std::string rawProperty(const std::string& selector, const std::string& property, const std::string& fallback = {}) const;

    static ImU32 parseColor(const std::string& value, ImU32 fallback = 0);
    static float parseFloat(const std::string& value, float fallback = 0.0f);
    static bool parseLength(const std::string& value, float base, float& out);

private:
    const CSSRule* rule(const std::string& selector) const;
    const CSSParser* m_parser = nullptr;
};

} // namespace eruption
