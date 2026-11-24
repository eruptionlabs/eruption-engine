// Aba Regions do painel Terrain: desenha regioes sobre uma textura do mapa e
// diz como cada uma se comporta ("janela com luz" acende de noite, "poste"
// acende e traz uma luz pontual, areia/neve sao tipos de superficie). E' um
// mapa de IDs por textura, como os ID maps do Substance.
//
// A logica (rasterizar, mascara emissiva, laco magnetico) esta em
// formats/TextureRegions; o motor monta a textura emissiva e sobe so' ela a
// cada edicao (Engine::refreshRegionEmissive), entao o slider de hora mostra
// a noite na hora, sem recarregar o mapa. Os dados vao para o arquivo da cena
// (SceneEdits, texture_regions) com Ctrl+S.
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "editor/EditorTheme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace eruption {

namespace {

using namespace texregion;

enum RegionTool { ToolSelect, ToolPoly, ToolRect, ToolEllipse, ToolBrush, ToolEraser, ToolLasso, ToolWand, ToolCount };
const char* kToolNames[ToolCount] = {"Select",  "Polygon",         "Rectangle", "Ellipse",
                                     "Brush",   "Eraser",          "Magnetic lasso", "Magic wand"};

// Lista extensivel: um tipo novo entra aqui e em isGlowType/isSurfaceType.
const char* kTypeIds[] = {kTypeWindow, kTypeLamp, kTypeSand, kTypeSnow};
const char* kTypeLabels[] = {"Lit window", "Lamp / post / torch", "Sand", "Snow"};
const char* kGlowLabels[] = {"Dark glows (window glass)", "Light glows (lantern glass)", "Paper (light glows)",
                             "Uniform"};

int typeIndex(const std::string& id) {
    for (int i = 0; i < IM_ARRAYSIZE(kTypeIds); ++i)
        if (id == kTypeIds[i]) return i;
    return -1;
}

ImU32 typeColor(const std::string& type, bool selected) {
    if (selected) return IM_COL32(60, 210, 255, 255);
    if (type == kTypeWindow) return IM_COL32(255, 170, 60, 255);
    if (type == kTypeLamp) return IM_COL32(255, 230, 90, 255);
    if (type == kTypeSand) return IM_COL32(220, 190, 120, 255);
    if (type == kTypeSnow) return IM_COL32(200, 225, 255, 255);
    return IM_COL32(180, 180, 180, 255);
}

// Regiao nova com os valores padrao do tipo.
Region newRegion(int typeIdx) {
    Region r;
    r.type = kTypeIds[std::clamp(typeIdx, 0, IM_ARRAYSIZE(kTypeIds) - 1)];
    if (r.type == kTypeLamp) {
        r.color[0] = 1.0f; r.color[1] = 0.82f; r.color[2] = 0.5f;
        r.glow = Glow::Light; // vidro de lampiao: o claro acende
    }
    return r;
}

} // namespace

texregion::ImageRegions& Editor::regionEdit() {
    auto& list = m_engine->textureRegions();
    for (auto& e : list)
        if (e.image == m_reg.image) return e;
    texregion::ImageRegions fresh;
    fresh.image = m_reg.image;
    m_reg.before = fresh; // o estado "antes" de uma imagem sem regioes
    list.push_back(fresh);
    return list.back();
}

void Editor::regionCommit(const std::string& label) {
    texregion::ImageRegions after = regionEdit();
    const texregion::ImageRegions before = m_reg.before;
    const std::string image = m_reg.image;
    m_reg.before = after;
    auto apply = [this, image](const texregion::ImageRegions& v) {
        auto& list = m_engine->textureRegions();
        auto it = std::find_if(list.begin(), list.end(), [&](const texregion::ImageRegions& e) { return e.image == image; });
        if (it == list.end()) list.push_back(v);
        else *it = v;
        if (m_reg.image == image) {
            m_reg.before = v;
            m_reg.sel = std::min(m_reg.sel, int(v.regions.size()) - 1);
        }
        m_reg.overlayDirty = true;
        m_engine->refreshRegionEmissive(image);
        updateTitle(true);
    };
    m_undo.push({label, [apply, before] { apply(before); }, [apply, after] { apply(after); }});
    m_engine->refreshRegionEmissive(image);
    m_reg.overlayDirty = true;
    updateTitle(true);
}

void Editor::regionOpenImage(const std::string& image) {
    RegionUi& g = m_reg;
    g.image = image;
    g.sel = -1;
    g.draft.clear();
    g.livePath.clear();
    g.dragging = g.stroking = false;
    g.rgba.clear();
    g.w = g.h = 0;
    releaseTexture(g.albedoTex);
    releaseTexture(g.overlayTex);
    g.before = texregion::ImageRegions{};
    g.before.image = image;
    for (const auto& e : m_engine->textureRegions())
        if (e.image == image) g.before = e;
    const uint8_t* px = nullptr;
    int w = 0, h = 0, ch = 4;
    if (!m_engine->regionAlbedo(image, px, w, h, ch)) return;
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 255);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = px[i * ch + (ch >= 3 ? c : 0)];
    int dw = w, dh = h;
    g.rgba = downscale(rgba.data(), w, h, 1024, dw, dh);
    g.w = dw;
    g.h = dh;
    g.albedoTex = makeTexture(g.rgba.data(), g.w, g.h, false);
    g.wire.setImage(g.rgba.data(), g.w, g.h, 4, 512);
    g.wireCell = -1;
    g.overlayDirty = true;
}

bool Editor::regionPickAt(float u, float v) {
    Vec3 origin, dir;
    viewportRay(u, v, origin, dir);
    float t = FLT_MAX;
    const int inst = raycastModels(origin, dir, t);
    ModelRenderer::SurfaceHit hit;
    if (inst < 0 || !m_engine->modelRenderer().surfaceTextureAt(uint32_t(inst), origin, dir, hit) || hit.texture.empty() ||
        !hit.hasUv)
        return false;
    auto norm = [](std::string n) {
        std::replace(n.begin(), n.end(), '/', '\\');
        if (!n.empty() && n[0] == '\\') n.erase(0, 1);
        return n;
    };
    const std::string want = norm(hit.texture);
    std::string image;
    for (const auto& n : m_engine->regionImages())
        if (norm(n) == want) image = n;
    if (image.empty()) return false;
    if (image != m_reg.image) regionOpenImage(image);
    const float pu = hit.uv.x - std::floor(hit.uv.x), pv = hit.uv.y - std::floor(hit.uv.y);
    m_reg.hasPick = true;
    m_reg.pickNode = hit.node;
    m_reg.pickU = pu;
    m_reg.pickV = pv;
    // A regiao de cima que cobre o ponto vira a selecionada (-1 = nenhuma).
    m_reg.sel = -1;
    for (const auto& e : m_engine->textureRegions()) {
        if (e.image != image) continue;
        std::vector<float> cov;
        for (int k = int(e.regions.size()) - 1; k >= 0 && m_reg.sel < 0; --k) {
            texregion::rasterize(e.regions[size_t(k)], 128, 128, cov);
            if (cov[size_t(std::min(127, int(pv * 128))) * 128 + size_t(std::min(127, int(pu * 128)))] > 0.5f) m_reg.sel = k;
        }
    }
    m_reg.overlayDirty = true;
    ERUPTION_LOG_INFO("Regioes: clique em '%s' (%.3f, %.3f), regiao %d", image.c_str(), pu, pv, m_reg.sel);
    return true;
}

void Editor::regionRebuildOverlay() {
    RegionUi& g = m_reg;
    if (g.rgba.empty()) return;
    const texregion::ImageRegions* e = nullptr;
    for (const auto& r : m_engine->textureRegions())
        if (r.image == g.image) e = &r;
    std::vector<uint8_t> out(size_t(g.w) * g.h * 4, 0);
    if (e && g.showGlow) {
        std::vector<uint8_t> rgb;
        texregion::buildEmissive(g.rgba.data(), g.w, g.h, 4, *e, m_engine->regionHour(), rgb);
        for (size_t i = 0; i < size_t(g.w) * g.h; ++i) {
            const uint8_t m = std::max({rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]});
            out[i * 4] = rgb[i * 3];
            out[i * 4 + 1] = rgb[i * 3 + 1];
            out[i * 4 + 2] = rgb[i * 3 + 2];
            out[i * 4 + 3] = uint8_t(std::min(255, int(m) * 2));
        }
    } else if (e) {
        std::vector<float> cov;
        for (size_t k = 0; k < e->regions.size(); ++k) {
            const texregion::Region& r = e->regions[k];
            const bool sel = int(k) == g.sel;
            const ImU32 col = typeColor(r.type, sel);
            const float a = sel ? 0.55f : 0.35f;
            texregion::rasterize(r, g.w, g.h, cov);
            for (size_t i = 0; i < cov.size(); ++i) {
                if (cov[i] <= 0.0f) continue;
                const float t = cov[i] * a;
                for (int c = 0; c < 3; ++c) {
                    const float dst = out[i * 4 + c], src = float((col >> (8 * c)) & 255u);
                    out[i * 4 + c] = uint8_t(dst + (src - dst) * t);
                }
                out[i * 4 + 3] = uint8_t(std::min(255.0f, out[i * 4 + 3] + t * 255.0f));
            }
        }
    }
    releaseTexture(g.overlayTex);
    g.overlayTex = makeTexture(out.data(), g.w, g.h, false);
    g.overlayDirty = false;
    g.overlayAt = float(ImGui::GetTime());
}

void Editor::regionAddLight(const texregion::Region& r) {
    Vec3 at;
    if (!groundPointAt(0.5f, 0.5f, at)) at = m_engine->camera().target();
    auto& lights = m_engine->m_deferredLighting.getPointLights();
    PointLight l;
    l.position = at + Vec3(0.0f, 15.0f, 0.0f);
    l.color = Vec3(r.color[0], r.color[1], r.color[2]);
    l.intensity = r.lightIntensity;
    l.radius = r.lightRadius;
    l.nightOnly = true;
    l.enabled = false; // a luz noturna decide a cada quadro (Engine::updateNightLights)
    l.onHour = r.onHour;
    l.offHour = r.offHour;
    l.flickerAmp = r.flicker;
    l.flickerSpeed = r.flickerSpeed;
    l.glowRadius = r.glowRadius;
    l.glowStrength = r.glowStrength;
    lights.push_back(l);
    const int idx = static_cast<int>(lights.size() - 1);
    select({SelectionKind::Light, idx});
    Engine* engine = m_engine;
    auto setEnabled = [engine, idx](bool on) {
        auto& ls = engine->m_deferredLighting.getPointLights();
        if (idx < static_cast<int>(ls.size())) {
            ls[static_cast<size_t>(idx)].nightOnly = on;
            if (!on) ls[static_cast<size_t>(idx)].enabled = false;
        }
    };
    m_undo.push({"Add light", [setEnabled] { setEnabled(false); }, [setEnabled] { setEnabled(true); }});
    updateTitle(true);
}

void Editor::drawRegionPanel() {
    RegionUi& g = m_reg;
    const ImGuiIO& io = ImGui::GetIO();
    if (m_engine->currentMapName().empty()) {
        ImGui::TextWrapped("%s", tr("Open a map first."));
        return;
    }
    const auto& images = m_engine->regionImages();
    if (images.empty()) {
        ImGui::TextWrapped("%s", tr("This map has no textured surfaces."));
        return;
    }
    ImGui::TextWrapped("%s", tr("Draw regions on a map texture and say how each one behaves. Saved with Ctrl+S."));
    ImGui::TextDisabled("%s", tr("Click a surface in the view to pick its texture and the region under the cursor."));

    // ---- imagem
    if (ImGui::BeginCombo(tr("Texture"), g.image.empty() ? tr("(choose)") : g.image.c_str())) {
        for (const auto& name : images) {
            const bool live = m_engine->regionLive(name);
            const std::string label = name + (live ? "" : std::string("  *"));
            if (ImGui::Selectable(label.c_str(), name == g.image)) regionOpenImage(name);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", tr("* The map must be reloaded before a region on this texture shows (its pixels are not available to the editor)."));

    // ---- hora do dia (vale para tudo: regioes, luzes noturnas)
    {
        float hour = m_engine->regionHour();
        if (ImGui::SliderFloat(tr("Time of day"), &hour, 0.0f, 24.0f, "%.1f h")) {
            m_engine->dayNightCycle().setTimeOfDay(std::clamp(hour, 0.0f, 23.999f) / 24.0f);
            m_engine->dayNightCycle().setPaused(true);
            g.overlayDirty = true;
        }
        bool paused = m_engine->dayNightCycle().isPaused();
        if (ImGui::Checkbox(tr("Freeze time"), &paused)) m_engine->dayNightCycle().setPaused(paused);
        ImGui::SetItemTooltip("%s", tr("Night is where the glow shows: try 22 h."));
    }

    if (g.image.empty()) return;
    if (g.rgba.empty()) {
        ImGui::TextWrapped("%s", tr("This texture's pixels are not available."));
        return;
    }
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 9.0f); // o rotulo fica a' direita
    texregion::ImageRegions& e = regionEdit();
    {
        bool on = e.enabled;
        if (ImGui::Checkbox(tr("Glow on this texture"), &on)) {
            e.enabled = on;
            regionCommit("Texture glow");
        }
    }

    // ---- ferramentas
    ImGui::SeparatorText(tr("Tools"));
    static int lastTool = -1;
    {
        // Tres por linha, cada um do mesmo tamanho: o painel e' estreito.
        const float cw = (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        for (int i = 0; i < ToolCount; ++i) {
            if (i % 3) ImGui::SameLine();
            if (ImGui::Selectable(tr(kToolNames[i]), g.tool == i, 0, ImVec2(cw, 0.0f))) g.tool = i;
        }
    }
    if (lastTool != g.tool) {
        g.draft.clear();
        g.livePath.clear();
        g.dragging = false;
        lastTool = g.tool;
    }
    int typeIdx = g.newType;
    {
        std::vector<const char*> labels;
        for (const char* l : kTypeLabels) labels.push_back(tr(l));
        ImGui::Combo(tr("New region type"), &typeIdx, labels.data(), int(labels.size()));
        g.newType = typeIdx;
    }
    if (g.tool == ToolBrush || g.tool == ToolEraser) {
        ImGui::SliderFloat(tr("Brush size"), &g.brush, 0.002f, 0.15f, "%.3f");
        ImGui::SetItemTooltip("%s", tr("Fraction of the texture width."));
    }
    if (g.tool == ToolWand) {
        ImGui::SliderInt(tr("Tolerance"), &g.wandTolerance, 0, 128);
        ImGui::Checkbox(tr("Contiguous only"), &g.wandContiguous);
        ImGui::SetItemTooltip("%s", tr("On: only pixels connected to the click. Off: every similar pixel in the texture."));
    }
    if (ImGui::Button(tr("Select all (Ctrl+A)"))) {
        e.regions.push_back(newRegion(g.newType));
        e.regions.back().shape = Shape::All;
        g.sel = int(e.regions.size()) - 1;
        regionCommit("Add region");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Preview glow"), &g.showGlow)) g.overlayDirty = true;

    // ---- tela
    const float availW = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const float aspect = float(g.h) / float(g.w);
    ImVec2 size(availW, availW * aspect);
    if (size.y > 460.0f) size = ImVec2(460.0f / aspect, 460.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 20, 255));
    dl->AddImage(reinterpret_cast<ImTextureID>(g.albedoTex.set), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                 g.showGlow ? IM_COL32(110, 110, 110, 255) : IM_COL32_WHITE);
    const float now = float(ImGui::GetTime());
    if (g.overlayDirty && (now - g.overlayAt > 0.12f || !g.overlayTex.valid())) regionRebuildOverlay();
    if (g.overlayTex.valid()) dl->AddImage(reinterpret_cast<ImTextureID>(g.overlayTex.set), p0, p1);
    ImGui::InvisibleButton("##regcanvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    auto toScreen = [&](float u, float v) { return ImVec2(p0.x + u * size.x, p0.y + v * size.y); };
    const float mu = std::clamp((io.MousePos.x - p0.x) / size.x, 0.0f, 1.0f);
    const float mv = std::clamp((io.MousePos.y - p0.y) / size.y, 0.0f, 1.0f);

    // contornos das regioes vetoriais, por cima
    auto polyline = [&](const std::vector<float>& pts, ImU32 col, bool closed, float thick) {
        std::vector<ImVec2> sp;
        for (size_t i = 0; i + 1 < pts.size(); i += 2) sp.push_back(toScreen(pts[i], pts[i + 1]));
        if (sp.size() >= 2) dl->AddPolyline(sp.data(), int(sp.size()), col, closed ? ImDrawFlags_Closed : 0, thick);
    };
    for (size_t k = 0; k < e.regions.size(); ++k) {
        const Region& r = e.regions[k];
        const ImU32 col = typeColor(r.type, int(k) == g.sel);
        if (r.shape == Shape::Poly) polyline(r.pts, col, true, 1.5f);
        else if (r.shape == Shape::Rect && r.pts.size() >= 4)
            dl->AddRect(toScreen(r.pts[0], r.pts[1]), toScreen(r.pts[2], r.pts[3]), col, 0.0f, 0, 1.5f);
        else if (r.shape == Shape::Ellipse && r.pts.size() >= 4) {
            std::vector<float> ring;
            for (int i = 0; i < 48; ++i) {
                const float a = float(i) / 48 * 6.2831853f;
                ring.push_back(r.pts[0] + std::cos(a) * r.pts[2]);
                ring.push_back(r.pts[1] + std::sin(a) * r.pts[3]);
            }
            polyline(ring, col, true, 1.5f);
        }
    }

    // ---- interacao com a tela
    const bool left = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    auto finishPolygon = [&](std::vector<float>& pts) {
        if (pts.size() >= 6) {
            Region r = newRegion(g.newType);
            r.shape = Shape::Poly;
            r.pts = pts;
            e.regions.push_back(std::move(r));
            g.sel = int(e.regions.size()) - 1;
            regionCommit("Add region");
        }
        pts.clear();
        g.livePath.clear();
    };
    if (hovered && !io.WantTextInput) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            e.regions.push_back(newRegion(g.newType));
            e.regions.back().shape = Shape::All;
            g.sel = int(e.regions.size()) - 1;
            regionCommit("Add region");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && g.sel >= 0 && g.sel < int(e.regions.size())) {
            e.regions.erase(e.regions.begin() + g.sel);
            g.sel = -1;
            regionCommit("Delete region");
        }
    }
    if (hovered && !io.WantTextInput && (g.tool == ToolPoly || g.tool == ToolLasso)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) finishPolygon(g.draft);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g.draft.clear(); g.livePath.clear(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && g.draft.size() >= 2) {
            g.draft.resize(g.draft.size() - 2);
            g.wireCell = -1;
        }
    }
    switch (g.tool) {
        case ToolPoly:
            if (hovered && left) {
                // Perto do primeiro ponto fecha.
                if (g.draft.size() >= 6 && std::hypot((g.draft[0] - mu) * size.x, (g.draft[1] - mv) * size.y) < 8.0f)
                    finishPolygon(g.draft);
                else { g.draft.push_back(mu); g.draft.push_back(mv); }
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) finishPolygon(g.draft);
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && g.draft.size() >= 2) g.draft.resize(g.draft.size() - 2);
            break;
        case ToolRect:
        case ToolEllipse:
            if (hovered && left) {
                g.dragging = true;
                g.drag[0] = g.drag[2] = mu;
                g.drag[1] = g.drag[3] = mv;
            }
            if (g.dragging && leftDown) { g.drag[2] = mu; g.drag[3] = mv; }
            if (g.dragging && released) {
                g.dragging = false;
                const float u0 = std::min(g.drag[0], g.drag[2]), u1 = std::max(g.drag[0], g.drag[2]);
                const float v0 = std::min(g.drag[1], g.drag[3]), v1 = std::max(g.drag[1], g.drag[3]);
                if ((u1 - u0) * size.x > 3.0f && (v1 - v0) * size.y > 3.0f) {
                    Region r = newRegion(g.newType);
                    if (g.tool == ToolRect) {
                        r.shape = Shape::Rect;
                        r.pts = {u0, v0, u1, v1};
                    } else {
                        r.shape = Shape::Ellipse;
                        r.pts = {(u0 + u1) * 0.5f, (v0 + v1) * 0.5f, (u1 - u0) * 0.5f, (v1 - v0) * 0.5f};
                    }
                    e.regions.push_back(std::move(r));
                    g.sel = int(e.regions.size()) - 1;
                    regionCommit("Add region");
                }
            }
            if (g.dragging) {
                const ImVec2 a = toScreen(g.drag[0], g.drag[1]), b = toScreen(g.drag[2], g.drag[3]);
                if (g.tool == ToolRect) dl->AddRect(a, b, IM_COL32(255, 255, 255, 255));
                else {
                    std::vector<float> ring;
                    for (int i = 0; i < 48; ++i) {
                        const float t = float(i) / 48 * 6.2831853f;
                        ring.push_back((g.drag[0] + g.drag[2]) * 0.5f + std::cos(t) * (g.drag[2] - g.drag[0]) * 0.5f);
                        ring.push_back((g.drag[1] + g.drag[3]) * 0.5f + std::sin(t) * (g.drag[3] - g.drag[1]) * 0.5f);
                    }
                    polyline(ring, IM_COL32(255, 255, 255, 255), true, 1.0f);
                }
            }
            break;
        case ToolBrush:
        case ToolEraser: {
            const bool add = g.tool == ToolBrush;
            if (hovered && left && (add || (g.sel >= 0 && g.sel < int(e.regions.size())))) {
                if (g.sel < 0 || g.sel >= int(e.regions.size())) {
                    Region r = newRegion(g.newType);
                    r.shape = Shape::Bitmap;
                    r.bw = g.w;
                    r.bh = g.h;
                    r.bitmap.assign(size_t(g.w) * g.h, 0);
                    e.regions.push_back(std::move(r));
                    g.sel = int(e.regions.size()) - 1;
                }
                texregion::toBitmap(e.regions[size_t(g.sel)], g.w, g.h);
                g.stroking = true;
                g.lastU = mu;
                g.lastV = mv;
            }
            if (g.stroking && leftDown && g.sel >= 0 && g.sel < int(e.regions.size())) {
                Region& r = e.regions[size_t(g.sel)];
                texregion::strokeLine(r, g.lastU * r.bw, g.lastV * r.bh, mu * r.bw, mv * r.bh, g.brush * r.bw, add);
                g.lastU = mu;
                g.lastV = mv;
                g.overlayDirty = true;
            }
            if (g.stroking && released) {
                g.stroking = false;
                regionCommit(add ? "Paint region" : "Erase region");
            }
            if (hovered)
                dl->AddCircle(ImVec2(io.MousePos.x, io.MousePos.y), g.brush * size.x, IM_COL32(255, 255, 255, 220), 32);
            break;
        }
        case ToolLasso: {
            if (hovered && g.wire.ready()) {
                const int cell = int(mu * 512) * 4096 + int(mv * 512);
                if (!g.draft.empty() && cell != g.wireCell) {
                    g.wireCell = cell;
                    g.livePath = g.wire.pathTo(mu, mv);
                }
            }
            if (hovered && left) {
                if (g.draft.size() >= 6 && std::hypot((g.draft[0] - mu) * size.x, (g.draft[1] - mv) * size.y) < 8.0f) {
                    finishPolygon(g.draft);
                } else {
                    if (g.draft.empty()) {
                        g.draft = {mu, mv};
                    } else {
                        // O trecho vivo vira parte do contorno.
                        const std::vector<float> seg = g.wire.pathTo(mu, mv);
                        g.draft.insert(g.draft.end(), seg.begin(), seg.end());
                    }
                    g.wire.setSeed(mu, mv);
                    g.wireCell = -1;
                    g.livePath.clear();
                }
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) finishPolygon(g.draft);
            break;
        }
        case ToolWand:
            if (hovered && left) {
                Region r = newRegion(g.newType);
                texregion::wandFill(g.rgba.data(), g.w, g.h, 4, int(mu * g.w), int(mv * g.h), g.wandTolerance, g.wandContiguous, r);
                e.regions.push_back(std::move(r));
                g.sel = int(e.regions.size()) - 1;
                regionCommit("Add region");
            }
            break;
        case ToolSelect:
            if (hovered && left) {
                int hit = -1;
                std::vector<float> cov;
                for (int k = int(e.regions.size()) - 1; k >= 0 && hit < 0; --k) {
                    texregion::rasterize(e.regions[size_t(k)], 128, 128, cov);
                    if (cov[size_t(std::min(127, int(mv * 128))) * 128 + size_t(std::min(127, int(mu * 128)))] > 0.5f) hit = k;
                }
                g.sel = hit;
                g.overlayDirty = true;
            }
            break;
    }
    if (g.hasPick) {
        // Onde o clique no viewport caiu na textura.
        const ImVec2 c = toScreen(g.pickU, g.pickV);
        dl->AddLine(ImVec2(c.x - 7, c.y), ImVec2(c.x + 7, c.y), IM_COL32(255, 60, 60, 255), 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - 7), ImVec2(c.x, c.y + 7), IM_COL32(255, 60, 60, 255), 1.5f);
    }
    // rascunho (poligono / laco)
    if (!g.draft.empty()) {
        polyline(g.draft, IM_COL32(255, 255, 255, 255), false, 1.5f);
        if (!g.livePath.empty()) polyline(g.livePath, IM_COL32(60, 210, 255, 255), false, 1.5f);
        else if (g.tool == ToolPoly && hovered) {
            std::vector<float> rubber = {g.draft[g.draft.size() - 2], g.draft[g.draft.size() - 1], mu, mv};
            polyline(rubber, IM_COL32(255, 255, 255, 140), false, 1.0f);
        }
        dl->AddCircle(toScreen(g.draft[0], g.draft[1]), 5.0f, IM_COL32(255, 255, 255, 255));
    }
    if (g.tool == ToolPoly || g.tool == ToolLasso)
        ImGui::TextDisabled("%s", tr("Click to add points. Enter, double-click or the first point closes. Backspace undoes a point."));
    else if (g.tool == ToolBrush || g.tool == ToolEraser)
        ImGui::TextDisabled("%s", tr("Drag to paint (or erase) the selected region."));

    // ---- lista de regioes e propriedades
    ImGui::SeparatorText(tr("Regions"));
    for (int k = 0; k < int(e.regions.size()); ++k) {
        const Region& r = e.regions[size_t(k)];
        const int ti = typeIndex(r.type);
        std::string label = std::string(ti >= 0 ? tr(kTypeLabels[ti]) : r.type.c_str()) + "  (" +
                            (r.shape == Shape::Poly      ? tr("polygon")
                             : r.shape == Shape::Rect    ? tr("rectangle")
                             : r.shape == Shape::Ellipse ? tr("ellipse")
                             : r.shape == Shape::All     ? tr("whole texture")
                                                         : tr("painted")) + ")###reg" + std::to_string(k);
        if (ImGui::Selectable(label.c_str(), g.sel == k)) {
            g.sel = k;
            g.overlayDirty = true;
        }
    }
    if (e.regions.empty()) ImGui::TextDisabled("%s", tr("No regions yet."));
    if (g.sel >= 0 && g.sel < int(e.regions.size())) {
        Region& r = e.regions[size_t(g.sel)];
        bool liveEdit = false;
        // Cada controle: muda na hora (previa) e entra no desfazer ao soltar.
        auto done = [&](bool changed) {
            liveEdit |= changed;
            if (ImGui::IsItemDeactivatedAfterEdit()) regionCommit("Edit region");
        };
        if (ImGui::Button(tr("Delete region"))) {
            e.regions.erase(e.regions.begin() + g.sel);
            g.sel = -1;
            regionCommit("Delete region");
        } else {
            int ti = typeIndex(r.type);
            std::vector<const char*> labels;
            for (const char* l : kTypeLabels) labels.push_back(tr(l));
            if (ti >= 0 && ImGui::Combo(tr("Type"), &ti, labels.data(), int(labels.size()))) {
                r.type = kTypeIds[ti];
                regionCommit("Edit region");
            }
            if (isGlowType(r.type)) {
                done(ImGui::ColorEdit3(tr("Color"), r.color));
                done(ImGui::SliderFloat(tr("Intensity"), &r.intensity, 0.0f, 1.0f, "%.2f"));
                int gm = int(r.glow);
                std::vector<const char*> gl;
                for (const char* l : kGlowLabels) gl.push_back(tr(l));
                if (ImGui::Combo(tr("Glow mode"), &gm, gl.data(), int(gl.size()))) {
                    r.glow = Glow(gm);
                    regionCommit("Edit region");
                }
                bool sched = r.onHour >= 0.0f && r.offHour >= 0.0f;
                if (ImGui::Checkbox(tr("Own schedule"), &sched)) {
                    r.onHour = sched ? 18.0f : -1.0f;
                    r.offHour = sched ? 6.0f : -1.0f;
                    regionCommit("Edit region");
                }
                ImGui::SetItemTooltip("%s", tr("Off: lit with the engine's night curve (follows the sun)."));
                if (sched) {
                    done(ImGui::SliderFloat(tr("Turns on at"), &r.onHour, 0.0f, 24.0f, "%.1f h"));
                    done(ImGui::SliderFloat(tr("Turns off at"), &r.offHour, 0.0f, 24.0f, "%.1f h"));
                }
                done(ImGui::SliderFloat(tr("Flicker"), &r.flicker, 0.0f, 1.0f, "%.2f"));
                ImGui::SetItemTooltip("%s", tr("Light and glow of the texture flicker together (one factor per texture)."));
                if (r.flicker > 0.0f) done(ImGui::SliderFloat(tr("Flicker speed (Hz)"), &r.flickerSpeed, 0.5f, 20.0f, "%.1f"));
                if (r.type == kTypeLamp) {
                    done(ImGui::DragFloat(tr("Light radius"), &r.lightRadius, 0.5f, 1.0f, 500.0f, "%.0f"));
                    done(ImGui::DragFloat(tr("Light intensity"), &r.lightIntensity, 0.05f, 0.0f, 50.0f, "%.2f"));
                    done(ImGui::DragFloat(tr("Object glow radius"), &r.glowRadius, 0.1f, 0.0f, 100.0f, "%.1f"));
                    ImGui::SetItemTooltip("%s", tr("Surfaces within this radius of the light glow with its color (0 = off)."));
                    if (r.glowRadius > 0.0f) done(ImGui::DragFloat(tr("Object glow strength"), &r.glowStrength, 0.01f, 0.0f, 10.0f, "%.2f"));
                    if (ImGui::Button(tr("Add world light with these values"))) regionAddLight(r);
                    ImGui::SetItemTooltip("%s", tr("Creates a night light in the middle of the view. Move it with the Move tool."));
                }
            } else {
                ImGui::TextWrapped("%s", tr("Surface type: characters.on_step gives it to the game as ground.surface, so a script picks the footprint or dust."));
            }
        }
        // Previa na hora enquanto o controle e' arrastado (no maximo 10x por segundo).
        static float lastLive = 0.0f;
        if (liveEdit) {
            g.overlayDirty = true;
            if (now - lastLive > 0.1f) {
                m_engine->refreshRegionEmissive(g.image);
                lastLive = now;
            }
        }
    }

    // ---- nos do mapa
    if (ImGui::CollapsingHeader(tr("Nodes of the map"))) {
        ImGui::TextDisabled("%s", tr("Untick a node to switch its glow off."));
        const auto& off = m_engine->emissiveOffNodes();
        for (const auto& n : m_engine->regionNodes()) {
            bool on = std::find(off.begin(), off.end(), n) == off.end();
            if (ImGui::Checkbox((n + "##node").c_str(), &on)) {
                Engine* engine = m_engine;
                engine->setRegionNodeEmissive(n, on);
                m_undo.push({"Node glow", [engine, n, on] { engine->setRegionNodeEmissive(n, !on); },
                             [engine, n, on] { engine->setRegionNodeEmissive(n, on); }});
                updateTitle(true);
            }
        }
    }
    if (ImGui::Button(tr("Save (Ctrl+S)"))) saveScene();
    ImGui::PopItemWidth();
}

} // namespace eruption
