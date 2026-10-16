#define GLM_ENABLE_EXPERIMENTAL
#include "scene/SceneEdits.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "script/ScriptApi.hpp"

#include <lua.h>
#include <lualib.h>
#include "script/LuauChunk.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

std::string num(float v) {
    if (std::abs(v - std::round(v)) < 1e-4f) return std::to_string(static_cast<long long>(std::lround(v)));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

std::string vec(const Vec3& v) { return "{" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + "}"; }

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string quotedList(const std::vector<std::string>& items) {
    std::string out = "{";
    for (size_t k = 0; k < items.size(); ++k) out += (k ? ", " : "") + quoted(items[k]);
    return out + "}";
}

// Leitura da tabela devolvida pelo arquivo (pilha do Luau).
bool readVec(lua_State* L, int idx, const char* field, Vec3& out) {
    lua_getfield(L, idx, field);
    bool ok = false;
    if (lua_istable(L, -1)) {
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, -1, i + 1);
            out[i] = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

float readNum(lua_State* L, int idx, const char* field, float def) {
    lua_getfield(L, idx, field);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}

bool readBool(lua_State* L, int idx, const char* field, bool def) {
    lua_getfield(L, idx, field);
    const bool v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) != 0 : def;
    lua_pop(L, 1);
    return v;
}

std::string readStr(lua_State* L, int idx, const char* field) {
    lua_getfield(L, idx, field);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
}

// Lista de textos: { "a", "b" } (um texto solto também vale).
std::vector<std::string> readStrList(lua_State* L, int idx, const char* field) {
    std::vector<std::string> out;
    lua_getfield(L, idx, field);
    if (lua_isstring(L, -1)) out.push_back(lua_tostring(L, -1));
    else if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1)) out.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return out;
}

SceneEdits::Transform readTransform(lua_State* L, int idx, bool& any) {
    SceneEdits::Transform t;
    any = readVec(L, idx, "position", t.position);
    any |= readVec(L, idx, "rotation", t.rotation);
    any |= readVec(L, idx, "scale", t.scale);
    return t;
}

} // namespace

Mat4 SceneEdits::Transform::matrix() const {
    Mat4 m = glm::translate(Mat4(1.0f), position);
    m *= glm::eulerAngleYXZ(glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z));
    return glm::scale(m, scale);
}

SceneEdits::Transform SceneEdits::Transform::from(const Mat4& m) {
    Transform t;
    t.position = Vec3(m[3]);
    t.scale = Vec3(glm::length(Vec3(m[0])), glm::length(Vec3(m[1])), glm::length(Vec3(m[2])));
    Mat4 r(1.0f);
    for (int a = 0; a < 3; ++a) r[a] = Vec4(Vec3(m[a]) / std::max(1e-6f, t.scale[a]), 0.0f);
    float y = 0, x = 0, z = 0;
    glm::extractEulerAngleYXZ(r, y, x, z);
    t.rotation = glm::degrees(Vec3(x, y, z));
    return t;
}

bool SceneEdits::addModel(Engine& engine, const std::string& file, const std::string& node, const std::string& name,
                          const Mat4& root, const Mat4& inner, AddedInstance& out) {
    ModelRenderer& mr = engine.modelRenderer();
    const int first = engine.addModelFromFile(file, Vec3(0.0f), node);
    if (first < 0) return false;
    out = AddedInstance{};
    out.file = file;
    out.name = name;
    out.inner = inner;
    out.first = static_cast<uint32_t>(first);
    out.count = static_cast<uint32_t>(mr.getInstances().size()) - out.first;
    // Centro da base do conjunto vai para a origem: um nó tirado do mapa (em
    // coordenada de mundo) e um .glb solto se comportam igual.
    Vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (uint32_t k = 0; k < out.count; ++k) {
        const ModelInstance& inst = mr.getInstances()[out.first + k];
        lo = glm::min(lo, inst.worldAabbMin);
        hi = glm::max(hi, inst.worldAabbMax);
    }
    const Mat4 center = glm::translate(Mat4(1.0f), -Vec3((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f));
    for (uint32_t k = 0; k < out.count; ++k) {
        const uint32_t idx = out.first + k;
        const ModelInstance& inst = mr.getInstances()[idx];
        out.local.push_back(inner * center * inst.transform);
        mr.setInstanceName(idx, out.count == 1 ? name : name + "/" + inst.name);
        mr.setInstanceTransform(idx, root * out.local.back());
    }
    return true;
}

fs::path SceneEdits::pathFor(const std::string& map) {
    return project::game("assets/scenes") / (map + ".scene.luau");
}

bool SceneEdits::fileMentions(const std::string& map, const char* word) {
    std::ifstream f(pathFor(map), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str().find(word) != std::string::npos;
}

bool SceneEdits::load(const fs::path& file, std::string& error) {
    *this = SceneEdits{};
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    std::ifstream f(file, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_sandbox(L);
    bool ok = loadLuauChunk(L, "@" + file.generic_string(), src) && lua_pcall(L, 0, 1, 0) == 0;
    if (!ok || !lua_istable(L, -1)) {
        error = ok ? file.generic_string() + ": the file must return a table" : lua_tostring(L, -1);
        lua_close(L);
        return false;
    }
    const int root = lua_gettop(L);

    lua_getfield(L, root, "objects");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
                Object o;
                o.name = lua_tostring(L, -2);
                o.transform = readTransform(L, lua_gettop(L), o.hasTransform);
                o.hidden = readBool(L, lua_gettop(L), "hidden", false);
                o.solid = readBool(L, lua_gettop(L), "solid", false);
                o.animation = readStr(L, lua_gettop(L), "animation");
                o.parent = readStr(L, lua_gettop(L), "parent");
                o.tags = readStrList(L, lua_gettop(L), "tags");
                o.scripts = readStrList(L, lua_gettop(L), "scripts");
                objects.push_back(std::move(o));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "added");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                Added a;
                bool any = false;
                a.file = readStr(L, lua_gettop(L), "file");
                a.node = readStr(L, lua_gettop(L), "node");
                a.prefab = readStr(L, lua_gettop(L), "prefab");
                a.name = readStr(L, lua_gettop(L), "name");
                a.transform = readTransform(L, lua_gettop(L), any);
                if ((!a.file.empty() || !a.prefab.empty()) && !a.name.empty()) added.push_back(std::move(a));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "copies");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                Copy c;
                bool any = false;
                c.from = readStr(L, lua_gettop(L), "from");
                c.name = readStr(L, lua_gettop(L), "name");
                c.transform = readTransform(L, lua_gettop(L), any);
                if (!c.from.empty()) copies.push_back(std::move(c));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "lights");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_isnumber(L, -2) && lua_istable(L, -1)) {
                Light l;
                const int t = lua_gettop(L);
                l.index = static_cast<int>(lua_tonumber(L, -2));
                readVec(L, t, "position", l.position);
                readVec(L, t, "color", l.color);
                l.intensity = readNum(L, t, "intensity", 1.0f);
                l.range = readNum(L, t, "range", 10.0f);
                l.enabled = readBool(L, t, "enabled", true);
                lights.push_back(l);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "characters");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                const int t = lua_gettop(L);
                Character c;
                c.name = readStr(L, t, "name");
                c.sprite = readStr(L, t, "sprite");
                readVec(L, t, "position", c.position);
                c.facing = readNum(L, t, "facing", 0.0f);
                c.hidden = readBool(L, t, "hidden", false);
                if (!c.name.empty() && !c.sprite.empty()) characters.push_back(std::move(c));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    hasSpawn = readVec(L, root, "spawn", spawn);

    lua_getfield(L, root, "terrain");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "layers");
        if (lua_istable(L, -1)) {
            const int n = lua_objlen(L, -1);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_isstring(L, -1)) terrainLayers.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "areas");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            const int t = lua_gettop(L);
            if (lua_istable(L, t)) {
                Area a;
                a.name = readStr(L, t, "name");
                readVec(L, t, "position", a.position);
                readVec(L, t, "size", a.size);
                a.rotation = readNum(L, t, "rotation", 0.0f);
                if (!a.name.empty()) areas.push_back(std::move(a));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "sounds");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            const int t = lua_gettop(L);
            if (lua_istable(L, t)) {
                Sound snd;
                snd.name = readStr(L, t, "name");
                snd.file = readStr(L, t, "file");
                readVec(L, t, "position", snd.position);
                snd.volume = readNum(L, t, "volume", 1.0f);
                snd.range = readNum(L, t, "range", 200.0f);
                if (!snd.name.empty()) sounds.push_back(std::move(snd));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "neighbors");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            const int t = lua_gettop(L);
            if (lua_istable(L, t)) {
                Neighbor nb;
                nb.map = readStr(L, t, "map");
                readVec(L, t, "offset", nb.offset);
                if (!nb.map.empty()) neighbors.push_back(std::move(nb));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root, "environment");
    if (lua_istable(L, -1)) {
        const int t = lua_gettop(L);
        hasEnvironment = true;
        time = readNum(L, t, "time", 12.0f);
        paused = readBool(L, t, "paused", false);
        weather = readStr(L, t, "weather");
        lua_getfield(L, t, "water");
        if (lua_isnumber(L, -1)) {
            hasWater = true;
            waterLevel = float(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_close(L);
    return true;
}

std::string SceneEdits::serialize(const std::string& map) const {
    std::ostringstream o;
    o << "-- Edits of the map \"" << map << "\", written by the editor (Ctrl+S).\n"
         "-- Rotation in degrees. Names that no longer exist in the map are ignored.\n"
         "return {\n";
    if (!added.empty()) {
        o << "    added = {\n";
        for (const auto& a : added) {
            o << "        { ";
            if (!a.prefab.empty()) {
                o << "prefab = " << quoted(a.prefab);
            } else {
                o << "file = " << quoted(a.file);
                if (!a.node.empty()) o << ", node = " << quoted(a.node);
            }
            o << ", name = " << quoted(a.name) << ", position = " << vec(a.transform.position)
              << ", rotation = " << vec(a.transform.rotation) << ", scale = " << vec(a.transform.scale) << " },\n";
        }
        o << "    },\n";
    }
    if (!objects.empty()) {
        o << "    objects = {\n";
        for (const auto& ob : objects) {
            std::vector<std::string> parts;
            if (ob.hidden) parts.push_back("hidden = true");
            if (ob.solid) parts.push_back("solid = true");
            if (!ob.animation.empty()) parts.push_back("animation = " + quoted(ob.animation));
            if (!ob.parent.empty()) parts.push_back("parent = " + quoted(ob.parent));
            if (!ob.tags.empty()) parts.push_back("tags = " + quotedList(ob.tags));
            if (!ob.scripts.empty()) parts.push_back("scripts = " + quotedList(ob.scripts));
            if (ob.hasTransform)
                parts.push_back("position = " + vec(ob.transform.position) + ", rotation = " + vec(ob.transform.rotation) +
                                ", scale = " + vec(ob.transform.scale));
            o << "        [" << quoted(ob.name) << "] = { ";
            for (size_t k = 0; k < parts.size(); ++k) o << (k ? ", " : "") << parts[k];
            o << " },\n";
        }
        o << "    },\n";
    }
    if (!copies.empty()) {
        o << "    copies = {\n";
        for (const auto& c : copies)
            o << "        { from = " << quoted(c.from) << ", name = " << quoted(c.name) << ", position = "
              << vec(c.transform.position) << ", rotation = " << vec(c.transform.rotation) << ", scale = "
              << vec(c.transform.scale) << " },\n";
        o << "    },\n";
    }
    if (!lights.empty()) {
        o << "    lights = {\n";
        for (const auto& l : lights)
            o << "        [" << l.index << "] = { position = " << vec(l.position) << ", color = " << vec(l.color)
              << ", intensity = " << num(l.intensity) << ", range = " << num(l.range)
              << ", enabled = " << (l.enabled ? "true" : "false") << " },\n";
        o << "    },\n";
    }
    if (!characters.empty()) {
        o << "    characters = {\n";
        for (const auto& c : characters) {
            o << "        { name = " << quoted(c.name) << ", sprite = " << quoted(c.sprite) << ", position = "
              << vec(c.position) << ", facing = " << num(c.facing);
            if (c.hidden) o << ", hidden = true";
            o << " },\n";
        }
        o << "    },\n";
    }
    if (hasSpawn) o << "    spawn = " << vec(spawn) << ",\n";
    if (!terrainLayers.empty()) {
        o << "    terrain = { layers = {";
        for (size_t i = 0; i < terrainLayers.size(); ++i) o << (i ? ", " : " ") << quoted(terrainLayers[i]);
        o << " } },\n";
    }
    if (!areas.empty()) {
        o << "    areas = {\n";
        for (const auto& a : areas)
            o << "        { name = " << quoted(a.name) << ", position = " << vec(a.position) << ", size = " << vec(a.size)
              << ", rotation = " << num(a.rotation) << " },\n";
        o << "    },\n";
    }
    if (!sounds.empty()) {
        o << "    sounds = {\n";
        for (const auto& snd : sounds)
            o << "        { name = " << quoted(snd.name) << ", file = " << quoted(snd.file) << ", position = "
              << vec(snd.position) << ", volume = " << num(snd.volume) << ", range = " << num(snd.range) << " },\n";
        o << "    },\n";
    }
    if (!neighbors.empty()) {
        o << "    neighbors = {\n";
        for (const auto& nb : neighbors)
            o << "        { map = " << quoted(nb.map) << ", offset = " << vec(nb.offset) << " },\n";
        o << "    },\n";
    }
    if (hasEnvironment) {
        o << "    environment = { time = " << num(time) << ", paused = " << (paused ? "true" : "false");
        if (!weather.empty()) o << ", weather = " << quoted(weather);
        if (hasWater) o << ", water = " << num(waterLevel);
        o << " },\n";
    }
    o << "}\n";
    return o.str();
}

int SceneEdits::apply(Engine& engine, std::vector<AddedInstance>* addedOut) const {
    ModelRenderer& mr = engine.modelRenderer();
    int applied = 0;
    struct PrefabRoot {
        uint32_t first, count;
        std::string name, prefab;
        Mat4 placement;
    };
    std::vector<PrefabRoot> prefabRoots;
    for (const auto& a : added) {
        if (scriptFindModel(a.name) >= 0) continue;
        std::string file = a.file, node = a.node;
        Mat4 inner(1.0f);
        if (!a.prefab.empty()) {
            Prefab p;
            std::string error;
            if (!p.load(a.prefab, error)) {
                ERUPTION_LOG_ERROR("%s", error.c_str());
                continue;
            }
            file = p.file;
            node = p.node;
            inner = p.matrix();
            // Prefab sem modelo próprio (raiz só grupo) não se põe na cena: o
            // editor guarda o que foi posto pelas instâncias da raiz.
            if (file.empty()) {
                ERUPTION_LOG_WARN("%s: a prefab without its own model is placed by scripts (objects.spawn)",
                                  a.prefab.c_str());
                continue;
            }
        }
        AddedInstance inst;
        if (!addModel(engine, file, node, a.name, a.transform.matrix(), inner, inst)) continue;
        inst.node = node;
        inst.prefab = a.prefab;
        if (!a.prefab.empty()) prefabRoots.push_back({inst.first, inst.count, a.name, a.prefab, a.transform.matrix()});
        if (addedOut) addedOut->push_back(std::move(inst));
        ++applied;
    }
    // Prefab composto: filhos, etiquetas e scripts sob o objeto da raiz.
    if (!prefabRoots.empty()) {
        SceneObjects& so = engine.sceneObjects();
        so.refresh(engine);
        for (const PrefabRoot& r : prefabRoots) {
            const ObjectId root = r.count == 1 ? so.objectOfInstance(r.first) : so.findSceneName(r.name);
            so.buildPrefabExtras(engine, root, r.placement, r.prefab);
        }
    }
    for (const auto& ob : objects) {
        const int idx = scriptFindModel(ob.name);
        if (idx < 0) continue;
        if (ob.hasTransform) mr.setInstanceTransform(static_cast<uint32_t>(idx), ob.transform.matrix());
        if (ob.hidden) mr.setInstanceEnabled(static_cast<uint32_t>(idx), false);
        ++applied;
    }
    for (const auto& a : areas) {
        if (engine.findArea(a.name) >= 0) continue;
        engine.addArea(a.name, a.position, a.size, a.rotation);
        ++applied;
    }
    for (const auto& snd : sounds) {
        if (engine.findSoundEmitter(snd.name) >= 0) continue;
        engine.addSoundEmitter(snd.name, snd.file, snd.position, snd.volume, snd.range);
        ++applied;
    }
    for (const auto& c : copies) {
        const int src = scriptFindModel(c.from);
        if (src < 0 || scriptFindModel(c.name) >= 0) continue;
        mr.duplicateInstance(static_cast<uint32_t>(src), c.name, c.transform.matrix());
        ++applied;
    }
    // Sólido depois das cópias (uma cópia também pode ser sólida). O nome de um
    // modelo acrescentado com vários nós vale para todos ("muro" -> "muro/Box"...).
    for (const auto& ob : objects) {
        if (!ob.solid) continue;
        const int idx = scriptFindModel(ob.name);
        if (idx >= 0) {
            engine.setSolid(static_cast<uint32_t>(idx), true);
            continue;
        }
        const auto& insts = mr.getInstances();
        for (uint32_t i = 0; i < insts.size(); ++i)
            if (insts[i].name.rfind(ob.name + "/", 0) == 0) engine.setSolid(i, true);
    }
    // Animação inicial (modelo com esqueleto), também nos nós "nome/...".
    for (const auto& ob : objects) {
        if (ob.animation.empty()) continue;
        const auto& insts = mr.getInstances();
        for (uint32_t i = 0; i < insts.size(); ++i)
            if (insts[i].name == ob.name || insts[i].name.rfind(ob.name + "/", 0) == 0) mr.playAnimation(i, ob.animation);
    }
    auto& ls = engine.deferredLighting().getPointLights();
    for (const auto& l : lights) {
        if (l.index < 1 || l.index > 1024) continue;
        // Índice além das luzes do mapa: luz criada no editor.
        if (l.index > static_cast<int>(ls.size())) ls.resize(static_cast<size_t>(l.index));
        PointLight& p = ls[static_cast<size_t>(l.index - 1)];
        p.position = l.position;
        p.color = l.color;
        p.intensity = l.intensity;
        p.radius = l.range;
        p.enabled = l.enabled;
        ++applied;
    }
    for (const auto& c : characters) {
        if (engine.findCharacter(c.name) >= 0) continue;
        const int idx = engine.addCharacter(c.name, c.sprite, c.position, c.facing);
        engine.characters()[static_cast<size_t>(idx)].visible = !c.hidden;
        ++applied;
    }
    if (hasSpawn) {
        engine.setPlayerStart(spawn);
        ++applied;
    }
    const auto present = engine.splatLayerFiles();
    for (const auto& layer : terrainLayers) {
        if (std::find(present.begin(), present.end(), layer) != present.end()) continue; // recarga da cena
        if (engine.splatLayerCount() >= 8) break;
        if (engine.addSplatLayer(layer)) ++applied;
        else ERUPTION_LOG_WARN("Terrain layer %s could not be loaded", layer.c_str());
    }
    if (hasEnvironment) {
        engine.dayNightCycle().setTimeOfDay(time / 24.0f);
        engine.dayNightCycle().setPaused(paused);
        if (!weather.empty()) engine.setInitialWeatherType(weather);
        if (hasWater) engine.setWaterLevel(true, waterLevel);
        ++applied;
    }
    // Objetos da cena: etiquetas, pai e scripts. Por último, quando tudo que
    // a cena cria já existe (e já tem objeto).
    for (const auto& ob : objects) {
        if (!ob.decorated()) continue;
        engine.sceneObjects().author(engine, {ob.name, ob.parent, ob.tags, ob.scripts});
        ++applied;
    }
    return applied;
}

Mat4 Prefab::matrix() const {
    SceneEdits::Transform t;
    t.rotation = rotation;
    t.scale = scale;
    return t.matrix();
}

bool Prefab::isPrefabPath(const fs::path& p) {
    const std::string n = p.filename().string();
    static const std::string kExt = ".prefab.luau";
    return n.size() > kExt.size() && n.compare(n.size() - kExt.size(), kExt.size(), kExt) == 0;
}

bool Prefab::load(const fs::path& path, std::string& error) {
    *this = Prefab{};
    std::ifstream f(project::gameRef(path.string()), std::ios::binary);
    if (!f) {
        error = "Prefab not found: " + path.generic_string();
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_sandbox(L);
    const bool ok = loadLuauChunk(L, "@" + path.generic_string(), src) && lua_pcall(L, 0, 1, 0) == 0;
    if (!ok || !lua_istable(L, -1)) {
        error = ok ? path.generic_string() + ": the file must return a table" : lua_tostring(L, -1);
        lua_close(L);
        return false;
    }
    const int t = lua_gettop(L);
    file = readStr(L, t, "file");
    node = readStr(L, t, "node");
    readVec(L, t, "rotation", rotation);
    readVec(L, t, "scale", scale);
    tags = readStrList(L, t, "tags");
    scripts = readStrList(L, t, "scripts");
    lua_getfield(L, t, "children");
    if (lua_istable(L, -1)) {
        const int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            const int c = lua_gettop(L);
            if (lua_istable(L, c)) {
                Child ch;
                ch.prefab = readStr(L, c, "prefab");
                ch.file = readStr(L, c, "file");
                ch.node = readStr(L, c, "node");
                ch.name = readStr(L, c, "name");
                readVec(L, c, "position", ch.position);
                readVec(L, c, "rotation", ch.rotation);
                readVec(L, c, "scale", ch.scale);
                if (!ch.prefab.empty() || !ch.file.empty()) children.push_back(std::move(ch));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_close(L);
    if (file.empty() && children.empty()) {
        error = path.generic_string() + ": missing 'file' (or 'children')";
        return false;
    }
    return true;
}

std::string Prefab::serialize() const {
    std::ostringstream o;
    o << "-- Prefab written by the editor. Drop it on the scene from the Project panel;\n"
         "-- changing it changes every copy placed in the maps.\n"
         "return {\n";
    if (!file.empty()) o << "    file = " << quoted(file) << ",\n";
    if (!node.empty()) o << "    node = " << quoted(node) << ",\n";
    o << "    rotation = " << vec(rotation) << ",\n    scale = " << vec(scale) << ",\n";
    if (!tags.empty()) o << "    tags = " << quotedList(tags) << ",\n";
    if (!scripts.empty()) o << "    scripts = " << quotedList(scripts) << ",\n";
    if (!children.empty()) {
        o << "    children = {\n";
        for (const Child& c : children) {
            o << "        { ";
            if (!c.prefab.empty()) o << "prefab = " << quoted(c.prefab);
            else o << "file = " << quoted(c.file) << (c.node.empty() ? "" : ", node = " + quoted(c.node));
            if (!c.name.empty()) o << ", name = " << quoted(c.name);
            o << ", position = " << vec(c.position) << ", rotation = " << vec(c.rotation) << ", scale = " << vec(c.scale)
              << " },\n";
        }
        o << "    },\n";
    }
    o << "}\n";
    return o.str();
}

} // namespace eruption
