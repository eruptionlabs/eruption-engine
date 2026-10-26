#include "renderer/UiDocument.hpp"
#include <unordered_set>

#include "core/Logger.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cmath>

namespace eruption {

namespace {

std::string withHash(const std::string& s) {
    return (!s.empty() && s[0] == '#') ? s : ("#" + s);
}

ImU32 toImU32(const Vec4& c) {
    return IM_COL32(int(std::clamp(c.r, 0.0f, 1.0f) * 255.0f), int(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
                    int(std::clamp(c.b, 0.0f, 1.0f) * 255.0f), int(std::clamp(c.a, 0.0f, 1.0f) * 255.0f));
}

ImU32 withAlpha(ImU32 c, float mul) {
    const int a = int(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(mul, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
}

} // namespace

bool UiDocument::load(const std::filesystem::path& cssPath, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(cssPath, ec)) {
        if (error) *error = "interface file not found: " + cssPath.generic_string();
        return false;
    }
    if (!m_parser.load(cssPath.string())) {
        if (error) *error = "could not read " + cssPath.generic_string();
        return false;
    }
    m_path = cssPath;
    m_mtime = std::filesystem::last_write_time(cssPath, ec);
    m_loaded = true;
    build();
    ERUPTION_LOG_WARN("[UI] %s: %zu elementos", cssPath.filename().string().c_str(), m_elements.size());
    return true;
}

void UiDocument::reloadIfChanged() {
    if (!m_loaded) return;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(m_path, ec);
    if (ec || t == m_mtime) return;
    m_mtime = t;
    // Guarda o que o jogo preencheu: recarregar o ARQUIVO não pode apagar o
    // valor vivo (a barra de vida não some porque alguém salvou o .css).
    std::vector<Element> old = std::move(m_elements);
    if (!m_parser.load(m_path.string())) return;
    build();
    for (const Element& o : old) {
        Element* e = find(o.selector);
        if (!e) continue;
        e->hasText = o.hasText; e->text = o.text;
        e->hasValue = o.hasValue; e->value = o.value;
        e->hasVisible = o.hasVisible; e->visible = o.visible;
        for (int i = 0; i < 2; ++i) {
            e->hasColor[i] = o.hasColor[i];
            e->color[i] = o.color[i];
        }
    }
    ERUPTION_LOG_WARN("[UI] %s recarregado", m_path.filename().string().c_str());
}

void UiDocument::clear() {
    m_elements.clear();
    m_byId.clear();
    m_loaded = false;
    m_path.clear();
}

void UiDocument::build() {
    m_elements.clear();
    m_byId.clear();
    // `m_byId` só é preenchido no fim (depois da ordenação por z), então ele
    // não serve para saber quem já saiu daqui: o id repetido tem de ser
    // marcado AGORA. Duas regras com o mesmo id são cascata, não dois
    // elementos — e desde que `--class` existe o id repetido é a regra, não a
    // exceção, porque a classe entra como mais uma regra do mesmo id.
    std::unordered_set<std::string> criados;
    for (const CSSRule& r : m_parser.rules()) {
        if (r.selector.empty() || r.selector[0] != '#') continue;
        if (!criados.insert(r.selector).second) continue; // a cascata já foi resolvida na leitura
        const CSSRule merged = m_parser.mergedRule(r.selector);
        auto prop = [&](const char* name) -> const std::string* {
            auto it = merged.properties.find(name);
            return it != merged.properties.end() ? &it->second : nullptr;
        };

        Element e;
        e.selector = r.selector;
        if (auto v = prop("--parent")) e.parent = withHash(CSSParser::trim(*v));
        if (auto v = prop("--widget")) {
            const std::string w = CSSParser::trim(*v);
            if (w == "text") e.widget = Widget::Text;
            else if (w == "bar") e.widget = Widget::Bar;
            else if (w == "image") e.widget = Widget::Image;
            else e.widget = Widget::Rect;
        } else if (prop("content")) {
            e.widget = Widget::Text; // tem texto e não disse o tipo: é texto
        }
        if (auto v = prop("content")) {
            std::string t = CSSParser::trim(*v);
            if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = t.substr(1, t.size() - 2);
            e.textCss = t;
        }
        if (auto v = prop("--image")) {
            std::string t = CSSParser::trim(*v);
            if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = t.substr(1, t.size() - 2);
            e.image = t;
        }
        if (auto v = prop("--image-tint")) e.tintCss = CSSLayout::parseColor(*v, IM_COL32_WHITE);
        if (auto v = prop("--bar-shape")) e.arc = CSSParser::trim(*v) == "arc";
        if (auto v = prop("--arc-start")) e.arcStart = CSSLayout::parseFloat(*v, e.arcStart);
        if (auto v = prop("--arc-end")) e.arcEnd = CSSLayout::parseFloat(*v, e.arcEnd);
        if (auto v = prop("--arc-thickness")) e.arcThickness = CSSLayout::parseFloat(*v, 15.0f) / 100.0f;
        if (auto v = prop("--value")) e.valueCss = CSSLayout::parseFloat(*v, 1.0f);
        if (auto v = prop("text-align")) {
            const std::string a = CSSParser::trim(*v);
            e.align = a == "center" ? 1 : (a == "right" ? 2 : 0);
        }
        e.z = m_layout.zIndex(r.selector, 0);
        e.visibleCss = m_layout.displayVisible(r.selector);
        e.text = e.textCss;
        e.value = e.valueCss;
        e.visible = e.visibleCss;
        m_elements.push_back(std::move(e));
    }

    // Ordem de desenho: z-index, empate mantém a ordem do arquivo.
    std::stable_sort(m_elements.begin(), m_elements.end(),
                     [](const Element& a, const Element& b) { return a.z < b.z; });
    for (size_t i = 0; i < m_elements.size(); ++i) m_byId[m_elements[i].selector] = i;
}

UiDocument::Element* UiDocument::find(const std::string& selector) {
    auto it = m_byId.find(withHash(selector));
    return it == m_byId.end() ? nullptr : &m_elements[it->second];
}

const UiDocument::Element* UiDocument::find(const std::string& selector) const {
    auto it = m_byId.find(withHash(selector));
    return it == m_byId.end() ? nullptr : &m_elements[it->second];
}

void UiDocument::setText(const std::string& selector, const std::string& text) {
    if (Element* e = find(selector)) {
        e->hasText = true;
        e->text = text;
    }
}

void UiDocument::setImage(const std::string& selector, const std::string& png) {
    if (Element* e = find(selector)) e->image = png;
}

void UiDocument::setValue(const std::string& selector, float value01) {
    if (Element* e = find(selector)) {
        e->hasValue = true;
        e->value = std::clamp(value01, 0.0f, 1.0f);
    }
}

void UiDocument::setVisible(const std::string& selector, bool visible) {
    if (Element* e = find(selector)) {
        e->hasVisible = true;
        e->visible = visible;
    }
}

void UiDocument::setColor(const std::string& selector, int which, const Vec4& color) {
    Element* e = find(selector);
    if (!e || which < 0 || which > 1) return;
    e->hasColor[which] = true;
    e->color[which] = color;
}

void UiDocument::setPlace(const std::string& selector, float leftPct, float topPct, float widthPct, float heightPct) {
    if (Element* e = find(selector)) {
        e->hasPlace = true;
        e->place = Vec4(leftPct, topPct, widthPct, heightPct) * 0.01f;
    }
}

void UiDocument::clearPlace(const std::string& selector) {
    if (Element* e = find(selector)) e->hasPlace = false;
}

void UiDocument::setAnchor(const std::string& selector, float x, float y, bool onScreen) {
    if (Element* e = find(selector)) {
        e->hasAnchor = true;
        e->anchorOn = onScreen;
        e->anchor = Vec2(x, y);
    }
}

void UiDocument::clearAnchor(const std::string& selector) {
    if (Element* e = find(selector)) e->hasAnchor = false;
}

bool UiDocument::anchorHidden(const Element& e) const {
    // A mesma cadeia curta de resolveRect.
    const Element* p = &e;
    for (int n = 0; p && n < 5; ++n) {
        if (p->hasAnchor && !p->anchorOn) return true;
        if (p->parent.empty() || p->parent == p->selector) break;
        p = find(p->parent);
    }
    return false;
}

void UiDocument::setImageRect(const std::string& selector, float u0, float v0, float u1, float v1) {
    if (Element* e = find(selector)) e->uv = Vec4(u0, v0, u1, v1);
}

void UiDocument::clearImageRect(const std::string& selector) {
    if (Element* e = find(selector)) e->uv = Vec4(0.0f, 0.0f, 1.0f, 1.0f);
}

void UiDocument::resetOverrides() {
    for (Element& e : m_elements) {
        e.hasText = e.hasValue = e.hasVisible = false;
        e.hasColor[0] = e.hasColor[1] = false;
        e.hasPlace = false;
        e.hasAnchor = false;
        e.uv = Vec4(0.0f, 0.0f, 1.0f, 1.0f);
        e.text = e.textCss;
        e.value = e.valueCss;
        e.visible = e.visibleCss;
    }
}

CSSRect UiDocument::resolveRect(const Element& e, const Vec2& area) const {
    const ImVec2 display(area.x, area.y);
    CSSRect parent{};
    if (!e.parent.empty()) {
        // Cadeia de pais, curta de propósito: 4 níveis chegam em qualquer
        // interface sã, e para o ciclo de pai antes de travar o frame.
        const Element* chain[4] = {nullptr, nullptr, nullptr, nullptr};
        int n = 0;
        const Element* p = find(e.parent);
        while (p && n < 4) {
            chain[n++] = p;
            if (p->parent.empty() || p->parent == p->selector) break;
            p = find(p->parent);
        }
        for (int i = n - 1; i >= 0; --i) parent = ownRect(*chain[i], display, parent);
    }
    return ownRect(e, display, parent);
}

CSSRect UiDocument::ownRect(const Element& e, const ImVec2& display, const CSSRect& parent) const {
    CSSRect r = m_layout.resolve(e.selector, display, parent);
    if (e.hasAnchor) {
        // Ancora vence o resto: o tamanho fica o do CSS (ou o de ui.place).
        if (e.hasPlace) {
            if (e.place.z >= 0.0f) r.w = e.place.z * display.x;
            if (e.place.w >= 0.0f) r.h = e.place.w * display.y;
        }
        r.x = e.anchor.x - r.w * 0.5f;
        r.y = e.anchor.y - r.h * 0.5f;
        return r;
    }
    if (!e.hasPlace) return r;
    CSSRect base = parent;
    if (base.w <= 0.0f || base.h <= 0.0f) base = CSSRect{0.0f, 0.0f, display.x, display.y};
    r.x = base.x + e.place.x * base.w;
    r.y = base.y + e.place.y * base.h;
    if (e.place.z >= 0.0f) r.w = e.place.z * base.w;
    if (e.place.w >= 0.0f) r.h = e.place.w * base.h;
    return r;
}

bool UiDocument::rectOf(const std::string& selector, float x, float y, float w, float h, CSSRect& out) const {
    const Element* e = find(selector);
    if (!e) return false;
    out = resolveRect(*e, Vec2(w, h));
    // Texto sem caixa (so' posicao, como uma seta "»"): o retangulo vira o do
    // texto desenhado, senao ui.clicked so' acerta o ponto da ancora e o
    // toque no celular nunca pega.
    if (e->widget == Widget::Text && out.w <= 0.0f && out.h <= 0.0f && ImGui::GetCurrentContext()) {
        const std::string& txt = e->hasText ? e->text : e->textCss;
        float size = h * 0.03f;
        CSSLayout::parseLength(m_layout.rawProperty(e->selector, "font-size"), h, size);
        const float tw = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, txt.c_str()).x;
        out.x -= e->align == 0 ? 0.0f : (e->align == 1 ? tw * 0.5f : tw);
        out.w = tw;
        out.h = size;
    }
    out.x += x;
    out.y += y;
    return true;
}

// Barra em arco (--bar-shape: arc): o trilho e' o arco inteiro na cor de
// fundo, o preenchimento vai de --arc-start ate' a fracao do valor. Um traco
// grosso no raio do meio da faixa, com um segmento a cada ~2 px de arco: a
// area pintada segue a fracao do angulo (o arco de 140 graus a 50% mede 50%).
void UiDocument::drawArcBar(ImDrawList* dl, const CSSRect& r, const Element& e, float value, uint32_t rail, uint32_t fill) {
    const float side = std::min(r.w, r.h);
    const float thick = std::max(1.0f, side * std::clamp(e.arcThickness, 0.01f, 0.5f));
    const float radius = side * 0.5f - thick * 0.5f;
    if (radius <= 0.0f) return;
    const ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    // CSS: 0 grau em cima, sentido horario. ImGui: 0 rad a' direita, y para baixo.
    const float a0 = (e.arcStart - 90.0f) * 0.01745329f, a1 = (e.arcEnd - 90.0f) * 0.01745329f;
    auto stroke = [&](float from, float to, ImU32 col) {
        if (!(col >> IM_COL32_A_SHIFT) || std::abs(to - from) < 1e-4f) return;
        const int segs = std::max(8, static_cast<int>(std::abs(to - from) * radius / 2.0f));
        dl->PathArcTo(c, radius, from, to, segs);
        dl->PathStroke(col, ImDrawFlags_None, thick);
    };
    stroke(a0, a1, rail);
    stroke(a0, a0 + (a1 - a0) * std::clamp(value, 0.0f, 1.0f), fill);
}

void UiDocument::draw(ImDrawList* dl, float x, float y, float w, float h) const {
    if (!dl || !m_loaded || w <= 0.0f || h <= 0.0f) return;
    const Vec2 area(w, h);

    for (const Element& e : m_elements) {
        const bool visible = e.hasVisible ? e.visible : e.visibleCss;
        if (!visible || anchorHidden(e)) continue;

        CSSRect r = resolveRect(e, area);
        r.x += x;
        r.y += y;
        if (r.w <= 0.0f && e.widget != Widget::Text) continue;

        const float alpha = m_layout.opacity(e.selector, 1.0f);
        const ImU32 bg = e.hasColor[0] ? toImU32(e.color[0]) : m_layout.backgroundColor(e.selector, 0);
        const ImU32 edge = e.hasColor[1] ? toImU32(e.color[1]) : m_layout.borderColor(e.selector, 0);

        // A pintura e' a mesma da HUD do jogador: CSSLayout::drawBox/drawBar.
        switch (e.widget) {
        case Widget::Rect:
            m_layout.drawBox(dl, r, e.selector, alpha, 0, 0, 0.0f, 0.0f, bg, edge);
            break;

        case Widget::Bar: {
            // Preenchimento: --fill-color, ou a cor do texto como reserva.
            ImU32 fill = e.hasColor[1] ? toImU32(e.color[1])
                                       : CSSLayout::parseColor(m_layout.rawProperty(e.selector, "--fill-color"), 0);
            if (!fill) fill = m_layout.textColor(e.selector, IM_COL32(220, 220, 220, 255));
            if (e.arc) {
                drawArcBar(dl, r, e, e.hasValue ? e.value : e.valueCss, withAlpha(bg, alpha), withAlpha(fill, alpha));
                break;
            }
            const std::string& txt = e.hasText ? e.text : e.textCss;
            float size = r.h * 0.8f;
            CSSLayout::parseLength(m_layout.rawProperty(e.selector, "font-size"), h, size);
            m_layout.drawBar(dl, r, e.selector, e.hasValue ? e.value : e.valueCss, fill, fill,
                             txt.empty() ? nullptr : txt.c_str(), size, alpha);
            break;
        }

        case Widget::Text: {
            float size = h * 0.03f;
            CSSLayout::parseLength(m_layout.rawProperty(e.selector, "font-size"), h, size);
            const std::string& txt = e.hasText ? e.text : e.textCss;
            const float px = e.align == 0 ? r.x : (e.align == 1 ? r.center().x : r.x + r.w);
            CSSLayout::drawLabel(dl, px, r.y, size,
                                 withAlpha(e.hasColor[0] ? toImU32(e.color[0]) : m_layout.textColor(e.selector), alpha),
                                 txt.c_str(), e.align);
            break;
        }

        case Widget::Image: {
            // Com resolvedor, desenha a imagem de verdade. Sem ele (ou se o
            // arquivo nao abrir), volta a moldura com o nome do arquivo: quem
            // edita no Caldera ve' o lugar certo e o jogo nao finge que
            // desenhou.
            void* tex = (m_imageResolver && !e.image.empty()) ? m_imageResolver(e.image) : nullptr;
            if (tex) {
                // Tinta multiplica a imagem: ui.color(sel, cor, 0), senao
                // --image-tint, senao branco (a imagem como e').
                const ImU32 tint = withAlpha(e.hasColor[0] ? toImU32(e.color[0]) : ImU32(e.tintCss), alpha);
                dl->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h),
                             ImVec2(e.uv.x, e.uv.y), ImVec2(e.uv.z, e.uv.w), tint);
                break;
            }
            m_layout.drawBox(dl, r, e.selector, alpha, 0, IM_COL32(150, 150, 150, 200), 1.0f, 0.0f, bg, edge);
            if (!e.image.empty() && r.h > 12.0f)
                CSSLayout::drawLabel(dl, r.center().x, r.center().y - 6.0f, 11.0f, IM_COL32(200, 200, 200, 180),
                                     e.image.c_str(), 1);
            break;
        }
        }
    }

}

} // namespace eruption
