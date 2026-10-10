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
