#include "editor/GameExport.hpp"
#include "core/GraphicsMerge.hpp"
#include "core/Logger.hpp"
#include "formats/GroundPack.hpp"

#include "scene/SceneEdits.hpp"
#include "script/LuauChunk.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <map>
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

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Efeitos de partícula são citados pelo NOME ("tocha"), não pelo caminho.
const std::string kEffectSuffix = ".vfx.json";

// Arquivos do jogo que citam outros. As configurações do motor (data/ e as
// tabelas de assets/data) não entram: são copiadas como estão, e as chaves
// delas não são caminhos do projeto. Efeitos citam texturas e valem mesmo
// quando são modelos do motor (data/vfx).
bool followsReferences(const fs::path& rel) {
    const std::string e = rel.extension().string();
    const std::string generic = rel.generic_string();
    if (endsWith(rel.filename().string(), kEffectSuffix)) return true;
    if (generic.rfind("data/", 0) == 0 || generic.rfind("assets/data/", 0) == 0) return false;
    return e == ".luau" || e == ".env" || e == ".css" || e == ".json";
}

// Glob de um componente só: '*' casa qualquer trecho, o resto é literal.
bool globMatch(const std::string& pattern, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// Fim de um colchete longo do Luau aberto em `i` ("[[", "[=[", "[==[" ...):
// devolve a posição logo depois do fechamento, ou npos se não é colchete longo.
size_t longBracketEnd(const std::string& text, size_t i) {
    if (i >= text.size() || text[i] != '[') return std::string::npos;
    size_t level = 0, k = i + 1;
    while (k < text.size() && text[k] == '=') ++level, ++k;
    if (k >= text.size() || text[k] != '[') return std::string::npos;
    const std::string close = "]" + std::string(level, '=') + "]";
    const size_t end = text.find(close, k + 1);
    return end == std::string::npos ? text.size() : end + close.size();
}

// Texto de script sem os comentários (a detecção de módulo não pode ligar o
// som porque um comentário fala de audio.play). Strings ficam inteiras: um
// "--" dentro de um caminho não é comentário.
std::string withoutLuauComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '"' || c == '\'' || c == '`') {
            size_t k = i + 1;
            while (k < text.size() && text[k] != c && text[k] != '\n') k += text[k] == '\\' ? 2 : 1;
            k = std::min(k + 1, text.size());
            out.append(text, i, k - i);
            i = k;
            continue;
        }
        if (c == '[') {
            const size_t end = longBracketEnd(text, i);
            if (end != std::string::npos) {
                out.append(text, i, end - i);
                i = end;
                continue;
            }
        }
        if (text.compare(i, 2, "--") != 0) {
            out += text[i++];
            continue;
        }
        const size_t block = longBracketEnd(text, i + 2);
        if (block != std::string::npos) {
            i = block;
            continue;
        }
        const size_t eol = text.find('\n', i);
        i = eol == std::string::npos ? text.size() : eol;
    }
    return out;
}

// O grafo de dependência do jogo exportado. Os caminhos são relativos à raiz
// do projeto; cada um é procurado no projeto e, na falta, no motor.
class ExportGraph {
public:
    ExportGraph(const fs::path& root, const fs::path& engineRoot) : m_root(root), m_engineRoot(engineRoot) {}

    const std::set<fs::path>& files() const { return m_files; }
    std::vector<std::string>& missing() { return m_missing; }

    fs::path sourceOf(const fs::path& rel) const {
        std::error_code ec;
        if (fs::is_regular_file(m_root / rel, ec)) return m_root / rel;
        if (fs::is_regular_file(m_engineRoot / rel, ec)) return m_engineRoot / rel;
        return {};
    }

    // export.exclude vale desde já: o que o arquivo tirado cita não entra por ele.
    void setExcludes(const std::vector<std::string>& patterns) {
        for (const auto& pattern : patterns) {
            std::vector<std::string> parts;
            for (const auto& part : fs::path(pattern).lexically_normal()) parts.push_back(part.string());
            m_excludes.push_back(std::move(parts));
        }
    }

    bool excluded(const fs::path& rel) const {
        std::vector<std::string> path;
        for (const auto& part : rel) path.push_back(part.string());
        for (const auto& pat : m_excludes)
            if (pathMatches(pat, 0, path, 0)) return true;
        return false;
    }

    size_t excludedCount() const { return m_excludedHits.size(); }

    bool add(const fs::path& rel) {
        const fs::path norm = rel.lexically_normal();
        if (norm.empty() || norm.native().rfind("..", 0) == 0 || sourceOf(norm).empty()) return false;
        if (!m_excludes.empty() && excluded(norm)) {
            m_excludedHits.insert(norm);
            return true; // existe; o autor pediu para não levar
        }
        if (m_files.insert(norm).second && followsReferences(norm)) m_scan.push_back(norm);
        return true;
    }

    // Pasta inteira (recursiva), do projeto e do motor.
    void addDir(const fs::path& rel, bool (*keep)(const fs::path&)) {
        for (const fs::path& base : {m_root, m_engineRoot}) {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(base / rel, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && (!keep || keep(it->path()))) add(it->path().lexically_relative(base));
        }
    }

    // Só os arquivos do topo da pasta (sem descer), do projeto e do motor.
    void addTopFiles(const fs::path& rel, const std::string& skipName) {
        for (const fs::path& base : {m_root, m_engineRoot}) {
            std::error_code ec;
            for (fs::directory_iterator it(base / rel, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && it->path().filename() != skipName) add(it->path().lexically_relative(base));
        }
    }

    // Biblioteca de efeitos por nome, na ordem de resolveEffectPath: o efeito
    // do jogo (assets/effects) esconde o modelo de mesmo nome do motor (data/vfx).
    void indexEffects() {
        indexEffectDir(m_root / "assets" / "effects", fs::path("assets") / "effects");
        indexEffectDir(m_engineRoot / "data" / "vfx", fs::path("data") / "vfx");
    }

    // Lê os arquivos que citam outros até o grafo fechar.
    void resolve() {
        while (!m_scan.empty()) {
            const fs::path rel = m_scan.front();
            m_scan.pop_front();
            // Comentário de script não puxa arquivo nenhum para o jogo.
            const std::string raw = readText(sourceOf(rel));
            const std::string text = rel.extension() == ".luau" ? withoutLuauComments(raw) : raw;
            followQuoted(text);
            followTemplates(text);
            if (rel.extension() == ".css") followCssValues(text);
        }
    }

    // Raiz extra vinda do projeto: caminho literal ou glob ('*' por
    // componente, "**" desce em qualquer profundidade).
    void addPattern(const std::string& pattern) {
        const fs::path p = fs::path(pattern).lexically_normal();
        if (p.is_absolute() || p.native().rfind("..", 0) == 0) {
            m_missing.push_back(pattern + " (export.include outside the project)");
            return;
        }
        if (pattern.find('*') == std::string::npos) {
            if (!add(p)) m_missing.push_back(pattern + " (export.include)");
            return;
        }
        std::vector<std::string> parts;
        for (const auto& part : p) parts.push_back(part.string());
        // Só a pasta do projeto: um "**" não varre o motor (build, dependências).
        m_globHits = 0;
        expandGlob(m_root, fs::path(), parts, 0);
        if (m_globHits == 0) m_missing.push_back(pattern + " (export.include matched nothing)");
    }

    // Tira do grafo o que casar com o glob (antes da cópia).
    size_t remove(const std::string& pattern) {
        std::vector<std::string> parts;
        for (const auto& part : fs::path(pattern).lexically_normal()) parts.push_back(part.string());
        size_t removed = 0;
        for (auto it = m_files.begin(); it != m_files.end();) {
            std::vector<std::string> path;
            for (const auto& part : *it) path.push_back(part.string());
            if (pathMatches(parts, 0, path, 0)) {
                it = m_files.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

private:
    void indexEffectDir(const fs::path& dir, const fs::path& rel) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string fn = it->path().filename().string();
            if (!it->is_regular_file(ec) || !endsWith(fn, kEffectSuffix)) continue;
            m_effects.emplace(fn.substr(0, fn.size() - kEffectSuffix.size()), rel / fn);
        }
    }

    // Caminhos entre aspas (duplas ou simples: CSS aceita as duas) e nomes de
    // efeito citados em qualquer string. "arquivo.glb#Nó" (um nó só, como em
    // objects.spawn) leva o arquivo.
    void followQuoted(const std::string& text) {
        // Aspas duplas aceitam apóstrofo no nome (O'Neil.png) e vice-versa.
        static const std::regex doubleQuoted(
            "\"([^\"\\n]{3,260}\\.(?:glb|png|spr|luau|ter|json|css|wav|ogg|mp3|flac))(?:#[^\"\\n]*)?\"");
        static const std::regex singleQuoted(
            "'([^'\\n]{3,260}\\.(?:glb|png|spr|luau|ter|json|css|wav|ogg|mp3|flac))(?:#[^'\\n]*)?'");
        for (std::sregex_iterator it(text.begin(), text.end(), doubleQuoted), end; it != end; ++it) followPath((*it)[1].str());
        for (std::sregex_iterator it(text.begin(), text.end(), singleQuoted), end; it != end; ++it) followPath((*it)[1].str());
        static const std::regex word("([\"'])([A-Za-z0-9_\\-]{1,64})\\1");
        for (std::sregex_iterator it(text.begin(), text.end(), word), end; it != end; ++it) {
            const auto fx = m_effects.find((*it)[2].str());
            if (fx != m_effects.end()) add(fx->second);
        }
    }

    // Valor de propriedade sem aspas no CSS (--image: assets/ui/hp.png;).
    void followCssValues(const std::string& text) {
        static const std::regex bare(":\\s*([^\"';\\s{}]{3,260}\\.(?:png|spr|css))\\s*[;}]");
        for (std::sregex_iterator it(text.begin(), text.end(), bare), end; it != end; ++it) followPath((*it)[1].str());
    }

    // Strings interpoladas do Luau (`assets/ui/portraits/{nome}.png`): o
    // caminho é montado na hora, então vão todos os arquivos da pasta que
    // casam com o modelo. O trecho entre chaves só pode estar no nome do arquivo.
    void followTemplates(const std::string& text) {
        static const std::regex tmpl("`([^`\\n]{3,260}\\.(?:glb|png|spr|luau|ter|json|css|wav|ogg|mp3|flac))`");
        static const std::regex hole("\\{[^}]*\\}");
        for (std::sregex_iterator it(text.begin(), text.end(), tmpl), end; it != end; ++it) {
            const std::string raw = (*it)[1].str();
            if (raw.find('{') == std::string::npos) {
                followPath(raw);
                continue;
            }
            const fs::path pattern = std::regex_replace(raw, hole, "*");
            if (pattern.parent_path().native().find('*') != std::string::npos) {
                m_missing.push_back(raw + " (folder built at run time: list it in project export.include)");
                continue;
            }
            if (!addMatching(pattern.parent_path(), pattern.filename().string()))
                m_missing.push_back(raw + " (no file matches)");
        }
    }

    void followPath(const std::string& raw) {
        const fs::path ref = raw;
        if (ref.is_absolute()) {
            // Fora do projeto não vai junto (não roda em outra máquina).
            m_missing.push_back(ref.string() + " (outside the project)");
            return;
        }
        if (!add(ref)) {
            m_missing.push_back(ref.generic_string());
            return;
        }
        // Sprite: o .spr do importador mora ao lado do PNG.
        if (ref.extension() == ".png") add(fs::path(ref).replace_extension(".spr"));
    }

    bool addMatching(const fs::path& dir, const std::string& namePattern) {
        bool any = false;
        for (const fs::path& base : {m_root, m_engineRoot}) {
            std::error_code ec;
            for (fs::directory_iterator it(base / dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || !globMatch(namePattern, it->path().filename().string())) continue;
                const fs::path rel = dir / it->path().filename();
                any = add(rel) || any;
                if (rel.extension() == ".png") add(fs::path(rel).replace_extension(".spr"));
            }
        }
        return any;
    }

    void expandGlob(const fs::path& base, const fs::path& rel, const std::vector<std::string>& parts, size_t i) {
        if (i == parts.size()) {
            if (add(rel)) ++m_globHits;
            return;
        }
        std::error_code ec;
        if (parts[i] == "**") {
            expandGlob(base, rel, parts, i + 1);
            // Pasta ligada por symlink não entra no "**" (um laço desceria para sempre).
            for (fs::directory_iterator it(base / rel, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_directory(ec) && !it->is_symlink(ec)) expandGlob(base, rel / it->path().filename(), parts, i);
            return;
        }
        if (parts[i].find('*') == std::string::npos) {
            expandGlob(base, rel / parts[i], parts, i + 1);
            return;
        }
        for (fs::directory_iterator it(base / rel, ec), end; !ec && it != end; it.increment(ec))
            if (globMatch(parts[i], it->path().filename().string())) expandGlob(base, rel / it->path().filename(), parts, i + 1);
    }

    static bool pathMatches(const std::vector<std::string>& pat, size_t i, const std::vector<std::string>& path, size_t j) {
        if (i == pat.size()) return j == path.size();
        if (pat[i] == "**") {
            for (size_t k = j; k <= path.size(); ++k)
                if (pathMatches(pat, i + 1, path, k)) return true;
            return false;
        }
        return j < path.size() && globMatch(pat[i], path[j]) && pathMatches(pat, i + 1, path, j + 1);
    }

    fs::path m_root, m_engineRoot;
    std::set<fs::path> m_files;     // relativos à raiz do projeto
    std::deque<fs::path> m_scan;    // arquivos de texto ainda não lidos
    std::map<std::string, fs::path> m_effects; // nome -> arquivo do efeito
    std::vector<std::string> m_missing;
    std::vector<std::vector<std::string>> m_excludes;
    std::set<fs::path> m_excludedHits;
    size_t m_globHits = 0; // quantos arquivos o último glob casou (mesmo os que já iam)
};

bool isSceneFile(const fs::path& p) { return endsWith(p.filename().string(), ".scene.luau"); }
bool isSpirv(const fs::path& p) { return p.extension() == ".spv"; }
// A grade de sondas cozida (<mapa>.glb.probes) e' do pacote do celular; o
// desktop recoze na carga e um arquivo velho so' pesaria no export.
bool notProbeCache(const fs::path& p) { return p.extension() != ".probes"; }
bool isGameScript(const fs::path& p) { return p.extension() == ".luau" && p.filename() != "eruption.d.luau"; }

// Mundo contínuo: os mapas emendados (nos dois sentidos) vão junto.
std::vector<std::string> withNeighborMaps(const fs::path& root, std::vector<std::string> maps) {
    std::vector<std::pair<std::string, std::string>> links;
    std::error_code ec;
    for (fs::directory_iterator it(root / "assets" / "scenes", ec), end; !ec && it != end; it.increment(ec)) {
        if (!isSceneFile(it->path())) continue;
        const std::string fn = it->path().filename().string();
        SceneEdits e;
        std::string error;
        if (readText(it->path()).find("neighbors") == std::string::npos || !e.load(it->path(), error)) continue;
        const std::string self = fn.substr(0, fn.size() - std::string(".scene.luau").size());
        for (const auto& n : e.neighbors) links.push_back({self, n.map});
    }
    for (size_t i = 0; i < maps.size(); ++i)
        for (const auto& [a, b] : links) {
            const std::string other = a == maps[i] ? b : (b == maps[i] ? a : std::string());
            if (!other.empty() && std::find(maps.begin(), maps.end(), other) == maps.end()) maps.push_back(other);
        }
    return maps;
}

// Bloco "export" do project.eruption: { "include": [globs], "exclude": [globs] }.
void readProjectExportRules(const fs::path& root, std::vector<std::string>& include, std::vector<std::string>& exclude) {
    std::ifstream f(root / "project.eruption");
    if (!f) return;
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object() || !j.contains("export") || !j["export"].is_object()) return;
    const nlohmann::json& ex = j["export"];
    for (const char* key : {"include", "exclude"}) {
        if (!ex.contains(key) || !ex[key].is_array()) continue;
        for (const auto& v : ex[key])
            if (v.is_string()) (std::string(key) == "include" ? include : exclude).push_back(v.get<std::string>());
    }
}

// CHAO EMPACOTADO: o <mapa>.glb + <mapa>.ter de um mapa gerado vao como um
// <mapa>.relevo (formats/GroundPack: 30 MB -> 1,45 MB num mapa de 601x601),
// que a carga desempacota. So' quando a volta reproduz os dois; senao o par
// vai como esta' e o relatorio diz por que.
std::set<fs::path> packGrounds(const ExportOptions& o, const ExportGraph& graph, ExportReport& report) {
    std::set<fs::path> packed;
    const std::set<fs::path>& files = graph.files();
    for (const auto& rel : files) {
        if (rel.extension() != ".glb") continue;
        const fs::path ter = fs::path(rel).replace_extension(".ter");
        if (!files.count(ter)) continue;
        std::error_code ec;
        const fs::path dst = o.outDir / fs::path(rel).replace_extension(".relevo");
        fs::create_directories(dst.parent_path(), ec);
        std::string error;
        if (!packGround(graph.sourceOf(rel), graph.sourceOf(ter), dst, error)) {
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

bool copyGameFiles(const ExportOptions& o, const ExportGraph& graph, ExportReport& report) {
    const std::set<fs::path> packed = packGrounds(o, graph, report);
    for (const auto& rel : graph.files()) {
        if (packed.count(rel)) continue;
        std::error_code ec;
        const fs::path dst = o.outDir / rel;
        fs::create_directories(dst.parent_path(), ec);
        if (rel.extension() == ".luau") {
            // Scripts e arquivos de cena vão compilados: o jogo não leva o compilador.
            std::string err;
            const std::string bc = compileLuauForExport(readText(graph.sourceOf(rel)), err);
            if (bc.empty()) {
                report.error = rel.generic_string() + ": " + err;
                return false;
            }
            std::ofstream(dst, std::ios::binary | std::ios::trunc).write(bc.data(), std::streamsize(bc.size()));
            ++report.compiled;
        } else {
            fs::copy_file(graph.sourceOf(rel), dst, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            report.error = "Could not copy " + rel.generic_string() + ": " + ec.message();
            return false;
        }
        ++report.files;
        report.bytes += fs::file_size(dst, ec);
    }
    return true;
}

// "export": {"icon": "<png>"} do project.eruption: vai como
// assets/icon/game.png e o launcher_config diz qual e' (Project::gameIcon).
void copyGameIcon(const ExportOptions& o, const nlohmann::json& pj, nlohmann::json& launch, ExportReport& report) {
    if (!pj.contains("export") || !pj["export"].is_object()) return;
    const std::string icon = pj["export"].value("icon", std::string());
    if (icon.empty()) return;
    std::error_code ec;
    const fs::path dst = o.outDir / "assets" / "icon" / "game.png";
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(o.projectRoot / icon, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        ERUPTION_LOG_WARN("[EXPORT] icone %s: %s", icon.c_str(), ec.message().c_str());
        return;
    }
    launch["icon"] = "assets/icon/game.png";
    report.bytes += fs::file_size(dst, ec);
    ++report.files;
}

// Atalho do Linux (<pasta>/<jogo>.desktop) com o icone do launcher_config
// (o do jogo) ou o do motor. Caminhos absolutos (o .desktop nao aceita
// relativos): pasta movida, exportar de novo.
void writeDesktopEntry(const ExportOptions& o) {
#if defined(__linux__) && !defined(__ANDROID__)
    std::error_code ec;
    const fs::path dir = fs::absolute(o.outDir, ec);
    std::string name, icon = "assets/icon/eruption_v3_256.png";
    {
        std::ifstream pf(o.projectRoot / "project.eruption");
        const nlohmann::json pj = pf ? nlohmann::json::parse(pf, nullptr, false) : nlohmann::json();
        if (pj.is_object()) name = pj.value("name", std::string());
        std::ifstream lf(dir / "data" / "launcher_config.json");
        const nlohmann::json lj = lf ? nlohmann::json::parse(lf, nullptr, false) : nlohmann::json();
        if (lj.is_object() && lj.contains("icon") && lj["icon"].is_string()) icon = lj["icon"].get<std::string>();
    }
    if (name.empty()) name = dir.filename().string();
    std::string file;
    for (char c : name) file.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    const fs::path desktop = dir / (file + ".desktop");
    std::ofstream d(desktop);
    d << "[Desktop Entry]\nType=Application\nName=" << name << "\nExec=\"" << (dir / "run.sh").string() << "\"\nPath="
      << dir.string() << "\nIcon=" << (dir / icon).string() << "\nTerminal=false\nCategories=Game;\n";
    d.close();
    fs::permissions(desktop, fs::perms::owner_exec, fs::perm_options::add, ec);
#else
    (void)o;
#endif
}

// Executável, as bibliotecas que ele carrega ao lado e o run.sh.
bool copyProgram(const ExportOptions& o, ExportReport& report) {
    std::error_code ec;
    const fs::path exe = o.outDir / o.binary.filename();
    fs::copy_file(o.binary, exe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        report.error = "Could not copy the game program: " + ec.message();
        return false;
    }
    fs::permissions(exe, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    report.bytes += fs::file_size(exe, ec);
    ++report.files;
    for (fs::directory_iterator it(o.binary.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("libonnxruntime.so", 0) != 0) continue;
        std::error_code copyEc;
        fs::copy_file(it->path(), o.outDir / n, fs::copy_options::overwrite_existing, copyEc);
        report.bytes += fs::file_size(o.outDir / n, copyEc);
        ++report.files;
    }
    std::ofstream run(o.outDir / "run.sh");
    run << "#!/bin/sh\n# Starts the game from its own folder.\ncd \"$(dirname \"$0\")\" && exec ./"
        << exe.filename().string() << " --game \"$@\"\n";
    run.close();
    fs::permissions(o.outDir / "run.sh", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    writeDesktopEntry(o);
    return true;
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
    ExportGraph graph(root, o.engineRoot.empty() ? o.projectRoot : o.engineRoot);
    graph.indexEffects();
    const std::vector<std::string> maps = withNeighborMaps(root, o.maps);
    report.neighbors = maps.size() - o.maps.size();

    // Mapas e o que é deles.
    std::error_code ec;
    for (const auto& map : maps) {
        if (!fs::is_directory(root / "assets" / "external" / map, ec)) {
            report.error = "Map not found: " + map;
            return report;
        }
        graph.addDir(fs::path("assets") / "external" / map, notProbeCache);
        for (fs::directory_iterator it(root / "assets" / "scenes", ec), end; !ec && it != end; it.increment(ec))
            if (it->path().filename().string().rfind(map + ".", 0) == 0) graph.add(it->path().lexically_relative(root));
        graph.add(fs::path("data") / "climate" / (map + ".json"));
    }
    // Motor: configurações (sem o clima de outros mapas), shaders compilados,
    // materiais, ícones e sprite padrão do jogador.
    graph.addTopFiles("data", "launcher_config.json");
    graph.addDir("data/hud", nullptr);
    graph.addDir("assets/data", nullptr);
    graph.addDir("shaders", isSpirv);
    graph.addDir("assets/icon", nullptr); // ícone da janela e logo da abertura
    graph.addDir("assets/fonts", nullptr); // fonte da interface do jogo (Engine::initImGui)
    graph.add("assets/sprites/default.png");
    graph.add("assets/sprites/default.spr");
    // Scripts do jogo (o arquivo de tipos é só do editor).
    graph.addDir("assets/scripts", isGameScript);
    // Módulo C++ (native/): vai a última versão compilada, não os fontes.
    graph.add(fs::path(".eruption") / "native" / "game.so");
    // Raízes declaradas pelo autor (caminhos montados de um jeito que o
    // texto não mostra).
    std::vector<std::string> include, exclude;
    readProjectExportRules(root, include, exclude);
    graph.setExcludes(exclude);
    for (const auto& pattern : include) graph.addPattern(pattern);

    // Caminhos e nomes de efeito citados nos arquivos de texto: o resto do grafo.
    graph.resolve();
    size_t removed = 0;
    for (const auto& pattern : exclude) removed += graph.remove(pattern); // o que entrou antes da regra valer
    report.excluded = graph.excludedCount() + removed;

    fs::create_directories(o.outDir, ec);
    if (ec) {
        report.error = "Could not create " + o.outDir.string() + ": " + ec.message();
        return report;
    }
    if (!copyGameFiles(o, graph, report)) return report;
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
    // Configuração de partida: o mapa inicial, o enquadramento do jogo
    // ("spawn" e "camera" do project.eruption - sem eles o exportado abria com
    // a abertura padrão do motor em vez da do jogo) e a tela de carga
    // ("loading": sem ela o exportado mostrava a do motor). Nada desta máquina.
    nlohmann::json launch;
    launch["initial_map"] = o.maps.front();
    {
        std::ifstream pf(o.projectRoot / "project.eruption");
        const nlohmann::json pj = pf ? nlohmann::json::parse(pf, nullptr, false) : nlohmann::json();
        if (pj.is_object()) {
            for (const char* key : {"name", "spawn", "camera", "loading"})
                if (pj.contains(key)) launch[key] = pj[key];
            copyGameIcon(o, pj, launch, report);
        }
    }
    fs::create_directories(o.outDir / "data", ec);
    std::ofstream(o.outDir / "data" / "launcher_config.json") << launch.dump(1) << "\n";
    if (!copyProgram(o, report)) return report;
    report.missing = std::move(graph.missing());
    std::sort(report.missing.begin(), report.missing.end());
    report.missing.erase(std::unique(report.missing.begin(), report.missing.end()), report.missing.end());
    return report;
}

// ---------------------------------------------------------------------------
// Módulos opcionais (cmake/modules.json)
// ---------------------------------------------------------------------------

namespace {

std::string jsonString(const nlohmann::json& j, const char* key, const std::string& def) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : def;
}

bool jsonBool(const nlohmann::json& j, const char* key, bool def) {
    return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : def;
}

std::vector<std::string> stringList(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const auto& v : j[key])
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

// Algum arquivo da pasta (recursiva, pelas extensões dadas) casa com a regra?
bool anyFileMatches(const fs::path& dir, const std::vector<std::string>& exts, const std::regex& rule) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        if (std::find(exts.begin(), exts.end(), p.extension().string()) == exts.end()) continue;
        if (endsWith(p.filename().string(), ".d.luau")) continue; // tipos da API citam todos os módulos
        std::string text = readText(p);
        if (p.extension() == ".luau") text = withoutLuauComments(text);
        if (std::regex_search(text, rule)) return true;
    }
    return false;
}

std::string regexAlternatives(const std::vector<std::string>& words) {
    static const std::regex special("[.^$|()\\[\\]{}*+?\\\\]");
    std::string out;
    for (const auto& w : words) out += (out.empty() ? "" : "|") + std::regex_replace(w, special, "\\$&");
    return out;
}

} // namespace

std::vector<ModuleInfo> loadModuleTable(const fs::path& engineRoot, std::string* error) {
    std::vector<ModuleInfo> table;
    std::ifstream f(engineRoot / "cmake" / "modules.json");
    const nlohmann::json j = f ? nlohmann::json::parse(f, nullptr, false) : nlohmann::json();
    if (!j.is_object() || !j.contains("modules") || !j["modules"].is_array()) {
        if (error) *error = "cmake/modules.json missing or invalid";
        return table;
    }
    for (const auto& m : j["modules"]) {
        // Entrada torta (não é objeto, campo do tipo errado) é pulada, não derruba o editor.
        if (!m.is_object()) continue;
        ModuleInfo info;
        info.name = jsonString(m, "name", "");
        info.option = jsonString(m, "option", "");
        info.label = jsonString(m, "label", info.name);
        info.summary = jsonString(m, "summary", "");
        info.defaultOn = jsonBool(m, "default", true);
        info.exportable = jsonBool(m, "export", true);
        if (m.contains("detect") && m["detect"].is_object()) {
            info.scriptGlobals = stringList(m["detect"], "script_globals");
            info.sceneFields = stringList(m["detect"], "scene_fields");
        }
        if (!info.option.empty()) table.push_back(std::move(info));
    }
    return table;
}

// Detecção pela API de script (`global.` ou `global:` fora de comentário) e
// pelos campos de cena (`campo =` nas cenas Luau, `"campo":` nos .env/.json).
bool moduleUsedByProject(const ModuleInfo& m, const fs::path& root) {
    if (!m.scriptGlobals.empty()) {
        const std::regex call("(^|[^A-Za-z0-9_.])(" + regexAlternatives(m.scriptGlobals) + ")\\s*[.:]");
        if (anyFileMatches(root / "assets" / "scripts", {".luau"}, call) ||
            anyFileMatches(root / "assets" / "scenes", {".luau"}, call))
            return true;
    }
    if (!m.sceneFields.empty()) {
        const std::string fields = regexAlternatives(m.sceneFields);
        const std::regex luauField("(^|[^A-Za-z0-9_])(" + fields + ")\\s*=\\s*\\{");
        const std::regex jsonField("\"(" + fields + ")\"\\s*:\\s*\\[\\s*\\{");
        if (anyFileMatches(root / "assets" / "scenes", {".luau"}, luauField) ||
            anyFileMatches(root / "assets" / "external", {".env"}, jsonField))
            return true;
    }
    return false;
}

bool builtProfile(const fs::path& buildDir, const std::vector<ModuleInfo>& table, BuildProfile& out) {
    std::ifstream f(buildDir / "CMakeCache.txt");
    if (!f) return false;
    std::string line;
    bool found = false;
    while (std::getline(f, line))
        for (const auto& m : table)
            if (line.rfind(m.option + ":BOOL=", 0) == 0) {
                out.modules[m.option] = line.substr(line.find('=') + 1) == "ON";
                found = true;
            }
    return found;
}

std::string profileCMakeFlags(const std::vector<ModuleInfo>& table, const BuildProfile& p) {
    std::string flags;
    for (const auto& m : table) {
        const auto it = p.modules.find(m.option);
        const bool on = m.exportable && it != p.modules.end() && it->second;
        flags += (flags.empty() ? "" : " ") + std::string("-D") + m.option + (on ? "=ON" : "=OFF");
    }
    return flags;
}

} // namespace eruption
