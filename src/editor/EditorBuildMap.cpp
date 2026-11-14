// File > Build Map: o pacote do mapa aberto (<mapa>.emap, ver utils/AssetVfs.hpp).
// Roda o proprio programa com --bake-map num processo a parte: o build precisa
// de uma carga limpa do mapa (nesta sessao as texturas ja' na GPU nao seriam
// lidas de novo e ficariam fora do pacote), e o editor segue respondendo.
#include "editor/Editor.hpp"

#include "core/Engine.hpp"
#include "core/Project.hpp"
#include "utils/AssetVfs.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>

namespace eruption {

namespace fs = std::filesystem;

void Editor::startBuildMap() {
    if (m_buildMapJob.valid() || m_engine->currentMapName().empty()) return;
    if (!saveScene()) {
        log(LogLevel::Error, "Build Map: the scene could not be saved; nothing was built");
        return;
    }
    m_buildMapName = m_engine->currentMapName();
    fs::path exe = project::engineRoot() / "eruption-engine";
#ifdef _WIN32
    exe += ".exe";
#endif
    std::string cmd = "\"" + exe.string() + "\" --game --bake-map \"" + m_buildMapName + "\"";
    if (project::isOpen()) cmd += " --project \"" + project::root().string() + "\"";
    m_buildMapJob = std::async(std::launch::async, [cmd] { return std::system(cmd.c_str()); });
    log(LogLevel::Info, "Build Map: building " + m_buildMapName + " in a separate process...");
}

void Editor::pollBuildMap() {
    if (!m_buildMapJob.valid() || m_buildMapJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const int rc = m_buildMapJob.get();
    const fs::path out = vfs::bundlePathFor(m_buildMapName);
    std::error_code ec;
    const auto size = fs::file_size(out, ec);
    if (rc == 0 && !ec)
        log(LogLevel::Info, "Build Map: " + out.string() + " (" + std::to_string(size / (1024 * 1024)) +
                                " MB). It is used the next time the map opens.");
    else
        log(LogLevel::Error, "Build Map: " + m_buildMapName + " failed (exit " + std::to_string(rc) +
                                 "); see the engine log");
}

} // namespace eruption
