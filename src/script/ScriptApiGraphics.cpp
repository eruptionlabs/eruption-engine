// Quality screen support for games: the table `graphics` (live preset switch and
// the machine's GPU name; the memory is the total of the device-local heaps). Frame-time statistics need no engine
// support: a script samples perf.frame() itself.
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"

#include <lua.h>
#include <lualib.h>

#include <nlohmann/json.hpp>

namespace eruption {

namespace {

Engine* g_engine = nullptr;

void pushValue(lua_State* L, const nlohmann::json& j) {
    if (j.is_boolean()) lua_pushboolean(L, j.get<bool>());
    else if (j.is_number()) lua_pushnumber(L, j.get<double>());
    else if (j.is_string()) lua_pushstring(L, j.get<std::string>().c_str());
    else lua_pushnil(L);
}

// Scalar table fields only (number, boolean, string): what a preset holds.
nlohmann::json readOverrides(lua_State* L, int idx) {
    nlohmann::json out = nlohmann::json::object();
    if (!lua_istable(L, idx)) return out;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const std::string key = lua_tostring(L, -2);
            if (lua_isboolean(L, -1)) out[key] = lua_toboolean(L, -1) != 0;
            else if (lua_type(L, -1) == LUA_TNUMBER) out[key] = lua_tonumber(L, -1);
            else if (lua_type(L, -1) == LUA_TSTRING) out[key] = std::string(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
    return out;
}

int graphics_info(lua_State* L) {
    lua_createtable(L, 0, 5);
    lua_pushstring(L, g_engine->vulkan().gpuName());
    lua_setfield(L, -2, "gpu");
    // Total size of the device-local memory heaps in MB (perf.system().vram_mb is what is allocated now).
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(g_engine->vulkan().physicalDevice(), &memProps);
    double totalBytes = 0.0;
    for (uint32_t h = 0; h < memProps.memoryHeapCount; ++h)
        if (memProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) totalBytes += static_cast<double>(memProps.memoryHeaps[h].size);
    if (totalBytes > 0.0) {
        lua_pushnumber(L, totalBytes / (1024.0 * 1024.0));
        lua_setfield(L, -2, "vram_total_mb");
    }
    lua_pushstring(L, g_engine->graphicsPresetName().c_str());
    lua_setfield(L, -2, "preset");
    return 1;
}

int graphics_values(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const nlohmann::json& root = g_engine->graphicsConfig().root();
    if (!root.is_object() || !root.contains("presets") || !root["presets"].contains(name)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (auto it = root["presets"][name].begin(); it != root["presets"][name].end(); ++it) {
        pushValue(L, it.value());
        lua_setfield(L, -2, it.key().c_str());
    }
    return 1;
}

int graphics_apply(lua_State* L) {
    const char* base = luaL_checkstring(L, 1);
    lua_pushboolean(L, g_engine->applyGraphicsLive(base, readOverrides(L, 2)));
    return 1;
}

} // namespace

void registerGraphicsApi(lua_State* L, Engine& engine) {
    g_engine = &engine;
    static const luaL_Reg fns[] = {{"info", graphics_info}, {"values", graphics_values}, {"apply", graphics_apply}, {nullptr, nullptr}};
    lua_newtable(L);
    for (const luaL_Reg* f = fns; f->name; ++f) {
        lua_pushcfunction(L, f->func, f->name);
        lua_setfield(L, -2, f->name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "graphics");
}

const char* graphicsApiDefinitions() {
    return R"(
-- Graphics quality, for a game's own options screen.
declare graphics: {
    -- The machine's GPU name, the size of its device-local memory in MB (a
    -- quick tier hint; perf.system().vram_mb is what is allocated right now)
    -- and the name of the active preset.
    info: () -> { gpu: string, vram_total_mb: number?, preset: string },
    -- The keys of a preset of data/graphics.json (project patch included), or
    -- nil when it does not exist.
    values: (name: string) -> { [string]: any }?,
    -- Switches quality live: the active preset becomes a copy of preset `base`
    -- with `overrides` (preset keys; numbers, booleans, strings) on top, and
    -- the scene resolution follows "render_scale". "present_mode" ("fifo" or
    -- "immediate") sets vsync. Returns false when `base` does not exist.
    -- Keys decided before the pipelines exist (shader_lite, post_format,
    -- texture_layer_*, texture compression) only take effect on the next
    -- start; so do mesh relief bakes of meshes already loaded and the texture
    -- cap of textures already loaded.
    apply: (base: string, overrides: { [string]: any }?) -> boolean,
}
)";
}

} // namespace eruption
