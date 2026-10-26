// Visao de equipe no script: a tabela `vision` (core/Vision). O mapa nunca
// fecha: fora da visao a tela escurece e o motor esconde o que se move ali.
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"

#include <lua.h>
#include <lualib.h>

#include <cstring>

namespace eruption {

namespace {

Engine* g_engine = nullptr;

Vec2 checkGround(lua_State* L, int idx) {
    const float* v = luaL_checkvector(L, idx);
    return Vec2(v[0], v[2]);
}

float optField(lua_State* L, int idx, const char* key, float def) {
    if (!lua_istable(L, idx)) return def;
    lua_getfield(L, idx, key);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}

int vision_enable(lua_State* L) {
    Vision& v = g_engine->vision();
    const bool on = lua_isnoneornil(L, 1) || lua_toboolean(L, 1);
    v.setDim(optField(L, 2, "dim", v.dim()));
    v.setFade(optField(L, 2, "fade", 0.15f));
    v.setTeam(static_cast<int>(optField(L, 2, "team", static_cast<float>(v.team()))));
    v.setEnabled(on);
    return 0;
}

int vision_enabled(lua_State* L) {
    lua_pushboolean(L, g_engine->vision().enabled());
    return 1;
}

int vision_add_source(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    const Vec2 p = checkGround(L, 2);
    const float radius = static_cast<float>(luaL_checknumber(L, 3));
    const int team = static_cast<int>(luaL_optinteger(L, 4, 1));
    g_engine->vision().setSource(id, p, radius, team);
    return 0;
}

int vision_remove_source(lua_State* L) {
    lua_pushboolean(L, g_engine->vision().removeSource(luaL_checkstring(L, 1)));
    return 1;
}

int vision_zone(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    const Vec2 p = checkGround(L, 2);
    const float radius = static_cast<float>(luaL_checknumber(L, 3));
    bool reveal = false;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "mode");
        if (const char* m = lua_tostring(L, -1)) {
            if (std::strcmp(m, "reveal") == 0) reveal = true;
            else if (std::strcmp(m, "block") != 0) luaL_error(L, "vision.zone: mode is \"block\" or \"reveal\", not \"%s\"", m);
        }
        lua_pop(L, 1);
    }
    g_engine->vision().setZone(id, p, radius, reveal);
    return 0;
}

int vision_remove_zone(lua_State* L) {
    lua_pushboolean(L, g_engine->vision().removeZone(luaL_checkstring(L, 1)));
    return 1;
}

int vision_visible(lua_State* L) {
    lua_pushboolean(L, g_engine->vision().visible(checkGround(L, 1)));
    return 1;
}

} // namespace

void registerVisionApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    static const luaL_Reg fns[] = {{"enable", vision_enable},
                                   {"enabled", vision_enabled},
                                   {"add_source", vision_add_source},
                                   {"remove_source", vision_remove_source},
                                   {"zone", vision_zone},
                                   {"remove_zone", vision_remove_zone},
                                   {"visible", vision_visible},
                                   {nullptr, nullptr}};
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "vision");
}

const char* visionApiDefinitions() {
    return R"(
-- Team vision. The map is never closed: outside the vision of the sources it
-- only dims (the sky does not), and characters, effects and minimap markers
-- there are hidden. Off by default; off, nothing changes on screen.
declare vision: {
    -- opts: dim (fraction of brightness left outside, 0.05..1, default 0.35),
    -- fade (half-life in seconds of the edge, default 0.15), team (whose
    -- sources count, default 1). enable(false) turns it off.
    enable: (on: boolean?, opts: { dim: number?, fade: number?, team: number? }?) -> (),
    enabled: () -> boolean,
    -- A disc of vision on the ground (y is ignored). The same id moves it.
    add_source: (id: string | number, position: vector, radius: number, team: number?) -> (),
    remove_source: (id: string | number) -> boolean,
    -- mode "block" (default): inside, only a source that is also inside sees
    -- (a pit, a tall grass patch). "reveal": always visible, no source needed.
    zone: (id: string | number, position: vector, radius: number, opts: { mode: string? }?) -> (),
    remove_zone: (id: string | number) -> boolean,
    -- The exact rule, anywhere on the map (no edge, no fade): the radius of a
    -- source is the line. Always true while vision is off.
    visible: (position: vector) -> boolean,
}
)";
}

} // namespace eruption
