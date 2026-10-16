// eulerAngleYXZ vem das extensoes "gtx" do GLM.
#define GLM_ENABLE_EXPERIMENTAL
#include "script/ScriptApi.hpp"

#include "script/ScriptHost.hpp"
#include "script/NativeModule.hpp"
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "game/PlayerController.hpp"
#include "renderer/UiCanvas.hpp"
#include "renderer/WeatherTypes.hpp"

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <lua.h>
#include <lualib.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

namespace eruption {

// Acesso aos membros internos do Engine que a API usa (Engine.hpp declara
// esta struct como amiga).
struct ScriptBridge {
    static Engine* engine;
    static std::vector<PointLight>& lights() { return engine->m_deferredLighting.getPointLights(); }
    static PlayerController* player() { return engine->m_playerController; }
    static void setWeather(WeatherType t) { engine->applyWeatherTypeFull(t, 1.0f); }
    // Só nesta execução (não grava data/char_config.json).
    static void setPlayerSprite(const std::string& path) {
        engine->m_charConfig.job = path;
        if (engine->m_playerController) engine->m_playerController->reloadSprite();
    }
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

// Os liquidos que o mapa declara: { name, kind ("water" | "lava"), level,
// center (vector na cota do liquido), radius (o maior raio da margem) }.
int world_liquids(lua_State* L) {
    lua_newtable(L);
    const auto* map = eng().currentMap();
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
int characters_exists(lua_State* L) {
    lua_pushboolean(L, eng().findCharacter(luaL_checkstring(L, 1)) >= 0);
    return 1;
}
int characters_position(lua_State* L) {
    pushVec3(L, checkCharacter(L, 1)->position);
    return 1;
}
int characters_set_position(lua_State* L) {
    Engine::SceneCharacter* c = checkCharacter(L, 1);
    c->position = checkVec3(L, 2);
    c->walking = false;
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
    lua_pushboolean(L, checkCharacter(L, 1)->walking);
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
    eng().camera().setOrbit(glm::radians(static_cast<float>(luaL_checknumber(L, 1))),
                            glm::radians(static_cast<float>(luaL_checknumber(L, 2))),
                            static_cast<float>(luaL_checknumber(L, 3)));
    return 0;
}

// ---------------------------------------------------------------- input

int input_down(lua_State* L) {
    const Key k = scriptKeyFromName(luaL_checkstring(L, 1));
    lua_pushboolean(L, k != Key::Unknown && Input::isKeyDown(k));
    return 1;
}
int input_pressed(lua_State* L) {
    const Key k = scriptKeyFromName(luaL_checkstring(L, 1));
    lua_pushboolean(L, k != Key::Unknown && Input::isKeyPressed(k));
    return 1;
}

// ---------------------------------------------------------------- models

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
int game_save(lua_State* L) {
    ScriptHost* host = ScriptHost::current();
    std::string error;
    const bool ok = host && host->saveGame(slotArg(L, 1), error);
    if (!ok) Logger::error("game.save: %s", error.c_str());
    lua_pushboolean(L, ok);
    return 1;
}
int game_load(lua_State* L) {
    ScriptHost* host = ScriptHost::current();
    std::string error;
    const bool ok = host && host->loadGame(slotArg(L, 1), error);
    if (!ok) Logger::error("game.load: %s", error.c_str());
    lua_pushboolean(L, ok);
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
int audio_play(lua_State* L) {
    const uint32_t id = eng().audio().play(luaL_checkstring(L, 1), float(luaL_optnumber(L, 2, 1.0)));
    lua_pushinteger(L, static_cast<int>(id));
    return 1;
}
int audio_play_at(lua_State* L) {
    const uint32_t id = eng().audio().playAt(luaL_checkstring(L, 1), soundPlace(L, 2), float(luaL_optnumber(L, 3, 1.0)),
                                             false, float(luaL_optnumber(L, 4, 300.0)));
    lua_pushinteger(L, static_cast<int>(id));
    return 1;
}
int audio_loop(lua_State* L) {
    const uint32_t id = eng().audio().play(luaL_checkstring(L, 1), float(luaL_optnumber(L, 2, 1.0)), true);
    lua_pushinteger(L, static_cast<int>(id));
    return 1;
}
int audio_loop_at(lua_State* L) {
    const uint32_t id = eng().audio().playAt(luaL_checkstring(L, 1), soundPlace(L, 2), float(luaL_optnumber(L, 3, 1.0)),
                                             true, float(luaL_optnumber(L, 4, 300.0)));
    lua_pushinteger(L, static_cast<int>(id));
    return 1;
}
int audio_stop(lua_State* L) {
    eng().audio().stop(static_cast<uint32_t>(luaL_checkinteger(L, 1)), float(luaL_optnumber(L, 2, 0.0)));
    return 0;
}
int audio_playing(lua_State* L) {
    lua_pushboolean(L, eng().audio().playing(static_cast<uint32_t>(luaL_checkinteger(L, 1))));
    return 1;
}
int audio_music(lua_State* L) {
    eng().audio().music(luaL_checkstring(L, 1), float(luaL_optnumber(L, 2, 1.0)), float(luaL_optnumber(L, 3, 1.0)));
    return 0;
}
int audio_stop_music(lua_State* L) {
    eng().audio().stopMusic(float(luaL_optnumber(L, 1, 1.0)));
    return 0;
}
int audio_set_volume(lua_State* L) {
    eng().audio().setMasterVolume(float(luaL_checknumber(L, 1)));
    return 0;
}

// ---------------------------------------------------------------- lights

int lights_count(lua_State* L) {
    lua_pushinteger(L, static_cast<int>(ScriptBridge::lights().size()));
    return 1;
}
PointLight& light(lua_State* L) {
    auto& ls = ScriptBridge::lights();
    return ls[checkIndex(L, 1, ls.size(), "light")];
}
int lights_position(lua_State* L) { pushVec3(L, light(L).position); return 1; }
int lights_set_position(lua_State* L) { light(L).position = checkVec3(L, 2); return 0; }
int lights_color(lua_State* L) { pushVec3(L, light(L).color); return 1; }
int lights_set_color(lua_State* L) { light(L).color = checkVec3(L, 2); return 0; }
int lights_intensity(lua_State* L) { lua_pushnumber(L, light(L).intensity); return 1; }
int lights_set_intensity(lua_State* L) {
    light(L).intensity = static_cast<float>(luaL_checknumber(L, 2));
    return 0;
}
int lights_enabled(lua_State* L) { lua_pushboolean(L, light(L).enabled); return 1; }
int lights_set_enabled(lua_State* L) { light(L).enabled = lua_toboolean(L, 2) != 0; return 0; }
int lights_turn_on(lua_State* L) { light(L).enabled = true; return 0; }
int lights_turn_off(lua_State* L) { light(L).enabled = false; return 0; }


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
    eng().uiCanvas().screenFade(optColor(L, 1, Vec4(1.0f)), static_cast<float>(luaL_optnumber(L, 3, 0.6)), 0.0f,
                                static_cast<float>(luaL_optnumber(L, 2, 0.18)));
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
int world_set_dof(lua_State* L) {
    eng().postSettings().enableDoF = lua_toboolean(L, 1) != 0;
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
        return 0;
    }
    const Vec3 p = checkVec3(L, 1);
    const Camera& c = eng().camera();
    // PROFUNDIDADE AO LONGO DO EIXO DA CAMERA, nao distancia em linha reta:
    // e' o que o mapa de profundidade guarda, e e' contra ele que o shader
    // de CoC compara. Com a distancia reta, um assunto fora do centro da
    // tela ficaria desfocado por estar "longe" quando esta' no mesmo plano.
    const Vec3 frente = glm::normalize(c.forward());
    s.focusDistance = glm::max(glm::dot(p - c.position(), frente), 0.01f);
    s.focusAperture = lua_isnoneornil(L, 2) ? -1.0f : static_cast<float>(luaL_checknumber(L, 2));
    s.focusForeground = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    s.focusOverride = true;
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
    static const luaL_Reg ui[] = {{"message", ui_message}, {"show_hud", ui_show_hud}, {"show_minimap", ui_show_minimap},
                                  {"load", ui_load}, {"text", ui_text}, {"value", ui_value}, {"image", ui_image},
                                  {"visible", ui_visible}, {"color", ui_color}, {"has", ui_has},
                                  {"reset", ui_reset}, {nullptr, nullptr}};
    static const luaL_Reg world[] = {
        {"time", world_time}, {"set_time", world_set_time}, {"time_between", world_time_between},
        {"weather", world_weather}, {"is_weather", world_is_weather}, {"set_weather", world_set_weather},
        {"look", world_look}, {"set_look", world_set_look}, {"set_dof", world_set_dof}, {"set_motion_blur", world_set_motion_blur},
        {"set_chromatic_aberration", world_set_chromatic_aberration}, {"set_effect_radius", world_set_effect_radius},
        {"set_focus", world_set_focus},
        {"ground_height", world_ground_height}, {"ground_layers", world_ground_layers}, {"liquids", world_liquids},
        {"block_slopes", world_block_slopes},
        {"map", world_map}, {"load_map", world_load_map},
        {"warp_map", world_warp_map}, {"warping", world_warping}, {nullptr, nullptr}};
    static const luaL_Reg player[] = {
        {"position", player_position}, {"set_position", player_set_position}, {"set_sprite", player_set_sprite}, {"near", player_near},
        {"inside", player_inside}, {"say", player_say}, {"play", player_play}, {"animation", player_animation},
        {"freeze", player_freeze}, {"frozen", player_frozen}, {"hide", player_hide},
        {nullptr, nullptr}};
    static const luaL_Reg camera[] = {
        {"target", camera_target}, {"set_target", camera_set_target},
        {"orbit", camera_orbit}, {"set_orbit", camera_set_orbit}, {"basis", camera_basis},
        {"view", camera_view},
        {"free", camera_free}, {"keep_inside_map", camera_keep_inside_map}, {nullptr, nullptr}};
    static const luaL_Reg fx[] = {
        {"shake_camera", fx_shake_camera}, {"number", fx_number}, {"fade", fx_fade}, {"flash", fx_flash},
        {"fading", fx_fading}, {nullptr, nullptr}};
    static const luaL_Reg input[] = {{"down", input_down}, {"pressed", input_pressed}, {nullptr, nullptr}};
    static const luaL_Reg models[] = {
        {"count", models_count}, {"find", models_find}, {"name", models_name},
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
        {"exists", characters_exists}, {"position", characters_position},
        {"set_position", characters_set_position}, {"walk_to", characters_walk_to},
        {"walking", characters_walking}, {"face", characters_face}, {"show", characters_show}, {"say", characters_say}, {"play", characters_play}, {"animation", characters_animation},
        {"follow", characters_follow}, {"patrol", characters_patrol}, {"stop", characters_stop},
        {"hide", characters_hide}, {"set_sprite", characters_set_sprite},
        {"add", characters_add},
        {"flash", characters_flash}, {"shake", characters_shake}, {"lunge", characters_lunge},
        {"pop", characters_pop}, {"fade", characters_fade}, {nullptr, nullptr}};
    static const luaL_Reg lights[] = {
        {"count", lights_count}, {"position", lights_position}, {"set_position", lights_set_position},
        {"color", lights_color}, {"set_color", lights_set_color}, {"intensity", lights_intensity},
        {"set_intensity", lights_set_intensity}, {"enabled", lights_enabled},
        {"set_enabled", lights_set_enabled}, {"turn_on", lights_turn_on}, {"turn_off", lights_turn_off},
        {nullptr, nullptr}};
    static const luaL_Reg audio[] = {
        {"play", audio_play}, {"play_at", audio_play_at}, {"loop", audio_loop}, {"loop_at", audio_loop_at},
        {"stop", audio_stop}, {"playing", audio_playing}, {"music", audio_music}, {"stop_music", audio_stop_music},
        {"set_volume", audio_set_volume}, {nullptr, nullptr}};
    lib(L, "on", on);
    lib(L, "audio", audio);
    static const luaL_Reg game[] = {{"save", game_save}, {"load", game_load}, {"has_save", game_has_save},
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
    lib(L, "characters", characters);
    lib(L, "fx", fx);
    registerVfxApi(L, engine);
    registerObjectsApi(L, engine);
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
    -- which: 0 = background / rail / text, 1 = bar fill or border.
    color: (selector: string, color: vector, which: number?, opacity: number?) -> (),
    has: (selector: string) -> boolean,
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
    -- at the liquid's level, radius is the farthest shore.
    liquids: () -> { { name: string, kind: string, level: number, center: vector, radius: number } },
    -- Blocks walking on every ground point steeper than `degrees` (this run
    -- only; the saved walk grid is untouched). Returns how many were blocked.
    block_slopes: (degrees: number) -> number,
    -- Scene look: a preset from data/diorama.json (color grading, god rays)
    -- and the depth-of-field blur. False if there is no preset by that name.
    look: () -> string,
    -- `preset` may also be a TABLE with the same fields as a preset (only
    -- the given ones change): a game's own look without a preset in the engine.
    set_look: (preset: string | { [string]: any }) -> boolean,
    set_dof: (on: boolean) -> (),
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
    -- `foreground` also blurs what sits IN FRONT of that point. No point at
    -- all gives the lens back to the automatic focus.
    set_focus: (point: vector?, aperture: number?, foreground: boolean?) -> (),
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
}

-- Saved games: map, player position, vars, time, weather and music, kept in the
-- player's data folder. Slots are names or numbers (default 1).
declare game: {
    save: (slot: (string | number)?) -> boolean,
    load: (slot: (string | number)?) -> boolean,
    has_save: (slot: (string | number)?) -> boolean,
    delete_save: (slot: (string | number)?) -> boolean,
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
    -- Hidden: the player's sprite is not drawn (a scene he is not in).
    hide: (on: boolean) -> (),
}

declare characters: {
    -- Puts a new character in the scene (sprite PNG or .spr of the project).
    -- False if the sprite could not be read.
    add: (name: string, sprite: string, position: vector, facing: number?) -> boolean,
    -- Characters placed in the scene (drag a PNG from the Project panel), by name.
    exists: (name: string) -> boolean,
    position: (name: string) -> vector,
    set_position: (name: string, position: vector) -> (),
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
    -- Flash: full opacity (or `strength`) fading to nothing.
    flash: (color: vector?, seconds: number?, strength: number?) -> (),
    fading: () -> boolean,
}


declare input: {
    down: (key: string) -> boolean,
    -- True only on the frame the key went down.
    pressed: (key: string) -> boolean,
}

-- Models placed in the map.
declare models: {
    count: () -> number,
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
    play: (file: string, volume: number?) -> number,
    -- At a point or model: quieter far away (`range` units, default 300), panned to its side.
    play_at: (file: string, place: vector | Model, volume: number?, range: number?) -> number,
    loop: (file: string, volume: number?) -> number,
    loop_at: (file: string, place: vector | Model, volume: number?, range: number?) -> number,
    stop: (sound: number, fade_seconds: number?) -> (),
    playing: (sound: number) -> boolean,
    -- One music at a time; a new one cross-fades (default 1 s).
    music: (file: string, volume: number?, fade_seconds: number?) -> (),
    stop_music: (fade_seconds: number?) -> (),
    -- Master volume, 1 = normal.
    set_volume: (volume: number) -> (),
}

-- Point lights of the map. Indices start at 1.
declare lights: {
    count: () -> number,
    position: (index: number) -> vector,
    set_position: (index: number, position: vector) -> (),
    -- Linear RGB, 0 to 1 per channel.
    color: (index: number) -> vector,
    set_color: (index: number, color: vector) -> (),
    intensity: (index: number) -> number,
    set_intensity: (index: number, intensity: number) -> (),
    enabled: (index: number) -> boolean,
    set_enabled: (index: number, enabled: boolean) -> (),
    turn_on: (index: number) -> (),
    turn_off: (index: number) -> (),
}
)" + std::string(vfxApiDefinitions()) + objectsApiDefinitions() + NativeModule::apiDefinitions();
}

} // namespace eruption
