// Tabela `vfx` do Luau: efeitos de particula (VfxSystem). Separada de `fx`,
// que e' a apresentacao de impacto (tremor, clarao, numero que sobe).
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"
#include "renderer/vfx/VfxSystem.hpp"
#include "script/ScriptHost.hpp"

#include <lua.h>
#include <lualib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

namespace eruption {

namespace {

Engine* g_engine = nullptr;
// Instancias tocadas por script: param quando os scripts recarregam (o
// script novo nao conhece os ids do antigo).
std::vector<uint32_t> g_owned;

Vec3 vec3Arg(lua_State* L, int idx) {
    const float* v = luaL_checkvector(L, idx);
    return Vec3(v[0], v[1], v[2]);
}

Mat4 placement(const Vec3& pos, float scale, float yawDeg) {
    Mat4 t = glm::translate(Mat4(1.0f), pos);
    t = glm::rotate(t, glm::radians(yawDeg), Vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(t, Vec3(scale));
}

int vfx_play(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    const char* name = luaL_checkstring(L, 1);
    const Vec3 pos = vec3Arg(L, 2);
    // play(nome, pos, escala?, giro?) ou play(nome, pos, {scale, yaw, color,
    // tint, offsets = {emissor = vector}}): color recolore (mantem o brilho),
    // tint multiplica - variantes de cor sem um arquivo por cor.
    float scale = 1.0f, yaw = 0.0f;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "scale"); if (lua_isnumber(L, -1)) scale = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 3, "yaw"); if (lua_isnumber(L, -1)) yaw = float(lua_tonumber(L, -1)); lua_pop(L, 1);
    } else {
        scale = static_cast<float>(luaL_optnumber(L, 3, 1.0));
        yaw = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    }
    const uint32_t id = g_engine->vfx().spawn(name, placement(pos, scale, yaw));
    if (id && lua_istable(L, 3)) {
        for (const char* k : {"color", "tint"}) {
            lua_getfield(L, 3, k);
            if (lua_isvector(L, -1)) g_engine->vfx().setColor(id, vec3Arg(L, -1), k[0] == 'c');
            lua_pop(L, 1);
        }
        lua_getfield(L, 3, "offsets");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_isvector(L, -1))
                    g_engine->vfx().setEmitterOffset(id, lua_tostring(L, -2), vec3Arg(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    if (id) g_owned.push_back(id);
    lua_pushnumber(L, id);
    return 1;
}

int vfx_stop(lua_State* L) {
    const uint32_t id = static_cast<uint32_t>(luaL_checknumber(L, 1));
    g_engine->vfx().stop(id, lua_toboolean(L, 2) != 0);
    return 0;
}

int vfx_move(lua_State* L) {
    const uint32_t id = static_cast<uint32_t>(luaL_checknumber(L, 1));
    const Vec3 pos = vec3Arg(L, 2);
    const float scale = static_cast<float>(luaL_optnumber(L, 3, 1.0));
    const float yaw = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    g_engine->vfx().setTransform(id, placement(pos, scale, yaw));
    return 0;
}

int vfx_playing(lua_State* L) {
    lua_pushboolean(L, g_engine->vfx().isPlaying(static_cast<uint32_t>(luaL_checknumber(L, 1))));
    return 1;
}

} // namespace

void registerVfxApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    for (uint32_t id : g_owned) engine.vfx().stop(id, true);
    g_owned.clear();
    static const luaL_Reg fns[] = {{"play", vfx_play},
                                   {"stop", vfx_stop},
                                   {"move", vfx_move},
                                   {"playing", vfx_playing},
                                   {nullptr, nullptr}};
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setglobal(L, "vfx");
}

const char* vfxApiDefinitions() {
    return R"(
-- Particle effects (Window > Effects). `name` is an effect of the game
-- (assets/effects/<name>.vfx.json) or an engine template (data/vfx: fogo,
-- faiscas, fumaca, chamine, magia, detritos, rastro, lanterna, janela, and
-- for lava scenery magma_jorro, magma_brasa, magma_fenda, magma_vapor and
-- magma_rio, sized in meters: pass `scale` for a bigger world); a path
-- ending in .json also works.
declare vfx: {
    -- Starts the effect at a world position; returns its id (0 = not found).
    -- `scale` multiplies size, speed and forces; `yaw` turns it in degrees.
    -- As a table: {scale, yaw, color = vector (recolors, keeping each key's
    -- brightness), tint = vector (multiplies), offsets = {emitter = vector}
    -- (meters, effect space)} - color variants without one file per color.
    play: (name: string, position: vector, options: (number | { [string]: any })?, yaw: number?) -> number,
    -- Stops spawning (the live particles finish); `immediate` clears them now.
    stop: (id: number, immediate: boolean?) -> (),
    -- Moves a playing effect (a trail follows the object this way).
    move: (id: number, position: vector, scale: number?, yaw: number?) -> (),
    -- False once the effect ended or was stopped.
    playing: (id: number) -> boolean,
}
)";
}

} // namespace eruption
