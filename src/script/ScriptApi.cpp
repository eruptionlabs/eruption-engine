// eulerAngleYXZ vem das extensoes "gtx" do GLM.
#define GLM_ENABLE_EXPERIMENTAL
#include "script/ScriptApi.hpp"

#include "script/ScriptHost.hpp"
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "game/PlayerController.hpp"
#include "renderer/WeatherTypes.hpp"

#include <lua.h>
#include <lualib.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <cctype>
#include <cstring>
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
int world_ground_height(lua_State* L) {
    lua_pushnumber(L, eng().sampleTerrainHeight(static_cast<float>(luaL_checknumber(L, 1)),
                                                static_cast<float>(luaL_checknumber(L, 2))));
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
        {"near", on_near}, {"hour", on_hour}, {nullptr, nullptr}};
    static const luaL_Reg vars[] = {{"get", vars_get}, {"set", vars_set}, {"add", vars_add}, {nullptr, nullptr}};
    static const luaL_Reg ui[] = {{"message", ui_message}, {nullptr, nullptr}};
    static const luaL_Reg world[] = {
        {"time", world_time}, {"set_time", world_set_time}, {"time_between", world_time_between},
        {"weather", world_weather}, {"is_weather", world_is_weather}, {"set_weather", world_set_weather},
        {"ground_height", world_ground_height}, {nullptr, nullptr}};
    static const luaL_Reg player[] = {
        {"position", player_position}, {"set_position", player_set_position}, {"near", player_near},
        {nullptr, nullptr}};
    static const luaL_Reg camera[] = {
        {"target", camera_target}, {"set_target", camera_set_target},
        {"orbit", camera_orbit}, {"set_orbit", camera_set_orbit}, {nullptr, nullptr}};
    static const luaL_Reg input[] = {{"down", input_down}, {"pressed", input_pressed}, {nullptr, nullptr}};
    static const luaL_Reg models[] = {
        {"count", models_count}, {"find", models_find}, {"name", models_name},
        {"position", models_position}, {"set_position", models_set_position},
        {"set_rotation", models_set_rotation}, {"set_scale", models_set_scale},
        {"move_by", models_move_by}, {"rotate_by", models_rotate_by},
        {"visible", models_visible}, {"set_visible", models_set_visible},
        {"show", models_show}, {"hide", models_hide}, {nullptr, nullptr}};
    static const luaL_Reg lights[] = {
        {"count", lights_count}, {"position", lights_position}, {"set_position", lights_set_position},
        {"color", lights_color}, {"set_color", lights_set_color}, {"intensity", lights_intensity},
        {"set_intensity", lights_set_intensity}, {"enabled", lights_enabled},
        {"set_enabled", lights_set_enabled}, {"turn_on", lights_turn_on}, {"turn_off", lights_turn_off},
        {nullptr, nullptr}};
    lib(L, "on", on);
    lib(L, "vars", vars);
    lib(L, "ui", ui);
    lib(L, "world", world);
    lib(L, "player", player);
    lib(L, "camera", camera);
    lib(L, "input", input);
    lib(L, "models", models);
    lib(L, "lights", lights);
}

std::string scriptApiDefinitions() {
    return R"(-- Generated by Eruption Engine. Do not edit: rewritten on every start.

-- Prints to the editor console.
declare function log(...: any): ()

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
}

declare player: {
    position: () -> vector,
    set_position: (position: vector) -> (),
    near: (model: Model, distance: number) -> boolean,
}

declare camera: {
    target: () -> vector,
    set_target: (target: vector) -> (),
    -- Yaw and pitch in degrees, distance in world units.
    orbit: () -> (number, number, number),
    set_orbit: (yaw: number, pitch: number, distance: number) -> (),
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
)";
}

} // namespace eruption
