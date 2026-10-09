#include "script/LuauChunk.hpp"

#include <lua.h>
#if ERUPTION_LUAU_COMPILER
#include <luacode.h>
#endif

#include <cstdlib>
#include <cstring>

namespace eruption {

namespace {
bool g_debugCompile = false;
// Começa com ESC: nenhum arquivo de texto Luau começa assim.
constexpr char kMagic[] = "\x1b" "ERUPTLBC";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
} // namespace

void setLuauDebugCompile(bool on) { g_debugCompile = on; }

bool isLuauBytecode(const std::string& data) {
    return data.size() > kMagicSize && std::memcmp(data.data(), kMagic, kMagicSize) == 0;
}

bool loadLuauChunk(lua_State* L, const std::string& chunkName, const std::string& data) {
    if (isLuauBytecode(data))
        return luau_load(L, chunkName.c_str(), data.data() + kMagicSize, data.size() - kMagicSize, 0) == 0;
#if ERUPTION_LUAU_COMPILER
    lua_CompileOptions opts{};
    opts.optimizationLevel = g_debugCompile ? 0 : 1;
    opts.debugLevel = g_debugCompile ? 2 : 1; // linhas nas mensagens de erro (e locais ao depurar)
    size_t size = 0;
    char* bc = luau_compile(data.data(), data.size(), &opts, &size);
    const bool ok = luau_load(L, chunkName.c_str(), bc, size, 0) == 0;
    std::free(bc);
    return ok;
#else
    lua_pushstring(L, (chunkName.substr(chunkName[0] == '@' ? 1 : 0) +
                       ": not compiled (export the game again from the editor)").c_str());
    return false;
#endif
}

#if ERUPTION_LUAU_COMPILER
std::string compileLuauForExport(const std::string& source, std::string& error) {
    if (isLuauBytecode(source)) return source;
    lua_CompileOptions opts{};
    opts.optimizationLevel = 2;
    opts.debugLevel = 1;
    size_t size = 0;
    char* bc = luau_compile(source.data(), source.size(), &opts, &size);
    std::string out;
    if (size > 0 && bc[0] == 0) {
        error.assign(bc + 1, size - 1); // erro de compilação
    } else {
        out.assign(kMagic, kMagicSize);
        out.append(bc, size);
    }
    std::free(bc);
    return out;
}
#endif

} // namespace eruption
