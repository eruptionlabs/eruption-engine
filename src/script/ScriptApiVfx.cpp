// Tabela `vfx` do Luau: efeitos de particula (VfxSystem). Separada de `fx`,
// que e' a apresentacao de impacto (tremor, clarao, numero que sobe).
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"
#include "renderer/vfx/VfxSystem.hpp"
#include "script/ScriptHost.hpp"

#include <lua.h>
#include <lualib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <string>
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
        if (!lua_istable(L, 4)) yaw = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    }
    const uint32_t id = g_engine->vfx().spawn(name, placement(pos, scale, yaw));
    // play(nome, pos, escala, {vision = false}): as opcoes tambem depois da escala.
    if (id && !lua_istable(L, 3) && lua_istable(L, 4)) {
        lua_getfield(L, 4, "vision");
        if (lua_isboolean(L, -1)) g_engine->vfx().setVision(id, lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
    }
    if (id && lua_istable(L, 3)) {
        lua_getfield(L, 3, "vision");
        if (lua_isboolean(L, -1)) g_engine->vfx().setVision(id, lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
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

// vfx.area(id, {shape, position, ...}) ou vfx.area(id, nil): indicador de
// area no chao (decal do characters.ring), preso ao id ate' sair.
float areaField(lua_State* L, const char* k, float def) {
    lua_getfield(L, 2, k);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}

int vfx_area(lua_State* L) {
    const std::string id = luaL_checkstring(L, 1); // numero vira texto
    if (lua_isnoneornil(L, 2)) {
        g_engine->setGroundArea(id, nullptr);
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "shape");
    const std::string shape = lua_isstring(L, -1) ? lua_tostring(L, -1) : "circle";
    lua_pop(L, 1);
    lua_getfield(L, 2, "position");
    if (!lua_isvector(L, -1)) luaL_error(L, "vfx.area: position (vector) is required");
    const float* pv = lua_tovector(L, -1);
    const Vec3 pos(pv[0], pv[1], pv[2]);
    lua_pop(L, 1);
    // Direcao: vetor (y ignorado) ou yaw em graus, como no vfx.play.
    const float yaw = glm::radians(areaField(L, "yaw", 0.0f));
    Vec2 dir(std::sin(yaw), std::cos(yaw));
    lua_getfield(L, 2, "direction");
    if (lua_isvector(L, -1)) {
        const float* v = lua_tovector(L, -1);
        if (v[0] * v[0] + v[2] * v[2] > 1e-8f) dir = glm::normalize(Vec2(v[0], v[2]));
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "color");
    Vec3 cor(1.0f);
    if (lua_isvector(L, -1)) {
        const float* v = lua_tovector(L, -1);
        cor = Vec3(v[0], v[1], v[2]);
    }
    lua_pop(L, 1);

    GroundRing a;
    a.dir = Vec4(dir, 4.0f, 0.0f);
    float extent = 0.0f;
    if (shape == "line") {
        extent = areaField(L, "length", 0.0f);
        a.shape.w = 1.0f;
        a.dir.w = 0.5f * std::max(areaField(L, "width", extent * 0.2f), 0.1f);
    } else {
        extent = areaField(L, "radius", 0.0f);
        const bool setor = shape == "cone" || shape == "arc";
        if (shape == "ring" || shape == "arc")
            a.dir.w = std::clamp(areaField(L, "inner", extent * 0.6f), 0.0f, extent * 0.98f);
        else if (shape != "circle" && shape != "cone")
            luaL_error(L, "vfx.area: unknown shape '%s' (circle, ring, line, cone, arc)", shape.c_str());
        if (setor) a.dir.z = 0.5f * glm::radians(std::clamp(areaField(L, "angle", 90.0f), 1.0f, 360.0f));
    }
    if (extent <= 0.0f) luaL_error(L, "vfx.area: %s must be > 0", shape == "line" ? "length" : "radius");
    a.center = Vec4(pos, extent);
    a.color = Vec4(cor, std::clamp(areaField(L, "alpha", 0.8f), 0.0f, 1.0f));
    // Folga de um raio para cima e para baixo (rampa de ate' 45 graus), como o anel.
    const float folga = g_engine->groundDecalSlack(extent);
    a.shape = Vec4(std::max(areaField(L, "border", std::max(1.0f, extent * 0.04f)), 0.0f), folga, folga, a.shape.w);
    a.fill = Vec4(std::clamp(areaField(L, "fill", 1.0f), 0.0f, 1.0f),
                  std::clamp(areaField(L, "fill_alpha", 0.25f), 0.0f, 1.0f), 0.0f, 0.0f);
    g_engine->groundDecalPlane(a);
    g_engine->setGroundArea(id, &a);
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
    engine.clearGroundAreas(); // o script novo nao conhece os ids do antigo
    static const luaL_Reg fns[] = {{"play", vfx_play},
                                   {"stop", vfx_stop},
                                   {"move", vfx_move},
                                   {"playing", vfx_playing},
                                   {"area", vfx_area},
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
    -- vision = false (also "vision": false in the .vfx.json): scenery that
    -- team vision only dims, never hides. The table may also come after a
    -- number scale: play(name, pos, 2, {vision = false}).
    play: (name: string, position: vector, options: (number | { [string]: any })?, yaw: (number | { [string]: any })?) -> number,
    -- Stops spawning (the live particles finish); `immediate` clears them now.
    stop: (id: number, immediate: boolean?) -> (),
    -- Moves a playing effect (a trail follows the object this way).
    move: (id: number, position: vector, scale: number?, yaw: number?) -> (),
    -- False once the effect ended or was stopped.
    playing: (id: number) -> boolean,
    -- A shape on the ground lying on the relief (skill area, aim, range),
    -- kept under `id` until area(id, nil). Calling again with the same id
    -- moves or changes it. shape: "circle" (default), "ring" (inner
    -- radius, default 0.6 of radius), "line" (from position along the
    -- direction: length, width default 0.2 of length), "cone" (angle, the
    -- full opening in degrees, default 90) or "arc" (a cone with inner).
    -- Sizes in world units, like positions. direction: vector (y ignored)
    -- or yaw in degrees. fill (0..1, default 1) grows the inside from the
    -- center (or the start of the line), with a bright front while it runs;
    -- fill_alpha (default 0.25) is the inside, border (default 4% of the
    -- size, at least 1; 0 = none) the outline. color defaults to white,
    -- alpha to 0.8. Not hidden by team vision; cleared on script reload.
    area: (id: string | number, area: { shape: string?, position: vector, radius: number?, inner: number?,
        length: number?, width: number?, angle: number?, direction: vector?, yaw: number?, fill: number?,
        fill_alpha: number?, border: number?, color: vector?, alpha: number? }?) -> (),
}
)";
}

} // namespace eruption
