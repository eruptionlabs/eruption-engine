#include "renderer/UiCanvas.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

ImU32 col(const Vec4& c, float alphaMul = 1.0f) {
    return IM_COL32(int(std::clamp(c.r, 0.0f, 1.0f) * 255.0f), int(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
                    int(std::clamp(c.b, 0.0f, 1.0f) * 255.0f), int(std::clamp(c.a * alphaMul, 0.0f, 1.0f) * 255.0f));
}

// Texto no tamanho pedido com a fonte que já está carregada. A fonte do ImGui
// escala por fator; `size` é a altura em pixels.
void text(ImDrawList* dl, float px, float py, float size, ImU32 color, const char* s, uint8_t align, bool shadow) {
    ImFont* font = ImGui::GetFont();
    const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    const float x = align == 0 ? px : (align == 1 ? px - w * 0.5f : px - w);
    if (shadow) dl->AddText(font, size, ImVec2(x + 1.0f, py + 1.0f), IM_COL32(0, 0, 0, 170), s);
    dl->AddText(font, size, ImVec2(x, py), color, s);
}

} // namespace

uint32_t UiCanvas::add(Kind kind) {
    Node n;
    n.id = m_nextId++;
    n.kind = kind;
    m_nodes.push_back(std::move(n));
    return m_nodes.back().id;
}

UiCanvas::Node* UiCanvas::get(uint32_t id) {
    for (Node& n : m_nodes)
        if (n.id == id && n.alive) return &n;
    return nullptr;
}

void UiCanvas::remove(uint32_t id) {
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].id != id) continue;
        m_nodes.erase(m_nodes.begin() + static_cast<long>(i));
        return;
    }
}

void UiCanvas::clear() {
    m_nodes.clear();
    m_popups.clear();
    m_fade = Fade{};
}

void UiCanvas::popup(const Vec3& world, const std::string& text, const Vec4& color, float seconds, float rise,
                     float size) {
    Popup p;
    p.world = world;
    p.text = text;
    p.color = color;
    p.total = p.left = std::max(0.05f, seconds);
    p.rise = rise;
    p.size = size > 0.0f ? size : 0.04f;
    m_popups.push_back(std::move(p));
}

void UiCanvas::screenFade(const Vec4& color, float from, float to, float seconds) {
    m_fade.color = color;
    m_fade.from = from;
    m_fade.to = to;
    m_fade.total = m_fade.left = std::max(0.0f, seconds);
    m_fade.now = from;
    if (m_fade.total <= 0.0f) m_fade.now = to;
}

void UiCanvas::update(float dt) {
    for (size_t i = m_popups.size(); i-- > 0;) {
        m_popups[i].left -= dt;
        if (m_popups[i].left <= 0.0f) m_popups.erase(m_popups.begin() + static_cast<long>(i));
    }
    if (m_fade.left > 0.0f) {
        m_fade.left = std::max(0.0f, m_fade.left - dt);
        const float t = 1.0f - m_fade.left / std::max(1e-4f, m_fade.total);
        m_fade.now = m_fade.from + (m_fade.to - m_fade.from) * t;
    }
}

void UiCanvas::draw(ImDrawList* dl, float x, float y, float w, float h, const Mat4& viewProj) const {
    if (!dl || w <= 0.0f || h <= 0.0f) return;

    // Nós por z (ordem estável: empate mantém a ordem de criação).
    m_sorted.clear();
    m_sorted.reserve(m_nodes.size());
    for (const Node& n : m_nodes)
        if (n.visible) m_sorted.push_back(&n);
    std::stable_sort(m_sorted.begin(), m_sorted.end(), [](const Node* a, const Node* b) { return a->z < b->z; });

    for (const Node* n : m_sorted) {
        const ImVec2 p0(x + n->x * w, y + n->y * h);
        const ImVec2 p1(p0.x + n->w * w, p0.y + n->h * h);
        switch (n->kind) {
        case Kind::Rect:
            if (n->color.a > 0.0f) dl->AddRectFilled(p0, p1, col(n->color), n->radius);
            if (n->border > 0.0f && n->color2.a > 0.0f) dl->AddRect(p0, p1, col(n->color2), n->radius, 0, n->border);
            break;
        case Kind::Bar: {
            if (n->color.a > 0.0f) dl->AddRectFilled(p0, p1, col(n->color), n->radius);
            const float v = std::clamp(n->value, 0.0f, 1.0f);
            if (v > 0.0f && n->color2.a > 0.0f) {
                const float pad = n->border;
                dl->AddRectFilled(ImVec2(p0.x + pad, p0.y + pad),
                                  ImVec2(p0.x + pad + (p1.x - p0.x - pad * 2.0f) * v, p1.y - pad), col(n->color2),
                                  n->radius);
            }
            if (n->border > 0.0f) dl->AddRect(p0, p1, col(Vec4(0.0f, 0.0f, 0.0f, 0.6f)), n->radius, 0, n->border);
            if (!n->text.empty())
                text(dl, (p0.x + p1.x) * 0.5f, p0.y + ((p1.y - p0.y) - n->textSize * h) * 0.5f, n->textSize * h,
                     IM_COL32_WHITE, n->text.c_str(), 1, n->shadow);
            break;
        }
        case Kind::Text: {
            const float size = n->textSize * h;
            const float px = n->align == 0 ? p0.x : (n->align == 1 ? (p0.x + p1.x) * 0.5f : p1.x);
            text(dl, px, p0.y, size, col(n->color), n->text.c_str(), n->align, n->shadow);
            break;
        }
        }
    }

    // Números: sobem e apagam no fim. Fora da tela não desenha.
    for (const Popup& p : m_popups) {
        const Vec4 c = viewProj * Vec4(p.world, 1.0f);
        if (c.w <= 1e-3f) continue;
        const float t = 1.0f - p.left / std::max(1e-4f, p.total);
        const float sx = x + (c.x / c.w * 0.5f + 0.5f) * w;
        const float sy = y + (c.y / c.w * 0.5f + 0.5f) * h - p.rise * h * t;
        if (sx < x - 50.0f || sx > x + w + 50.0f || sy < y - 50.0f || sy > y + h + 50.0f) continue;
        // Sobe rápido e desacelera; só o último terço perde opacidade.
        const float alpha = t < 0.66f ? 1.0f : 1.0f - (t - 0.66f) / 0.34f;
        const float pop = t < 0.15f ? 1.0f + (0.15f - t) * 2.0f : 1.0f; // estica ao nascer
        text(dl, sx, sy, p.size * h * pop, col(p.color, alpha), p.text.c_str(), 1, true);
    }

    if (m_fade.now > 0.001f) {
        Vec4 c = m_fade.color;
        c.a = m_fade.now;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(c));
    }
}

} // namespace eruption
