#define GLM_ENABLE_EXPERIMENTAL
#include "scene/SceneEdits.hpp"

#include "core/Engine.hpp"
#include "script/ScriptApi.hpp"

#include <lua.h>
#include <lualib.h>
#include <luacode.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

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

fs::path SceneEdits::pathFor(const std::string& map) {
    return fs::path("assets/scenes") / (map + ".scene.luau");
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
    size_t size = 0;
    char* bc = luau_compile(src.data(), src.size(), nullptr, &size);
    const std::string chunk = "@" + file.generic_string();
    bool ok = luau_load(L, chunk.c_str(), bc, size, 0) == 0 && lua_pcall(L, 0, 1, 0) == 0;
    std::free(bc);
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
                objects.push_back(std::move(o));
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

    lua_getfield(L, root, "environment");
    if (lua_istable(L, -1)) {
        const int t = lua_gettop(L);
        hasEnvironment = true;
        time = readNum(L, t, "time", 12.0f);
        paused = readBool(L, t, "paused", false);
        weather = readStr(L, t, "weather");
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
    if (!objects.empty()) {
        o << "    objects = {\n";
        for (const auto& ob : objects) {
            o << "        [" << quoted(ob.name) << "] = { ";
            if (ob.hidden) o << "hidden = true";
            if (ob.hasTransform) {
                if (ob.hidden) o << ", ";
                o << "position = " << vec(ob.transform.position) << ", rotation = " << vec(ob.transform.rotation)
                  << ", scale = " << vec(ob.transform.scale);
            }
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
    if (hasEnvironment) {
        o << "    environment = { time = " << num(time) << ", paused = " << (paused ? "true" : "false");
        if (!weather.empty()) o << ", weather = " << quoted(weather);
        o << " },\n";
    }
    o << "}\n";
    return o.str();
}

int SceneEdits::apply(Engine& engine) const {
    ModelRenderer& mr = engine.modelRenderer();
    int applied = 0;
    for (const auto& ob : objects) {
        const int idx = scriptFindModel(ob.name);
        if (idx < 0) continue;
        if (ob.hasTransform) mr.setInstanceTransform(static_cast<uint32_t>(idx), ob.transform.matrix());
        if (ob.hidden) mr.setInstanceEnabled(static_cast<uint32_t>(idx), false);
        ++applied;
    }
    for (const auto& c : copies) {
        const int src = scriptFindModel(c.from);
        if (src < 0 || scriptFindModel(c.name) >= 0) continue;
        mr.duplicateInstance(static_cast<uint32_t>(src), c.name, c.transform.matrix());
        ++applied;
    }
    auto& ls = engine.deferredLighting().getPointLights();
    for (const auto& l : lights) {
        if (l.index < 1 || l.index > static_cast<int>(ls.size())) continue;
        PointLight& p = ls[static_cast<size_t>(l.index - 1)];
        p.position = l.position;
        p.color = l.color;
        p.intensity = l.intensity;
        p.radius = l.range;
        p.enabled = l.enabled;
        ++applied;
    }
    if (hasEnvironment) {
        engine.dayNightCycle().setTimeOfDay(time / 24.0f);
        engine.dayNightCycle().setPaused(paused);
        if (!weather.empty()) engine.setInitialWeatherType(weather);
        ++applied;
    }
    return applied;
}

} // namespace eruption
