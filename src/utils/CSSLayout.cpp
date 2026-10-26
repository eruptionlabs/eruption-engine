#include "utils/CSSLayout.hpp"
#include <imgui_internal.h> // TexUvWhitePixel, para os triangulos do poligono concavo
#include <cctype>
#include <cmath>
#include <algorithm>
#include <vector>

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

namespace {

// Expressao de comprimento de calc()/min()/max()/clamp(): + - * /, parenteses
// e termos com unidade, tudo resolvido em px na hora (% sobre `base`, v* sobre
// a tela). Ex.: calc(68px * min(100vh / 1080, 100vw / 2400)) escala igual em
// 16:9 e 21:9.
class LengthExpr {
public:
    LengthExpr(const std::string& s, float base, const ImVec2* vp) : m_s(s), m_base(base), m_vp(vp) {}

    bool eval(float& out) {
        if (!sum(out)) return false;
        skip();
        return m_i == m_s.size();
    }

private:
    void skip() {
        while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i]))) ++m_i;
    }
    bool eat(char c) {
        skip();
        if (m_i < m_s.size() && m_s[m_i] == c) { ++m_i; return true; }
        return false;
    }
    bool sum(float& v) {
        if (!product(v)) return false;
        for (;;) {
            float r = 0.0f;
            if (eat('+')) { if (!product(r)) return false; v += r; }
            else if (eat('-')) { if (!product(r)) return false; v -= r; }
            else return true;
        }
    }
    bool product(float& v) {
        if (!unary(v)) return false;
        for (;;) {
            float r = 0.0f;
            if (eat('*')) { if (!unary(r)) return false; v *= r; }
            else if (eat('/')) { if (!unary(r) || r == 0.0f) return false; v /= r; }
            else return true;
        }
    }
    bool unary(float& v) {
        if (eat('-')) { if (!unary(v)) return false; v = -v; return true; }
        eat('+');
        return primary(v);
    }
    // Lista de argumentos ja' depois do "(": guarda cada um em `args`.
    bool argList(std::vector<float>& args) {
        do {
            float v = 0.0f;
            if (!sum(v)) return false;
            args.push_back(v);
        } while (eat(','));
        return eat(')');
    }
    bool primary(float& v) {
        skip();
        if (eat('(')) return sum(v) && eat(')');
        size_t j = m_i;
        while (j < m_s.size() && std::isalpha(static_cast<unsigned char>(m_s[j]))) ++j;
        if (j > m_i) {
            const std::string fn = m_s.substr(m_i, j - m_i);
            m_i = j;
            if (!eat('(')) return false;
            std::vector<float> args;
            if (!argList(args)) return false;
            if (fn == "calc" && args.size() == 1) v = args[0];
            else if (fn == "min") v = *std::min_element(args.begin(), args.end());
            else if (fn == "max") v = *std::max_element(args.begin(), args.end());
            else if (fn == "clamp" && args.size() == 3) v = std::max(args[0], std::min(args[1], args[2]));
            else return false;
            return true;
        }
        j = m_i;
        while (j < m_s.size() && (std::isdigit(static_cast<unsigned char>(m_s[j])) || m_s[j] == '.')) ++j;
        if (j == m_i) return false;
        try { v = std::stof(m_s.substr(m_i, j - m_i)); } catch (...) { return false; }
        m_i = j;
        while (j < m_s.size() && (std::isalpha(static_cast<unsigned char>(m_s[j])) || m_s[j] == '%')) ++j;
        const std::string unit = m_s.substr(m_i, j - m_i);
        m_i = j;
        const float vw = m_vp ? m_vp->x / 100.0f : 1.0f, vh = m_vp ? m_vp->y / 100.0f : 1.0f;
        if (unit == "%") v *= m_base / 100.0f;
        else if (unit == "vw") v *= vw;
        else if (unit == "vh") v *= vh;
        else if (unit == "vmin") v *= std::min(vw, vh);
        else if (unit == "vmax") v *= std::max(vw, vh);
        else if (!unit.empty() && unit != "px") return false;
        return true;
    }

    const std::string& m_s;
    size_t m_i = 0;
    float m_base;
    const ImVec2* m_vp;
};

} // namespace

bool CSSLayout::parseLength(const std::string& value, float base, float& out, const ImVec2* viewport) {
    if (value.empty()) return false;
    if (value == "auto") { out = -1.0f; return true; }
    if (value.find('(') != std::string::npos) {
        float v = 0.0f;
        if (!LengthExpr(value, base, viewport).eval(v)) return false;
        out = v;
        return true;
    }

    size_t idx = 0;
    while (idx < value.size() && (std::isdigit(static_cast<unsigned char>(value[idx])) || value[idx] == '.' || value[idx] == '-')) ++idx;

    std::string numStr = value.substr(0, idx);
    std::string unit = CSSParser::trim(value.substr(idx));

    float num = 0.0f;
    try { num = std::stof(numStr); } catch (...) { return false; }

    if (unit == "%") {
        out = num * base / 100.0f;
    } else if (viewport && unit.size() >= 2 && unit[0] == 'v') {
        const float vw = viewport->x / 100.0f, vh = viewport->y / 100.0f;
        if (unit == "vw") out = num * vw;
        else if (unit == "vh") out = num * vh;
        else if (unit == "vmin") out = num * std::min(vw, vh);
        else if (unit == "vmax") out = num * std::max(vw, vh);
        else out = num;
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
    const ImVec2* vp = &displaySize;
    if (auto v = get("width")) {
        if (*v == "auto") out.w = -1.0f;
        else parseLength(*v, baseW, out.w, vp);
    }
    // Height
    if (auto v = get("height")) {
        if (*v == "auto") out.h = -1.0f;
        else parseLength(*v, baseH, out.h, vp);
    }

    // Posicionamento horizontal
    bool hasLeft = false, hasRight = false;
    float left = 0.0f, right = 0.0f;
    if (auto v = get("left")) { hasLeft = parseLength(*v, baseW, left, vp); }
    if (auto v = get("right")) { hasRight = parseLength(*v, baseW, right, vp); }

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
    if (auto v = get("top")) { hasTop = parseLength(*v, baseH, top, vp); }
    if (auto v = get("bottom")) { hasBottom = parseLength(*v, baseH, bottom, vp); }

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

namespace {

ImU32 fadeAlpha(ImU32 c, float mul) {
    if (mul >= 0.999f) return c;
    const int a = int(((c >> IM_COL32_A_SHIFT) & 0xFF) * (mul < 0.0f ? 0.0f : mul));
    return (c & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
}

bool opaque(ImU32 c) { return ((c >> IM_COL32_A_SHIFT) & 0xFF) != 0; }

} // namespace

void CSSLayout::drawBox(ImDrawList* dl, const CSSRect& r, const std::string& selector, float alphaMul,
                        ImU32 fallbackBg, ImU32 fallbackBorder, float fallbackBorderWidth, float fallbackRadius,
                        ImU32 bgOverride, ImU32 borderOverride) const {
    if (!dl) return;
    const float radius = borderRadius(selector, fallbackRadius);
    const ImU32 bg = bgOverride ? bgOverride : backgroundColor(selector, fallbackBg);
    if (opaque(bg)) dl->AddRectFilled(r.min(), r.max(), fadeAlpha(bg, alphaMul), radius);
    const float bw = borderWidth(selector, fallbackBorderWidth);
    const ImU32 bc = borderOverride ? borderOverride : borderColor(selector, fallbackBorder);
    if (bw > 0.0f && opaque(bc)) dl->AddRect(r.min(), r.max(), fadeAlpha(bc, alphaMul), radius, 0, bw);
}

void CSSLayout::drawBar(ImDrawList* dl, const CSSRect& r, const std::string& selector, float ratio, ImU32 fillLow,
                        ImU32 fillHigh, const char* valueText, float textSize, float alphaMul, ImU32 fallbackBg,
                        ImU32 fallbackBorder, float fallbackBorderWidth, float fallbackRadius) const {
    if (!dl) return;
    ratio = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
    drawBox(dl, r, selector, alphaMul, fallbackBg, fallbackBorder, fallbackBorderWidth, fallbackRadius);
    const float bw = borderWidth(selector, fallbackBorderWidth);
    const ImVec2 fillMin(r.x + bw, r.y + bw);
    const ImVec2 fillMax(r.x + bw + (r.w - 2.0f * bw) * ratio, r.y + r.h - bw);
    if (fillMax.x > fillMin.x)
        dl->AddRectFilledMultiColor(fillMin, fillMax, fadeAlpha(fillLow, alphaMul), fadeAlpha(fillHigh, alphaMul),
                                    fadeAlpha(fillHigh, alphaMul), fadeAlpha(fillLow, alphaMul));
    if (valueText && *valueText) {
        const ImU32 tc = fadeAlpha(textColor(selector, IM_COL32(240, 240, 240, 255)), alphaMul);
        if (textSize > 0.0f) {
            drawLabel(dl, r.center().x, r.y + (r.h - textSize) * 0.5f, textSize, tc, valueText, 1);
        } else {
            const ImVec2 ts = ImGui::CalcTextSize(valueText);
            drawLabel(dl, r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - ts.y) * 0.5f, 0.0f, tc, valueText, 0);
        }
    }
}

bool CSSLayout::parsePolygon(const std::string& value, std::vector<PolyPoint>& out) {
    out.clear();
    const size_t open = value.find("polygon(");
    const size_t close = value.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    // Um termo "N%", "Npx" (sem unidade = px) ou N vw/vh/vmin/vmax, somado
    // com o sinal dado.
    auto term = [](const std::string& t, float sign, float& frac, float& px, float* v) {
        static const char* kUnits[4] = {"vmin", "vmax", "vw", "vh"};
        static const int kSlot[4] = {2, 3, 0, 1};
        for (int u = 0; u < 4; ++u) {
            const size_t n = std::char_traits<char>::length(kUnits[u]);
            if (t.size() > n && t.compare(t.size() - n, n, kUnits[u]) == 0) {
                v[kSlot[u]] += sign * parseFloat(t.substr(0, t.size() - n)) / 100.0f;
                return;
            }
        }
        if (!t.empty() && t.back() == '%') frac += sign * parseFloat(t.substr(0, t.size() - 1)) / 100.0f;
        else if (t.size() > 2 && t.compare(t.size() - 2, 2, "px") == 0) px += sign * parseFloat(t.substr(0, t.size() - 2));
        else px += sign * parseFloat(t, 0.0f);
    };
    const std::string body = value.substr(open + 8, close - open - 8);
    // Pares separados por virgula fora de parenteses: min(a, b) fica inteiro.
    std::vector<std::string> pairs(1);
    int depth = 0;
    for (char ch : body) {
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        if (ch == ',' && depth == 0) pairs.emplace_back();
        else pairs.back() += ch;
    }
    for (const std::string& pair : pairs) {
        // Palavras fora de parenteses: "calc(50% - 40px)" e' uma so'.
        std::vector<std::string> tk;
        depth = 0;
        bool fresh = true;
        for (char ch : pair) {
            if (depth == 0 && std::isspace(static_cast<unsigned char>(ch))) { fresh = true; continue; }
            if (fresh) { tk.emplace_back(); fresh = false; }
            if (ch == '(') ++depth;
            else if (ch == ')') --depth;
            tk.back() += ch;
        }
        // Cada coordenada e' uma soma de termos ("100% - 20px"); com funcao,
        // guarda a expressao para o desenho.
        size_t k = 0;
        auto coord = [&](float& frac, float& px, float* v, std::string& ex) {
            frac = px = 0.0f;
            if (k >= tk.size()) return false;
            const size_t first = k++;
            while (k + 1 < tk.size() && (tk[k] == "+" || tk[k] == "-")) k += 2;
            bool fn = false;
            for (size_t i = first; i < k; ++i) fn |= tk[i].find('(') != std::string::npos;
            if (fn) {
                for (size_t i = first; i < k; ++i) ex += (i > first ? " " : "") + tk[i];
                return true;
            }
            term(tk[first], 1.0f, frac, px, v);
            for (size_t i = first + 1; i < k; i += 2) term(tk[i + 1], tk[i] == "-" ? -1.0f : 1.0f, frac, px, v);
            return true;
        };
        PolyPoint p;
        if (coord(p.fx, p.px, p.vx, p.ex) && coord(p.fy, p.py, p.vy, p.ey)) out.push_back(p);
    }
    if (out.size() < 3) out.clear();
    return !out.empty();
}

ImVec2 CSSLayout::PolyPoint::at(const CSSRect& r, float vw, float vh) const {
    const float u[4] = {vw, vh, std::min(vw, vh), std::max(vw, vh)};
    float x = r.x + fx * r.w + px, y = r.y + fy * r.h + py;
    for (int k = 0; k < 4; ++k) {
        x += vx[k] * u[k];
        y += vy[k] * u[k];
    }
    const ImVec2 vp(vw, vh);
    float e = 0.0f;
    if (!ex.empty() && parseLength(ex, r.w, e, &vp)) x = r.x + e;
    if (!ey.empty() && parseLength(ey, r.h, e, &vp)) y = r.y + e;
    return ImVec2(x, y);
}

void CSSLayout::parseCornerRadii(const std::string& value, float radii[4], uint8_t& pctMask) {
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool pct[4] = {false, false, false, false};
    int n = 0;
    size_t i = 0;
    while (n < 4 && i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        size_t j = i;
        while (j < value.size() && !std::isspace(static_cast<unsigned char>(value[j]))) ++j;
        if (j > i) {
            const std::string t = value.substr(i, j - i);
            pct[n] = t.back() == '%';
            v[n++] = parseFloat(t, 0.0f);
        }
        i = j;
    }
    // Atalhos do CSS: 1 valor = todos; 2 = (sup-esq/inf-dir, sup-dir/inf-esq);
    // 3 = (sup-esq, sup-dir/inf-esq, inf-dir).
    static const int kMap[5][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    pctMask = 0;
    for (int k = 0; k < 4; ++k) {
        radii[k] = v[kMap[n][k]];
        if (pct[kMap[n][k]]) pctMask |= uint8_t(1u << k);
    }
}

void CSSLayout::roundedRectPoints(const CSSRect& r, const float radii[4], std::vector<ImVec2>& out) {
    out.clear();
    const float maxR = 0.5f * std::min(r.w, r.h);
    // Cantos em sentido horario a partir do sup-esq; angulo inicial de cada arco.
    const ImVec2 corner[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
    const ImVec2 inward[4] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
    const float a0[4] = {3.14159265f, 4.71238898f, 0.0f, 1.57079633f};
    for (int k = 0; k < 4; ++k) {
        const float rad = std::clamp(radii[k], 0.0f, maxR);
        if (rad < 0.5f) { out.push_back(corner[k]); continue; }
        const ImVec2 c(corner[k].x + inward[k].x * rad, corner[k].y + inward[k].y * rad);
        const int segs = std::clamp(static_cast<int>(rad * 0.5f), 3, 16);
        for (int s = 0; s <= segs; ++s) {
            const float a = a0[k] + 1.57079633f * float(s) / float(segs);
            out.push_back(ImVec2(c.x + std::cos(a) * rad, c.y + std::sin(a) * rad));
        }
    }
}

namespace {

float cross2(const ImVec2& o, const ImVec2& a, const ImVec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

} // namespace

void CSSLayout::fillPolygon(ImDrawList* dl, const ImVec2* pts, int count, ImU32 color) {
    if (!dl || count < 3 || !opaque(color)) return;
    float area = 0.0f;
    bool pos = false, neg = false;
    for (int i = 0; i < count; ++i) {
        const ImVec2& a = pts[i];
        const ImVec2& b = pts[(i + 1) % count];
        area += a.x * b.y - b.x * a.y;
        const float c = cross2(a, b, pts[(i + 2) % count]);
        pos |= c > 1e-4f;
        neg |= c < -1e-4f;
    }
    if (!(pos && neg)) { dl->AddConvexPolyFilled(pts, count, color); return; }
    // Concavo: recorte de orelhas, O(n^2) nos poucos vertices de um clip-path.
    const float sign = area > 0.0f ? 1.0f : -1.0f;
    std::vector<int> idx(count);
    for (int i = 0; i < count; ++i) idx[i] = i;
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    int guard = count * count;
    while (idx.size() > 2 && guard-- > 0) {
        const int n = static_cast<int>(idx.size());
        bool cut = false;
        for (int i = 0; i < n && !cut; ++i) {
            const ImVec2& a = pts[idx[(i + n - 1) % n]];
            const ImVec2& b = pts[idx[i]];
            const ImVec2& c = pts[idx[(i + 1) % n]];
            if (cross2(a, b, c) * sign <= 0.0f) continue; // vertice reflexo
            bool inside = false;
            for (int k = 0; k < n && !inside; ++k) {
                const int q = idx[k];
                if (q == idx[(i + n - 1) % n] || q == idx[i] || q == idx[(i + 1) % n]) continue;
                const ImVec2& p = pts[q];
                inside = cross2(a, b, p) * sign >= 0.0f && cross2(b, c, p) * sign >= 0.0f &&
                         cross2(c, a, p) * sign >= 0.0f;
            }
            if (inside) continue;
            dl->PrimReserve(3, 3);
            dl->PrimVtx(a, uv, color);
            dl->PrimVtx(b, uv, color);
            dl->PrimVtx(c, uv, color);
            idx.erase(idx.begin() + i);
            cut = true;
        }
        if (!cut) break; // poligono que se cruza: o que sobrou fica sem pintar
    }
}

void CSSLayout::drawGlow(ImDrawList* dl, const CSSRect& r, float rounding, ImU32 color, float radius) {
    if (!dl || radius <= 0.0f || !opaque(color)) return;
    // Camadas de 2 px, cada uma com uma fatia da opacidade que cai com o
    // quadrado da distancia: as de dentro somam, a borda some suave.
    const int layers = std::clamp(static_cast<int>(radius * 0.5f), 2, 10);
    const float a = float((color >> IM_COL32_A_SHIFT) & 0xFF);
    for (int i = layers; i >= 1; --i) {
        const float t = float(i) / float(layers);
        const float grow = radius * t;
        const float fall = (1.0f - t) + 1.0f / float(layers);
        const int la = static_cast<int>(a * fall * fall * 2.0f / float(layers));
        if (la <= 0) continue;
        const ImU32 c = (color & ~IM_COL32_A_MASK) | (ImU32(std::min(la, 255)) << IM_COL32_A_SHIFT);
        dl->AddRectFilled(ImVec2(r.x - grow, r.y - grow), ImVec2(r.x + r.w + grow, r.y + r.h + grow), c,
                          rounding + grow);
    }
}

void CSSLayout::rotateVertices(ImDrawList* dl, int vtxStart, const ImVec2& center, float degrees) {
    if (!dl || degrees == 0.0f) return;
    const float a = degrees * 0.01745329f, cs = std::cos(a), sn = std::sin(a);
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        const float dx = p.x - center.x, dy = p.y - center.y;
        // y para baixo: angulo positivo gira no sentido horario, como no CSS.
        p = ImVec2(center.x + dx * cs - dy * sn, center.y + dx * sn + dy * cs);
    }
}

void CSSLayout::drawLabel(ImDrawList* dl, float x, float y, float size, ImU32 color, const char* text,
                          uint8_t align) {
    if (!dl || !text || !*text) return;
    ImFont* font = ImGui::GetFont();
    const float px = size > 0.0f ? size : ImGui::GetFontSize();
    const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
    const float left = align == 0 ? x : (align == 1 ? x - w * 0.5f : x - w);
    dl->AddText(font, px, ImVec2(left + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(font, px, ImVec2(left, y), color, text);
}

} // namespace eruption
