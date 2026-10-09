// File > Export Game: escolhe os mapas, a pasta e o programa do jogo (o
// enxuto, sem editor, compilado pelo próprio editor em segundo plano).
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"
#include "editor/EditorTheme.hpp"

#include <imgui.h>

#include <cstdlib>
#include <thread>

namespace eruption {

namespace {

std::filesystem::path leanBinary(const std::filesystem::path& root) { return root / "build-export" / "eruption-engine"; }

} // namespace

void Editor::startExport() {
    ExportState& ex = m_export;
    if (ex.running) return;
    ExportOptions o;
    o.projectRoot = m_projectRoot;
    for (const auto& [map, on] : ex.maps)
        if (on) o.maps.push_back(map);
    // O mapa aberto vem primeiro (é onde o jogo começa).
    std::stable_partition(o.maps.begin(), o.maps.end(), [&](const std::string& m) { return m == m_engine->currentMapName(); });
    o.outDir = ex.outDir;
    std::error_code ec;
    o.binary = std::filesystem::exists(leanBinary(m_projectRoot), ec) ? leanBinary(m_projectRoot)
                                                                      : m_projectRoot / "eruption-engine";
    ex.running = true;
    ex.status = "Exporting...";
    std::thread([this, o] {
        const ExportReport r = exportGame(o);
        std::string msg;
        if (!r.error.empty()) {
            msg = "Export failed: " + r.error;
        } else {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Exported %zu files (%zu scripts compiled, %zu neighbor maps, %.1f MB) to ", r.files, r.compiled, r.neighbors, r.bytes / (1024.0 * 1024.0));
            msg = buf + o.outDir.string();
            for (const auto& m : r.missing) msg += "\n  not found (left out): " + m;
        }
        log(r.error.empty() ? LogLevel::Info : LogLevel::Error, msg);
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_export.status = msg;
        m_export.running = false;
    }).detach();
}

void Editor::buildLeanBinary() {
    ExportState& ex = m_export;
    if (ex.building) return;
    ex.building = true;
    ex.buildStatus = "Building the game program (several minutes)...";
    const std::filesystem::path root = m_projectRoot;
    std::thread([this, root] {
        const std::string dir = (root / "build-export").string();
        const std::string cmd = "cmake -S \"" + root.string() + "\" -B \"" + dir +
                                "\" -DCMAKE_BUILD_TYPE=Release -DERUPTION_EDITOR=OFF -DERUPTION_LEAN=ON -DERUPTION_ONNX=OFF "
                                "-DERUPTION_DEBUG=OFF > \"" + dir + ".log\" 2>&1 && cmake --build \"" + dir +
                                "\" --target eruption-engine -j4 >> \"" + dir + ".log\" 2>&1";
        const int rc = std::system(cmd.c_str());
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_export.buildStatus = rc == 0 ? "Game program ready." : "Build failed: see " + dir + ".log";
        m_export.building = false;
    }).detach();
}

void Editor::drawExportWindow() {
    if (!m_showExport) return;
    ExportState& ex = m_export;
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Export Game###ExportGame"), &m_showExport)) {
        ImGui::End();
        return;
    }
    if (ex.maps.empty())
        for (const auto& m : m_engine->availableMaps()) ex.maps.push_back({m, m == m_engine->currentMapName()});
    if (ex.outDir[0] == '\0') {
        const char* home = std::getenv("HOME");
        std::snprintf(ex.outDir, sizeof(ex.outDir), "%s/EruptionGames/%s", home ? home : ".",
                      m_engine->currentMapName().empty() ? "game" : m_engine->currentMapName().c_str());
    }
    ImGui::TextWrapped("%s", tr("Copies the chosen maps and only what they use (models, sprites, prefabs, scripts, terrain "
                       "edits) into a folder that runs on its own. The first map is where the game starts."));
    ImGui::SeparatorText(tr("Maps"));
    ImGui::BeginChild("##maps", ImVec2(0, 120), ImGuiChildFlags_Border);
    for (auto& [map, on] : ex.maps) ImGui::Checkbox(map.c_str(), &on);
    ImGui::EndChild();
    ImGui::SeparatorText(tr("Folder"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##out", ex.outDir, sizeof(ex.outDir));
    ImGui::SeparatorText(tr("Game program"));
    std::error_code ec;
    const bool lean = std::filesystem::exists(leanBinary(m_projectRoot), ec);
    if (lean) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.0f), tr("Small game program (no editor): %.1f MB"),
                           std::filesystem::file_size(leanBinary(m_projectRoot), ec) / (1024.0 * 1024.0));
    } else {
        ImGui::TextWrapped("%s", tr("The small game program is not built yet; the export would carry the editor's program."));
    }
    ImGui::BeginDisabled(ex.building);
    if (ImGui::Button(lean ? "Rebuild game program" : "Build game program")) buildLeanBinary();
    ImGui::EndDisabled();
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        if (!ex.buildStatus.empty()) ImGui::TextDisabled("%s", ex.buildStatus.c_str());
    }
    ImGui::Separator();
    ImGui::BeginDisabled(ex.running || ex.building);
    if (ImGui::Button(tr("Export"), ImVec2(140, 0))) startExport();
    ImGui::EndDisabled();
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        if (!ex.status.empty()) ImGui::TextWrapped("%s", ex.status.c_str());
    }
    ImGui::End();
}

} // namespace eruption
