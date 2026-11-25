// Tabela `weather` do Luau: nuvens volumetricas locais criadas por script (raio,
// tempestade, neve forte no ceu em cima de um alvo). O sistema e' o das nuvens
// locais do clima (LocalCloud), mas a nuvem de script tem vida propria e nao e'
// apagada pela troca de clima; ver Engine::spawnScriptCloud (ScriptCloud.cpp).
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"

#include <lua.h>
#include <lualib.h>

#include <string>

namespace eruption {

namespace {

Engine* g_engine = nullptr;

using Spec = Engine::ScriptCloudSpec;

// Le o campo numerico `name` da tabela em `idx`; se existe, grava e marca o bit.
void readNumber(lua_State* L, int idx, const char* name, float& out, uint32_t bit, uint32_t& mask) {
    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "campo '%s' deve ser um numero", name);
        out = static_cast<float>(lua_tonumber(L, -1));
        mask |= bit;
    }
    lua_pop(L, 1);
}

// Todos os campos de uma especificacao (cloud_spawn e cloud_set).
uint32_t readSpec(lua_State* L, int idx, Spec& s, bool allowPreset) {
    uint32_t mask = 0;
    luaL_checktype(L, idx, LUA_TTABLE);
    if (allowPreset) {
        // O preset entra primeiro; os campos explicitos da tabela o sobrescrevem.
        lua_getfield(L, idx, "preset");
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "campo 'preset' deve ser texto (\"storm\" ou \"snow\")");
            const char* name = lua_tostring(L, -1);
            if (!Engine::scriptCloudPreset(name, s)) luaL_error(L, "preset '%s' desconhecido (use \"storm\" ou \"snow\")", name);
        }
        lua_pop(L, 1);
    }
    readNumber(L, idx, "x", s.x, Spec::FX, mask);
    readNumber(L, idx, "z", s.z, Spec::FZ, mask);
    readNumber(L, idx, "radius_x", s.radiusX, Spec::FRadiusX, mask);
    readNumber(L, idx, "radius_z", s.radiusZ, Spec::FRadiusZ, mask);
    readNumber(L, idx, "density", s.density, Spec::FDensity, mask);
    readNumber(L, idx, "coverage", s.coverage, Spec::FCoverage, mask);
    readNumber(L, idx, "altitude", s.altitude, Spec::FAltitude, mask);
    readNumber(L, idx, "falloff", s.falloff, Spec::FFalloff, mask);
    readNumber(L, idx, "darkness", s.darkness, Spec::FDarkness, mask);
    readNumber(L, idx, "fade_in", s.fadeIn, Spec::FFadeIn, mask);
    readNumber(L, idx, "life", s.life, Spec::FLife, mask);
    readNumber(L, idx, "fade_out", s.fadeOut, Spec::FFadeOut, mask);
    readNumber(L, idx, "thickness", s.thickness, Spec::FThickness, mask);
    readNumber(L, idx, "frame", s.frame, Spec::FFrame, mask);
    readNumber(L, idx, "area_radius", s.areaRadius, Spec::FArea, mask);
    lua_getfield(L, idx, "tint");
    if (!lua_isnil(L, -1)) {
        if (!lua_isvector(L, -1)) luaL_error(L, "campo 'tint' deve ser um vector (r, g, b)");
        const float* v = lua_tovector(L, -1);
        s.tint = Vec3(v[0], v[1], v[2]);
        mask |= Spec::FTint;
    }
    lua_pop(L, 1);
    lua_getfield(L, idx, "frame_fit");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) luaL_error(L, "campo 'frame_fit' deve ser booleano");
        s.frameFit = lua_toboolean(L, -1) != 0;
        mask |= Spec::FFrameFit;
    }
    lua_pop(L, 1);
    lua_getfield(L, idx, "drift");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) luaL_error(L, "campo 'drift' deve ser booleano");
        s.drift = lua_toboolean(L, -1) != 0;
        mask |= Spec::FDrift;
    }
    lua_pop(L, 1);
    lua_getfield(L, idx, "precip");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) luaL_error(L, "campo 'precip' deve ser booleano");
        s.precip = lua_toboolean(L, -1) != 0;
        mask |= Spec::FPrecip;
    }
    lua_pop(L, 1);
    lua_getfield(L, idx, "climate");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) luaL_error(L, "campo 'climate' deve ser booleano");
        s.climate = lua_toboolean(L, -1) != 0;
        mask |= Spec::FClimate;
    }
    lua_pop(L, 1);
    return mask;
}

uint32_t checkId(lua_State* L, int idx) {
    const double v = luaL_checknumber(L, idx);
    return v >= 1.0 ? static_cast<uint32_t>(v) : 0u;
}

int weather_cloud_spawn(lua_State* L) {
    Spec s;
    if (!lua_isnoneornil(L, 1)) {
        const uint32_t mask = readSpec(L, 1, s, true);
        // Raios derivados da area da habilidade; radius_x/radius_z explicitos ganham.
        if (s.areaRadius > 0.0f) {
            const float rx = s.radiusX, rz = s.radiusZ;
            Engine::deriveRadiiFromArea(s);
            if (mask & Spec::FRadiusX) s.radiusX = rx;
            if (mask & Spec::FRadiusZ) s.radiusZ = rz;
        }
    }
    const uint32_t id = g_engine->spawnScriptCloud(s);
    if (id == 0) lua_pushnil(L);
    else lua_pushnumber(L, static_cast<double>(id));
    return 1;
}

int weather_cloud_move(lua_State* L) {
    const uint32_t id = checkId(L, 1);
    const float x = static_cast<float>(luaL_checknumber(L, 2));
    const float z = static_cast<float>(luaL_checknumber(L, 3));
    lua_pushboolean(L, g_engine->moveScriptCloud(id, x, z));
    return 1;
}

int weather_cloud_set(lua_State* L) {
    const uint32_t id = checkId(L, 1);
    Spec s;
    const uint32_t mask = readSpec(L, 2, s, false);
    lua_pushboolean(L, g_engine->setScriptCloud(id, s, mask));
    return 1;
}

int weather_cloud_remove(lua_State* L) {
    const uint32_t id = checkId(L, 1);
    const float fadeOut = lua_isnoneornil(L, 2) ? -1.0f : static_cast<float>(luaL_checknumber(L, 2));
    lua_pushboolean(L, g_engine->removeScriptCloud(id, fadeOut));
    return 1;
}

// weather.cloud_point(id): ponto no miolo da nuvem (de onde o raio nasce).
int weather_cloud_point(lua_State* L) {
    Vec3 p;
    if (!g_engine->scriptCloudPoint(checkId(L, 1), p)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvector(L, p.x, p.y, p.z);
    return 1;
}

// weather.cloud_flash(id, intensity, duration): relampago por dentro da massa.
int weather_cloud_flash(lua_State* L) {
    const uint32_t id = checkId(L, 1);
    const float intensity = static_cast<float>(luaL_optnumber(L, 2, 2.0));
    const float duration = static_cast<float>(luaL_optnumber(L, 3, 0.25));
    lua_pushboolean(L, g_engine->flashScriptCloud(id, intensity, duration));
    return 1;
}

// weather.cloud_precip(id, {kind, rate, radius}) liga; weather.cloud_precip(id, nil) desliga.
int weather_cloud_precip(lua_State* L) {
    const uint32_t id = checkId(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushboolean(L, g_engine->setScriptCloudPrecip(id, false, true, 1.0f, 0.0f));
        return 1;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    bool snow = true;
    lua_getfield(L, 2, "kind");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "campo 'kind' deve ser \"snow\" ou \"rain\"");
        const std::string k = lua_tostring(L, -1);
        if (k == "rain") snow = false;
        else if (k != "snow") luaL_error(L, "kind '%s' desconhecido (use \"snow\" ou \"rain\")", k.c_str());
    }
    lua_pop(L, 1);
    float rate = 1.0f, radius = 0.0f;
    uint32_t dummy = 0;
    readNumber(L, 2, "rate", rate, 1u, dummy);
    readNumber(L, 2, "radius", radius, 1u, dummy);
    lua_pushboolean(L, g_engine->setScriptCloudPrecip(id, true, snow, rate, radius));
    return 1;
}

} // namespace

void registerWeatherApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    static const luaL_Reg fns[] = {{"cloud_spawn", weather_cloud_spawn},
                                   {"cloud_move", weather_cloud_move},
                                   {"cloud_set", weather_cloud_set},
                                   {"cloud_remove", weather_cloud_remove},
                                   {"cloud_point", weather_cloud_point},
                                   {"cloud_flash", weather_cloud_flash},
                                   {"cloud_precip", weather_cloud_precip},
                                   {nullptr, nullptr}};
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "weather");
}

const char* weatherApiDefinitions() {
    return R"(
-- Nuvens volumetricas locais, para efeitos (tempestade, neve, raio) criarem
-- uma massa de nuvem no ceu em cima de um alvo. Nao dependem do clima do mapa e
-- a troca de clima nao as apaga. Dividem o teto de 100 nuvens com as do clima:
-- ate' 24 nuvens de script vivas ao mesmo tempo (cota garantida; se o teto esta'
-- cheio, a nuvem do clima mais longe cede o lugar). Acima da cota cloud_spawn
-- devolve nil e avisa no log.
declare weather: {
    -- Cria uma nuvem e devolve o id, ou nil se a cota esta' cheia. Todos os
    -- campos sao opcionais:
    --   preset                "storm" (massa escura e densa, darkness 0,95) ou "snow" (clara e
    --                         volumosa); os campos abaixo sobrescrevem o preset
    --   area_radius           raio da area da habilidade em metros; os raios da
    --                         nuvem derivam dele (a borda visivel fica em 1,5x a area;
    --                         o chao escurece num disco de 1,3x a area).
    --                         radius_x/radius_z explicitos ganham
    --   x, z                  centro no mundo (padrao 0, 0)
    --   radius_x, radius_z    semi-eixos em metros (80, 60); a borda visivel fica
    --                         em radius / falloff
    --   thickness             sem efeito (a nuvem tem a forma das do clima)
    --   density               0..1 (0.9)
    --   coverage              0..1, quanto do ruido interno vira nuvem (0.7)
    --   altitude              metros do chao ate' a BASE da nuvem (230, a altura das
    --                         nuvens do clima)
    --   falloff               suavidade da borda, menor = mais larga e mais densa (1.2)
    --   darkness              0..1, base escurecida de tempestade (0)
    --   tint                  vector(r, g, b) que multiplica a cor (1, 1, 1)
    --   frame                 0..1: desloca a massa para o lado da camera para que
    --                         apareca centrada em cima de (x, z) na tela (0 = fica
    --                         em cima do ponto no mundo, acima do alvo na tela)
    --   frame_fit             true: posiciona pela BASE (raio e altitude) para ela
    --                         aparecer no terco superior da tela (false; independe de frame)
    --   fade_in               segundos para condensar (0.7)
    --   life                  segundos formada; <= 0 fica ate' cloud_remove (6)
    --   fade_out              segundos para dispersar no fim da vida (1.5)
    --   drift                 true: o vento leva a nuvem (false)
    --   precip                true (padrao): a nuvem de preset traz a chuva do clima
    --                         ("storm": as mesmas gotas e fitas da chuva) ou os flocos
    --                         ("snow"), so' na area, formando e dispersando com ela.
    --                         false desliga. cloud_precip, se chamado, manda no lugar
    --   climate               true (padrao): com preset, o clima de chuva/neve vale so'
    --                         em volta da nuvem (raio de influencia de 2x area_radius,
    --                         borda bem suave, pelo mundo e nao pela tela): a luz cai, a
    --                         cena fica cinza e fria e a neblina sobe, o mesmo que o
    --                         clima de chuva faz ao mundo todo (parametros do clima
    --                         Stormy / Snowy). Segue a formacao e a dispersao da nuvem.
    --                         false deixa so' o visual. Ate' 4 nuvens com clima ao mesmo
    --                         tempo
    -- E' uma nuvem do mesmo desenho das do clima, so' escura e localizada. Nasce
    -- condensando (aberta e rala, fecha em fade_in) e, ao fim da vida, se dispersa.
    cloud_spawn: (spec: {
        preset: string?, area_radius: number?, x: number?, z: number?, radius_x: number?, radius_z: number?,
        thickness: number?, density: number?, coverage: number?, altitude: number?, falloff: number?,
        darkness: number?, tint: vector?, frame: number?, frame_fit: boolean?, fade_in: number?, life: number?, fade_out: number?,
        drift: boolean?, precip: boolean?, climate: boolean?,
    }?) -> number?,
    -- Leva a nuvem para (x, z); a altitude segue o chao dali. Devolve false se o
    -- id nao existe mais.
    cloud_move: (id: number, x: number, z: number) -> boolean,
    -- Muda ao vivo qualquer campo de cloud_spawn (so' os que vierem na tabela;
    -- "preset" nao). Pode ser chamada todo quadro: density, coverage, radius,
    -- thickness e falloff refazem a bolha (uma atualizacao de 48 bytes no comando
    -- do quadro e a cobertura da sombra); darkness, tint e altitude nao custam
    -- nada. fade_in so' vale enquanto a nuvem ainda se forma. Devolve false se o
    -- id nao existe mais.
    cloud_set: (id: number, fields: {
        area_radius: number?, x: number?, z: number?, radius_x: number?, radius_z: number?, thickness: number?,
        density: number?, coverage: number?, altitude: number?, falloff: number?, darkness: number?,
        tint: vector?, frame: number?, frame_fit: boolean?, fade_in: number?, life: number?, fade_out: number?, drift: boolean?,
        precip: boolean?, climate: boolean?,
    }) -> boolean,
    -- Dispersa a nuvem em `fade_out` segundos (padrao: o fade_out dela) e a
    -- remove: ela se abre, esgarca e afina (cobertura cai, borda erodida,
    -- raio se espalha, densidade vai a zero). No meio do fade_in ela dispersa a
    -- partir do estado em que esta, sem pular nem voltar a crescer. Devolve
    -- false se o id nao existe mais.
    cloud_remove: (id: number, fade_out: number?) -> boolean,
    -- Um ponto aleatorio na BASE da nuvem (no ceu, na altura do clima);
    -- e' de la' que um raio desce. nil se o id nao existe mais.
    cloud_point: (id: number) -> vector?,
    -- Relampago por dentro da massa: a nuvem brilha azulada com `intensity`
    -- (0..8, padrao 2) por `duration` segundos (padrao 0.25), subindo rapido e
    -- caindo com tremulo. vfx.bolt com cloud = id chama isto a cada sorteio.
    cloud_flash: (id: number, intensity: number?, duration: number?) -> boolean,
    -- Precipitacao da base da nuvem ate' o chao, so' na area (radius, padrao a
    -- area da habilidade ou 70% da borda). kind: "snow" (padrao) ou "rain"; rate:
    -- multiplicador da quantidade (0..3, padrao 1). Acompanha cloud_move, some com
    -- a dispersao e com cloud_remove. cloud_precip(id, nil) a desliga.
    cloud_precip: (id: number, opts: { kind: string?, rate: number?, radius: number? }?) -> boolean,
}
)";
}

} // namespace eruption
