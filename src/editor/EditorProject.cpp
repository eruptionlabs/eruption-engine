// Janela "Game Project": criar, abrir e configurar um projeto de jogo - a
// pasta fora do repositorio do motor onde vive todo o conteudo (mapas,
// sprites, cenas, scripts, sons). Ver core/Project.hpp.
//
// Trocar de projeto REABRE o programa (platform::relaunch): o motor resolve
// as pastas de asset uma vez, na abertura, e varios caches (texturas
// cozidas, fontes de asset, scripts) nasceram apontando pro projeto antigo.
// Um comeco limpo e' mais honesto do que esvaziar cache por cache.
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"
#include "editor/EditorTheme.hpp"

#include "core/Engine.hpp"
#include "core/Platform.hpp"
#include "core/Project.hpp"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace eruption {

namespace fs = std::filesystem;

void Editor::openGameProject(const std::filesystem::path& dir) {
    Project::rememberLastOpened(dir);
    log(LogLevel::Info, "Opening the project " + dir.generic_string() + "...");
    if (!platform::relaunch({"--editor", "--project", dir.string()})) {
        m_projectPanel.error = "Could not reopen the editor; close it and open it again.";
        log(LogLevel::Error, m_projectPanel.error);
    }
}

void Editor::drawGameProjectWindow() {
    if (!m_showGameProject) return;
    ProjectPanel& st = m_projectPanel;
    Project& prj = Project::instance();
    if (!st.loaded) {
        st.loaded = true;
        std::snprintf(st.openPath, sizeof(st.openPath), "%s",
                      prj.isOpen() ? prj.root().string().c_str() : m_engineRoot.string().c_str());
        if (prj.isOpen()) {
            std::snprintf(st.name, sizeof(st.name), "%s", prj.manifest().name.c_str());
            std::snprintf(st.version, sizeof(st.version), "%s", prj.manifest().version.c_str());
            std::snprintf(st.author, sizeof(st.author), "%s", prj.manifest().author.c_str());
            std::snprintf(st.description, sizeof(st.description), "%s", prj.manifest().description.c_str());
        }
    }

    ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Game Project###GameProject"), &m_showGameProject)) {
        ImGui::End();
        return;
    }

    if (prj.isOpen()) {
        ImGui::TextColored(kAccent, "%s", prj.manifest().name.c_str());
        ImGui::TextDisabled("%s", prj.root().string().c_str());
    } else {
        ImGui::TextWrapped("%s", tr("No project open: the editor is using the engine folder itself. A game project "
                                    "keeps every map, sprite, scene, script and sound of one game in its own folder."));
    }
    if (!st.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", st.error.c_str());
    ImGui::Separator();

    if (prj.isOpen() && ImGui::CollapsingHeader(tr("Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText(tr("Game name"), st.name, sizeof(st.name));
        ImGui::InputText(tr("Version"), st.version, sizeof(st.version));
        ImGui::InputText(tr("Author"), st.author, sizeof(st.author));
        ImGui::InputText(tr("Description"), st.description, sizeof(st.description));

        // Mapa de abertura: o que o jogo (e o editor) carrega ao abrir.
        const std::vector<std::string>& maps = m_engine->availableMaps();
        const std::string& startup = prj.manifest().startupMap;
        if (ImGui::BeginCombo(tr("Startup map"), startup.empty() ? tr("(the first one)") : startup.c_str())) {
            if (ImGui::Selectable(tr("(the first one)"), startup.empty())) prj.manifest().startupMap.clear();
            for (const std::string& m : maps)
                if (ImGui::Selectable(m.c_str(), m == startup)) prj.manifest().startupMap = m;
            ImGui::EndCombo();
        }
        if (ImGui::Checkbox(tr("Player starts at a fixed spot"), &prj.manifest().hasSpawn)) st.error.clear();
        if (prj.manifest().hasSpawn) {
            ImGui::DragFloat3(tr("Spawn"), prj.manifest().spawn, 1.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Use the camera"))) {
                const Vec3 p = m_engine->camera().target(); // o ponto no chao, nao o olho
                prj.manifest().spawn[0] = p.x;
                prj.manifest().spawn[1] = p.y;
                prj.manifest().spawn[2] = p.z;
            }
        }
        if (ImGui::Button(tr("Save project"))) {
            prj.manifest().name = st.name;
            prj.manifest().version = st.version;
            prj.manifest().author = st.author;
            prj.manifest().description = st.description;
            std::string err;
            if (prj.saveManifest(&err)) {
                st.error.clear();
                log(LogLevel::Info, "Project saved in " + (prj.root() / Project::kManifestName).generic_string());
            } else {
                st.error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Open the folder"))) openInExternalEditor(prj.root());
    }

    if (ImGui::CollapsingHeader(tr("New project"), prj.isOpen() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", tr("Creates the folder with everything a game needs: maps, scenes, prefabs, "
                                    "scripts, sprites, animations, effects, sounds and docs."));
        ImGui::InputText(tr("Name##new"), st.newName, sizeof(st.newName));
        ImGui::InputText(tr("Folder##new"), st.newPath, sizeof(st.newPath));
        ImGui::BeginDisabled(st.newPath[0] == '\0');
        if (ImGui::Button(tr("Create and open"))) {
            std::string err;
            const fs::path dir = fs::path(st.newPath);
            if (Project::instance().create(dir, st.newName, &err)) openGameProject(dir);
            else st.error = err;
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader(tr("Open project"))) {
        ImGui::InputText(tr("Folder##open"), st.openPath, sizeof(st.openPath));
        ImGui::TextDisabled("%s", tr("The folder that holds project.eruption."));
        ImGui::BeginDisabled(st.openPath[0] == '\0');
        if (ImGui::Button(tr("Open"))) {
            std::error_code ec;
            const fs::path dir = fs::path(st.openPath);
            if (fs::is_regular_file(dir / Project::kManifestName, ec)) openGameProject(dir);
            else st.error = std::string(tr("No project.eruption in ")) + dir.string();
        }
        ImGui::EndDisabled();
        if (prj.isOpen()) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Close the project"))) {
                Project::rememberLastOpened({});
                if (!platform::relaunch({"--editor"}))
                    st.error = "Could not reopen the editor; close it and open it again.";
            }
        }
    }

    ImGui::End();
}

} // namespace eruption
