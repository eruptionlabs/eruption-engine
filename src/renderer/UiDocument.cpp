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

void UiDocument::setBadge(const std::string& selector, const std::string& text, const std::string& image) {
    if (Element* e = find(selector)) {
        e->hasBadge = true;
        e->badgeText = text;
        e->badgeImage = image;
    }
}

void UiDocument::clearBadge(const std::string& selector) {
    if (Element* e = find(selector)) e->hasBadge = false;
}

// Circulo do selo: centro no canto de cima a' direita, diametro de 4% da
// altura da tela (minimo 18 px), --badge-size no CSS do elemento muda. Fica
// dentro da tela quando o elemento encosta na borda.
CSSRect UiDocument::badgeVisual(const Element& e, const Vec2& area) const {
    const CSSRect r = resolveRect(e, area);
    float d = std::max(18.0f, area.y * 0.04f);
    CSSLayout::parseLength(m_layout.rawProperty(e.selector, "--badge-size"), area.y, d);
    const float cx = std::clamp(r.x + r.w, d * 0.5f, area.x - d * 0.5f);
    const float cy = std::clamp(r.y, d * 0.5f, area.y - d * 0.5f);
    return CSSRect{cx - d * 0.5f, cy - d * 0.5f, d, d};
}

bool UiDocument::badgeRects(const std::string& selector, float x, float y, float w, float h, float minTouch,
                            CSSRect& visual, CSSRect& touch) const {
    const Element* e = find(selector);
    if (!e || !e->hasBadge) return false;
    visual = badgeVisual(*e, Vec2(w, h));
    visual.x += x;
    visual.y += y;
    const float side = std::max(visual.w, minTouch);
    touch = CSSRect{visual.x + visual.w * 0.5f - side * 0.5f, visual.y + visual.h * 0.5f - side * 0.5f, side, side};
    return true;
}

// Pintura do selo: --badge-color (fundo; vermelho), a imagem inteira no
// circulo, ou o texto centrado em branco.
void UiDocument::drawBadge(ImDrawList* dl, const Element& e, const CSSRect& c) const {
    const ImVec2 mid(c.x + c.w * 0.5f, c.y + c.h * 0.5f);
    void* tex = (m_imageResolver && !e.badgeImage.empty()) ? m_imageResolver(e.badgeImage) : nullptr;
    if (tex) {
        dl->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(c.x, c.y), ImVec2(c.x + c.w, c.y + c.h));
        return;
    }
    ImU32 fill = CSSLayout::parseColor(m_layout.rawProperty(e.selector, "--badge-color"), 0);
    if (!fill) fill = IM_COL32(210, 40, 50, 255);
    dl->AddCircleFilled(mid, c.w * 0.5f, fill, 24);
    dl->AddCircle(mid, c.w * 0.5f, IM_COL32(255, 255, 255, 220), 24, std::max(1.0f, c.w * 0.06f));
    if (!e.badgeText.empty())
        CSSLayout::drawLabel(dl, mid.x, mid.y - c.h * 0.36f, c.h * 0.72f, IM_COL32(255, 255, 255, 255),
                             e.badgeText.c_str(), 1);
}

void UiDocument::setGraph(const std::string& selector, std::vector<GraphNode> nodes) {
    Element* e = find(selector);
    if (!e) return;
    const size_t n = std::min<size_t>(nodes.size(), 4096);
    nodes.resize(n);
    std::unordered_map<std::string, uint16_t> index;
    for (size_t i = 0; i < n; ++i) index.emplace(nodes[i].id, static_cast<uint16_t>(i));
    e->graphEdges.clear();
    std::vector<int> incoming(n, 0);
    for (size_t i = 0; i < n; ++i)
        for (const std::string& to : nodes[i].links) {
            const auto it = index.find(to);
            if (it == index.end() || it->second == i) continue;
            e->graphEdges.emplace_back(static_cast<uint16_t>(i), it->second);
            ++incoming[it->second];
        }

    // Camadas: profundidade pela busca em largura a partir das raizes; o que
    // sobra (ciclo sem raiz) comeca uma busca nova na profundidade 0.
    std::vector<std::vector<uint16_t>> next(n);
    for (const auto& ed : e->graphEdges) next[ed.first].push_back(ed.second);
    std::vector<int> depth(n, -1);
    std::vector<uint16_t> fila;
    fila.reserve(n);
    auto busca = [&](uint16_t raiz) {
        depth[raiz] = 0;
        fila.push_back(raiz);
        for (size_t k = fila.size() - 1; k < fila.size(); ++k)
            for (uint16_t j : next[fila[k]])
                if (depth[j] < 0) { depth[j] = depth[fila[k]] + 1; fila.push_back(j); }
    };
    for (size_t i = 0; i < n; ++i)
        if (incoming[i] == 0 && depth[i] < 0) busca(static_cast<uint16_t>(i));
    for (size_t i = 0; i < n; ++i)
        if (depth[i] < 0) busca(static_cast<uint16_t>(i));
    int levels = 0;
    for (int d : depth) levels = std::max(levels, d + 1);
    std::vector<std::vector<uint16_t>> level(static_cast<size_t>(levels));
    for (size_t i = 0; i < n; ++i) level[static_cast<size_t>(depth[i])].push_back(static_cast<uint16_t>(i));
    size_t widest = 1;
    for (const auto& l : level) widest = std::max(widest, l.size());

    // Posicao na camada (Sugiyama, so' a parte do baricentro): uma descida
    // pela media dos pais acerta a ORDEM de cada camada; a camada mais larga
    // fica espalhada por igual nessa ordem; acima dela cada no vai para a
    // media dos filhos, abaixo para a dos pais, com folga minima de uma
    // coluna da camada mais larga. So' descer apertava os pais num canto e
    // espalhar todas por igual cruzava as ligacoes.
    std::vector<float> xs(n, 0.5f);
    const float gap = 1.0f / static_cast<float>(widest);
    auto media = [&](uint16_t j, int vizinha, bool pais) {
        float sum = 0.0f;
        int cnt = 0;
        for (const auto& ed : e->graphEdges) {
            const uint16_t de = pais ? ed.second : ed.first, para = pais ? ed.first : ed.second;
            if (de == j && depth[para] == vizinha) sum += xs[para], ++cnt;
        }
        return cnt ? sum / static_cast<float>(cnt) : xs[j];
    };
    auto assenta = [&](std::vector<uint16_t>& l, int vizinha, bool pais) {
        std::vector<float> alvo(l.size());
        for (size_t k = 0; k < l.size(); ++k) alvo[k] = media(l[k], vizinha, pais);
        for (size_t k = 0; k < l.size(); ++k) xs[l[k]] = alvo[k];
        std::stable_sort(l.begin(), l.end(), [&xs](uint16_t a, uint16_t b) { return xs[a] < xs[b]; });
        for (size_t k = 1; k < l.size(); ++k) xs[l[k]] = std::max(xs[l[k]], xs[l[k - 1]] + gap);
        const float over = xs[l.back()] - (1.0f - gap * 0.5f);
        if (over > 0.0f)
            for (uint16_t j : l) xs[j] -= over;
        xs[l[0]] = std::max(xs[l[0]], gap * 0.5f);
        for (size_t k = 1; k < l.size(); ++k) xs[l[k]] = std::max(xs[l[k]], xs[l[k - 1]] + gap);
    };
    auto espalha = [&](const std::vector<uint16_t>& l) {
        for (size_t k = 0; k < l.size(); ++k) xs[l[k]] = (k + 0.5f) / static_cast<float>(l.size());
    };
    size_t larga = 0;
    for (size_t d = 0; d < level.size(); ++d) {
        if (d == 0) espalha(level[d]);
        else assenta(level[d], static_cast<int>(d) - 1, true);
        if (level[d].size() > level[larga].size()) larga = d;
    }
    espalha(level[larga]);
    for (size_t d = larga; d-- > 0;) assenta(level[d], static_cast<int>(d) + 1, false);
    for (size_t d = larga + 1; d < level.size(); ++d) assenta(level[d], static_cast<int>(d) - 1, true);

    e->graphAt.resize(n);
    e->graphAuto.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const bool autoPos = nodes[i].pos.x < 0.0f || nodes[i].pos.y < 0.0f;
        e->graphAuto[i] = autoPos ? 1 : 0;
        e->graphAt[i] = autoPos ? Vec2(xs[i], (depth[i] + 0.5f) / static_cast<float>(levels)) : nodes[i].pos;
    }
    e->graph = std::move(nodes);
}

void UiDocument::clearGraph(const std::string& selector) {
    if (Element* e = find(selector)) {
        e->graph.clear();
        e->graphAt.clear();
        e->graphAuto.clear();
        e->graphEdges.clear();
    }
}

// Diametro do no: --node-size no CSS do elemento, senao 6% da altura da tela
// (65 px a 1080), nunca menos de 16 px.
float UiDocument::graphRadius(const Element& e, const Vec2& area) const {
    float d = std::max(16.0f, area.y * 0.06f);
    CSSLayout::parseLength(m_layout.rawProperty(e.selector, "--node-size"), area.y, d);
    return d * 0.5f;
}

bool UiDocument::graphSideways(const Element& e) const {
    return m_layout.rawProperty(e.selector, "--graph-direction") == "right";
}

// Lido uma vez por desenho: a consulta ao CSS por ponto era metade do custo.
Vec2 UiDocument::graphPoint(const Element& e, size_t i, const CSSRect& r, bool sideways) {
    Vec2 a = e.graphAt[i];
    if (sideways && e.graphAuto[i]) a = Vec2(a.y, a.x);
    return Vec2(r.x + a.x * r.w, r.y + a.y * r.h);
}

// Ligacoes por baixo (--link-color), depois os nos: circulo (--node-color ou
// a cor do no), o icone inscrito, ou o texto dentro; com icone, o texto vai
// embaixo do no.
void UiDocument::drawGraph(ImDrawList* dl, const Element& e, const CSSRect& r, const Vec2& area, float alpha) const {
    const float rad = graphRadius(e, area);
    ImU32 link = CSSLayout::parseColor(m_layout.rawProperty(e.selector, "--link-color"), 0);
    if (!link) link = IM_COL32(200, 200, 210, 200);
    link = withAlpha(link, alpha);
    const float thick = std::max(1.5f, rad * 0.12f);
    const bool side = graphSideways(e);
    for (const auto& ed : e.graphEdges) {
        const Vec2 a = graphPoint(e, ed.first, r, side), b = graphPoint(e, ed.second, r, side);
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), link, thick);
    }
    ImU32 fillCss = CSSLayout::parseColor(m_layout.rawProperty(e.selector, "--node-color"), 0);
    if (!fillCss) fillCss = IM_COL32(40, 55, 80, 235);
    const ImU32 rim = withAlpha(IM_COL32(255, 255, 255, 220), alpha);
    const ImU32 ink = withAlpha(m_layout.textColor(e.selector, IM_COL32(240, 240, 240, 255)), alpha);
    for (size_t i = 0; i < e.graph.size(); ++i) {
        const GraphNode& g = e.graph[i];
        const Vec2 p = graphPoint(e, i, r, side);
        const ImVec2 c(p.x, p.y);
        dl->AddCircleFilled(c, rad, withAlpha(g.hasColor ? toImU32(g.color) : fillCss, alpha), 24);
        dl->AddCircle(c, rad, rim, 24, std::max(1.0f, rad * 0.08f));
        void* tex = (m_imageResolver && !g.image.empty()) ? m_imageResolver(g.image) : nullptr;
        if (tex) {
            const float s = rad * 0.7f;
            dl->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s),
                         ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), withAlpha(IM_COL32(255, 255, 255, 255), alpha));
            if (!g.text.empty())
                CSSLayout::drawLabel(dl, c.x, c.y + rad + 2.0f, rad * 0.55f, ink, g.text.c_str(), 1);
        } else if (!g.text.empty()) {
            CSSLayout::drawLabel(dl, c.x, c.y - rad * 0.4f, rad * 0.8f, ink, g.text.c_str(), 1);
        }
    }
}

bool UiDocument::graphNodeAt(const std::string& selector, float x, float y, float w, float h, float minTouch,
                             const Vec2& p, std::string& id) const {
    const Element* e = find(selector);
    if (!e || e->graph.empty() || chainHidden(*e)) return false;
    const Vec2 area(w, h);
    CSSRect r = resolveRect(*e, area);
    r.x += x;
    r.y += y;
    const float reach = std::max(graphRadius(*e, area), minTouch * 0.5f);
    float best = reach * reach;
    const bool side = graphSideways(*e);
    int hit = -1;
    for (size_t i = 0; i < e->graph.size(); ++i) {
        const Vec2 d = graphPoint(*e, i, r, side) - p;
        const float d2 = d.x * d.x + d.y * d.y;
        if (d2 <= best) { best = d2; hit = static_cast<int>(i); }
    }
    if (hit < 0) return false;
    id = e->graph[static_cast<size_t>(hit)].id;
    return true;
}

bool UiDocument::graphNodeCenter(const std::string& selector, const std::string& id, float x, float y, float w,
                                 float h, Vec2& center) const {
    const Element* e = find(selector);
    if (!e) return false;
    CSSRect r = resolveRect(*e, Vec2(w, h));
    r.x += x;
    r.y += y;
    for (size_t i = 0; i < e->graph.size(); ++i)
        if (e->graph[i].id == id) { center = graphPoint(*e, i, r, graphSideways(*e)); return true; }
    return false;
}

bool UiDocument::chainHidden(const Element& e) const {
    // A mesma cadeia curta de resolveRect.
    const Element* p = &e;
    for (int n = 0; p && n < 5; ++n) {
        if (!(p->hasVisible ? p->visible : p->visibleCss)) return true;
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
        e.hasBadge = false;
        e.graph.clear();
        e.graphAt.clear();
        e.graphAuto.clear();
        e.graphEdges.clear();
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
        if (chainHidden(e)) continue;

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
        if (!e.graph.empty()) drawGraph(dl, e, r, area, alpha);
    }

    // Selos por cima de todos os elementos: o de um botao nao some sob o
    // vizinho de z maior.
    for (const Element& e : m_elements) {
        if (!e.hasBadge || chainHidden(e)) continue;
        CSSRect c = badgeVisual(e, area);
        c.x += x;
        c.y += y;
        drawBadge(dl, e, c);
    }
}

} // namespace eruption
