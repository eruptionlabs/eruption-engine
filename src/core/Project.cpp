#include "core/Project.hpp"

#include "core/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

namespace eruption {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr const char* kLauncherConfig = "data/launcher_config.json";
}

const std::vector<std::string>& Project::contentDirs() {
    // Tudo o que um jogo guarda. O editor cria as pastas vazias pra pessoa
    // ver onde cada coisa vai, mesmo antes de ter conteudo nenhum.
    static const std::vector<std::string> kDirs = {
        "assets/external",   // mapas: <mapa>/<mapa>.glb + .ter/.nav/.env
        "assets/scenes",     // edicoes de cena por mapa (.scene.luau)
        "assets/prefabs",    // prefabs (.prefab.luau)
        "assets/scripts",    // logica do jogo (Luau)
        "assets/sprites",    // personagens e NPCs
        "assets/animations", // animacoes de sprite
        "assets/effects",    // efeitos (particulas, flashes)
        "assets/sounds",     // som e musica
        "assets/props",      // modelos soltos pra espalhar no mapa
        "assets/data/texture", // texturas e mapas PBR cozidos
        "assets/mapgen",     // receitas do gerador de mapa
        "data/climate",      // clima por mapa
        "docs",              // documentacao do jogo
    };
    return kDirs;
}

Project::Project() {
    std::error_code ec;
    m_engineRoot = fs::current_path(ec);
    if (ec) m_engineRoot = fs::path(".");
}

Project& Project::instance() {
    static Project p;
    return p;
}

bool Project::open(const fs::path& dir, std::string* err) {
    std::error_code ec;
    fs::path root = fs::absolute(dir, ec);
    if (ec) root = dir;
    // Aceita tanto a pasta do projeto quanto o proprio project.eruption.
    if (root.filename() == kManifestName) root = root.parent_path();
    const fs::path file = root / kManifestName;
    if (!fs::is_regular_file(file, ec)) {
        if (err) *err = "project.eruption not found in " + root.string();
        return false;
    }

    Manifest m;
    try {
        std::ifstream f(file);
        json j;
        f >> j;
        m.name = j.value("name", root.filename().string());
        m.version = j.value("version", std::string("0.1.0"));
        m.author = j.value("author", std::string());
        m.description = j.value("description", std::string());
        m.startupMap = j.value("startup_map", std::string());
        m.devTools = j.value("dev_tools", false);
        readFraming(j, m);
    } catch (const std::exception& e) {
        if (err) *err = std::string("project.eruption is not valid JSON: ") + e.what();
        return false;
    }

    m_root = root;
    m_manifest = m;
    m_open = true;
    ERUPTION_LOG_WARN("[PROJECT] \"%s\" aberto em %s", m_manifest.name.c_str(), m_root.string().c_str());
    return true;
}

bool Project::create(const fs::path& dir, const std::string& name, std::string* err) {
    std::error_code ec;
    fs::path root = fs::absolute(dir, ec);
    if (ec) root = dir;
    if (fs::is_regular_file(root / kManifestName, ec)) {
        if (err) *err = "there is already a project in " + root.string();
        return false;
    }
    fs::create_directories(root, ec);
    if (ec) {
        if (err) *err = "could not create " + root.string() + ": " + ec.message();
        return false;
    }
    for (const std::string& d : contentDirs()) fs::create_directories(root / d, ec);

    // A declaracao de tipos do Luau serve pro autocompletar do editor de
    // codigo; ela e' do motor, entao vai COPIADA pro projeto.
    const fs::path decl = m_engineRoot / "assets" / "scripts" / "eruption.d.luau";
    if (fs::is_regular_file(decl, ec))
        fs::copy_file(decl, root / "assets" / "scripts" / "eruption.d.luau", fs::copy_options::skip_existing, ec);
    // Sprite de reserva: um projeto novo ja' comeca com um personagem
    // visivel em vez de um boneco invisivel.
    for (const char* f : {"default.png", "default.spr"}) {
        const fs::path src = m_engineRoot / "assets" / "sprites" / f;
        if (fs::is_regular_file(src, ec))
            fs::copy_file(src, root / "assets" / "sprites" / f, fs::copy_options::skip_existing, ec);
    }

    m_root = root;
    m_manifest = Manifest{};
    m_manifest.name = name.empty() ? root.filename().string() : name;
    m_open = true;
    if (!saveManifest(err)) {
        m_open = false;
        m_root.clear();
        return false;
    }
    ERUPTION_LOG_WARN("[PROJECT] \"%s\" criado em %s", m_manifest.name.c_str(), m_root.string().c_str());
    return true;
}

bool Project::saveManifest(std::string* err) {
    if (!m_open) {
        if (err) *err = "no project open";
        return false;
    }
    try {
        json j;
        j["name"] = m_manifest.name;
        j["version"] = m_manifest.version;
        j["author"] = m_manifest.author;
        j["description"] = m_manifest.description;
        j["startup_map"] = m_manifest.startupMap;
        if (m_manifest.devTools) j["dev_tools"] = true;
        if (m_manifest.hasSpawn) j["spawn"] = {m_manifest.spawn[0], m_manifest.spawn[1], m_manifest.spawn[2]};
        // Salvar o manifesto nao pode APAGAR o enquadramento: o editor grava
        // este arquivo ao mexer no projeto, e o bloco da camera precisa
        // sobreviver a isso.
        {
            json c = json::object();
            if (m_manifest.cameraFov) c["fov"] = *m_manifest.cameraFov;
            if (m_manifest.cameraYaw) c["yaw"] = *m_manifest.cameraYaw;
            if (m_manifest.cameraPitch) c["pitch"] = *m_manifest.cameraPitch;
            if (m_manifest.cameraDistance) c["distance"] = *m_manifest.cameraDistance;
            if (m_manifest.cameraKeepInsideMap) c["keep_inside_map"] = true;
            if (!c.empty()) j["camera"] = c;
        }
        std::ofstream f(m_root / kManifestName);
        if (!f) {
            if (err) *err = "could not write " + (m_root / kManifestName).string();
            return false;
        }
        f << j.dump(2) << "\n";
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    return true;
}

void Project::close() {
    m_open = false;
    m_root.clear();
    m_manifest = Manifest{};
}

std::vector<std::string> Project::maps() const {
    std::vector<std::string> out;
    if (!m_open) return out;
    std::error_code ec;
    const fs::path dir = m_root / "assets" / "external";
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it) {
        if (!it->is_directory(ec)) continue;
        const std::string n = it->path().filename().string();
        if (fs::is_regular_file(it->path() / (n + ".glb"), ec)) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Project::readFraming(const json& j, Manifest& m) {
    if (j.contains("spawn") && j["spawn"].is_array() && j["spawn"].size() == 3) {
        for (int i = 0; i < 3; ++i) m.spawn[i] = j["spawn"][i].get<float>();
        m.hasSpawn = true;
    }
    if (j.contains("camera") && j["camera"].is_object()) {
        const json& c = j["camera"];
        auto numero = [&c](const char* chave) -> std::optional<float> {
            if (c.contains(chave) && c[chave].is_number()) return c[chave].get<float>();
            return std::nullopt;
        };
        m.cameraFov = numero("fov");
        m.cameraYaw = numero("yaw");
        m.cameraPitch = numero("pitch");
        m.cameraDistance = numero("distance");
        if (c.contains("keep_inside_map") && c["keep_inside_map"].is_boolean())
            m.cameraKeepInsideMap = c["keep_inside_map"].get<bool>();
    }
}

fs::path Project::requested(int argc, char** argv, bool* explicitlyAsked) {
    if (explicitlyAsked) *explicitlyAsked = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--project" || a == "-p") && i + 1 < argc) return fs::path(argv[i + 1]);
        if (a.rfind("--project=", 0) == 0) return fs::path(a.substr(10));
    }
    if (const char* e = std::getenv("ERUPTION_PROJECT"); e && *e) return fs::path(e);
    if (explicitlyAsked) *explicitlyAsked = false;
    try {
        std::ifstream f(kLauncherConfig);
        if (f) {
            json j;
            f >> j;
            if (j.contains("project") && j["project"].is_string()) {
                const std::string p = j["project"].get<std::string>();
                if (!p.empty()) return fs::path(p);
            }
        }
    } catch (const std::exception&) {
        // launcher_config.json quebrado nao impede abrir sem projeto
    }
    return {};
}

void Project::rememberLastOpened(const fs::path& dir) {
    try {
        json j = json::object();
        {
            std::ifstream f(kLauncherConfig);
            if (f) f >> j;
        }
        if (!j.is_object()) j = json::object();
        if (dir.empty()) j.erase("project");
        else j["project"] = dir.generic_string();
        std::ofstream f(kLauncherConfig);
        if (f) f << j.dump(2) << "\n";
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("[PROJECT] could not record the project in %s: %s", kLauncherConfig, e.what());
    }
}

namespace project {

fs::path game(const std::string& rel) {
    const Project& p = Project::instance();
    return (p.isOpen() ? p.root() : p.engineRoot()) / rel;
}

fs::path engine(const std::string& rel) {
    return Project::instance().engineRoot() / rel;
}

fs::path gameRef(const std::string& path) {
    const fs::path p(path);
    return p.is_absolute() ? p : game(path);
}

} // namespace project
} // namespace eruption
