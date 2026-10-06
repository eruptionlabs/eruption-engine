#include "game/HudRenderer.hpp"
#include "game/PlayerController.hpp"
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace eruption {

void HudRenderer::init(Engine* engine, PlayerController* player, const std::string& cssPath) {
    m_engine = engine;
    m_player = player;
    m_cssPath = cssPath;
    m_layout.setParser(&m_parser);
    m_loaded = false;
    reloadIfChanged();
}

void HudRenderer::shutdown() {
    m_engine = nullptr;
    m_player = nullptr;
    m_loaded = false;
}

void HudRenderer::update() {
    reloadIfChanged();
}

void HudRenderer::reloadIfChanged() {
    namespace fs = std::filesystem;
    try {
        if (!fs::exists(m_cssPath)) {
            if (!m_loaded) ERUPTION_LOG_WARN("[HUD] CSS nao encontrado: %s (usando layout padrao)", m_cssPath.c_str());
            m_loaded = true; // nao insistir a cada frame
            return;
        }
        auto mtime = fs::last_write_time(m_cssPath);
        if (m_loaded && mtime == m_cssMTime) return;
        if (m_parser.load(m_cssPath)) {
            m_cssMTime = mtime;
            m_loaded = true;
            ERUPTION_LOG_INFO("[HUD] CSS carregado de %s", m_cssPath.c_str());
        } else {
            ERUPTION_LOG_WARN("[HUD] Falha ao ler %s", m_cssPath.c_str());
            m_loaded = true;
        }
    } catch (...) {
        // erros de filesystem nao derrubam a HUD
    }
}

CSSRect HudRenderer::resolveWidget(const std::string& selector, const ImVec2& displaySize,
                                   const CSSRect& parent, bool* found) const {
    const bool bindToPlayer = CSSLayout::parseFloat(
        m_layout.rawProperty(selector, "--bind-to-player", "0"), 0.0f) >= 0.5f;

    CSSRect out = m_layout.resolve(selector, displaySize, bindToPlayer ? CSSRect{} : parent, found);

    if (bindToPlayer && m_engine && m_player) {
        Vec2 base = m_player->getSpriteScreenBase(m_engine->camera(), displaySize.x, displaySize.y);
        if (base.x >= 0.0f && base.y >= 0.0f) {
            float zoomScale = 1.0f;
            const Camera& cam = m_engine->camera();
            if (cam.isOrthographic()) {
                zoomScale = cam.zoomLevel();
            } else {
                float dist = cam.orbitDistance();
                float def = cam.defaultOrbitDistance();
                if (dist > 0.0f && def > 0.0f) zoomScale = def / dist;
            }
            out.x = base.x + out.x * zoomScale;
            out.y = base.y + out.y * zoomScale;
            out.w *= zoomScale;
            out.h *= zoomScale;
        }
    }
    return out;
}

CSSRect HudRenderer::getMinimapRect(const ImVec2& displaySize) const {
    bool found = false;
    return resolveWidget("#minimap", displaySize, {}, &found);
}

CSSRect HudRenderer::getMinimapInfoRect(const ImVec2& displaySize) const {
    bool found = false;
    return resolveWidget("#minimap-info", displaySize, {}, &found);
}

void HudRenderer::drawStyleBox(ImDrawList* dl, const CSSRect& r, const std::string& selector) const {
    if (!m_layout.displayVisible(selector)) return;
    const float radius = m_layout.borderRadius(selector, 4.0f);
    const ImU32 bg = m_layout.backgroundColor(selector, IM_COL32(20, 22, 28, 220));
    if ((bg >> IM_COL32_A_SHIFT) & 0xFF) dl->AddRectFilled(r.min(), r.max(), bg, radius);
    const float bw = m_layout.borderWidth(selector, 1.0f);
    const ImU32 bc = m_layout.borderColor(selector, IM_COL32(90, 100, 120, 255));
    if (bw > 0.0f && ((bc >> IM_COL32_A_SHIFT) & 0xFF)) dl->AddRect(r.min(), r.max(), bc, radius, 0, bw);
}

void HudRenderer::drawBar(ImDrawList* dl, const CSSRect& r, float ratio, ImU32 fillLow, ImU32 fillHigh,
                          const std::string& selector, const char* valueText) const {
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    drawStyleBox(dl, r, selector);
    const float bw = m_layout.borderWidth(selector, 1.0f);
    const float radius = m_layout.borderRadius(selector, 4.0f);
    ImVec2 fillMin(r.x + bw, r.y + bw);
    ImVec2 fillMax(r.x + bw + (r.w - 2.0f * bw) * ratio, r.y + r.h - bw);
    if (fillMax.x > fillMin.x) {
        dl->AddRectFilledMultiColor(fillMin, fillMax, fillLow, fillHigh, fillHigh, fillLow);
        (void)radius;
    }
    if (valueText && *valueText) {
        ImVec2 ts = ImGui::CalcTextSize(valueText);
        ImVec2 p(r.x + (r.w - ts.x) * 0.5f, r.y + (r.h - ts.y) * 0.5f);
        const ImU32 tc = m_layout.textColor(selector, IM_COL32(240, 240, 240, 255));
        dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), valueText);
        dl->AddText(p, tc, valueText);
    }
}

void HudRenderer::draw() {
    if (!m_engine) return;
    // ERUPTION_TEST_HUD_DEBUG=1: loga (uma vez) a ancora do sprite na tela e a
    // altura projetada do quad do personagem, para calibrar o Caldera.
    static const bool kHudDebug = std::getenv("ERUPTION_TEST_HUD_DEBUG") != nullptr;
    static int s_debugLeft = kHudDebug ? 3 : 0;
    if (s_debugLeft > 0 && m_player) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const Camera& cam = m_engine->camera();
        Vec2 base = m_player->getSpriteScreenBase(cam, ds.x, ds.y);
        const float h = m_player->spriteHeight();
        auto toScreen = [&](const Vec3& world) {
            Vec4 clip = cam.viewProjNoJitter() * Vec4(world, 1.0f);
            Vec3 ndc = Vec3(clip) / clip.w;
            return Vec2((ndc.x * 0.5f + 0.5f) * ds.x, (ndc.y * 0.5f + 0.5f) * ds.y);
        };
        // quad do billboard: no espaco de VISTA, de anchor ate anchor + up*h
        Vec4 anchorView = cam.viewMatrix() * Vec4(m_player->pos(), 1.0f);
        Vec4 topView = anchorView + Vec4(0.0f, h, 0.0f, 0.0f);
        Vec4 clipA = cam.projNoJitter() * anchorView, clipT = cam.projNoJitter() * topView;
        Vec2 sA((clipA.x / clipA.w * 0.5f + 0.5f) * ds.x, (clipA.y / clipA.w * 0.5f + 0.5f) * ds.y);
        Vec2 sT((clipT.x / clipT.w * 0.5f + 0.5f) * ds.x, (clipT.y / clipT.w * 0.5f + 0.5f) * ds.y);
        Vec2 feet = toScreen(m_player->pos());
        ERUPTION_LOG_WARN("[HUD-DEBUG] display %.0fx%.0f | pos=(%.1f,%.1f,%.1f) h=%.2f | pes tela=(%.1f,%.1f) ancora(0.4h)=(%.1f,%.1f) | quad view: base y=%.1f topo y=%.1f (alt %.1f px) | orbitDist=%.1f pitch=%.1f",
            ds.x, ds.y, m_player->pos().x, m_player->pos().y, m_player->pos().z, h, feet.x, feet.y, base.x, base.y,
            sA.y, sT.y, sA.y - sT.y, cam.orbitDistance(), glm::degrees(cam.orbitPitch()));
        --s_debugLeft;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // Painel inferior
    bool found = false;
    CSSRect panel = resolveWidget("#hero-panel", displaySize, {}, &found);
    if (!found) panel = CSSRect{0.0f, displaySize.y - 120.0f, displaySize.x, 120.0f};
    drawStyleBox(dl, panel, "#hero-panel");

    // Retrato: moldura + iniciais do nome (a aplicacao pode trocar por textura)
    CSSRect portrait = resolveWidget("#hero-portrait", displaySize, panel, &found);
    if (!found) portrait = CSSRect{panel.x + 16.0f, panel.y + 12.0f, 80.0f, 80.0f};
    drawStyleBox(dl, portrait, "#hero-portrait");
    {
        std::string initials;
        for (char c : m_stats.name) if (std::isupper(static_cast<unsigned char>(c))) initials += c;
        if (initials.empty() && !m_stats.name.empty()) initials = m_stats.name.substr(0, 1);
        const float fs = ImGui::GetFontSize() * 1.6f;
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, initials.c_str());
        dl->AddText(ImGui::GetFont(), fs,
                    ImVec2(portrait.x + (portrait.w - ts.x) * 0.5f, portrait.y + (portrait.h - ts.y) * 0.5f),
                    m_layout.textColor("#hero-portrait", IM_COL32(230, 230, 230, 255)), initials.c_str());
    }

    // Nivel
    CSSRect level = resolveWidget("#hero-level", displaySize, panel, &found);
    if (!found) level = CSSRect{portrait.x, portrait.y + portrait.h + 4.0f, portrait.w, 18.0f};
    drawStyleBox(dl, level, "#hero-level");
    char lvl[16]; std::snprintf(lvl, sizeof(lvl), "Lv. %d", m_stats.level);
    ImVec2 ls = ImGui::CalcTextSize(lvl);
    dl->AddText(ImVec2(level.x + (level.w - ls.x) * 0.5f, level.y + (level.h - ls.y) * 0.5f),
                m_layout.textColor("#hero-level", IM_COL32(20, 20, 20, 255)), lvl);

    // Nome ao lado do retrato
    dl->AddText(ImVec2(portrait.x + portrait.w + 12.0f, portrait.y),
                m_layout.textColor("#hero-panel", IM_COL32(230, 230, 230, 255)), m_stats.name.c_str());

    // Barras
    CSSRect hpR = resolveWidget("#hero-hp-bar", displaySize, panel, &found);
    if (!found) hpR = CSSRect{portrait.x + portrait.w + 12.0f, portrait.y + 24.0f, 220.0f, 20.0f};
    CSSRect spR = resolveWidget("#hero-sp-bar", displaySize, panel, &found);
    if (!found) spR = CSSRect{hpR.x, hpR.y + hpR.h + 6.0f, hpR.w, hpR.h};

    char hpText[32], spText[32];
    std::snprintf(hpText, sizeof(hpText), "%.0f / %.0f", m_stats.hp, m_stats.hpMax);
    std::snprintf(spText, sizeof(spText), "%.0f / %.0f", m_stats.sp, m_stats.spMax);
    const float hpRatio = m_stats.hpMax > 0.0f ? m_stats.hp / m_stats.hpMax : 0.0f;
    const float spRatio = m_stats.spMax > 0.0f ? m_stats.sp / m_stats.spMax : 0.0f;
    drawBar(dl, hpR, hpRatio, IM_COL32(170, 40, 40, 255), IM_COL32(230, 90, 90, 255), "#hero-hp-bar", hpText);
    drawBar(dl, spR, spRatio, IM_COL32(40, 80, 170, 255), IM_COL32(90, 140, 230, 255), "#hero-sp-bar", spText);

    // HP/SP sobre o personagem: duas barras retas (HP em cima, SP embaixo).
    CSSRect oh = resolveWidget("#overhead-hp-sp", displaySize, {}, &found);
    if (found && oh.w > 2.0f && oh.h > 2.0f) {
        ImDrawList* fg = ImGui::GetBackgroundDrawList();
        const float gap = 1.0f;
        const float barH = std::max(2.0f, (oh.h - gap) * 0.5f);
        auto bar = [&](float top, float ratio, ImU32 fill) {
            ratio = std::clamp(ratio, 0.0f, 1.0f);
            fg->AddRectFilled(ImVec2(oh.x, top), ImVec2(oh.x + oh.w, top + barH), IM_COL32(20, 22, 28, 230));
            if (ratio > 0.0f)
                fg->AddRectFilled(ImVec2(oh.x + 1.0f, top + 1.0f), ImVec2(oh.x + 1.0f + (oh.w - 2.0f) * ratio, top + barH - 1.0f), fill);
            fg->AddRect(ImVec2(oh.x, top), ImVec2(oh.x + oh.w, top + barH), IM_COL32(58, 65, 82, 255));
        };
        bar(oh.y, hpRatio, IM_COL32(200, 60, 60, 255));
        bar(oh.y + barH + gap, spRatio, IM_COL32(70, 120, 220, 255));
    }

    // Barra de conjuracao (so' enquanto ha' conjuracao), abaixo das barras de HP/SP.
    if (m_stats.castProgress > 0.0f) {
        CSSRect cb = resolveWidget("#cast-bar", displaySize, {}, &found);
        if (found && cb.w > 2.0f && cb.h > 2.0f) {
            ImDrawList* fg = ImGui::GetBackgroundDrawList();
            const float ratio = std::clamp(m_stats.castProgress, 0.0f, 1.0f);
            const ImU32 bg = m_layout.backgroundColor("#cast-bar", IM_COL32(12, 16, 24, 240));
            const ImU32 bc = m_layout.borderColor("#cast-bar", IM_COL32(80, 110, 150, 255));
            fg->AddRectFilled(cb.min(), cb.max(), bg, 2.0f);
            fg->AddRectFilled(ImVec2(cb.x + 1.0f, cb.y + 1.0f), ImVec2(cb.x + 1.0f + (cb.w - 2.0f) * ratio, cb.y + cb.h - 1.0f), IM_COL32(60, 190, 255, 255));
            fg->AddRect(cb.min(), cb.max(), bc, 2.0f);
            if (!m_stats.castLabel.empty()) {
                ImVec2 ts = ImGui::CalcTextSize(m_stats.castLabel.c_str());
                fg->AddText(ImVec2(cb.x + (cb.w - ts.x) * 0.5f, cb.y + cb.h + 2.0f), IM_COL32(230, 240, 255, 255), m_stats.castLabel.c_str());
            }
        }
    }
}

} // namespace eruption
