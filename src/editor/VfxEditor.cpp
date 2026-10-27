#include "editor/VfxEditor.hpp"

#include "core/Engine.hpp"
#include "core/Project.hpp"
#include "editor/EditorTheme.hpp"
#include "editor/Translate.hpp"
#include "renderer/vfx/VfxBuiltins.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace eruption {

namespace fs = std::filesystem;

namespace {

const char* kShapeNames[] = {"Point", "Sphere", "Box", "Cone", "Disc"};
const char* kFacingNames[] = {"Camera", "Velocity", "Horizontal", "Ribbon (trail)"};
const char* kBlendNames[] = {"Additive", "Premultiplied", "Alpha"};
const char* kFlipNames[] = {"Over lifetime", "Frames per second", "Random fixed frame"};

template <size_t N>
bool comboEnum(const char* label, uint32_t& v, const char* (&names)[N]) {
    bool changed = false;
    if (ImGui::BeginCombo(label, tr(names[std::min<size_t>(v, N - 1)]))) {
        for (size_t i = 0; i < N; ++i)
            if (ImGui::Selectable(tr(names[i]), v == i)) {
                v = static_cast<uint32_t>(i);
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

// Faixa sorteada por particula (min/max).
bool range(const char* label, Vec2& v, float speed, float lo, float hi, const char* fmt = "%.2f") {
    return ImGui::DragFloat2(label, &v.x, speed, lo, hi, fmt);
}

ImU32 toU32(const Vec4& c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f),
                                                 std::clamp(c.b, 0.0f, 1.0f), std::clamp(c.a, 0.0f, 1.0f)));
}

VfxEffectDesc defaultEffect(const std::string& name) {
    VfxEffectDesc fx;
    fx.name = name;
    VfxEmitterDesc e;
    e.name = "emissor";
    e.texture = "builtin:spark";
    e.color.keys = {{0.0f, Vec4(1.0f, 0.8f, 0.4f, 0.0f)}, {0.2f, Vec4(1.0f, 0.6f, 0.2f, 1.0f)},
                    {1.0f, Vec4(0.8f, 0.2f, 0.05f, 0.0f)}};
    e.emissive = 2.0f;
    fx.emitters.push_back(e);
    return fx;
}

} // namespace

void VfxEditor::init(Engine* engine) {
    m_engine = engine;
    m_previewReady = m_preview.init(*engine);
    if (m_previewReady) m_preview.frame();
    m_graphEditor.init();
}

void VfxEditor::shutdown() {
    playInScene(false);
    if (m_previewReady) m_preview.shutdown();
    m_previewReady = false;
    m_graphEditor.shutdown();
}

void VfxEditor::renderPreview(VkCommandBuffer cmd, float dt) {
    if (m_visible && m_previewReady && !m_name.empty()) m_preview.render(cmd, dt);
}

void VfxEditor::refreshLibrary() {
    m_library.clear();
    auto scan = [&](const fs::path& dir, bool engineTemplate) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;
        for (const auto& it : fs::directory_iterator(dir, ec)) {
            const std::string fn = it.path().filename().string();
            const std::string ext = ".vfx.json";
            if (fn.size() <= ext.size() || fn.compare(fn.size() - ext.size(), ext.size(), ext) != 0) continue;
            m_library.push_back({fn.substr(0, fn.size() - ext.size()), it.path().string(), engineTemplate});
        }
    };
    scan(project::game("assets/effects"), false);
    // Com projeto aberto, os modelos do motor continuam disponiveis para abrir
    // e salvar como efeito do jogo.
    if (project::isOpen() || project::game("assets/effects") != project::engine("data/vfx"))
        scan(project::engine("data/vfx"), true);
    std::sort(m_library.begin(), m_library.end(), [](const LibraryItem& a, const LibraryItem& b) {
        return a.engineTemplate != b.engineTemplate ? !a.engineTemplate : a.name < b.name;
    });
}

bool VfxEditor::openEffect(const std::string& nameOrPath) {
    refreshLibrary();
    std::string path = nameOrPath;
    std::string name = nameOrPath;
    if (!fs::exists(path)) {
        path.clear();
        for (const auto& it : m_library)
            if (it.name == nameOrPath) {
                path = it.path;
                break;
            }
    } else {
        name = fs::path(path).filename().string();
        const size_t dot = name.find(".vfx.json");
        if (dot != std::string::npos) name = name.substr(0, dot);
    }
    VfxEffectDesc fx;
    std::string err;
    if (path.empty() || !loadVfxEffect(path, fx, &err)) {
        m_status = std::string(tr("Could not open effect: ")) + (err.empty() ? nameOrPath : err);
        return false;
    }
    playInScene(false);
    m_fx = fx;
    m_name = name;
    m_path = path;
    m_sel = 0;
    m_gradKey = 0;
    m_dirty = false;
    m_undo.clear();
    m_redo.clear();
    m_snapshot = vfxEffectToJson(m_fx);
    m_saved = m_snapshot;
    m_visible = true;
    std::snprintf(m_saveAsName, sizeof(m_saveAsName), "%s", m_name.c_str());
    if (m_previewReady) m_preview.play(m_name, m_fx, true);
    m_status.clear();
    return true;
}

void VfxEditor::newEffect() {
    playInScene(false);
    m_name = "novo_efeito";
    m_path.clear();
    m_fx = defaultEffect(m_name);
    m_sel = 0;
    m_dirty = true;
    m_undo.clear();
    m_redo.clear();
    m_snapshot = vfxEffectToJson(m_fx);
    std::snprintf(m_saveAsName, sizeof(m_saveAsName), "%s", m_name.c_str());
    if (m_previewReady) m_preview.play(m_name, m_fx, true);
}

bool VfxEditor::save(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string err;
    m_fx.name = m_name;
    if (!saveVfxEffect(path, m_fx, &err)) {
        m_status = std::string(tr("Could not save: ")) + err;
        return false;
    }
    m_path = path;
    m_dirty = false;
    m_saved = m_snapshot = vfxEffectToJson(m_fx);
    // O sistema da cena passa a usar a versao salva (efeitos ja' tocando).
    m_engine->vfx().setEffect(m_name, m_fx);
    m_status = std::string(tr("Saved: ")) + path;
    refreshLibrary();
    return true;
}

void VfxEditor::applyLive() {
    if (m_previewReady) m_preview.play(m_name, m_fx, false);
    if (m_sceneInstance) m_engine->vfx().setEffect(m_name, m_fx);
}

void VfxEditor::trackUndo() {
    // Um passo de desfazer por gesto: so' fecha o retrato quando nenhum
    // controle (nem no do grafo) esta' sendo arrastado.
    if (ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    std::string now = vfxEffectToJson(m_fx);
    if (now == m_snapshot) return;
    m_undo.push_back(std::move(m_snapshot));
    if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_snapshot = std::move(now);
    m_dirty = m_snapshot != m_saved;
}

void VfxEditor::undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(m_snapshot);
    m_snapshot = m_undo.back();
    m_undo.pop_back();
    vfxEffectFromJson(m_snapshot, m_fx);
    m_sel = std::min<int>(m_sel, int(m_fx.emitters.size()) - 1);
    m_dirty = m_snapshot != m_saved;
    applyLive();
}

void VfxEditor::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(m_snapshot);
    m_snapshot = m_redo.back();
    m_redo.pop_back();
    vfxEffectFromJson(m_snapshot, m_fx);
    m_sel = std::min<int>(m_sel, int(m_fx.emitters.size()) - 1);
    m_dirty = m_snapshot != m_saved;
    applyLive();
}

void VfxEditor::playInScene(bool on) {
    if (!m_engine) return;
    VfxSystem& sys = m_engine->vfx();
    if (m_sceneInstance) {
        sys.stop(m_sceneInstance, true);
        m_sceneInstance = 0;
    }
    if (!on || m_name.empty()) return;
    // Na frente da camera do editor, no alvo dela.
    sys.setEffect(m_name, m_fx);
    const Vec3 at = m_engine->camera().target();
    m_sceneInstance = sys.spawn(m_name, glm::translate(Mat4(1.0f), at));
}

// ---------------------------------------------------------------------------
// Janela
// ---------------------------------------------------------------------------

void VfxEditor::draw(bool* open) {
    if (!*open) {
        if (m_visible) playInScene(false);
        m_visible = false;
        return;
    }
    m_visible = true;
    if (m_library.empty()) refreshLibrary();
    if (m_name.empty() && !m_library.empty()) openEffect(m_library.front().name);
    ImGui::SetNextWindowSize(ImVec2(1100, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Effects"), open, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }
    // Atalhos com a janela focada.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) io.KeyShift ? redo() : undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !m_path.empty()) save(m_path);
    }
    if (ImGui::BeginMenuBar()) {
        if (ImGui::MenuItem(tr("New"))) newEffect();
        if (ImGui::MenuItem(tr("Save"), "Ctrl+S", false, !m_path.empty())) save(m_path);
        if (ImGui::BeginMenu(tr("Save as"))) {
            ImGui::InputText("##saveas", m_saveAsName, sizeof(m_saveAsName));
            if (ImGui::Button(tr("Save in the game (assets/effects)")) && m_saveAsName[0]) {
                m_name = m_saveAsName;
                save(project::game(std::string("assets/effects/") + m_saveAsName + ".vfx.json").string());
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(tr("Undo"), "Ctrl+Z", false, !m_undo.empty())) undo();
        if (ImGui::MenuItem(tr("Redo"), "Ctrl+Y", false, !m_redo.empty())) redo();
        ImGui::Separator();
        bool inScene = m_sceneInstance != 0;
        if (ImGui::MenuItem(tr("Play in scene"), nullptr, inScene)) playInScene(!inScene);
        ImGui::EndMenuBar();
    }

    const float h = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##lib", ImVec2(190, h), true);
    drawLibrary();
    ImGui::EndChild();
    ImGui::SameLine();
    const float rightW = 380.0f;
    ImGui::BeginChild("##prev", ImVec2(std::max(200.0f, ImGui::GetContentRegionAvail().x - rightW - 8), h), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawPreviewPane();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##stack", ImVec2(0, h), true);
    drawStack();
    ImGui::EndChild();
    ImGui::TextDisabled("%s%s%s", m_path.empty() ? tr("(not saved)") : m_path.c_str(), m_dirty ? " *" : "",
                        m_status.empty() ? "" : ("   " + m_status).c_str());
    ImGui::End();

    // Janela do grafo do emissor selecionado.
    if (m_showGraph && m_sel >= 0 && m_sel < int(m_fx.emitters.size())) {
        VfxEmitterDesc& e = m_fx.emitters[size_t(m_sel)];
        const std::string key = m_name + "#" + std::to_string(m_sel);
        if (m_graphEditor.draw(trw("Effect Graph"), key, e.graph, &m_showGraph)) applyLive();
    }

    trackUndo();
}

void VfxEditor::drawLibrary() {
    ImGui::TextColored(kAccent, "%s", tr("Effects"));
    if (ImGui::SmallButton(tr("Refresh"))) refreshLibrary();
    bool templates = false;
    for (const auto& it : m_library) {
        if (it.engineTemplate && !templates) {
            templates = true;
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("Engine templates"));
        }
        if (ImGui::Selectable(it.name.c_str(), it.path == m_path)) openEffect(it.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.path.c_str());
    }
}

void VfxEditor::drawPreviewPane() {
    if (!m_previewReady) {
        ImGui::TextDisabled("%s", tr("Preview unavailable"));
        return;
    }
    // Barra: tocar/pausar, recomecar, velocidade, enquadrar.
    if (ImGui::Button(m_preview.playing ? tr("Pause") : tr("Play"))) m_preview.playing = !m_preview.playing;
    ImGui::SameLine();
    if (ImGui::Button(tr("Restart"))) m_preview.restart();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("##speed", &m_preview.speed, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    if (ImGui::Button(tr("Frame (F)"))) m_preview.frame();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t w = static_cast<uint32_t>(std::max(64.0f, avail.x));
    const uint32_t hh = static_cast<uint32_t>(std::max(64.0f, avail.y - ImGui::GetTextLineHeightWithSpacing()));
    m_preview.setSize(w, hh);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (m_preview.texture()) ImGui::Image(m_preview.texture(), ImVec2(float(w), float(hh)));
    else ImGui::Dummy(ImVec2(float(w), float(hh)));
    // Entrada sobre a imagem (botao invisivel: arrastar nao move a janela).
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##prevInput", ImVec2(float(w), float(hh)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    if (ImGui::IsItemActive()) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_preview.orbit(d.x, d.y);
        else m_preview.pan(d.x, d.y);
    }
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) m_preview.zoom(wheel);
        if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_preview.frame();
    }
    const auto& st = m_preview.stats();
    ImGui::TextDisabled(tr("%.1f s | %u alive | %u drawn | %u emitters"), m_preview.effectTime(), st.alive,
                        st.rendered, st.emitters);
}

void VfxEditor::drawStack() {
    if (m_name.empty()) return;
    bool changed = false;
    // Efeito
    ImGui::TextColored(kAccent, "%s", m_name.c_str());
    changed |= ImGui::DragFloat(tr("Duration (0 = endless)"), &m_fx.duration, 0.05f, 0.0f, 600.0f, "%.2f s");
    if (m_fx.duration > 0.0f) changed |= ImGui::Checkbox(tr("Loop"), &m_fx.loop);

    // Emissores
    ImGui::SeparatorText(tr("Emitters"));
    for (int i = 0; i < int(m_fx.emitters.size()); ++i) {
        ImGui::PushID(i);
        VfxEmitterDesc& e = m_fx.emitters[size_t(i)];
        changed |= ImGui::Checkbox("##on", &e.enabled);
        ImGui::SameLine();
        if (ImGui::Selectable(e.name.c_str(), m_sel == i)) m_sel = i;
        ImGui::PopID();
    }
    if (ImGui::SmallButton(tr("+ Emitter"))) {
        VfxEmitterDesc e = defaultEffect("x").emitters[0];
        e.name = "emissor" + std::to_string(m_fx.emitters.size() + 1);
        m_fx.emitters.push_back(e);
        m_sel = int(m_fx.emitters.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Duplicate")) && m_sel >= 0 && m_sel < int(m_fx.emitters.size())) {
        VfxEmitterDesc e = m_fx.emitters[size_t(m_sel)];
        e.name += "_copia";
        m_fx.emitters.insert(m_fx.emitters.begin() + m_sel + 1, e);
        ++m_sel;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Remove")) && m_fx.emitters.size() > 1) {
        m_fx.emitters.erase(m_fx.emitters.begin() + m_sel);
        m_sel = std::min<int>(m_sel, int(m_fx.emitters.size()) - 1);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Up")) && m_sel > 0) {
        std::swap(m_fx.emitters[size_t(m_sel)], m_fx.emitters[size_t(m_sel - 1)]);
        --m_sel;
        changed = true;
    }
    if (m_sel >= 0 && m_sel < int(m_fx.emitters.size())) {
        ImGui::Separator();
        changed |= drawEmitter(m_fx.emitters[size_t(m_sel)]);
    }
    if (changed) applyLive();
}

bool VfxEditor::drawEmitter(VfxEmitterDesc& e) {
    bool c = false;
    ImGui::PushItemWidth(-140);
    char nm[64];
    std::snprintf(nm, sizeof(nm), "%s", e.name.c_str());
    if (ImGui::InputText(tr("Name"), nm, sizeof(nm))) {
        e.name = nm;
        c = true;
    }
    int cap = int(e.capacity);
    if (ImGui::DragInt(tr("Max alive"), &cap, 10.0f, 1, 1 << 20)) {
        e.capacity = uint32_t(std::max(1, cap));
        c = true;
    }
    c |= ImGui::Checkbox(tr("Half resolution (smoke, dust)"), &e.lowRes);
    c |= ImGui::Checkbox(tr("Decal (projected on the ground)"), &e.decal);
    // Malha no lugar do sprite: opaca, iluminada pela cena (detritos, pedras).
    if (ImGui::BeginCombo(tr("Mesh"), e.mesh.empty() ? tr("None (sprite)") : e.mesh.c_str())) {
        if (ImGui::Selectable(tr("None (sprite)"), e.mesh.empty())) {
            e.mesh.clear();
            c = true;
        }
        for (const std::string& m : vfxBuiltinMeshNames())
            if (ImGui::Selectable(m.c_str(), e.mesh == m)) {
                e.mesh = m;
                c = true;
            }
        ImGui::EndCombo();
    }

    if (ImGui::CollapsingHeader(tr("Spawning"), ImGuiTreeNodeFlags_DefaultOpen)) {
        c |= ImGui::DragFloat(tr("Rate (per s)"), &e.rate, 0.5f, 0.0f, 100000.0f, "%.1f");
        c |= ImGui::DragFloat(tr("Per meter moved"), &e.ratePerMeter, 0.1f, 0.0f, 10000.0f, "%.1f");
        int burst = int(e.burstCount);
        if (ImGui::DragInt(tr("Burst"), &burst, 1.0f, 0, 100000)) {
            e.burstCount = uint32_t(std::max(0, burst));
            c = true;
        }
        c |= ImGui::DragFloat(tr("Repeat burst every (s)"), &e.burstInterval, 0.02f, 0.0f, 600.0f, "%.2f");
        c |= ImGui::DragFloat(tr("Start delay (s)"), &e.startDelay, 0.02f, 0.0f, 600.0f, "%.2f");
        c |= ImGui::DragFloat(tr("Emit for (s, 0 = always)"), &e.duration, 0.02f, 0.0f, 600.0f, "%.2f");
    }
    if (ImGui::CollapsingHeader(tr("Shape"), ImGuiTreeNodeFlags_DefaultOpen)) {
        uint32_t s = uint32_t(e.shape);
        if (comboEnum(tr("Shape##shape"), s, kShapeNames)) {
            e.shape = VfxShape(s);
            c = true;
        }
        c |= ImGui::DragFloat3(tr("Size (m)"), &e.shapeSize.x, 0.02f, 0.0f, 1000.0f, "%.2f");
        c |= ImGui::Checkbox(tr("Surface only"), &e.surfaceOnly);
        if (e.shape == VfxShape::Cone) c |= ImGui::DragFloat(tr("Cone angle"), &e.coneAngle, 0.5f, 0.0f, 179.0f, "%.0f deg");
    }
    if (ImGui::CollapsingHeader(tr("Initial"), ImGuiTreeNodeFlags_DefaultOpen)) {
        c |= range(tr("Lifetime (s)"), e.lifetime, 0.02f, 0.01f, 600.0f);
        c |= range(tr("Speed (m/s)"), e.speed, 0.05f, -1000.0f, 1000.0f);
        c |= ImGui::DragFloat3(tr("Direction"), &e.direction.x, 0.01f, -1.0f, 1.0f, "%.2f");
        c |= ImGui::SliderFloat(tr("Spread"), &e.spread, 0.0f, 1.0f, "%.2f");
        c |= range(tr("Size (m)##ps"), e.size, 0.01f, 0.0f, 1000.0f, "%.3f");
        c |= range(tr("Rotation (deg)"), e.rotation, 1.0f, -3600.0f, 3600.0f, "%.0f");
        c |= range(tr("Spin (deg/s)"), e.angularVelocity, 1.0f, -3600.0f, 3600.0f, "%.0f");
        c |= ImGui::SliderFloat(tr("Inherit velocity"), &e.inheritVelocity, 0.0f, 1.0f, "%.2f");
    }
    if (ImGui::CollapsingHeader(tr("Motion"), ImGuiTreeNodeFlags_DefaultOpen)) {
        c |= ImGui::DragFloat3(tr("Gravity (m/s2)"), &e.gravity.x, 0.05f, -100.0f, 100.0f, "%.2f");
        c |= ImGui::DragFloat(tr("Drag"), &e.drag, 0.01f, 0.0f, 50.0f, "%.2f");
        c |= ImGui::DragFloat(tr("Turbulence"), &e.noiseStrength, 0.05f, 0.0f, 500.0f, "%.2f");
        c |= ImGui::DragFloat(tr("Turbulence scale (1/m)"), &e.noiseFrequency, 0.005f, 0.0f, 50.0f, "%.3f");
        c |= ImGui::DragFloat(tr("Turbulence speed"), &e.noiseSpeed, 0.01f, 0.0f, 50.0f, "%.2f");
    }
    if (ImGui::CollapsingHeader(tr("Collision"))) {
        c |= ImGui::Checkbox(tr("Horizontal plane"), &e.collidePlane);
        if (e.collidePlane) c |= ImGui::DragFloat(tr("Plane height (world)"), &e.planeY, 0.5f);
        c |= ImGui::Checkbox(tr("What the camera sees (depth)"), &e.collideDepth);
        c |= ImGui::SliderFloat(tr("Bounce"), &e.bounce, 0.0f, 1.0f, "%.2f");
        c |= ImGui::SliderFloat(tr("Friction"), &e.friction, 0.0f, 1.0f, "%.2f");
        c |= ImGui::SliderFloat(tr("Life lost per hit"), &e.collisionLifeLoss, 0.0f, 1.0f, "%.2f");
    }
    if (ImGui::CollapsingHeader(tr("Over lifetime"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted(tr("Color and opacity"));
        c |= gradientEditor("##grad", e.color);
        ImGui::TextUnformatted(tr("Size multiplier"));
        c |= curveEditor("##size", e.sizeOverLife);
    }
    if (ImGui::CollapsingHeader(tr("Light"))) {
        c |= ImGui::Checkbox(tr("Lights the scene (one light per emitter)"), &e.light);
        if (e.light) {
            c |= ImGui::ColorEdit3(tr("Light color"), &e.lightColor.x);
            c |= ImGui::DragFloat(tr("Light intensity"), &e.lightIntensity, 0.05f, 0.0f, 50.0f);
            c |= ImGui::DragFloat(tr("Light radius (m)"), &e.lightRadius, 0.05f, 0.1f, 100.0f);
            c |= ImGui::SliderFloat(tr("Flicker"), &e.lightFlicker, 0.0f, 1.0f);
            c |= ImGui::DragFloat3(tr("Light offset (m)"), &e.lightOffset.x, 0.02f);
        }
    }
    if (ImGui::CollapsingHeader(tr("Sub-emitters"))) {
        c |= subEmitterUi(tr("On death"), e.onDeath, e.name);
        c |= subEmitterUi(tr("On collision"), e.onCollision, e.name);
        ImGui::TextDisabled("%s", tr("The child is another emitter of this effect (usually with rate 0)."));
    }
    if (ImGui::CollapsingHeader(tr("Graph"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (e.graph.empty()) ImGui::TextDisabled("%s", tr("No graph: only the modules above."));
        else ImGui::TextDisabled(tr("%zu nodes, %zu links"), e.graph.nodes.size(), e.graph.links.size());
        if (ImGui::Button(e.graph.empty() ? tr("Create graph") : tr("Open graph"))) {
            if (e.graph.empty()) {
                // Ponto de partida: forca = turbulencia(posicao, tempo) x 0.
                VfxGraph& g = e.graph;
                g = VfxGraph{};
                g.nodes.push_back({1, "in_position", Vec4(0.0f), Vec2(0.0f, 0.0f)});
                g.nodes.push_back({2, "in_time", Vec4(0.0f), Vec2(0.0f, 90.0f)});
                g.nodes.push_back({3, "noise", Vec4(0.0f), Vec2(200.0f, 30.0f)});
                g.nodes.push_back({4, "mul", Vec4(0.0f, 0.0f, 0.0f, 0.0f), Vec2(380.0f, 30.0f)});
                g.nodes.push_back({5, "out_force", Vec4(0.0f), Vec2(560.0f, 30.0f)});
                g.links = {{1, 0, 3, 0}, {2, 0, 3, 1}, {3, 0, 4, 0}, {4, 0, 5, 0}};
                g.nextId = 6;
                c = true;
            }
            m_showGraph = true;
        }
        if (!e.graph.empty()) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Remove graph"))) {
                e.graph = VfxGraph{};
                c = true;
            }
        }
    }
    if (ImGui::CollapsingHeader(tr("Render"), ImGuiTreeNodeFlags_DefaultOpen)) {
        uint32_t b = uint32_t(e.blend);
        if (comboEnum(tr("Blend"), b, kBlendNames)) {
            e.blend = VfxBlend(b);
            c = true;
        }
        uint32_t f = uint32_t(e.facing);
        if (comboEnum(tr("Facing"), f, kFacingNames)) {
            e.facing = VfxFacing(f);
            c = true;
        }
        // Textura: embutidas do motor ou arquivo do jogo.
        if (ImGui::BeginCombo(tr("Texture"), e.texture.empty() ? tr("(soft disc)") : e.texture.c_str())) {
            if (ImGui::Selectable(tr("(soft disc)"), e.texture.empty())) {
                e.texture.clear();
                c = true;
            }
            for (const std::string& b2 : vfxBuiltinNames())
                if (ImGui::Selectable(b2.c_str(), e.texture == b2)) {
                    e.texture = b2;
                    if (b2 == "builtin:puff" || b2 == "builtin:flame") e.flipCols = e.flipRows = 4;
                    else e.flipCols = e.flipRows = 1;
                    c = true;
                }
            ImGui::EndCombo();
        }
        char tex[256];
        std::snprintf(tex, sizeof(tex), "%s", e.texture.c_str());
        if (ImGui::InputText(tr("File (PNG)"), tex, sizeof(tex), ImGuiInputTextFlags_EnterReturnsTrue)) {
            e.texture = tex;
            c = true;
        }
        int grid[2] = {int(e.flipCols), int(e.flipRows)};
        if (ImGui::DragInt2(tr("Frames (columns, rows)"), grid, 0.1f, 1, 64)) {
            e.flipCols = uint32_t(std::clamp(grid[0], 1, 64));
            e.flipRows = uint32_t(std::clamp(grid[1], 1, 64));
            c = true;
        }
        if (e.flipCols * e.flipRows > 1) {
            uint32_t m = uint32_t(e.flipMode);
            if (comboEnum(tr("Frames play"), m, kFlipNames)) {
                e.flipMode = VfxFlipMode(m);
                c = true;
            }
            if (e.flipMode == VfxFlipMode::Fps) c |= ImGui::DragFloat(tr("Frames per second"), &e.flipFps, 0.2f, 0.0f, 240.0f);
            c |= ImGui::Checkbox(tr("Blend next frame"), &e.flipBlend);
            c |= ImGui::Checkbox(tr("Random first frame"), &e.flipRandomStart);
        }
        c |= ImGui::DragFloat(tr("Stretch with speed"), &e.stretch, 0.002f, 0.0f, 10.0f, "%.3f");
        c |= ImGui::DragFloat(tr("Soft edge (m)"), &e.softDistance, 0.01f, 0.0f, 100.0f, "%.2f");
        c |= ImGui::DragFloat(tr("Glow (emissive)"), &e.emissive, 0.02f, 0.0f, 100.0f, "%.2f");
        c |= ImGui::SliderFloat(tr("Lit by the scene"), &e.lit, 0.0f, 1.0f, "%.2f");
        c |= ImGui::SliderFloat(tr("Max screen size"), &e.maxScreenSize, 0.01f, 4.0f, "%.2f");
        c |= ImGui::DragFloat(tr("Fade near camera (m)"), &e.nearFade, 0.01f, 0.0f, 100.0f, "%.2f");
    }
    ImGui::PopItemWidth();
    return c;
}

bool VfxEditor::subEmitterUi(const char* label, VfxSubEmitter& s, const std::string& self) {
    bool c = false;
    ImGui::PushID(label);
    if (ImGui::BeginCombo(label, s.emitter.empty() ? tr("(none)") : s.emitter.c_str())) {
        if (ImGui::Selectable(tr("(none)"), s.emitter.empty())) {
            s.emitter.clear();
            c = true;
        }
        for (const auto& other : m_fx.emitters)
            if (other.name != self && ImGui::Selectable(other.name.c_str(), s.emitter == other.name)) {
                s.emitter = other.name;
                c = true;
            }
        ImGui::EndCombo();
    }
    if (!s.emitter.empty()) {
        int n = int(s.count);
        if (ImGui::SliderInt(tr("Children"), &n, 1, 64)) {
            s.count = uint32_t(n);
            c = true;
        }
        c |= ImGui::SliderFloat(tr("Chance"), &s.probability, 0.0f, 1.0f, "%.2f");
        c |= ImGui::SliderFloat(tr("Inherit velocity##sub"), &s.inheritVelocity, 0.0f, 1.0f, "%.2f");
    }
    ImGui::PopID();
    return c;
}

bool VfxEditor::gradientEditor(const char* id, VfxGradient& g) {
    bool changed = false;
    ImGui::PushID(id);
    if (g.keys.empty()) g.keys.push_back({0.0f, Vec4(1.0f)});
    m_gradKey = std::clamp(m_gradKey, 0, int(g.keys.size()) - 1);
    const float w = ImGui::GetContentRegionAvail().x;
    const float barH = 22.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Xadrez atras (le' a opacidade).
    for (int x = 0; x < int(w); x += 8)
        for (int y = 0; y < int(barH); y += 8)
            dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min<float>(w, x + 8.0f), p.y + std::min(barH, y + 8.0f)),
                              ((x + y) / 8) % 2 ? IM_COL32(90, 90, 90, 255) : IM_COL32(140, 140, 140, 255));
    for (int i = 0; i < 64; ++i) {
        const float t0 = float(i) / 64.0f, t1 = float(i + 1) / 64.0f;
        dl->AddRectFilledMultiColor(ImVec2(p.x + t0 * w, p.y), ImVec2(p.x + t1 * w, p.y + barH), toU32(g.sample(t0)),
                                    toU32(g.sample(t1)), toU32(g.sample(t1)), toU32(g.sample(t0)));
    }
    ImGui::InvisibleButton("##bar", ImVec2(w, barH));
    // Clique duplo na barra: chave nova ali.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / w, 0.0f, 1.0f);
        g.keys.push_back({t, g.sample(t)});
        std::sort(g.keys.begin(), g.keys.end(), [](const auto& a, const auto& b) { return a.t < b.t; });
        changed = true;
    }
    // Marcadores das chaves (arrastaveis).
    const ImVec2 mk = ImGui::GetCursorScreenPos();
    for (int i = 0; i < int(g.keys.size()); ++i) {
        const float x = mk.x + g.keys[size_t(i)].t * w;
        ImGui::SetCursorScreenPos(ImVec2(x - 5, mk.y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##k", ImVec2(10, 12));
        if (ImGui::IsItemActivated()) m_gradKey = i;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            g.keys[size_t(i)].t = std::clamp((ImGui::GetIO().MousePos.x - mk.x) / w, 0.0f, 1.0f);
            changed = true;
        }
        const ImU32 col = i == m_gradKey ? ImGui::ColorConvertFloat4ToU32(kAccent) : IM_COL32(220, 220, 220, 255);
        dl->AddTriangleFilled(ImVec2(x, mk.y), ImVec2(x - 5, mk.y + 10), ImVec2(x + 5, mk.y + 10), col);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(mk.x, mk.y + 14));
    if (changed) {
        const float selT = g.keys[size_t(m_gradKey)].t;
        std::stable_sort(g.keys.begin(), g.keys.end(), [](const auto& a, const auto& b) { return a.t < b.t; });
        for (int i = 0; i < int(g.keys.size()); ++i)
            if (g.keys[size_t(i)].t == selT) m_gradKey = i;
    }
    VfxGradient::Key& k = g.keys[size_t(m_gradKey)];
    changed |= ImGui::ColorEdit4("##col", &k.color.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    changed |= ImGui::DragFloat("##t", &k.t, 0.005f, 0.0f, 1.0f, "%.2f");
    ImGui::SameLine();
    if (ImGui::SmallButton("-") && g.keys.size() > 1) {
        g.keys.erase(g.keys.begin() + m_gradKey);
        m_gradKey = 0;
        changed = true;
    }
    ImGui::TextDisabled("%s", tr("Double-click the bar to add a key; drag the markers."));
    ImGui::PopID();
    return changed;
}

bool VfxEditor::curveEditor(const char* id, VfxCurve& c) {
    bool changed = false;
    ImGui::PushID(id);
    if (c.keys.empty()) c.keys.push_back(Vec2(0.0f, 1.0f));
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 80.0f;
    float vmax = 1.0f;
    for (const Vec2& k : c.keys) vmax = std::max(vmax, k.y);
    vmax *= 1.15f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(30, 32, 36, 255));
    dl->AddLine(ImVec2(p.x, p.y + h - h / vmax), ImVec2(p.x + w, p.y + h - h / vmax), IM_COL32(70, 70, 80, 255));
    auto toScreen = [&](float t, float v) { return ImVec2(p.x + t * w, p.y + h - std::clamp(v / vmax, 0.0f, 1.0f) * h); };
    ImVec2 prev = toScreen(0.0f, c.sample(0.0f));
    for (int i = 1; i <= 48; ++i) {
        const float t = float(i) / 48.0f;
        const ImVec2 q = toScreen(t, c.sample(t));
        dl->AddLine(prev, q, ImGui::ColorConvertFloat4ToU32(kAccent), 2.0f);
        prev = q;
    }
    ImGui::InvisibleButton("##area", ImVec2(w, h));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float t = std::clamp((m.x - p.x) / w, 0.0f, 1.0f);
        const float v = std::max(0.0f, (p.y + h - m.y) / h * vmax);
        c.keys.emplace_back(t, v);
        std::sort(c.keys.begin(), c.keys.end(), [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
        changed = true;
    }
    const ImVec2 after = ImGui::GetCursorScreenPos();
    int removeAt = -1;
    for (int i = 0; i < int(c.keys.size()); ++i) {
        const ImVec2 q = toScreen(c.keys[size_t(i)].x, c.keys[size_t(i)].y);
        ImGui::SetCursorScreenPos(ImVec2(q.x - 5, q.y - 5));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##pt", ImVec2(10, 10));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            c.keys[size_t(i)].x = std::clamp((m.x - p.x) / w, 0.0f, 1.0f);
            c.keys[size_t(i)].y = std::max(0.0f, (p.y + h - m.y) / h * vmax);
            changed = true;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && c.keys.size() > 1) removeAt = i;
        dl->AddCircleFilled(q, 4.0f, ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 255));
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        c.keys.erase(c.keys.begin() + removeAt);
        changed = true;
    }
    if (changed)
        std::stable_sort(c.keys.begin(), c.keys.end(), [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
    ImGui::SetCursorScreenPos(after);
    ImGui::TextDisabled("%s", tr("Double-click to add a point, drag to move, right-click to remove."));
    ImGui::PopID();
    return changed;
}

} // namespace eruption
