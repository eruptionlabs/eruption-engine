// eulerAngleYXZ vem das extensoes "gtx" do GLM.
#define GLM_ENABLE_EXPERIMENTAL
#include <cfloat>
#include "script/ScriptApi.hpp"

#include "script/ScriptHost.hpp"
#include "script/NativeModule.hpp"
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "game/PlayerController.hpp"
#include "renderer/UiCanvas.hpp"
#include "renderer/WeatherTypes.hpp"
#include "utils/Profiler.hpp"
#include "utils/SystemMonitor.hpp"

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <lua.h>
#include <lualib.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace eruption {

// Acesso aos membros internos do Engine que a API usa (Engine.hpp declara
// esta struct como amiga).
struct ScriptBridge {
    static Engine* engine;
    static std::vector<PointLight>& lights() { return engine->m_deferredLighting.getPointLights(); }
    // Luzes de um mapa vizinho em cena (mundo contínuo); nulo fora de cena.
    static std::vector<PointLight>* regionLights(const char* map) {
        for (auto& r : engine->m_regions)
            if (r.map == map && r.state == Engine::WorldRegion::State::Shown) return &r.lights;
        return nullptr;
    }
    // Mapa vizinho em cena (líquidos já no lugar do mundo); nulo fora de cena.
    static LoadedMap* regionMap(const char* map) {
        for (auto& r : engine->m_regions)
            if (r.map == map && r.state == Engine::WorldRegion::State::Shown && r.data) return r.data.get();
        return nullptr;
    }
    // A lista do quadro copia as dos vizinhos: refazer depois de mexer nelas.
    static void regionLightsChanged() { engine->rebuildWorldLights(); }
    static PlayerController* player() { return engine->m_playerController; }
    static void setWeather(WeatherType t) { engine->applyWeatherTypeFull(t, 1.0f); }
    // Só nesta execução (não grava data/char_config.json).
    static void setPlayerSprite(const std::string& path) {
        engine->m_charConfig.job = path;
        if (engine->m_playerController) engine->m_playerController->reloadSprite();
    }
    // perf.*: o que o laco principal e o F3 ja' medem, sem medir de novo.
    static int& fpsCap() { return engine->m_fpsCap; }
    static VkExtent2D renderExtent() { return engine->renderExtent(); }
    static float cpuWorkMs() {
        const Engine& e = *engine;
        // Fases do laco do quadro anterior menos as esperas: a fence/aquisicao
        // do beginFrame e o present (com vsync os dois bloqueiam pela GPU ou
        // pela tela, nao pela CPU). O sono do limitador ja' fica de fora.
        const float soma = e.m_cpuAdvanceMs + e.m_cpuAppUpdateMs + e.m_cpuRenderMs + e.m_cpuTailMs;
        return std::max(0.0f, soma - e.m_cpuBeginFrameMs - e.m_vulkan.lastPresentMs());
    }
    // Mesmo total do F3 (sem os agregados, que repetem os passes filhos).
    static float gpuMs() { return std::max(0.0f, Profiler::lastGpuTotalMs()); }
    static double vramMb() { return engine->deviceMemoryMb(); }
};
Engine* ScriptBridge::engine = nullptr;

namespace {

Engine& eng() { return *ScriptBridge::engine; }
const std::vector<ModelInstance>& instances() { return eng().modelRenderer().getInstances(); }

Vec3 checkVec3(lua_State* L, int idx) {
    const float* v = luaL_checkvector(L, idx);
    return Vec3(v[0], v[1], v[2]);
}

void pushVec3(lua_State* L, const Vec3& v) { lua_pushvector(L, v.x, v.y, v.z); }

bool equalsNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

// Índices dos scripts começam em 1, como o resto do Lua.
size_t checkIndex(lua_State* L, int arg, size_t count, const char* what) {
    const int i = luaL_checkinteger(L, arg);
    if (i < 1 || static_cast<size_t>(i) > count)
        luaL_errorL(L, "%s index %d out of range (1..%zu)", what, i, count);
    return static_cast<size_t>(i - 1);
}

// Modelo pelo nome ou pelo índice (base 1).
size_t checkModel(lua_State* L, int arg) {
    if (lua_type(L, arg) == LUA_TSTRING) {
        const char* name = lua_tostring(L, arg);
        const int idx = scriptFindModel(name);
        if (idx < 0) luaL_errorL(L, "no model named \"%s\" in this map", name);
        return static_cast<size_t>(idx);
    }
    return checkIndex(L, arg, instances().size(), "model");
}

// ---------------------------------------------------------------- log, ui

int api_log(lua_State* L) {
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += ' ';
        line.append(s, len);
        lua_pop(L, 1);
    }
    Logger::message(LogLevel::Info, line.c_str());
    return 0;
}

int ui_show_hud(lua_State* L) {
    eng().setHudBars(lua_toboolean(L, 1) != 0);
    return 0;
}
int ui_show_minimap(lua_State* L) {
    eng().setMinimapHud(lua_toboolean(L, 1) != 0);
    return 0;
}
// Texto do quadro abaixo do minimapa (#minimap-info): o nome do lugar no
// lugar do nome interno do mapa, coordenada e relogio. nil volta ao padrao.
int ui_minimap_label(lua_State* L) {
    eng().setMinimapLabel(luaL_optstring(L, 1, ""));
    return 0;
}
int ui_message(lua_State* L) {
    size_t len = 0;
    const char* text = luaL_tolstring(L, 1, &len);
    const float seconds = static_cast<float>(luaL_optnumber(L, 2, 3.0));
    if (ScriptHost* host = ScriptHost::current()) host->showMessage(std::string(text, len), seconds);
    lua_pop(L, 1);
    return 0;
}

// ---------------------------------------------------------------- on (eventos)

int addEvent(lua_State* L, ScriptHost::EventKind kind, int fnArg, double number, const char* text) {
    luaL_checktype(L, fnArg, LUA_TFUNCTION);
    ScriptHost* host = ScriptHost::current();
    ScriptHost::Handler h;
    h.kind = kind;
    h.number = number;
    if (text) h.text = text;
    lua_pushvalue(L, fnArg);
    h.fnRef = lua_ref(L, -1);
    lua_pop(L, 1);
    if (!host || !host->addHandler(h)) {
        lua_unref(L, h.fnRef);
        luaL_errorL(L, "on.* must be called at the top level of a script, not inside a function");
    }
    return 0;
}
int on_start(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Start, 1, 0.0, nullptr); }
int on_update(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Update, 1, 0.0, nullptr); }
int on_every(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Every, 2, luaL_checknumber(L, 1), nullptr); }
int on_key(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Key, 2, 0.0, luaL_checkstring(L, 1)); }
int on_near(lua_State* L) {
    const char* model = luaL_checkstring(L, 1);
    return addEvent(L, ScriptHost::EventKind::Near, 3, luaL_checknumber(L, 2), model);
}
int on_hour(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Hour, 2, luaL_checknumber(L, 1), nullptr); }
int on_enter(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Enter, 2, 0.0, luaL_checkstring(L, 1)); }
int on_leave(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Leave, 2, 0.0, luaL_checkstring(L, 1)); }

// ---------------------------------------------------------------- vars

void pushVars(lua_State* L) {
    ScriptHost* host = ScriptHost::current();
    if (!host || host->varsRef() < 0) luaL_errorL(L, "variables are not available");
    lua_getref(L, host->varsRef());
}
int vars_get(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    pushVars(L);
    lua_getfield(L, -1, name);
    return 1;
}
int vars_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checkany(L, 2);
    pushVars(L);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
    return 0;
}
int vars_add(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const double amount = luaL_checknumber(L, 2);
    pushVars(L);
    lua_getfield(L, -1, name);
    const double cur = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    lua_pushnumber(L, cur + amount);
    lua_setfield(L, -2, name);
    return 0;
}

// ---------------------------------------------------------------- world

double hourNow() { return eng().dayNightCycle().timeOfDay() * 24.0; }

int world_time(lua_State* L) {
    lua_pushnumber(L, hourNow());
    return 1;
}
int world_set_time(lua_State* L) {
    double h = luaL_checknumber(L, 1);
    h = h - 24.0 * static_cast<int>(h / 24.0);
    if (h < 0) h += 24.0;
    eng().dayNightCycle().setTimeOfDay(static_cast<float>(h / 24.0));
    return 0;
}
int world_time_between(lua_State* L) {
    const double a = luaL_checknumber(L, 1), b = luaL_checknumber(L, 2), h = hourNow();
    lua_pushboolean(L, a <= b ? (h >= a && h < b) : (h >= a || h < b));
    return 1;
}
int world_weather(lua_State* L) {
    lua_pushstring(L, weatherTypeName(eng().weatherSystem().currentType()));
    return 1;
}
int world_is_weather(lua_State* L) {
    lua_pushboolean(L, equalsNoCase(weatherTypeName(eng().weatherSystem().currentType()), luaL_checkstring(L, 1)));
    return 1;
}
int world_set_weather(lua_State* L) {
    ScriptBridge::setWeather(weatherTypeFromName(luaL_checkstring(L, 1)));
    return 0;
}
int world_map(lua_State* L) {
    lua_pushstring(L, eng().currentMapName().c_str());
    return 1;
}
int world_load_map(lua_State* L) {
    const char* map = luaL_checkstring(L, 1);
    if (ScriptHost* host = ScriptHost::current()) host->rememberMapState(); // voltar aqui acha tudo como estava
    if (lua_isvector(L, 2)) {
        const Vec3 p = checkVec3(L, 2);
        eng().requestMap(map, &p);
    } else {
        eng().requestMap(map, nullptr);
    }
    return 0;
}
// Troca de mapa SEM espera: a leitura do mapa novo acontece numa thread, e
// a troca entra sozinha quando termina (poucos ms na linha principal). O
// jogo chama isso quando o jogador ENTRA na boca da passagem, e quando ele
// chega do outro lado o mapa ja' trocou. Ver Engine::updateWarp.
int world_warp_map(lua_State* L) {
    eng().setPendingMapWarp(luaL_checkstring(L, 1));
    return 0;
}
int world_warping(lua_State* L) {
    lua_pushboolean(L, eng().warpLoading());
    return 1;
}

int world_ground_height(lua_State* L) {
    lua_pushnumber(L, eng().sampleTerrainHeight(static_cast<float>(luaL_checknumber(L, 1)),
                                                static_cast<float>(luaL_checknumber(L, 2))));
    return 1;
}

// Caminho de arquivo como o conteudo do jogo o cita: relativo a' pasta do
// projeto quando esta' dentro dela.
std::string gamePath(const std::string& path) {
    if (path.empty() || !project::isOpen()) return path;
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, project::root(), ec);
    if (ec || rel.empty() || *rel.begin() == "..") return path;
    return rel.generic_string();
}

// Mistura das camadas pintadas do chao em volta de (x, z): media dos pesos
// num disco de raio `radius` (0 = o ponto da grade mais perto). Tabela
// arquivo da camada -> peso; vazia num mapa sem camadas.
int world_ground_layers(lua_State* L) {
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float z = static_cast<float>(luaL_checknumber(L, 2));
    const float radius = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    const Engine::SplatEdit& sp = eng().splatEdit();
    const Engine::HeightEdit& h = eng().heightEdit();
    lua_newtable(L);
    if (!sp.active || sp.w == 0 || sp.d == 0 || sp.weights.size() < size_t(sp.w) * sp.d * 8 || h.scale <= 0.0f)
        return 1;
    const float gx = (x - h.offX) / h.scale, gz = (z - h.offZ) / h.scale;
    const float gr = std::max(radius / h.scale, 0.0f);
    const int x0 = std::max(0, int(std::floor(gx - gr))), x1 = std::min(int(sp.w) - 1, int(std::ceil(gx + gr)));
    const int z0 = std::max(0, int(std::floor(gz - gr))), z1 = std::min(int(sp.d) - 1, int(std::ceil(gz + gr)));
    float sum[8] = {};
    int n = 0;
    for (int iz = z0; iz <= z1; ++iz)
        for (int ix = x0; ix <= x1; ++ix) {
            const float dx = float(ix) - gx, dz = float(iz) - gz;
            if (gr > 0.0f && dx * dx + dz * dz > gr * gr) continue;
            const float* w = &sp.weights[(size_t(iz) * sp.w + ix) * 8];
            for (int l = 0; l < 8; ++l) sum[l] += w[l];
            ++n;
        }
    if (n == 0) return 1;
    for (int l = 0; l < 8; ++l) {
        const std::string& name = sp.layerFiles[l].empty() ? sp.layerNames[l] : sp.layerFiles[l];
        if (name.empty()) continue;
        lua_pushnumber(L, sum[l] / float(n));
        lua_setfield(L, -2, gamePath(name).c_str());
    }
    return 1;
}

// Bloqueia na grade de caminhada todo ponto do chao mais inclinado que
// `degrees` (so' nesta partida; o arquivo salvo nao muda). Devolve quantos
// pontos ficaram bloqueados. 0 num mapa sem grade que siga o terreno.
int world_block_slopes(lua_State* L) {
    lua_pushinteger(L, eng().blockSteepSlopes(static_cast<float>(luaL_checknumber(L, 1))));
    return 1;
}

// O mapa dos liquidos: o aberto, ou o vizinho em cena com o nome dado no
// argumento `arg` (os liquidos dele ja' estao no lugar do mundo).
LoadedMap* liquidMap(lua_State* L, int arg) {
    if (lua_isnoneornil(L, arg)) return eng().currentMap();
    const char* name = luaL_checkstring(L, arg);
    LoadedMap* map = ScriptBridge::regionMap(name);
    if (!map) luaL_errorL(L, "map '%s' is not in the scene", name);
    return map;
}

// Os liquidos que o mapa declara: { name, kind ("water" | "lava"), level,
// center (vector na cota do liquido), radius (o maior raio da margem) }.
int world_liquids(lua_State* L) {
    const auto* map = liquidMap(L, 1);
    lua_newtable(L);
    if (!map) return 1;
    int i = 0;
    for (const auto& lq : map->liquids) {
        float r = lq.radius;
        for (float s : lq.shoreRadius) r = std::max(r, s);
        lua_newtable(L);
        lua_pushstring(L, lq.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, lq.kind == 1 ? "lava" : "water");
        lua_setfield(L, -2, "kind");
        lua_pushnumber(L, lq.level);
        lua_setfield(L, -2, "level");
        pushVec3(L, Vec3(lq.centerX, lq.level, lq.centerZ));
        lua_setfield(L, -2, "center");
        lua_pushnumber(L, r);
        lua_setfield(L, -2, "radius");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// Nivel de uma lava do mapa (indice 1-based de world.liquids ou o nome):
// a superficie, world.liquid_at e o piso andavel seguem no mesmo quadro.
// Falso para agua, nome/indice que nao existe ou nivel invalido.
int world_set_liquid_level(lua_State* L) {
    LoadedMap* map = liquidMap(L, 3);
    int index = -1;
    if (map && lua_type(L, 1) == LUA_TSTRING) {
        const char* name = lua_tostring(L, 1);
        for (size_t i = 0; i < map->liquids.size() && index < 0; ++i)
            if (map->liquids[i].name == name) index = int(i);
    } else {
        index = int(luaL_checkinteger(L, 1)) - 1;
    }
    lua_pushboolean(L, eng().setLiquidLevel(index, float(luaL_checknumber(L, 2)), map));
    return 1;
}

// Custo de andar nas celulas a ate' `radius` de (x, z), so' nesta partida:
// 1 = normal, math.huge bloqueia. world.walkable, world.path e o andar veem
// no mesmo quadro. Devolve quantas celulas mudaram.
int world_set_walk_cost(lua_State* L) {
    const float x = float(luaL_checknumber(L, 1)), z = float(luaL_checknumber(L, 2));
    const float radius = float(luaL_checknumber(L, 3));
    const double cost = luaL_checknumber(L, 4);
    if (std::isnan(cost)) luaL_argerror(L, 4, "cost is NaN");
    lua_pushinteger(L, eng().setWalkCost(x, z, radius, float(cost)));
    return 1;
}
int world_walk_cost(lua_State* L) {
    lua_pushnumber(L, eng().walkCost(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2))));
    return 1;
}

// ---------------------------------------------------------------- player

int player_position(lua_State* L) {
    PlayerController* p = ScriptBridge::player();
    pushVec3(L, p ? p->pos() : Vec3(0.0f));
    return 1;
}
int player_set_position(lua_State* L) {
    if (PlayerController* p = ScriptBridge::player()) p->setPos(checkVec3(L, 1));
    return 0;
}
int player_set_sprite(lua_State* L) {
    // PNG solto ou PNG com o .spr do importador ao lado (o motor prefere o .spr).
    ScriptBridge::setPlayerSprite(luaL_checkstring(L, 1));
    return 0;
}
int player_inside(lua_State* L) {
    const int a = eng().findArea(luaL_checkstring(L, 1));
    PlayerController* p = ScriptBridge::player();
    lua_pushboolean(L, p && a >= 0 && eng().areas()[static_cast<size_t>(a)].contains(p->pos()));
    return 1;
}
int player_near(lua_State* L) {
    const size_t i = checkModel(L, 1);
    const double dist = luaL_checknumber(L, 2);
    PlayerController* p = ScriptBridge::player();
    if (!p) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const ModelInstance& inst = instances()[i];
    const Vec3 q = glm::clamp(p->pos(), inst.worldAabbMin, inst.worldAabbMax);
    lua_pushboolean(L, glm::length(p->pos() - q) <= dist);
    return 1;
}

// ---------------------------------------------------------------- characters
// Personagens da cena, pelo nome que o editor mostra na Hierarquia.

Engine::SceneCharacter* checkCharacter(lua_State* L, int arg) {
    const char* name = luaL_checkstring(L, arg);
    const int i = eng().findCharacter(name);
    if (i < 0) luaL_error(L, "no character named '%s' in this map", name);
    return &eng().characters()[static_cast<size_t>(i)];
}
// Prepara folhas de sprite (.spr) fora do quadro: o characters.add seguinte
// so' cria e envia. Um arquivo ou uma lista; vale no mapa seguinte.
int characters_preload(lua_State* L) {
    if (lua_istable(L, 1)) {
        const int n = lua_objlen(L, 1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            if (lua_isstring(L, -1)) eng().preloadCharacterSprite(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        eng().preloadCharacterSprite(luaL_checkstring(L, 1));
    }
    return 0;
}
int characters_exists(lua_State* L) {
    lua_pushboolean(L, eng().findCharacter(luaL_checkstring(L, 1)) >= 0);
    return 1;
}
int characters_position(lua_State* L) {
    pushVec3(L, checkCharacter(L, 1)->position);
    return 1;
}
// Com o 3º argumento verdadeiro o motor anima o andar pelo deslocamento
// (Engine::placeCharacter); sem ele, só move (recuo, entrada em cena).
int characters_set_position(lua_State* L) {
    checkCharacter(L, 1);
    eng().placeCharacter(eng().findCharacter(lua_tostring(L, 1)), checkVec3(L, 2), lua_toboolean(L, 3) != 0);
    return 0;
}
int characters_walk_to(lua_State* L) {
    checkCharacter(L, 1);
    eng().walkCharacter(eng().findCharacter(lua_tostring(L, 1)), checkVec3(L, 2),
                        static_cast<float>(luaL_optnumber(L, 3, 0.0))); // 0: passo normal
    return 0;
}
int characters_say(lua_State* L) {
    Engine::SceneCharacter* c = checkCharacter(L, 1);
    c->say = luaL_checkstring(L, 2);
    c->sayTime = std::max(0.1f, static_cast<float>(luaL_optnumber(L, 3, 3.0)));
    return 0;
}
int player_say(lua_State* L) {
    eng().sayPlayer(luaL_checkstring(L, 1), static_cast<float>(luaL_optnumber(L, 2, 3.0)));
    return 0;
}
int characters_play(lua_State* L) {
    checkCharacter(L, 1);
    const int idx = eng().findCharacter(lua_tostring(L, 1));
    lua_pushboolean(L, eng().playCharacterAction(idx, luaL_optstring(L, 2, ""), lua_toboolean(L, 3) != 0));
    return 1;
}
int player_animation(lua_State* L) {
    lua_pushstring(L, eng().playerAction().c_str());
    return 1;
}
int characters_animation(lua_State* L) {
    lua_pushstring(L, checkCharacter(L, 1)->action.c_str());
    return 1;
}
int player_play(lua_State* L) {
    lua_pushboolean(L, eng().playPlayerAction(luaL_optstring(L, 1, ""), lua_toboolean(L, 2) != 0));
    return 1;
}
int characters_follow(lua_State* L) {
    checkCharacter(L, 1);
    eng().followPlayer(eng().findCharacter(lua_tostring(L, 1)), static_cast<float>(luaL_optnumber(L, 2, 30.0)));
    return 0;
}
int characters_patrol(lua_State* L) {
    checkCharacter(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<Vec3> points;
    for (int i = 1, n = lua_objlen(L, 2); i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        if (lua_isvector(L, -1)) points.push_back(checkVec3(L, -1));
        lua_pop(L, 1);
    }
    eng().patrolCharacter(eng().findCharacter(lua_tostring(L, 1)), points, static_cast<float>(luaL_optnumber(L, 3, 1.0)));
    return 0;
}
int characters_stop(lua_State* L) {
    checkCharacter(L, 1);
    eng().stopCharacter(eng().findCharacter(lua_tostring(L, 1)));
    return 0;
}
int characters_walking(lua_State* L) {
    const Engine::SceneCharacter* c = checkCharacter(L, 1);
    lua_pushboolean(L, c->walking || c->scriptWalkLeft > 0.0f);
    return 1;
}
int characters_face(lua_State* L) {
    checkCharacter(L, 1)->facing = static_cast<float>(luaL_checknumber(L, 2));
    return 0;
}
int characters_show(lua_State* L) {
    checkCharacter(L, 1)->visible = true;
    return 0;
}
int characters_hide(lua_State* L) {
    checkCharacter(L, 1)->visible = false;
    return 0;
}
int characters_set_sprite(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    checkCharacter(L, 1);
    const char* png = luaL_checkstring(L, 2);
    if (!eng().setCharacterSprite(eng().findCharacter(lua_tostring(L, 1)), png))
        luaL_error(L, "could not load sprite '%s'", png);
    return 0;
}

// ---------------------------------------------------------------- camera

int camera_target(lua_State* L) {
    pushVec3(L, eng().camera().target());
    return 1;
}
int camera_set_target(lua_State* L) {
    eng().camera().setOrbitTarget(checkVec3(L, 1));
    return 0;
}
int camera_orbit(lua_State* L) {
    const Camera& c = eng().camera();
    lua_pushnumber(L, glm::degrees(c.orbitYaw()));
    lua_pushnumber(L, glm::degrees(c.orbitPitch()));
    lua_pushnumber(L, c.orbitDistance());
    return 3;
}
// Abertura vertical em graus e proporcao da tela: quem precisa ENQUADRAR um
// grupo (duas fileiras de personagens, um diorama) resolve a distancia por
// conta propria, em vez de deixar um numero fixo escrito no script que quebra
// quando a janela muda de proporcao ou o projeto muda a abertura.
int camera_view(lua_State* L) {
    const Camera& c = eng().camera();
    lua_pushnumber(L, c.fov());
    lua_pushnumber(L, c.aspect());
    return 2;
}
int camera_set_orbit(lua_State* L) {
    const float pitchDeg = static_cast<float>(luaL_checknumber(L, 2));
    const float distance = static_cast<float>(luaL_checknumber(L, 3));
    Camera& cam = eng().camera();
    cam.setOrbit(glm::radians(static_cast<float>(luaL_checknumber(L, 1))), glm::radians(pitchDeg), distance);
    // A camera corta inclinacao e distancia aos limites do jogo: sem aviso, o
    // script acha que a pose pedida valeu. Avisa uma vez por corte (de novo
    // so' depois de um pedido que caiba), nao a cada quadro.
    static bool pitchWarned = false, distWarned = false;
    const float gotPitch = glm::degrees(cam.orbitPitch());
    if (std::abs(gotPitch - pitchDeg) > 0.01f) {
        if (!pitchWarned)
            ERUPTION_LOG_WARN("camera.set_orbit: pitch %.2f clamped to %.2f (camera pitch limits)", pitchDeg, gotPitch);
        pitchWarned = true;
    } else {
        pitchWarned = false;
    }
    if (std::abs(cam.orbitDistance() - distance) > 0.01f) {
        if (!distWarned)
            ERUPTION_LOG_WARN("camera.set_orbit: distance %.1f clamped to %.1f (max_orbit_distance)", distance,
                              cam.orbitDistance());
        distWarned = true;
    } else {
        distWarned = false;
    }
    return 0;
}

// ---------------------------------------------------------------- input

// Botões de controle pelo nome "pad_*", na ordem do GLFW (layout Xbox: A
// embaixo, B à direita). Qualquer controle ligado vale: menu de jogo não
// pergunta qual. -1 = não é nome de botão.
int padButtonFromName(const char* name) {
    static const char* const kPad[] = {"pad_a", "pad_b", "pad_x", "pad_y", "pad_lb", "pad_rb",
                                       "pad_back", "pad_start", "pad_guide", "pad_ls", "pad_rs",
                                       "pad_up", "pad_right", "pad_down", "pad_left"};
    for (int b = 0; b < 15; ++b)
        if (equalsNoCase(name, kPad[b])) return b;
    return -1;
}
bool padButton(int button, bool pressed) {
    for (int jid = 0; jid < 4; ++jid) {
        if (!Input::isGamepadPresent(jid)) continue;
        if (pressed ? Input::isGamepadButtonPressed(button, jid) : Input::isGamepadButtonDown(button, jid))
            return true;
    }
    return false;
}

int input_down(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const int b = padButtonFromName(name);
    if (b >= 0) {
        lua_pushboolean(L, padButton(b, false));
        return 1;
    }
    const Key k = scriptKeyFromName(name);
    lua_pushboolean(L, k != Key::Unknown && Input::isKeyDown(k));
    return 1;
}
// Nome do primeiro controle ligado, ou nil: o jogo troca o desenho das
// teclas pelo dos botões.
int input_gamepad(lua_State* L) {
    for (int jid = 0; jid < 4; ++jid) {
        if (!Input::isGamepadPresent(jid)) continue;
        const char* n = Input::getGamepadName(jid);
        lua_pushstring(L, n && n[0] ? n : "gamepad");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}
// Tela de toque (o Android; ERUPTION_TEST_TOUCH=1 finge no PC, para testar a
// interface do celular): o jogo esconde o desenho das teclas.
// Mostra/esconde o controle de toque (ERUPTION_TOUCH_PAD; sem ele, nada muda).
int input_touch_pad(lua_State* L) {
    eng().setTouchPadVisible(lua_toboolean(L, 1) != 0);
    return 0;
}
int input_double_tapped(lua_State* L) {
    lua_pushboolean(L, Input::isDoubleTapped());
    return 1;
}
int input_touch(lua_State* L) {
    // ERUPTION_TEST_TOUCH (teste): finge tela de toque no PC.
    static const bool kFinge = std::getenv("ERUPTION_TEST_TOUCH") != nullptr;
    lua_pushboolean(L, kFinge || Input::touchScreen());
    return 1;
}
// Cursor do mouse em pixels da janela, com ou sem botao (borda da tela).
int input_mouse_position(lua_State* L) {
    const glm::vec2 p = Input::mousePosition();
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    return 2;
}
// Roda do mouse neste quadro (0 parada; positivo para frente).
int input_scroll(lua_State* L) {
    lua_pushnumber(L, Input::mouseScrollDelta());
    return 1;
}
// Dedos no mundo (fora da interface e do controle de toque): {id, x, y}.
int input_touches(lua_State* L) {
    const auto& ts = Input::touches();
    lua_createtable(L, int(ts.size()), 0);
    for (size_t i = 0; i < ts.size(); ++i) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, ts[i].id);
        lua_setfield(L, -2, "id");
        lua_pushnumber(L, ts[i].x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, ts[i].y);
        lua_setfield(L, -2, "y");
        lua_rawseti(L, -2, int(i) + 1);
    }
    return 1;
}
int input_pressed(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const int b = padButtonFromName(name);
    if (b >= 0) {
        lua_pushboolean(L, padButton(b, true));
        return 1;
    }
    const Key k = scriptKeyFromName(name);
    lua_pushboolean(L, k != Key::Unknown && Input::isKeyPressed(k));
    return 1;
}

// ---------------------------------------------------------------- models

// Le e decodifica .glb fora do quadro (texturas embutidas incluidas): o
// modelo posto depois por esse arquivo so' sobe para a GPU. Aceita um
// arquivo ou uma lista; vale para o mapa seguinte (world.preload_map).
int models_preload(lua_State* L) {
    if (lua_istable(L, 1)) {
        const int n = lua_objlen(L, 1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            if (lua_isstring(L, -1)) eng().preloadModel(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        eng().preloadModel(luaL_checkstring(L, 1));
    }
    return 0;
}
int models_count(lua_State* L) {
    lua_pushinteger(L, static_cast<int>(instances().size()));
    return 1;
}
int models_find(lua_State* L) {
    const int idx = scriptFindModel(luaL_checkstring(L, 1));
    if (idx < 0) lua_pushnil(L);
    else lua_pushinteger(L, idx + 1);
    return 1;
}
int models_name(lua_State* L) {
    lua_pushstring(L, instances()[checkModel(L, 1)].name.c_str());
    return 1;
}
int models_position(lua_State* L) {
    pushVec3(L, Vec3(instances()[checkModel(L, 1)].transform[3]));
    return 1;
}
// Modelos ligados cuja posicao fica a ate' `radius` de `position` no plano
// do chao (a altura nao conta), do mais perto ao mais longe.
int models_near(lua_State* L) {
    const Vec3 c = checkVec3(L, 1);
    const float r = static_cast<float>(luaL_checknumber(L, 2));
    const auto& all = instances();
    std::vector<std::pair<float, int>> found;
    for (size_t i = 0; i < all.size(); ++i) {
        if (!all[i].enabled) continue;
        const Vec3 p = Vec3(all[i].transform[3]);
        const float d2 = (p.x - c.x) * (p.x - c.x) + (p.z - c.z) * (p.z - c.z);
        if (d2 <= r * r) found.emplace_back(d2, int(i));
    }
    std::sort(found.begin(), found.end());
    lua_createtable(L, int(found.size()), 0);
    for (size_t k = 0; k < found.size(); ++k) {
        lua_pushinteger(L, found[k].second + 1);
        lua_rawseti(L, -2, int(k + 1));
    }
    return 1;
}
int models_file(lua_State* L) {
    lua_pushstring(L, gamePath(instances()[checkModel(L, 1)].assetPath).c_str());
    return 1;
}
int models_scale(lua_State* L) {
    const Mat4& m = instances()[checkModel(L, 1)].transform;
    pushVec3(L, Vec3(glm::length(Vec3(m[0])), glm::length(Vec3(m[1])), glm::length(Vec3(m[2]))));
    return 1;
}
int models_set_position(lua_State* L) {
    const size_t i = checkModel(L, 1);
    Mat4 m = instances()[i].transform;
    m[3] = Vec4(checkVec3(L, 2), 1.0f);
    eng().modelRenderer().setInstanceTransform(static_cast<uint32_t>(i), m);
    return 0;
}
int models_set_rotation(lua_State* L) {
    const size_t i = checkModel(L, 1);
    const Vec3 deg = checkVec3(L, 2);
    const Mat4& cur = instances()[i].transform;
    const Vec3 scale(glm::length(Vec3(cur[0])), glm::length(Vec3(cur[1])), glm::length(Vec3(cur[2])));
    Mat4 m = glm::eulerAngleYXZ(glm::radians(deg.y), glm::radians(deg.x), glm::radians(deg.z));
    m = glm::scale(m, scale);
    m[3] = cur[3];
    eng().modelRenderer().setInstanceTransform(static_cast<uint32_t>(i), m);
    return 0;
}
int models_set_scale(lua_State* L) {
    const size_t i = checkModel(L, 1);
    const Vec3 s = lua_isnumber(L, 2) ? Vec3(static_cast<float>(lua_tonumber(L, 2))) : checkVec3(L, 2);
    Mat4 m = instances()[i].transform;
    for (int a = 0; a < 3; ++a) {
        const float len = glm::length(Vec3(m[a]));
        if (len > 1e-6f) m[a] = Vec4(Vec3(m[a]) / len * s[a], 0.0f);
    }
    eng().modelRenderer().setInstanceTransform(static_cast<uint32_t>(i), m);
    return 0;
}
int models_move_by(lua_State* L) {
    const size_t i = checkModel(L, 1);
    const Vec3 move = checkVec3(L, 2);
    const float seconds = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    if (ScriptHost* host = ScriptHost::current()) host->addTween(static_cast<uint32_t>(i), move, 0.0f, seconds);
    return 0;
}
int models_rotate_by(lua_State* L) {
    const size_t i = checkModel(L, 1);
    const float degrees = static_cast<float>(luaL_checknumber(L, 2));
    const float seconds = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    if (ScriptHost* host = ScriptHost::current()) host->addTween(static_cast<uint32_t>(i), Vec3(0.0f), degrees, seconds);
    return 0;
}
int models_visible(lua_State* L) {
    lua_pushboolean(L, instances()[checkModel(L, 1)].enabled);
    return 1;
}
int models_set_visible(lua_State* L) {
    const size_t i = checkModel(L, 1);
    eng().modelRenderer().setInstanceEnabled(static_cast<uint32_t>(i), lua_toboolean(L, 2) != 0);
    return 0;
}
int models_show(lua_State* L) {
    eng().modelRenderer().setInstanceEnabled(static_cast<uint32_t>(checkModel(L, 1)), true);
    return 0;
}
int models_hide(lua_State* L) {
    eng().modelRenderer().setInstanceEnabled(static_cast<uint32_t>(checkModel(L, 1)), false);
    return 0;
}
int models_set_solid(lua_State* L) {
    eng().setSolid(static_cast<uint32_t>(checkModel(L, 1)), lua_toboolean(L, 2) != 0);
    return 0;
}
int models_solid(lua_State* L) {
    lua_pushboolean(L, eng().isSolid(static_cast<uint32_t>(checkModel(L, 1))));
    return 1;
}
int models_play(lua_State* L) {
    const auto i = static_cast<uint32_t>(checkModel(L, 1));
    const char* name = luaL_optstring(L, 2, "");
    const bool loop = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    const float speed = static_cast<float>(luaL_optnumber(L, 4, 1.0));
    const float fade = static_cast<float>(luaL_optnumber(L, 5, ModelRenderer::kAnimationFade));
    const bool ok = eng().modelRenderer().playAnimation(i, name, loop, speed, fade);
    if (!ok && *name) Logger::warning("models.play: %s has no animation \"%s\"", instances()[i].name.c_str(), name);
    lua_pushboolean(L, ok);
    return 1;
}
int models_animation(lua_State* L) {
    lua_pushstring(L, eng().modelRenderer().currentAnimation(static_cast<uint32_t>(checkModel(L, 1))).c_str());
    return 1;
}
int models_morphs(lua_State* L) {
    const auto names = eng().modelRenderer().morphTargetsOf(static_cast<uint32_t>(checkModel(L, 1)));
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t k = 0; k < names.size(); ++k) {
        lua_pushstring(L, names[k].c_str());
        lua_rawseti(L, -2, static_cast<int>(k + 1));
    }
    return 1;
}
int models_set_morph(lua_State* L) {
    const auto i = static_cast<uint32_t>(checkModel(L, 1));
    const std::string target = lua_type(L, 2) == LUA_TNUMBER ? std::to_string(luaL_checkinteger(L, 2)) : luaL_checkstring(L, 2);
    // Sem peso (nil): volta ao que a animação manda.
    const float w = lua_isnoneornil(L, 3) ? std::numeric_limits<float>::quiet_NaN()
                                          : static_cast<float>(luaL_checknumber(L, 3));
    const bool ok = eng().modelRenderer().setMorphWeight(i, target, w);
    if (!ok) Logger::warning("models.set_morph: %s has no morph target \"%s\"", instances()[i].name.c_str(), target.c_str());
    lua_pushboolean(L, ok);
    return 1;
}
int models_animations(lua_State* L) {
    const auto names = eng().modelRenderer().animationsOf(static_cast<uint32_t>(checkModel(L, 1)));
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t k = 0; k < names.size(); ++k) {
        lua_pushstring(L, names[k].c_str());
        lua_rawseti(L, -2, static_cast<int>(k + 1));
    }
    return 1;
}

// ---------------------------------------------------------------- game (salvar)

std::string slotArg(lua_State* L, int arg) {
    if (lua_isnoneornil(L, arg)) return "1";
    if (lua_type(L, arg) == LUA_TNUMBER) return std::to_string(static_cast<int>(lua_tonumber(L, arg)));
    return luaL_checkstring(L, arg);
}
// Salvar/carregar e' trabalho do motor (gravar/refazer os objetos do mapa),
// nao do script: fica fora do limite de 250 ms por chamada (NativeWork).
int game_save(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    ScriptHost* host = ScriptHost::current();
    std::string error;
    const bool ok = host && host->saveGame(slotArg(L, 1), error);
    if (!ok) Logger::error("game.save: %s", error.c_str());
    lua_pushboolean(L, ok);
    return 1;
}
int game_load(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    ScriptHost* host = ScriptHost::current();
    std::string error;
    const bool ok = host && host->loadGame(slotArg(L, 1), error);
    if (!ok) Logger::error("game.load: %s", error.c_str());
    lua_pushboolean(L, ok);
    return 1;
}
// Recomeca o jogo: scripts do zero (variaveis limpas, objetos criados por
// script somem) no mapa inicial do projeto. Atendido depois do quadro.
int game_restart(lua_State*) {
    if (ScriptHost* host = ScriptHost::current()) host->requestRestart();
    return 0;
}
int game_save_settings(lua_State* L) {
    std::string error;
    const bool ok = ScriptHost::saveSettings(L, 1, error);
    if (!ok) Logger::error("game.save_settings: %s", error.c_str());
    lua_pushboolean(L, ok);
    return 1;
}
int game_settings(lua_State* L) {
    ScriptHost::pushSettings(L);
    return 1;
}
int game_has_save(lua_State* L) {
    lua_pushboolean(L, ScriptHost::hasSave(slotArg(L, 1)));
    return 1;
}
int game_delete_save(lua_State* L) {
    lua_pushboolean(L, ScriptHost::deleteSave(slotArg(L, 1)));
    return 1;
}

// ---------------------------------------------------------------- audio

// Ponto do som: um vector ou um modelo (o centro dele).
Vec3 soundPlace(lua_State* L, int arg) {
    if (lua_isvector(L, arg)) return checkVec3(L, arg);
    const ModelInstance& inst = instances()[checkModel(L, arg)];
    return (inst.worldAabbMin + inst.worldAabbMax) * 0.5f;
}
// Opcoes de um toque: tabela {volume, pitch, range, min, falloff, group, loop}
// no lugar do volume, ou os numeros soltos de antes (volume, alcance, tom).
AudioSystem::PlayOptions soundOptions(lua_State* L, int arg, bool loop, int rangeArg, int pitchArg) {
    AudioSystem::PlayOptions o;
    o.loop = loop;
    if (lua_istable(L, arg)) {
        auto num = [&](const char* k, float& out) {
            lua_getfield(L, arg, k);
            if (lua_isnumber(L, -1)) out = float(lua_tonumber(L, -1));
            lua_pop(L, 1);
        };
        num("volume", o.volume);
        num("pitch", o.pitch);
        num("range", o.range);
        num("min", o.minDistance);
        lua_getfield(L, arg, "loop");
        if (lua_isboolean(L, -1)) o.loop = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, arg, "group");
        if (lua_isstring(L, -1)) o.group = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, arg, "falloff");
        if (lua_isstring(L, -1)) {
            const std::string f = lua_tostring(L, -1);
            o.falloff = f == "inverse" ? AudioSystem::Falloff::Inverse
                      : f == "exponential" ? AudioSystem::Falloff::Exponential : AudioSystem::Falloff::Linear;
        }
        lua_pop(L, 1);
        return o;
    }
    o.volume = float(luaL_optnumber(L, arg, 1.0));
    if (rangeArg) o.range = float(luaL_optnumber(L, rangeArg, 300.0));
    if (pitchArg) o.pitch = float(luaL_optnumber(L, pitchArg, 1.0));
    return o;
}
int pushSound(lua_State* L, uint32_t id) {
    lua_pushinteger(L, static_cast<int>(id));
    return 1;
}
int audio_play(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    return pushSound(L, eng().audio().play(luaL_checkstring(L, 1), soundOptions(L, 2, false, 0, 3)));
}
int audio_play_at(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    return pushSound(L, eng().audio().playAt(luaL_checkstring(L, 1), soundPlace(L, 2), soundOptions(L, 3, false, 4, 5)));
}
int audio_loop(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    return pushSound(L, eng().audio().play(luaL_checkstring(L, 1), soundOptions(L, 2, true, 0, 3)));
}
int audio_loop_at(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    return pushSound(L, eng().audio().playAt(luaL_checkstring(L, 1), soundPlace(L, 2), soundOptions(L, 3, true, 4, 5)));
}
int audio_stop(lua_State* L) {
    eng().audio().stop(static_cast<uint32_t>(luaL_checkinteger(L, 1)), float(luaL_optnumber(L, 2, 0.0)));
    return 0;
}
int audio_playing(lua_State* L) {
    lua_pushboolean(L, eng().audio().playing(static_cast<uint32_t>(luaL_checkinteger(L, 1))));
    return 1;
}
// Abrir o arquivo (Ogg: cabecalho e tabela do stb_vorbis) e' trabalho do
// motor: num celular de flash lenta passava dos 250 ms por chamada e o
// vigia matava o jogo na troca de musica (NativeWork, como o game.save).
// music(arquivo, volume?, fade?) | music(introducao, laco, volume?, fade?) |
// music(arquivo, {volume, fade, loop = laco, resume}).
int audio_music(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    const char* file = luaL_checkstring(L, 1);
    float volume = 1.0f, fade = 1.0f;
    std::string loopFile;
    bool resume = false;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "volume"); if (lua_isnumber(L, -1)) volume = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 2, "fade"); if (lua_isnumber(L, -1)) fade = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 2, "loop"); if (lua_isstring(L, -1)) loopFile = lua_tostring(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "resume"); resume = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        loopFile = lua_tostring(L, 2);
        volume = float(luaL_optnumber(L, 3, 1.0));
        fade = float(luaL_optnumber(L, 4, 1.0));
    } else {
        volume = float(luaL_optnumber(L, 2, 1.0));
        fade = float(luaL_optnumber(L, 3, 1.0));
    }
    eng().audio().music(file, volume, fade, loopFile, resume);
    return 0;
}
int audio_stop_music(lua_State* L) {
    eng().audio().stopMusic(float(luaL_optnumber(L, 1, 1.0)));
    return 0;
}
// set_volume(volume): volume geral. set_volume(som, volume, fade?): um som que
// ja' toca, sem recomecar.
int audio_set_volume(lua_State* L) {
    if (lua_gettop(L) <= 1) eng().audio().setMasterVolume(float(luaL_checknumber(L, 1)));
    else eng().audio().setVolume(static_cast<uint32_t>(luaL_checkinteger(L, 1)), float(luaL_checknumber(L, 2)),
                                 float(luaL_optnumber(L, 3, 0.0)));
    return 0;
}
int audio_set_pitch(lua_State* L) {
    eng().audio().setPitch(static_cast<uint32_t>(luaL_checkinteger(L, 1)), float(luaL_checknumber(L, 2)));
    return 0;
}
int audio_music_volume(lua_State* L) {
    eng().audio().setMusicVolume(float(luaL_checknumber(L, 1)), float(luaL_optnumber(L, 2, 0.0)));
    return 0;
}
int audio_group_volume(lua_State* L) {
    eng().audio().setGroupVolume(luaL_checkstring(L, 1), float(luaL_checknumber(L, 2)), float(luaL_optnumber(L, 3, 0.0)));
    return 0;
}
int audio_preload(lua_State* L) {
    lua_pushboolean(L, eng().audio().preload(luaL_checkstring(L, 1)));
    return 1;
}

// ---------------------------------------------------------------- lights

// O último argumento, opcional, é o nome de um mapa vizinho em cena (mundo
// contínuo): sem ele, as luzes do mapa aberto; com ele, as daquele vizinho,
// numeradas como na cena dele.
std::vector<PointLight>& lightList(lua_State* L, int mapArg) {
    if (lua_isnoneornil(L, mapArg)) return ScriptBridge::lights();
    const char* map = luaL_checkstring(L, mapArg);
    std::vector<PointLight>* ls = ScriptBridge::regionLights(map);
    if (!ls) luaL_errorL(L, "map '%s' is not in the scene", map);
    return *ls;
}
PointLight& light(lua_State* L, int mapArg) {
    auto& ls = lightList(L, mapArg);
    return ls[checkIndex(L, 1, ls.size(), "light")];
}
// Depois de mudar uma luz: a de um vizinho só vale com a lista refeita.
void lightEdited(lua_State* L, int mapArg) {
    if (!lua_isnoneornil(L, mapArg)) ScriptBridge::regionLightsChanged();
}
int lights_count(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, static_cast<int>(ScriptBridge::lights().size()));
    } else {
        const std::vector<PointLight>* ls = ScriptBridge::regionLights(luaL_checkstring(L, 1));
        lua_pushinteger(L, ls ? static_cast<int>(ls->size()) : 0);
    }
    return 1;
}
int lights_position(lua_State* L) { pushVec3(L, light(L, 2).position); return 1; }
int lights_set_position(lua_State* L) { light(L, 3).position = checkVec3(L, 2); lightEdited(L, 3); return 0; }
int lights_color(lua_State* L) { pushVec3(L, light(L, 2).color); return 1; }
int lights_set_color(lua_State* L) { light(L, 3).color = checkVec3(L, 2); lightEdited(L, 3); return 0; }
int lights_intensity(lua_State* L) { lua_pushnumber(L, light(L, 2).intensity); return 1; }
int lights_set_intensity(lua_State* L) {
    light(L, 3).intensity = static_cast<float>(luaL_checknumber(L, 2));
    lightEdited(L, 3);
    return 0;
}
int lights_enabled(lua_State* L) { lua_pushboolean(L, light(L, 2).enabled); return 1; }
int lights_set_enabled(lua_State* L) { light(L, 3).enabled = lua_toboolean(L, 2) != 0; lightEdited(L, 3); return 0; }
int lights_turn_on(lua_State* L) { light(L, 2).enabled = true; lightEdited(L, 2); return 0; }
int lights_turn_off(lua_State* L) { light(L, 2).enabled = false; lightEdited(L, 2); return 0; }
// A luz projeta sombra de sprite (PointLight::spriteShadow).
int lights_set_sprite_shadow(lua_State* L) {
    light(L, 3).spriteShadow = lua_toboolean(L, 2) != 0;
    lightEdited(L, 3);
    return 0;
}
// Tipo de sombra dos sprites do projeto (Engine::setSpriteShadow).
int characters_set_shadow(lua_State* L) {
    const char* k = luaL_optstring(L, 1, "");
    const int kind = std::strcmp(k, "planar") == 0 ? 0 : std::strcmp(k, "circle") == 0 ? 1
                   : std::strcmp(k, "lights") == 0 ? 2 : std::strcmp(k, "map") == 0 ? 3 : -1;
    eng().setSpriteShadow(kind);
    return 0;
}


// ------------------------------------------------------- suco de cena (fx)
//
// Nada aqui sabe o que e' batalha ou turno: sao primitivas de apresentacao
// (tremor, clarao, numero que sobe, retangulo, texto, barra). O jogo decide
// o que elas significam.

Vec4 optColor(lua_State* L, int arg, const Vec4& fallback) {
    if (lua_isnoneornil(L, arg)) return fallback;
    const float* v = luaL_checkvector(L, arg);
    return Vec4(v[0], v[1], v[2], 1.0f);
}

int characters_flash(lua_State* L) {
    const int idx = eng().findCharacter(luaL_checkstring(L, 1));
    if (idx < 0) luaL_error(L, "no character named '%s' in this map", lua_tostring(L, 1));
    eng().flashCharacter(idx, Vec3(optColor(L, 2, Vec4(1.0f))), static_cast<float>(luaL_optnumber(L, 3, 0.12)));
    return 0;
}
int characters_shake(lua_State* L) {
    const int idx = eng().findCharacter(luaL_checkstring(L, 1));
    if (idx < 0) luaL_error(L, "no character named '%s' in this map", lua_tostring(L, 1));
    eng().shakeCharacter(idx, static_cast<float>(luaL_optnumber(L, 2, 2.0)),
                         static_cast<float>(luaL_optnumber(L, 3, 0.25)));
    return 0;
}
int characters_lunge(lua_State* L) {
    const int idx = eng().findCharacter(luaL_checkstring(L, 1));
    if (idx < 0) luaL_error(L, "no character named '%s' in this map", lua_tostring(L, 1));
    eng().lungeCharacter(idx, checkVec3(L, 2), static_cast<float>(luaL_optnumber(L, 3, 12.0)),
                         static_cast<float>(luaL_optnumber(L, 4, 0.3)));
    return 0;
}
int characters_pop(lua_State* L) {
    const int idx = eng().findCharacter(luaL_checkstring(L, 1));
    if (idx < 0) luaL_error(L, "no character named '%s' in this map", lua_tostring(L, 1));
    eng().popCharacter(idx, static_cast<float>(luaL_optnumber(L, 2, 0.2)),
                       static_cast<float>(luaL_optnumber(L, 3, 0.2)));
    return 0;
}
int characters_set_scale(lua_State* L) {
    const int idx = eng().findCharacter(luaL_checkstring(L, 1));
    if (idx < 0) luaL_error(L, "no character named '%s' in this map", lua_tostring(L, 1));
    eng().scaleCharacter(idx, static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}
int characters_fade(lua_State* L) {
    const int idx = eng().findCharacter(luaL_checkstring(L, 1));
    if (idx < 0) luaL_error(L, "no character named '%s' in this map", lua_tostring(L, 1));
    eng().fadeCharacter(idx, static_cast<float>(luaL_checknumber(L, 2)),
                        static_cast<float>(luaL_optnumber(L, 3, 0.0)));
    return 0;
}

int fx_shake_camera(lua_State* L) {
    eng().camera().shake(static_cast<float>(luaL_optnumber(L, 1, 1.5)), static_cast<float>(luaL_optnumber(L, 2, 0.3)));
    return 0;
}
int fx_number(lua_State* L) {
    eng().uiCanvas().popup(checkVec3(L, 1), luaL_checkstring(L, 2), optColor(L, 3, Vec4(1.0f)),
                           static_cast<float>(luaL_optnumber(L, 4, 0.9)),
                           static_cast<float>(luaL_optnumber(L, 5, 0.08)),
                           static_cast<float>(luaL_optnumber(L, 6, 0.045)));
    return 0;
}
int fx_fade(lua_State* L) {
    eng().uiCanvas().screenFade(optColor(L, 1, Vec4(0.0f)), static_cast<float>(luaL_checknumber(L, 2)),
                                static_cast<float>(luaL_checknumber(L, 3)),
                                static_cast<float>(luaL_optnumber(L, 4, 0.25)));
    return 0;
}
int fx_flash(lua_State* L) {
    // Clarao: entra cheio e sai. Dois fades em sequencia ficariam a cargo do
    // script; aqui e' um so', porque todo golpe usa isso.
    eng().uiCanvas().screenFlash(optColor(L, 1, Vec4(1.0f)), static_cast<float>(luaL_optnumber(L, 3, 0.6)),
                                 static_cast<float>(luaL_optnumber(L, 2, 0.18)));
    return 0;
}
// So' o cenario escurece (a interface fica por cima, intacta); anima da forca
// atual ate' `strength` em `seconds`. dim(nil, 0, t) volta.
int fx_dim(lua_State* L) {
    eng().uiCanvas().sceneDim(optColor(L, 1, Vec4(0.0f, 0.0f, 0.0f, 1.0f)), static_cast<float>(luaL_checknumber(L, 2)),
                              static_cast<float>(luaL_optnumber(L, 3, 0.25)));
    return 0;
}
int fx_fading(lua_State* L) {
    lua_pushboolean(L, eng().uiCanvas().fading());
    return 1;
}

// ui: documento de interface em CSS. O script so' preenche valor; o layout
// e' do arquivo (e do Caldera). Ver renderer/UiDocument.hpp.
int ui_load(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    const std::string rel = luaL_checkstring(L, 1);
    std::string err;
    std::filesystem::path p = project::gameRef(rel);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) p = project::engine(rel);
    if (!eng().uiDocument().load(p, &err)) {
        ERUPTION_LOG_WARN("ui.load: %s", err.c_str());
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}
// Posicao/tamanho de um elemento em % do pai (ou da tela), como no CSS; sem
// largura/altura fica a do arquivo. Sem esquerda (nil) volta ao CSS.
int ui_place(lua_State* L) {
    const char* sel = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) {
        eng().uiDocument().clearPlace(sel);
        return 0;
    }
    eng().uiDocument().setPlace(sel, static_cast<float>(luaL_checknumber(L, 2)),
                                static_cast<float>(luaL_checknumber(L, 3)),
                                static_cast<float>(luaL_optnumber(L, 4, -1.0)),
                                static_cast<float>(luaL_optnumber(L, 5, -1.0)));
    return 0;
}
// Recorte da imagem de um elemento (u0, v0, u1, v1 em 0..1); nil volta ao todo.
int ui_image_rect(lua_State* L) {
    const char* sel = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) {
        eng().uiDocument().clearImageRect(sel);
        return 0;
    }
    eng().uiDocument().setImageRect(sel, static_cast<float>(luaL_checknumber(L, 2)),
                                    static_cast<float>(luaL_checknumber(L, 3)),
                                    static_cast<float>(luaL_checknumber(L, 4)),
                                    static_cast<float>(luaL_checknumber(L, 5)));
    return 0;
}
int ui_text(lua_State* L) {
    eng().uiDocument().setText(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return 0;
}
int ui_value(lua_State* L) {
    eng().uiDocument().setValue(luaL_checkstring(L, 1), static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}
int ui_image(lua_State* L) {
    eng().uiDocument().setImage(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return 0;
}

int ui_visible(lua_State* L) {
    eng().uiDocument().setVisible(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}
int ui_color(lua_State* L) {
    const float* v = luaL_checkvector(L, 2);
    const Vec4 c(v[0], v[1], v[2], static_cast<float>(luaL_optnumber(L, 4, 1.0)));
    eng().uiDocument().setColor(luaL_checkstring(L, 1), static_cast<int>(luaL_optinteger(L, 3, 0)), c);
    return 0;
}
int ui_held(lua_State* L) {
    lua_pushnumber(L, eng().uiHeld(luaL_checkstring(L, 1)));
    return 1;
}
int ui_released(lua_State* L) {
    lua_pushboolean(L, eng().uiReleased(luaL_checkstring(L, 1)));
    return 1;
}
int ui_size(lua_State* L) {
    const ImVec2 s = ImGui::GetIO().DisplaySize;
    lua_pushnumber(L, s.x);
    lua_pushnumber(L, s.y);
    return 2;
}
int ui_dragged(lua_State* L) {
    const glm::vec2 d = eng().uiDragged(luaL_checkstring(L, 1));
    lua_pushnumber(L, d.x);
    lua_pushnumber(L, d.y);
    return 2;
}
int ui_clicked(lua_State* L) {
    lua_pushboolean(L, eng().uiClicked(luaL_checkstring(L, 1)));
    return 1;
}
int ui_has(lua_State* L) {
    lua_pushboolean(L, eng().uiDocument().has(luaL_checkstring(L, 1)));
    return 1;
}
int ui_reset(lua_State* L) {
    (void)L;
    eng().uiDocument().resetOverrides();
    return 0;
}

int camera_basis(lua_State* L) {
    // Direita e frente da camera no plano do chao: o jogo posiciona o que
    // precisa em relacao a TELA (a esquerda, a direita, a frente) com isso.
    const Vec3 f = eng().camera().forward();
    const Vec3 flat = glm::length(Vec2(f.x, f.z)) > 1e-4f ? glm::normalize(Vec3(f.x, 0.0f, f.z)) : Vec3(0, 0, 1);
    pushVec3(L, glm::normalize(glm::cross(Vec3(0.0f, 1.0f, 0.0f), flat)));
    pushVec3(L, flat);
    return 2;
}


// LOOK da cena por script: os presets de data/diorama.json (grading, god
// rays) e o desfoque de profundidade. Generico: vale pra RPG, pra maquete e
// pra estudo de arquitetura - e' o "visual desta cena", nao uma regra de jogo.
int world_set_look(lua_State* L) {
    // TABELA em vez de nome: o look do proprio jogo, com os mesmos campos dos
    // presets de data/diorama.json (so' os que vierem mudam; o resto fica).
    // Um jogo tem a sua direcao de arte sem precisar pôr preset no motor.
    if (lua_istable(L, 1)) {
        nlohmann::json j = nlohmann::json::object();
        lua_pushnil(L);
        while (lua_next(L, 1) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char* k = lua_tostring(L, -2);
                if (lua_type(L, -1) == LUA_TNUMBER) j[k] = lua_tonumber(L, -1);
                else if (lua_type(L, -1) == LUA_TBOOLEAN) j[k] = lua_toboolean(L, -1) != 0;
                else if (lua_type(L, -1) == LUA_TSTRING) j[k] = lua_tostring(L, -1);
                else if (lua_type(L, -1) == LUA_TVECTOR) { // cor como vector(r, g, b)
                    const float* v = lua_tovector(L, -1);
                    j[k] = {v[0], v[1], v[2]};
                } else if (lua_type(L, -1) == LUA_TTABLE) { // cor como {r, g, b}
                    nlohmann::json arr = nlohmann::json::array();
                    for (int i = 1; i <= 4; ++i) {
                        lua_rawgeti(L, -1, i);
                        if (lua_type(L, -1) == LUA_TNUMBER) arr.push_back(lua_tonumber(L, -1));
                        lua_pop(L, 1);
                    }
                    if (!arr.empty()) j[k] = arr;
                }
            }
            lua_pop(L, 1);
        }
        eng().lookConfig().loadFromJson(j);
        eng().lookConfig().name = j.value("name", std::string("custom"));
        lua_pushboolean(L, 1);
        return 1;
    }
    const char* name = luaL_checkstring(L, 1);
    const nlohmann::json& root = eng().dioramaConfig().root();
    if (!root.contains("presets") || !root["presets"].contains(name)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    eng().lookConfig().loadFromJson(root["presets"][name]);
    eng().lookConfig().name = name;
    lua_pushboolean(L, 1);
    return 1;
}
int world_look(lua_State* L) {
    lua_pushstring(L, eng().lookConfig().name.c_str());
    return 1;
}
// Nevoa do jogo: tabela {color = vector|{r,g,b}, start, end, opacity, height,
// falloff} (os campos do bloco "fog" do .env) vence a do mapa e a do clima;
// nil devolve o comando para eles. Vale ate' a proxima troca de mapa.
int world_set_fog(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        eng().setScriptFog(MapFog{});
        return 0;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    nlohmann::json j = nlohmann::json::object();
    for (const char* k : {"start", "end", "opacity", "height", "falloff"}) {
        lua_getfield(L, 1, k);
        if (lua_type(L, -1) == LUA_TNUMBER) j[k] = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_getfield(L, 1, "color");
    if (lua_type(L, -1) == LUA_TVECTOR) {
        const float* v = lua_tovector(L, -1);
        j["color"] = {v[0], v[1], v[2]};
    } else if (lua_type(L, -1) == LUA_TTABLE) {
        nlohmann::json c = nlohmann::json::array();
        for (int i = 1; i <= 3; ++i) {
            lua_rawgeti(L, -1, i);
            c.push_back(lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : 0.0);
            lua_pop(L, 1);
        }
        j["color"] = c;
    }
    lua_pop(L, 1);
    eng().setScriptFog(parseMapFog(j));
    return 0;
}
// Escala de tempo do mundo (hit-stop, camera lenta): animacao de sprite, VFX,
// clima e o dt dos scripts andam escalados; `seconds` conta em tempo cheio e
// devolve a 1 sozinho (0 ou nada = ate' a proxima chamada). O som e o
// clarao/fade de tela seguem.
int world_set_time_scale(lua_State* L) {
    eng().timer().setTimeScale(float(luaL_checknumber(L, 1)), float(luaL_optnumber(L, 2, 0.0)));
    return 0;
}
int world_time_scale(lua_State* L) {
    lua_pushnumber(L, eng().timer().timeScale());
    return 1;
}
// Grade de caminhada do motor exposta ao jogo (alcance de pontos, saidas):
// a mesma conta do andar dos personagens - grade do mapa (com block_slopes)
// mais os objetos solidos no raio.
int world_walkable(lua_State* L) {
    const float x = float(luaL_checknumber(L, 1)), z = float(luaL_checknumber(L, 2));
    const float radius = float(luaL_optnumber(L, 3, 0.0));
    const Vec3 p(x, eng().sampleTerrainHeight(x, z), z);
    lua_pushboolean(L, eng().navWalkable(x, z) && !eng().blockedAt(p, radius));
    return 1;
}
// Caminho no chao (ja' alisado) de `de` a `para`, ou nil se nao ha'.
int world_path(lua_State* L) {
    // Busca longa num mapa grande e' trabalho do motor, nao laco do script:
    // fora do orcamento de tempo da chamada (como game.load).
    ScriptHost::NativeWork trabalho;
    const Vec3 from = checkVec3(L, 1), to = checkVec3(L, 2);
    const float radius = float(luaL_optnumber(L, 3, 0.0));
    std::vector<Vec3> path;
    if (!eng().findPath(from, to, radius, path)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(path.size()), 0);
    for (size_t i = 0; i < path.size(); ++i) {
        pushVec3(L, path[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}
// Consultas do chao com os objetos solidos (pontes, passarelas, lajes de kit):
// world.walkable/ground_height so' conhecem o terreno.
float fromYArg(lua_State* L, int idx) {
    return lua_isnoneornil(L, idx) ? FLT_MAX : static_cast<float>(luaL_checknumber(L, idx));
}
int world_floor_height(lua_State* L) {
    bool onModel = false;
    lua_pushnumber(L, eng().floorHeight(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                        fromYArg(L, 3), &onModel));
    lua_pushboolean(L, onModel);
    return 2;
}
int world_floor_walkable(lua_State* L) {
    lua_pushboolean(L, eng().floorWalkable(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                           float(luaL_optnumber(L, 3, 0.0)), fromYArg(L, 4)));
    return 1;
}
int world_liquid_at(lua_State* L) {
    Engine::LiquidAt at;
    if (!eng().liquidAt(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), at)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushstring(L, at.kind == 1 ? "lava" : "water");
    lua_setfield(L, -2, "kind");
    lua_pushnumber(L, at.level);
    lua_setfield(L, -2, "level");
    lua_pushinteger(L, at.index + 1); // 0 = o plano de agua global
    lua_setfield(L, -2, "index");
    // Lava de um vizinho em cena: index conta nos liquidos dele.
    if (at.map && at.map != eng().currentMap()) {
        lua_pushstring(L, at.map->mapName.c_str());
        lua_setfield(L, -2, "map");
    }
    return 1;
}
// Grade inteira numa chamada: dois buffers (alturas f32, flags u8), x mais
// rapido. Amostrar ponto a ponto passava dos 250 ms com ~80 mil pontos.
int world_ground_grid(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    const float x0 = float(luaL_checknumber(L, 1)), z0 = float(luaL_checknumber(L, 2));
    const float step = float(luaL_checknumber(L, 3));
    const int nx = int(luaL_checkinteger(L, 4)), nz = int(luaL_checkinteger(L, 5));
    const float radius = float(luaL_optnumber(L, 6, 0.0));
    luaL_argcheck(L, step > 0.0f, 3, "step must be > 0");
    luaL_argcheck(L, nx > 0 && nz > 0 && size_t(nx) * size_t(nz) <= (size_t(1) << 22), 4, "1..4M points");
    const size_t n = size_t(nx) * size_t(nz);
    auto* heights = static_cast<float*>(lua_newbuffer(L, n * sizeof(float)));
    auto* flags = static_cast<uint8_t*>(lua_newbuffer(L, n));
    eng().sampleGround(x0, z0, step, nx, nz, radius, heights, flags);
    return 2;
}
int world_set_dof(lua_State* L) {
    eng().setDofEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}
// Borrao de movimento e aberracao cromatica vem do preset de QUALIDADE, mas
// sao escolha de direcao de arte: num jogo de sprite o borrao arrasta o
// billboard (ele gira com a camera, e o vetor de movimento da tela e' o de
// geometria parada) e a franja colorida come o contorno do pixel.
// O segundo argumento (opcional) e' a forca: 1 = a do motor para o borrao,
// 0,5 para a aberracao.
int world_set_motion_blur(lua_State* L) {
    auto& s = eng().postSettings();
    s.enableMotionBlur = lua_toboolean(L, 1) != 0;
    if (lua_isnumber(L, 2)) s.motionBlurAmount = std::clamp(static_cast<float>(lua_tonumber(L, 2)), 0.0f, 4.0f);
    return 0;
}
int world_set_chromatic_aberration(lua_State* L) {
    auto& s = eng().postSettings();
    s.enableChromaticAberration = lua_toboolean(L, 1) != 0;
    if (lua_isnumber(L, 2)) s.chromaticAberrationAmount = std::clamp(static_cast<float>(lua_tonumber(L, 2)), 0.0f, 5.0f);
    return 0;
}
// RAIO LIMPO: dentro do circulo em volta do centro da tela, sem borrao nem
// aberracao; fora, os dois crescem suaves. Fracoes da altura da tela; raio 0
// devolve o efeito a tela inteira.
int world_set_effect_radius(lua_State* L) {
    auto& s = eng().postSettings();
    s.effectClearRadius = std::clamp(static_cast<float>(luaL_checknumber(L, 1)), 0.0f, 2.0f);
    if (lua_isnumber(L, 2)) s.effectClearFeather = std::clamp(static_cast<float>(lua_tonumber(L, 2)), 0.0f, 2.0f);
    return 0;
}

// ONDE A LENTE FOCA. Sem isto o desfoque de profundidade so' sabe responder
// "o jogador aproximou quanto?" — a curva de zoom —, que e' a resposta certa
// para quem anda pelo mapa e errada para toda cena que tem um ASSUNTO: um
// palco de combate, uma mesa de dialogo, uma peca em exame. Dando o ponto,
// quem montou a cena diz onde esta' o assunto, e o borrao vira moldura em
// vez de enfeite. `nil` devolve a lente a' curva.
//
// A abertura aqui e' "borrao por unidade de profundidade": com o assunto a
// 450 unidades e o fundo 200 atras dele, 0.15 deixa o fundo com cerca de um
// pixel e meio e o assunto inteiro cravado.
int world_set_focus(lua_State* L) {
    auto& s = eng().postSettings();
    if (lua_isnoneornil(L, 1)) {
        s.focusOverride = false;
        eng().setFocusPoint(nullptr);
        return 0;
    }
    const Vec3 p = checkVec3(L, 1);
    eng().setFocusPoint(&p); // camera.see_through(.., "focus")
    const Camera& c = eng().camera();
    // PROFUNDIDADE AO LONGO DO EIXO DA CAMERA, nao distancia em linha reta:
    // e' o que o mapa de profundidade guarda, e e' contra ele que o shader
    // de CoC compara. Com a distancia reta, um assunto fora do centro da
    // tela ficaria desfocado por estar "longe" quando esta' no mesmo plano.
    const Vec3 frente = glm::normalize(c.forward());
    s.focusDistance = glm::max(glm::dot(p - c.position(), frente), 0.01f);
    s.focusAperture = lua_isnoneornil(L, 2) ? -1.0f : static_cast<float>(luaL_checknumber(L, 2));
    s.focusForeground = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    // Faixa nitida e teto do borrao (PostSettings::focusBand/focusMaxBlur).
    s.focusBand = lua_isnoneornil(L, 4) ? -1.0f : std::max(0.0f, static_cast<float>(luaL_checknumber(L, 4)));
    s.focusMaxBlur = lua_isnoneornil(L, 5) ? -1.0f : std::max(0.0f, static_cast<float>(luaL_checknumber(L, 5)));
    s.focusOverride = true;
    return 0;
}

// Recorte por volume (Engine::setCutaway): slot 1..4 e a tabela do volume, ou
// nil para desligar. Medidas em unidades do mundo.
int world_set_cutaway(lua_State* L) {
    const int slot = int(luaL_checkinteger(L, 1)) - 1;
    if (lua_isnoneornil(L, 2)) {
        eng().setCutaway(slot, 0, Vec3(0.0f), Vec3(0.0f), 0.0f, 0.0f, 1.0f);
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    auto num = [L](const char* k, float def) {
        lua_getfield(L, 2, k);
        const float v = lua_isnumber(L, -1) ? float(lua_tonumber(L, -1)) : def;
        lua_pop(L, 1);
        return v;
    };
    lua_getfield(L, 2, "shape");
    const std::string shape = lua_isstring(L, -1) ? lua_tostring(L, -1) : "box";
    lua_pop(L, 1);
    lua_getfield(L, 2, "center");
    const Vec3 center = checkVec3(L, -1);
    lua_pop(L, 1);
    Vec3 half(0.0f);
    int kind = 1;
    if (shape == "cylinder") {
        kind = 2;
        half = Vec3(num("radius", 0.0f), 0.5f * num("height", 0.0f), 0.0f);
    } else if (shape == "box") {
        lua_getfield(L, 2, "size");
        if (!lua_isnil(L, -1)) half = 0.5f * checkVec3(L, -1);
        lua_pop(L, 1);
    } else {
        luaL_error(L, "set_cutaway: shape must be \"box\" or \"cylinder\"");
    }
    // Borda suave padrao: 15% da menor medida do volume.
    const float smallest = kind == 2 ? std::min(half.x, half.y) : std::min({half.x, half.y, half.z});
    eng().setCutaway(slot, kind, center, half, num("yaw", 0.0f), num("soft", 0.15f * smallest),
                     num("opacity", 0.0f));
    return 0;
}

// Juncao malha x terreno (Engine::setGroundBlend).
int world_set_ground_blend(lua_State* L) {
    eng().setGroundBlend(static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.3)));
    return 0;
}

// Personagem novo em tempo de execucao: o jogo monta o elenco que precisa
// (NPC de cidade, figurantes de uma cena) sem passar pelo arquivo de cena. Devolve
// falso se o sprite nao abriu.
// require("nome"): a tabela que o outro script devolveu. Carrega na hora se
// ele ainda nao entrou. Ver ScriptHost::moduleRef.
int api_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    ScriptHost* host = ScriptHost::current();
    if (!host) luaL_errorL(L, "require is only available while the game is running");
    std::string err;
    const int ref = host->moduleRef(name, err);
    if (ref < 0) luaL_errorL(L, "%s", err.c_str());
    lua_getref(L, ref);
    return 1;
}

int player_freeze(lua_State* L) {
    if (PlayerController* p = ScriptBridge::player()) p->setInputFrozen(lua_toboolean(L, 1) != 0);
    return 0;
}
int player_set_run(lua_State* L) {
    if (PlayerController* p = ScriptBridge::player()) p->setRunLatched(lua_toboolean(L, 1) != 0);
    return 0;
}
int player_hide(lua_State* L) {
    if (PlayerController* p = ScriptBridge::player()) p->setHidden(lua_toboolean(L, 1) != 0);
    return 0;
}
int camera_free(lua_State* L) {
    if (PlayerController* p = ScriptBridge::player()) p->setFreeCamera(lua_toboolean(L, 1) != 0);
    return 0;
}
int camera_keep_inside_map(lua_State* L) {
    if (PlayerController* p = ScriptBridge::player()) p->setKeepViewInsideMap(lua_toboolean(L, 1) != 0);
    return 0;
}
// Parede entre a camera e o jogador sai em dither (Engine::setSeeThrough).
int camera_see_through(lua_State* L) {
    static const char* const kCenters[] = {"player", "focus", nullptr};
    const bool atFocus = luaL_checkoption(L, 3, "player", kCenters) == 1;
    eng().setSeeThrough(static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.25)),
                        atFocus);
    return 0;
}
// Geometria colada na camera sai em dither (Engine::setNearFade).
int camera_near_fade(lua_State* L) {
    eng().setNearFade(static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.0)));
    return 0;
}

// Arrasto do mouse/dedo gira a orbita do jogador (Engine::setOrbitInput).
int camera_set_orbit_input(lua_State* L) {
    eng().setOrbitInput(lua_toboolean(L, 1) != 0);
    return 0;
}

int player_frozen(lua_State* L) {
    const PlayerController* p = ScriptBridge::player();
    lua_pushboolean(L, p && p->inputFrozen());
    return 1;
}

int characters_add(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (eng().findCharacter(name) >= 0) luaL_error(L, "there is already a character named '%s'", name);
    // Carregar o sprite (e, no primeiro do mapa, a rede de PBR junto) e'
    // trabalho do motor: nao conta no cao de guarda do script.
    ScriptHost::NativeWork trabalho;
    const int idx = eng().addCharacter(name, luaL_checkstring(L, 2), checkVec3(L, 3),
                                       static_cast<float>(luaL_optnumber(L, 4, 0.0)));
    // SPRITE QUE NAO ABRIU = FALSO, como a declaracao promete. O motor guarda
    // o personagem mesmo sem sprite (o editor precisa: a cena nao pode perder
    // um personagem porque o arquivo dele sumiu), mas para o script isso
    // devolvia verdadeiro e nenhum jogo conseguia notar arte faltando - nem
    // tentar outra. O personagem sem sprite sai, para o nome ficar livre para
    // a segunda tentativa.
    if (idx >= 0) {
        auto& c = eng().characters()[static_cast<size_t>(idx)];
        if (!c.look.loaded) {
            c.removed = true;
            c.name.clear();
            lua_pushboolean(L, 0);
            return 1;
        }
    }
    lua_pushboolean(L, idx >= 0);
    return 1;
}

// ---------------------------------------------------------------- perf
// Contador de desempenho para o jogo desenhar (o F3 e' do editor). Os numeros
// do quadro sao os que o laco principal ja' mede para o F3 e a telemetria; o
// que vem da maquina (temperatura, bateria) e' lido de /sys no maximo uma vez
// por segundo, so' quando um script pede.

int perf_fps(lua_State* L) {
    lua_pushnumber(L, eng().fps());
    return 1;
}
int perf_frame(lua_State* L) {
    lua_pushnumber(L, eng().timer().realDeltaTime() * 1000.0f);
    lua_pushnumber(L, ScriptBridge::cpuWorkMs());
    const float gpu = ScriptBridge::gpuMs();
    if (gpu > 0.0f) lua_pushnumber(L, gpu);
    else lua_pushnil(L);
    return 3;
}
int perf_resolution(lua_State* L) {
    const VkExtent2D cena = ScriptBridge::renderExtent();
    const VkExtent2D tela = eng().displayExtent();
    lua_pushinteger(L, static_cast<int>(cena.width));
    lua_pushinteger(L, static_cast<int>(cena.height));
    lua_pushnumber(L, tela.width ? static_cast<double>(cena.width) / tela.width : 1.0);
    lua_pushinteger(L, static_cast<int>(tela.width));
    lua_pushinteger(L, static_cast<int>(tela.height));
    return 5;
}
int perf_fps_cap(lua_State* L) {
    lua_pushinteger(L, ScriptBridge::fpsCap());
    return 1;
}
int perf_set_fps_cap(lua_State* L) {
    ScriptBridge::fpsCap() = std::clamp(static_cast<int>(luaL_checknumber(L, 1)), 0, 1000);
    return 0;
}

// Leitura da maquina. NaN = a maquina nao informa (vira nil no script).
struct SysReadings {
    double cpuTemp = NAN, gpuTemp = NAN, gpuWatts = NAN;
    double battery = NAN, batteryTemp = NAN, batteryWatts = NAN;
    int charging = -1;          // -1 sem bateria
    int thermal = -1;           // AThermalStatus (Android), -1 sem
    double thermalHeadroom = NAN;
};

#if defined(__linux__)
bool readSysNumber(const std::string& path, double& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[64] = {};
    const bool ok = std::fgets(buf, sizeof(buf), f) != nullptr;
    std::fclose(f);
    if (!ok) return false;
    char* fim = nullptr;
    const double v = std::strtod(buf, &fim);
    if (fim == buf) return false;
    out = v;
    return true;
}
std::string readSysLine(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[64] = {};
    if (!std::fgets(buf, sizeof(buf), f)) buf[0] = '\0';
    std::fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// Onde ler, achado uma vez: a lista de /sys nao muda com o jogo aberto.
struct SysPaths {
    std::string cpuTemp, gpuTemp, battery;
};
const SysPaths& sysPaths() {
    static const SysPaths p = [] {
        SysPaths r;
        std::error_code ec;
        // hwmon: sensor de pacote da CPU (Intel, AMD) e o da GPU com driver
        // aberto (AMD, nouveau). NVIDIA proprietaria vai pelo NVML abaixo.
        for (const auto& e : std::filesystem::directory_iterator("/sys/class/hwmon", ec)) {
            const std::string d = e.path().string();
            const std::string nome = readSysLine(d + "/name");
            if (r.cpuTemp.empty() && (nome == "coretemp" || nome == "k10temp" || nome == "zenpower" ||
                                      nome == "cpu_thermal"))
                r.cpuTemp = d + "/temp1_input";
            else if (r.gpuTemp.empty() && (nome == "amdgpu" || nome == "nouveau" || nome == "radeon"))
                r.gpuTemp = d + "/temp1_input";
        }
        // Bateria: o nome do Android primeiro (listar a pasta costuma ser
        // negado ao app, abrir o arquivo nao); no PC, a primeira do tipo Battery.
        if (readSysLine("/sys/class/power_supply/battery/type") == "Battery")
            r.battery = "/sys/class/power_supply/battery";
        else {
            for (const auto& e : std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
                const std::string d = e.path().string();
                if (readSysLine(d + "/type") == "Battery" && readSysLine(d + "/scope") != "Device") {
                    r.battery = d;
                    break;
                }
            }
        }
        return r;
    }();
    return p;
}

// NVML (GPU NVIDIA com driver proprietario): temperatura e consumo. O
// SystemMonitor ja' carrega a biblioteca para uso e VRAM; nvmlInit conta
// referencias, entao abrir de novo aqui nao reinicia nada.
struct Nvml {
    using InitFn = int (*)();
    using HandleFn = int (*)(unsigned int, void**);
    using TempFn = int (*)(void*, int, unsigned int*);
    using PowerFn = int (*)(void*, unsigned int*);
    TempFn temp = nullptr;
    PowerFn power = nullptr;
    void* device = nullptr;
};
const Nvml& nvml() {
    static const Nvml n = [] {
        Nvml r;
#if !defined(__ANDROID__)
        void* lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
        if (!lib) return r;
        auto init = reinterpret_cast<Nvml::InitFn>(dlsym(lib, "nvmlInit_v2"));
        auto handle = reinterpret_cast<Nvml::HandleFn>(dlsym(lib, "nvmlDeviceGetHandleByIndex_v2"));
        if (!init || !handle || init() != 0 || handle(0, &r.device) != 0) return r;
        r.temp = reinterpret_cast<Nvml::TempFn>(dlsym(lib, "nvmlDeviceGetTemperature"));
        r.power = reinterpret_cast<Nvml::PowerFn>(dlsym(lib, "nvmlDeviceGetPowerUsage"));
#endif
        return r;
    }();
    return n;
}

#if defined(__ANDROID__)
// Estado termico do sistema (AThermal, API 30; folga, API 31). Por dlsym: o
// pacote declara API 28, e nos aparelhos antigos os simbolos nao existem.
struct Thermal {
    using StatusFn = int (*)(void*);
    using HeadroomFn = float (*)(void*, int);
    void* manager = nullptr;
    StatusFn status = nullptr;
    HeadroomFn headroom = nullptr;
};
const Thermal& thermal() {
    static const Thermal t = [] {
        Thermal r;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return r;
        using AcquireFn = void* (*)();
        auto acquire = reinterpret_cast<AcquireFn>(dlsym(lib, "AThermal_acquireManager"));
        r.status = reinterpret_cast<Thermal::StatusFn>(dlsym(lib, "AThermal_getCurrentThermalStatus"));
        r.headroom = reinterpret_cast<Thermal::HeadroomFn>(dlsym(lib, "AThermal_getThermalHeadroom"));
        if (acquire) r.manager = acquire();
        return r;
    }();
    return t;
}
#endif

void readSystem(SysReadings& r) {
    const SysPaths& p = sysPaths();
    double v = 0.0;
    // hwmon e o temp da bateria: milesimos de grau e decimos de grau.
    if (!p.cpuTemp.empty() && readSysNumber(p.cpuTemp, v)) r.cpuTemp = v / 1000.0;
    if (!p.gpuTemp.empty() && readSysNumber(p.gpuTemp, v)) r.gpuTemp = v / 1000.0;
    const Nvml& n = nvml();
    unsigned int u = 0;
    if (n.temp && n.temp(n.device, 0 /* NVML_TEMPERATURE_GPU */, &u) == 0) r.gpuTemp = u;
    if (n.power && n.power(n.device, &u) == 0) r.gpuWatts = u / 1000.0;
    if (!p.battery.empty()) {
        const std::string& b = p.battery;
        if (readSysNumber(b + "/capacity", v)) r.battery = v;
        if (readSysNumber(b + "/temp", v)) r.batteryTemp = v / 10.0;
        // Consumo: power_now (uW) ou corrente (uA) x tensao (uV). O sinal da
        // corrente muda de fabricante para fabricante: vale o modulo.
        double i = 0.0, volts = 0.0;
        if (readSysNumber(b + "/power_now", v)) r.batteryWatts = std::fabs(v) / 1e6;
        else if (readSysNumber(b + "/current_now", i) && readSysNumber(b + "/voltage_now", volts))
            r.batteryWatts = std::fabs(i * volts) / 1e12;
        const std::string st = readSysLine(b + "/status");
        if (!st.empty()) r.charging = (st == "Charging" || st == "Full") ? 1 : 0;
    }
#if defined(__ANDROID__)
    const Thermal& t = thermal();
    if (t.manager && t.status) r.thermal = t.status(t.manager);
    // Chamar mais de uma vez por segundo devolve NaN: o cache abaixo cuida.
    if (t.manager && t.headroom) r.thermalHeadroom = static_cast<double>(t.headroom(t.manager, 0));
#endif
}
#else
void readSystem(SysReadings&) {}
#endif

void setNumberField(lua_State* L, const char* k, double v) {
    if (std::isnan(v)) return;
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}

int perf_system(lua_State* L) {
    ScriptHost::NativeWork trabalho; // leitura de arquivos, nao laco do script
    static SysReadings s_cache;
    static std::chrono::steady_clock::time_point s_last{};
    const auto agora = std::chrono::steady_clock::now();
    if (s_last.time_since_epoch().count() == 0 || agora - s_last >= std::chrono::seconds(1)) {
        SysReadings r;
        readSystem(r);
        // A folga termica vem NaN quando o sistema acha cedo demais: fica a anterior.
        if (std::isnan(r.thermalHeadroom)) r.thermalHeadroom = s_cache.thermalHeadroom;
        s_cache = r;
        s_last = agora;
    }
    const SysReadings& r = s_cache;
    lua_createtable(L, 0, 12);
    // RAM, uso de CPU e de GPU: os do SystemMonitor (o motor ja' le uma vez
    // por segundo). 0 = nao medido aqui.
    if (const size_t ram = SystemMonitor::processRamBytes()) setNumberField(L, "ram_mb", ram / (1024.0 * 1024.0));
    const double vram = ScriptBridge::vramMb();
    if (vram > 0.0) setNumberField(L, "vram_mb", vram);
    if (const float c = SystemMonitor::processCpuPercent(); c > 0.0f) setNumberField(L, "cpu_percent", c);
    if (const float g = SystemMonitor::gpuCorePercent(); g > 0.0f) setNumberField(L, "gpu_percent", g);
    setNumberField(L, "cpu_temp", r.cpuTemp);
    setNumberField(L, "gpu_temp", r.gpuTemp);
    setNumberField(L, "gpu_watts", r.gpuWatts);
    setNumberField(L, "battery", r.battery);
    setNumberField(L, "battery_temp", r.batteryTemp);
    setNumberField(L, "battery_watts", r.batteryWatts);
    if (r.charging >= 0) {
        lua_pushboolean(L, r.charging);
        lua_setfield(L, -2, "charging");
    }
    static const char* const kThermal[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    if (r.thermal >= 0 && r.thermal <= 6) {
        lua_pushstring(L, kThermal[r.thermal]);
        lua_setfield(L, -2, "thermal");
    }
    setNumberField(L, "thermal_headroom", r.thermalHeadroom);
    return 1;
}

void lib(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, name);
}

} // namespace

Key scriptKeyFromName(const char* name) {
    if (name[0] && !name[1]) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<Key>(c);
    }
    static const struct { const char* name; Key key; } kNamed[] = {
        {"space", Key::Space}, {"enter", Key::Enter}, {"escape", Key::Escape}, {"tab", Key::Tab},
        {"backspace", Key::Backspace}, {"left", Key::Left}, {"right", Key::Right}, {"up", Key::Up},
        {"down", Key::Down}, {"shift", static_cast<Key>(340)}, {"ctrl", static_cast<Key>(341)},
        {"alt", static_cast<Key>(342)},
    };
    for (const auto& k : kNamed)
        if (equalsNoCase(name, k.name)) return k.key;
    return Key::Unknown;
}

Vec3 scriptPlayerPosition() {
    PlayerController* p = ScriptBridge::engine ? ScriptBridge::player() : nullptr;
    return p ? p->pos() : Vec3(0.0f);
}

int scriptFindModel(const std::string& name) {
    // Mapa nome -> índice, refeito quando a lista de instâncias muda (troca
    // de mapa). A busca linear em milhares de modelos por chamada pesaria.
    static std::unordered_map<std::string, int> cache;
    static const ModelInstance* cachedData = nullptr;
    static size_t cachedSize = 0;
    const auto& insts = instances();
    if (insts.data() != cachedData || insts.size() != cachedSize) {
        cache.clear();
        for (size_t i = 0; i < insts.size(); ++i) cache.emplace(insts[i].name, static_cast<int>(i));
        cachedData = insts.data();
        cachedSize = insts.size();
    }
    const auto it = cache.find(name);
    return it == cache.end() ? -1 : it->second;
}

void registerScriptApi(lua_State* L, Engine& engine) {
    ScriptBridge::engine = &engine;
    lua_pushcfunction(L, api_log, "log");
    lua_setglobal(L, "log");
    lua_pushcfunction(L, api_log, "print");
    lua_setglobal(L, "print");

    static const luaL_Reg on[] = {
        {"start", on_start}, {"update", on_update}, {"every", on_every}, {"key", on_key},
        {"near", on_near}, {"hour", on_hour}, {"enter", on_enter}, {"leave", on_leave}, {nullptr, nullptr}};
    static const luaL_Reg vars[] = {{"get", vars_get}, {"set", vars_set}, {"add", vars_add}, {nullptr, nullptr}};
    static const luaL_Reg ui[] = {{"message", ui_message}, {"minimap_label", ui_minimap_label}, {"show_hud", ui_show_hud}, {"show_minimap", ui_show_minimap},
                                  {"load", ui_load}, {"place", ui_place}, {"image_rect", ui_image_rect}, {"text", ui_text}, {"value", ui_value}, {"image", ui_image},
                                  {"visible", ui_visible}, {"color", ui_color}, {"has", ui_has}, {"clicked", ui_clicked}, {"dragged", ui_dragged}, {"size", ui_size}, {"held", ui_held}, {"released", ui_released},
                                  {"reset", ui_reset}, {nullptr, nullptr}};
    static const luaL_Reg world[] = {
        {"time", world_time}, {"set_time", world_set_time}, {"time_between", world_time_between},
        {"weather", world_weather}, {"is_weather", world_is_weather}, {"set_weather", world_set_weather},
        {"look", world_look}, {"set_look", world_set_look}, {"set_dof", world_set_dof}, {"set_fog", world_set_fog}, {"set_time_scale", world_set_time_scale}, {"time_scale", world_time_scale}, {"walkable", world_walkable}, {"path", world_path}, {"floor_height", world_floor_height}, {"floor_walkable", world_floor_walkable}, {"liquid_at", world_liquid_at}, {"ground_grid", world_ground_grid}, {"set_motion_blur", world_set_motion_blur},
        {"set_chromatic_aberration", world_set_chromatic_aberration}, {"set_effect_radius", world_set_effect_radius},
        {"set_focus", world_set_focus}, {"set_ground_blend", world_set_ground_blend}, {"set_cutaway", world_set_cutaway},
        {"ground_height", world_ground_height}, {"ground_layers", world_ground_layers}, {"liquids", world_liquids}, {"set_liquid_level", world_set_liquid_level},
        {"block_slopes", world_block_slopes}, {"set_walk_cost", world_set_walk_cost}, {"walk_cost", world_walk_cost},
        {"map", world_map}, {"load_map", world_load_map},
        {"warp_map", world_warp_map}, {"warping", world_warping}, {nullptr, nullptr}};
    static const luaL_Reg player[] = {
        {"position", player_position}, {"set_position", player_set_position}, {"set_sprite", player_set_sprite}, {"near", player_near},
        {"inside", player_inside}, {"say", player_say}, {"play", player_play}, {"animation", player_animation},
        {"freeze", player_freeze}, {"frozen", player_frozen}, {"hide", player_hide}, {"set_run", player_set_run},
        {nullptr, nullptr}};
    static const luaL_Reg camera[] = {
        {"target", camera_target}, {"set_target", camera_set_target},
        {"orbit", camera_orbit}, {"set_orbit", camera_set_orbit}, {"basis", camera_basis},
        {"view", camera_view},
        {"free", camera_free}, {"keep_inside_map", camera_keep_inside_map},
        {"see_through", camera_see_through}, {"near_fade", camera_near_fade}, {"set_orbit_input", camera_set_orbit_input}, {nullptr, nullptr}};
    static const luaL_Reg fx[] = {
        {"shake_camera", fx_shake_camera}, {"number", fx_number}, {"fade", fx_fade}, {"flash", fx_flash}, {"dim", fx_dim},
        {"fading", fx_fading}, {nullptr, nullptr}};
    static const luaL_Reg input[] = {{"down", input_down}, {"pressed", input_pressed}, {"touch", input_touch}, {"touch_pad", input_touch_pad}, {"double_tapped", input_double_tapped}, {"gamepad", input_gamepad}, {"mouse_position", input_mouse_position}, {"scroll", input_scroll}, {"touches", input_touches}, {nullptr, nullptr}};
    static const luaL_Reg models[] = {
        {"count", models_count}, {"preload", models_preload}, {"find", models_find}, {"name", models_name},
        {"position", models_position}, {"set_position", models_set_position}, {"near", models_near},
        {"file", models_file}, {"scale", models_scale},
        {"set_rotation", models_set_rotation}, {"set_scale", models_set_scale},
        {"move_by", models_move_by}, {"rotate_by", models_rotate_by},
        {"visible", models_visible}, {"set_visible", models_set_visible},
        {"show", models_show}, {"hide", models_hide}, {"solid", models_solid}, {"set_solid", models_set_solid},
        {"play", models_play}, {"animation", models_animation}, {"animations", models_animations},
        {"morphs", models_morphs}, {"set_morph", models_set_morph},
        {nullptr, nullptr}};
    static const luaL_Reg characters[] = {
        {"exists", characters_exists}, {"preload", characters_preload}, {"position", characters_position},
        {"set_position", characters_set_position}, {"walk_to", characters_walk_to},
        {"walking", characters_walking}, {"face", characters_face}, {"show", characters_show}, {"say", characters_say}, {"play", characters_play}, {"animation", characters_animation},
        {"follow", characters_follow}, {"patrol", characters_patrol}, {"stop", characters_stop},
        {"set_shadow", characters_set_shadow},
        {"hide", characters_hide}, {"set_sprite", characters_set_sprite},
        {"add", characters_add},
        {"flash", characters_flash}, {"shake", characters_shake}, {"lunge", characters_lunge},
        {"pop", characters_pop}, {"fade", characters_fade}, {"set_scale", characters_set_scale},
        {nullptr, nullptr}};
    static const luaL_Reg lights[] = {
        {"count", lights_count}, {"position", lights_position}, {"set_position", lights_set_position},
        {"color", lights_color}, {"set_color", lights_set_color}, {"intensity", lights_intensity},
        {"set_intensity", lights_set_intensity}, {"enabled", lights_enabled},
        {"set_enabled", lights_set_enabled}, {"turn_on", lights_turn_on}, {"turn_off", lights_turn_off},
        {"set_sprite_shadow", lights_set_sprite_shadow}, {nullptr, nullptr}};
    static const luaL_Reg audio[] = {
        {"play", audio_play}, {"play_at", audio_play_at}, {"loop", audio_loop}, {"loop_at", audio_loop_at},
        {"stop", audio_stop}, {"playing", audio_playing}, {"music", audio_music}, {"stop_music", audio_stop_music},
        {"set_volume", audio_set_volume}, {"set_pitch", audio_set_pitch}, {"music_volume", audio_music_volume},
        {"group_volume", audio_group_volume}, {"preload", audio_preload}, {nullptr, nullptr}};
    lib(L, "on", on);
    lib(L, "audio", audio);
    static const luaL_Reg game[] = {{"save", game_save}, {"load", game_load}, {"restart", game_restart}, {"save_settings", game_save_settings}, {"settings", game_settings}, {"has_save", game_has_save},
                                    {"delete_save", game_delete_save}, {nullptr, nullptr}};
    lib(L, "game", game);
    lib(L, "vars", vars);
    lib(L, "ui", ui);
    lib(L, "world", world);
    lib(L, "player", player);
    lib(L, "camera", camera);
    lib(L, "input", input);
    lib(L, "models", models);
    lib(L, "lights", lights);
    lua_pushcfunction(L, api_require, "require");
    lua_setglobal(L, "require");
    static const luaL_Reg perf[] = {
        {"fps", perf_fps}, {"frame", perf_frame}, {"resolution", perf_resolution},
        {"fps_cap", perf_fps_cap}, {"set_fps_cap", perf_set_fps_cap}, {"system", perf_system},
        {nullptr, nullptr}};
    lib(L, "characters", characters);
    lib(L, "fx", fx);
    lib(L, "perf", perf);
    registerVfxApi(L, engine);
    registerObjectsApi(L, engine);
    registerRibbonApi(L, engine);
    NativeModule::registerApi(L);
}

std::string scriptApiDefinitions() {
    return R"(-- Generated by Eruption Engine. Do not edit: rewritten on every start.

-- Prints to the editor console.
declare function log(...: any): ()

-- The table another script of this project returned, loading it now if it has
-- not run yet. The name is the path without the extension, from the scripts
-- folder: require("data") or require("battle/tables"). Scripts that only hold
-- data (tables of classes, enemies, items) live in their own file this way.
declare function require(module: string): any

-- A model is referenced by its name (string) or its index (number, from 1).
type Model = string | number

-- Events. Call these at the top level of a script, not inside functions.
declare on: {
    start: (fn: () -> ()) -> (),
    update: (fn: (dt: number) -> ()) -> (),
    every: (seconds: number, fn: () -> ()) -> (),
    -- Key names: "A".."Z", "0".."9", "space", "enter", "escape", "tab",
    -- "left", "right", "up", "down", "shift", "ctrl", "alt".
    key: (key: string, fn: () -> ()) -> (),
    -- Fires when the player gets within `distance` of the model.
    near: (model: string, distance: number, fn: () -> ()) -> (),
    -- Fires when the clock passes this hour (0 to 24).
    hour: (hour: number, fn: () -> ()) -> (),
    -- Fires when the player walks into / out of a trigger area (Add > Trigger Area).
    enter: (area: string, fn: () -> ()) -> (),
    leave: (area: string, fn: () -> ()) -> (),
}

-- Variables shared by every script. They reset when the game starts.
declare vars: {
    get: (name: string) -> any,
    set: (name: string, value: any) -> (),
    add: (name: string, amount: number) -> (),
}

declare ui: {
    -- Shows a message on screen for some seconds (default 3).
    message: (text: string, seconds: number?) -> (),
    -- Text of the box under the minimap instead of map name, cell and time;
    -- nil goes back to the default.
    minimap_label: (text: string?) -> (),
    -- Position and size of an element of the loaded UI file, in percent of
    -- its parent (or of the screen), like CSS left/top/width/height. Without
    -- width/height the file's size stays; left = nil gives it back to the CSS.
    place: (selector: string, left: number?, top: number?, width: number?, height: number?) -> (),
    -- Crop of an image element in texture coordinates (0..1): a window into a
    -- bigger image. u0 = nil shows the whole image again.
    image_rect: (selector: string, u0: number?, v0: number?, u1: number?, v1: number?) -> (),
    -- Character bars (life, mana) and the minimap with the map name and time.
    -- Off in exported games until the game turns them on.
    show_hud: (on: boolean) -> (),
    show_minimap: (on: boolean) -> (),
    -- The game's own interface, described in a CSS file of the project and
    -- edited in Caldera: every rule with an id (#menu, #hp-1) is an element,
    -- and `--widget` says whether it is a rect, text, a bar or an image.
    -- The script only fills VALUES in; where each thing sits, in what color
    -- and what size, is the file's business, reloaded when it changes.
    load: (css: string) -> boolean,
    text: (selector: string, text: string) -> (),
    value: (selector: string, value: number) -> (),   -- bar, 0 to 1
    -- Troca o arquivo de um widget de imagem (--widget: image).
    image: (selector: string, png: string) -> (),
    visible: (selector: string, on: boolean) -> (),
    -- which: 0 = background / rail / text / image tint (multiplies the
    -- image; --image-tint in the file), 1 = bar fill or border.
    color: (selector: string, color: vector, which: number?, opacity: number?) -> (),
    has: (selector: string) -> boolean,
    -- Tapped (phone) or clicked (mouse) this frame: an element of the file
    -- that is on screen, or "minimap". Makes any element a button.
    clicked: (selector: string) -> boolean,
    -- Finger or mouse movement this frame, in screen pixels, for a drag that
    -- started on the element (0, 0 until it moves past a small slop): scroll
    -- a list by it.
    dragged: (selector: string) -> (number, number),
    -- Size of the interface in pixels (the unit of ui.dragged): scale a drag
    -- by it so scrolling feels the same on any screen.
    size: () -> (number, number),
    -- Seconds the finger (mouse button) has been held on a press that started
    -- on the element; 0 when not. With ui.released: hold to preview, let go
    -- to confirm.
    held: (selector: string) -> number,
    -- The press that started on the element was let go over it this frame.
    released: (selector: string) -> boolean,
    -- Drops everything the script set; the file decides again.
    reset: () -> (),
}

declare world: {
    -- Hour of the day, 0 to 24.
    time: () -> number,
    set_time: (hours: number) -> (),
    -- True between two hours; wraps past midnight (e.g. 18 to 6).
    time_between: (from: number, to: number) -> boolean,
    weather: () -> string,
    is_weather: (name: string) -> boolean,
    -- Weather by name, e.g. "clear", "rainy", "stormy".
    set_weather: (name: string) -> (),
    -- Terrain height at a world position.
    ground_height: (x: number, z: number) -> number,
    -- Painted ground layers around (x, z): average weight of each layer in
    -- a disc of `radius` (0 or nil: the nearest grid point), keyed by the
    -- layer's texture file. Empty if the map has no painted layers.
    ground_layers: (x: number, z: number, radius: number?) -> { [string]: number },
    -- Liquids the map declares (lakes): kind is "water" or "lava", center is
    -- at the liquid's level, radius is the farthest shore. `map` names a
    -- neighbor map in the scene: its liquids, in world coordinates.
    liquids: (map: string?) -> { { name: string, kind: string, level: number, center: vector, radius: number } },
    -- Moves a lava of the map (1-based index in liquids(), or its name) to
    -- `level` during play: the surface, liquid_at and walkable follow in the
    -- same frame. False for water or an unknown liquid. With `map`, a lava of
    -- that neighbor, `level` in world coordinates.
    set_liquid_level: (liquid: number | string, level: number, map: string?) -> boolean,
    -- Blocks walking on every ground point steeper than `degrees` (this run
    -- only; the saved walk grid is untouched). Returns how many were blocked.
    block_slopes: (degrees: number) -> number,
    -- Scene look: a preset from data/diorama.json (color grading, god rays)
    -- and the depth-of-field blur. False if there is no preset by that name.
    look: () -> string,
    -- `preset` may also be a TABLE with the same fields as a preset (only
    -- the given ones change): a game's own look without a preset in the engine.
    -- Fields include saturation, contrast, god_ray_*, vignette (strength,
    -- 0 = off), vignette_softness, shadow_tint / highlight_tint (a color as
    -- vector or {r, g, b}) with shadow_tint_strength / highlight_tint_strength.
    set_look: (preset: string | { [string]: any }) -> boolean,
    set_dof: (on: boolean) -> (),
    -- World time scale (hit-stop, slow motion): sprite animation, effects,
    -- weather and the scripts' dt run scaled (0 = frozen). `seconds` counts in
    -- full time and goes back to 1 by itself (0 or none = until the next call).
    -- Sound and the screen fade/flash keep going.
    set_time_scale: (scale: number, seconds: number?) -> (),
    time_scale: () -> number,
    -- Walk grid of the engine (block_slopes and solid objects included):
    -- whether a character of `radius` can stand at (x, z).
    walkable: (x: number, z: number, radius: number?) -> boolean,
    -- Cost of walking the grid cells within `radius` of (x, z) (0: only the
    -- cell of the point), this run only: 1 is normal, math.huge blocks.
    -- walkable, path and walking see it in the same frame; path goes around
    -- costly cells when that is cheaper. Returns how many cells changed.
    set_walk_cost: (x: number, z: number, radius: number, cost: number) -> number,
    -- Cost of the cell at (x, z) (1 when the game set none).
    walk_cost: (x: number, z: number) -> number,
    -- Path on the ground from `from` to `to` (smoothed), or nil if none.
    path: (from: vector, to: vector, radius: number?) -> { vector }?,
    -- Floor under (x, z) counting solid objects (bridges, walkways, kit
    -- slabs): the highest top below `fromY` (default: anywhere). Second value:
    -- true when it is an object, not the terrain.
    floor_height: (x: number, z: number, fromY: number?) -> (number, boolean),
    -- Whether a character of `radius` can stand on that floor: the walk grid
    -- on the terrain, the piece itself on an object, no other solid in the body.
    floor_walkable: (x: number, z: number, radius: number?, fromY: number?) -> boolean,
    -- Liquid showing at (x, z) (inside its shore and above the floor: a bridge
    -- over lava has none): {kind = "water" | "lava", level, index} or nil.
    -- index is the map's liquid (1-based), 0 for the global water plane.
    liquid_at: (x: number, z: number) -> { kind: string, level: number, index: number, map: string? }?,
    -- A whole nx * nz grid from (x0, z0) with spacing `step`, x varying
    -- fastest: heights (f32 per point) and flags (u8 per point: 1 walkable,
    -- 2 water, 4 lava, 8 on an object). Read with buffer.readf32(h, i * 4).
    ground_grid: (x0: number, z0: number, step: number, nx: number, nz: number, radius: number?) -> (buffer, buffer),
    -- The game's own fog: {color = vector or {r, g, b}, start, end, opacity,
    -- height, falloff} (the map .env "fog" block fields). Wins over the map's
    -- and the weather's fog until the next map change; nil gives control back.
    set_fog: (fog: { [string]: any }?) -> (),
    -- Motion blur and chromatic aberration come from the quality preset; a
    -- game can tune them as art direction. `amount`: strength (engine default
    -- 1 for the blur, 0.5 for the fringe).
    set_motion_blur: (on: boolean, amount: number?) -> (),
    set_chromatic_aberration: (on: boolean, amount: number?) -> (),
    -- Clear radius: inside a circle around the screen center (where the camera
    -- puts the subject) there is no motion blur or chromatic aberration; they
    -- grow smoothly outside it. Fractions of the screen HEIGHT; radius 0 puts
    -- the effects back on the whole screen.
    set_effect_radius: (radius: number, feather: number?) -> (),
    -- Where the lens focuses: a point of the scene, instead of the automatic
    -- \"how far did the player zoom in\" curve. `aperture` is blur per unit of
    -- depth (0.15 keeps a stage sharp and softens the background);
    -- `foreground` also blurs what sits IN FRONT of that point. `band` keeps
    -- that many world units on each side of the point fully sharp (a whole
    -- group, not a single plane); `max_blur` is the blur limit in pixels at
    -- full resolution (default 3; above it a cheap wide blur takes over). No
    -- point at all gives the lens back to the automatic focus.
    set_focus: (point: vector?, aperture: number?, foreground: boolean?, band: number?, max_blur: number?) -> (),
    -- Hides map and model geometry inside a volume (a mountain or a wall
    -- between the camera and the action) as a dither pattern. `slot` is 1..4;
    -- nil turns that slot off. Table: shape = "box" (size = vector, full
    -- width, height and depth) or "cylinder" (radius, height); center =
    -- vector; yaw in degrees; soft = fade width inward from the surface
    -- (default 15% of the smallest measure); opacity = what stays inside
    -- (default 0). Shadows are unaffected.
    set_cutaway: (slot: number, volume: { [string]: any }?) -> (),
    -- Models touching the ground take on the ground's own material (splat
    -- layers, color, normal) within `band` world units above it, so the
    -- contact line stops reading as a hard seam. Each model's band is at most
    -- `fraction` (default 0.3) of its own height, so a low stone keeps its
    -- look. 0 turns it off.
    set_ground_blend: (band: number, fraction: number?) -> (),
    -- Name of the map being played.
    map: () -> string,
    -- Goes to another map (a door, a cave); the player appears at `position`
    -- (default: the map's Player Start).
    load_map: (map: string, position: vector?) -> (),
    -- Same, but WITHOUT the wait: the next map is read in a thread and slots
    -- in by itself when ready (a few ms on the main thread). Call it when the
    -- player walks into the mouth of the passage, not when they reach it.
    warp_map: (map: string) -> (),
    warping: () -> boolean,
)" + ribbonApiDefinitions() + R"(}

-- Saved games: map, player position, vars, time, weather and music, kept in the
-- player's data folder. Slots are names or numbers (default 1).
declare game: {
    save: (slot: (string | number)?) -> boolean,
    load: (slot: (string | number)?) -> boolean,
    has_save: (slot: (string | number)?) -> boolean,
    delete_save: (slot: (string | number)?) -> boolean,
    -- Starts the game over: scripts from scratch (variables cleared, objects
    -- made by scripts removed) on the project's first map. Happens after the
    -- current frame.
    restart: () -> (),
    -- Player settings (volumes, speed...), kept apart from saved games.
    save_settings: (settings: { [string]: any }) -> boolean,
    -- The table last saved with save_settings, or an empty table.
    settings: () -> { [string]: any },
}

declare player: {
    position: () -> vector,
    set_position: (position: vector) -> (),
    -- Player look: a PNG, animated when the Sprite Importer's .spr sits next to it.
    set_sprite: (png: string) -> (),
    near: (model: Model, distance: number) -> boolean,
    -- True while the player stands in the trigger area.
    inside: (area: string) -> boolean,
    -- Speech bubble over the player's head for `seconds` (default 3).
    say: (text: string, seconds: number?) -> (),
    -- Extra animation of the player's sprite; once unless `loop`.
    play: (animation: string, loop: boolean?) -> boolean,
    animation: () -> string,
    -- Frozen: the player stops answering keyboard and mouse (the world keeps
    -- running). For dialogue, cutscenes and battle.
    freeze: (on: boolean) -> (),
    frozen: () -> boolean,
    -- Keeps running on without holding the run key (a phone's run button).
    set_run: (on: boolean) -> (),
    -- Hidden: the player's sprite is not drawn (a scene he is not in).
    hide: (on: boolean) -> (),
}

declare characters: {
    -- Reads and prepares sprite sheets off the frame, so a character added
    -- later with one of them only uploads it. One file or a list; still valid
    -- on the next map (call it with world.preload_map).
    preload: (sprites: string | { string }) -> (),
    -- Shadow of every character: "planar" (cast by the sun), "circle" (a
    -- contact disc) or "lights" (the disc plus a shadow cast by each of the
    -- two strongest nearby lights) or "map" (the sprite's silhouette in the sun's
    -- shadow map: it follows the relief, breaks on steps and climbs walls). No
    -- argument goes back to the engine default.
    set_shadow: (kind: string?) -> (),
    -- Puts a new character in the scene (sprite PNG or .spr of the project).
    -- False if the sprite could not be read.
    add: (name: string, sprite: string, position: vector, facing: number?) -> boolean,
    -- Characters placed in the scene (drag a PNG from the Project panel), by name.
    exists: (name: string) -> boolean,
    position: (name: string) -> vector,
    -- With `walk` true the engine animates walking from the displacement
    -- since the last frame (step rhythm from the measured speed); a jump of
    -- more than 2 body heights is a teleport. Facing is left to the script.
    set_position: (name: string, position: vector, walk: boolean?) -> (),
    -- Walks there around solid models and blocked ground, playing the walking
    -- animation; `seconds` sets the pace (default: normal walking speed).
    walk_to: (name: string, position: vector, seconds: number?) -> (),
    -- Speech bubble over the head for `seconds` (default 3).
    say: (name: string, text: string, seconds: number?) -> (),
    -- Extra animation of the sprite (Sprite Importer > Extra animations), e.g.
    -- "attack"; once unless `loop`. "" goes back to standing/walking.
    play: (name: string, animation: string, loop: boolean?) -> boolean,
    -- Extra animation playing now ("" when standing or walking).
    animation: (name: string) -> string,
    -- Follows the player, stopping `distance` away (default 30).
    follow: (name: string, distance: number?) -> (),
    -- Walks between the points in a loop, waiting `wait` seconds at each (default 1).
    patrol: (name: string, points: {vector}, wait: number?) -> (),
    -- Stops walking, following and patrolling.
    stop: (name: string) -> (),
    -- True while the walk animation plays: on a path (walk_to, follow,
    -- patrol) or moved by set_position(..., true).
    walking: (name: string) -> boolean,
    face: (name: string, degrees: number) -> (),
    show: (name: string) -> (),
    hide: (name: string) -> (),
    set_sprite: (name: string, png: string) -> (),
    -- Presentation (see `fx`): the sprite flashes a color, shakes in place,
    -- steps toward a point and back (a blow, not a walk), squashes and
    -- stretches, or changes opacity.
    flash: (name: string, color: vector?, seconds: number?) -> (),
    shake: (name: string, amplitude: number?, seconds: number?) -> (),
    lunge: (name: string, towards: vector, distance: number?, seconds: number?) -> (),
    pop: (name: string, amount: number?, seconds: number?) -> (),
    -- Size that stays (1 = the sheet's size); pop, shake and the rest act on top.
    set_scale: (name: string, scale: number) -> (),
    fade: (name: string, to: number, seconds: number?) -> (),
}

declare camera: {
    target: () -> vector,
    set_target: (target: vector) -> (),
    -- Yaw and pitch in degrees, distance in world units.
    orbit: () -> (number, number, number),
    set_orbit: (yaw: number, pitch: number, distance: number) -> (),
    -- Camera right and forward on the ground plane, to place things relative
    -- to the view (a row of fighters on the left, another on the right).
    basis: () -> (vector, vector),
    -- Free camera: stops following the player, so the script (or the mouse)
    -- decides where it looks. Same switch as the middle mouse button.
    free: (on: boolean) -> (),
    -- The following camera stops before the FRAME passes the map edge (the
    -- player leaves the center near the border, as in a 2D RPG). Same as
    -- "keep_inside_map" in the project's camera block.
    keep_inside_map: (on: boolean) -> (),
    -- What stands between the camera and the player (a wall, a cliff) is
    -- drawn as a dither pattern within `radius` world units of the player on
    -- screen, down to `opacity` (default 0.25) at the center. 0 turns it off.
    -- center "focus": around the point given to world.set_focus instead (a
    -- group under a bridge, with the leader off to the side); the player
    -- while no focus point is set.
    see_through: (radius: number, opacity: number?, center: ("player" | "focus")?) -> (),
    -- Map and model geometry closer than `start` world units to the camera
    -- disappears as a dither pattern and comes back fully by `end` (default
    -- 1.5 x start), so a fence brushing the lens no longer covers the screen.
    -- Shadows are unaffected. 0 turns it off.
    near_fade: (start: number, end_: number?) -> (),
    -- false: dragging with the left or right mouse button (a finger on the
    -- world, on a phone) no longer turns the player camera, so the game can
    -- run its own gesture from input.touches. The middle button still pans.
    -- Back to true when the scripts restart.
    set_orbit_input: (on: boolean) -> (),
}

-- Presentation of a hit: what makes a blow feel like it landed. The engine
-- knows nothing about combat; these are camera shake, screen flash and
-- numbers that float up. Colors are vectors, 0 to 1 per channel.
declare fx: {
    -- Camera shake: amplitude in world units, decaying over `seconds`. A new
    -- shake only wins if it hits harder than the one still running.
    shake_camera: (amplitude: number?, seconds: number?) -> (),
    -- Number (or word: "MISS") floating up from a world position. `rise` is a
    -- fraction of the screen height, `size` the text height as a fraction.
    number: (position: vector, text: string, color: vector?, seconds: number?, rise: number?, size: number?) -> (),
    -- Full-screen color going from one opacity to another: the battle wipe.
    fade: (color: vector, from: number, to: number, seconds: number?) -> (),
    -- Flash: full opacity (or `strength`) fading to nothing. Its own layer:
    -- it does not cancel a fade or a dim in progress.
    flash: (color: vector?, seconds: number?, strength: number?) -> (),
    -- Darkens only the SCENE (the interface stays on top, untouched): blends
    -- toward `color` by `strength` (0..1), animating from the current value.
    dim: (color: vector?, strength: number, seconds: number?) -> (),
    fading: () -> boolean,
}


declare input: {
    -- Key names as in on.key, or a controller button (any connected
    -- controller, Xbox layout): "pad_a", "pad_b", "pad_x", "pad_y",
    -- "pad_lb", "pad_rb", "pad_back", "pad_start", "pad_guide", "pad_ls",
    -- "pad_rs", "pad_up", "pad_right", "pad_down", "pad_left".
    down: (key: string) -> boolean,
    -- True only on the frame the key went down.
    pressed: (key: string) -> boolean,
    -- The screen is a touch screen (phone): hide drawn key hints.
    touch: () -> boolean,
    -- Shows or hides the on-screen touch stick (phone builds with a touch
    -- pad); show it only where it is used, such as free exploration.
    touch_pad: (on: boolean) -> (),
    -- Double tap on the camera area (phone) or double right click (PC), true
    -- for one frame. The engine does nothing with it; the game decides.
    double_tapped: () -> boolean,
    -- Name of the first connected controller, or nil: show button prompts
    -- instead of key prompts while it is there.
    gamepad: () -> string?,
    -- Mouse cursor in window pixels, with or without a button down (pan at
    -- the screen edge). On a phone it follows the finger that moves the camera.
    mouse_position: () -> (number, number),
    -- Mouse wheel movement this frame (0 when still; positive forward).
    scroll: () -> number,
    -- Fingers on the world this frame (not on the interface or the touch
    -- stick), in window pixels: {id, x, y}. Two fingers make a pan or a
    -- pinch; the game reads them and decides.
    touches: () -> { { id: number, x: number, y: number } },
}

-- Models placed in the map.
declare models: {
    count: () -> number,
    -- Reads .glb files off the frame (embedded textures decoded), so placing
    -- them later only uploads to the GPU. One file or a list; still valid on
    -- the next map (call it with world.preload_map).
    preload: (files: string | { string }) -> (),
    -- Index of the first model with this name, or nil.
    find: (name: string) -> number?,
    name: (model: Model) -> string,
    position: (model: Model) -> vector,
    set_position: (model: Model, position: vector) -> (),
    -- Enabled models within `radius` of `position` on the ground plane
    -- (height ignored), nearest first.
    near: (position: vector, radius: number) -> {Model},
    -- The file the model came from (relative to the game folder).
    file: (model: Model) -> string,
    -- Scale along X, Y and Z.
    scale: (model: Model) -> vector,
    -- Rotation in degrees around X, Y and Z.
    set_rotation: (model: Model, degrees: vector) -> (),
    set_scale: (model: Model, scale: number | vector) -> (),
    -- Smooth movement over `seconds` (0 or nil: instant).
    move_by: (model: Model, offset: vector, seconds: number?) -> (),
    rotate_by: (model: Model, degrees: number, seconds: number?) -> (),
    visible: (model: Model) -> boolean,
    set_visible: (model: Model, visible: boolean) -> (),
    show: (model: Model) -> (),
    hide: (model: Model) -> (),
    -- Solid models block the player (the bottom of the model: trunk, wall base).
    solid: (model: Model) -> boolean,
    set_solid: (model: Model, solid: boolean) -> (),
    -- Models with a skeleton (.glb with skin): plays the animation by name
    -- ("" = rest pose). loop defaults to true, speed to 1. The pose on screen
    -- blends into the new one over `fade` seconds (default 0.2; 0 = cut).
    -- False if it doesn't exist.
    play: (model: Model, animation: string, loop: boolean?, speed: number?, fade: number?) -> boolean,
    -- Animation playing now ("" = rest pose).
    animation: (model: Model) -> string,
    -- Names of the animations in the model file.
    animations: (model: Model) -> {string},
    -- Morph targets (blend shapes) of the model, by name.
    morphs: (model: Model) -> {string},
    -- Holds a morph target (name or number from 1) at `weight` (0 to 1, more
    -- exaggerates); nil gives it back to the animation. False if it doesn't exist.
    set_morph: (model: Model, target: string | number, weight: number?) -> boolean,
}

-- Sound: wav, ogg, mp3 or flac files of the project. Functions that start a
-- sound return a number for audio.stop / audio.playing.
declare audio: {
    -- `options`: a volume number, or {volume, pitch, range, min, falloff, group, loop}.
    -- pitch: 1 = as recorded, 2 = an octave up. group: a bus with its own volume.
    play: (file: string, options: (number | { [string]: any })?, pitch: number?) -> number,
    -- At a point or model: quieter far away (`range` units, default 300), panned to its side.
    -- `min`: full volume up to this distance; `falloff`: "linear", "inverse" or "exponential".
    play_at: (file: string, place: vector | Model, options: (number | { [string]: any })?, range: number?, pitch: number?) -> number,
    loop: (file: string, options: (number | { [string]: any })?, pitch: number?) -> number,
    loop_at: (file: string, place: vector | Model, options: (number | { [string]: any })?, range: number?, pitch: number?) -> number,
    stop: (sound: number, fade_seconds: number?) -> (),
    playing: (sound: number) -> boolean,
    -- Decodes a file in the background so its first play starts at once.
    preload: (file: string) -> boolean,
    -- One music at a time; a new one cross-fades (default 1 s). With a second
    -- file, the first is an intro and the second loops after it with no gap.
    -- As a table: {volume, fade, loop = file, resume = true} (resume: from where
    -- this track last stopped).
    music: (file: string, options: (number | string | { [string]: any })?, fade_seconds: number?) -> (),
    stop_music: (fade_seconds: number?) -> (),
    -- Volume of the music that is playing, without restarting it.
    music_volume: (volume: number, fade_seconds: number?) -> (),
    -- set_volume(volume): master volume, 1 = normal. set_volume(sound, volume,
    -- fade_seconds?): a sound that is already playing.
    set_volume: (sound_or_volume: number, volume: number?, fade_seconds: number?) -> (),
    set_pitch: (sound: number, pitch: number) -> (),
    -- Volume of a group (bus); "music" is the group of the music.
    group_volume: (group: string, volume: number, fade_seconds: number?) -> (),
}

-- Point lights of the map. Indices start at 1.
-- The optional last argument `map` names a neighbor map in the scene
-- (scene `neighbors`): its lights, numbered as in its own scene. count(map)
-- is 0 while that neighbor is not in the scene.
declare lights: {
    count: (map: string?) -> number,
    position: (index: number, map: string?) -> vector,
    set_position: (index: number, position: vector, map: string?) -> (),
    -- Linear RGB, 0 to 1 per channel.
    color: (index: number, map: string?) -> vector,
    set_color: (index: number, color: vector, map: string?) -> (),
    intensity: (index: number, map: string?) -> number,
    set_intensity: (index: number, intensity: number, map: string?) -> (),
    enabled: (index: number, map: string?) -> boolean,
    set_enabled: (index: number, enabled: boolean, map: string?) -> (),
    turn_on: (index: number, map: string?) -> (),
    turn_off: (index: number, map: string?) -> (),
    -- Whether this light casts sprite shadows (characters.set_shadow("lights")).
    -- Turn it off on a light the game moves along with the party.
    set_sprite_shadow: (index: number, on: boolean, map: string?) -> (),
}

-- Machine readings for a performance meter. A field is nil where this
-- machine does not report it. Temperatures in degrees Celsius.
type PerfSystem = {
    ram_mb: number?,            -- memory of this process
    vram_mb: number?,           -- GPU memory allocated by the engine
    cpu_percent: number?,       -- this process, of the whole machine
    gpu_percent: number?,       -- the whole GPU
    cpu_temp: number?,
    gpu_temp: number?,
    gpu_watts: number?,
    battery: number?,           -- charge, 0 to 100
    battery_temp: number?,
    battery_watts: number?,     -- power drawn from (or into) the battery
    charging: boolean?,
    -- Phones: "none", "light", "moderate", "severe", "critical",
    -- "emergency", "shutdown" (the system throttles from "moderate" on).
    thermal: string?,
    -- Phones: 1.0 = the point where the system starts to throttle.
    thermal_headroom: number?,
}

-- Performance counters for an on-screen meter: the numbers the engine already
-- measures for its debug panel and telemetry.
declare perf: {
    -- Frames per second over the last half second.
    fps: () -> number,
    -- The last frame, in ms: wall time (for graphs and the 1% low), CPU work
    -- (simulation, scripts, recording; no waiting for the GPU, vsync or the
    -- FPS cap) and GPU time, nil where the GPU has no timers.
    frame: () -> (number, number, number?),
    -- Scene width and height, scale (scene / screen), then screen width and
    -- height, in pixels.
    resolution: () -> (number, number, number, number, number),
    -- Frame limiter in frames per second, 0 = none. The graphics preset sets
    -- it (30 on phones); a change lasts until the preset is applied again.
    fps_cap: () -> number,
    set_fps_cap: (fps: number) -> (),
    -- Temperatures, battery and memory, read at most once a second.
    system: () -> PerfSystem,
}
)" + std::string(vfxApiDefinitions()) + objectsApiDefinitions() + NativeModule::apiDefinitions();
}

} // namespace eruption
