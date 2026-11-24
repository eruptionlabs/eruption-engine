#include "script/NativeModule.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "script/ScriptApi.hpp"
#include "script/ScriptHost.hpp"

#include <eruption/native_module.h>

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>

#if defined(__unix__) && !defined(__ANDROID__)
#include <dlfcn.h>
#define ERUPTION_NATIVE_RELOAD 1
#endif

#if defined(ERUPTION_NATIVE_STATIC)
// O módulo do projeto linkado na biblioteca do jogo (Android).
extern "C" const EruptionModule* eruption_module(void);
#endif

namespace eruption {

namespace {
constexpr int kMaxValueDepth = 16;
constexpr size_t kMaxQueuedEvents = 4096;
constexpr size_t kMaxTablePairs = 65536;
using Val = NativeModule::Val;

// EruptionValue (do módulo) -> Val com cópia. Falso se estourar profundidade/tamanho.
bool copyIn(const EruptionValue& v, Val& out, int depth) {
    if (depth > kMaxValueDepth) return false;
    out.type = v.type;
    switch (v.type) {
    case ERUPTION_NIL: return true;
    case ERUPTION_BOOL:
    case ERUPTION_NUMBER: out.number = v.number; return true;
    case ERUPTION_STRING:
    case ERUPTION_BYTES:
        if (v.size && !v.data) return false;
        out.bytes.assign(v.data ? v.data : "", v.size);
        return true;
    case ERUPTION_TABLE:
        if (v.count > kMaxTablePairs || (v.count && !v.pairs)) return false;
        out.pairs.resize(v.count);
        for (size_t i = 0; i < v.count; ++i)
            if (!copyIn(v.pairs[i].key, out.pairs[i].first, depth + 1) ||
                !copyIn(v.pairs[i].value, out.pairs[i].second, depth + 1))
                return false;
        return true;
    default: return false;
    }
}

// Val -> EruptionValue apontando para dentro de `v`; as listas de pares ficam em `arena`.
void viewOf(const Val& v, EruptionValue& out, std::deque<std::vector<EruptionPair>>& arena) {
    out = {v.type, v.number, v.bytes.data(), v.bytes.size(), nullptr, 0};
    if (v.type != ERUPTION_TABLE) return;
    arena.emplace_back(v.pairs.size());
    std::vector<EruptionPair>& list = arena.back();
    for (size_t i = 0; i < v.pairs.size(); ++i) {
        viewOf(v.pairs[i].first, list[i].key, arena);
        viewOf(v.pairs[i].second, list[i].value, arena);
    }
    out.pairs = list.data();
    out.count = list.size();
}

bool luaToVal(lua_State* L, int idx, Val& out, int depth) {
    if (depth > kMaxValueDepth) return false;
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out.type = ERUPTION_NIL; return true;
    case LUA_TBOOLEAN: out.type = ERUPTION_BOOL; out.number = lua_toboolean(L, idx) ? 1.0 : 0.0; return true;
    case LUA_TNUMBER: out.type = ERUPTION_NUMBER; out.number = lua_tonumber(L, idx); return true;
    case LUA_TSTRING: {
        size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        out.type = ERUPTION_STRING;
        out.bytes.assign(s, n);
        return true;
    }
    case LUA_TBUFFER: {
        size_t n = 0;
        const void* p = lua_tobuffer(L, idx, &n);
        out.type = ERUPTION_BYTES;
        out.bytes.assign(static_cast<const char*>(p), n);
        return true;
    }
    case LUA_TTABLE: {
        out.type = ERUPTION_TABLE;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (out.pairs.size() >= kMaxTablePairs) { lua_pop(L, 2); return false; }
            const int kt = lua_type(L, -2);
            if (kt != LUA_TSTRING && kt != LUA_TNUMBER) { lua_pop(L, 1); continue; } // outras chaves: ignora
            Val k, v;
            if (!luaToVal(L, -2, k, depth + 1) || !luaToVal(L, -1, v, depth + 1)) { lua_pop(L, 2); return false; }
            out.pairs.emplace_back(std::move(k), std::move(v));
            lua_pop(L, 1);
        }
        return true;
    }
    default: out.type = ERUPTION_NIL; return true; // funções etc. não atravessam
    }
}

void pushVal(lua_State* L, const Val& v) {
    switch (v.type) {
    case ERUPTION_BOOL: lua_pushboolean(L, v.number != 0.0); break;
    case ERUPTION_NUMBER: lua_pushnumber(L, v.number); break;
    case ERUPTION_STRING: lua_pushlstring(L, v.bytes.data(), v.bytes.size()); break;
    case ERUPTION_BYTES: {
        void* p = lua_newbuffer(L, v.bytes.size());
        if (!v.bytes.empty()) std::memcpy(p, v.bytes.data(), v.bytes.size());
        break;
    }
    case ERUPTION_TABLE:
        lua_createtable(L, 0, int(v.pairs.size()));
        for (const auto& kv : v.pairs) {
            pushVal(L, kv.first);
            pushVal(L, kv.second);
            lua_rawset(L, -3);
        }
        break;
    default: lua_pushnil(L); break;
    }
}
} // namespace

namespace fs = std::filesystem;

#ifdef ERUPTION_NATIVE_RELOAD // so' a recompilacao em tempo de execucao usa estes auxiliares
// native/link.txt: uma biblioteca de sistema por linha (ex.: ws2_32, bcrypt);
// vazias e as que começam com # são ignoradas. No desktop (Linux) viram -l<nome>
// na compilação; no Android o CMake faz o mesmo no link do jogo. No Windows não
// há compilação em tempo de execução: a lista é lida pelo CMake/exportação.
static std::vector<std::string> readLinkList(const fs::path& sources) {
    std::vector<std::string> libs;
    std::ifstream f(sources / "link.txt");
    for (std::string line; std::getline(f, line);) {
        const size_t a = line.find_first_not_of(" \t\r");
        const size_t b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#') continue;
        const std::string name = line.substr(a, b - a + 1);
        // só nomes de biblioteca (nada de flags nem espaços para o shell)
        if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-") == std::string::npos)
            libs.push_back(name);
    }
    return libs;
}
#endif

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
    static void exposeValue(const char* name, EruptionNativeValueFn fn) {
        if (name && *name && fn) m().m_exposedV[name] = fn;
    }
    static int emit(const char* event, const EruptionValue* value) { return m().queueEvent(event, value) ? 1 : 0; }

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
        h.expose_value = &exposeValue;
        h.emit = &emit;
        return h;
    }
};

#ifdef ERUPTION_NATIVE_RELOAD
namespace {
// FNV-1a de 64 bits, encadeável.
std::uint64_t hashString(const std::string& s, std::uint64_t h) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
std::uint64_t hashFile(const fs::path& p, std::uint64_t h) {
    std::ifstream f(p, std::ios::binary);
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        for (std::streamsize i = 0; i < f.gcount(); ++i) h = (h ^ static_cast<unsigned char>(buf[i])) * 1099511628211ull;
        if (f.eof()) break;
    }
    return h;
}
// Compilador por sondagem: a variável de ambiente, senão o primeiro candidato que
// responde a --version (um alias quebrado, como c++ -> g++-12 removido, não conta).
// Versões g++-N / clang++-N achadas no PATH entram depois dos nomes simples, maior
// primeiro. O resultado vale pelo processo todo.
std::vector<std::string> versionedCompilers(const std::string& prefix) {
    std::vector<std::pair<int, std::string>> found;
    const char* path = std::getenv("PATH");
    std::istringstream dirs(path ? path : "");
    std::error_code ec;
    for (std::string dir; std::getline(dirs, dir, ':');) {
        if (dir.empty()) continue;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n.size() <= prefix.size() + 1 || n.compare(0, prefix.size(), prefix) != 0 || n[prefix.size()] != '-') continue;
            const std::string ver = n.substr(prefix.size() + 1);
            if (ver.find_first_not_of("0123456789") != std::string::npos) continue;
            found.emplace_back(std::atoi(ver.c_str()), n);
        }
        ec.clear();
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> names;
    for (const auto& f : found)
        if (std::find(names.begin(), names.end(), f.second) == names.end()) names.push_back(f.second);
    return names;
}

std::string pickCompiler(const char* envName, const std::vector<std::string>& simple, const std::vector<std::string>& versioned,
                         std::string& cache) {
    if (!cache.empty()) return cache;
    if (const char* e = std::getenv(envName); e && *e) return cache = e;
    std::vector<std::string> cands = simple;
    for (const auto& v : versioned) cands.push_back(v);
    for (const std::string& c : cands) {
        if (std::system((c + " --version >/dev/null 2>&1").c_str()) == 0) {
            Logger::warning("[native] compiler %s: %s (set %s to override)", envName, c.c_str(), envName);
            return cache = c;
        }
    }
    Logger::error("[native] no working compiler found (tried %s...); set %s to the compiler command", cands.empty() ? "" : cands[0].c_str(), envName);
    return cache = "false"; // o build falha com mensagem do shell, sem reprobar a cada vez
}
} // namespace
#endif // ERUPTION_NATIVE_RELOAD

NativeModule& NativeModule::instance() {
    static NativeModule s;
    return s;
}

void NativeModule::init(Engine& engine, const fs::path& projectRoot, const fs::path& engineRoot) {
    closeModule(); // os ouvintes de native.on (registrados na criação do estado Luau) ficam
    m_engine = &engine;
    m_sources = projectRoot / "native";
    m_buildDir = projectRoot / ".eruption" / "native";
    m_includeDir = engineRoot / "include";
    std::error_code ec;
#if defined(ERUPTION_NATIVE_STATIC)
    (void)ec;
    activate(eruption_module(), "game", nullptr);
#elif defined(ERUPTION_NATIVE_RELOAD)
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
    closeModule();
    m_listeners.clear(); // o estado Luau vai fechar
    m_L = nullptr;
}

void NativeModule::closeModule() {
    if (m_build.valid()) m_build.wait();
    m_build = {};
    if (m_running) stop();
    unloadCurrent(false);
    m_state.clear();
    m_hasState = false;
    m_engine = nullptr;
    std::lock_guard<std::mutex> lock(m_eventMutex);
    m_events.clear();
    m_droppedEvents = 0;
}

bool NativeModule::sourcesChanged() {
    // Data e tamanho de cada arquivo da pasta (cabeçalhos também).
    std::uintmax_t sig = 1469598103934665603ull;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_sources, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".cc" && ext != ".c" && ext != ".hpp" && ext != ".h" && ext != ".txt") continue;
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
    std::vector<fs::path> files, cfiles;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_sources, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const auto ext = it->path().extension().string();
        if (!it->is_regular_file(ec)) continue;
        if (ext == ".cpp" || ext == ".cc") files.push_back(it->path());
        else if (ext == ".c") cfiles.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::sort(cfiles.begin(), cfiles.end());
    if (files.empty() && cfiles.empty()) return;
    fs::create_directories(m_buildDir, ec);
    const fs::path out = m_buildDir / ("build-" + std::to_string(++m_buildCount) + ".so");
    static std::string s_cxx, s_cc;
    const std::string cxx = pickCompiler("ERUPTION_CXX", {"c++", "g++", "clang++"}, !s_cxx.empty() ? std::vector<std::string>() : [] {
        auto v = versionedCompilers("g++"); auto c = versionedCompilers("clang++"); v.insert(v.end(), c.begin(), c.end()); return v; }(), s_cxx);
    const std::string cc = pickCompiler("ERUPTION_CC", {"cc", "gcc", "clang"}, !s_cc.empty() ? std::vector<std::string>() : [] {
        auto v = versionedCompilers("gcc"); auto c = versionedCompilers("clang"); v.insert(v.end(), c.begin(), c.end()); return v; }(), s_cc);
    auto quote = [](const fs::path& p) { return "\"" + p.string() + "\""; };
    // Cabeçalhos do motor, da pasta native/ e de native/include.
    const std::string incs = " -I" + quote(m_includeDir) + " -I" + quote(m_sources) + " -I" + quote(m_sources / "include");
    // Cache por arquivo: cada fonte vira um .o em .eruption/native/obj, cujo nome
    // carrega a chave (compilador + opções + caminho + conteúdo do fonte + conteúdo
    // de todos os cabeçalhos). Fonte que não mudou não é compilada de novo; só a
    // ligação roda sempre. Cabeçalhos entram todos na chave (conservador, sem -MMD).
    const std::string cxxFlags = " -std=c++17 -O2 -g -fPIC -fvisibility=hidden";
    const std::string cFlags = " -std=c11 -O2 -g -fPIC -fvisibility=hidden";
    const fs::path objDir = m_buildDir / "obj";
    fs::create_directories(objDir, ec);
    std::uint64_t headerHash = 1469598103934665603ull;
    {
        std::vector<fs::path> headers;
        for (const fs::path& root : {m_sources, m_includeDir}) {
            for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                const auto ext = it->path().extension().string();
                if (it->is_regular_file(ec) && (ext == ".h" || ext == ".hpp")) headers.push_back(it->path());
            }
            ec.clear();
        }
        std::sort(headers.begin(), headers.end());
        for (const auto& h : headers) headerHash = hashFile(h, hashString(h.generic_string(), headerHash));
    }
    std::string cmd;
    std::vector<fs::path> objects, tempObjects;
    std::vector<std::string> liveObjects; // nomes mantidos no cache
    size_t compiled = 0, reused = 0;
    auto addUnit = [&](const fs::path& src, bool isC) {
        const std::string& compiler = isC ? cc : cxx;
        std::uint64_t k = hashString(compiler + (isC ? cFlags : cxxFlags) + incs, headerHash);
        k = hashFile(src, hashString(src.generic_string(), k));
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(k));
        const std::string name = src.stem().string() + (isC ? "_c-" : "-") + hex + ".o";
        const fs::path obj = objDir / name;
        objects.push_back(obj);
        liveObjects.push_back(name);
        if (fs::is_regular_file(obj, ec)) {
            ++reused;
            return;
        }
        ++compiled;
        // Compila para um nome provisório e renomeia no fim: objeto pela metade nunca entra no cache.
        const fs::path tmp = objDir / (name + ".tmp");
        tempObjects.push_back(tmp);
        cmd += compiler + (isC ? cFlags : cxxFlags) + incs + " -c " + quote(src) + " -o " + quote(tmp) + " 2>&1 && mv -f " +
               quote(tmp) + " " + quote(obj) + " && ";
    };
    for (const auto& f : files) addUnit(f, false);
    for (const auto& f : cfiles) addUnit(f, true);
    const std::string linkFlags = cxx + " -std=c++17 -O2 -g -fPIC -shared -fvisibility=hidden";
    std::string linkLibs;
    for (const std::string& lib : readLinkList(m_sources)) linkLibs += " -l" + lib; // native/link.txt
    // Chave do link: nomes dos .o (já levam o hash de cada fonte), flags e bibliotecas.
    std::uint64_t lk = hashString(linkFlags + linkLibs, 1469598103934665603ull);
    for (const std::string& n : liveObjects) lk = hashString(n, lk);
    char lkHex[17];
    std::snprintf(lkHex, sizeof(lkHex), "%016llx", static_cast<unsigned long long>(lk));
    const std::string linkKey = lkHex;
    const fs::path game = m_buildDir / "game.so";
    bool linkReused = false;
    if (compiled == 0 && fs::is_regular_file(game, ec)) {
        std::ifstream kf(m_buildDir / "game.so.key");
        std::string stored;
        if (kf >> stored && stored == linkKey) linkReused = true;
    }
    if (linkReused) {
        cmd = "cp -f " + quote(game) + " " + quote(out); // mesma ligação: só uma cópia, sem compilar nem ligar
    } else {
        cmd += linkFlags;
        for (const auto& o : objects) cmd += " " + quote(o);
        cmd += " -o " + quote(out) + linkLibs;
    }
    cmd += " 2>&1";
    Logger::warning("[native] compiling %zu file(s), %zu from cache, link %s (%zu C++, %zu C)...", compiled, reused,
                    linkReused ? "reused" : "run", files.size(), cfiles.size());
    // Objetos de versões antigas dos mesmos fontes saem do cache.
    for (auto it = fs::directory_iterator(objDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (std::find(liveObjects.begin(), liveObjects.end(), n) == liveObjects.end()) {
            std::error_code rec;
            fs::remove(it->path(), rec);
        }
    }
    m_build = std::async(std::launch::async, [cmd, out, tempObjects, linkKey] {
        BuildResult r;
        r.linkKey = linkKey;
        const auto t0 = std::chrono::steady_clock::now();
        r.library = out;
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[512];
            while (fgets(buf, sizeof(buf), p)) r.output += buf;
            r.ok = pclose(p) == 0;
        } else {
            r.output = "could not run the compiler";
        }
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::error_code rec;
        for (const auto& o : tempObjects) fs::remove(o, rec);
        return r;
    });
#endif
}

void NativeModule::poll(float dt) {
#ifdef ERUPTION_NATIVE_RELOAD
    if (!m_engine) return;
    if (m_build.valid() && m_build.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        BuildResult r = m_build.get();
        Logger::warning("[native] build thread took %.0f ms", r.ms);
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
            std::ofstream(m_buildDir / "game.so.key") << r.linkKey;
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
    m_exposedV.clear();
    m_exposed.clear(); // os ponteiros eram da biblioteca que sai
    m_module = nullptr;
#ifdef ERUPTION_NATIVE_RELOAD
    if (m_handle) dlclose(m_handle);
#endif
    m_handle = nullptr;
    std::error_code ec;
    if (!m_loadedFile.empty() && m_loadedFile.filename() != "game.so" && m_loadedFile.extension() == ".so") fs::remove(m_loadedFile, ec);
    m_loadedFile.clear();
}

bool NativeModule::activate(const EruptionModule* mod, const fs::path& file, void* handle) {
    if (!mod || mod->version < 1 || mod->version > ERUPTION_NATIVE_API_VERSION) {
        Logger::error("[native] module built for another API version");
        return false;
    }
    const bool reloading = m_module != nullptr;
    unloadCurrent(true);
    m_handle = handle;
    m_module = mod;
    m_loadedFile = file;
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
                    file.filename().string().c_str(), m_exposed.size() + m_exposedV.size());
    // Recarga com o jogo rodando: os scripts ficam sabendo (native.on("reloaded")).
    // Uma conexao que o modulo nao passou pelo state (socket, thread) morre em
    // silencio com a biblioteca velha; o evento deixa o script reconectar ou
    // voltar a uma tela limpa em vez de esperar pacotes que nao chegam.
    if (reloading) {
        const EruptionValue kept = eruption_bool(reloaded ? 1 : 0);
        queueEvent("reloaded", &kept);
    }
    // Recarregado com o jogo rodando: continua de onde estava (sem start de novo).
    return true;
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
    if (!mod || mod->version < 1 || mod->version > ERUPTION_NATIVE_API_VERSION) {
        Logger::error("[native] %s: %s", library.filename().string().c_str(),
                      mod ? "built for another API version (rebuild with this engine's header)"
                          : "eruption_module() not found (export it with ERUPTION_MODULE_EXPORT)");
        dlclose(handle);
        return false;
    }
    return activate(mod, library, handle);
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
    drainEvents(); // emit() do módulo ou de sua thread: entrega no quadro, antes dos scripts
}

bool NativeModule::call(const std::string& name, const double* args, int count, double& out) const {
    const auto it = m_exposed.find(name);
    if (it == m_exposed.end()) return false;
    EruptionHost h = NativeHostCalls::host();
    out = it->second(&h, args, count);
    return true;
}

bool NativeModule::callValue(const std::string& name, const std::vector<Val>& args, Val& out) const {
    const auto it = m_exposedV.find(name);
    if (it == m_exposedV.end()) return false;
    std::deque<std::vector<EruptionPair>> arena;
    std::vector<EruptionValue> views(args.size());
    for (size_t i = 0; i < args.size(); ++i) viewOf(args[i], views[i], arena);
    EruptionValue result = eruption_nil();
    EruptionHost h = NativeHostCalls::host();
    it->second(&h, views.data(), int(views.size()), &result);
    out = Val{};
    if (!copyIn(result, out, 0)) {
        Logger::warning("[native] %s: invalid return value (too deep or malformed), returned nil", name.c_str());
        out = Val{};
    }
    return true;
}

bool NativeModule::queueEvent(const char* event, const EruptionValue* value) {
    if (!event || !*event) return false;
    Val v;
    if (value && !copyIn(*value, v, 0)) return false;
    std::lock_guard<std::mutex> lock(m_eventMutex);
    if (m_events.size() >= kMaxQueuedEvents) {
        if (m_droppedEvents++ == 0) Logger::warning("[native] event queue full (%zu); dropping events", kMaxQueuedEvents);
        return false;
    }
    m_events.emplace_back(event, std::move(v));
    return true;
}

void NativeModule::addListener(const std::string& event, int ref) { m_listeners[event].push_back(ref); }

void NativeModule::drainEvents() {
    if (!m_L) return;
    std::deque<std::pair<std::string, Val>> batch;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        batch.swap(m_events);
        m_droppedEvents = 0;
    }
    for (auto& ev : batch) {
        const auto it = m_listeners.find(ev.first);
        if (it == m_listeners.end()) continue;
        const std::vector<int> refs = it->second; // um ouvinte pode registrar outro
        for (int ref : refs) {
            lua_getref(m_L, ref);
            pushVal(m_L, ev.second);
            if (!ScriptHost::protectedCall(m_L, 1, ev.first.c_str())) continue;
        }
    }
}

namespace {

// native.<nome>: o __index devolve uma função que procura o nome a cada
// chamada, então continua valendo depois de uma recarga. Funções de valores
// (expose_value) têm prioridade sobre as de números (expose).
int callByName(lua_State* L, const char* name, int first) {
    NativeModule& nm = NativeModule::instance();
    const int top = lua_gettop(L);
    std::vector<Val> args;
    args.reserve(size_t(std::max(0, top - first + 1)));
    for (int i = first; i <= top; ++i) {
        args.emplace_back();
        if (!luaToVal(L, i, args.back(), 0)) luaL_errorL(L, "native.%s: argument %d is too deep or too large", name, i - first + 1);
    }
    Val out;
    if (nm.callValue(name, args, out)) {
        pushVal(L, out);
        return 1;
    }
    double nums[16];
    const int n = std::min(int(args.size()), 16);
    for (int i = 0; i < n; ++i) nums[i] = luaL_checknumber(L, first + i);
    double result = 0.0;
    if (!nm.call(name, nums, n, result))
        luaL_errorL(L, "native.%s: no C++ function with this name (is the module in native/ built?)", name);
    lua_pushnumber(L, result);
    return 1;
}

int nativeCall(lua_State* L) { return callByName(L, lua_tostring(L, lua_upvalueindex(1)), 1); }

int nativeGeneric(lua_State* L) { // native.call("nome", ...)
    const char* name = luaL_checkstring(L, 1);
    return callByName(L, name, 2);
}

int nativeOn(lua_State* L) { // native.on("evento", function(valor) ... end)
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    NativeModule::instance().addListener(event, ref);
    return 0;
}

int nativeIndex(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, nativeCall, "native", 1);
    return 1;
}

} // namespace

void NativeModule::registerApi(lua_State* L) {
    NativeModule& nm = instance();
    nm.m_listeners.clear(); // estado novo: as referências antigas morreram com o anterior
    nm.m_L = lua_mainthread(L);
    lua_newtable(L);
    lua_pushcfunction(L, nativeOn, "native.on");
    lua_setfield(L, -2, "on");
    lua_pushcfunction(L, nativeGeneric, "native.call");
    lua_setfield(L, -2, "call");
    lua_newtable(L);
    lua_pushcfunction(L, nativeIndex, "__index");
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "native");
}

const char* NativeModule::apiDefinitions() {
    return R"(
-- C++ functions of this project (native/*.cpp and *.c, include/eruption/native_module.h):
-- native.<name>(...) takes and returns plain values (numbers, booleans, strings,
-- buffers, nested tables). native.call("name", ...) is the same by string.
-- native.on("event", fn) listens to events the module emits (delivered once per
-- frame, before the scripts' on_update). Saving a .cpp file rebuilds and
-- reloads the module while the game runs (desktop); Android links it in.
-- After such a reload the engine sends native.on("reloaded", fn(stateKept)):
-- anything the module did not keep in host->state (an open socket, a thread)
-- is gone, so this is the place to reconnect or go back to a clean screen.
declare native: {
    on: (event: string, handler: (any) -> ()) -> (),
    call: (name: string, ...any) -> any,
    [string]: (...any) -> any,
}
)";
}

} // namespace eruption
