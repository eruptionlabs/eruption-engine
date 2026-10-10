// File > New Map: cria assets/external/<nome>/<nome>.glb com um chão plano
// e um .env com a direção do sol, e abre o mapa novo.
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"
#include "core/Project.hpp"
#include "formats/FlatMap.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

bool validMapName(const char* s) {
    if (!*s) return false;
    for (const char* p = s; *p; ++p)
        if (!(std::isalnum(static_cast<unsigned char>(*p)) || *p == '_' || *p == '-')) return false;
    return true;
}

} // namespace

bool Editor::createFlatMap(const std::string& name, float size, std::string& error) {
    // A criacao do mapa vazio mora em formats/FlatMap: o --new-map da linha
    // de comando usa exatamente este codigo.
    FlatMap::Options o;
    o.size = size;
    if (!FlatMap::create(project::game("assets/external") / name, name, o, error)) return false;
    m_engine->populateMaps();
    return true;
}

void Editor::drawNewMapDialog() {
    if (m_newMapOpen) {
        ImGui::OpenPopup("New Map");
        m_newMapOpen = false;
        m_newMapError.clear();
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("New Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    static const char* kSizes[] = {"Small (500 x 500)", "Medium (1000 x 1000)", "Large (2000 x 2000)"};
    static const float kSizeValues[] = {500.0f, 1000.0f, 2000.0f};
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText(tr("Name"), m_newMapName, sizeof(m_newMapName),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Combo(tr("Size"), &m_newMapSize, kSizes, IM_ARRAYSIZE(kSizes));
    ImGui::TextDisabled("%s", tr("A flat ground. Letters, digits, _ and - in the name."));
    const bool ok = validMapName(m_newMapName);
    if (!m_newMapError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_newMapError.c_str());

    ImGui::Separator();
    ImGui::BeginDisabled(!ok);
    const bool create = ImGui::Button(tr("Create"), ImVec2(120, 0)) || (enter && ok);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (create) {
        std::string error;
        if (createFlatMap(m_newMapName, kSizeValues[m_newMapSize], error)) {
            log(LogLevel::Info, std::string("Created map ") + m_newMapName);
            m_pendingMap = m_newMapName;
            m_newMapName[0] = '\0';
            ImGui::CloseCurrentPopup();
        } else {
            m_newMapError = error;
        }
    }
    ImGui::EndPopup();
}

} // namespace eruption
