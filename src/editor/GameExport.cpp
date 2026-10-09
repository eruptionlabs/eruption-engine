#include "editor/GameExport.hpp"

#include "script/LuauChunk.hpp"

#include <nlohmann/json.hpp>

#include <deque>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Arquivos do jogo que citam outros (as configurações do motor não entram:
// elas são copiadas como estão e citam conteúdo antigo de outros mapas).
bool followsReferences(const fs::path& rel) {
    const std::string e = rel.extension().string();
    const std::string top = rel.begin() != rel.end() ? rel.begin()->string() : std::string();
    return e == ".luau" || e == ".env" || (e == ".json" && top != "data");
}

} // namespace

ExportReport exportGame(const ExportOptions& o) {
    ExportReport report;
    if (o.maps.empty()) {
        report.error = "Choose at least one map.";
        return report;
    }
    const fs::path& root = o.projectRoot;
    std::set<fs::path> files; // relativos à raiz do projeto
    std::deque<fs::path> scan; // arquivos de texto ainda não lidos
    std::error_code ec;
    auto add = [&](const fs::path& rel) {
        const fs::path norm = rel.lexically_normal();
        if (norm.empty() || norm.native().rfind("..", 0) == 0 || !fs::is_regular_file(root / norm, ec)) return false;
        if (files.insert(norm).second && followsReferences(norm)) scan.push_back(norm);
        return true;
    };
    auto addDir = [&](const fs::path& rel, auto&& keep) {
        for (auto it = fs::recursive_directory_iterator(root / rel, ec); !ec && it != fs::recursive_directory_iterator(); ++it)
            if (it->is_regular_file() && keep(it->path())) add(fs::relative(it->path(), root, ec));
    };
    auto all = [](const fs::path&) { return true; };

    // Mapas e o que é deles.
    for (const auto& map : o.maps) {
        if (!fs::is_directory(root / "assets" / "external" / map, ec)) {
            report.error = "Map not found: " + map;
            return report;
        }
        addDir(fs::path("assets") / "external" / map, all);
        for (auto it = fs::directory_iterator(root / "assets" / "scenes", ec); !ec && it != fs::directory_iterator(); ++it)
            if (it->path().filename().string().rfind(map + ".", 0) == 0) add(fs::relative(it->path(), root, ec));
        add(fs::path("data") / "climate" / (map + ".json"));
    }
    // Motor: configurações (sem o clima de outros mapas), shaders compilados,
    // materiais, ícones e sprite padrão do jogador.
    for (auto it = fs::directory_iterator(root / "data", ec); !ec && it != fs::directory_iterator(); ++it)
        if (it->is_regular_file() && it->path().filename() != "launcher_config.json") add(fs::relative(it->path(), root, ec));
    addDir("data/hud", all);
    addDir("assets/data", all);
    addDir("shaders", [](const fs::path& p) { return p.extension() == ".spv"; });
    addDir("assets/icon", all); // ícone da janela e logo da abertura
    add("assets/sprites/default.png");
    add("assets/sprites/default.spr");
    // Scripts do jogo (o arquivo de tipos é só do editor).
    addDir("assets/scripts", [](const fs::path& p) { return p.extension() == ".luau" && p.filename() != "eruption.d.luau"; });

    // Caminhos entre aspas nos arquivos de texto: o resto do grafo.
    static const std::regex quoted("\"([^\"\\n]{3,260}\\.(?:glb|png|spr|luau|ter|json))\"");
    while (!scan.empty()) {
        const fs::path rel = scan.front();
        scan.pop_front();
        const std::string text = readText(root / rel);
        for (std::sregex_iterator it(text.begin(), text.end(), quoted), end; it != end; ++it) {
            const fs::path ref = (*it)[1].str();
            if (ref.is_absolute()) {
                // Fora do projeto não vai junto (não roda em outra máquina).
                report.missing.push_back(ref.string() + " (outside the project)");
                continue;
            }
            if (!add(ref)) {
                report.missing.push_back(ref.generic_string());
                continue;
            }
            // Sprite: o .spr do importador mora ao lado do PNG.
            if (ref.extension() == ".png") add(fs::path(ref).replace_extension(".spr"));
        }
    }

    // Copia.
    fs::create_directories(o.outDir, ec);
    if (ec) {
        report.error = "Could not create " + o.outDir.string() + ": " + ec.message();
        return report;
    }
    for (const auto& rel : files) {
        const fs::path dst = o.outDir / rel;
        fs::create_directories(dst.parent_path(), ec);
        if (rel.extension() == ".luau") {
            // Scripts e arquivos de cena vão compilados: o jogo não leva o compilador.
            std::string err;
            const std::string bc = compileLuauForExport(readText(root / rel), err);
            if (bc.empty()) {
                report.error = rel.generic_string() + ": " + err;
                return report;
            }
            std::ofstream(dst, std::ios::binary | std::ios::trunc).write(bc.data(), std::streamsize(bc.size()));
            ++report.compiled;
        } else {
            fs::copy_file(root / rel, dst, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            report.error = "Could not copy " + rel.generic_string() + ": " + ec.message();
            return report;
        }
        ++report.files;
        report.bytes += fs::file_size(dst, ec);
    }
    // Configuração de partida: só o mapa inicial (nada desta máquina).
    {
        nlohmann::json launch;
        launch["initial_map"] = o.maps.front();
        fs::create_directories(o.outDir / "data", ec);
        std::ofstream(o.outDir / "data" / "launcher_config.json") << launch.dump(1) << "\n";
    }
    // Executável e o que ele carrega ao lado.
    const fs::path exe = o.outDir / o.binary.filename();
    fs::copy_file(o.binary, exe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        report.error = "Could not copy the game program: " + ec.message();
        return report;
    }
    fs::permissions(exe, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    report.bytes += fs::file_size(exe, ec);
    ++report.files;
    for (auto it = fs::directory_iterator(o.binary.parent_path(), ec); !ec && it != fs::directory_iterator(); ++it) {
        const std::string n = it->path().filename().string();
        if (n.rfind("libonnxruntime.so", 0) == 0) {
            fs::copy_file(it->path(), o.outDir / n, fs::copy_options::overwrite_existing, ec);
            report.bytes += fs::file_size(o.outDir / n, ec);
            ++report.files;
        }
    }
    std::ofstream run(o.outDir / "run.sh");
    run << "#!/bin/sh\n# Starts the game from its own folder.\ncd \"$(dirname \"$0\")\" && exec ./"
        << exe.filename().string() << " --game \"$@\"\n";
    run.close();
    fs::permissions(o.outDir / "run.sh", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    std::sort(report.missing.begin(), report.missing.end());
    report.missing.erase(std::unique(report.missing.begin(), report.missing.end()), report.missing.end());
    return report;
}

} // namespace eruption
