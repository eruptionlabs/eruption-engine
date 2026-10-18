// Tabela `objects` do Luau e o tipo Object: os objetos da cena (SceneObjects)
// vistos por script. Um Object é um handle: vale enquanto o objeto existe e,
// depois de destruído, toda leitura diz que ele se foi (alive() falso) em vez
// de apontar para quem ocupou a vaga.
#define GLM_ENABLE_EXPERIMENTAL
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "script/ScriptHost.hpp"

#include <lua.h>
#include <lualib.h>

#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace eruption {

namespace {

Engine* g_engine = nullptr;
// Criados por script: somem quando os scripts param ou recarregam (o Stop
// do editor volta a cena ao que era).
std::vector<ObjectId> g_spawned;

const char* const kObjectType = "Object";
const char* const kDataRegistry = "eruption.object_data";

SceneObjects& scene() { return g_engine->sceneObjects(); }
// Antes do primeiro quadro do mapa (on.start) os modelos ainda não têm objeto.
SceneObjects& freshScene() {
    g_engine->sceneObjects().refresh(*g_engine);
    return g_engine->sceneObjects();
}
ObjectWorld& world() { return g_engine->sceneObjects().world(); }

Vec3 vec3Arg(lua_State* L, int idx) {
    const float* v = luaL_checkvector(L, idx);
    return Vec3(v[0], v[1], v[2]);
}

void pushVec3(lua_State* L, const Vec3& v) { lua_pushvector(L, v.x, v.y, v.z); }

void pushObject(lua_State* L, ObjectId id) {
    if (!id.valid()) {
        lua_pushnil(L);
        return;
    }
    auto* ud = static_cast<ObjectId*>(lua_newuserdata(L, sizeof(ObjectId)));
    *ud = id;
    luaL_getmetatable(L, kObjectType);
    lua_setmetatable(L, -2);
}

ObjectId toObject(lua_State* L, int idx) {
    auto* ud = static_cast<ObjectId*>(luaL_checkudata(L, idx, kObjectType));
    return *ud;
}

// Handle de objeto vivo, ou erro com o nome da função.
ObjectId checkAlive(lua_State* L, int idx) {
    const ObjectId id = toObject(L, idx);
    if (!world().alive(id)) luaL_errorL(L, "the object was destroyed");
    return id;
}

// Objeto opcional (nil = nenhum), para pai.
ObjectId optObject(lua_State* L, int idx) {
    return lua_isnoneornil(L, idx) ? ObjectId{} : checkAlive(L, idx);
}

// Giro em graus por eixo, na mesma ordem das cenas (Y, depois X, depois Z).
Quat quatFromDegrees(const Vec3& deg) {
    return glm::quat_cast(glm::eulerAngleYXZ(glm::radians(deg.y), glm::radians(deg.x), glm::radians(deg.z)));
}

Vec3 degreesFromQuat(const Quat& q) {
    float y = 0.0f, x = 0.0f, z = 0.0f;
    glm::extractEulerAngleYXZ(glm::mat4_cast(q), y, x, z);
    return glm::degrees(Vec3(x, y, z));
}

void pushObjectList(lua_State* L, const std::vector<ObjectId>& ids) {
    lua_createtable(L, static_cast<int>(ids.size()), 0);
    int n = 0;
    for (const ObjectId& id : ids) {
        if (!id.valid()) continue;
        pushObject(L, id);
        lua_rawseti(L, -2, ++n);
    }
}

// Chave da tabela de dados do objeto: posição e geração num número só (a
// geração separa o objeto novo que ocupou a vaga do antigo).
double dataKey(ObjectId id) { return static_cast<double>(id.index) * 4294967296.0 + id.generation; }

void forgetData(lua_State* L, ObjectId id) {
    lua_getfield(L, LUA_REGISTRYINDEX, kDataRegistry);
    if (lua_istable(L, -1)) {
        lua_pushnumber(L, dataKey(id));
        lua_pushnil(L);
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);
}

// ---------------------------------------------------------------- métodos

bool isDead(const ObjectId& id) { return !world().alive(id); }

// Handle de objeto ja' destruido onde a API nao da' erro (destroy, has_tag):
// o script segue achando que fez algo. Comum com um handle guardado de antes
// de um game.load. Avisa uma vez por objeto, para nao inundar o log.
void warnDead(const ObjectId& id, const char* what) {
    static std::unordered_set<uint64_t> warned;
    const uint64_t key = (uint64_t(id.index) << 32) | id.generation;
    if (!warned.insert(key).second) return;
    ERUPTION_LOG_WARN("Script: %s on a destroyed object (#%u gen %u) was ignored; a handle kept from before "
                      "game.load or destroy?", what, id.index, id.generation);
}

// O objeto e os descendentes perdem a tabela de dados junto.
void forgetSubtree(lua_State* L, ObjectId id) {
    forgetData(L, id);
    for (const ObjectId& c : world().children(id)) forgetSubtree(L, c);
}

int object_destroy(lua_State* L) {
    const ObjectId id = toObject(L, 1);
    if (!world().alive(id)) {
        if (id.valid()) warnDead(id, "destroy()");
        return 0;
    }
    forgetSubtree(L, id);
    scene().destroy(*g_engine, id);
    // A lista dos criados por script não cresce com o que já morreu.
    g_spawned.erase(std::remove_if(g_spawned.begin(), g_spawned.end(), isDead), g_spawned.end());
    return 0;
}

int object_alive(lua_State* L) {
    lua_pushboolean(L, world().alive(toObject(L, 1)));
    return 1;
}

int object_children(lua_State* L) {
    pushObjectList(L, world().children(checkAlive(L, 1)));
    return 1;
}

int object_find(lua_State* L) {
    ObjectId at = checkAlive(L, 1);
    const std::string path = luaL_checkstring(L, 2);
    size_t start = 0;
    while (at.valid() && start <= path.size()) {
        const size_t slash = path.find('/', start);
        at = scene().childNamed(at, path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    pushObject(L, at);
    return 1;
}

int object_set_parent(lua_State* L) {
    const ObjectId id = checkAlive(L, 1);
    const ObjectId parent = optObject(L, 2);
    const bool keepWorld = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    if (!world().setParent(id, parent, keepWorld)) luaL_errorL(L, "an object cannot be inside itself");
    return 0;
}

int object_add_tag(lua_State* L) {
    world().addTag(checkAlive(L, 1), luaL_checkstring(L, 2));
    return 0;
}

int object_remove_tag(lua_State* L) {
    world().removeTag(checkAlive(L, 1), luaL_checkstring(L, 2));
    return 0;
}

int object_has_tag(lua_State* L) {
    const ObjectId id = toObject(L, 1);
    if (id.valid() && !world().alive(id)) warnDead(id, "has_tag()");
    lua_pushboolean(L, world().hasTag(id, luaL_checkstring(L, 2)));
    return 1;
}

int object_tags(lua_State* L) {
    const auto tags = world().tags(checkAlive(L, 1));
    lua_createtable(L, static_cast<int>(tags.size()), 0);
    for (size_t k = 0; k < tags.size(); ++k) {
        lua_pushstring(L, tags[k].c_str());
        lua_rawseti(L, -2, static_cast<int>(k + 1));
    }
    return 1;
}

// Script de objeto (ScriptHost::attachBehavior): o módulo devolve
// { start = function(self) ... end, update = function(self, dt) ... end }.
int object_attach(lua_State* L) {
    const ObjectId id = checkAlive(L, 1);
    const char* module = luaL_checkstring(L, 2);
    ScriptHost* host = ScriptHost::current();
    if (!host) luaL_errorL(L, "attach is only available while the game is running");
    std::string error;
    if (!host->attachBehavior(id, module, error)) luaL_errorL(L, "%s", error.c_str());
    return 0;
}

int object_move_by(lua_State* L) {
    const ObjectId id = checkAlive(L, 1);
    world().setWorldPosition(id, world().worldPosition(id) + vec3Arg(L, 2));
    return 0;
}

const luaL_Reg kMethods[] = {
    {"destroy", object_destroy},       {"alive", object_alive},         {"children", object_children},
    {"find", object_find},             {"set_parent", object_set_parent}, {"add_tag", object_add_tag},
    {"remove_tag", object_remove_tag}, {"has_tag", object_has_tag},     {"tags", object_tags},
    {"move_by", object_move_by},       {"attach", object_attach},      {nullptr, nullptr},
};

// ---------------------------------------------------------------- campos

// Tabela de dados do jogo presa ao objeto (vida, dono, estado): criada na
// primeira leitura, solta quando o objeto é destruído por script.
void pushData(lua_State* L, ObjectId id) {
    lua_getfield(L, LUA_REGISTRYINDEX, kDataRegistry);
    lua_pushnumber(L, dataKey(id));
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushnumber(L, dataKey(id));
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
}

int object_index(lua_State* L) {
    const ObjectId id = toObject(L, 1);
    const char* key = luaL_checkstring(L, 2);
    for (const luaL_Reg* m = kMethods; m->name; ++m) {
        if (std::strcmp(key, m->name) != 0) continue;
        lua_pushcfunction(L, m->func, m->name);
        return 1;
    }
    if (std::strcmp(key, "id") == 0) {
        lua_pushnumber(L, dataKey(id));
        return 1;
    }
    if (!world().alive(id)) luaL_errorL(L, "the object was destroyed (reading '%s')", key);
    const ObjectWorld::Transform& local = world().local(id);
    if (std::strcmp(key, "name") == 0) lua_pushstring(L, world().name(id).c_str());
    else if (std::strcmp(key, "position") == 0) pushVec3(L, world().worldPosition(id));
    else if (std::strcmp(key, "local_position") == 0) pushVec3(L, local.position);
    else if (std::strcmp(key, "rotation") == 0) pushVec3(L, degreesFromQuat(local.rotation));
    else if (std::strcmp(key, "scale") == 0) pushVec3(L, local.scale);
    else if (std::strcmp(key, "parent") == 0) pushObject(L, world().parent(id));
    else if (std::strcmp(key, "active") == 0) lua_pushboolean(L, world().active(id));
    else if (std::strcmp(key, "data") == 0) pushData(L, id);
    else if (std::strcmp(key, "model") == 0) {
        // Primeira instância (índice dos `models.*`, de 1), ou nil.
        const auto insts = scene().instancesOf(id);
        if (insts.empty()) lua_pushnil(L);
        else lua_pushinteger(L, static_cast<int>(insts.front()) + 1);
    } else if (std::strcmp(key, "light") == 0 || std::strcmp(key, "area") == 0 || std::strcmp(key, "sound") == 0) {
        // Luz: índice de lights.* (de 1); área e som: o nome.
        SceneObjects::PointKind kind;
        size_t index = 0;
        const bool is = scene().pointOf(id, kind, index);
        if (is && key[0] == 'l' && kind == SceneObjects::PointKind::Light) lua_pushinteger(L, static_cast<int>(index) + 1);
        else if (is && key[0] == 'a' && kind == SceneObjects::PointKind::Area)
            lua_pushstring(L, g_engine->areas()[index].name.c_str());
        else if (is && key[0] == 's' && kind == SceneObjects::PointKind::Sound)
            lua_pushstring(L, g_engine->soundEmitters()[index].name.c_str());
        else
            lua_pushnil(L);
    } else if (std::strcmp(key, "character") == 0) {
        const int c = scene().characterOf(id);
        if (c < 0) lua_pushnil(L);
        else lua_pushstring(L, g_engine->characters()[static_cast<size_t>(c)].name.c_str());
    } else
        luaL_errorL(L, "Object has no field '%s'", key);
    return 1;
}

int object_newindex(lua_State* L) {
    const ObjectId id = checkAlive(L, 1);
    const char* key = luaL_checkstring(L, 2);
    ObjectWorld& w = world();
    if (std::strcmp(key, "name") == 0) w.rename(id, luaL_checkstring(L, 3));
    else if (std::strcmp(key, "position") == 0) w.setWorldPosition(id, vec3Arg(L, 3));
    else if (std::strcmp(key, "local_position") == 0) w.setLocalPosition(id, vec3Arg(L, 3));
    else if (std::strcmp(key, "rotation") == 0) w.setLocalRotation(id, quatFromDegrees(vec3Arg(L, 3)));
    else if (std::strcmp(key, "scale") == 0)
        w.setLocalScale(id, lua_isnumber(L, 3) ? Vec3(static_cast<float>(lua_tonumber(L, 3))) : vec3Arg(L, 3));
    else if (std::strcmp(key, "parent") == 0) {
        if (!w.setParent(id, optObject(L, 3), true)) luaL_errorL(L, "an object cannot be inside itself");
    } else if (std::strcmp(key, "active") == 0) w.setActive(id, lua_toboolean(L, 3) != 0);
    else
        luaL_errorL(L, "Object field '%s' cannot be set", key);
    return 0;
}

int object_eq(lua_State* L) {
    lua_pushboolean(L, toObject(L, 1) == toObject(L, 2));
    return 1;
}

int object_tostring(lua_State* L) {
    const ObjectId id = toObject(L, 1);
    if (!world().alive(id)) lua_pushstring(L, "Object(destroyed)");
    else lua_pushfstring(L, "Object(%s)", world().name(id).c_str());
    return 1;
}

// ---------------------------------------------------------------- objects.*

ObjectWorld::Transform placeAt(lua_State* L, int idx, ObjectId parent) {
    ObjectWorld::Transform t;
    if (lua_isnoneornil(L, idx)) return t;
    const Vec3 p = vec3Arg(L, idx);
    // Posição dada é de mundo, como em todo o resto da API.
    t.position = parent.valid() ? Vec3(glm::inverse(world().world(parent)) * Vec4(p, 1.0f)) : p;
    return t;
}

int objects_find(lua_State* L) {
    pushObject(L, freshScene().findPath(luaL_checkstring(L, 1)));
    return 1;
}

int objects_find_all(lua_State* L) {
    pushObjectList(L, freshScene().world().findAll(luaL_checkstring(L, 1)));
    return 1;
}

int objects_with_tag(lua_State* L) {
    pushObjectList(L, freshScene().world().findByTag(luaL_checkstring(L, 1)));
    return 1;
}

int objects_create(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    // O mapa ganha objetos antes (senão o primeiro sync recomeça a tabela e
    // leva junto este, criado cedo demais - on.start).
    freshScene();
    const ObjectId parent = optObject(L, 3);
    const ObjectId id = world().create(name, parent, placeAt(L, 2, parent));
    scene().markMade(id);
    g_spawned.push_back(id);
    pushObject(L, id);
    return 1;
}

int objects_spawn(lua_State* L) {
    ScriptHost::NativeWork work; // carregar o modelo não conta como tempo do script
    const std::string source = luaL_checkstring(L, 1);
    const ObjectId parent = optObject(L, 3);
    std::string name = luaL_optstring(L, 4, "");
    if (name.empty()) name = SceneObjects::defaultName(source);
    std::string error;
    std::vector<SceneObjects::ObjectScript> scripts;
    const ObjectId id = scene().spawn(*g_engine, source, name, parent, placeAt(L, 2, parent), &error, &scripts);
    if (!id.valid()) {
        Logger::warning("objects.spawn: %s", error.c_str());
        lua_pushnil(L);
        return 1;
    }
    g_spawned.push_back(id);
    // Scripts que o prefab pede (na raiz e nos filhos).
    if (ScriptHost* host = ScriptHost::current())
        for (const auto& [object, module] : scripts)
            if (!host->attachBehavior(object, module, error)) Logger::warning("objects.spawn: %s", error.c_str());
    pushObject(L, id);
    return 1;
}

int objects_of_model(lua_State* L) {
    const int model = luaL_checkinteger(L, 1);
    pushObject(L, model >= 1 ? freshScene().objectOfInstance(static_cast<uint32_t>(model - 1)) : ObjectId{});
    return 1;
}

int objects_count(lua_State* L) {
    lua_pushinteger(L, static_cast<int>(freshScene().world().count()));
    return 1;
}

void registerObjectType(lua_State* L) {
    luaL_newmetatable(L, kObjectType);
    lua_pushcfunction(L, object_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, object_newindex, "__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, object_eq, "__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, object_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, kObjectType);
    lua_setfield(L, -2, "__type");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kDataRegistry);
}

} // namespace

void scriptPushObject(lua_State* L, ObjectId id) { pushObject(L, id); }

bool scriptToObject(lua_State* L, int idx, ObjectId& out) {
    if (lua_type(L, idx) != LUA_TUSERDATA || !lua_getmetatable(L, idx)) return false;
    luaL_getmetatable(L, kObjectType);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    if (same) out = *static_cast<ObjectId*>(lua_touserdata(L, idx));
    return same;
}

std::vector<ObjectId> scriptObjects() {
    std::vector<ObjectId> out;
    for (const ObjectId& id : g_spawned)
        if (g_engine && world().alive(id)) out.push_back(id);
    return out;
}

void scriptAdoptObject(ObjectId id) { g_spawned.push_back(id); }

std::vector<ObjectId> scriptObjectsWithData(lua_State* L) {
    std::vector<ObjectId> out;
    lua_getfield(L, LUA_REGISTRYINDEX, kDataRegistry);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TNUMBER) {
                const double key = lua_tonumber(L, -2);
                const auto index = static_cast<uint32_t>(key / 4294967296.0);
                const auto generation = static_cast<uint32_t>(key - static_cast<double>(index) * 4294967296.0);
                const ObjectId id{index, generation};
                if (world().alive(id)) out.push_back(id);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return out;
}

bool scriptPushObjectDataIfAny(lua_State* L, ObjectId id) {
    lua_getfield(L, LUA_REGISTRYINDEX, kDataRegistry);
    lua_pushnumber(L, dataKey(id));
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

void resetScriptObjectData(lua_State* L) {
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kDataRegistry);
}

void scriptSetObjectData(lua_State* L, ObjectId id) {
    lua_getfield(L, LUA_REGISTRYINDEX, kDataRegistry);
    lua_pushnumber(L, dataKey(id));
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 2);
}

void releaseScriptObjects() {
    if (!g_engine) return;
    // Tudo que o jogo fez, inclusive filhos de prefab que ele tirou do lugar.
    scene().destroyMade(*g_engine);
    g_spawned.clear();
}

void registerObjectsApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    releaseScriptObjects();
    registerObjectType(L);
    static const luaL_Reg fns[] = {{"find", objects_find},
                                   {"find_all", objects_find_all},
                                   {"with_tag", objects_with_tag},
                                   {"create", objects_create},
                                   {"spawn", objects_spawn},
                                   {"of_model", objects_of_model},
                                   {"count", objects_count},
                                   {nullptr, nullptr}};
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "objects");
}

const char* objectsApiDefinitions() {
    return R"(
-- An object of the scene: every model and character of the map is one, and
-- scripts make more. The handle stays valid until the object is destroyed;
-- after that alive() is false and reading a field is an error. game.load
-- builds the objects again: look them up by name after loading (handles kept
-- in vars and in obj.data come back on their own).
-- Positions are in the world; local_position, rotation and scale are relative
-- to the parent. Rotation is in degrees around X, Y and Z.
declare class Object
    name: string
    position: vector
    local_position: vector
    rotation: vector
    scale: vector
    parent: Object?
    -- Off hides the object and everything under it.
    active: boolean
    -- The game's own table for this object (life, owner, state).
    data: {[any]: any}
    -- Index of its model for models.* (from 1), or nil.
    model: number?
    -- Name of its character for characters.*, or nil.
    character: string?
    -- Index of its light for lights.* (from 1), or nil. Lights are objects
    -- named "light_1", "light_2"...: put one inside a model and it follows;
    -- turning the object off puts the light out.
    light: number?
    -- Name of its trigger area / sound emitter, or nil.
    area: string?
    sound: string?
    -- A number unique to this object while the game runs (a table key).
    -- It changes after game.load; to keep a reference, store the Object.
    id: number
    function destroy(self): ()
    function alive(self): boolean
    function children(self): {Object}
    -- A child by name; "door/handle" goes down the hierarchy.
    function find(self, name: string): Object?
    -- keep_world (default true): stays where it is on screen.
    function set_parent(self, parent: Object?, keep_world: boolean?): ()
    function move_by(self, offset: vector): ()
    -- Gives the object a script: assets/scripts/<module>.luau returns
    -- { start = function(self) end, update = function(self, dt) end,
    --   stop = function(self) end } (all optional). Keep the object's own
    -- state in self.data: saving the file swaps the code, not the state.
    function attach(self, module: string): ()
    function add_tag(self, tag: string): ()
    function remove_tag(self, tag: string): ()
    function has_tag(self, tag: string): boolean
    function tags(self): {string}
end

declare objects: {
    -- By name; "house/door" goes down the hierarchy.
    find: (name: string) -> Object?,
    find_all: (name: string) -> {Object},
    with_tag: (tag: string) -> {Object},
    -- An empty object (a group, a point to aim at).
    create: (name: string, position: vector?, parent: Object?) -> Object,
    -- An object with a model: a .glb ("file.glb#Node" takes one node) or a
    -- .prefab.luau. Destroyed objects give their models back for the next
    -- spawn of the same file. Objects made by scripts go away on Stop.
    spawn: (file: string, position: vector?, parent: Object?, name: string?) -> Object?,
    -- The object of a models.* index.
    of_model: (model: number) -> Object?,
    count: () -> number,
}
)";
}

} // namespace eruption
