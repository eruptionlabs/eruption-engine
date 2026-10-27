// Tabela `telemetry` do Luau: eventos do jogo num JSONL (utils/EventLog) para
// balancear - dano, cura, morte, habilidade. Ligada por telemetry.open, pela
// opcao --game-telemetry <arquivo> ou por ERUPTION_GAME_TELEMETRY=<arquivo>.
// Desligada, emit volta na primeira linha: nada aloca.
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"
#include "core/Project.hpp"
#include "script/ScriptHost.hpp"
#include "utils/EventLog.hpp"

#include <lua.h>
#include <lualib.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

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
                                   {"stats", telemetry_stats},     {nullptr, nullptr}};
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
}
)";
}

} // namespace eruption
