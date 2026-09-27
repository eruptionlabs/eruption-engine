#pragma once
#include "utils/CSSParser.hpp"
#include <imgui.h>
#include <cstdint>

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
