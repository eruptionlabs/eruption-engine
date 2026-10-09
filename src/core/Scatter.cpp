// Objetos espalhados pelo pincel (árvores, pedras, grama): guardados por
// modelo em assets/scenes/<mapa>.scatter.luau, criados em lote e fora da
// hierarquia de objetos (se editam pelo pincel, não um a um).
#include "core/Engine.hpp"

#include "core/Logger.hpp"
#include "scene/SceneEdits.hpp"

#include <lua.h>
#include <lualib.h>
#include <luacode.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

const char* kScatterName = "~scatter";

fs::path scatterPathFor(const std::string& map) {
    fs::path p = SceneEdits::pathFor(map);
    p.replace_extension();
    p.replace_extension(".scatter.luau");
    return p;
}

std::string q(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

std::string n4(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.4g", v);
    return b;
}

} // namespace

bool Engine::isScatterInstance(uint32_t index) const {
    const auto& insts = m_modelRenderer.getInstances();
    return index < insts.size() && insts[index].name == kScatterName;
}

int Engine::scatterGroup(const std::string& file, const std::string& node, const std::string& prefab) {
    for (size_t i = 0; i < m_scatter.size(); ++i)
        if (m_scatter[i].file == file && m_scatter[i].node == node && m_scatter[i].prefab == prefab) return int(i);
    ScatterGroup g;
    g.file = file;
    g.node = node;
    g.prefab = prefab;
    Mat4 inner(1.0f);
    std::string f = file, nd = node;
    if (!prefab.empty()) {
        Prefab p;
        std::string error;
        if (!p.load(prefab, error)) {
            ERUPTION_LOG_ERROR("%s", error.c_str());
            return -1;
        }
        f = p.file;
        nd = p.node;
        inner = p.matrix();
    }
    // Molde: o modelo criado uma vez (desligado), de onde saem as cópias.
    SceneEdits::AddedInstance tmpl;
    if (!SceneEdits::addModel(*this, f, nd, kScatterName, Mat4(1.0f), inner, tmpl)) return -1;
    g.templateFirst = tmpl.first;
    g.local = tmpl.local;
    std::vector<uint32_t> idx;
    for (uint32_t k = 0; k < tmpl.count; ++k) {
        idx.push_back(tmpl.first + k);
        m_modelRenderer.setInstanceName(tmpl.first + k, kScatterName);
    }
    m_modelRenderer.setInstancesEnabled(idx, false);
    m_scatter.push_back(std::move(g));
    return int(m_scatter.size() - 1);
}

std::vector<uint32_t> Engine::scatterAdd(int group, const std::vector<Mat4>& roots) {
    std::vector<uint32_t> items;
    if (group < 0 || group >= int(m_scatter.size()) || roots.empty()) return items;
    ScatterGroup& g = m_scatter[size_t(group)];
    std::vector<std::pair<uint32_t, Mat4>> copies;
    copies.reserve(roots.size() * g.local.size());
    for (const Mat4& r : roots)
        for (uint32_t k = 0; k < g.local.size(); ++k) copies.push_back({g.templateFirst + k, r * g.local[k]});
    const uint32_t first = m_modelRenderer.duplicateBatch(copies, kScatterName);
    for (size_t i = 0; i < roots.size(); ++i) {
        items.push_back(uint32_t(g.items.size()));
        g.items.push_back({first + uint32_t(i * g.local.size()), false});
    }
    return items;
}

void Engine::scatterSetRemoved(int group, const std::vector<uint32_t>& items, bool removed) {
    if (group < 0 || group >= int(m_scatter.size())) return;
    ScatterGroup& g = m_scatter[size_t(group)];
    std::vector<uint32_t> inst;
    for (uint32_t it : items) {
        if (it >= g.items.size()) continue;
        g.items[it].removed = removed;
        for (uint32_t k = 0; k < g.local.size(); ++k) inst.push_back(g.items[it].first + k);
    }
    m_modelRenderer.setInstancesEnabled(inst, !removed);
}

Mat4 Engine::scatterRoot(int group, uint32_t item) const {
    const ScatterGroup& g = m_scatter[size_t(group)];
    // Do jeito que está agora (o chão pode ter subido debaixo dele).
    return m_modelRenderer.getInstances()[g.items[item].first].transform * glm::inverse(g.local[0]);
}

void Engine::clearScatter() { m_scatter.clear(); }

int Engine::loadScatter(const std::string& map) {
    m_scatter.clear();
    std::ifstream f(scatterPathFor(map), std::ios::binary);
    if (!f) return 0;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_sandbox(L);
    size_t size = 0;
    char* bc = luau_compile(src.data(), src.size(), nullptr, &size);
    const std::string chunk = "@" + scatterPathFor(map).generic_string();
    const bool ok = luau_load(L, chunk.c_str(), bc, size, 0) == 0 && lua_pcall(L, 0, 1, 0) == 0;
    std::free(bc);
    if (!ok || !lua_istable(L, -1)) {
        ERUPTION_LOG_ERROR("%s", ok ? "scatter file must return a table" : lua_tostring(L, -1));
        lua_close(L);
        return 0;
    }
    auto str = [&](int t, const char* k) {
        lua_getfield(L, t, k);
        std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return v;
    };
    int total = 0;
    const int groups = lua_objlen(L, -1);
    for (int gi = 1; gi <= groups; ++gi) {
        lua_rawgeti(L, -1, gi);
        const int t = lua_gettop(L);
        if (lua_istable(L, t)) {
            const int group = scatterGroup(str(t, "file"), str(t, "node"), str(t, "prefab"));
            std::vector<Mat4> roots;
            lua_getfield(L, t, "items");
            if (group >= 0 && lua_istable(L, -1)) {
                const int n = lua_objlen(L, -1);
                for (int i = 1; i <= n; ++i) {
                    lua_rawgeti(L, -1, i);
                    float v[7] = {0, 0, 0, 0, 0, 0, 1};
                    for (int c = 0; c < 7; ++c) {
                        lua_rawgeti(L, -1, c + 1);
                        if (lua_isnumber(L, -1)) v[c] = float(lua_tonumber(L, -1));
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                    SceneEdits::Transform tr;
                    tr.position = Vec3(v[0], v[1], v[2]);
                    tr.rotation = Vec3(v[3], v[4], v[5]);
                    tr.scale = Vec3(v[6]);
                    roots.push_back(tr.matrix());
                }
            }
            lua_pop(L, 1);
            total += int(scatterAdd(group, roots).size());
        }
        lua_pop(L, 1);
    }
    lua_close(L);
    ERUPTION_LOG_WARN("Scatter: %d painted objects in %zu groups (%s)", total, m_scatter.size(),
                      scatterPathFor(map).string().c_str());
    return total;
}

bool Engine::saveScatter(std::string& error) const {
    const fs::path path = scatterPathFor(m_currentMapName);
    size_t live = 0;
    for (const auto& g : m_scatter)
        for (const auto& it : g.items) live += it.removed ? 0 : 1;
    std::error_code ec;
    if (live == 0) {
        fs::remove(path, ec);
        return true;
    }
    std::ostringstream o;
    o << "-- Objects painted with the Objects brush of the editor: {x, y, z, rot x, rot y, rot z, scale}.\n"
         "return {\n";
    for (size_t gi = 0; gi < m_scatter.size(); ++gi) {
        const ScatterGroup& g = m_scatter[gi];
        o << "    { ";
        if (!g.prefab.empty()) o << "prefab = " << q(g.prefab);
        else o << "file = " << q(g.file) << (g.node.empty() ? "" : ", node = " + q(g.node));
        o << ", items = {\n";
        for (uint32_t i = 0; i < g.items.size(); ++i) {
            if (g.items[i].removed) continue;
            const SceneEdits::Transform t = SceneEdits::Transform::from(scatterRoot(int(gi), i));
            o << "        {" << n4(t.position.x) << ", " << n4(t.position.y) << ", " << n4(t.position.z) << ", "
              << n4(t.rotation.x) << ", " << n4(t.rotation.y) << ", " << n4(t.rotation.z) << ", " << n4(t.scale.x) << "},\n";
        }
        o << "    } },\n";
    }
    o << "}\n";
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary) << o.str();
    if (!fs::exists(path, ec)) {
        error = "Could not write " + path.generic_string();
        return false;
    }
    return true;
}

} // namespace eruption
