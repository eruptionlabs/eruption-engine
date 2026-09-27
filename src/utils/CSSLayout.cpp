#include "utils/CSSLayout.hpp"
#include <cctype>
#include <cmath>
#include <algorithm>

namespace eruption {

CSSLayout::CSSLayout(const CSSParser* parser) : m_parser(parser) {}

const CSSRule* CSSLayout::rule(const std::string& selector) const {
    if (!m_parser) return nullptr;
    static CSSRule merged;
    merged = m_parser->mergedRule(selector);
    if (merged.properties.empty()) return nullptr;
    return &merged;
}

ImU32 CSSLayout::parseColor(const std::string& value, ImU32 fallback) {
    if (value.empty() || value[0] != '#') return fallback;

    std::string hex = value.substr(1);
    // Remove caracteres invalidos
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return fallback;
    }

    if (hex.size() == 3) { // #rgb
        int r = std::stoi(hex.substr(0, 1), nullptr, 16) * 17;
        int g = std::stoi(hex.substr(1, 1), nullptr, 16) * 17;
        int b = std::stoi(hex.substr(2, 1), nullptr, 16) * 17;
        return IM_COL32(r, g, b, 255);
    }
    if (hex.size() == 4) { // #rgba
        int r = std::stoi(hex.substr(0, 1), nullptr, 16) * 17;
        int g = std::stoi(hex.substr(1, 1), nullptr, 16) * 17;
        int b = std::stoi(hex.substr(2, 1), nullptr, 16) * 17;
        int a = std::stoi(hex.substr(3, 1), nullptr, 16) * 17;
        return IM_COL32(r, g, b, a);
    }
    if (hex.size() == 6) { // #rrggbb
        int r = std::stoi(hex.substr(0, 2), nullptr, 16);
        int g = std::stoi(hex.substr(2, 2), nullptr, 16);
        int b = std::stoi(hex.substr(4, 2), nullptr, 16);
        return IM_COL32(r, g, b, 255);
    }
    if (hex.size() == 8) { // #rrggbbaa
        int r = std::stoi(hex.substr(0, 2), nullptr, 16);
        int g = std::stoi(hex.substr(2, 2), nullptr, 16);
        int b = std::stoi(hex.substr(4, 2), nullptr, 16);
        int a = std::stoi(hex.substr(6, 2), nullptr, 16);
        return IM_COL32(r, g, b, a);
    }
    return fallback;
}

float CSSLayout::parseFloat(const std::string& value, float fallback) {
    if (value.empty()) return fallback;
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

bool CSSLayout::parseLength(const std::string& value, float base, float& out) {
    if (value.empty()) return false;
    if (value == "auto") { out = -1.0f; return true; }

    size_t idx = 0;
    while (idx < value.size() && (std::isdigit(static_cast<unsigned char>(value[idx])) || value[idx] == '.' || value[idx] == '-')) ++idx;

    std::string numStr = value.substr(0, idx);
    std::string unit = CSSParser::trim(value.substr(idx));

    float num = 0.0f;
    try { num = std::stof(numStr); } catch (...) { return false; }

    if (unit == "%") {
        out = num * base / 100.0f;
    } else {
        // px, ou sem unidade -> pixels
        out = num;
    }
    return true;
}

CSSRect CSSLayout::resolve(const std::string& selector, const ImVec2& displaySize,
                           const CSSRect& parent, bool* found) const {
    CSSRect out;
    const CSSRule* r = rule(selector);
    if (found) *found = (r != nullptr);
    if (!r) return out;

    auto get = [&](const std::string& prop) -> const std::string* {
        auto it = r->properties.find(prop);
        return it != r->properties.end() ? &it->second : nullptr;
    };

    float baseW = (parent.w > 0.0f) ? parent.w : displaySize.x;
    float baseH = (parent.h > 0.0f) ? parent.h : displaySize.y;

    // Width
    if (auto v = get("width")) {
        if (*v == "auto") out.w = -1.0f;
        else parseLength(*v, baseW, out.w);
    }
    // Height
    if (auto v = get("height")) {
        if (*v == "auto") out.h = -1.0f;
        else parseLength(*v, baseH, out.h);
    }

    // Posicionamento horizontal
    bool hasLeft = false, hasRight = false;
    float left = 0.0f, right = 0.0f;
    if (auto v = get("left")) { hasLeft = parseLength(*v, baseW, left); }
    if (auto v = get("right")) { hasRight = parseLength(*v, baseW, right); }

    if (hasLeft && hasRight && out.w >= 0.0f) {
        out.x = left;
        out.w = baseW - left - right;
    } else if (hasLeft) {
        out.x = left;
    } else if (hasRight) {
        if (out.w >= 0.0f) out.x = baseW - right - out.w;
        else out.x = baseW - right;
    }

    // Centraliza com translateX(-50%)
    auto transform = get("transform");
    bool centerX = transform && transform->find("translateX(-50%)") != std::string::npos;
    if (centerX && out.w >= 0.0f) {
        out.x -= out.w * 0.5f;
    }

    // Posicionamento vertical
    bool hasTop = false, hasBottom = false;
    float top = 0.0f, bottom = 0.0f;
    if (auto v = get("top")) { hasTop = parseLength(*v, baseH, top); }
    if (auto v = get("bottom")) { hasBottom = parseLength(*v, baseH, bottom); }

    if (hasTop && hasBottom && out.h >= 0.0f) {
        out.y = top;
        out.h = baseH - top - bottom;
    } else if (hasTop) {
        out.y = top;
    } else if (hasBottom) {
        if (out.h >= 0.0f) out.y = baseH - bottom - out.h;
        else out.y = baseH - bottom;
    }

    // Converte coordenadas relativas ao parent em coordenadas absolutas da tela
    out.x += parent.x;
    out.y += parent.y;

    return out;
}

bool CSSLayout::displayVisible(const std::string& selector) const {
    const CSSRule* r = rule(selector);
    if (!r) return true;
    auto it = r->properties.find("display");
    if (it == r->properties.end()) return true;
    return it->second != "none";
}

ImU32 CSSLayout::backgroundColor(const std::string& selector, ImU32 fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find("background-color");
    if (it == r->properties.end()) return fallback;
    return parseColor(it->second, fallback);
}

ImU32 CSSLayout::borderColor(const std::string& selector, ImU32 fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find("border-color");
    if (it == r->properties.end()) return fallback;
    return parseColor(it->second, fallback);
}

ImU32 CSSLayout::textColor(const std::string& selector, ImU32 fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find("color");
    if (it == r->properties.end()) return fallback;
    return parseColor(it->second, fallback);
}

float CSSLayout::borderWidth(const std::string& selector, float fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find("border-width");
    if (it == r->properties.end()) return fallback;
    return parseFloat(it->second, fallback);
}

float CSSLayout::borderRadius(const std::string& selector, float fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find("border-radius");
    if (it == r->properties.end()) return fallback;
    return parseFloat(it->second, fallback);
}

float CSSLayout::opacity(const std::string& selector, float fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find("opacity");
    if (it == r->properties.end()) return fallback;
    return std::clamp(parseFloat(it->second, fallback), 0.0f, 1.0f);
}

int CSSLayout::zIndex(const std::string& selector, int fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find("z-index");
    if (it == r->properties.end()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}

std::string CSSLayout::rawProperty(const std::string& selector, const std::string& property, const std::string& fallback) const {
    const CSSRule* r = rule(selector);
    if (!r) return fallback;
    auto it = r->properties.find(property);
    if (it == r->properties.end()) return fallback;
    return it->second;
}

} // namespace eruption
