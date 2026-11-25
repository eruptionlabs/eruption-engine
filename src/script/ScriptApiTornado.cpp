// vfx.tornado / vfx.tornado_move / vfx.tornado_stop (renderer/vfx/TornadoField.hpp).
// As funcoes entram na tabela global `vfx` ja' criada por registerVfxApi.
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"

#include <lua.h>
#include <lualib.h>

#include <string>

namespace eruption {

namespace {

Engine* g_engine = nullptr;

void readNum(lua_State* L, int idx, const char* name, float& out) {
    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "vfx.tornado: field '%s' must be a number", name);
        out = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
}

void readBool(lua_State* L, int idx, const char* name, bool& out) {
    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) luaL_error(L, "vfx.tornado: field '%s' must be a boolean", name);
        out = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
}

int vfx_tornado(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    TornadoSpec s;
    lua_getfield(L, 1, "pos");
    if (!lua_isvector(L, -1)) luaL_error(L, "vfx.tornado: field 'pos' must be a vector (x, y, z)");
    {
        const float* v = lua_tovector(L, -1);
        s.pos = Vec3(v[0], v[1], v[2]);
    }
    lua_pop(L, 1);
    readNum(L, 1, "height", s.height);
    readNum(L, 1, "radius_top", s.radiusTop);
    readNum(L, 1, "radius_bottom", s.radiusBottom);
    readNum(L, 1, "spin", s.spin);
    readNum(L, 1, "life", s.life);
    readNum(L, 1, "fade", s.fade);
    readBool(L, 1, "debris", s.debris);
    lua_getfield(L, 1, "dust");
    if (lua_isvector(L, -1)) {
        const float* v = lua_tovector(L, -1);
        s.dust = Vec3(v[0], v[1], v[2]);
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "vfx.tornado: field 'dust' must be a vector (r, g, b)");
    }
    lua_pop(L, 1);
    const uint32_t id = g_engine->startTornado(s);
    if (id == 0) lua_pushnil(L);
    else lua_pushnumber(L, static_cast<double>(id));
    return 1;
}

int vfx_tornado_move(lua_State* L) {
    const double id = luaL_checknumber(L, 1);
    const float* v = luaL_checkvector(L, 2);
    lua_pushboolean(L, id >= 1.0 && g_engine->moveTornado(static_cast<uint32_t>(id), Vec3(v[0], v[1], v[2])));
    return 1;
}

int vfx_tornado_stop(lua_State* L) {
    const double id = luaL_checknumber(L, 1);
    lua_pushboolean(L, id >= 1.0 && g_engine->stopTornado(static_cast<uint32_t>(id)));
    return 1;
}

} // namespace

void registerTornadoApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    engine.tornados().clear(); // o script novo nao conhece os ids do antigo
    static const luaL_Reg fns[] = {{"tornado", vfx_tornado},
                                   {"tornado_move", vfx_tornado_move},
                                   {"tornado_stop", vfx_tornado_stop},
                                   {nullptr, nullptr}};
    lua_getglobal(L, "vfx");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_setreadonly(L, -1, false);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
}

std::string tornadoApiDefinitions() {
    return R"(    -- TORNADO: funil volumetrico (parede turbulenta torcendo, corda que balanca,
    -- mais densa na parede que no miolo, escura por dentro e clara na borda, com auto-sombra
    -- do sol), saia de poeira rolando na base, nuvem-mae escura girando em cima e destrocos
    -- (pedras, folhas, tabuas) em espiral. Um so' ray-march analitico, sem simulacao nem
    -- textura. A corda desce da nuvem nos primeiros ~6 s e sobe de volta ao sair. Perto, o
    -- vento (world.wind_burst) se curva em volta e o chao por onde passa escurece. Devolve um id
    -- (nil se o preset nao permite ou o teto de tornados simultaneos foi atingido: chaves
    -- tornado, tornado_max, tornado_steps, tornado_sun_taps, tornado_scale, tornado_debris).
    -- Campos (so' pos e' obrigatorio):
    --   pos           vector: o pe, em coordenadas de mundo (y = chao)
    --   height        do chao ate' a base da nuvem-mae, em unidades do mundo (200)
    --   radius_top    raio da parede sob a nuvem (36)
    --   radius_bottom raio da corda no chao (6)
    --   spin          1 = giro tipico; multiplica a velocidade angular (1)
    --   dust          vector rgb da poeira do chao (0.46, 0.38, 0.29)
    --   debris        pedras, folhas e tabuas em espiral (true)
    --   life          segundos ate' comecar a sair; <= 0 fica ate' tornado_stop (0)
    --   fade          segundos da saida (3)
    tornado: (spec: { pos: vector, height: number?, radius_top: number?, radius_bottom: number?,
        spin: number?, dust: vector?, debris: boolean?, life: number?, fade: number? }) -> number?,
    -- Move o pe do tornado (o funil e os destrocos seguem). false se o id ja' acabou.
    tornado_move: (id: number, position: vector) -> boolean,
    -- Comeca a saida (a corda sobe e a poeira baixa em `fade` s). false se o id ja' acabou.
    tornado_stop: (id: number) -> boolean,
)";
}

} // namespace eruption
