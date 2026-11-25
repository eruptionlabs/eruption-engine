// vfx.bolt / vfx.bolt_stop: raio 3D entre dois pontos (renderer/vfx/VfxBolt.hpp).
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
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "campo '%s' deve ser um numero", name);
        out = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
}

void readVec(lua_State* L, int idx, const char* name, Vec3& out, bool required) {
    lua_getfield(L, idx, name);
    if (lua_isvector(L, -1)) {
        const float* v = lua_tovector(L, -1);
        out = Vec3(v[0], v[1], v[2]);
    } else if (required || !lua_isnil(L, -1)) {
        luaL_error(L, "campo '%s' deve ser um vector (x, y, z)", name);
    }
    lua_pop(L, 1);
}

int vfx_bolt(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    BoltSpec b;
    readVec(L, 1, "from", b.from, true);
    readVec(L, 1, "to", b.to, true);
    readNum(L, 1, "width", b.width);
    float branches = static_cast<float>(b.branches);
    readNum(L, 1, "branches", branches);
    b.branches = static_cast<int>(branches);
    readNum(L, 1, "jitter", b.jitter);
    readNum(L, 1, "flicker_ms", b.flickerMs);
    readNum(L, 1, "life", b.life);
    readVec(L, 1, "color", b.color, false);
    readVec(L, 1, "core_color", b.coreColor, false);
    readNum(L, 1, "intensity", b.intensity);
    readNum(L, 1, "light_radius", b.lightRadius);
    readNum(L, 1, "light_intensity", b.lightIntensity);
    float cloud = 0.0f;
    readNum(L, 1, "cloud", cloud);
    b.cloudId = cloud >= 1.0f ? static_cast<uint32_t>(cloud) : 0u;
    const uint32_t id = g_engine->startBolt(b);
    if (id == 0) lua_pushnil(L);
    else lua_pushnumber(L, static_cast<double>(id));
    return 1;
}

int vfx_bolt_stop(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    lua_pushboolean(L, v >= 1.0 && g_engine->stopBolt(static_cast<uint32_t>(v)));
    return 1;
}

} // namespace

void registerBoltApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    engine.bolts().clear(); // o script novo nao conhece os ids do antigo
    static const luaL_Reg fns[] = {{"bolt", vfx_bolt}, {"bolt_stop", vfx_bolt_stop}, {nullptr, nullptr}};
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

std::string boltApiDefinitions() {
    return R"(    -- Raio 3D entre dois pontos do mundo; devolve um id (nil se ha' raios demais
    -- ao mesmo tempo: o teto e' 16). O trajeto e' gerado em 3D (deslocamento do
    -- ponto medio) com galhos que afinam e somem, a largura afina ate' a ponta e
    -- tudo e' sorteado de novo a cada flicker_ms (o raio treme). Desenho aditivo
    -- em fita virada para a camera por segmento, com nucleo quente e halo; o
    -- cenario tapa o que esta' atras. Campos (todos opcionais menos from e to):
    --   from, to          vector: o ponto de saida (cloud_point da nuvem) e o impacto
    --   width             largura na raiz em metros (1.2); o nucleo e' fino (~12% dela, nunca
    --                     abaixo de ~1 pixel na tela) e o halo ~6x o nucleo
    --   branches          numero de galhos (3, ate' 12)
    --   jitter            0..1, quanto o trajeto desvia (0.35); o numero de segmentos cresce
    --                     com o comprimento (~1 a cada 2,5 m: 230 m = 128), entao o zigue-zague
    --                     aparece em qualquer tamanho. Os galhos tem sub-galhos
    --   flicker_ms        intervalo do novo sorteio em ms (70)
    --   life              segundos de vida; <= 0 fica ate' bolt_stop (0.45)
    --   color, core_color vector rgb do halo e do nucleo (azulado, quase branco)
    --   intensity         brilho (1)
    --   light_radius, light_intensity   luz pontual curta na saida e no impacto
    --                     (60 m, 8); light_radius = 0 desliga
    --   cloud             id de weather.cloud_spawn: o trecho dentro da nuvem fica
    --                     difuso e cada sorteio chama weather.cloud_flash nela
    bolt: (spec: { from: vector, to: vector, width: number?, branches: number?, jitter: number?,
        flicker_ms: number?, life: number?, color: vector?, core_color: vector?, intensity: number?,
        light_radius: number?, light_intensity: number?, cloud: number? }) -> number?,
    -- Apaga o raio na hora. false se o id ja' acabou.
    bolt_stop: (id: number) -> boolean,
)";
}

} // namespace eruption
