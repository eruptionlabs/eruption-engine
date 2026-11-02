// Rede no script: net.host / net.join / net.leave, jogadores, eventos
// confiáveis (net.on, net.send, net.send_to, net.ask) e números (net.stats).
// A rede mesma está em src/net (módulo opcional `net`); aqui só a ponte com
// o Luau. Com o módulo desligado tudo existe e nada conecta.
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "net/Net.hpp"
#include "script/ScriptHost.hpp"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace eruption {

namespace {

Engine* g_engine = nullptr;
constexpr size_t kMaxEventName = 32;
constexpr size_t kMaxEventBytesScript = 900;

NetSystem& net() { return NetSystem::instance(); }

// Chamada do lado errado (net.send no cliente...): avisa uma vez por nome.
void warnOnce(const char* what) {
    static std::set<std::string> warned;
    if (warned.insert(what).second) ERUPTION_LOG_WARN("%s", what);
}

std::string checkEventName(lua_State* L, int idx) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    if (len == 0 || len > kMaxEventName) luaL_error(L, "net: event name must have 1 to %d bytes", int(kMaxEventName));
    return std::string(s, len);
}

bool toScalar(lua_State* L, int idx, NetValue& out) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL: out = NetValue(); return true;
        case LUA_TBOOLEAN: out = NetValue::makeBool(lua_toboolean(L, idx) != 0); return true;
        case LUA_TNUMBER: {
            const double v = lua_tonumber(L, idx);
            if (!std::isfinite(v)) return false;
            out = NetValue::makeNumber(v);
            return true;
        }
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            if (len > kNetValueMaxString) return false;
            out = NetValue::makeString(std::string(s, len));
            return true;
        }
        default: return false;
    }
}

// `dados`: número, bool, string curta ou tabela rasa desses (erro de quem
// escreve o jogo se não for: aparece no console na hora).
NetValue checkValue(lua_State* L, int idx) {
    NetValue v;
    if (lua_type(L, idx) != LUA_TTABLE) {
        if (!toScalar(L, idx, v))
            luaL_error(L, "net: data must be a number, boolean, string (up to %d bytes) or a flat table",
                       int(kNetValueMaxString));
        return v;
    }
    v.type = NetValue::Type::Table;
    const int t = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        NetValue key, value;
        const int kt = lua_type(L, -2);
        if ((kt != LUA_TNUMBER && kt != LUA_TSTRING) || !toScalar(L, -2, key) || lua_type(L, -1) == LUA_TTABLE ||
            !toScalar(L, -1, value)) {
            luaL_error(L, "net: table data must be flat (number/string keys, number/boolean/string values)");
        }
        if (v.table.size() >= kNetValueMaxEntries) luaL_error(L, "net: table data has more than %d entries", int(kNetValueMaxEntries));
        v.table.emplace_back(std::move(key), std::move(value));
        lua_pop(L, 1);
    }
    return v;
}

void pushScalar(lua_State* L, const NetValue& v) {
    switch (v.type) {
        case NetValue::Type::Bool: lua_pushboolean(L, v.boolean); break;
        case NetValue::Type::Number: lua_pushnumber(L, v.number); break;
        case NetValue::Type::String: lua_pushlstring(L, v.string.data(), v.string.size()); break;
        default: lua_pushnil(L); break;
    }
}

void pushValue(lua_State* L, const NetValue& v) {
    if (v.type != NetValue::Type::Table) {
        pushScalar(L, v);
        return;
    }
    lua_createtable(L, 0, static_cast<int>(v.table.size()));
    for (const auto& kv : v.table) {
        pushScalar(L, kv.first);
        pushScalar(L, kv.second);
        lua_settable(L, -3);
    }
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// O que os dois lados têm de ter igual: mapa, faixas da cena, scripts do
// projeto e a versão que o jogo declarar. Vira o hash do hello.
std::vector<std::string> contentParts(const std::string& version) {
    std::vector<std::string> parts;
    parts.push_back("map:" + g_engine->currentMapName());
    for (const Engine::SceneRibbon& r : g_engine->ribbons()) {
        if (r.fromScript) continue; // as do script já entram pelos scripts
        std::string bytes = "ribbon:" + r.def.name + (r.def.closed ? ":c" : ":o");
        bytes.append(reinterpret_cast<const char*>(r.def.pointsXZ.data()), r.def.pointsXZ.size() * sizeof(float));
        bytes.append(reinterpret_cast<const char*>(r.def.widths.data()), r.def.widths.size() * sizeof(float));
        parts.push_back(std::move(bytes));
    }
    if (ScriptHost* host = ScriptHost::current()) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(host->dir(), ec), end; !ec && it != end; it.increment(ec)) {
            const std::filesystem::path& p = it->path();
            // As declarações de tipo são reescritas a cada início: ficam fora.
            if (p.extension() == ".luau" && p.filename() != "eruption.d.luau") files.push_back(p);
        }
        std::sort(files.begin(), files.end());
        for (const auto& p : files)
            parts.push_back("script:" + std::filesystem::relative(p, host->dir(), ec).generic_string() + ":" + readFile(p));
    }
    parts.push_back("version:" + version);
    return parts;
}

std::string optString(lua_State* L, int idx, const char* key, const char* def) {
    if (lua_type(L, idx) != LUA_TTABLE) return def;
    lua_getfield(L, idx, key);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

double optNumber(lua_State* L, int idx, const char* key, double def) {
    if (lua_type(L, idx) != LUA_TTABLE) return def;
    lua_getfield(L, idx, key);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

bool optBool(lua_State* L, int idx, const char* key, bool def) {
    if (lua_type(L, idx) != LUA_TTABLE) return def;
    lua_getfield(L, idx, key);
    const bool v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) != 0 : def;
    lua_pop(L, 1);
    return v;
}

// --- partida

int net_host(lua_State* L) {
    const int maxPlayers = static_cast<int>(optNumber(L, 1, "max_players", 4));
    const int port = static_cast<int>(optNumber(L, 1, "port", 41920));
    std::string error;
    if (!net().host(std::clamp(maxPlayers, 1, 16), port, optBool(L, 1, "dedicated", false),
                    optString(L, 1, "name", "jogador"), contentParts(optString(L, 1, "version", "")),
                    optString(L, 1, "relay", ""), optString(L, 1, "relay_key", ""), error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    const std::string address = net().address();
    lua_pushstring(L, address.c_str());
    return 1;
}

int net_join(lua_State* L) {
    const char* destination = luaL_checkstring(L, 1);
    lua_pushboolean(L, net().join(destination, optString(L, 2, "name", "jogador"),
                                  contentParts(optString(L, 2, "version", ""))));
    return 1;
}

int net_address(lua_State* L) {
    lua_pushstring(L, net().address().c_str());
    return 1;
}

int net_leave(lua_State*) {
    net().leave();
    return 0;
}

int net_state(lua_State* L) {
    lua_pushstring(L, net().state());
    return 1;
}

int net_is_server(lua_State* L) {
    lua_pushboolean(L, net().isServer());
    return 1;
}

int net_my_id(lua_State* L) {
    lua_pushinteger(L, net().myId());
    return 1;
}

int net_players(lua_State* L) {
    const std::vector<NetPlayerInfo> list = net().players();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (size_t i = 0; i < list.size(); ++i) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, list[i].id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, list[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, std::round(list[i].pingMs));
        lua_setfield(L, -2, "ping");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int net_stats(lua_State* L) {
    const NetStatsInfo s = net().stats();
    lua_createtable(L, 0, 6);
    lua_pushnumber(L, s.pingMs);
    lua_setfield(L, -2, "ping");
    lua_pushnumber(L, s.jitterMs);
    lua_setfield(L, -2, "jitter");
    lua_pushnumber(L, s.lossPercent);
    lua_setfield(L, -2, "loss");
    lua_pushnumber(L, s.kbpsIn);
    lua_setfield(L, -2, "kbps_in");
    lua_pushnumber(L, s.kbpsOut);
    lua_setfield(L, -2, "kbps_out");
    lua_pushinteger(L, s.players);
    lua_setfield(L, -2, "players");
    return 1;
}

// --- eventos (registrados como os on.*: no topo do script, liberados na recarga)

int addNetHandler(lua_State* L, int fnArg, const std::string& key) {
    luaL_checktype(L, fnArg, LUA_TFUNCTION);
    ScriptHost* host = ScriptHost::current();
    ScriptHost::Handler h;
    h.kind = ScriptHost::EventKind::Net;
    h.text = key;
    lua_pushvalue(L, fnArg);
    h.fnRef = lua_ref(L, -1);
    lua_pop(L, 1);
    if (!host || !host->addHandler(h)) {
        lua_unref(L, h.fnRef);
        luaL_errorL(L, "net.on* must be called at the top level of a script, not inside a function");
    }
    return 0;
}

int net_on_join(lua_State* L) { return addNetHandler(L, 1, "@join"); }
int net_on_leave(lua_State* L) { return addNetHandler(L, 1, "@leave"); }
int net_on_fail(lua_State* L) { return addNetHandler(L, 1, "@fail"); }

int net_on(lua_State* L) {
    const std::string name = checkEventName(L, 1);
    if (optBool(L, 3, "from_client", false)) {
        const double maxBytes = optNumber(L, 3, "max_bytes", 256);
        net().allowFromClient(name, static_cast<size_t>(std::clamp(maxBytes, 1.0, double(kMaxEventBytesScript))));
    }
    return addNetHandler(L, 2, name);
}

int net_send(lua_State* L) {
    const std::string name = checkEventName(L, 1);
    const NetValue v = checkValue(L, 2);
    if (!net().isServer()) {
        warnOnce("net.send: only the host or server sends to everyone (clients use net.ask)");
        return 0;
    }
    if (!net().send(name, v)) warnOnce("net.send: data too large or match closed");
    return 0;
}

int net_send_to(lua_State* L) {
    const int player = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string name = checkEventName(L, 2);
    const NetValue v = checkValue(L, 3);
    if (!net().isServer()) {
        warnOnce("net.send_to: only the host or server sends to a player");
        return 0;
    }
    lua_pushboolean(L, net().sendTo(player, name, v));
    return 1;
}

int net_ask(lua_State* L) {
    const std::string name = checkEventName(L, 1);
    const NetValue v = checkValue(L, 2);
    if (std::string(net().state()) == "offline" || net().myId() == 0) {
        warnOnce("net.ask: not in a match as a player");
        return 0;
    }
    if (!net().ask(name, v)) warnOnce("net.ask: data too large or match closed");
    return 0;
}

} // namespace

std::string netEventKey(const NetScriptEvent& e) {
    switch (e.kind) {
        case NetScriptEvent::Kind::Join: return "@join";
        case NetScriptEvent::Kind::Leave: return "@leave";
        case NetScriptEvent::Kind::Fail: return "@fail";
        case NetScriptEvent::Kind::Message: return e.name;
    }
    return {};
}

int scriptPushNetEvent(lua_State* L, const NetScriptEvent& e) {
    switch (e.kind) {
        case NetScriptEvent::Kind::Join:
            lua_pushinteger(L, e.player);
            lua_pushstring(L, e.name.c_str());
            return 2;
        case NetScriptEvent::Kind::Leave:
            lua_pushinteger(L, e.player);
            lua_pushstring(L, e.reason.c_str());
            return 2;
        case NetScriptEvent::Kind::Fail:
            lua_pushstring(L, e.reason.c_str());
            return 1;
        case NetScriptEvent::Kind::Message:
            lua_pushinteger(L, e.player);
            pushValue(L, e.value);
            return 2;
    }
    return 0;
}

void registerNetApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    static const luaL_Reg fns[] = {
        {"host", net_host},         {"join", net_join},       {"leave", net_leave}, {"address", net_address},
        {"state", net_state},       {"is_server", net_is_server}, {"my_id", net_my_id},
        {"players", net_players},   {"stats", net_stats},     {"on_join", net_on_join},
        {"on_leave", net_on_leave}, {"on_fail", net_on_fail}, {"on", net_on},
        {"send", net_send},         {"send_to", net_send_to}, {"ask", net_ask},
        {nullptr, nullptr}};
    lua_createtable(L, 0, 16);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, true);
    lua_setglobal(L, "net");
}

const char* netApiDefinitions() {
    return R"(
-- Multiplayer. One player hosts (their game is the server) or a dedicated
-- server runs the match; the others join by "ip:port". Both sides must have
-- the same map, scene ribbons, scripts and `version`, or the join fails with
-- "conteudo_diferente". Events are reliable and ordered.
-- Reasons in on_leave/on_fail: "saiu", "caiu", "cheio", "sem_resposta",
-- "conteudo_diferente", "mensagem_invalida", "excesso", "servidor_saiu",
-- "endereco_invalido", "token_vencido".
declare net: {
    -- Opens a match. opts: max_players (1..16, default 4), port (default
    -- 41920, 0 = any), name (local player), version (game version string),
    -- dedicated (no local player), relay ("ip:port" of a VPS running
    -- eruption-relay, for hosts behind carrier NAT) and relay_key (the
    -- relay's 64 hex digit key). Returns "ip:port" for the others to join
    -- ("" while the relay has not answered yet: read net.address()), or nil
    -- and the reason.
    host: (opts: { [string]: any }?) -> (string?, string?),
    -- destination: "ip", "ip:port" or "localhost". opts: name, version.
    -- The result arrives in on_join (success) or on_fail.
    join: (destination: string, opts: { [string]: any }?) -> boolean,
    leave: () -> (),
    -- Host: the address the others join ("" while waiting for the relay).
    address: () -> string,
    -- "offline" | "connecting" | "connected" | "hosting" | "server"
    state: () -> string,
    -- True on the host and on a dedicated server (the side that decides).
    is_server: () -> boolean,
    -- Local player id (0 = dedicated server or not in a match).
    my_id: () -> number,
    players: () -> { { id: number, name: string, ping: number } },
    stats: () -> { ping: number, jitter: number, loss: number, kbps_in: number, kbps_out: number, players: number },
    -- Top level of a script, like on.*. on_join also fires for players already
    -- in the match when you enter, and for yourself.
    on_join: (fn: (id: number, name: string) -> ()) -> (),
    on_leave: (fn: (id: number, reason: string) -> ()) -> (),
    on_fail: (fn: (reason: string) -> ()) -> (),
    -- Event handler. `from` is the sender id (0 = server). Clients may only
    -- send names registered with opts.from_client = true (max_bytes, default
    -- 256); anything else disconnects them.
    on: (name: string, fn: (from: number, data: any) -> (), opts: { from_client: boolean?, max_bytes: number? }?) -> (),
    -- data: number, boolean, string (up to 200 bytes) or a flat table of those.
    -- Server -> everyone (the host's own player included).
    send: (name: string, data: any) -> (),
    -- Server -> one player.
    send_to: (id: number, name: string, data: any) -> boolean,
    -- Player -> server (only names the server registered with from_client).
    ask: (name: string, data: any) -> (),
}
)";
}

} // namespace eruption
