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
#include <filesystem>

#include <glm/gtc/matrix_transform.hpp>

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

// .glb e prefab arrastáveis do painel Projeto para a cena.
void modelDragSource(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    if ((e != ".glb" && e != ".png" && !Prefab::isPrefabPath(p)) || !ImGui::BeginDragDropSource()) return;
    const std::string ps = p.string();
    ImGui::SetDragDropPayload(kModelPayload, ps.c_str(), ps.size() + 1);
    ImGui::Text("%s", p.filename().string().c_str());
    ImGui::TextDisabled(e == ".png" ? "Drop on the scene to place a character" : "Drop on the scene to place it");
    ImGui::EndDragDropSource();
}

bool isPreviewableImage(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
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
    if (m_selection.kind == SelectionKind::Asset && s.kind != SelectionKind::Asset) closeAssetPreview();
    m_selection = s;
    m_extraSelection.clear();
}

bool Editor::isSelected(const Selection& s) const {
    return s.kind != SelectionKind::None &&
           (m_selection == s || std::find(m_extraSelection.begin(), m_extraSelection.end(), s) != m_extraSelection.end());
}

void Editor::toggleSelect(const Selection& s) {
    // Só itens com posição entram juntos (modelo, personagem, luz, área, início).
    if (!hasTransformOf(s)) {
        select(s);
        return;
    }
    if (!hasTransformOf(m_selection)) {
        select(s);
        return;
    }
    if (m_selection == s) {
        if (m_extraSelection.empty()) {
            select({});
        } else {
            m_selection = m_extraSelection.front();
            m_extraSelection.erase(m_extraSelection.begin());
        }
        return;
    }
    auto it = std::find(m_extraSelection.begin(), m_extraSelection.end(), s);
    if (it != m_extraSelection.end()) m_extraSelection.erase(it);
    else m_extraSelection.push_back(s);
}

std::vector<Editor::Selection> Editor::selectionList() const {
    std::vector<Selection> out;
    if (hasTransformOf(m_selection)) out.push_back(m_selection);
    for (const auto& s : m_extraSelection)
        if (hasTransformOf(s) && !(s == m_selection)) out.push_back(s);
    return out;
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
        case SelectionKind::Spawn: return "Player Start";
        case SelectionKind::Area: {
            const auto& as = m_engine->areas();
            return s.index >= 0 && s.index < static_cast<int>(as.size()) ? as[static_cast<size_t>(s.index)].name : std::string();
        }
        case SelectionKind::Character: {
            const auto& cs = m_engine->characters();
            return s.index >= 0 && s.index < static_cast<int>(cs.size()) ? cs[static_cast<size_t>(s.index)].name : std::string();
        }
        case SelectionKind::Environment: return "Environment";
        case SelectionKind::Asset: return m_asset.path.filename().string();
        case SelectionKind::None: break;
    }
    return {};
}

bool Editor::hasTransformOf(const Selection& s) const {
    if (s.kind == SelectionKind::Model)
        return s.index >= 0 &&
               s.index < static_cast<int>(m_engine->modelRenderer().getInstances().size());
    if (s.kind == SelectionKind::Light)
        return s.index >= 0 &&
               s.index < static_cast<int>(m_engine->m_deferredLighting.getPointLights().size());
    if (s.kind == SelectionKind::Character)
        return s.index >= 0 && s.index < static_cast<int>(m_engine->characters().size()) &&
               !m_engine->characters()[static_cast<size_t>(s.index)].removed;
    if (s.kind == SelectionKind::Spawn) return m_engine->hasPlayerStart();
    if (s.kind == SelectionKind::Area)
        return s.index >= 0 && s.index < static_cast<int>(m_engine->areas().size()) &&
               !m_engine->areas()[static_cast<size_t>(s.index)].removed;
    return false;
}

bool Editor::selectionHasTransform() const { return hasTransformOf(m_selection); }

namespace {
// Personagem como matriz para o gizmo: posição + giro em Y (para onde olha).
Mat4 characterMatrix(const Engine::SceneCharacter& c) {
    return glm::rotate(glm::translate(Mat4(1.0f), c.position), glm::radians(c.facing), Vec3(0.0f, 1.0f, 0.0f));
}
void setCharacterFromMatrix(Engine::SceneCharacter& c, const Mat4& m) {
    c.position = Vec3(m[3]);
    const Vec3 z(m[2]);
    if (std::abs(z.x) + std::abs(z.z) > 1e-5f) c.facing = glm::degrees(std::atan2(z.x, z.z));
}
} // namespace

Mat4 Editor::transformOf(const Selection& s) const {
    if (!hasTransformOf(s)) return Mat4(1.0f);
    if (s.kind == SelectionKind::Model)
        return m_engine->modelRenderer().getInstances()[static_cast<size_t>(s.index)].transform;
    if (s.kind == SelectionKind::Character)
        return characterMatrix(m_engine->characters()[static_cast<size_t>(s.index)]);
    if (s.kind == SelectionKind::Spawn) return glm::translate(Mat4(1.0f), m_engine->playerStart());
    if (s.kind == SelectionKind::Area) return m_engine->areas()[static_cast<size_t>(s.index)].matrix();
    Mat4 m(1.0f);
    m[3] = Vec4(m_engine->m_deferredLighting.getPointLights()[static_cast<size_t>(s.index)].position, 1.0f);
    return m;
}

Mat4 Editor::selectionTransform() const { return transformOf(m_selection); }

void Editor::setTransformOf(const Selection& s, const Mat4& m) {
    if (!hasTransformOf(s)) return;
    if (s.kind == SelectionKind::Model)
        m_engine->modelRenderer().setInstanceTransform(static_cast<uint32_t>(s.index), m);
    else if (s.kind == SelectionKind::Character)
        setCharacterFromMatrix(m_engine->characters()[static_cast<size_t>(s.index)], m);
    else if (s.kind == SelectionKind::Spawn)
        m_engine->movePlayerStart(Vec3(m[3]));
    else if (s.kind == SelectionKind::Area)
        m_engine->areas()[static_cast<size_t>(s.index)].setMatrix(m);
    else
        m_engine->m_deferredLighting.getPointLights()[static_cast<size_t>(s.index)].position = Vec3(m[3]);
}

void Editor::setSelectionTransform(const Mat4& m) { setTransformOf(m_selection, m); }

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
        } else if (s.kind == SelectionKind::Character) {
            auto& cs = engine->characters();
            if (s.index >= 0 && s.index < static_cast<int>(cs.size())) setCharacterFromMatrix(cs[static_cast<size_t>(s.index)], m);
        } else if (s.kind == SelectionKind::Spawn) {
            engine->movePlayerStart(Vec3(m[3]));
        } else if (s.kind == SelectionKind::Area) {
            auto& as = engine->areas();
            if (s.index >= 0 && s.index < static_cast<int>(as.size())) as[static_cast<size_t>(s.index)].setMatrix(m);
        }
    };
    m_undo.push({"Transform " + selectionName(s), [apply, before] { apply(before); }, [apply, after] { apply(after); }});
}

void Editor::deleteSelection() {
    const auto list = selectionList();
    if (list.size() > 1) {
        // Um desfazer para o grupo todo.
        m_undo.beginGroup("Delete " + std::to_string(list.size()) + " objects");
        for (const auto& s : list) {
            m_selection = s;
            m_extraSelection.clear();
            deleteOne();
        }
        m_undo.endGroup();
        select({});
        return;
    }
    deleteOne();
}

void Editor::deleteOne() {
    const Selection s = m_selection;
    Engine* engine = m_engine;
    if (s.kind == SelectionKind::Model && selectionHasTransform()) {
        auto setEnabled = [engine, s](bool on) {
            engine->modelRenderer().setInstanceEnabled(static_cast<uint32_t>(s.index), on);
        };
        setEnabled(false);
        m_undo.push({"Delete " + selectionName(s), [setEnabled] { setEnabled(true); }, [setEnabled] { setEnabled(false); }});
        select({});
    } else if (s.kind == SelectionKind::Spawn && selectionHasTransform()) {
        const Vec3 p = m_engine->playerStart();
        m_engine->clearPlayerStart();
        m_undo.push({"Delete Player Start", [engine, p] { engine->movePlayerStart(p); }, [engine] { engine->clearPlayerStart(); }});
        select({});
    } else if (s.kind == SelectionKind::Character && selectionHasTransform()) {
        auto setRemoved = [engine, s](bool removed) {
            auto& cs = engine->characters();
            if (s.index < static_cast<int>(cs.size())) cs[static_cast<size_t>(s.index)].removed = removed;
        };
        setRemoved(true);
        m_undo.push({"Delete " + selectionName(s), [setRemoved] { setRemoved(false); }, [setRemoved] { setRemoved(true); }});
        select({});
    } else if (s.kind == SelectionKind::Area && selectionHasTransform()) {
        auto setRemoved = [engine, s](bool removed) {
            auto& as = engine->areas();
            if (s.index < static_cast<int>(as.size())) as[static_cast<size_t>(s.index)].removed = removed;
        };
        setRemoved(true);
        m_undo.push({"Delete " + selectionName(s), [setRemoved] { setRemoved(false); }, [setRemoved] { setRemoved(true); }});
        select({});
    } else if (s.kind == SelectionKind::Light && selectionHasTransform()) {
        auto setEnabled = [engine, s](bool on) {
            auto& lights = engine->m_deferredLighting.getPointLights();
            if (s.index < static_cast<int>(lights.size())) lights[static_cast<size_t>(s.index)].enabled = on;
        };
        setEnabled(false);
        m_undo.push({"Delete " + selectionName(s), [setEnabled] { setEnabled(true); }, [setEnabled] { setEnabled(false); }});
        select({});
    }
}

void Editor::restoreSelection(const Selection& s) {
    Engine* engine = m_engine;
    std::function<void(bool)> setEnabled;
    if (s.kind == SelectionKind::Model)
        setEnabled = [engine, s](bool on) { engine->modelRenderer().setInstanceEnabled(static_cast<uint32_t>(s.index), on); };
    else if (s.kind == SelectionKind::Light)
        setEnabled = [engine, s](bool on) {
            auto& lights = engine->m_deferredLighting.getPointLights();
            if (s.index < static_cast<int>(lights.size())) lights[static_cast<size_t>(s.index)].enabled = on;
        };
    else
        return;
    setEnabled(true);
    m_undo.push({"Restore " + selectionName(s), [setEnabled] { setEnabled(false); }, [setEnabled] { setEnabled(true); }});
    select(s);
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
        if (isSelected(s)) f |= ImGuiTreeNodeFlags_Selected;
        if (!enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TreeNodeEx(label.c_str(), f);
        if (!enabled) ImGui::PopStyleColor();
        // Ctrl/Shift soma ou tira; botão direito num já selecionado mantém o grupo (menu vale para todos).
        const bool add = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (add) toggleSelect(s);
            else select(s);
        } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !isSelected(s)) {
            select(s);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frameSelection();
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Frame", "F", false, s.kind != SelectionKind::Environment)) frameSelection();
            if (enabled) {
                if (ImGui::MenuItem("Delete", "Delete", false, s.kind != SelectionKind::Environment)) deleteSelection();
            } else if (ImGui::MenuItem("Restore")) {
                restoreSelection(s);
            }
            if (s.kind == SelectionKind::Model && enabled) {
                ImGui::Separator();
                if (ImGui::MenuItem("Save as Prefab...")) {
                    select(s);
                    m_commands.run("edit.save_prefab");
                }
            }
            ImGui::EndPopup();
        }
    };

    if (icontains("Environment", m_hierarchyFilter))
        row({SelectionKind::Environment, 0}, "Environment", true);
    if (m_engine->hasPlayerStart() && icontains("Player Start", m_hierarchyFilter))
        row({SelectionKind::Spawn, 0}, "Player Start", true);

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

    const auto& areas = m_engine->areas();
    size_t liveAreas = 0;
    for (const auto& a : areas) liveAreas += a.removed || a.fromNeighbor ? 0 : 1;
    if (liveAreas) {
        char ahdr[64];
        std::snprintf(ahdr, sizeof(ahdr), "Areas (%zu)###areas", liveAreas);
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::TreeNodeEx(ahdr, ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (size_t i = 0; i < areas.size(); ++i) {
                if (areas[i].removed || areas[i].fromNeighbor || !icontains(areas[i].name, m_hierarchyFilter)) continue;
                ImGui::PushID(static_cast<int>(i) + 2000000);
                row({SelectionKind::Area, static_cast<int>(i)}, areas[i].name, true);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    const auto& chars = m_engine->characters();
    size_t liveChars = 0;
    for (const auto& c : chars) liveChars += c.removed || c.fromNeighbor ? 0 : 1;
    if (liveChars) {
        char chdr[64];
        std::snprintf(chdr, sizeof(chdr), "Characters (%zu)###characters", liveChars);
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::TreeNodeEx(chdr, ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (size_t i = 0; i < chars.size(); ++i) {
                if (chars[i].removed || chars[i].fromNeighbor || !icontains(chars[i].name, m_hierarchyFilter)) continue;
                ImGui::PushID(static_cast<int>(i) + 1000000);
                row({SelectionKind::Character, static_cast<int>(i)}, chars[i].name, chars[i].visible);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    const auto& insts = m_engine->modelRenderer().getInstances();
    static std::vector<int> visible;
    visible.clear();
    for (size_t i = 0; i < insts.size(); ++i) {
        // Cópias e modelos acrescentados que foram apagados não aparecem; um
        // objeto do mapa apagado aparece apagado (Restore o traz de volta).
        if (i >= m_original.count && !insts[i].enabled) continue;
        if (m_engine->isScatterInstance(static_cast<uint32_t>(i))) continue; // editados pelo pincel
        if (m_engine->isRegionInstance(static_cast<uint32_t>(i))) continue;  // mapa vizinho
        if (icontains(insts[i].name, m_hierarchyFilter)) visible.push_back(static_cast<int>(i));
    }
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

    if (m_tool == Tool::Terrain) {
        drawTerrainPanel();
        ImGui::End();
        return;
    }
    if (m_selection.kind == SelectionKind::None) {
        emptyState("Select something in the Scene or the Hierarchy to see and edit its properties here.");
        ImGui::End();
        return;
    }

    if (m_selection.kind == SelectionKind::Asset) {
        drawAssetPreview();
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
            if (ImGui::SliderInt("##time", &minutes, 0, 1439, clock)) {
                dn.setTimeOfDay(static_cast<float>(minutes) / 1440.0f);
                m_envEdited = true;
            }
            bool running = !dn.isPaused();
            propertyLabel("Advance");
            if (ImGui::Checkbox("##run", &running)) {
                dn.setPaused(!running);
                m_envEdited = true;
            }
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
                    if (ImGui::Selectable(weatherTypeName(wt), wt == cur)) {
                        m_engine->applyWeatherTypeFull(wt, 1.0f);
                        m_envEdited = true;
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen)) {
            static float level = 0.0f;
            bool on = m_engine->waterEnabled();
            if (!ImGui::IsAnyItemActive()) level = m_engine->waterLevel();
            propertyLabel("Water");
            if (ImGui::Checkbox("##water", &on)) {
                m_engine->setWaterLevel(on, level);
                m_envEdited = m_waterEdited = true;
            }
            ImGui::SetItemTooltip("A flat water surface over the whole map. Lower the ground below it (Terrain tool) to make lakes.");
            ImGui::BeginDisabled(!on);
            propertyLabel("Level");
            ImGui::DragFloat("##wlevel", &level, 0.25f, -1000.0f, 5000.0f, "%.1f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                m_engine->setWaterLevel(true, level);
                m_envEdited = m_waterEdited = true;
            }
            ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader("Neighbor maps", ImGuiTreeNodeFlags_DefaultOpen)) drawNeighborMaps();
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
    if (const size_t more = selectionList().size(); more > 1)
        ImGui::TextDisabled("+ %zu more selected: the tools move, rotate, scale, delete and duplicate all of them.", more - 1);
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
        // Modelo e área têm giro e tamanho (na área, o tamanho é o da caixa).
        const bool isModel = m_selection.kind == SelectionKind::Model || m_selection.kind == SelectionKind::Area;

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
            bool solid = m_engine->isSolid(static_cast<uint32_t>(m_selection.index));
            propertyLabel("Solid");
            if (ImGui::Checkbox("##solid", &solid)) {
                const uint32_t idx = static_cast<uint32_t>(m_selection.index);
                Engine* engine = m_engine;
                engine->setSolid(idx, solid);
                m_undo.push({std::string(solid ? "Solid " : "Not solid ") + name, [engine, idx, solid] { engine->setSolid(idx, !solid); },
                             [engine, idx, solid] { engine->setSolid(idx, solid); }});
            }
            ImGui::SetItemTooltip("The player can't walk through the bottom of this model (trunk, wall base).");
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
    } else if (m_selection.kind == SelectionKind::Area) {
        Engine::TriggerArea& a = m_engine->areas()[static_cast<size_t>(m_selection.index)];
        if (ImGui::CollapsingHeader("Trigger Area", ImGuiTreeNodeFlags_DefaultOpen)) {
            propertyLabel("Name");
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", a.name.c_str());
            if (ImGui::InputText("##aname", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) && buf[0] &&
                m_engine->findArea(buf) < 0) {
                const std::string before = a.name, after = buf;
                const int idx = m_selection.index;
                Engine* engine = m_engine;
                auto rename = [engine, idx](const std::string& n) {
                    auto& as = engine->areas();
                    if (idx < static_cast<int>(as.size())) as[static_cast<size_t>(idx)].name = n;
                };
                rename(after);
                m_undo.push({"Rename area", [rename, before] { rename(before); }, [rename, after] { rename(after); }});
            }
            ImGui::SetItemTooltip("Rules and scripts use this name: on.enter(\"name\", ...). Press Enter to rename.");
            ImGui::TextWrapped("Fires \"enter\" and \"leave\" when the player walks in and out. Move, Rotate and Scale "
                               "tools change the box.");
        }
    } else if (m_selection.kind == SelectionKind::Character) {
        Engine::SceneCharacter& c = m_engine->characters()[static_cast<size_t>(m_selection.index)];
        if (ImGui::CollapsingHeader("Character", ImGuiTreeNodeFlags_DefaultOpen)) {
            propertyLabel("Name");
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", c.name.c_str());
            if (ImGui::InputText("##name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) && buf[0] &&
                m_engine->findCharacter(buf) < 0)
                c.name = buf;
            ImGui::SetItemTooltip("Scripts and rules find the character by this name. Press Enter to rename.");
            propertyLabel("Sprite");
            ImGui::TextWrapped("%s", c.sprite.c_str());
            const bool pngSelected = std::filesystem::path(m_projectSelected).extension() == ".png";
            ImGui::BeginDisabled(!pngSelected);
            if (ImGui::Button("Use PNG selected in Project")) {
                std::error_code ec;
                const auto rel = std::filesystem::relative(m_projectSelected, std::filesystem::current_path(), ec);
                const std::string path = (!ec && !rel.empty() && rel.native().rfind("..", 0) != 0) ? rel.generic_string()
                                                                                                    : m_projectSelected;
                if (!m_engine->setCharacterSprite(m_selection.index, path)) log(LogLevel::Error, "Could not load " + path);
            }
            ImGui::EndDisabled();
            propertyLabel("Facing");
            ImGui::DragFloat("##facing", &c.facing, 1.0f, -360.0f, 360.0f, "%.0f deg");
            ImGui::SetItemTooltip("Direction the character looks at, in degrees (0 = +Z). Rotate tool works too.");
            propertyLabel("Visible at start");
            ImGui::Checkbox("##vis", &c.visible);
            ImGui::SetItemTooltip("Hidden characters can be shown later by a rule or script.");
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
            const fs::path rel = m_projectDir.lexically_relative(m_projectRoot);
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
            if (ImGui::SmallButton(m_projectGrid ? "List" : "Grid")) m_projectGrid = !m_projectGrid;
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
            // Um clique: prévia no Inspetor. Dois cliques: abre (pasta,
            // script no editor de código, resto no programa do sistema).
            auto activate = [&](const Item& it, bool dbl) {
                m_projectSelected = it.path.string();
                if (!it.dir && !dbl) selectAsset(it.path);
                if (!dbl) return;
                if (it.dir) m_projectDir = it.path;
                else if (it.path.extension() == ".luau") codeOpen(it.path);
                else openInExternalEditor(it.path);
            };
            auto contextMenu = [&](const Item& it) {
                if (ImGui::BeginPopupContextItem()) {
                    if (!it.dir && it.path.extension() == ".luau" && ImGui::MenuItem("Edit here")) codeOpen(it.path);
                    if (!it.dir && it.path.extension() == ".png") {
                        if (ImGui::MenuItem("Import as Sprite Sheet...")) openSpriteImporter(it.path.string());
                        if (ImGui::MenuItem("Use as Player Sprite")) usePlayerSprite(it.path.string(), true);
                    }
                    if (!it.dir && ImGui::MenuItem("Open in VS Code")) openInExternalEditor(it.path);
                    if (it.dir && ImGui::MenuItem("Open folder")) m_projectDir = it.path;
                    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(it.path.string().c_str());
                    ImGui::EndPopup();
                }
            };
            if (m_projectGrid) {
                const float cell = 104.0f;
                const int cols = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 8.0f)));
                int col = 0;
                for (const auto& it : items) {
                    const std::string n = it.path.filename().string();
                    if (!icontains(n, filter)) continue;
                    ImGui::PushID(n.c_str());
                    ImGui::BeginGroup();
                    const bool selected = m_projectSelected == it.path.string();
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##cell", ImVec2(cell, cell + ImGui::GetTextLineHeight() + 6));
                    if (!it.dir) modelDragSource(it.path);
                    const bool hovered = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked()) activate(it, false);
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) activate(it, true);
                    contextMenu(it);
                    if (hovered) ImGui::SetTooltip("%s", n.c_str());
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    if (selected || hovered)
                        dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell + ImGui::GetTextLineHeight() + 6),
                                          selected ? IM_COL32(255, 60, 40, 70) : IM_COL32(255, 255, 255, 20), 4.0f);
                    const ImVec2 box0(p0.x + 8, p0.y + 4), box1(p0.x + cell - 8, p0.y + cell - 12);
                    const GuiTexture* thumb = (!it.dir && isPreviewableImage(it.path)) ? thumbnail(it.path.string()) : nullptr;
                    if (thumb) {
                        const float bw = box1.x - box0.x, bh = box1.y - box0.y;
                        const float sc = std::min(bw / thumb->width, bh / thumb->height);
                        const ImVec2 sz(thumb->width * sc, thumb->height * sc);
                        const ImVec2 c((box0.x + box1.x) * 0.5f, (box0.y + box1.y) * 0.5f);
                        dl->AddImage(reinterpret_cast<ImTextureID>(thumb->set), ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f),
                                     ImVec2(c.x + sz.x * 0.5f, c.y + sz.y * 0.5f));
                    } else {
                        // Ícone desenhado: pasta ou etiqueta com a extensão.
                        if (it.dir) {
                            dl->AddRectFilled(ImVec2(box0.x + 6, box0.y + 14), ImVec2(box1.x - 6, box1.y - 6), IM_COL32(200, 150, 70, 255), 4.0f);
                            dl->AddRectFilled(ImVec2(box0.x + 6, box0.y + 6), ImVec2(box0.x + 36, box0.y + 18), IM_COL32(200, 150, 70, 255), 3.0f);
                        } else {
                            dl->AddRectFilled(ImVec2(box0.x + 14, box0.y + 4), ImVec2(box1.x - 14, box1.y), IM_COL32(70, 72, 78, 255), 4.0f);
                            std::string ext = it.path.extension().string();
                            if (!ext.empty()) ext = ext.substr(1);
                            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return std::toupper(ch); });
                            const ImVec2 ts = ImGui::CalcTextSize(ext.c_str());
                            dl->AddText(ImVec2((box0.x + box1.x - ts.x) * 0.5f, (box0.y + box1.y - ts.y) * 0.5f), IM_COL32(230, 230, 232, 255), ext.c_str());
                        }
                    }
                    // Nome cortado com reticências.
                    std::string label = n;
                    while (label.size() > 3 && ImGui::CalcTextSize(label.c_str()).x > cell - 6) label = label.substr(0, label.size() - 4) + "...";
                    const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
                    dl->AddText(ImVec2(p0.x + (cell - ls.x) * 0.5f, p0.y + cell - 6), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                    ImGui::EndGroup();
                    ImGui::PopID();
                    if (++col % cols != 0) ImGui::SameLine(0, 8);
                }
            } else {
                for (const auto& it : items) {
                    const std::string n = it.path.filename().string();
                    if (!icontains(n, filter)) continue;
                    const std::string label = std::string(it.dir ? "[folder]  " : "") + n;
                    if (ImGui::Selectable(label.c_str(), m_projectSelected == it.path.string(),
                                          ImGuiSelectableFlags_AllowDoubleClick))
                        activate(it, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                    if (!it.dir) modelDragSource(it.path);
                    contextMenu(it);
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

// Mundo contínuo: mapas emendados ao aberto (gravados no scene.luau). O
// jogador anda de um para o outro sem tela de carregamento.
void Editor::drawNeighborMaps() {
    ImGui::TextWrapped("Maps joined to this one. The player walks into them with no loading screen; they load when "
                       "someone gets near the border.");
    // Arrastando o deslocamento: mexe num rascunho e aplica ao soltar
    // (encaixar um mapa custa mais que um frame).
    const std::vector<SceneEdits::Neighbor> before = m_engine->worldNeighbors();
    if (!m_neighborEditing) m_neighborEditStart = before;
    std::vector<SceneEdits::Neighbor>& list = m_neighborEditStart;
    bool finished = false;
    int removeAt = -1;
    const auto regions = m_engine->worldRegions();
    for (size_t i = 0; i < list.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const char* state = "";
        for (const auto& r : regions)
            if (r.map == list[i].map) state = r.state;
        ImGui::TextUnformatted(list[i].map.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", *state ? state : "no height grid");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 20);
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::SetItemTooltip("Unjoin this map");
        propertyLabel("Offset");
        float off[3] = {list[i].offset.x, list[i].offset.y, list[i].offset.z};
        if (ImGui::DragFloat3("##off", off, 1.0f, -100000.0f, 100000.0f, "%.0f")) {
            list[i].offset = Vec3(off[0], off[1], off[2]);
            m_neighborEditing = true;
        }
        finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Where the neighbor sits relative to this map (world units).");
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        list.erase(list.begin() + removeAt);
        finished = true;
    }
    // Novo vizinho: escolhe o mapa e o lado; o deslocamento sai das grades.
    static int pick = -1;
    const auto& maps = m_engine->availableMaps();
    std::vector<const std::string*> choices;
    for (const auto& m : maps) {
        if (m == m_engine->currentMapName()) continue;
        bool used = false;
        for (const auto& n : list) used |= n.map == m;
        if (!used) choices.push_back(&m);
    }
    if (pick >= static_cast<int>(choices.size())) pick = -1;
    propertyLabel("Join");
    if (ImGui::BeginCombo("##join", pick >= 0 ? choices[static_cast<size_t>(pick)]->c_str() : "choose a map")) {
        for (size_t i = 0; i < choices.size(); ++i)
            if (ImGui::Selectable(choices[i]->c_str(), pick == static_cast<int>(i))) pick = static_cast<int>(i);
        ImGui::EndCombo();
    }
    Vec2 lo, hi, nlo, nhi;
    const bool canPlace = pick >= 0 && Engine::mapGroundArea(m_engine->currentMapName(), lo, hi) &&
                          Engine::mapGroundArea(*choices[static_cast<size_t>(pick)], nlo, nhi);
    if (pick >= 0 && !canPlace)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Both maps need a height grid (New Map or Generate Map).");
    ImGui::BeginDisabled(!canPlace);
    // Lados na tela de cima: +X a leste, +Z ao norte.
    const struct { const char* label; Vec3 off; } sides[] = {
        {"East", Vec3(hi.x - nlo.x, 0.0f, lo.y - nlo.y)},
        {"West", Vec3(lo.x - nhi.x, 0.0f, lo.y - nlo.y)},
        {"North", Vec3(lo.x - nlo.x, 0.0f, hi.y - nlo.y)},
        {"South", Vec3(lo.x - nlo.x, 0.0f, lo.y - nhi.y)},
    };
    for (size_t s = 0; s < 4; ++s) {
        if (s) ImGui::SameLine();
        if (ImGui::Button(sides[s].label)) {
            list.push_back({*choices[static_cast<size_t>(pick)], sides[s].off});
            finished = true;
            pick = -1;
        }
    }
    ImGui::EndDisabled();
    if (finished) {
        Engine* engine = m_engine;
        const std::vector<SceneEdits::Neighbor> after = list;
        m_undo.push({"Neighbor maps", [engine, before] { engine->setWorldNeighbors(before); },
                     [engine, after] { engine->setWorldNeighbors(after); }});
        m_engine->setWorldNeighbors(after);
        m_neighborEditing = false;
    }
}

} // namespace eruption
