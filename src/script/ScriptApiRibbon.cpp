// Faixas por spline no script: world.ribbon (monta uma faixa a partir de
// pontos) e as consultas world.ribbon_query / ribbon_point / ribbon_length.
// As consultas sao puras (mesma entrada, mesmo resultado): quem re-simula um
// passo de fisica varias vezes por quadro conta com isso.
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"

#include <lua.h>
#include <lualib.h>

#include <string>

namespace eruption {

namespace {

Engine* g_engine = nullptr;

const Ribbon& checkRibbon(lua_State* L, int idx) {
    const char* name = luaL_checkstring(L, idx);
    const Ribbon* r = g_engine->findRibbon(name);
    if (!r) luaL_error(L, "ribbon '%s' does not exist", name);
    return *r;
}

void setNum(lua_State* L, const char* key, float v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, key);
}

void setVec(lua_State* L, const char* key, float x, float y, float z) {
    lua_pushvector(L, x, y, z);
    lua_setfield(L, -2, key);
}

// A mesma tabela para a consulta e para o ponto: o que nao vale no ponto
// (normal da borda, lateral) sai zerado.
void pushQuery(lua_State* L, const RibbonQuery& q) {
    lua_createtable(L, 0, 10);
    setNum(L, "t", q.t);
    setNum(L, "s", q.s);
    setNum(L, "lateral", q.lateral);
    setNum(L, "edge", q.edge);
    setNum(L, "width", q.width);
    setNum(L, "height", q.height);
    setVec(L, "normal", q.nx, 0.0f, q.nz);
    setVec(L, "tangent", q.tx, 0.0f, q.tz);
    setVec(L, "position", q.x, q.height, q.z);
    lua_pushinteger(L, q.segment);
    lua_setfield(L, -2, "segment");
}

float optField(lua_State* L, int idx, const char* key, float def) {
    lua_getfield(L, idx, key);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}

// world.ribbon(name, points, opts): pontos como vector (y nao conta) ou {x, z}.
int world_ribbon(lua_State* L) {
    Engine::RibbonDef def;
    def.name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const int n = lua_objlen(L, 2);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        if (const float* v = lua_tovector(L, -1)) {
            def.pointsXZ.push_back(v[0]);
            def.pointsXZ.push_back(v[2]);
        } else if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            def.pointsXZ.push_back(static_cast<float>(lua_tonumber(L, -2)));
            def.pointsXZ.push_back(static_cast<float>(lua_tonumber(L, -1)));
            lua_pop(L, 2);
        } else {
            luaL_error(L, "world.ribbon: point %d is not a vector", i);
        }
        lua_pop(L, 1);
    }
    float width = 8.0f;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "closed");
        def.closed = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        width = optField(L, 3, "width", width);
        def.step = optField(L, 3, "step", def.step);
        def.uvLength = optField(L, 3, "uv_length", def.uvLength);
        def.lift = optField(L, 3, "lift", def.lift);
        lua_getfield(L, 3, "widths");
        if (lua_istable(L, -1)) {
            const int nw = lua_objlen(L, -1);
            for (int i = 1; i <= nw; ++i) {
                lua_rawgeti(L, -1, i);
                def.widths.push_back(static_cast<float>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    if (def.widths.empty()) def.widths.push_back(width);
    std::string error;
    if (!g_engine->addRibbon(def, true, error)) luaL_error(L, "world.ribbon: %s", error.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

int world_ribbon_query(lua_State* L) {
    const Ribbon& r = checkRibbon(L, 1);
    const float* p = luaL_checkvector(L, 2);
    pushQuery(L, r.query(p[0], p[2]));
    return 1;
}

int world_ribbon_point(lua_State* L) {
    const Ribbon& r = checkRibbon(L, 1);
    pushQuery(L, r.point(static_cast<float>(luaL_checknumber(L, 2))));
    return 1;
}

int world_ribbon_length(lua_State* L) {
    lua_pushnumber(L, checkRibbon(L, 1).length());
    return 1;
}

} // namespace

void registerRibbonApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    static const luaL_Reg fns[] = {{"ribbon", world_ribbon},
                                   {"ribbon_query", world_ribbon_query},
                                   {"ribbon_point", world_ribbon_point},
                                   {"ribbon_length", world_ribbon_length},
                                   {nullptr, nullptr}};
    // `world` ja' existe e e' somente leitura (ScriptApi.cpp): abre, poe as
    // funcoes da faixa e fecha de novo.
    lua_getglobal(L, "world");
    lua_setreadonly(L, -1, false);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
}

std::string ribbonApiDefinitions() {
    // Linhas de dentro do bloco `declare world` (ScriptApi.cpp as poe la').
    const std::string info = "{ t: number, s: number, lateral: number, edge: number, width: number, height: number, "
                             "normal: vector, tangent: vector, position: vector, segment: number }";
    return R"(    -- Ribbon (road, trail, shallow river) along a centripetal Catmull-Rom
    -- through `points` (vectors; y is ignored, the ground gives it). opts:
    -- closed, width (or widths, one per point), step (meters between
    -- samples), uv_length, lift (gap above the ground). Same name replaces it.
    -- Ribbons in the scene file (ribbons = {...}) exist from the map load.
    ribbon: (name: string, points: { vector }, opts: { [string]: any }?) -> boolean,
    -- Where `position` is relative to the ribbon (pure: same input, same
    -- result): t (0..1 along), s (meters), lateral (+ = right of the way),
    -- edge (distance to the nearest edge, < 0 = outside), normal (from that
    -- edge inward), tangent, width, height (ribbon surface), position (the
    -- nearest point of the center line).
    ribbon_query: (name: string, position: vector) -> )" + info + R"(,
    -- The center line at t (closed ribbons wrap, open ones clamp).
    ribbon_point: (name: string, t: number) -> )" + info + R"(,
    ribbon_length: (name: string) -> number,
)";
}

} // namespace eruption
