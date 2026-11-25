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

void readVec3(lua_State* L, int idx, const char* name, Vec3& out) {
    lua_getfield(L, idx, name);
    if (lua_isvector(L, -1)) {
        const float* v = lua_tovector(L, -1);
        out = Vec3(v[0], v[1], v[2]);
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "vfx.tornado: field '%s' must be a vector (r, g, b)", name);
    }
    lua_pop(L, 1);
}

// Le o campo texto; devolve "" se ausente.
std::string readString(lua_State* L, int idx, const char* name, bool& present) {
    std::string out;
    present = false;
    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "vfx.tornado: field '%s' must be a string", name);
        out = lua_tostring(L, -1);
        present = true;
    }
    lua_pop(L, 1);
    return out;
}

// `style` so' preenche os padroes; qualquer campo explicito vence.
void applyStyle(lua_State* L, const std::string& style, TornadoSpec& s) {
    if (style.empty() || style == "storm") return;
    if (style == "whirl") { // redemoinho de poeira e folhas, de efeito de nivel
        s.height = 22.0f; s.radiusTop = 3.2f; s.radiusBottom = 0.9f; s.spin = 2.2f;
        s.cloud = 0.0f; s.grow = 0.5f; s.fade = 0.8f; s.life = 3.0f;
        s.color = Vec3(0.85f, 0.72f, 0.55f); s.dust = Vec3(0.62f, 0.50f, 0.34f);
        s.density = 2.0f; s.opacity = 0.9f; s.wind = 0.2f;
        s.debrisKind = TornadoDebrisKind::Leaves; s.debrisSize = 4.5f;
        s.trailKind.clear();
    } else if (style == "blizzard") { // vortice de neve: branco-azulado, frio, translucido
        s.color = Vec3(0.95f, 1.10f, 1.35f); s.cloudColor = Vec3(0.78f, 0.86f, 1.0f);
        s.dust = Vec3(0.80f, 0.90f, 1.0f);
        s.density = 0.7f; s.opacity = 0.85f; s.cloud = 0.8f;
        s.debrisKind = TornadoDebrisKind::Snow; s.debrisSize = 0.6f;
        s.snowfall = 1.0f;
        s.trailKind = "snow"; s.trailStrength = 0.6f;
    } else {
        luaL_error(L, "vfx.tornado: style must be \"storm\", \"whirl\" or \"blizzard\"");
    }
}

int vfx_tornado(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    TornadoSpec s;
    bool has = false;
    applyStyle(L, readString(L, 1, "style", has), s);
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
    readVec3(L, 1, "dust", s.dust);
    readVec3(L, 1, "color", s.color);
    readVec3(L, 1, "cloud_color", s.cloudColor);
    readNum(L, 1, "cloud", s.cloud);
    readNum(L, 1, "density", s.density);
    readNum(L, 1, "opacity", s.opacity);
    readNum(L, 1, "wind", s.wind);
    readNum(L, 1, "grow", s.grow);
    readNum(L, 1, "debris_size", s.debrisSize);
    readNum(L, 1, "snowfall", s.snowfall);
    readNum(L, 1, "trail_strength", s.trailStrength);
    {
        const std::string kind = readString(L, 1, "debris_kind", has);
        if (has) {
            if (kind == "debris") s.debrisKind = TornadoDebrisKind::Debris;
            else if (kind == "leaves") s.debrisKind = TornadoDebrisKind::Leaves;
            else if (kind == "dust") s.debrisKind = TornadoDebrisKind::Dust;
            else if (kind == "snow") s.debrisKind = TornadoDebrisKind::Snow;
            else if (kind == "none") s.debris = false;
            else luaL_error(L, "vfx.tornado: debris_kind must be \"debris\", \"leaves\", \"dust\", \"snow\" or \"none\"");
            if (kind != "none") s.debris = true;
        }
        const std::string trail = readString(L, 1, "trail_kind", has);
        if (has) s.trailKind = (trail == "none") ? std::string() : trail;
    }
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
    -- O teto de tornados vivos ao mesmo tempo e' a chave tornado_max do preset (1 ou 2 nos
    -- presets de fabrica, no maximo 8): acima dele vfx.tornado devolve nil; confira o retorno
    -- quando criar varios (redemoinhos).
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
    -- Variacoes (o motor so' desenha; o jogo escolhe o visual). `style` preenche os padroes
    -- abaixo e qualquer campo explicito vence:
    --   style         "storm" (o padrao acima), "whirl" (redemoinho pequeno: 22 de altura, sem nuvem,
    --                 folhas, vida de 3 s, vento fraco, sem rastro) ou "blizzard" (vortice de neve:
    --                 branco-azulado, translucido, flocos, rastro de neve)
    --   cloud         escala da nuvem-mae; 0 = sem nuvem (o funil afina sozinho no topo) (1)
    --   color         vector rgb do funil (condensacao) (0.34, 0.38, 0.47)
    --   cloud_color   vector rgb da nuvem-mae (0.30, 0.31, 0.36)
    --   density       multiplica a densidade/espessura, 0.05..4 (1)
    --   opacity       multiplica o alfa final, 0..1 (1)
    --   debris_kind   "debris" (pedras, folhas, tabuas), "leaves", "dust" (pontos macios),
    --                 "snow" (flocos e cristais) ou "none" ("debris")
    --   debris_size   multiplica o tamanho dos destrocos (1)
    --   snowfall      0..1: neve fina caindo em volta do tornado (flocos baratos, ate' 100); 0 = nenhuma (0; 1 no blizzard)
    --   wind          multiplica a rajada de world.wind_burst em volta; 0 = nenhuma (1)
    --   grow          segundos para a corda descer ate' o chao (6)
    --   trail_kind    world.ground_state deixado onde o pe passa: "scorch", "frost", "snow"...
    --                 ou "none" ("scorch")
    --   trail_strength intensidade do rastro, 0..1 (0.3)
    tornado: (spec: { pos: vector, style: string?, height: number?, radius_top: number?,
        radius_bottom: number?, spin: number?, dust: vector?, debris: boolean?, life: number?,
        fade: number?, cloud: number?, color: vector?, cloud_color: vector?, density: number?,
        opacity: number?, debris_kind: string?, debris_size: number?, snowfall: number?, wind: number?, grow: number?,
        trail_kind: string?, trail_strength: number? }) -> number?,
    -- Move o pe do tornado (o funil e os destrocos seguem). false se o id ja' acabou.
    tornado_move: (id: number, position: vector) -> boolean,
    -- Comeca a saida (a corda sobe e a poeira baixa em `fade` s). false se o id ja' acabou.
    tornado_stop: (id: number) -> boolean,
)";
}

} // namespace eruption
