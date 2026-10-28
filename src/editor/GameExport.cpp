#include "editor/GameExport.hpp"
#include "core/GraphicsMerge.hpp"

#include "formats/GroundPack.hpp"
#include "scene/SceneEdits.hpp"
#include "script/LuauChunk.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
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

// CHAO EMPACOTADO: o <mapa>.glb + <mapa>.ter de um mapa gerado vao como um
// <mapa>.relevo (formats/GroundPack: 30 MB -> 1,45 MB num mapa de 601x601),
// que a carga desempacota. So' quando a volta reproduz os dois; senao o par
// vai como esta' e o relatorio diz por que.
std::set<fs::path> packGrounds(const fs::path& outDir, const std::set<fs::path>& files,
                               const std::function<fs::path(const fs::path&)>& sourceOf, ExportReport& report) {
    std::set<fs::path> packed;
    for (const auto& rel : files) {
        if (rel.extension() != ".glb") continue;
        const fs::path ter = fs::path(rel).replace_extension(".ter");
        if (!files.count(ter)) continue;
        std::error_code ec;
        const fs::path dst = outDir / fs::path(rel).replace_extension(".relevo");
        fs::create_directories(dst.parent_path(), ec);
        std::string error;
        if (!packGround(sourceOf(rel), sourceOf(ter), dst, error)) {
            report.groundKept.push_back(rel.generic_string() + ": " + error);
            continue;
        }
        packed.insert(rel);
        packed.insert(ter);
        ++report.groundPacked;
        ++report.files;
        report.bytes += fs::file_size(dst, ec);
    }
    return packed;
}

} // namespace

ExportReport exportGame(const ExportOptions& o) {
    ExportReport report;
    if (o.maps.empty()) {
        report.error = "Choose at least one map.";
        return report;
    }
    const fs::path& root = o.projectRoot;
    // Com o projeto fora do motor, cada caminho relativo e' procurado
    // primeiro no projeto e depois no motor; o jogo exportado junta os dois
    // numa pasta so'.
    const fs::path engineRoot = o.engineRoot.empty() ? o.projectRoot : o.engineRoot;
    std::set<fs::path> files; // relativos à raiz do projeto
    std::deque<fs::path> scan; // arquivos de texto ainda não lidos
    std::error_code ec;
    auto srcOf = [&](const fs::path& rel) -> fs::path {
        if (fs::is_regular_file(root / rel, ec)) return root / rel;
        if (fs::is_regular_file(engineRoot / rel, ec)) return engineRoot / rel;
        return {};
    };
    auto add = [&](const fs::path& rel) {
        const fs::path norm = rel.lexically_normal();
        if (norm.empty() || norm.native().rfind("..", 0) == 0 || srcOf(norm).empty()) return false;
        if (files.insert(norm).second && followsReferences(norm)) scan.push_back(norm);
        return true;
    };
    auto addDir = [&](const fs::path& rel, auto&& keep) {
        for (const fs::path& base : {root, engineRoot})
            for (auto it = fs::recursive_directory_iterator(base / rel, ec); !ec && it != fs::recursive_directory_iterator(); ++it)
                if (it->is_regular_file() && keep(it->path())) add(fs::relative(it->path(), base, ec));
    };
    auto all = [](const fs::path&) { return true; };

    // Mundo contínuo: os mapas emendados (nos dois sentidos) vão junto.
    std::vector<std::string> maps = o.maps;
    {
        std::vector<std::pair<std::string, std::string>> links;
        for (auto it = fs::directory_iterator(root / "assets" / "scenes", ec); !ec && it != fs::directory_iterator(); ++it) {
            const std::string fn = it->path().filename().string();
            const std::string suffix = ".scene.luau";
            if (fn.size() <= suffix.size() || fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            SceneEdits e;
            std::string error;
            if (readText(it->path()).find("neighbors") == std::string::npos || !e.load(it->path(), error)) continue;
            for (const auto& n : e.neighbors) links.push_back({fn.substr(0, fn.size() - suffix.size()), n.map});
        }
        for (size_t i = 0; i < maps.size(); ++i)
            for (const auto& [a, b] : links) {
                const std::string other = a == maps[i] ? b : (b == maps[i] ? a : std::string());
                if (!other.empty() && std::find(maps.begin(), maps.end(), other) == maps.end()) maps.push_back(other);
            }
        report.neighbors = maps.size() - o.maps.size();
    }

    // Mapas e o que é deles.
    for (const auto& map : maps) {
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
    for (const fs::path& base : {root, engineRoot})
        for (auto it = fs::directory_iterator(base / "data", ec); !ec && it != fs::directory_iterator(); ++it)
            if (it->is_regular_file() && it->path().filename() != "launcher_config.json")
                add(fs::relative(it->path(), base, ec));
    addDir("data/hud", all);
    addDir("assets/data", all);
    addDir("shaders", [](const fs::path& p) { return p.extension() == ".spv"; });
    addDir("assets/icon", all); // ícone da janela e logo da abertura
    addDir("assets/fonts", all); // fonte da interface do jogo (Engine::initImGui)
    add("assets/sprites/default.png");
    add("assets/sprites/default.spr");
    // Scripts do jogo (o arquivo de tipos é só do editor).
    addDir("assets/scripts", [](const fs::path& p) { return p.extension() == ".luau" && p.filename() != "eruption.d.luau"; });
    // Módulo C++ (native/): vai a última versão compilada, não os fontes.
    if (fs::is_regular_file(root / ".eruption" / "native" / "game.so", ec)) add(fs::path(".eruption") / "native" / "game.so");

    // Caminhos entre aspas nos arquivos de texto: o resto do grafo.
    static const std::regex quoted("\"([^\"\\n]{3,260}\\.(?:glb|png|spr|luau|ter|json|wav|ogg|mp3|flac))\"");
    while (!scan.empty()) {
        const fs::path rel = scan.front();
        scan.pop_front();
        const std::string text = readText(srcOf(rel));
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
    const std::set<fs::path> packed = packGrounds(o.outDir, files, srcOf, report);
    for (const auto& rel : files) {
        if (packed.count(rel)) continue;
        const fs::path dst = o.outDir / rel;
        fs::create_directories(dst.parent_path(), ec);
        if (rel.extension() == ".luau") {
            // Scripts e arquivos de cena vão compilados: o jogo não leva o compilador.
            std::string err;
            const std::string bc = compileLuauForExport(readText(srcOf(rel)), err);
            if (bc.empty()) {
                report.error = rel.generic_string() + ": " + err;
                return report;
            }
            std::ofstream(dst, std::ios::binary | std::ios::trunc).write(bc.data(), std::streamsize(bc.size()));
            ++report.compiled;
        } else {
            fs::copy_file(srcOf(rel), dst, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            report.error = "Could not copy " + rel.generic_string() + ": " + ec.message();
            return report;
        }
        ++report.files;
        report.bytes += fs::file_size(dst, ec);
    }
    // Configuração de partida: o mapa inicial, o enquadramento do jogo
    // ("spawn" e "camera" do project.eruption - sem eles o exportado abria com
    // a abertura padrão do motor em vez da do jogo) e a tela de carga
    // ("loading": sem ela o exportado mostrava a do motor). Nada desta máquina.
    {
        nlohmann::json launch;
        launch["initial_map"] = o.maps.front();
        std::ifstream pf(o.projectRoot / "project.eruption");
        const nlohmann::json pj = pf ? nlohmann::json::parse(pf, nullptr, false) : nlohmann::json();
        if (pj.is_object()) {
            for (const char* key : {"spawn", "camera", "loading"})
                if (pj.contains(key)) launch[key] = pj[key];
        }
        fs::create_directories(o.outDir / "data", ec);
        std::ofstream(o.outDir / "data" / "launcher_config.json") << launch.dump(1) << "\n";
    }
    // data/graphics.json do projeto e' so' o patch: copiado como veio, o jogo
    // exportado (que abre sem projeto) ficava sem "presets"/"auto_presets" e
    // o celular caia no padrao. Vai o arquivo ja' mesclado sobre o do motor.
    // O skybox_config.json do projeto tambem e' patch (sol, halo, estrelas).
    for (const char* name : {"graphics.json", "skybox_config.json"}) {
        const fs::path engineGfx = (o.engineRoot.empty() ? o.projectRoot : o.engineRoot) / "data" / name;
        const fs::path projectGfx = o.projectRoot / "data" / name;
        std::ifstream pf(projectGfx), ef(engineGfx);
        nlohmann::json patch = pf ? nlohmann::json::parse(pf, nullptr, false) : nlohmann::json();
        nlohmann::json merged = ef ? nlohmann::json::parse(ef, nullptr, false) : nlohmann::json();
        if (!fs::equivalent(projectGfx, engineGfx, ec) && patch.is_object() && merged.is_object()) {
            mergeGraphicsPatch(merged, std::move(patch));
            fs::create_directories(o.outDir / "data", ec);
            std::ofstream(o.outDir / "data" / name) << merged.dump(2) << "\n";
        }
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

bool projectUsesSound(const fs::path& root) {
    std::error_code ec;
    auto scan = [&](const fs::path& dir, const char* needle) {
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".luau") continue;
            if (it->path().filename().string().find(".d.luau") != std::string::npos) continue; // tipos da API
            std::ifstream f(it->path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (text.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    return scan(root / "assets" / "scripts", "audio.") || scan(root / "assets" / "scenes", "sounds =");
}

bool builtProfile(const fs::path& buildDir, BuildProfile& out) {
    std::ifstream f(buildDir / "CMakeCache.txt");
    if (!f) return false;
    std::string line;
    bool found = false;
    while (std::getline(f, line)) {
        if (line.rfind("ERUPTION_MODULE_AUDIO:BOOL=", 0) == 0) {
            out.audio = line.substr(line.find('=') + 1) == "ON";
            found = true;
        }
    }
    return found;
}

std::string profileCMakeFlags(const BuildProfile& p) {
    return std::string("-DERUPTION_MODULE_AUDIO=") + (p.audio ? "ON" : "OFF") + " -DERUPTION_MODULE_NEURAL_PBR=OFF";
}

} // namespace eruption
