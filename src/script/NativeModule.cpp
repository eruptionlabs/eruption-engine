#include "script/NativeModule.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "script/ScriptApi.hpp"

#include <eruption/native_module.h>

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(__unix__) && !defined(__ANDROID__)
#include <dlfcn.h>
#define ERUPTION_NATIVE_RELOAD 1
#endif

namespace eruption {

namespace fs = std::filesystem;

// Funções que o módulo recebe em EruptionHost (C ABI, sem captura).
struct NativeHostCalls {
    static NativeModule& m() { return NativeModule::instance(); }
    static void log(const char* text) { Logger::warning("[native] %s", text ? text : ""); }
    static void expose(const char* name, EruptionNativeFn fn) {
        if (name && *name && fn) m().m_exposed[name] = fn;
    }
    static const std::vector<ModelInstance>* instances() {
        return m().m_engine ? &m().m_engine->modelRenderer().getInstances() : nullptr;
    }
    static int findModel(const char* name) {
        const int i = name ? scriptFindModel(name) : -1;
        return i < 0 ? 0 : i + 1;
    }
    static void modelPosition(int model, float out[3]) {
        const auto* insts = instances();
        const Vec3 p = insts && model >= 1 && size_t(model) <= insts->size() ? Vec3((*insts)[size_t(model - 1)].transform[3])
                                                                              : Vec3(0.0f);
        out[0] = p.x;
        out[1] = p.y;
        out[2] = p.z;
    }
    static void setModelPosition(int model, const float position[3]) {
        const auto* insts = instances();
        if (!insts || model < 1 || size_t(model) > insts->size()) return;
        Mat4 t = (*insts)[size_t(model - 1)].transform;
        t[3] = Vec4(position[0], position[1], position[2], 1.0f);
        m().m_engine->modelRenderer().setInstanceTransform(static_cast<uint32_t>(model - 1), t);
    }
    static void playerPosition(float out[3]) {
        const Vec3 p = scriptPlayerPosition();
        out[0] = p.x;
        out[1] = p.y;
        out[2] = p.z;
    }
    static float groundHeight(float x, float z) { return m().m_engine ? m().m_engine->sampleTerrainHeight(x, z) : 0.0f; }
    static double time() { return m().m_time; }

    static EruptionHost host() {
        NativeModule& n = m();
        EruptionHost h{};
        h.version = ERUPTION_NATIVE_API_VERSION;
        h.state = n.m_state.empty() ? nullptr : n.m_state.data();
        h.stateSize = n.m_state.size();
        h.log = &log;
        h.expose = &expose;
        h.findModel = &findModel;
        h.modelPosition = &modelPosition;
        h.setModelPosition = &setModelPosition;
        h.playerPosition = &playerPosition;
        h.groundHeight = &groundHeight;
        h.time = &time;
        return h;
    }
};

NativeModule& NativeModule::instance() {
    static NativeModule s;
    return s;
}

void NativeModule::init(Engine& engine, const fs::path& projectRoot, const fs::path& engineRoot) {
    shutdown();
    m_engine = &engine;
    m_sources = projectRoot / "native";
    m_buildDir = projectRoot / ".eruption" / "native";
    m_includeDir = engineRoot / "include";
    std::error_code ec;
#ifdef ERUPTION_NATIVE_RELOAD
    // Sobras de uma execução que não fechou direito (as cópias com número).
    for (auto it = fs::directory_iterator(m_buildDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().filename().string().rfind("build-", 0) == 0) fs::remove(it->path(), ec);
    ec.clear();
    if (fs::is_directory(m_sources, ec)) {
        sourcesChanged(); // assinatura inicial
        startBuild();
        return;
    }
    // Jogo exportado: só a biblioteca da última compilação.
    const fs::path prebuilt = m_buildDir / "game.so";
    if (fs::is_regular_file(prebuilt, ec)) swapTo(prebuilt);
#else
    if (fs::is_directory(m_sources, ec))
        Logger::warning("[native] C++ modules are not supported on this platform yet; native/ ignored");
#endif
}

void NativeModule::shutdown() {
    if (m_build.valid()) m_build.wait();
    m_build = {};
    if (m_running) stop();
    unloadCurrent(false);
    m_state.clear();
    m_hasState = false;
    m_engine = nullptr;
}

bool NativeModule::sourcesChanged() {
    // Data e tamanho de cada arquivo da pasta (cabeçalhos também).
    std::uintmax_t sig = 1469598103934665603ull;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_sources, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".cc" && ext != ".hpp" && ext != ".h") continue;
        const auto t = fs::last_write_time(it->path(), ec).time_since_epoch().count();
        const std::string key = it->path().generic_string();
        sig = (sig ^ std::hash<std::string>()(key)) * 1099511628211ull;
        sig = (sig ^ static_cast<std::uintmax_t>(t)) * 1099511628211ull;
        sig = (sig ^ it->file_size(ec)) * 1099511628211ull;
    }
    const bool changed = sig != m_signature;
    m_signature = sig;
    return changed;
}

void NativeModule::startBuild() {
#ifdef ERUPTION_NATIVE_RELOAD
    if (m_build.valid()) {
        m_rebuildPending = true; // termina esta e compila de novo
        return;
    }
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_sources, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const auto ext = it->path().extension().string();
        if (it->is_regular_file(ec) && (ext == ".cpp" || ext == ".cc")) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) return;
    fs::create_directories(m_buildDir, ec);
    const fs::path out = m_buildDir / ("build-" + std::to_string(++m_buildCount) + ".so");
    const char* cxxEnv = std::getenv("ERUPTION_CXX");
    const std::string cxx = cxxEnv && *cxxEnv ? cxxEnv : "c++";
    auto quote = [](const fs::path& p) { return "\"" + p.string() + "\""; };
    std::string cmd = cxx + " -std=c++17 -O2 -g -fPIC -shared -fvisibility=hidden -I" + quote(m_includeDir) + " -I" +
                      quote(m_sources);
    for (const auto& f : files) cmd += " " + quote(f);
    cmd += " -o " + quote(out) + " 2>&1";
    Logger::warning("[native] compiling %zu file(s)...", files.size());
    m_build = std::async(std::launch::async, [cmd, out] {
        BuildResult r;
        r.library = out;
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[512];
            while (fgets(buf, sizeof(buf), p)) r.output += buf;
            r.ok = pclose(p) == 0;
        } else {
            r.output = "could not run the compiler";
        }
        return r;
    });
#endif
}

void NativeModule::poll(float dt) {
#ifdef ERUPTION_NATIVE_RELOAD
    if (!m_engine) return;
    if (m_build.valid() && m_build.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        BuildResult r = m_build.get();
        // Mensagens do compilador linha a linha ("arquivo:linha: erro" vira link no VS Code).
        std::istringstream lines(r.output);
        for (std::string line; std::getline(lines, line);)
            if (!line.empty()) Logger::warning("[native] %s", line.c_str());
        std::error_code ec;
        if (!r.ok) {
            Logger::error("[native] build failed; %s", m_module ? "the previous version keeps running" : "no module loaded");
            fs::remove(r.library, ec);
        } else if (swapTo(r.library)) {
            fs::copy_file(r.library, m_buildDir / "game.so", fs::copy_options::overwrite_existing, ec);
        }
        if (m_rebuildPending) {
            m_rebuildPending = false;
            startBuild();
        }
    }
    m_pollTimer += dt;
    if (m_pollTimer < 1.0f) return;
    m_pollTimer = 0.0f;
    std::error_code ec;
    if (fs::is_directory(m_sources, ec) && sourcesChanged()) startBuild();
#else
    (void)dt;
    (void)m_rebuildPending; // só existem para a recarga
    (void)m_pollTimer;
    (void)m_buildCount;
#endif
}

void NativeModule::unloadCurrent(bool reloading) {
    if (!m_module) return;
    if (m_module->unload) {
        EruptionHost h = NativeHostCalls::host();
        m_module->unload(&h, reloading ? 1 : 0);
    }
    m_exposed.clear(); // os ponteiros eram da biblioteca que sai
    m_module = nullptr;
#ifdef ERUPTION_NATIVE_RELOAD
    if (m_handle) dlclose(m_handle);
#endif
    m_handle = nullptr;
    std::error_code ec;
    if (!m_loadedFile.empty() && m_loadedFile.filename() != "game.so") fs::remove(m_loadedFile, ec);
    m_loadedFile.clear();
}

bool NativeModule::swapTo(const fs::path& library) {
#ifdef ERUPTION_NATIVE_RELOAD
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        Logger::error("[native] %s", dlerror());
        return false;
    }
    using EntryFn = const EruptionModule* (*)();
    auto entry = reinterpret_cast<EntryFn>(dlsym(handle, "eruption_module"));
    const EruptionModule* mod = entry ? entry() : nullptr;
    if (!mod || mod->version != ERUPTION_NATIVE_API_VERSION) {
        Logger::error("[native] %s: %s", library.filename().string().c_str(),
                      mod ? "built for another API version (rebuild with this engine's header)"
                          : "eruption_module() not found (export it with ERUPTION_MODULE_EXPORT)");
        dlclose(handle);
        return false;
    }
    const bool reloading = m_module != nullptr;
    unloadCurrent(true);
    m_handle = handle;
    m_module = mod;
    m_loadedFile = library;
    // Estado: os bytes da versão anterior, no tamanho que a nova pede (o que
    // cresceu vem zerado).
    m_state.resize(mod->stateSize, 0);
    const bool reloaded = m_hasState && reloading;
    m_hasState = true;
    if (mod->load) {
        EruptionHost h = NativeHostCalls::host();
        mod->load(&h, reloaded ? 1 : 0);
    }
    Logger::warning("[native] %s %s (%zu function(s) for the scripts)", reloading ? "reloaded" : "loaded",
                    library.filename().string().c_str(), m_exposed.size());
    // Recarregado com o jogo rodando: continua de onde estava (sem start de novo).
    return true;
#else
    (void)library;
    return false;
#endif
}

void NativeModule::start() {
    m_running = true;
    m_time = 0.0;
    if (m_module && m_module->start) {
        EruptionHost h = NativeHostCalls::host();
        m_module->start(&h);
    }
}

void NativeModule::stop() {
    if (m_module && m_module->stop) {
        EruptionHost h = NativeHostCalls::host();
        m_module->stop(&h);
    }
    m_running = false;
}

void NativeModule::update(float dt) {
    if (!m_running) return;
    m_time += dt;
    if (m_module && m_module->update) {
        EruptionHost h = NativeHostCalls::host();
        m_module->update(&h, dt);
    }
}

bool NativeModule::call(const std::string& name, const double* args, int count, double& out) const {
    const auto it = m_exposed.find(name);
    if (it == m_exposed.end()) return false;
    EruptionHost h = NativeHostCalls::host();
    out = it->second(&h, args, count);
    return true;
}

namespace {

// native.<nome>: o __index devolve uma função que procura o nome a cada
// chamada, então continua valendo depois de uma recarga.
int nativeCall(lua_State* L) {
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    const int n = std::min(lua_gettop(L), 16);
    double args[16];
    for (int i = 0; i < n; ++i) args[i] = luaL_checknumber(L, i + 1);
    double out = 0.0;
    if (!NativeModule::instance().call(name, args, n, out))
        luaL_errorL(L, "native.%s: no C++ function with this name (is the module in native/ built?)", name);
    lua_pushnumber(L, out);
    return 1;
}

int nativeIndex(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, nativeCall, "native", 1);
    return 1;
}

} // namespace

void NativeModule::registerApi(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, nativeIndex, "__index");
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "native");
}

const char* NativeModule::apiDefinitions() {
    return R"(
-- C++ functions of this project (native/*.cpp, include/eruption/native_module.h):
-- native.<name>(...) with numbers in and one number out. Saving a .cpp file
-- rebuilds and reloads them while the game runs.
declare native: { [string]: (...number) -> number }
)";
}

} // namespace eruption
