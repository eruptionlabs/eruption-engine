// Tabela `telemetry` do Luau: eventos do jogo num JSONL (utils/EventLog) para
// balancear - dano, cura, morte, habilidade. Ligada por telemetry.open, pela
// opcao --game-telemetry <arquivo> ou por ERUPTION_GAME_TELEMETRY=<arquivo>.
// Desligada, emit volta na primeira linha: nada aloca.
// telemetry.export junta a pasta telemetry/ num .zip para o jogador mandar.
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "script/ScriptHost.hpp"
#include "utils/EventLog.hpp"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// Deflate do stb_image_write (implementado em utils/ImageUtils.cpp; o
// cabecalho nao declara): o motor nao leva zlib, e o .zip so' precisa disso.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int dataLen, int* outLen, int quality);

namespace eruption {

namespace {

namespace fs = std::filesystem;

Engine* g_engine = nullptr;
constexpr size_t kMaxChannel = 64;

EventLog& eventLog() { return EventLog::instance(); }

EventLog::Header header() {
    EventLog::Header h;
    if (Project::instance().isOpen()) h.project = Project::instance().manifest().name;
    if (g_engine) h.map = g_engine->currentMapName();
    return h;
}

void addField(lua_State* L, const char* key) {
    // Chave em -2, valor em -1.
    switch (lua_type(L, -1)) {
    case LUA_TNUMBER:
        eventLog().field(key, double(lua_tonumber(L, -1)));
        break;
    case LUA_TBOOLEAN:
        eventLog().field(key, lua_toboolean(L, -1) != 0);
        break;
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        eventLog().field(key, std::string_view(s, len));
        break;
    }
    case LUA_TVECTOR:
        eventLog().field(key, lua_tovector(L, -1), 3);
        break;
    default:
        // Tabela dentro de tabela fica de fora: o arquivo e' lido por
        // planilha, Python e LLM, e evento raso e' o que todos leem igual.
        luaL_error(L, "telemetry.emit: field '%s' must be a number, boolean, string or vector", key);
    }
}

int telemetry_emit(lua_State* L) {
    if (!eventLog().enabled()) return 0;
    size_t len = 0;
    const char* channel = luaL_checklstring(L, 1, &len);
    if (len == 0 || len > kMaxChannel || channel[0] == '_')
        luaL_error(L, "telemetry.emit: channel must have 1..64 characters and not start with '_'");
    const bool hasFields = !lua_isnoneornil(L, 2);
    if (hasFields) luaL_checktype(L, 2, LUA_TTABLE);
    eventLog().begin(std::string_view(channel, len));
    if (hasFields) {
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "telemetry.emit: field names must be strings");
            const char* key = lua_tostring(L, -2);
            if (!std::strcmp(key, "t") || !std::strcmp(key, "f") || !std::strcmp(key, "ch"))
                luaL_error(L, "telemetry.emit: '%s' is written by the engine", key);
            addField(L, key);
            lua_pop(L, 1);
        }
    }
    eventLog().commit();
    return 0;
}

int telemetry_open(lua_State* L) {
    fs::path path = luaL_checkstring(L, 1);
    double maxMb = 64.0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "max_mb");
        if (!lua_isnil(L, -1)) maxMb = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    if (!(maxMb > 0.0)) luaL_error(L, "telemetry.open: max_mb must be > 0");
    // Relativo: na pasta de dados do jogo, ao lado dos jogos salvos (num
    // jogo exportado ou no celular a pasta do projeto nao e' gravavel).
    if (path.is_relative()) path = ScriptHost::saveDir().parent_path() / "telemetry" / path;
    if (!eventLog().open(path, header(), static_cast<uint64_t>(maxMb * 1024.0 * 1024.0))) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, path.string().c_str());
    return 1;
}

int telemetry_close(lua_State*) {
    eventLog().close();
    return 0;
}

int telemetry_enabled(lua_State* L) {
    lua_pushboolean(L, eventLog().enabled());
    return 1;
}

int telemetry_stats(lua_State* L) {
    lua_pushnumber(L, double(eventLog().events()));
    lua_pushnumber(L, double(eventLog().enabled() ? eventLog().dropped() : 0));
    return 2;
}

// --- telemetry.export: .zip (PKWARE APPNOTE, sem zip64) -------------------

uint32_t crc32(const std::string& s) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::string& o, uint32_t v) {
    o.push_back(char(v & 0xFFu));
    o.push_back(char((v >> 8) & 0xFFu));
}
void put32(std::string& o, uint32_t v) {
    put16(o, v & 0xFFFFu);
    put16(o, v >> 16);
}

// Pasta que o jogador ve: Documentos (XDG_DOCUMENTS_DIR no Linux, que segue
// o idioma do sistema). No celular a pasta do app (o compartilhar vem depois).
// ERUPTION_TEST_EXPORT_DIR (testes) troca o destino.
fs::path exportDir() {
    if (const char* d = std::getenv("ERUPTION_TEST_EXPORT_DIR"); d && *d) return d;
#if defined(__ANDROID__)
    return ScriptHost::saveDir().parent_path() / "export";
#elif defined(_WIN32)
    if (const char* u = std::getenv("USERPROFILE"); u && *u) return fs::path(u) / "Documents";
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        const char* cfg = std::getenv("XDG_CONFIG_HOME");
        std::ifstream dirs((cfg && *cfg ? fs::path(cfg) : fs::path(home) / ".config") / "user-dirs.dirs");
        for (std::string line; std::getline(dirs, line);) {
            const std::string key = "XDG_DOCUMENTS_DIR=\"";
            if (line.rfind(key, 0) != 0 || line.size() < key.size() + 1 || line.back() != '"') continue;
            std::string v = line.substr(key.size(), line.size() - key.size() - 1);
            if (v.rfind("$HOME", 0) == 0) v = std::string(home) + v.substr(5);
            std::error_code ec;
            if (fs::is_directory(v, ec)) return v;
        }
        std::error_code ec;
        if (fs::is_directory(fs::path(home) / "Documents", ec)) return fs::path(home) / "Documents";
        return home;
    }
#endif
    return ScriptHost::saveDir().parent_path() / "export";
}

// Nome do arquivo: o pedido, so' com letra, numero, '-', '_' e '.', ou
// <jogo>-telemetry-<data>. Sempre .zip.
std::string exportName(const char* asked) {
    std::string name;
    for (const char* c = asked ? asked : ""; *c; ++c)
        if (std::isalnum(static_cast<unsigned char>(*c)) || *c == '-' || *c == '_' || *c == '.') name.push_back(*c);
    if (name.empty() || name.find_first_not_of('.') == std::string::npos) {
        std::string game = Project::instance().isOpen() ? Project::instance().manifest().name : "";
        game.erase(std::remove_if(game.begin(), game.end(),
                                  [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_'; }),
                   game.end());
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        name = (game.empty() ? std::string("game") : game) + "-telemetry-" + stamp;
    }
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".zip") != 0) name += ".zip";
    return name;
}

// Junta os arquivos de telemetry/ (cada um ate' a ultima linha inteira: o da
// sessao aberta esta' sendo escrito por outra thread, que grava a cada
// segundo) num .zip. Deflate quando encolhe, senao guardado como esta'.
bool writeTelemetryZip(const fs::path& srcDir, const fs::path& dest, int& files) {
    std::vector<fs::path> paths;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(srcDir, ec))
        if (e.is_regular_file(ec)) paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());

    std::tm tm{};
    const std::time_t now = std::time(nullptr);
    if (const std::tm* l = std::localtime(&now)) tm = *l;
    const uint32_t dosTime = uint32_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    const uint32_t dosDate = uint32_t(std::max(tm.tm_year - 80, 0) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);

    std::string zip, central;
    files = 0;
    for (const fs::path& p : paths) {
        std::ifstream in(p, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t end = data.rfind('\n');
        data.resize(end == std::string::npos ? 0 : end + 1);
        if (data.empty() || data.size() > 0x7FFFFFFFu) continue;

        // stbi devolve zlib (2 bytes de cabecalho + deflate + adler32 de 4);
        // o zip quer o deflate cru.
        int zlen = 0;
        unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(data.data()), int(data.size()), &zlen, 8);
        std::string body;
        uint32_t method = 0;
        if (z && zlen > 6 && size_t(zlen - 6) < data.size()) {
            body.assign(reinterpret_cast<const char*>(z) + 2, size_t(zlen - 6));
            method = 8;
        } else {
            body = data;
        }
        std::free(z);
        if (zip.size() + body.size() > 0xF0000000u) break;

        const std::string name = p.filename().string();
        const uint32_t crc = crc32(data), offset = uint32_t(zip.size());
        auto common = [&](std::string& o) {
            put16(o, 20);     // versao para extrair
            put16(o, 0x0800); // nome em UTF-8
            put16(o, method);
            put16(o, dosTime);
            put16(o, dosDate);
            put32(o, crc);
            put32(o, uint32_t(body.size()));
            put32(o, uint32_t(data.size()));
            put16(o, uint32_t(name.size()));
            put16(o, 0); // extra
        };
        put32(zip, 0x04034b50u);
        common(zip);
        zip += name;
        zip += body;

        put32(central, 0x02014b50u);
        put16(central, 20); // feito por
        common(central);
        put16(central, 0); // comentario
        put16(central, 0); // disco
        put16(central, 0); // atributos internos
        put32(central, 0); // atributos externos
        put32(central, offset);
        central += name;
        ++files;
    }
    const uint32_t centralAt = uint32_t(zip.size());
    zip += central;
    put32(zip, 0x06054b50u);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, uint32_t(files));
    put16(zip, uint32_t(files));
    put32(zip, uint32_t(central.size()));
    put32(zip, centralAt);
    put16(zip, 0);

    // Grava ao lado e troca: nada de .zip pela metade com o nome final.
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.write(zip.data(), std::streamsize(zip.size()))) return false;
    }
    fs::rename(tmp, dest, ec);
    return !ec;
}

// O .zip sai numa thread: 16 MB de sessao levam 229 ms no PC (medido) e
// passavam do vigia de 250 ms do script com duas chamadas; no celular, mais.
struct Exporter {
    std::thread worker;
    std::atomic<bool> busy{false};
    std::atomic<int> lastOk{-1}; // -1 = nenhum terminou; 0 falhou; 1 gravou
    ~Exporter() {
        if (worker.joinable()) worker.join();
    }
};

Exporter& exporter() {
    static Exporter e;
    return e;
}

void runExport(fs::path src, fs::path dest) {
    int files = 0;
    const bool ok = writeTelemetryZip(src, dest, files) && files > 0;
    if (ok) {
        ERUPTION_LOG_INFO("[TELEMETRY] export: %d arquivo(s) em %s", files, dest.string().c_str());
    } else {
        ERUPTION_LOG_WARN("[TELEMETRY] export falhou: %s", dest.string().c_str());
        std::error_code ec;
        fs::remove(dest, ec);
    }
    exporter().lastOk = ok ? 1 : 0;
    exporter().busy = false;
}

bool hasTelemetryFiles(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.file_size(ec) > 0) return true;
    return false;
}

int telemetry_export(lua_State* L) {
    const char* asked = luaL_optstring(L, 1, nullptr);
    Exporter& ex = exporter();
    const fs::path src = ScriptHost::saveDir().parent_path() / "telemetry";
    if (ex.busy || !hasTelemetryFiles(src)) {
        lua_pushnil(L);
        return 1;
    }
    const fs::path dest = exportDir() / exportName(asked);
    if (ex.worker.joinable()) ex.worker.join(); // o anterior ja' terminou (busy = false)
    ex.busy = true;
    ex.worker = std::thread(runExport, src, dest);
    lua_pushstring(L, dest.string().c_str());
    return 1;
}

int telemetry_exporting(lua_State* L) {
    lua_pushboolean(L, exporter().busy);
    const int last = exporter().lastOk;
    if (last < 0) lua_pushnil(L);
    else lua_pushboolean(L, last == 1);
    return 2;
}

} // namespace

void openTelemetryFromEnv() {
    // ERUPTION_GAME_TELEMETRY=<arquivo> (ou --game-telemetry): liga sem
    // mexer no script. O caminho vale como escrito (relativo a' pasta atual).
    const char* p = std::getenv("ERUPTION_GAME_TELEMETRY");
    if (!p || !*p || eventLog().enabled()) return;
    eventLog().open(p, header());
}

void registerTelemetryApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    static const luaL_Reg fns[] = {{"emit", telemetry_emit},       {"open", telemetry_open},
                                   {"close", telemetry_close},     {"enabled", telemetry_enabled},
                                   {"stats", telemetry_stats},     {"export", telemetry_export},
                                   {"exporting", telemetry_exporting}, {nullptr, nullptr}};
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setglobal(L, "telemetry");
}

const char* telemetryApiDefinitions() {
    return R"(
-- Game event log for balancing: one JSON object per line (JSONL). The first
-- line is {"ch":"_head", v, project, map, build, date, part}; each event is
-- {"t": game time in ms, "f": script frame, "ch": channel, ...your fields};
-- the last is {"ch":"_end", events, dropped}. tools/telemetry_summary.py
-- prints count, sum, mean, p50 and p95 per channel and field.
-- Also turned on by --game-telemetry <file> or ERUPTION_GAME_TELEMETRY.
-- While off, emit costs nothing. Writing happens off the game thread through
-- a fixed 1 MB buffer: if it fills, events are dropped and counted, the
-- frame never waits for the disk.
declare telemetry: {
    -- Records an event. Fields are flat: number, boolean, string (cut at 512
    -- bytes) or vector; t, f and ch are reserved. Channels starting with "_"
    -- belong to the engine.
    emit: (channel: string, fields: { [string]: any }?) -> (),
    -- Starts a new file (truncates it) and returns its full path, or nil. A
    -- relative path lands in the game's data folder, next to the saves, under
    -- telemetry/. Past max_mb (default 64) the file becomes <file>.1 and a
    -- new part starts.
    open: (path: string, options: { max_mb: number? }?) -> string?,
    -- Writes what is pending and the "_end" line. Stopping the game closes too.
    close: () -> (),
    enabled: () -> boolean,
    -- Events written and dropped since open.
    stats: () -> (number, number),
    -- Packs every file in telemetry/ into one .zip (each cut at its last full
    -- line; the open session's newest ~1 s may be missing) and returns the
    -- path it will have, or nil when there is nothing to pack or an export is
    -- still running. The zip is written in the background and appears at that
    -- path whole when done (see exporting). On PC it lands in the user's
    -- Documents folder; the name is the one given (letters, digits, '-', '_',
    -- '.') or <game>-telemetry-<date>.zip.
    export: (name: string?) -> string?,
    -- Whether an export is still being written, and whether the last one
    -- that finished succeeded (nil before any finished).
    exporting: () -> (boolean, boolean?),
}
)";
}

} // namespace eruption
