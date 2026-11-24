// eulerAngleYXZ vem das extensoes "gtx" do GLM.
#define GLM_ENABLE_EXPERIMENTAL
#include <cfloat>
#include "script/ScriptApi.hpp"
#include "script/ApiFilter.hpp"
#include "core/Platform.hpp"
#include "utils/AssetVfs.hpp"

#include "script/ScriptHost.hpp"
#include "script/NativeModule.hpp"
#include "core/Engine.hpp"
#include "core/NavHeight.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "game/PlayerController.hpp"
#include "renderer/BandwidthMeter.hpp"
#include "renderer/ImmediateDraw.hpp"
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
#include <fstream>
#include <iterator>
#include <cstring>
#include <ctime>
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
    static Window& window() { return engine->m_window; }
    static void screenshot(const std::string& path, bool ui) { engine->requestScriptScreenshot(path, ui); }
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
    static TextureLayers::Stats layerStats() { return engine->m_bindless.layerStats(); }
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
// Quadro ortografico do minimapa (cx, cz, zoom): centro em mundo e meio-lado
// do enquadramento que o motor calcula internamente a partir das AABBs reais
// de terreno/instancia (Engine::m_minimapFrame, ver Minimap.cpp). O jogo tinha
// uma formula propria assumindo a AABB NOMINAL do mapa; isto devolve a AABB
// REAL usada no bake, que pode diferir quando um mapa carrega so' parcialmente
// ou tem geometria fora da grade nominal. zoom <= 0 antes do primeiro bake.
int ui_minimap_frame(lua_State* L) {
    const Vec3 f = eng().minimapFrame();
    lua_pushnumber(L, f.x);
    lua_pushnumber(L, f.y);
    lua_pushnumber(L, f.z);
    return 3;
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
// on.step / characters.on_step(fn(name, position, ground)): cada passo de
// personagem (ver ScriptHost::dispatch).
int on_step(lua_State* L) { return addEvent(L, ScriptHost::EventKind::Step, 1, 0.0, nullptr); }
int characters_on_step(lua_State* L) { return on_step(L); }

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
    const WeatherType t = weatherTypeFromName(luaL_checkstring(L, 1));
    // Com o modo classico ligado o clima nao muda (o classico e' sem efeitos):
    // ele fica guardado e entra quando o HD voltar. Antes o script do mapa
    // trocava para neve/chuva em cima do classico ao carregar o mapa.
    if (eng().classicMode()) {
        eng().setClassicSavedWeather(t);
        return 0;
    }
    ScriptBridge::setWeather(t);
    return 0;
}
// world.set_wetness(target?, seconds?): fixa o chao molhado (0..1), andando
// ate' ele em `seconds` de 0 a 1; nil volta ao automatico (chuva e clima).
int world_set_wetness(lua_State* L) {
    const float target = lua_isnoneornil(L, 1) ? -1.0f : std::clamp(float(luaL_checknumber(L, 1)), 0.0f, 1.0f);
    eng().setWetness(target, float(luaL_optnumber(L, 2, 5.0)));
    return 0;
}
int world_wetness(lua_State* L) {
    lua_pushnumber(L, eng().wetness());
    return 1;
}
// world.wet_area(pos, radius, amount?, dry_seconds?): molha um disco do chao.
int world_wet_area(lua_State* L) {
    const Vec3 p = checkVec3(L, 1);
    const float radius = float(luaL_checknumber(L, 2));
    const float amount = std::clamp(float(luaL_optnumber(L, 3, 1.0)), 0.0f, 1.0f);
    const float dry = std::clamp(float(luaL_optnumber(L, 4, 30.0)), 0.1f, 3600.0f);
    eng().wetArea(p, radius, amount, dry);
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
// Le' o mapa numa thread ANTES do warp (Engine::preloadMap): o jogo pede a
// proxima area enquanto o jogador ainda esta' longe da passagem, e o
// warp_map do mesmo mapa so' faz o envio para a GPU. Falso se o mapa nao
// existe.
int world_preload_map(lua_State* L) {
    ScriptHost::NativeWork trabalho; // pedido ao motor, nao laco do script
    lua_pushboolean(L, eng().preloadMap(luaL_checkstring(L, 1)));
    return 1;
}
int world_set_load_distance(lua_State* L) {
    // A thread do vizinho ja' e' assincrona; o que o jogo ganha e' comecar mais cedo.
    eng().worldLoadDistance = std::clamp(static_cast<float>(luaL_checknumber(L, 1)), 50.0f, 3000.0f);
    return 0;
}
int world_load_distance(lua_State* L) {
    lua_pushnumber(L, eng().worldLoadDistance);
    return 1;
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

// Altura (Y) do caminho andavel pelas alturas de canto da grade de navegacao
// do mapa em (x, z); nil sem alturas (.nav antigo) ou fora da grade.
int world_nav_height(lua_State* L) {
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float z = static_cast<float>(luaL_checknumber(L, 2));
    float y = 0.0f;
    const LoadedMap* map = eng().mapAt(x, z);
    if (map && navHeightAt(*map, x, z, y)) lua_pushnumber(L, y);
    else lua_pushnil(L);
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
int characters_preload_ready(lua_State* L) {
    lua_pushboolean(L, eng().characterSpritePreloaded(luaL_checkstring(L, 1)));
    return 1;
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
    lua_pushboolean(L, eng().playCharacterAction(idx, luaL_optstring(L, 2, ""), lua_toboolean(L, 3) != 0,
                                                 static_cast<float>(luaL_optnumber(L, 4, 0.0))));
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
// Anel no chao preso ao personagem; nil tira.
int characters_ring(lua_State* L) {
    Engine::SceneCharacter* c = checkCharacter(L, 1);
    if (lua_isnoneornil(L, 2)) {
        c->ringRadius = 0.0f;
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    auto campo = [L](const char* k, float def) {
        lua_getfield(L, 2, k);
        const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
        lua_pop(L, 1);
        return v;
    };
    const float raio = campo("radius", 0.0f);
    if (raio <= 0.0f) luaL_error(L, "characters.ring: radius must be > 0");
    lua_getfield(L, 2, "color");
    Vec3 cor(1.0f);
    if (lua_isvector(L, -1)) {
        const float* v = lua_tovector(L, -1);
        cor = Vec3(v[0], v[1], v[2]);
    }
    lua_pop(L, 1);
    c->ringRadius = raio;
    c->ringWidth = std::clamp(campo("width", std::max(1.0f, raio * 0.15f)), 0.1f, raio);
    c->ringColor = Vec4(cor, std::clamp(campo("alpha", 0.8f), 0.0f, 1.0f));
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

// Caixa de texto aberta (console do Enter, campo do editor): as letras sao do
// texto, nao do jogo. Sem isto WASD andava a camera e H/1/2 mexiam no visor
// enquanto se digitava. O controle (pad_*) segue valendo.
// "0".."9" sao a fileira de cima e o teclado numerico: os menus numerados
// do jogo esperam as duas (GLFW da' codigos diferentes, 48.. e 320..).
bool scriptKey(Key k, bool pressed) {
    const auto test = [pressed](Key key) { return pressed ? Input::isKeyPressed(key) : Input::isKeyDown(key); };
    if (test(k)) return true;
    const int c = static_cast<int>(k);
    return c >= '0' && c <= '9' && test(static_cast<Key>(320 + c - '0'));
}
bool typingText() {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput;
}

// "mouse_left" / "mouse_right" / "mouse_middle": -1 se nao for botao do mouse.
int mouseButtonFromName(const char* n) {
    const std::string s = n;
    return s == "mouse_left" ? 0 : (s == "mouse_right" ? 1 : (s == "mouse_middle" ? 2 : -1));
}
// Botao do mouse para o jogo: um aperto que comecou num botao da interface
// (CSS) e' da interface, nao do mundo.
bool mouseForGame(int b, bool onlyPressed) {
    const auto btn = static_cast<MouseButton>(b);
    if (Input::pressOnCss()) return false;
    return onlyPressed ? Input::isMousePressed(btn) : Input::isMouseDown(btn);
}

// Entrada de texto: text_begin liga (e abre o teclado do celular), text_end
// desliga, text() devolve o UTF-8 digitado desde o quadro anterior (acento e
// layout vem do sistema; so' caracteres imprimiveis). As teclas CONTINUAM
// reportadas em input.down/pressed (o jogo decide ignorar as de acao com
// text_active()); apagar e Enter ficam por conta de input.pressed.
int input_text_begin(lua_State*) {
    Input::textBegin();
    return 0;
}
int input_text_end(lua_State*) {
    Input::textEnd();
    return 0;
}
int input_text_active(lua_State* L) {
    lua_pushboolean(L, Input::textActive());
    return 1;
}
int input_text(lua_State* L) {
    const std::string& t = Input::text();
    lua_pushlstring(L, t.data(), t.size());
    return 1;
}

int input_down(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (const int mb = mouseButtonFromName(name); mb >= 0) {
        lua_pushboolean(L, mouseForGame(mb, false));
        return 1;
    }
    const int b = padButtonFromName(name);
    if (b >= 0) {
        lua_pushboolean(L, padButton(b, false));
        return 1;
    }
    const Key k = scriptKeyFromName(name);
    lua_pushboolean(L, k != Key::Unknown && !typingText() && scriptKey(k, false));
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
// ---------------------------------------------------------------- io (so' leitura, so' no editor)

// Caminho permitido a um script de editor: a pasta do projeto e as pastas de
// "editor_read_paths" do manifesto. Relativo = a partir da pasta do projeto.
// Resolve ".." e links antes de comparar, para nao escapar da lista.
bool editorReadAllowed(const std::string& raw, std::filesystem::path& resolved) {
    namespace fs = std::filesystem;
    if (!project::isOpen() || raw.empty()) return false;
    std::error_code ec;
    auto canon = [&](const fs::path& p) {
        std::error_code e;
        fs::path r = fs::weakly_canonical(p.is_absolute() ? p : project::root() / p, e);
        return e ? fs::path() : r;
    };
    resolved = canon(fs::path(raw));
    if (resolved.empty()) return false;
    auto inside = [&](const fs::path& base) {
        const fs::path b = canon(base);
        if (b.empty()) return false;
        auto bi = b.begin();
        auto ri = resolved.begin();
        for (; bi != b.end(); ++bi, ++ri)
            if (ri == resolved.end() || *bi != *ri) return false;
        return true;
    };
    if (inside(project::root())) return true;
    for (const std::string& extra : Project::instance().manifest().editorReadPaths)
        if (inside(fs::path(extra))) return true;
    return false;
}

// io.read_text(path) -> (text) | (nil, reason)
int io_read_text(lua_State* L) {
    const std::string raw = luaL_checkstring(L, 1);
    auto fail = [&](const char* why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    };
    if (!ScriptHost::editorPreviewActive()) return fail("io.read_text only works in editor scripts");
    std::filesystem::path p;
    if (!editorReadAllowed(raw, p)) return fail("path not allowed (project folder and editor_read_paths only)");
    ScriptHost::NativeWork trabalho;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return fail("file not found");
    constexpr std::uintmax_t kMaxBytes = 16u * 1024u * 1024u;
    if (std::filesystem::file_size(p, ec) > kMaxBytes) return fail("file too large (16 MB limit)");
    std::ifstream f(p, std::ios::binary);
    if (!f) return fail("could not open file");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// io.list_dir(path) -> ({names}) | (nil, reason): nomes das entradas, em ordem.
int io_list_dir(lua_State* L) {
    const std::string raw = luaL_checkstring(L, 1);
    auto fail = [&](const char* why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    };
    if (!ScriptHost::editorPreviewActive()) return fail("io.list_dir only works in editor scripts");
    std::filesystem::path p;
    if (!editorReadAllowed(raw, p)) return fail("path not allowed (project folder and editor_read_paths only)");
    ScriptHost::NativeWork trabalho;
    std::error_code ec;
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
        names.push_back(it->path().filename().string());
    if (ec) return fail("could not list directory");
    std::sort(names.begin(), names.end());
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushlstring(L, names[i].data(), names[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// Cursor do mouse do jogo: set_cursor(caminho, hotspot_x, hotspot_y, opcoes) ou
// set_cursor(nil). Cache por caminho em core/CustomCursor.cpp: chamar todo
// quadro com os mesmos argumentos custa uma comparacao de string.
int input_set_cursor(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        Input::setCustomCursor("", 0, 0, 1.0f, 0, 0, 1, 8.0f);
        return 0;
    }
    const char* path = luaL_checkstring(L, 1);
    const int hx = int(luaL_optnumber(L, 2, 0));
    const int hy = int(luaL_optnumber(L, 3, 0));
    int fw = 0, fh = 0, frames = 1;
    float fps = 8.0f, scale = 1.0f;
    bool software = false;
    if (lua_istable(L, 4)) {
        auto num = [L](const char* k, double def) {
            lua_getfield(L, 4, k);
            const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
            lua_pop(L, 1);
            return v;
        };
        fw = int(num("frame_w", 0));
        fh = int(num("frame_h", 0));
        frames = int(num("frames", 1));
        fps = float(num("fps", 8.0));
        scale = float(num("scale", 1.0));
        lua_getfield(L, 4, "software");
        software = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    Input::setCustomCursor(path, hx, hy, scale, fw, fh, frames, fps, software);
    return 0;
}
// ---------------------------------------------------------------------------
// Desenho imediato (world.draw_strip, world.draw_sprite): a lista e' do quadro,
// o passe de efeitos (renderer/ImmediateDraw) a esvazia ao desenhar. Chamar todo
// quadro; parar de chamar apaga o efeito.
// ---------------------------------------------------------------------------
namespace {
ImmediateDraw::Blend drawBlend(lua_State* L, const char* name) {
    if (std::strcmp(name, "additive") == 0) return ImmediateDraw::Blend::Additive;
    if (std::strcmp(name, "alpha") == 0) return ImmediateDraw::Blend::Alpha;
    luaL_error(L, "blend must be \"additive\" or \"alpha\" (got \"%s\")", name);
    return ImmediateDraw::Blend::Additive;
}
bool tableBool(lua_State* L, int t, const char* k, bool def) {
    lua_getfield(L, t, k);
    const bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}
// Le uma cor RGBA: tabela {r,g,b,a} (campos ou posicoes 1..4), vetor (alfa 1) ou nil (branco).
Vec4 drawColor(lua_State* L, int idx) {
    Vec4 c(1.0f);
    if (lua_isvector(L, idx)) {
        const float* v = lua_tovector(L, idx);
        return Vec4(v[0], v[1], v[2], 1.0f);
    }
    if (!lua_istable(L, idx)) return c;
    static const char* const names[4] = {"r", "g", "b", "a"};
    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, idx, names[i]);
        }
        if (lua_isnumber(L, -1)) c[i] = float(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return c;
}
} // namespace

// world.draw_strip(texture, verts, opts?)
int world_draw_strip(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    ImmediateDraw::Flags flags;
    ImmediateDraw::Topology topo = ImmediateDraw::Topology::Strip;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "blend");
        if (lua_isstring(L, -1)) flags.blend = drawBlend(L, lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 3, "topology");
        if (lua_isstring(L, -1)) {
            const char* t = lua_tostring(L, -1);
            if (std::strcmp(t, "quads") == 0) topo = ImmediateDraw::Topology::Quads;
            else if (std::strcmp(t, "strip") != 0) luaL_error(L, "topology must be \"strip\" or \"quads\" (got \"%s\")", t);
        }
        lua_pop(L, 1);
        flags.depthTest = tableBool(L, 3, "depth_test", true);
        flags.cull = tableBool(L, 3, "cull", false);
        // O passe le a profundidade da cena em somente-leitura: depth_write nao existe.
        static bool warned = false;
        if (!warned && tableBool(L, 3, "depth_write", false)) {
            warned = true;
            ERUPTION_LOG_WARN("world.draw_strip: depth_write = true nao e' suportado (efeitos nao escrevem profundidade); ignorado");
        }
    }
    ImmediateDraw& dr = eng().immediateDraw();
    if (!dr.ready()) return 0;
    const int n = int(lua_objlen(L, 2));
    if (n <= 0) return 0;
    const int tex = dr.textureId(path);
    if (tex < 0) return 0;

    // Chaves internadas uma vez: rawget por chave empilhada evita o hash da string a cada campo.
    static const char* const keyNames[7] = {"pos", "u", "v", "r", "g", "b", "a"};
    luaL_checkstack(L, 12, "draw_strip");
    const int k0 = lua_gettop(L) + 1;
    for (const char* k : keyNames) lua_pushstring(L, k);
    static thread_local std::vector<ImmediateDraw::Vertex> scratch;
    scratch.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        if (!lua_istable(L, -1)) luaL_error(L, "verts[%d] must be a table {pos, u, v, r, g, b, a}", i + 1);
        const int vi = lua_gettop(L);
        ImmediateDraw::Vertex& out = scratch[size_t(i)];
        lua_pushvalue(L, k0);
        lua_rawget(L, vi);
        if (!lua_isvector(L, -1)) luaL_error(L, "verts[%d].pos must be a vector", i + 1);
        const float* p = lua_tovector(L, -1);
        out.x = p[0]; out.y = p[1]; out.z = p[2];
        lua_pop(L, 1);
        float* dst[6] = {&out.u, &out.v, &out.r, &out.g, &out.b, &out.a};
        for (int f = 0; f < 6; ++f) {
            lua_pushvalue(L, k0 + 1 + f);
            lua_rawget(L, vi);
            *dst[f] = lua_isnumber(L, -1) ? float(lua_tonumber(L, -1)) : (f < 2 ? 0.0f : 1.0f);
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // verts[i]
    }
    lua_pop(L, 7); // chaves
    if (ImmediateDraw::Vertex* w = dr.addGeometry(tex, topo, flags, uint32_t(n)))
        std::memcpy(w, scratch.data(), size_t(topo == ImmediateDraw::Topology::Quads ? (uint32_t(n) & ~3u) : uint32_t(n)) * sizeof(ImmediateDraw::Vertex));
    return 0;
}

// world.draw_sprite(texture, pos, size, rgba?, blend?, opts?)
int world_draw_sprite(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const Vec3 pos = checkVec3(L, 2);
    float w = 1.0f, h = 1.0f;
    if (lua_isvector(L, 3)) {
        const float* v = lua_tovector(L, 3);
        w = v[0];
        h = v[1];
    } else {
        w = h = float(luaL_checknumber(L, 3));
    }
    const Vec4 rgba = drawColor(L, 4);
    ImmediateDraw::Flags flags;
    if (lua_isstring(L, 5)) flags.blend = drawBlend(L, lua_tostring(L, 5));
    bool pixels = false;
    float rot = 0.0f;
    if (lua_istable(L, 6)) {
        pixels = tableBool(L, 6, "pixels", false);
        flags.depthTest = tableBool(L, 6, "depth_test", true);
        lua_getfield(L, 6, "rotation");
        if (lua_isnumber(L, -1)) rot = float(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    ImmediateDraw& dr = eng().immediateDraw();
    if (!dr.ready()) return 0;
    const int tex = dr.textureId(path);
    if (tex >= 0) dr.addSprite(tex, pos, w, h, rgba, flags, pixels, rot);
    return 0;
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
    if (const int mb = mouseButtonFromName(name); mb >= 0) {
        lua_pushboolean(L, mouseForGame(mb, true));
        return 1;
    }
    const int b = padButtonFromName(name);
    if (b >= 0) {
        lua_pushboolean(L, padButton(b, true));
        return 1;
    }
    const Key k = scriptKeyFromName(name);
    lua_pushboolean(L, k != Key::Unknown && !typingText() && scriptKey(k, true));
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
int models_set_solid_file(lua_State* L) {
    eng().setSolidFile(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}
int models_solid(lua_State* L) {
    lua_pushboolean(L, eng().isSolid(static_cast<uint32_t>(checkModel(L, 1))));
    return 1;
}
Engine::SurfaceKind checkSurface(lua_State* L, int arg) {
    Engine::SurfaceKind kind = Engine::kSurfaceNone;
    if (!Engine::parseSurface(luaL_checkstring(L, arg), kind))
        luaL_argerror(L, arg, "\"none\", \"floor\", \"hazard\", \"blocked\" or \"wall\"");
    return kind;
}
int models_surface(lua_State* L) {
    lua_pushstring(L, Engine::surfaceName(eng().surfaceOf(static_cast<uint32_t>(checkModel(L, 1)))));
    return 1;
}
int models_set_surface(lua_State* L) {
    eng().setSurface(static_cast<uint32_t>(checkModel(L, 1)), checkSurface(L, 2));
    return 0;
}
int models_set_surface_file(lua_State* L) {
    eng().setSurfaceFile(luaL_checkstring(L, 1), checkSurface(L, 2));
    return 0;
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
// game.screenshot(path?, {ui = true}) -> caminho. Atendida no fim do quadro;
// sem caminho grava em <pasta de dados>/screenshots/AAAA-MM-DD_HH-MM-SS.png.
int game_screenshot(lua_State* L) {
    bool ui = true;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "ui");
        if (!lua_isnil(L, -1)) ui = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    std::error_code ec;
    std::filesystem::path path;
    if (lua_isstring(L, 1) && *lua_tostring(L, 1)) {
        path = lua_tostring(L, 1);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    } else {
        // A pasta de dados do jogo e' a dos jogos salvos, um nivel acima.
        const std::filesystem::path dir = ScriptHost::saveDir().parent_path() / "screenshots";
        std::filesystem::create_directories(dir, ec);
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
        path = dir / (std::string(stamp) + ".png");
        // Duas capturas no mesmo segundo nao se sobrescrevem.
        for (int n = 2; std::filesystem::exists(path, ec) && n < 1000; ++n)
            path = dir / (std::string(stamp) + "_" + std::to_string(n) + ".png");
    }
    ScriptBridge::screenshot(path.string(), ui);
    lua_pushstring(L, path.string().c_str());
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

// ---------------------------------------------------------------- window

// Tela cheia em tempo de execucao (a mesma troca do Alt+Enter).
int window_set_fullscreen(lua_State* L) {
    ScriptBridge::window().setFullscreen(lua_toboolean(L, 1) != 0);
    return 0;
}
int window_is_fullscreen(lua_State* L) {
    lua_pushboolean(L, ScriptBridge::window().isFullscreen());
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
int characters_set_sink(lua_State* L) {
    eng().setSpriteSink(static_cast<float>(luaL_optnumber(L, 1, 0.0)));
    return 0;
}
// Luz de borda do lado da luz (Engine::setSpriteRim): forca e {color}.
int characters_set_rim(lua_State* L) {
    Vec3 tint(1.0f);
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "color");
        if (lua_isvector(L, -1)) {
            const float* v = lua_tovector(L, -1);
            tint = Vec3(v[0], v[1], v[2]);
        }
        lua_pop(L, 1);
    }
    eng().setSpriteRim(static_cast<float>(luaL_optnumber(L, 1, 0.0)), tint);
    return 0;
}
// Animacao em degraus (Engine::setSpriteStepFps): fps e, opcional, o nome
// de um personagem ou o arquivo de uma folha.
int characters_set_anim_fps(lua_State* L) {
    eng().setSpriteStepFps(luaL_optstring(L, 2, ""), static_cast<float>(luaL_optnumber(L, 1, 0.0)));
    return 0;
}
// Passada (Engine::setWalkStride): unidades por ciclo de andar e, opcional,
// o nome de um personagem; nil como nome = padrao de todos.
int characters_set_stride(lua_State* L) {
    const char* name = lua_isnoneornil(L, 1) ? "" : luaL_checkstring(L, 1);
    eng().setWalkStride(name, static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}
// Ritmo de andar (Engine::setWalkRate): unidades por celula, referencia em ms
// (150 por padrao) e, opcional, o nome; nil = padrao de todos.
int characters_set_walk_rate(lua_State* L) {
    const char* name = lua_isnoneornil(L, 1) ? "" : luaL_checkstring(L, 1);
    eng().setWalkRate(name, static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_optnumber(L, 3, 150.0)));
    return 0;
}
int characters_set_shadow(lua_State* L) {
    const char* k = luaL_optstring(L, 1, "");
    const int kind = std::strcmp(k, "planar") == 0 ? 0 : std::strcmp(k, "circle") == 0 ? 1
                   : std::strcmp(k, "lights") == 0 ? 2 : std::strcmp(k, "map") == 0 ? 3
                   : std::strcmp(k, "map_contact") == 0 ? 4 : -1;
    // {contact_sun = 0..1}: "map_contact" com o disco tambem ao sol.
    float contactSun = 0.0f;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "contact_sun");
        if (lua_isnumber(L, -1)) contactSun = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    eng().setSpriteShadow(kind, contactSun);
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
// Topo do sprite na tela: x, y em pixels da interface e se esta' visivel.
int characters_head_screen(lua_State* L) {
    const int idx = eng().findCharacter(luaL_checkstring(L, 1));
    if (idx < 0) luaL_error(L, "no character named '%s' in this map", lua_tostring(L, 1));
    Vec2 p(0.0f);
    const bool on = eng().characterHeadScreen(idx, p);
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    lua_pushboolean(L, on);
    return 3;
}
// Personagem sob o pixel (espaco de input.mouse_position), o mais perto da camera.
int characters_at_screen(lua_State* L) {
    const int idx = eng().characterAtScreen(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)));
    if (idx < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, eng().characters()[static_cast<size_t>(idx)].name.c_str());
    return 1;
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
    if (!vfs::exists(p)) p = project::engine(rel); // disco ou pacote .ecp
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
// Mouse sobre o retangulo desenhado de algum elemento que casa (#id, .classe,
// tag ou composto), como ui.clicked mede o toque.
int ui_hovered(lua_State* L) {
    lua_pushboolean(L, eng().uiHovered(luaL_checkstring(L, 1)));
    return 1;
}
// Liga ou desliga a classe `name` nos elementos que casam: o .css decide o que
// ela muda (`#id.name`, `.linha.name`).
int ui_state(lua_State* L) {
    eng().uiDocument().setState(luaL_checkstring(L, 1), luaL_checkstring(L, 2), lua_toboolean(L, 3) != 0);
    return 0;
}
int ui_has(lua_State* L) {
    lua_pushboolean(L, eng().uiDocument().has(luaL_checkstring(L, 1)));
    return 1;
}
// Prende o centro do elemento ao topo do sprite de um personagem, `dy` pixels
// acima; nil solta (volta ao CSS).
int ui_anchor(lua_State* L) {
    const char* sel = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) {
        eng().clearUiAnchor(sel);
        return 0;
    }
    eng().setUiAnchor(sel, luaL_checkstring(L, 2), static_cast<float>(luaL_optnumber(L, 3, 0.0)));
    return 0;
}
// Retrato de um personagem dentro do elemento; nil solta.
int ui_portrait(lua_State* L) {
    const char* sel = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) {
        eng().clearUiPortrait(sel);
        return 0;
    }
    const char* nome = luaL_checkstring(L, 2);
    Vec4 crop(0.0f, 0.0f, 1.0f, 1.0f);
    if (lua_istable(L, 3)) {
        for (int i = 0; i < 4; ++i) {
            lua_rawgeti(L, 3, i + 1);
            if (lua_isnumber(L, -1)) crop[i] = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        if (crop.z <= 0.0f || crop.w <= 0.0f) luaL_error(L, "ui.portrait: crop width and height must be > 0");
    }
    eng().setUiPortrait(sel, nome, crop);
    return 0;
}
// Selo sobre um elemento: texto ("+", "3") ou um PNG do projeto; nil tira.
int ui_badge(lua_State* L) {
    const char* sel = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) {
        eng().uiDocument().clearBadge(sel);
        return 0;
    }
    const std::string v = luaL_checkstring(L, 2);
    const bool png = v.size() > 4 && v.compare(v.size() - 4, 4, ".png") == 0;
    eng().uiDocument().setBadge(sel, png ? std::string() : v, png ? v : std::string());
    return 0;
}
// Grafo de nos: lista de { id, x?, y?, icon?, text?, color?, links? }; nil tira.
static std::string fieldString(lua_State* L, int t, const char* k) {
    lua_getfield(L, t, k);
    std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s;
}
static UiDocument::GraphNode readGraphNode(lua_State* L, int t) {
    UiDocument::GraphNode g;
    g.id = fieldString(L, t, "id");
    g.text = fieldString(L, t, "text");
    g.image = fieldString(L, t, "icon");
    lua_getfield(L, t, "x");
    lua_getfield(L, t, "y");
    if (lua_isnumber(L, -2) && lua_isnumber(L, -1))
        g.pos = Vec2(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
    lua_pop(L, 2);
    lua_getfield(L, t, "color");
    if (const float* v = lua_tovector(L, -1)) {
        g.hasColor = true;
        g.color = Vec4(v[0], v[1], v[2], 1.0f);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "links");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1)) g.links.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return g;
}
int ui_graph(lua_State* L) {
    const char* sel = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) {
        eng().uiDocument().clearGraph(sel);
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<UiDocument::GraphNode> nodes;
    const int n = lua_objlen(L, 2);
    nodes.reserve(static_cast<size_t>(n));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        if (lua_istable(L, -1)) nodes.push_back(readGraphNode(L, lua_gettop(L)));
        lua_pop(L, 1);
    }
    eng().uiDocument().setGraph(sel, std::move(nodes));
    return 0;
}
int ui_graph_clicked(lua_State* L) {
    const std::string id = eng().uiGraphClicked(luaL_checkstring(L, 1));
    if (id.empty()) lua_pushnil(L);
    else lua_pushlstring(L, id.data(), id.size());
    return 1;
}
int ui_graph_held(lua_State* L) {
    const float seconds = static_cast<float>(luaL_optnumber(L, 2, 0.5));
    const std::string id = eng().uiGraphHeld(luaL_checkstring(L, 1), seconds);
    if (id.empty()) lua_pushnil(L);
    else lua_pushlstring(L, id.data(), id.size());
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


// Particulas de ambiente (AmbientParticles): lista de camadas ou nil.
float ambientField(lua_State* L, int t, const char* k, float def) {
    lua_getfield(L, t, k);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}
int world_set_ambient(lua_State* L) {
    std::vector<AmbientLayer> layers;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        const int n = lua_objlen(L, 1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            const int t = lua_gettop(L);
            if (!lua_istable(L, t)) luaL_error(L, "world.set_ambient: layer %d is not a table", i);
            AmbientLayer l;
            lua_getfield(L, t, "type");
            const std::string kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            if (!AmbientLayer::kindFromName(kind, l.kind))
                luaL_error(L, "world.set_ambient: unknown type '%s' (dust, leaves, fireflies, embers)", kind.c_str());
            if (l.kind == AmbientLayer::Kind::Fireflies) l.when = AmbientLayer::When::Night;
            lua_getfield(L, t, "time");
            if (lua_isstring(L, -1)) {
                const std::string w = lua_tostring(L, -1);
                if (w == "day") l.when = AmbientLayer::When::Day;
                else if (w == "night") l.when = AmbientLayer::When::Night;
                else if (w == "any") l.when = AmbientLayer::When::Any;
                else luaL_error(L, "world.set_ambient: time '%s' (day, night, any)", w.c_str());
            }
            lua_pop(L, 1);
            l.density = ambientField(L, t, "density", 1.0f);
            l.radius = ambientField(L, t, "radius", 0.0f);
            l.height = ambientField(L, t, "height", 0.0f);
            l.size = ambientField(L, t, "size", 1.0f);
            lua_getfield(L, t, "color");
            if (lua_isvector(L, -1)) {
                const float* v = lua_tovector(L, -1);
                l.color = Vec3(v[0], v[1], v[2]);
                l.hasColor = true;
            }
            lua_pop(L, 1);
            lua_pop(L, 1);
            if (l.density > 0.0f) layers.push_back(l);
        }
    }
    eng().setAmbientLayers(layers);
    return 0;
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
// Exposicao travada: `value` multiplica a imagem no lugar da autoexposicao
// (1 = a luz como foi calibrada); nil volta ao "exposure" do .env ou a'
// automatica. Vale ate' a proxima troca de mapa.
int world_set_exposure(lua_State* L) {
    eng().setScriptExposure(lua_isnoneornil(L, 1) ? -1.0f
                                                  : std::clamp(static_cast<float>(luaL_checknumber(L, 1)), 0.01f, 100.0f));
    return 0;
}
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
// Borda de mundo (E-BORDA-VAZIO, spec 12): { x0, z0, x1, z1, fade, color }
// vence o bloco "world_border" do .env; nil devolve o comando para ele (ou
// desliga, sem mapa). Fora do retangulo a tela funde para `color` (preto por
// padrao) ao longo de `fade` unidades.
int world_set_border(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        eng().setScriptBorder(WorldBorder{});
        return 0;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    nlohmann::json j = nlohmann::json::object();
    for (const char* k : {"x0", "z0", "x1", "z1", "fade"}) {
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
    eng().setScriptBorder(parseWorldBorder(j));
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
// Capacidades de quem anda numa tabela { swim, protected } (MoveCaps).
uint8_t moveCaps(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return 0;
    uint8_t caps = 0;
    lua_getfield(L, idx, "swim");
    if (lua_toboolean(L, -1)) caps |= Engine::kMoveSwim;
    lua_getfield(L, idx, "protected");
    if (lua_toboolean(L, -1)) caps |= Engine::kMoveProtected;
    lua_pop(L, 2);
    return caps;
}
// Com as regras de terreno (world.set_nav_rules) a conta e' a do chao inteiro:
// o piso mais alto no ponto (pedra, ponte), o liquido e as capacidades.
int world_walkable(lua_State* L) {
    const float x = float(luaL_checknumber(L, 1)), z = float(luaL_checknumber(L, 2));
    const float radius = float(luaL_optnumber(L, 3, 0.0));
    if (eng().navRules().on || lua_istable(L, 4)) {
        lua_pushboolean(L, eng().floorWalkable(x, z, radius, FLT_MAX, moveCaps(L, 4)));
        return 1;
    }
    const Vec3 p(x, eng().sampleTerrainHeight(x, z), z);
    lua_pushboolean(L, eng().navWalkable(x, z) && !eng().blockedAt(p, radius));
    return 1;
}
// Regras de terreno: { max_step, max_slope } liga (vale ate' desligar, nos
// mapas seguintes tambem); nil desliga e volta a grade pintada.
int world_set_nav_rules(lua_State* L) {
    Engine::NavRules r;
    if (lua_istable(L, 1)) {
        r.on = true;
        lua_getfield(L, 1, "max_step");
        r.maxStep = float(luaL_optnumber(L, -1, r.maxStep));
        lua_getfield(L, 1, "max_slope");
        r.maxSlopeDeg = float(luaL_optnumber(L, -1, r.maxSlopeDeg));
        lua_pop(L, 2);
    } else if (!lua_isnoneornil(L, 1)) {
        luaL_typeerror(L, 1, "table or nil");
    }
    eng().setNavRules(r);
    return 0;
}
int world_nav_rules(lua_State* L) {
    const Engine::NavRules& r = eng().navRules();
    if (!r.on) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, r.maxStep);
    lua_setfield(L, -2, "max_step");
    lua_pushnumber(L, r.maxSlopeDeg);
    lua_setfield(L, -2, "max_slope");
    return 1;
}
// Caminho no chao (ja' alisado) de `de` a `para`, ou nil se nao ha'.
// Terceiro argumento: numero (raio so' contra os solidos, como sempre foi) ou
// tabela { radius, margin }: raio do agente mais folga, longe dos solidos E
// das celulas que nao andam (parede, rampa), tambem nos trechos alisados.
// avoid_liquid na tabela: celula com o liquido acima do piso fica fora do
// caminho (Engine::findPath).
int world_path(lua_State* L) {
    // Busca longa num mapa grande e' trabalho do motor, nao laco do script:
    // fora do orcamento de tempo da chamada (como game.load).
    ScriptHost::NativeWork trabalho;
    const Vec3 from = checkVec3(L, 1), to = checkVec3(L, 2);
    float radius = 0.0f, clearance = 0.0f;
    uint8_t avoid = 0;
    // Regras de terreno: o caminho nao passa por onde o corpo nao entra
    // (perigo sem protected, agua funda sem swim).
    if (eng().navRules().on) {
        const uint8_t caps = moveCaps(L, 3);
        if (!(caps & Engine::kMoveProtected)) avoid |= Engine::kAvoidLava;
        if (!(caps & Engine::kMoveSwim)) avoid |= Engine::kAvoidWater;
    }
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "radius");
        const float agent = std::max(0.0f, float(luaL_optnumber(L, -1, 0.0)));
        lua_getfield(L, 3, "margin");
        const float margin = std::max(0.0f, float(luaL_optnumber(L, -1, 0.0)));
        lua_pop(L, 2);
        radius = clearance = agent + margin;
        // avoid_liquid: true (qualquer liquido), "lava" ou "water".
        lua_getfield(L, 3, "avoid_liquid");
        if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
            avoid |= Engine::kAvoidWater | Engine::kAvoidLava;
        } else if (lua_isstring(L, -1)) {
            const std::string kind = lua_tostring(L, -1);
            if (kind == "lava") avoid |= Engine::kAvoidLava;
            else if (kind == "water") avoid |= Engine::kAvoidWater;
            else luaL_error(L, "avoid_liquid: true, \"lava\" or \"water\"");
        }
        lua_pop(L, 1);
    } else {
        radius = float(luaL_optnumber(L, 3, 0.0));
    }
    std::vector<Vec3> path;
    if (!eng().findPath(from, to, radius, path, clearance, avoid)) {
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
    int instance = -1;
    lua_pushnumber(L, eng().floorHeight(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                        fromYArg(L, 3), &onModel, &instance));
    lua_pushboolean(L, onModel);
    // O modelo do piso: o jogo le' o tipo dele (models.file) e decide corrimao,
    // perigo embaixo etc. pelo proprio dado.
    if (instance >= 0) lua_pushinteger(L, instance + 1);
    else lua_pushnil(L);
    return 3;
}
// Raio da camera pelo pixel (espaco de input.mouse_position) ate' o chao.
int world_screen_to_ground(lua_State* L) {
    Vec3 hit;
    if (!eng().screenToGround(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)), hit,
                              float(luaL_optnumber(L, 3, 8000.0)))) {
        lua_pushnil(L);
        return 1;
    }
    pushVec3(L, hit);
    return 1;
}
int world_floor_walkable(lua_State* L) {
    lua_pushboolean(L, eng().floorWalkable(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                           float(luaL_optnumber(L, 3, 0.0)), fromYArg(L, 4), moveCaps(L, 5)));
    return 1;
}
// Opcoes de world.footprint / world.dust: campo numerico opcional e cor {r,g,b}.
float optField(lua_State* L, int idx, const char* key, float def) {
    if (!lua_istable(L, idx)) return def;
    lua_getfield(L, idx, key);
    const float v = lua_isnumber(L, -1) ? float(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}
Vec3 optColor(lua_State* L, int idx, const char* key, const Vec3& def) {
    if (!lua_istable(L, idx)) return def;
    lua_getfield(L, idx, key);
    Vec3 c = def;
    if (lua_istable(L, -1)) {
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, -1, i + 1);
            if (lua_isnumber(L, -1)) c[i] = float(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return c;
}
// world.footprint(pos, dir, {texture, shape, size, life, alpha, color, mirror}): marca
// no chao deitada e projetada, com a ponta da textura para `dir` (x e z).
int world_footprint(lua_State* L) {
    const Vec3 pos = checkVec3(L, 1);
    const Vec3 dir = checkVec3(L, 2);
    Engine::FootprintParams p;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "texture");
        if (lua_isstring(L, -1)) p.texture = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "mirror");
        p.mirror = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 3, "shape");
        if (lua_isstring(L, -1)) p.shape = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "owner");
        if (lua_isstring(L, -1)) p.owner = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "size");
        p.sizeGiven = lua_isnumber(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 3, "color");
        p.colorGiven = lua_istable(L, -1);
        lua_pop(L, 1);
    }
    p.size = std::clamp(optField(L, 3, "size", p.size), 0.01f, p.shape.empty() ? 4.0f : 40.0f);
    p.life = std::clamp(optField(L, 3, "life", p.life), 0.5f, 600.0f);
    p.alpha = std::clamp(optField(L, 3, "alpha", p.alpha), 0.0f, 1.0f);
    p.color = optColor(L, 3, "color", p.color);
    const float len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    eng().spawnFootprint(pos, len > 1e-6f ? Vec2(dir.x / len, dir.z / len) : Vec2(0.0f, 1.0f), p);
    return 0;
}
// world.dust(pos, {color, size, count, life, rise, alpha}): sopro de poeira.
int world_dust(lua_State* L) {
    const Vec3 pos = checkVec3(L, 1);
    Engine::DustParams p;
    p.size = std::clamp(optField(L, 2, "size", p.size), 0.01f, 4.0f);
    p.count = std::clamp(optField(L, 2, "count", p.count), 1.0f, 64.0f);
    p.life = std::clamp(optField(L, 2, "life", p.life), 0.1f, 30.0f);
    p.rise = optField(L, 2, "rise", p.rise);
    p.alpha = std::clamp(optField(L, 2, "alpha", p.alpha), 0.0f, 1.0f);
    p.color = optColor(L, 2, "color", p.color);
    eng().spawnDust(pos, p);
    return 0;
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
// Modo classico (o mesmo do botao ERUPT): luz antiga so' difusa, sem PBR,
// relevo, sombras, GI, bloom, DoF nem SSAO; desligar devolve o estado de antes.
int world_set_classic(lua_State* L) {
    eng().setClassicMode(lua_toboolean(L, 1) != 0);
    return 0;
}
// Liga/desliga o ceu (cupula do fundo) como o "sky": false do .env: cavernas e
// interiores. Dura ate trocar de mapa; a carga do mapa volta ao valor do .env.
int world_set_sky(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    eng().setSkyEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}
int world_sky(lua_State* L) {
    lua_pushboolean(L, eng().skyEnabled());
    return 1;
}
int world_classic(lua_State* L) {
    lua_pushboolean(L, eng().classicMode());
    return 1;
}
// Agua do modo classico: set_classic_water(padrao, quadros, alfa?, ladrilho?).
// O padrao leva dois %d (tipo da agua do mapa e numero do quadro).
// So' aceita "%d" e "%0Nd" e exatamente dois: o padrao vai para snprintf.
bool padraoDoisInteiros(const std::string& p) {
    int n = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] != '%') continue;
        size_t j = i + 1;
        while (j < p.size() && p[j] >= '0' && p[j] <= '9') ++j;
        if (j >= p.size() || p[j] != 'd') return false;
        ++n;
        i = j;
    }
    return n == 2;
}
int world_set_classic_water(lua_State* L) {
    const std::string padrao = luaL_checkstring(L, 1);
    luaL_argcheck(L, padraoDoisInteiros(padrao), 1, "pattern needs exactly two %d (water type, frame)");
    eng().setClassicWater(padrao, int(luaL_checkinteger(L, 2)), float(luaL_optnumber(L, 3, 0.6)),
                          float(luaL_optnumber(L, 4, 10.0)));
    return 0;
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
    s.focusPointSet = true;
    s.focusPoint = p;
    s.focusModel = -1;
    s.focusTiltAngle = std::numeric_limits<float>::quiet_NaN();
    s.focusRampNear = s.focusRampFar = s.focusTiltMaxNear = s.focusTiltMaxFar = s.focusScreenMix = -1.0f;
    s.focusSpotHeight = -1.0f;
    if (lua_istable(L, 2)) {
        // Forma nova: world.set_focus(ponto, {aperture, foreground, band,
        // max_blur, model, tilt_angle, ramp_near, ramp_far, max_near,
        // max_far, screen_mix}). Campo ausente = o do postprocess.json.
        auto campo = [L](const char* k, float def) {
            lua_getfield(L, 2, k);
            const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
            lua_pop(L, 1);
            return v;
        };
        s.focusAperture = campo("aperture", -1.0f);
        lua_getfield(L, 2, "foreground");
        s.focusForeground = lua_isnil(L, -1) ? true : lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        const float faixa = campo("band", -1.0f);
        s.focusBand = faixa < 0.0f ? -1.0f : faixa;
        const float teto = campo("max_blur", -1.0f);
        s.focusMaxBlur = teto < 0.0f ? -1.0f : teto;
        lua_getfield(L, 2, "model");
        if (lua_isstring(L, -1)) {
            const std::string m = lua_tostring(L, -1);
            s.focusModel = m == "spotlight" ? 3
                         : (m == "tilt_plane" ? 2 : (m == "thin_lens" ? 1 : (m == "linear" ? 0 : -1)));
            if (s.focusModel < 0)
                luaL_error(L, "world.set_focus: model must be \"linear\", \"thin_lens\", \"tilt_plane\" or \"spotlight\"");
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "tilt_angle");
        if (lua_isnumber(L, -1)) s.focusTiltAngle = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        s.focusRampNear = campo("ramp_near", -1.0f);
        s.focusRampFar = campo("ramp_far", -1.0f);
        s.focusTiltMaxNear = campo("max_near", -1.0f);
        s.focusTiltMaxFar = campo("max_far", -1.0f);
        s.focusScreenMix = campo("screen_mix", -1.0f);
        // spotlight: band = raio, ramp_far = rampa, height/ramp_near = altura.
        s.focusSpotHeight = campo("height", -1.0f);
        s.focusOverride = true;
        return 0;
    }
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
    // {band = margem}: tambem a faixa da tela abaixo do centro (Engine::setSeeThrough).
    float band = 0.0f, minHidden = 0.0f;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "band");
        band = static_cast<float>(luaL_optnumber(L, -1, 0.0));
        lua_pop(L, 1);
        lua_getfield(L, 4, "min_hidden");
        minHidden = static_cast<float>(luaL_optnumber(L, -1, 0.0));
        lua_pop(L, 1);
    }
    eng().setSeeThrough(static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.25)),
                        atFocus, band, minHidden);
    return 0;
}
// Camera nao entra no relevo (Engine::setCameraCollide).
int camera_collide(lua_State* L) {
    float margin = 6.0f;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "margin");
        margin = static_cast<float>(luaL_optnumber(L, -1, margin));
        lua_pop(L, 1);
    }
    eng().setCameraCollide(lua_toboolean(L, 1) != 0, margin);
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
        c.transient = ScriptHost::editorPreviewActive(); // previa do editor: nao entra na cena
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

// Tira o personagem e, se foi o ultimo da folha, devolve as camadas dela.
int characters_remove(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    lua_pushboolean(L, eng().removeCharacter(luaL_checkstring(L, 1)));
    return 1;
}
// Solta uma folha de sprite da cache que nenhum personagem usa mais.
int characters_release_sheet(lua_State* L) {
    ScriptHost::NativeWork trabalho;
    lua_pushboolean(L, eng().releaseCharacterSheet(luaL_checkstring(L, 1)));
    return 1;
}

// ---------------------------------------------------------------- perf
// Contador de desempenho para o jogo desenhar (o F3 e' do editor). Os numeros
// do quadro sao os que o laco principal ja' mede para o F3 e a telemetria; o
// que vem da maquina (temperatura, bateria) e' lido de /sys no maximo uma vez
// por segundo, so' quando um script pede.

// loading.hold/release: o jogo segura a tela de carga do motor enquanto
// monta a propria cena, sem quadro de cena crua entre os dois.
int loading_hold(lua_State* L) {
    eng().holdLoading(true, float(luaL_optnumber(L, 1, 30.0)));
    return 0;
}
int loading_release(lua_State*) {
    eng().holdLoading(false);
    return 0;
}
int loading_held(lua_State* L) {
    lua_pushboolean(L, eng().loadingHeld());
    return 1;
}
int loading_game_progress(lua_State* L) {
    eng().setGameLoadProgress(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)));
    return 0;
}
int loading_progress(lua_State* L) {
    float done = 0.0f, total = 1.0f;
    bool ready = false;
    eng().loadProgress(done, total, ready);
    lua_pushnumber(L, done);
    lua_pushnumber(L, total);
    lua_pushboolean(L, ready);
    return 3;
}

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
    platform::setFrameRateHint(ScriptBridge::fpsCap());
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
    const TextureLayers::Stats layers = ScriptBridge::layerStats();
    if (layers.capacity > 0) {
        lua_createtable(L, 0, 6);
        setNumberField(L, "used", layers.used);
        setNumberField(L, "capacity", layers.capacity);
        setNumberField(L, "waiting", layers.waiting);
        setNumberField(L, "lost", layers.lost);
        setNumberField(L, "lost_events", layers.lostEvents);
        setNumberField(L, "side", layers.side);
        lua_setfield(L, -2, "layers");
    }
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

// perf.bandwidth(): o medidor de banda (spec 50). Bytes por passe do quadro
// anterior (estimativa dos anexos: formato x area x load/store), DRAM medida
// onde ha' contador (Intel) e ocupacao da VRAM (NVML). A primeira chamada liga
// o contador de DRAM.
int perf_bandwidth(lua_State* L) {
    static const bool kDram = bw::enableDramCounter();
    (void)kDram;
    constexpr double kMB = 1024.0 * 1024.0;
    const bw::FrameStat f = bw::lastFrame();
    lua_createtable(L, 0, 8);
    setNumberField(L, "read_mb", f.read / kMB);
    setNumberField(L, "write_mb", f.written / kMB);
    lua_createtable(L, static_cast<int>(f.passes.size()), 0);
    int i = 0;
    for (const auto& p : f.passes) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, p.name.c_str());
        lua_setfield(L, -2, "name");
        setNumberField(L, "read_mb", p.read / kMB);
        setNumberField(L, "write_mb", p.written / kMB);
        const float ms = Profiler::getGpuTime(p.name);
        if (ms > 0.0f) setNumberField(L, "gpu_ms", ms);
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "passes");
    if (f.dramValid && f.dramSeconds > 0.0) {
        setNumberField(L, "dram_read_gbs", f.dramReadBytes / f.dramSeconds / 1e9);
        setNumberField(L, "dram_write_gbs", f.dramWriteBytes / f.dramSeconds / 1e9);
    }
    lua_pushstring(L, bw::dramStatus());
    lua_setfield(L, -2, "dram");
    const float busy = SystemMonitor::gpuMemBusyPercent();
    if (busy >= 0.0f && bw::deviceVendor() == 0x10DE) setNumberField(L, "vram_busy_pct", busy);
    const bw::AuditStat a = bw::auditStats();
    lua_createtable(L, 0, 3);
    setNumberField(L, "targets", a.targets);
    setNumberField(L, "flagged", a.flagged);
    setNumberField(L, "warnings", a.warnings);
    lua_setfield(L, -2, "audit");
    lua_createtable(L, 0, 2);
    setNumberField(L, "compressed_mb", a.textureCompressedBytes / kMB);
    setNumberField(L, "uncompressed_mb", a.textureUncompressedBytes / kMB);
    lua_setfield(L, -2, "textures");
    return 1;
}

// Funcao que o export cortou (ERUPTION_API_FILTER, src/script/ApiFilter.hpp):
// o script que a chama recebe um erro claro. O upvalue e' "global.funcao".
int api_not_exported(lua_State* L) {
    luaL_error(L, "funcao nao exportada: %s (este jogo foi exportado so' com a API que os scripts usam; "
                  "para mante-la, liste-a em export_keep.json)",
               lua_tostring(L, lua_upvalueindex(1)));
    return 0;
}

void lib(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        if (f->func) {
            lua_pushcfunction(L, f->func, f->name);
        } else {
            lua_pushstring(L, (std::string(name) + "." + f->name).c_str());
            lua_pushcclosurek(L, api_not_exported, f->name, 1, nullptr);
        }
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
    // "f1".."f12": no jogo exportado as teclas F sao do jogo (no PC de
    // desenvolvimento F1-F4 tambem abrem os menus do motor).
    if ((name[0] == 'f' || name[0] == 'F') && name[1] >= '1' && name[1] <= '9') {
        const bool two = name[2] >= '0' && name[2] <= '9' && !name[3];
        const int n = !name[2] ? name[1] - '0' : (two ? (name[1] - '0') * 10 + (name[2] - '0') : 0);
        if (n >= 1 && n <= 12) return static_cast<Key>(static_cast<int>(Key::F1) + n - 1);
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
        ERUPTION_API_FN(on, start), ERUPTION_API_FN(on, update), ERUPTION_API_FN(on, every), ERUPTION_API_FN(on, key),
        ERUPTION_API_FN(on, near), ERUPTION_API_FN(on, hour), ERUPTION_API_FN(on, enter), ERUPTION_API_FN(on, leave), ERUPTION_API_FN(on, step), {nullptr, nullptr}};
    static const luaL_Reg vars[] = {ERUPTION_API_FN(vars, get), ERUPTION_API_FN(vars, set), ERUPTION_API_FN(vars, add), {nullptr, nullptr}};
    static const luaL_Reg ui[] = {ERUPTION_API_FN(ui, message), ERUPTION_API_FN(ui, minimap_label), ERUPTION_API_FN(ui, minimap_frame), ERUPTION_API_FN(ui, show_hud), ERUPTION_API_FN(ui, show_minimap),
                                  ERUPTION_API_FN(ui, load), ERUPTION_API_FN(ui, place), ERUPTION_API_FN(ui, image_rect), ERUPTION_API_FN(ui, text), ERUPTION_API_FN(ui, value), ERUPTION_API_FN(ui, image),
                                  ERUPTION_API_FN(ui, visible), ERUPTION_API_FN(ui, color), ERUPTION_API_FN(ui, has), ERUPTION_API_FN(ui, clicked), ERUPTION_API_FN(ui, hovered), ERUPTION_API_FN(ui, state), ERUPTION_API_FN(ui, dragged), ERUPTION_API_FN(ui, size), ERUPTION_API_FN(ui, held), ERUPTION_API_FN(ui, released),
                                  ERUPTION_API_FN(ui, reset), ERUPTION_API_FN(ui, anchor), ERUPTION_API_FN(ui, portrait), ERUPTION_API_FN(ui, badge),
                                  ERUPTION_API_FN(ui, graph), ERUPTION_API_FN(ui, graph_clicked), ERUPTION_API_FN(ui, graph_held), {nullptr, nullptr}};
    static const luaL_Reg world[] = {
        ERUPTION_API_FN(world, time), ERUPTION_API_FN(world, set_time), ERUPTION_API_FN(world, time_between),
        ERUPTION_API_FN(world, weather), ERUPTION_API_FN(world, is_weather), ERUPTION_API_FN(world, set_weather),
        ERUPTION_API_FN(world, set_wetness), ERUPTION_API_FN(world, wetness), ERUPTION_API_FN(world, wet_area),
        ERUPTION_API_FN(world, look), ERUPTION_API_FN(world, set_look), ERUPTION_API_FN(world, set_ambient), ERUPTION_API_FN(world, set_dof), ERUPTION_API_FN(world, set_classic), ERUPTION_API_FN(world, set_sky), ERUPTION_API_FN(world, sky), ERUPTION_API_FN(world, set_classic_water), ERUPTION_API_FN(world, classic), ERUPTION_API_FN(world, set_fog), ERUPTION_API_FN(world, set_border), ERUPTION_API_FN(world, set_exposure), ERUPTION_API_FN(world, set_time_scale), ERUPTION_API_FN(world, time_scale), ERUPTION_API_FN(world, walkable), ERUPTION_API_FN(world, set_nav_rules), ERUPTION_API_FN(world, nav_rules), ERUPTION_API_FN(world, path), ERUPTION_API_FN(world, floor_height), ERUPTION_API_FN(world, screen_to_ground), ERUPTION_API_FN(world, floor_walkable), ERUPTION_API_FN(world, liquid_at), ERUPTION_API_FN(world, footprint), ERUPTION_API_FN(world, dust), ERUPTION_API_FN(world, ground_grid), ERUPTION_API_FN(world, set_motion_blur),
        ERUPTION_API_FN(world, set_chromatic_aberration), ERUPTION_API_FN(world, set_effect_radius),
        ERUPTION_API_FN(world, set_focus), ERUPTION_API_FN(world, set_ground_blend),
        ERUPTION_API_FN(world, set_cutaway),
        ERUPTION_API_FN(world, ground_height), ERUPTION_API_FN(world, nav_height), ERUPTION_API_FN(world, ground_layers), ERUPTION_API_FN(world, liquids), ERUPTION_API_FN(world, set_liquid_level),
        ERUPTION_API_FN(world, block_slopes), ERUPTION_API_FN(world, set_walk_cost), ERUPTION_API_FN(world, walk_cost),
        ERUPTION_API_FN(world, map), ERUPTION_API_FN(world, load_map), ERUPTION_API_FN(world, set_load_distance),
        ERUPTION_API_FN(world, load_distance),
        ERUPTION_API_FN(world, warp_map), ERUPTION_API_FN(world, warping),
        ERUPTION_API_FN(world, preload_map),
        ERUPTION_API_FN(world, draw_strip), ERUPTION_API_FN(world, draw_sprite), {nullptr, nullptr}};
    static const luaL_Reg player[] = {
        ERUPTION_API_FN(player, position), ERUPTION_API_FN(player, set_position), ERUPTION_API_FN(player, set_sprite), ERUPTION_API_FN(player, near),
        ERUPTION_API_FN(player, inside), ERUPTION_API_FN(player, say), ERUPTION_API_FN(player, play), ERUPTION_API_FN(player, animation),
        ERUPTION_API_FN(player, freeze), ERUPTION_API_FN(player, frozen), ERUPTION_API_FN(player, hide), ERUPTION_API_FN(player, set_run),
        {nullptr, nullptr}};
    static const luaL_Reg camera[] = {
        ERUPTION_API_FN(camera, target), ERUPTION_API_FN(camera, set_target),
        ERUPTION_API_FN(camera, orbit), ERUPTION_API_FN(camera, set_orbit), ERUPTION_API_FN(camera, basis),
        ERUPTION_API_FN(camera, view),
        ERUPTION_API_FN(camera, free), ERUPTION_API_FN(camera, keep_inside_map),
        ERUPTION_API_FN(camera, see_through), ERUPTION_API_FN(camera, near_fade), ERUPTION_API_FN(camera, set_orbit_input),
        ERUPTION_API_FN(camera, collide), {nullptr, nullptr}};
    static const luaL_Reg fx[] = {
        ERUPTION_API_FN(fx, shake_camera), ERUPTION_API_FN(fx, number), ERUPTION_API_FN(fx, fade), ERUPTION_API_FN(fx, flash), ERUPTION_API_FN(fx, dim),
        ERUPTION_API_FN(fx, fading), {nullptr, nullptr}};
    static const luaL_Reg input[] = {ERUPTION_API_FN(input, down), ERUPTION_API_FN(input, pressed), ERUPTION_API_FN(input, text_begin), ERUPTION_API_FN(input, text_end), ERUPTION_API_FN(input, text_active), ERUPTION_API_FN(input, text), ERUPTION_API_FN(input, touch), ERUPTION_API_FN(input, touch_pad), ERUPTION_API_FN(input, double_tapped), ERUPTION_API_FN(input, gamepad), ERUPTION_API_FN(input, mouse_position), ERUPTION_API_FN(input, scroll), ERUPTION_API_FN(input, touches), ERUPTION_API_FN(input, set_cursor), {nullptr, nullptr}};
    static const luaL_Reg models[] = {
        ERUPTION_API_FN(models, count), ERUPTION_API_FN(models, preload), ERUPTION_API_FN(models, find), ERUPTION_API_FN(models, name),
        ERUPTION_API_FN(models, position), ERUPTION_API_FN(models, set_position), ERUPTION_API_FN(models, near),
        ERUPTION_API_FN(models, file), ERUPTION_API_FN(models, scale),
        ERUPTION_API_FN(models, set_rotation), ERUPTION_API_FN(models, set_scale),
        ERUPTION_API_FN(models, move_by), ERUPTION_API_FN(models, rotate_by),
        ERUPTION_API_FN(models, visible), ERUPTION_API_FN(models, set_visible),
        ERUPTION_API_FN(models, show), ERUPTION_API_FN(models, hide), ERUPTION_API_FN(models, solid), ERUPTION_API_FN(models, set_solid),
        ERUPTION_API_FN(models, set_solid_file), ERUPTION_API_FN(models, surface), ERUPTION_API_FN(models, set_surface),
        ERUPTION_API_FN(models, set_surface_file),
        ERUPTION_API_FN(models, play), ERUPTION_API_FN(models, animation), ERUPTION_API_FN(models, animations),
        ERUPTION_API_FN(models, morphs), ERUPTION_API_FN(models, set_morph),
        {nullptr, nullptr}};
    static const luaL_Reg characters[] = {
        ERUPTION_API_FN(characters, exists), ERUPTION_API_FN(characters, preload), ERUPTION_API_FN(characters, preload_ready),
        ERUPTION_API_FN(characters, position),
        ERUPTION_API_FN(characters, set_position), ERUPTION_API_FN(characters, walk_to),
        ERUPTION_API_FN(characters, walking), ERUPTION_API_FN(characters, face), ERUPTION_API_FN(characters, show), ERUPTION_API_FN(characters, say), ERUPTION_API_FN(characters, play), ERUPTION_API_FN(characters, animation),
        ERUPTION_API_FN(characters, follow), ERUPTION_API_FN(characters, patrol), ERUPTION_API_FN(characters, stop),
        ERUPTION_API_FN(characters, set_shadow), ERUPTION_API_FN(characters, set_sink),
        ERUPTION_API_FN(characters, set_rim),
        ERUPTION_API_FN(characters, set_anim_fps), ERUPTION_API_FN(characters, set_stride), ERUPTION_API_FN(characters, set_walk_rate),
        ERUPTION_API_FN(characters, hide), ERUPTION_API_FN(characters, ring), ERUPTION_API_FN(characters, set_sprite),
        ERUPTION_API_FN(characters, add), ERUPTION_API_FN(characters, remove), ERUPTION_API_FN(characters, release_sheet),
        ERUPTION_API_FN(characters, flash), ERUPTION_API_FN(characters, shake), ERUPTION_API_FN(characters, lunge),
        ERUPTION_API_FN(characters, pop), ERUPTION_API_FN(characters, fade), ERUPTION_API_FN(characters, set_scale), ERUPTION_API_FN(characters, head_screen), ERUPTION_API_FN(characters, at_screen), ERUPTION_API_FN(characters, on_step),
        {nullptr, nullptr}};
    static const luaL_Reg lights[] = {
        ERUPTION_API_FN(lights, count), ERUPTION_API_FN(lights, position), ERUPTION_API_FN(lights, set_position),
        ERUPTION_API_FN(lights, color), ERUPTION_API_FN(lights, set_color), ERUPTION_API_FN(lights, intensity),
        ERUPTION_API_FN(lights, set_intensity), ERUPTION_API_FN(lights, enabled),
        ERUPTION_API_FN(lights, set_enabled), ERUPTION_API_FN(lights, turn_on), ERUPTION_API_FN(lights, turn_off),
        ERUPTION_API_FN(lights, set_sprite_shadow), {nullptr, nullptr}};
    static const luaL_Reg audio[] = {
        ERUPTION_API_FN(audio, play), ERUPTION_API_FN(audio, play_at), ERUPTION_API_FN(audio, loop), ERUPTION_API_FN(audio, loop_at),
        ERUPTION_API_FN(audio, stop), ERUPTION_API_FN(audio, playing), ERUPTION_API_FN(audio, music), ERUPTION_API_FN(audio, stop_music),
        ERUPTION_API_FN(audio, set_volume), ERUPTION_API_FN(audio, set_pitch), ERUPTION_API_FN(audio, music_volume),
        ERUPTION_API_FN(audio, group_volume), ERUPTION_API_FN(audio, preload), {nullptr, nullptr}};
    lib(L, "on", on);
    lib(L, "audio", audio);
    static const luaL_Reg game[] = {ERUPTION_API_FN(game, save), ERUPTION_API_FN(game, load), ERUPTION_API_FN(game, restart), ERUPTION_API_FN(game, save_settings), ERUPTION_API_FN(game, settings), ERUPTION_API_FN(game, has_save),
                                    ERUPTION_API_FN(game, delete_save), ERUPTION_API_FN(game, screenshot), {nullptr, nullptr}};
    lib(L, "game", game);
    static const luaL_Reg windowLib[] = {ERUPTION_API_FN(window, set_fullscreen), ERUPTION_API_FN(window, is_fullscreen), {nullptr, nullptr}};
    lib(L, "window", windowLib);
    lib(L, "vars", vars);
    lib(L, "ui", ui);
    lib(L, "world", world);
    lib(L, "player", player);
    lib(L, "camera", camera);
    lib(L, "input", input);
    static const luaL_Reg ioLib[] = {ERUPTION_API_FN(io, read_text), ERUPTION_API_FN(io, list_dir), {nullptr, nullptr}};
    lib(L, "io", ioLib);
    lib(L, "models", models);
    lib(L, "lights", lights);
    lua_pushcfunction(L, api_require, "require");
    lua_setglobal(L, "require");
    static const luaL_Reg perf[] = {
        ERUPTION_API_FN(perf, fps), ERUPTION_API_FN(perf, frame), ERUPTION_API_FN(perf, resolution),
        ERUPTION_API_FN(perf, fps_cap), ERUPTION_API_FN(perf, set_fps_cap), ERUPTION_API_FN(perf, system),
        ERUPTION_API_FN(perf, bandwidth), {nullptr, nullptr}};
    lib(L, "characters", characters);
    lib(L, "fx", fx);
    lib(L, "perf", perf);
    static const luaL_Reg loading[] = {
        ERUPTION_API_FN(loading, hold), ERUPTION_API_FN(loading, release), ERUPTION_API_FN(loading, held), ERUPTION_API_FN(loading, progress),
        ERUPTION_API_FN(loading, game_progress), {nullptr, nullptr}};
    lib(L, "loading", loading);
    registerVfxApi(L, engine);
    registerObjectsApi(L, engine);
    registerRibbonApi(L, engine);
    registerVisionApi(L, engine);
    registerGraphicsApi(L, engine);
    registerTelemetryApi(L, engine);
    registerNetApi(L, engine);
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

-- EDITOR SCRIPTS. Files listed in project.eruption under "editor_scripts" run
-- only in the editor, in edit mode, to put PREVIEW content on the map (for
-- example characters a server would create in the real game). Lifecycle:
-- the file loads once (the top level runs); after every map load, and after
-- every Stop, the engine calls `on_map_loaded(map_name)`; before the preview
-- goes away (Play, map change) it calls `on_preview_clear()`. During Play no
-- editor script runs. Whatever they create with characters.add and
-- objects.spawn is transient: not saved in the scene, gone on Play and on map
-- change, rebuilt on Stop. Editor scripts are not run by the game.
--   "editor_scripts": ["assets/scripts/editor_preview.luau"]
--   "editor_read_paths": ["../server/maps"]   (extra folders io may read)
-- Read-only file access, available only inside editor scripts. Allowed paths:
-- the project folder and the folders of "editor_read_paths" (relative paths
-- start at the project folder; ".." and links are resolved before checking).
-- Both return nil plus a reason when the path is denied or does not exist.
declare io: {
    -- Whole text file (up to 16 MB).
    read_text: (path: string) -> (string?, string?),
    -- Names of the entries of a folder, sorted.
    list_dir: (path: string) -> ({string}?, string?),
}

-- Graphics presets (data/graphics.json). info() describes the GPU and the
-- active preset; values(name) returns a preset's keys (nil if it does not
-- exist); apply(base, overrides) switches live to a preset plus overrides and
-- returns false if the preset does not exist. Keys decided at startup
-- (post_format, shader_lite, texture_layer_*) only change on the next launch.
declare graphics: {
    info: () -> { gpu: string, vram_total_mb: number?, preset: string },
    values: (name: string) -> { [string]: any }?,
    apply: (base_preset: string, overrides: { [string]: boolean | number | string }?) -> boolean,
}

-- A model is referenced by its name (string) or its index (number, from 1).
type Model = string | number

type StepGround = { material: string, texture: string, node: string, surface: string, uv: vector, in_water: boolean, foot: number, direction: vector, contact: string, shape: string, width: number }

-- Events. Call these at the top level of a script, not inside functions.
declare on: {
    start: (fn: () -> ()) -> (),
    update: (fn: (dt: number) -> ()) -> (),
    every: (seconds: number, fn: () -> ()) -> (),
    -- Key names: "A".."Z", "0".."9" (top row or keypad), "space", "enter", "escape", "tab",
    -- "left", "right", "up", "down", "shift", "ctrl", "alt", "f1".."f12".
    key: (key: string, fn: () -> ()) -> (),
    -- Fires when the player gets within `distance` of the model.
    near: (model: string, distance: number, fn: () -> ()) -> (),
    -- Fires when the clock passes this hour (0 to 24).
    hour: (hour: number, fn: () -> ()) -> (),
    -- Fires when the player walks into / out of a trigger area (Add > Trigger Area).
    enter: (area: string, fn: () -> ()) -> (),
    leave: (area: string, fn: () -> ()) -> (),
    -- Fires for every footstep of a character (characters.add + set_stride;
    -- same as characters.on_step). `position` is the foot on the ground.
    -- `ground` describes the ground triangle under the foot: material = the
    -- glTF material name ("" when none; on tiled grounds it is the original
    -- texture path), texture = the colour image name, uv = TEXCOORD_0 at the
    -- foot wrapped to 0..1 (u, v, 0; row 0 is the top of the image, as
    -- sampled when drawing), surface = the surface type drawn on that texture
    -- with the editor's Regions tab ("sand", "snow"; "" when none),
    -- in_water = a water surface covers the foot (the
    -- other fields are then empty). Decide the effect here with world.dust
    -- and world.footprint; the engine has no surface table.
    -- A character with a sprite steps by its own pixels: `contact` is "strike"
    -- (a foot that was drawn landing on the ground), "drag" (a body that slides
    -- on the ground, stamped every stretch walked) or "stride" (no sprite, a
    -- step every half stride). The pixels decide when, how many and the
    -- shape: `shape` is the print of the sprite, estimated from its 8 walking
    -- directions (seen from the side a foot gives its length, from the front
    -- its width; the print is what fits all of them), the same for every
    -- step of that sprite, `width` its width in metres. Pass shape to
    -- world.footprint.
    step: (fn: (name: string, position: vector, ground: StepGround) -> ()) -> (),
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
    -- The engine's own minimap orthographic frame: world (cx, cz) of its
    -- centre and zoom (half-side in world units). This is the REAL frame the
    -- engine baked (from terrain/instance AABBs), not the map's nominal size,
    -- so it stays correct on a map whose geometry does not match its grid, or
    -- while it is still loading. Use it to place minimap markers instead of
    -- recomputing the framing formula from map data. zoom <= 0 before the
    -- first bake (no map loaded yet).
    minimap_frame: () -> (number, number, number),
    -- Position and size of an element of the loaded UI file, in percent of
    -- its parent (or of the screen), like CSS left/top/width/height. Without
    -- width/height the file's size stays; left = nil gives it back to the CSS.
    place: (selector: string, left: number?, top: number?, width: number?, height: number?) -> (),
    -- Crop of an image element in texture coordinates (0..1): a window into a
    -- bigger image. u0 = nil shows the whole image again.
    image_rect: (selector: string, u0: number?, v0: number?, u1: number?, v1: number?) -> (),
    -- Keeps the element's center on the top of a character's sprite (the
    -- current frame, scale included), `dy` pixels above it, every frame with
    -- the camera of that frame. Its children (--parent) follow it, and it
    -- hides while the character is off screen. character = nil lets go.
    anchor: (selector: string, character: string?, dy: number?) -> (),
    -- The element shows a character facing front, cropped to `crop` = {x, y,
    -- w, h} in fractions of its first idle frame (default the whole frame,
    -- {0, 0, 1, 0.4} for the head), fit inside without stretching. It plays
    -- the idle at the sheet's delay; a named animation the character plays
    -- (characters.play, a hit) shows on the same frame, and its flash and
    -- fade tint the portrait. No script runs per frame. character = nil
    -- lets go.
    portrait: (selector: string, character: string?, crop: { number }?) -> (),
    -- A badge on the element's top-right corner: text ("+", "3") or a PNG of
    -- the project; nil removes it. It hides with the element and has its own
    -- touch area of at least 48 dp: ui.clicked(selector .. ":badge"), and a
    -- tap on it is not a tap on the element. --badge-color and --badge-size
    -- in the element's CSS change the look.
    badge: (selector: string, content: string?) -> (),
    -- A graph of nodes drawn on the element (a skill tree, a map of stages):
    -- each node { id, x?, y?, icon?, text?, color?, links? }. x and y are
    -- 0..1 in the element's box; without them the nodes go in layers by
    -- depth from the roots (nodes no link reaches), top to bottom, or left
    -- to right with --graph-direction: right. links lists the ids a node
    -- points to. icon is a PNG of the project; with it, text goes under the
    -- node. --node-size, --node-color and --link-color in the element's CSS
    -- change the look. nil removes the graph.
    graph: (selector: string, nodes: { { id: string, x: number?, y: number?, icon: string?, text: string?, color: vector?, links: { string }? } }?) -> (),
    -- Id of the graph node tapped this frame, or nil. Each node takes taps
    -- within its radius or 24 dp, whichever is larger.
    graph_clicked: (selector: string) -> string?,
    -- Id of the node where the press still held began, once it has lasted
    -- `seconds` (default 0.5), every frame until it lifts; nil otherwise.
    -- A long press also fires graph_clicked when it starts.
    graph_held: (selector: string, seconds: number?) -> string?,
    -- Character bars (life, mana) and the minimap with the map name and time.
    -- Off in exported games until the game turns them on.
    show_hud: (on: boolean) -> (),
    show_minimap: (on: boolean) -> (),
    -- The game's own interface, described in a CSS file of the project and
    -- edited in Caldera: every rule with an id (#menu, #hp-1) is an element,
    -- and `--widget` says whether it is a rect, text, a bar or an image.
    -- The script only fills VALUES in; where each thing sits, in what color
    -- and what size, is the file's business, reloaded when it changes.
    -- Lengths take px, % of the parent, or vw/vh/vmin/vmax of the screen.
    -- Shapes: clip-path: polygon(x y, ...) and border-radius with one value
    -- per corner, on rects and bars; transform: rotate(30deg) on any element
    -- (taps keep the unrotated box); --glow: <color> <radius>, or
    -- box-shadow: dx dy blur <color>, lights up the area around it.
    load: (css: string) -> boolean,
    text: (selector: string, text: string) -> (),
    value: (selector: string, value: number) -> (),   -- bar, 0 to 1
    -- Troca o arquivo de um widget de imagem (--widget: image).
    image: (selector: string, png: string) -> (),
    -- Hiding an element hides its children (--parent) with it.
    visible: (selector: string, on: boolean) -> (),
    -- which: 0 = background / rail / text / image tint (multiplies the
    -- image; --image-tint in the file), 1 = bar fill or border.
    color: (selector: string, color: vector, which: number?, opacity: number?) -> (),
    has: (selector: string) -> boolean,
    -- Tapped (phone) or clicked (mouse) this frame: an element of the file
    -- that is on screen, or "minimap". Makes any element a button.
    clicked: (selector: string) -> boolean,
    -- The mouse is over the drawn box of any element matching the selector
    -- (#id, .class, tag or compound such as .row.selected), measured like
    -- clicked. Always false on a phone until a finger is down. For looks, prefer
    -- :hover in the .css; use this when the script must react (a tooltip).
    hovered: (selector: string) -> boolean,
    -- Adds or removes the class `name` on every element matching the selector
    -- (#id, .class, tag or compound), so the .css can style it: `#id.name`,
    -- `.row.name`, `.name`. Survives a reload of the file; reset() drops it.
    state: (selector: string, name: string, on: boolean) -> (),
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
    -- Wet ground (darker, smoother, puddles in cavities). By default it follows
    -- the weather: rain soaks the ground in a few seconds, only where the rain
    -- reaches (roofs, awnings and tree crowns keep it dry underneath), and it
    -- dries by the air afterwards (heat, wind and dry air speed it up; about a
    -- minute at 20 C; below freezing it does not dry). set_wetness(target)
    -- holds it at target (0..1), moving there over `seconds` for a full 0 to
    -- 1 change (default 5, 0 = at once); set_wetness(nil) goes back to the
    -- weather. Classic mode is always dry.
    set_wetness: (target: number?, seconds: number?) -> (),
    -- Current wet level of the ground (0..1).
    wetness: () -> number,
    -- Wets a disc of ground (a water skill, a splash): center `position`,
    -- `radius` in world units (as the position), amount 0..1 (default 1),
    -- drying linearly over dry_seconds (default 30). With or without rain,
    -- covered or not; overlapping areas keep the wettest; at most 32 at once
    -- (the driest goes first). Cleared on map change.
    wet_area: (position: vector, radius: number, amount: number?, dry_seconds: number?) -> (),
    -- Terrain height at a world position.
    ground_height: (x: number, z: number) -> number,
    -- Walkable-path height (Y) at a world position, from the corner heights of
    -- the navigation grid cell: bilinear over the 4 corners. Follows stairs,
    -- bridges and ramps, unlike ground_height. nil if the map has no corner
    -- heights or (x, z) is outside the grid.
    nav_height: (x: number, z: number) -> number?,
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
    -- Fields include saturation, contrast, god_ray_*, lens_flare (multiplies
    -- the preset's sun lens flare; 0 hides it), lens_flare_rays (lines of
    -- the flare's star; 1 = a single streak), vignette (strength,
    -- 0 = off), vignette_softness, shadow_tint / highlight_tint (a color as
    -- vector or {r, g, b}) with shadow_tint_strength / highlight_tint_strength,
    -- lut (a .cube or a 32^3 strip .png in the project) with lut_strength 0..1,
    -- halation_intensity / halation_threshold / halation_tint, bokeh_intensity /
    -- bokeh_threshold / bokeh_shape (0 disc, 1 hexagon), grain_intensity /
    -- grain_size (px at 1080p). These last ones override data/postprocess.json.
    set_look: (preset: string | { [string]: any }) -> boolean,
    -- Ambient particles around the camera focus: a list of layers, each
    -- {type = "dust" (motes lit by the sun through the shadow map: they
    -- shine in light shafts) | "leaves" (falling) | "fireflies" (blinking,
    -- night only by default) | "embers" (rising sparks), density (1 = the
    -- type's default count), radius / height (meters around and above the
    -- focus), size, color (vector, recolors), time = "day" | "night" |
    -- "any"}. nil or {} removes them. The preset's "ambient_particles"
    -- scales the density (0 = off).
    set_ambient: (layers: { { type: string, density: number?, radius: number?, height: number?,
        size: number?, color: vector?, time: string? } }?) -> (),
    set_dof: (on: boolean) -> (),
    -- World time scale (hit-stop, slow motion): sprite animation, effects,
    -- weather and the scripts' dt run scaled (0 = frozen). `seconds` counts in
    -- full time and goes back to 1 by itself (0 or none = until the next call).
    -- Sound and the screen fade/flash keep going.
    set_time_scale: (scale: number, seconds: number?) -> (),
    time_scale: () -> number,
    -- Walk grid of the engine (block_slopes and solid objects included):
    -- whether a character of `radius` can stand at (x, z). With terrain
    -- rules on (set_nav_rules) or a `caps` table it is the whole ground test
    -- of floor_walkable on the highest floor there: stone tops, bridges,
    -- liquid. caps: swim enters deep water, protected enters hazards (lava,
    -- "hazard" surfaces); a knocked-back body is the game's call.
    walkable: (x: number, z: number, radius: number?, caps: { swim: boolean?, protected: boolean? }?) -> boolean,
    -- Terrain rules: every cell inside the maps in the scene walks, except
    -- steep ones (more than max_step of rise within half a cell, or steeper
    -- than max_slope degrees, from the terrain heights); the painted walk
    -- grid no longer counts. Objects decide by their surface (models.
    -- set_surface), liquids by caps. Stays on for later maps; nil turns it off.
    set_nav_rules: (rules: { max_step: number?, max_slope: number? }?) -> (),
    nav_rules: () -> { max_step: number, max_slope: number }?,
    -- Cost of walking the grid cells within `radius` of (x, z) (0: only the
    -- cell of the point), this run only: 1 is normal, math.huge blocks.
    -- walkable, path and walking see it in the same frame; path goes around
    -- costly cells when that is cheaper. Returns how many cells changed.
    set_walk_cost: (x: number, z: number, radius: number, cost: number) -> number,
    -- Cost of the cell at (x, z) (1 when the game set none).
    walk_cost: (x: number, z: number) -> number,
    -- Path on the ground from `from` to `to` (smoothed), or nil if none.
    -- A number `radius` grows only the solid objects (the old call). A table
    -- { radius, margin } (world units) keeps every cell and every smoothed
    -- segment radius + margin away from solids AND from cells that do not
    -- walk (walls, blocked slopes); a start that touches a wall may leave it.
    -- The searches of one frame share a time budget: when it runs out the
    -- path ends at the point found closest to `to` (ask again later), and
    -- once it is spent the call returns nil.
    -- avoid_liquid (true, "lava" or "water") keeps the path out of cells with
    -- that liquid over the floor; the body can still stand there. The path
    -- also runs on solid objects: a bridge is crossed on its deck when the
    -- walk reaches it at its end, and passed under from below.
    -- With terrain rules on, the path also stays out of hazards and deep
    -- water unless the table says protected / swim.
    path: (from: vector, to: vector, radius: (number | { radius: number?, margin: number?, avoid_liquid: (boolean | string)?, swim: boolean?, protected: boolean? })?) -> { vector }?,
    -- Floor under (x, z) counting solid objects (bridges, walkways, kit
    -- slabs): the highest top below `fromY` (default: anywhere). Second value:
    -- true when it is an object, not the terrain.
    -- Ray from the camera through the pixel (x, y) to the ground: terrain or
    -- the top of a solid object, like floor_height. (x, y) are window pixels,
    -- the same space as input.mouse_position. nil when the ray hits nothing
    -- within maxDist (default 8000) or the camera is under the ground.
    screen_to_ground: (x: number, y: number, maxDist: number?) -> vector?,
    floor_height: (x: number, z: number, fromY: number?) -> (number, boolean),
    -- Whether a character of `radius` can stand on that floor: the walk grid
    -- on the terrain, the piece itself on an object, no other solid in the body.
    floor_walkable: (x: number, z: number, radius: number?, fromY: number?, caps: { swim: boolean?, protected: boolean? }?) -> boolean,
    -- Liquid showing at (x, z) (inside its shore and above the floor: a bridge
    -- over lava has none): {kind = "water" | "lava", level, index} or nil.
    -- index is the map's liquid (1-based), 0 for the global water plane.
    liquid_at: (x: number, z: number) -> { kind: string, level: number, index: number, map: string? }?,
    -- A footprint decal on the ground at `position`, tip of the texture toward
    -- `direction` (x and z used). Sizes are in metres. texture: PNG of the
    -- project (default: a built-in sole print, white with alpha, tinted by
    -- `color`); mirror flips it (right foot); life: seconds until it has
    -- faded; at most 64 stay alive (the oldest go first). shape: the
    -- `ground.shape` of a step: the print is that mask, at the sprite's size
    -- (size, if given, is the print's width), its front along `direction`;
    -- texture is ignored. Without `color` it only darkens the ground, which
    -- keeps its own tone (a pressed print); a color tints it. Trail pieces
    -- (drag steps) live in their own pool of 128, at most 48 per `owner`
    -- (the character's name). Call it from
    -- on.step / characters.on_step.
    footprint: (position: vector, direction: vector, opts: { texture: string?, shape: string?, owner: string?, size: number?, life: number?, alpha: number?, color: { number }?, mirror: boolean? }?) -> (),
    -- A puff of dust at `position`: size (m, per grain), count (particles,
    -- 1..64), life (s), rise (m/s2 upward), alpha, color {r, g, b} (linear).
    -- At most 24 puffs stay alive.
    dust: (position: vector, opts: { color: { number }?, size: number?, count: number?, life: number?, rise: number?, alpha: number? }?) -> (),
    -- A whole nx * nz grid from (x0, z0) with spacing `step`, x varying
    -- fastest: heights (f32 per point) and flags (u8 per point: 1 walkable,
    -- 2 water, 4 lava, 8 on an object; when not walkable, 16 wall (steep
    -- cell or a solid in the body) or 32 blocked (outside the maps, painted
    -- cell, "blocked" object); 64 standing on a "hazard" object).
    -- Read with buffer.readf32(h, i * 4).
    ground_grid: (x0: number, z0: number, step: number, nx: number, nz: number, radius: number?) -> (buffer, buffer),
    -- The game's own fog: {color = vector or {r, g, b}, start, end, opacity,
    -- height, falloff} (the map .env "fog" block fields). Wins over the map's
    -- and the weather's fog until the next map change; nil gives control back.
    set_fog: (fog: { [string]: any }?) -> (),
    -- World edge void (E-BORDA-VAZIO, spec 12): {x0, z0, x1, z1, fade, color}.
    -- Outside the [x0,z0]-[x1,z1] rectangle the screen fades to `color`
    -- (black by default) over `fade` world units (default 200). Wins over
    -- the map .env "world_border" block until the next map change; nil gives
    -- control back. The sky never darkens, same rule as vision.
    set_border: (border: { [string]: any }?) -> (),
    -- Locks the exposure at `value` (1 = the light as authored) instead of
    -- adapting to the frame, as the map .env "exposure": {"auto": false,
    -- "value": 1.0} does. nil gives control back (the map's lock or auto).
    -- Until the next map change.
    set_exposure: (value: number?) -> (),
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
    -- point at all gives the lens back to the automatic focus. The second
    -- argument may also be a table: {aperture, foreground, band, max_blur,
    -- model = \"linear\" | \"thin_lens\" | \"tilt_plane\" | \"spotlight\", tilt_angle (degrees,
    -- 0 = upright plane facing the camera, 90 = level plane), ramp_near,
    -- ramp_far (world units after the band), max_near, max_far (pixels at
    -- 1080p), screen_mix (0..1, mixes in a screen-height band)}.
    -- model = \"spotlight\": a sharp circle ON THE GROUND around the
    -- point; band = radius, ramp_far = ramp after it, max_far = blur outside;
    -- height + ramp_near + max_near blur what rises above the point.
    set_focus: (point: vector?, aperture: (number | {[string]: any})?, foreground: boolean?, band: number?, max_blur: number?) -> (),
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
    -- How close (meters from the map's edge) the player gets before a neighbor
    -- map of the scene starts loading in its thread (default 300; 50..3000).
    -- A bigger distance starts the load earlier, so a heavy neighbor is ready
    -- by the time the player reaches it, at the cost of keeping more maps in
    -- memory (a neighbor is released only past 1.5x this distance).
    set_load_distance: (meters: number) -> (),
    load_distance: () -> number,
    -- Reads a map in a thread ahead of time; a later warp_map to it only
    -- uploads. Call it when the next area becomes likely. False if the map
    -- does not exist.
    preload_map: (map: string) -> boolean,
    -- Immediate drawing of textured geometry in the world, for effects the
    -- engine has no primitive for (portals, energy rings, trails). Call it
    -- EVERY frame: the list is cleared after each frame, so stop calling to
    -- make it disappear. Drawn after the lighting and the particles, tested
    -- against the scene depth, in linear/HDR colour (rgb may exceed 1). The
    -- texture is a project PNG (sRGB), loaded once and cached by path; a file
    -- that fails to open only logs one warning. At most 32768 vertices per
    -- frame (the rest is dropped); at most 64 different textures.
    -- verts: array of { pos: vector, u: number?, v: number?, r: number?, g: number?,
    -- b: number?, a: number? } (u, v default 0; r, g, b, a default 1). U wraps
    -- (repeat), so a "rolling" U is just u + time.
    -- opts: blend = "additive" (SRC_ALPHA, ONE; default) | "alpha" (SRC_ALPHA,
    -- ONE_MINUS_SRC_ALPHA); topology = "strip" (default; triangle strip, ONE
    -- strip per call) | "quads" (4 vertices per quad in perimeter order, like a
    -- GL quad: 0-1-2-3, the remainder is ignored); depth_test = true (default);
    -- depth_write = false (the only supported value: the scene depth is read-only
    -- here, true is ignored with a warning); cull = false (default; true culls
    -- back faces, counter-clockwise is the front).
    draw_strip: (texture: string, verts: { { pos: vector, u: number?, v: number?, r: number?, g: number?, b: number?, a: number? } }, opts: { blend: string?, topology: string?, depth_test: boolean?, depth_write: boolean?, cull: boolean? }?) -> (),
    -- Billboard facing the camera, for particles. Call it every frame (see
    -- draw_strip; same texture rules). size: a number (square) or a vector
    -- (x = width, y = height), in WORLD UNITS (meters) by default; with
    -- opts.pixels = true it is in window pixels instead (fixed on-screen size,
    -- whatever the distance). rgba: { r, g, b, a } (array or named fields) or a
    -- vector (alpha 1); default white, linear/HDR. blend: "additive" (default) |
    -- "alpha". opts: pixels = false, depth_test = true, rotation = 0 (radians,
    -- on the screen plane).
    draw_sprite: (texture: string, pos: vector, size: number | vector, rgba: { [any]: number } | vector | nil, blend: string?, opts: { pixels: boolean?, depth_test: boolean?, rotation: number? }?) -> (),
)" + ribbonApiDefinitions() + R"(}
)" + netApiDefinitions() + R"(

-- Saved games: map, player position, vars, time, weather and music, kept in the
-- player's data folder. Slots are names or numbers (default 1).
declare game: {
    save: (slot: (string | number)?) -> boolean,
    load: (slot: (string | number)?) -> boolean,
    has_save: (slot: (string | number)?) -> boolean,
    delete_save: (slot: (string | number)?) -> boolean,
    -- Saves a PNG of the screen and returns the path it will write. The file
    -- appears a frame or two later. Without a path it goes to
    -- <player data folder>/screenshots/YYYY-MM-DD_HH-MM-SS.png (folder created
    -- as needed). `ui` (default true) includes the HUD and UI; with ui = false
    -- the frame is read before the UI pass (one synchronous readback).
    screenshot: (path: string?, opts: { ui: boolean? }?) -> string,
    -- Starts the game over: scripts from scratch (variables cleared, objects
    -- made by scripts removed) on the project's first map. Happens after the
    -- current frame.
    restart: () -> (),
    -- Player settings (volumes, speed...), kept apart from saved games.
    save_settings: (settings: { [string]: any }) -> boolean,
    -- The table last saved with save_settings, or an empty table.
    settings: () -> { [string]: any },
}

-- The game window. Alt+Enter does the same toggle.
declare window: {
    -- Fullscreen at the monitor's native mode, or back to the previous window.
    set_fullscreen: (on: boolean) -> (),
    is_fullscreen: () -> boolean,
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
    -- Same as on.step (top level of a script): fn(name, position, ground),
    -- see on.step for the `ground` fields.
    on_step: (fn: (name: string, position: vector, ground: StepGround) -> ()) -> (),
    -- Reads and prepares sprite sheets off the frame, so a character added
    -- later with one of them only uploads it. One file or a list; still valid
    -- on the next map (call it with world.preload_map).
    preload: (sprites: string | { string }) -> (),
    -- True when the sheet asked with preload is prepared, or when nothing is
    -- pending for it (never asked, PNG only, already on the GPU).
    preload_ready: (sprite: string) -> boolean,
    -- Shadow of every character: "planar" (cast by the sun), "circle" (a
    -- contact disc) or "lights" (the disc plus a shadow cast by each of the
    -- two strongest nearby lights) or "map" (the sprite's silhouette in the sun's
    -- shadow map: it follows the relief, breaks on steps and climbs walls) or
    -- "map_contact" (the silhouette, plus the contact disc where the sun does
    -- not reach the feet: valleys, under roofs). No argument goes back to the
    -- engine default. opts.contact_sun (0..1, default 0): with "map_contact",
    -- the disc also under the feet in the sun, at that strength. The disc is
    -- as wide as the sprite's feet (the bottom of its opaque pixels).
    set_shadow: (kind: string?, opts: { contact_sun: number? }?) -> (),
    -- Every character's sprite drawn this many frame pixels lower (0..64,
    -- default 0): the feet sink into the ground instead of resting on it.
    set_sink: (pixels: number?) -> (),
    -- Rim light: a thin bright edge on the side of every sprite that faces
    -- the sun (only where the sun reaches it) and a strong nearby light.
    -- strength 0 = off (default), ~0.3-0.8 subtle, up to 2; opts.color tints
    -- it (0..1, default white). The graphics preset's "sprite_rim" multiplies
    -- it (0 turns it off on weak GPUs).
    set_rim: (strength: number?, opts: { color: vector? }?) -> (),
    -- Stepped sprite animation ("on twos"): the frame shown changes only
    -- `fps` times per second (8-12 reads as hand-drawn), on a clock shared
    -- by every character; movement, camera and effects stay smooth. 0 = off
    -- (default). With `key` (a character name or a sheet file) the rule is
    -- only for it and wins over the global one; fps < 0 removes it. The
    -- preset's "sprite_step" = false turns every rule off.
    set_anim_fps: (fps: number?, key: string?) -> (),
    -- Stride: distance in units that one full walk cycle covers. While a
    -- character walks (set_position with walk = true, or a path), the frame
    -- delay becomes stride / speed / frame count, using the measured speed
    -- (smoothed), so the feet match the ground; the phase stays continuous.
    -- name = nil sets the default for all characters; a name sets just that
    -- one (it wins). units <= 0 restores the default pace (by body height).
    set_stride: (name: string?, units: number) -> (),
    -- Walk rate: while walking, each frame lasts sheet delay x (ms per cell at
    -- the current speed) / ref_ms, where a cell is `cell_units` units long and
    -- ref_ms defaults to 150. The whole cycle is frames x delay x ms-per-cell /
    -- ref_ms. name = nil is the default for all; cell_units <= 0 turns it off.
    -- Wins over set_stride when both are set.
    set_walk_rate: (name: string?, cell_units: number, ref_ms: number?) -> (),
    -- Puts a new character in the scene (sprite PNG or .spr of the project).
    -- False if the sprite could not be read.
    add: (name: string, sprite: string, position: vector, facing: number?) -> boolean,
    -- Takes a character out of the scene. When it was the last one using its
    -- sprite sheet, the sheet's GPU memory (and, on phones, its texture layers)
    -- is released too. False if there is no such character.
    remove: (name: string) -> boolean,
    -- Releases a sprite sheet no character uses any more (a monster type that
    -- has left the scene). False if some character still uses it or it is not loaded.
    release_sheet: (sprite: string) -> boolean,
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
    -- `seconds`: the whole animation lasts that long (each loop, when
    -- looping), e.g. an attack fitted to the interval between hits.
    play: (name: string, animation: string, loop: boolean?, seconds: number?) -> boolean,
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
    -- Top of the character's sprite on screen, in UI pixels (those of
    -- ui.size), and whether it is on screen this frame.
    head_screen: (name: string) -> (number, number, boolean),
    -- Name of the character whose drawn sprite box is under the pixel (x, y)
    -- (window pixels, the space of input.mouse_position); the one nearest to
    -- the camera when several overlap; nil if none.
    at_screen: (x: number, y: number) -> string?,
    -- A ring on the ground under the character's feet, lying on the relief.
    -- It follows hide/show, fade and team vision. width defaults to 15% of
    -- the radius (at least 1), color to white, alpha to 0.8. nil removes it.
    ring: (name: string, ring: { radius: number, color: vector?, alpha: number?, width: number? }?) -> (),
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
    -- while no focus point is set. With "focus", each character within
    -- `radius` of it also gets its own patch, cutting what stands more than
    -- 4 units in front of that character (a rock right in front of a big
    -- mount). options.band: also the strip of the
    -- screen below the center, down to the edge, cutting what is more than
    -- `band` world units in front of it (a low camera looking past rocks);
    -- painted ground meshes and floors at foot height stay.
    -- options.min_hidden (0..1, default 0 = always on): the cut only engages
    -- once at least that fraction of the focused character's sprite is hidden
    -- behind map models (a 4x6 ray grid, once per frame); it turns off below
    -- min_hidden - 0.1 and fades in and out over about 0.2 s.
    see_through: (radius: number, opacity: number?, center: ("player" | "focus")?,
                  options: { band: number?, min_hidden: number? }?) -> (),
    -- Map and model geometry closer than `start` world units to the camera
    -- disappears as a dither pattern and comes back fully by `end` (default
    -- 1.5 x start), so a fence brushing the lens no longer covers the screen.
    -- Shadows are unaffected. 0 turns it off.
    near_fade: (start: number, end_: number?) -> (),
    -- true: the orbit camera keeps out of the terrain (the open map and
    -- the neighbors in the scene). When the lens would be inside rock or
    -- behind a hill, it steps just past the rock, rises over it, or as a
    -- last resort moves in toward the target; it eases back slowly once the
    -- view is clear. options.margin keeps that many world units between lens
    -- and ground (default 6). Thin models (trees, posts) do not count;
    -- see_through handles them. Off by default.
    collide: (on: boolean, options: { margin: number? }?) -> (),
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
    -- Mouse buttons: "mouse_left", "mouse_right", "mouse_middle" (a press
    -- that started on a UI button is the UI's, not reported here); pair with
    -- input.mouse_position and world.screen_to_ground / characters.at_screen.
    -- Key names as in on.key, or a controller button (any connected
    -- controller, Xbox layout): "pad_a", "pad_b", "pad_x", "pad_y",
    -- "pad_lb", "pad_rb", "pad_back", "pad_start", "pad_guide", "pad_ls",
    -- "pad_rs", "pad_up", "pad_right", "pad_down", "pad_left".
    down: (key: string) -> boolean,
    -- True only on the frame the key went down.
    pressed: (key: string) -> boolean,
    -- Text entry (a chat line, a name field). text_begin() turns it on (and
    -- opens the phone keyboard), text_end() turns it off. While it is on,
    -- text() returns the UTF-8 typed since the last frame (layout and accents
    -- come from the system, printable characters only); with it off, text()
    -- is always "". Keys keep reporting in down/pressed, so a game ignores its
    -- action keys while text_active(); backspace and enter come from pressed.
    text_begin: () -> (),
    text_end: () -> (),
    text_active: () -> boolean,
    text: () -> string,
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
    -- Cursor do mouse do jogo (PC; no celular e no navegador nao faz nada).
    -- Sem chamar, vale o cursor do sistema. path e' um PNG do jogo; hotspot
    -- e' o pixel da imagem que aponta (a ponta da seta), em pixels da imagem
    -- original. opcoes: frame_w/frame_h (tamanho de um quadro da folha),
    -- frames (quantos), fps (animacao, padrao 8), scale (multiplica a escala
    -- da janela/DPI, padrao 1), software (true: o motor desenha o cursor, sem
    -- cursor de hardware; tambem vale se o sistema recusar o de hardware). Pode chamar todo quadro: a mesma chamada nao
    -- recarrega nem recria nada. set_cursor(nil) volta ao cursor do sistema.
    set_cursor: (path: string?, hotspot_x: number?, hotspot_y: number?, options: { frame_w: number?, frame_h: number?, frames: number?, fps: number?, scale: number?, software: boolean? }?) -> (),
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
    -- Surface of a model for walking: "floor" (the solid above: its top is
    -- floor, what rises past a step is wall), "hazard" (floor only for
    -- protected walkers), "blocked" and "wall" (never stepped on, at any
    -- height; wall also tells the game it stops projectiles), "none". In the
    -- scene file: objects = { name = { surface = "hazard" } }.
    surface: (model: Model) -> string,
    set_surface: (model: Model, surface: string) -> (),
    -- The same by model file, for every instance now and later (see set_solid_file).
    set_surface_file: (file: string, surface: string) -> (),
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

-- Texture layer use (phones). `lost` = textures that show the default texture
-- right now because no layer was left; `lost_events` = failed attempts since
-- start (one per retry). Release unused characters/maps when `lost` > 0.
type PerfLayers = {
    used: number,
    capacity: number,
    waiting: number,            -- copies still queued
    lost: number,
    lost_events: number,
    side: number,               -- pixels of each layer
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
    -- GPUs without descriptor indexing (most phones): the texture layers all
    -- textures are copied into. nil on GPUs that do not use them.
    layers: PerfLayers?,
}

-- One render pass in perf.bandwidth(): megabytes its attachments read and
-- write (format x size x load/store; texture sampling, compute and copies are
-- not counted) and its GPU time where the GPU has timers.
type PerfBandwidthPass = {
    name: string,
    read_mb: number,
    write_mb: number,
    gpu_ms: number?,
}

-- Memory traffic of the last frame. The pass list is an estimate; the dram_*
-- fields are measured by the memory controller (Intel on Linux; the whole
-- system: CPU, integrated GPU and display) and vram_busy_pct by the NVIDIA
-- driver (percent of time the video memory was busy, not GB/s).
type PerfBandwidth = {
    read_mb: number,
    write_mb: number,
    passes: { PerfBandwidthPass },
    dram_read_gbs: number?,
    dram_write_gbs: number?,
    dram: string,               -- counter source, or why there is none
    vram_busy_pct: number?,
    -- Render targets seen, how many carry a flag that turns off framebuffer
    -- compression, and the distinct warnings written to the log.
    audit: { targets: number, flagged: number, warnings: number },
    -- Sampled textures in memory: block-compressed and uncompressed.
    textures: { compressed_mb: number, uncompressed_mb: number },
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
    -- Bytes moved per render pass in the last frame and measured memory
    -- traffic where the machine has a counter.
    bandwidth: () -> PerfBandwidth,
}

-- The engine's loading screen, held by the game while it builds its own
-- scene: called before the first frame of a map (top of a script,
-- on.start, or before a map change), no frame of the scene shows until
-- release. The game keeps running behind it; the bar creeps toward 99%
-- and never reaches 100%.
declare loading: {
    -- Holds the loading screen; let go by itself after max_seconds
    -- (default 30) so a missing release never locks the game.
    hold: (max_seconds: number?) -> (),
    release: () -> (),
    held: () -> boolean,
    -- Pending loading work: (done, total, ready). done/total is the loading
    -- bar's fraction and never goes back. total counts the map plus every
    -- sprite sheet being prepared, neighbor map loading, texture waiting
    -- for upload and sound decoding seen since this load began. The map
    -- itself ends at 0.95; the rest is that pending work. On the first load
    -- with the project's loading screen the bar gives the boot 0-30% and
    -- the map 30-100%, so done/total is 0.3 + 0.7 * p there, as drawn.
    -- ready (and done/total = 1) only when nothing is pending; then it stays
    -- ready until the next map load (later streaming is the game running).
    progress: () -> (number, number, boolean),
    -- The game's own queue while the screen is held: done of total steps.
    -- The last part of the bar is the game's ("game_share" in the
    -- project's "loading" block, e.g. 0.3). Between calls the bar keeps
    -- moving at the pace of the steps so far, slowing down when a step
    -- runs long; it shows 100% on the frame of release.
    game_progress: (done: number, total: number) -> (),
}
)" + std::string(vfxApiDefinitions()) + visionApiDefinitions() + graphicsApiDefinitions() + objectsApiDefinitions() + telemetryApiDefinitions() +
           NativeModule::apiDefinitions();
}

} // namespace eruption
