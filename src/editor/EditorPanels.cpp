// Painéis Hierarquia, Inspetor, Projeto e Console, mais a seleção e as
// edições com desfazer.
#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"

#include "core/Engine.hpp"
#include "renderer/WeatherTypes.hpp"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>
#include <cmath>

namespace eruption {

namespace {


bool icontains(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    const size_t n = std::strlen(needle);
    if (n > hay.size()) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                            std::tolower(static_cast<unsigned char>(needle[k])))
            ++k;
        if (k == n) return true;
    }
    return false;
}

void emptyState(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

// Linha de propriedade: rótulo à esquerda, campo ocupando o resto.
void propertyLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(96.0f);
    ImGui::SetNextItemWidth(-28.0f);
}

bool resetButton(const char* id, bool changed) {
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    const bool pressed = ImGui::SmallButton(id);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Reset to default");
    return pressed;
}

} // namespace

// ---------------------------------------------------------------- seleção

void Editor::select(const Selection& s) {
    m_selection = s;
}

std::string Editor::selectionName(const Selection& s) const {
    switch (s.kind) {
        case SelectionKind::Model: {
            const auto& insts = m_engine->modelRenderer().getInstances();
            if (s.index >= 0 && s.index < static_cast<int>(insts.size())) {
                const std::string& n = insts[static_cast<size_t>(s.index)].name;
                return n.empty() ? "Model " + std::to_string(s.index) : n;
            }
            return {};
        }
        case SelectionKind::Light: return "Point Light " + std::to_string(s.index);
        case SelectionKind::Environment: return "Environment";
        case SelectionKind::None: break;
    }
    return {};
}

bool Editor::selectionHasTransform() const {
    if (m_selection.kind == SelectionKind::Model)
        return m_selection.index >= 0 &&
               m_selection.index < static_cast<int>(m_engine->modelRenderer().getInstances().size());
    if (m_selection.kind == SelectionKind::Light)
        return m_selection.index >= 0 &&
               m_selection.index < static_cast<int>(m_engine->m_deferredLighting.getPointLights().size());
    return false;
}

Mat4 Editor::selectionTransform() const {
    if (!selectionHasTransform()) return Mat4(1.0f);
    if (m_selection.kind == SelectionKind::Model)
        return m_engine->modelRenderer().getInstances()[static_cast<size_t>(m_selection.index)].transform;
    Mat4 m(1.0f);
    m[3] = Vec4(m_engine->m_deferredLighting.getPointLights()[static_cast<size_t>(m_selection.index)].position, 1.0f);
    return m;
}

void Editor::setSelectionTransform(const Mat4& m) {
    if (!selectionHasTransform()) return;
    if (m_selection.kind == SelectionKind::Model)
        m_engine->modelRenderer().setInstanceTransform(static_cast<uint32_t>(m_selection.index), m);
    else
        m_engine->m_deferredLighting.getPointLights()[static_cast<size_t>(m_selection.index)].position = Vec3(m[3]);
}

void Editor::pushTransformEdit(const Selection& s, const Mat4& before, const Mat4& after) {
    if (before == after || s.kind == SelectionKind::None) return;
    Engine* engine = m_engine;
    auto apply = [engine, s](const Mat4& m) {
        if (s.kind == SelectionKind::Model) {
            engine->modelRenderer().setInstanceTransform(static_cast<uint32_t>(s.index), m);
        } else if (s.kind == SelectionKind::Light) {
            auto& lights = engine->m_deferredLighting.getPointLights();
            if (s.index >= 0 && s.index < static_cast<int>(lights.size()))
                lights[static_cast<size_t>(s.index)].position = Vec3(m[3]);
        }
    };
    m_undo.push({"Transform " + selectionName(s), [apply, before] { apply(before); }, [apply, after] { apply(after); }});
}

void Editor::deleteSelection() {
    const Selection s = m_selection;
    Engine* engine = m_engine;
    if (s.kind == SelectionKind::Model && selectionHasTransform()) {
        auto setEnabled = [engine, s](bool on) {
            engine->modelRenderer().setInstanceEnabled(static_cast<uint32_t>(s.index), on);
        };
        setEnabled(false);
        m_undo.push({"Hide " + selectionName(s), [setEnabled] { setEnabled(true); }, [setEnabled] { setEnabled(false); }});
        select({});
    } else if (s.kind == SelectionKind::Light && selectionHasTransform()) {
        auto setEnabled = [engine, s](bool on) {
            auto& lights = engine->m_deferredLighting.getPointLights();
            if (s.index < static_cast<int>(lights.size())) lights[static_cast<size_t>(s.index)].enabled = on;
        };
        setEnabled(false);
        m_undo.push({"Hide " + selectionName(s), [setEnabled] { setEnabled(true); }, [setEnabled] { setEnabled(false); }});
        select({});
    }
}

// ---------------------------------------------------------------- hierarquia

void Editor::drawHierarchy() {
    if (!ImGui::Begin("Hierarchy###Hierarchy", &m_showHierarchy)) { ImGui::End(); return; }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##hfilter", "Search objects...", m_hierarchyFilter, sizeof(m_hierarchyFilter));

    if (m_engine->currentMapName().empty()) {
        emptyState("Nothing here yet. Open a map with File > Open Map (Ctrl+O).");
        ImGui::End();
        return;
    }

    ImGui::BeginChild("##tree");
    const ImGuiTreeNodeFlags leaf = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                    ImGuiTreeNodeFlags_SpanAvailWidth;
    auto row = [&](const Selection& s, const std::string& label, bool enabled) {
        ImGuiTreeNodeFlags f = leaf;
        if (m_selection == s) f |= ImGuiTreeNodeFlags_Selected;
        if (!enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TreeNodeEx(label.c_str(), f);
        if (!enabled) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) select(s);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frameSelection();
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Frame", "F", false, s.kind != SelectionKind::Environment)) frameSelection();
            if (ImGui::MenuItem("Hide", "Delete", false, enabled && s.kind != SelectionKind::Environment))
                deleteSelection();
            ImGui::EndPopup();
        }
    };

    if (icontains("Environment", m_hierarchyFilter))
        row({SelectionKind::Environment, 0}, "Environment", true);

    const auto& lights = m_engine->m_deferredLighting.getPointLights();
    if (!lights.empty()) {
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "Lights (%zu)###lights", lights.size());
        if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (size_t i = 0; i < lights.size(); ++i) {
                const Selection s{SelectionKind::Light, static_cast<int>(i)};
                const std::string name = selectionName(s);
                if (!icontains(name, m_hierarchyFilter)) continue;
                ImGui::PushID(static_cast<int>(i));
                row(s, name, lights[i].enabled);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    const auto& insts = m_engine->modelRenderer().getInstances();
    static std::vector<int> visible;
    visible.clear();
    for (size_t i = 0; i < insts.size(); ++i)
        if (icontains(insts[i].name, m_hierarchyFilter)) visible.push_back(static_cast<int>(i));
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "Models (%zu)###models", visible.size());
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const int i = visible[static_cast<size_t>(r)];
                ImGui::PushID(i);
                const Selection s{SelectionKind::Model, i};
                row(s, selectionName(s), insts[static_cast<size_t>(i)].enabled);
                ImGui::PopID();
            }
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- inspetor

void Editor::drawInspector() {
    if (!ImGui::Begin("Inspector###Inspector", &m_showInspector)) { ImGui::End(); return; }

    if (m_selection.kind == SelectionKind::None) {
        emptyState("Select something in the Scene or the Hierarchy to see and edit its properties here.");
        ImGui::End();
        return;
    }

    if (m_selection.kind == SelectionKind::Environment) {
        DayNightCycle& dn = m_engine->dayNightCycle();
        ImGui::TextColored(kAccent, "Environment");
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Time of Day", ImGuiTreeNodeFlags_DefaultOpen)) {
            int minutes = static_cast<int>(dn.timeOfDay() * 1440.0f);
            char clock[16];
            std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
            propertyLabel("Time");
            if (ImGui::SliderInt("##time", &minutes, 0, 1439, clock)) dn.setTimeOfDay(static_cast<float>(minutes) / 1440.0f);
            bool running = !dn.isPaused();
            propertyLabel("Advance");
            if (ImGui::Checkbox("##run", &running)) dn.setPaused(!running);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Let time pass on its own");
            float scale = dn.timeScale();
            propertyLabel("Speed");
            if (ImGui::SliderFloat("##speed", &scale, 0.1f, 8.0f, "%.1fx", ImGuiSliderFlags_Logarithmic)) dn.setTimeScale(scale);
        }
        if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen)) {
            const WeatherType cur = m_engine->weatherSystem().currentType();
            propertyLabel("Type");
            if (ImGui::BeginCombo("##weather", weatherTypeName(cur))) {
                for (uint32_t t = 0; t < WeatherTypeCount; ++t) {
                    const WeatherType wt = static_cast<WeatherType>(t);
                    if (ImGui::Selectable(weatherTypeName(wt), wt == cur)) m_engine->applyWeatherTypeFull(wt, 1.0f);
                }
                ImGui::EndCombo();
            }
        }
        ImGui::End();
        return;
    }

    if (!selectionHasTransform()) {
        select({});
        ImGui::End();
        return;
    }

    const std::string name = selectionName(m_selection);
    ImGui::TextColored(kAccent, "%s", name.c_str());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Mat4 current = selectionTransform();
        float t[3], r[3], s[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(current), t, r, s);
        bool edited = false, activated = false, finished = false;
        auto track = [&] {
            activated |= ImGui::IsItemActivated();
            finished |= ImGui::IsItemDeactivatedAfterEdit();
        };
        const bool isModel = m_selection.kind == SelectionKind::Model;

        propertyLabel("Position");
        edited |= ImGui::DragFloat3("##pos", t, 0.1f, 0.0f, 0.0f, "%.2f");
        track();
        if (isModel) {
            propertyLabel("Rotation");
            edited |= ImGui::DragFloat3("##rot", r, 0.5f, 0.0f, 0.0f, "%.1f");
            track();
            const bool rotChanged = r[0] != 0.0f || r[1] != 0.0f || r[2] != 0.0f;
            if (resetButton("R##rot", rotChanged)) {
                activated = finished = edited = true;
                r[0] = r[1] = r[2] = 0.0f;
            }
            propertyLabel("Scale");
            edited |= ImGui::DragFloat3("##scl", s, 0.01f, 0.001f, 1000.0f, "%.3f");
            track();
            const bool sclChanged = s[0] != 1.0f || s[1] != 1.0f || s[2] != 1.0f;
            if (resetButton("R##scl", sclChanged)) {
                activated = finished = edited = true;
                s[0] = s[1] = s[2] = 1.0f;
            }
        }
        if (activated && !m_inspectorEditing) {
            m_inspectorEditing = true;
            m_inspectorBefore = current;
        }
        if (edited) {
            Mat4 m(1.0f);
            for (float& v : s) if (std::abs(v) < 1e-4f) v = 1e-4f;
            ImGuizmo::RecomposeMatrixFromComponents(t, r, s, glm::value_ptr(m));
            setSelectionTransform(m);
        }
        if (finished && m_inspectorEditing) {
            pushTransformEdit(m_selection, m_inspectorBefore, selectionTransform());
            m_inspectorEditing = false;
        }
    }

    if (m_selection.kind == SelectionKind::Model) {
        const ModelInstance& inst = m_engine->modelRenderer().getInstances()[static_cast<size_t>(m_selection.index)];
        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool visible = inst.enabled;
            propertyLabel("Visible");
            if (ImGui::Checkbox("##visible", &visible)) {
                const Selection s = m_selection;
                Engine* engine = m_engine;
                auto setEnabled = [engine, s](bool on) {
                    engine->modelRenderer().setInstanceEnabled(static_cast<uint32_t>(s.index), on);
                };
                setEnabled(visible);
                m_undo.push({std::string(visible ? "Show " : "Hide ") + name,
                             [setEnabled, visible] { setEnabled(!visible); },
                             [setEnabled, visible] { setEnabled(visible); }});
            }
            propertyLabel("Asset");
            ImGui::TextWrapped("%s", inst.assetPath.empty() ? "(embedded in the map)" : inst.assetPath.c_str());
            const Vec3 size = inst.worldAabbMax - inst.worldAabbMin;
            propertyLabel("Size");
            ImGui::Text("%.2f x %.2f x %.2f", size.x, size.y, size.z);
        }
    } else if (m_selection.kind == SelectionKind::Light) {
        auto& lights = m_engine->m_deferredLighting.getPointLights();
        PointLight& light = lights[static_cast<size_t>(m_selection.index)];
        if (ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            static PointLight before;
            static bool editing = false;
            bool activated = false, finished = false;
            auto track = [&] {
                activated |= ImGui::IsItemActivated();
                finished |= ImGui::IsItemDeactivatedAfterEdit();
            };
            const PointLight snapshot = light;
            propertyLabel("Enabled");
            ImGui::Checkbox("##en", &light.enabled);
            if (ImGui::IsItemDeactivatedAfterEdit()) { activated = finished = true; }
            propertyLabel("Color");
            ImGui::ColorEdit3("##col", glm::value_ptr(light.color), ImGuiColorEditFlags_Float);
            track();
            propertyLabel("Intensity");
            ImGui::DragFloat("##int", &light.intensity, 0.05f, 0.0f, 100.0f, "%.2f");
            track();
            propertyLabel("Range");
            ImGui::DragFloat("##rad", &light.radius, 0.1f, 0.1f, 500.0f, "%.1f");
            track();
            propertyLabel("Night only");
            ImGui::Checkbox("##night", &light.nightOnly);
            if (ImGui::IsItemDeactivatedAfterEdit()) { activated = finished = true; }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Lit only at night (lamps, lanterns)");
            if (activated && !editing) { editing = true; before = snapshot; }
            if (finished && editing) {
                editing = false;
                const int idx = m_selection.index;
                Engine* engine = m_engine;
                const PointLight after = light;
                auto apply = [engine, idx](const PointLight& v) {
                    auto& ls = engine->m_deferredLighting.getPointLights();
                    if (idx < static_cast<int>(ls.size())) ls[static_cast<size_t>(idx)] = v;
                };
                const PointLight b = before;
                m_undo.push({"Edit " + name, [apply, b] { apply(b); }, [apply, after] { apply(after); }});
            }
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- projeto

void Editor::drawProject() {
    if (!ImGui::Begin("Project###Project", &m_showProject)) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("##projtabs")) {
        if (ImGui::BeginTabItem("Files")) {
            namespace fs = std::filesystem;
            struct Item { fs::path path; bool dir; };
            static fs::path listedDir;
            static std::vector<Item> items;
            static auto listedAt = std::chrono::steady_clock::now();
            static char filter[128] = {};
            const auto now = std::chrono::steady_clock::now();
            if (listedDir != m_projectDir || now - listedAt > std::chrono::seconds(2)) {
                listedDir = m_projectDir;
                listedAt = now;
                items.clear();
                std::error_code ec;
                for (const auto& e : fs::directory_iterator(m_projectDir, ec)) {
                    const std::string n = e.path().filename().string();
                    if (n.empty() || n[0] == '.' || n.rfind("build", 0) == 0 || n == "not_commit") continue;
                    items.push_back({e.path(), e.is_directory(ec)});
                }
                std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
                    if (a.dir != b.dir) return a.dir;
                    return a.path.filename().string() < b.path.filename().string();
                });
            }

            // Caminho clicável a partir da raiz do projeto.
            const fs::path rel = fs::relative(m_projectDir, m_projectRoot);
            if (ImGui::SmallButton("Project")) m_projectDir = m_projectRoot;
            fs::path acc = m_projectRoot;
            if (rel != ".") {
                for (const auto& part : rel) {
                    acc /= part;
                    ImGui::SameLine(0, 2);
                    ImGui::TextDisabled("/");
                    ImGui::SameLine(0, 2);
                    const std::string label = part.string() + "##crumb" + acc.string();
                    if (ImGui::SmallButton(label.c_str())) m_projectDir = acc;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ Script")) m_commands.run("game.new_script");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("New Luau script in the scripts folder");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x));
            ImGui::InputTextWithHint("##pfilter", "Filter...", filter, sizeof(filter));
            ImGui::Separator();

            ImGui::BeginChild("##files");
            if (items.empty()) emptyState("This folder is empty.");
            for (const auto& it : items) {
                const std::string n = it.path.filename().string();
                if (!icontains(n, filter)) continue;
                const std::string label = std::string(it.dir ? "[folder]  " : "") + n;
                if (ImGui::Selectable(label.c_str(), m_projectSelected == it.path.string(),
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_projectSelected = it.path.string();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (it.dir) m_projectDir = it.path;
                        else if (it.path.extension() == ".luau") codeOpen(it.path);
                        else openInExternalEditor(it.path);
                    }
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (!it.dir && it.path.extension() == ".luau" && ImGui::MenuItem("Edit here")) codeOpen(it.path);
                    if (!it.dir && ImGui::MenuItem("Open in VS Code")) openInExternalEditor(it.path);
                    if (it.dir && ImGui::MenuItem("Open folder")) m_projectDir = it.path;
                    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(it.path.string().c_str());
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Maps")) {
            const auto& maps = m_engine->availableMaps();
            if (maps.empty()) emptyState("No maps found.");
            ImGui::TextDisabled("Double-click a map to open it.");
            ImGui::BeginChild("##maps");
            for (const auto& name : maps) {
                if (ImGui::Selectable(name.c_str(), name == m_engine->currentMapName(),
                                      ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    m_pendingMap = name;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- console

void Editor::drawConsole() {
    if (!ImGui::Begin("Console###Console", &m_showConsole)) { ImGui::End(); return; }

    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        m_console.clear();
        m_consoleCounts[0] = m_consoleCounts[1] = m_consoleCounts[2] = 0;
    }
    ImGui::SameLine();
    char lbl[48];
    const char* names[3] = {"Info", "Warnings", "Errors"};
    for (int i = 0; i < 3; ++i) {
        std::snprintf(lbl, sizeof(lbl), "%s (%u)", names[i], m_consoleCounts[i]);
        ImGui::Checkbox(lbl, &m_consoleShow[i]);
        ImGui::SameLine();
    }
    ImGui::Checkbox("Auto-scroll", &m_consoleAutoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##cfilter", "Filter...", m_consoleFilter, sizeof(m_consoleFilter));
    ImGui::Separator();

    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    std::lock_guard<std::mutex> lock(m_consoleMutex);
    static std::vector<int> shown;
    shown.clear();
    for (size_t i = 0; i < m_console.size(); ++i) {
        const ConsoleLine& l = m_console[i];
        const int b = l.level >= LogLevel::Error ? 2 : (l.level == LogLevel::Warning ? 1 : 0);
        if (m_consoleShow[b] && icontains(l.text, m_consoleFilter)) shown.push_back(static_cast<int>(i));
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const ConsoleLine& l = m_console[static_cast<size_t>(shown[static_cast<size_t>(r)])];
            ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if (l.level >= LogLevel::Error) col = ImVec4(0.95f, 0.38f, 0.35f, 1.0f);
            else if (l.level == LogLevel::Warning) col = ImVec4(0.95f, 0.78f, 0.35f, 1.0f);
            ImGui::PushID(r);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (l.repeat > 1) ImGui::Text("%s  (x%u)", l.text.c_str(), l.repeat);
            else ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
            const bool hasSource = l.text.find(".luau:") != std::string::npos;
            if (hasSource && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Double-click to open this line in VS Code");
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openSourceReference(l.text);
            }
            if (ImGui::BeginPopupContextItem("##line")) {
                if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(l.text.c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (m_consoleAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace eruption
