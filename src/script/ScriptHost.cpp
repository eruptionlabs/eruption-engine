#include "script/ScriptHost.hpp"
#include <unordered_map>
#include <cstdio>

#include "script/ScriptApi.hpp"
#include "script/LuauChunk.hpp"
#include "script/ScriptDebugger.hpp"
#include "core/Engine.hpp"
#include "core/Input.hpp"
#include "core/Logger.hpp"
#include "game/PlayerController.hpp"

#include "script/NativeModule.hpp"
#include "core/Project.hpp"
#include "utils/EventLog.hpp"

#include <lua.h>
#include <lualib.h>
#include <luacodegen.h>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace eruption {

ScriptHost* ScriptHost::s_current = nullptr;

namespace {

namespace fs = std::filesystem;

// Relógio da chamada em andamento, lido pelo interrupt do Luau.
std::chrono::steady_clock::time_point g_callStart;
bool g_inCall = false;
// Tempo que o MOTOR gastou dentro da chamada (ver ScriptHost::NativeWork).
std::chrono::steady_clock::duration g_nativeTime{};

void interruptCallback(lua_State* L, int gc) {
    if (gc >= 0 || !g_inCall) return;
    const auto elapsed = std::chrono::steady_clock::now() - g_callStart - g_nativeTime;
    if (elapsed > std::chrono::milliseconds(ScriptHost::kWatchdogMs)) {
        g_inCall = false;
        luaL_errorL(L, "script took more than %d ms in one call (infinite loop?)", ScriptHost::kWatchdogMs);
    }
}

int errorHandler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    std::string full = msg ? msg : "(error object is not a string)";
    if (const char* trace = lua_debugtrace(L)) {
        full += "\nstack:\n";
        full += trace;
    }
    lua_pushstring(L, full.c_str());
    return 1;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Menor distância de um ponto a uma caixa (zero dentro dela).
float distanceToBox(const Vec3& p, const Vec3& bmin, const Vec3& bmax) {
    const Vec3 q = glm::clamp(p, bmin, bmax);
    return glm::length(p - q);
}

// A hora `target` foi atravessada entre `from` e `to` (relógio de 24 h).
bool crossedHour(float from, float to, float target) {
    if (from == to) return false;
    if (from < to) return target > from && target <= to;
    return target > from || target <= to; // virou a meia-noite
}

} // namespace

ScriptHost::ScriptHost() = default;
ScriptHost::~ScriptHost() = default;

void ScriptHost::resetWatchdog() {
    g_callStart = std::chrono::steady_clock::now();
    g_nativeTime = {};
}

ScriptHost::NativeWork::NativeWork() : m_t0(std::chrono::steady_clock::now()) {}
ScriptHost::NativeWork::~NativeWork() { g_nativeTime += std::chrono::steady_clock::now() - m_t0; }

void ScriptHost::reloadForDebugging() {
    if (!L) return;
    m_native = false;
    setLuauDebugCompile(true);
    for (auto& s : m_scripts) s.mtime = {}; // o próximo scan recarrega todos
    scan();
}

bool ScriptHost::createState() {
    L = luaL_newstate();
    if (!L) return false;
    s_current = this;
    luaL_openlibs(L);
    registerScriptApi(L, *m_engine);
    lua_callbacks(L)->interrupt = interruptCallback;
    // Globais e bibliotecas ficam somente leitura; cada script ganha um
    // ambiente próprio por cima delas.
    luaL_sandbox(L);
    m_varsRef = -1;
    resetVars();

    m_native = luau_codegen_supported() != 0;
    if (m_native) luau_codegen_create(L);
    return true;
}

void ScriptHost::reboot() {
    if (!L) return;
    if (m_running) stop();
    // Estado de módulo (locais do topo de cada .luau, tabelas de require)
    // vive na VM: só uma VM nova volta ao que era no boot. O depurador e o
    // módulo C++ ficam; eles não guardam nada da VM entre chamadas.
    for (auto& s : m_scripts) unload(s);
    m_scripts.clear();
    m_behaviors.clear();
    lua_close(L);
    L = nullptr;
    clearSession();
    m_engine->restoreScriptDefaults();
    if (!createState()) {
        Logger::error("Scripts: could not create a new Luau state for game.restart");
        return;
    }
    scan(); // todos os arquivos rodam de novo, do zero
}

bool ScriptHost::init(Engine& engine, const fs::path& dir) {
    m_engine = &engine;
    m_dir = dir;
    std::error_code ec;
    fs::create_directories(m_dir, ec);

    if (!createState()) return false;
    engine.captureScriptDefaults(); // o que game.restart devolve

#if ERUPTION_EDITOR
    // Porta do depurador (VS Code): ERUPTION_DEBUG_PORT, 0 desliga.
    if (const char* p = std::getenv("ERUPTION_DEBUG_PORT"); !p || std::atoi(p) != 0) {
        m_debugger = std::make_unique<ScriptDebugger>(*this);
        m_debugger->listen(p ? std::atoi(p) : 4711);
    }
#endif
    writeTypeDefinitions();
    scan();
    Logger::warning("Scripts: %zu file(s) in %s, %s", m_scripts.size(), m_dir.string().c_str(),
                    m_native ? "native code" : "interpreter");
    // Módulo C++ do projeto (native/): compila e carrega em segundo plano.
    NativeModule::instance().init(engine, project::isOpen() ? project::root() : project::engineRoot(),
                                  project::engineRoot());
    m_lastNativePoll = std::chrono::steady_clock::now();
    return true;
}

void ScriptHost::shutdown() {
    if (m_debugger) m_debugger->shutdown();
    m_debugger.reset();
    if (m_running) stop();
    NativeModule::instance().shutdown();
    for (auto& s : m_scripts) unload(s);
    m_scripts.clear();
    m_behaviors.clear();
    if (L) lua_close(L);
    L = nullptr;
    if (s_current == this) s_current = nullptr;
}

size_t ScriptHost::failedCount() const {
    return static_cast<size_t>(std::count_if(m_scripts.begin(), m_scripts.end(),
                                             [](const Script& s) { return s.failed; }));
}

bool ScriptHost::addHandler(Handler h) {
    if (!m_loadingHandlers) return false;
    m_loadingHandlers->push_back(std::move(h));
    return true;
}

void ScriptHost::showMessage(const std::string& text, float seconds) {
    m_messages.push_back({text, std::max(0.5f, seconds)});
    if (m_messages.size() > 4) m_messages.erase(m_messages.begin());
}

void ScriptHost::addTween(uint32_t model, const Vec3& move, float yawDegrees, float seconds) {
    const auto& insts = m_engine->modelRenderer().getInstances();
    if (model >= insts.size()) return;
    // Um movimento novo no mesmo modelo parte de onde o anterior parou.
    Mat4 start = insts[model].transform;
    for (auto it = m_tweens.begin(); it != m_tweens.end();) {
        if (it->model == model) it = m_tweens.erase(it);
        else ++it;
    }
    if (seconds <= 0.0f) {
        Mat4 m = glm::rotate(Mat4(1.0f), glm::radians(yawDegrees), Vec3(0, 1, 0)) * start;
        m[3] = Vec4(Vec3(start[3]) + move, 1.0f);
        m_engine->modelRenderer().setInstanceTransform(model, m);
        return;
    }
    m_tweens.push_back({model, start, move, yawDegrees, seconds, 0.0f});
}

void ScriptHost::updateTweens(float dt) {
    for (auto it = m_tweens.begin(); it != m_tweens.end();) {
        it->t = std::min(it->duration, it->t + dt);
        const float k = it->t / it->duration;
        const float e = k * k * (3.0f - 2.0f * k); // suave no começo e no fim
        Mat4 m = glm::rotate(Mat4(1.0f), glm::radians(it->yaw * e), Vec3(0, 1, 0)) * it->start;
        m[3] = Vec4(Vec3(it->start[3]) + it->move * e, 1.0f);
        m_engine->modelRenderer().setInstanceTransform(it->model, m);
        if (it->t >= it->duration) it = m_tweens.erase(it);
        else ++it;
    }
}

ScriptHost::Script* ScriptHost::scriptByPath(const std::filesystem::path& path) {
    for (auto& s : m_scripts)
        if (s.path == path) return &s;
    return nullptr;
}

bool ScriptHost::attachBehavior(ObjectId object, const std::string& module, std::string& error) {
    // Módulo com erro (pausado até salvar de novo) também aceita objetos: eles
    // esperam, como os outros do módulo, em vez de o erro passar para quem chamou.
    const Script* script = nullptr;
    std::string want = module;
    for (char& c : want)
        if (c == '\\') c = '/';
    if (want.size() > 5 && want.compare(want.size() - 5, 5, ".luau") == 0) want.resize(want.size() - 5);
    if (!want.empty() && want.find("..") == std::string::npos)
        script = scriptByPath((m_dir / (want + ".luau")).lexically_normal());
    if (!script) {
        const int ref = moduleRef(module, error);
        if (ref < 0) return false;
        for (const auto& s : m_scripts)
            if (s.moduleRef == ref) script = &s;
    }
    if (!script) {
        error = "attach: " + module + " is not loaded";
        return false;
    }
    for (const Behavior& b : m_behaviors)
        if (b.object == object && b.script == script->path) return true; // já tem
    m_behaviors.push_back({object, script->path, module, false});
    return true;
}

// Cada objeto com script: start na primeira vez, update todo quadro. Objeto
// destruído sai da lista; script com erro fica parado até salvar de novo
// (como os outros scripts), e os objetos dele esperam.
void ScriptHost::updateBehaviors(float dt) {
    ObjectWorld& objects = m_engine->sceneObjects().world();
    for (size_t k = 0; k < m_behaviors.size();) {
        Behavior& b = m_behaviors[k];
        Script* s = scriptByPath(b.script);
        if (!objects.alive(b.object) || !s) {
            m_behaviors.erase(m_behaviors.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        if (!b.started) {
            // Módulo com erro: start fica para quando o arquivo for salvo de novo.
            scriptPushObject(L, b.object);
            const bool started = call(*s, "start", 1);
            if (k < m_behaviors.size()) m_behaviors[k].started = started;
        }
        // start pode ter destruído o objeto ou acrescentado scripts (a lista
        // pode ter mudado de lugar): relê antes de seguir.
        if (k < m_behaviors.size() && m_behaviors[k].started && objects.alive(m_behaviors[k].object)) {
            if (Script* again = scriptByPath(m_behaviors[k].script)) {
                scriptPushObject(L, m_behaviors[k].object);
                lua_pushnumber(L, dt);
                call(*again, "update", 2);
            }
        }
        ++k;
    }
}

// Scripts de objeto que a cena declara (SceneEdits "scripts"): ligados a
// cada começo de jogo; o Stop os solta junto com os outros.
void ScriptHost::attachSceneScripts() {
    for (const auto& [object, module] : m_engine->sceneObjects().sceneScripts()) {
        std::string error;
        if (!attachBehavior(object, module, error)) Logger::error("%s", error.c_str());
    }
}

std::vector<std::string> ScriptHost::behaviorsOf(ObjectId object) const {
    std::vector<std::string> out;
    for (const Behavior& b : m_behaviors)
        if (b.object == object) out.push_back(b.module);
    return out;
}

std::vector<ObjectId> ScriptHost::behaviorObjects() const {
    std::vector<ObjectId> out;
    for (const Behavior& b : m_behaviors) out.push_back(b.object);
    return out;
}

void ScriptHost::stopBehaviors() {
    std::vector<Behavior> running;
    running.swap(m_behaviors);
    for (const Behavior& b : running) {
        Script* s = scriptByPath(b.script);
        if (!s || !b.started || !m_engine->sceneObjects().world().alive(b.object)) continue;
        scriptPushObject(L, b.object);
        call(*s, "stop", 1);
    }
}

void ScriptHost::resetVars() {
    if (m_varsRef >= 0) lua_unref(L, m_varsRef);
    lua_newtable(L);
    m_varsRef = lua_ref(L, -1);
    lua_pop(L, 1);
}

void ScriptHost::releaseHandlers(std::vector<Handler>& hs) {
    for (auto& h : hs)
        if (h.fnRef >= 0 && L) lua_unref(L, h.fnRef);
    hs.clear();
}

bool ScriptHost::load(Script& s, bool keepState) {
    const std::string source = readFile(s.path);
    const std::string chunk = "@" + s.path.generic_string();

    // Thread própria com ambiente isolado (sandbox) para o módulo.
    lua_State* T = lua_newthread(L);
    luaL_sandboxthread(T);
    if (!loadLuauChunk(T, chunk, source)) {
        Logger::error("%s", lua_tostring(T, -1));
        lua_pop(L, 1); // thread
        return false;
    }
    if (m_native) luau_codegen_compile(T, -1);
    // Guarda a função do arquivo: o depurador marca pontos de parada nela.
    if (s.chunkRef >= 0) lua_unref(L, s.chunkRef);
    lua_pushvalue(T, -1);
    s.chunkRef = lua_ref(T, -1);
    lua_pop(T, 1);
    if (m_debugger) m_debugger->applyBreakpoints(T, -1, s.path);

    std::vector<Handler> handlers;
    // EMPILHA. Um require() no topo de um arquivo carrega OUTRO arquivo no
    // meio desta carga; sem guardar o anterior, o on.* que vier depois do
    // require cairia no vazio ("must be called at the top level").
    std::vector<Handler>* const outerHandlers = m_loadingHandlers;
    m_loadingHandlers = &handlers;
    lua_pushcfunction(T, errorHandler, "errorHandler");
    lua_insert(T, -2);
    g_callStart = std::chrono::steady_clock::now();
    g_nativeTime = {};
    g_inCall = true;
    const int status = lua_pcall(T, 0, 1, -2);
    g_inCall = false;
    m_loadingHandlers = outerHandlers;
    if (status != 0) {
        Logger::error("%s", lua_tostring(T, -1));
        releaseHandlers(handlers);
        lua_pop(L, 1);
        return false;
    }
    if (lua_isnil(T, -1)) {
        // Arquivo só de eventos (on.*): o módulo é uma tabela vazia.
        lua_pop(T, 1);
        lua_newtable(T);
    } else if (!lua_istable(T, -1)) {
        Logger::error("%s: a script must return a table or nothing", s.path.generic_string().c_str());
        releaseHandlers(handlers);
        lua_pop(L, 1);
        return false;
    }
    lua_xmove(T, L, 1); // tabela do módulo vai para o estado principal

    if (keepState && s.moduleRef >= 0) {
        lua_getref(L, s.moduleRef);
        lua_getfield(L, -1, "state");
        if (!lua_isnil(L, -1)) lua_setfield(L, -3, "state");
        else lua_pop(L, 1);
        lua_pop(L, 1);
    }
    const int ref = lua_ref(L, -1);
    lua_pop(L, 2); // módulo e thread
    if (s.moduleRef >= 0) lua_unref(L, s.moduleRef);
    s.moduleRef = ref;
    releaseHandlers(s.handlers);
    s.handlers = std::move(handlers);
    s.failed = false;
    return true;
}

// MODULOS. Todo script ja' devolve uma tabela (o `moduleRef`); require so'
// da' acesso a ela por nome. Sem isso dois arquivos nao trocam nada: cada um
// roda num ambiente isolado, e dado grande (tabela de classe, de inimigo)
// teria que caber tudo dentro de um arquivo so'.
//
// Nome e' o caminho sem extensao, relativo a' pasta de scripts:
// require("dados") ou require("tabelas/itens"). Ciclo e' erro, nao travada.
int ScriptHost::moduleRef(const std::string& name, std::string& error) {
    namespace fs = std::filesystem;
    std::string want = name;
    for (char& c : want)
        if (c == '\\') c = '/';
    while (!want.empty() && want.front() == '/') want.erase(want.begin());
    if (want.size() > 5 && want.compare(want.size() - 5, 5, ".luau") == 0) want.resize(want.size() - 5);
    if (want.empty() || want.find("..") != std::string::npos) {
        error = "require: invalid module name \"" + name + "\"";
        return -1;
    }

    const fs::path target = (m_dir / (want + ".luau")).lexically_normal();
    auto it = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const Script& s) {
        return s.path.lexically_normal() == target;
    });
    if (it == m_scripts.end()) {
        std::error_code ec;
        if (!fs::is_regular_file(target, ec)) {
            error = "require: there is no script " + want + ".luau in " + m_dir.generic_string();
            return -1;
        }
        // Existe no disco mas o scan ainda nao pegou: entra agora.
        m_scripts.emplace_back();
        Script& s = m_scripts.back();
        s.path = target;
        s.mtime = fs::last_write_time(target, ec);
        if (!load(s, false)) {
            error = "require: " + want + ".luau failed to load";
            return -1;
        }
        if (m_running) startScript(s);
        return s.moduleRef;
    }
    if (it->failed || it->moduleRef < 0) {
        error = "require: " + want + ".luau is not loaded (it has an error)";
        return -1;
    }
    return it->moduleRef;
}

bool ScriptHost::callRef(Script& s, int fnRef, int nargs) {
    // Os argumentos já estão no topo da pilha.
    if (s.failed) {
        lua_pop(L, nargs);
        return false;
    }
    lua_getref(L, fnRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1 + nargs);
        return true;
    }
    lua_insert(L, -(nargs + 1));
    lua_pushcfunction(L, errorHandler, "errorHandler");
    lua_insert(L, -(nargs + 2));
    const int errIdx = lua_gettop(L) - nargs - 1;
    g_callStart = std::chrono::steady_clock::now();
    g_nativeTime = {};
    g_inCall = true;
    const int status = lua_pcall(L, nargs, 0, errIdx);
    g_inCall = false;
    profileCall(s, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_callStart).count());
    if (status != 0) {
        Logger::error("%s", lua_tostring(L, -1));
        Logger::error("%s: script paused after this error; save the file to run it again",
                      s.path.filename().string().c_str());
        lua_pop(L, 1);
        s.failed = true;
    }
    lua_pop(L, 1); // errorHandler
    return status == 0;
}

bool ScriptHost::call(Script& s, const char* fn, int nargs) {
    if (s.moduleRef < 0 || s.failed) {
        lua_pop(L, nargs);
        return false;
    }
    lua_getref(L, s.moduleRef);
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1 + nargs);
        return true;
    }
    const int tmp = lua_ref(L, -1);
    lua_pop(L, 1);
    const bool ok = callRef(s, tmp, nargs);
    lua_unref(L, tmp);
    return ok;
}

void ScriptHost::unload(Script& s) {
    releaseHandlers(s.handlers);
    if (s.moduleRef >= 0 && L) lua_unref(L, s.moduleRef);
    s.moduleRef = -1;
    if (s.chunkRef >= 0 && L) lua_unref(L, s.chunkRef);
    s.chunkRef = -1;
}

void ScriptHost::startScript(Script& s) {
    s.started = true;
    for (auto& h : s.handlers) {
        h.timer = 0.0;
        h.flag = false;
    }
    call(s, "on_start", 0);
    for (auto& h : s.handlers)
        if (h.kind == EventKind::Start) callRef(s, h.fnRef, 0);
}

void ScriptHost::scan() {
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::recursive_directory_iterator(m_dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".luau" &&
            e.path().filename().string().find(".d.luau") == std::string::npos)
            found.push_back(e.path());
    }
    std::sort(found.begin(), found.end());

    // Removidos
    for (auto it = m_scripts.begin(); it != m_scripts.end();) {
        if (std::find(found.begin(), found.end(), it->path) == found.end()) {
            if (m_running && it->started) call(*it, "on_stop", 0);
            unload(*it);
            Logger::warning("Script removed: %s", it->path.filename().string().c_str());
            it = m_scripts.erase(it);
        } else {
            ++it;
        }
    }
    // Novos e alterados
    for (const auto& p : found) {
        const auto mtime = fs::last_write_time(p, ec);
        auto it = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const Script& s) { return s.path == p; });
        if (it == m_scripts.end()) {
            m_scripts.emplace_back();
            Script& s = m_scripts.back();
            s.path = p;
            s.mtime = mtime;
            if (load(s, false) && m_running) startScript(s);
            continue;
        }
        if (it->mtime == mtime) continue;
        it->mtime = mtime;
        int keptOld = -1;
        if (it->moduleRef >= 0) {
            lua_getref(L, it->moduleRef);
            keptOld = lua_ref(L, -1);
            lua_pop(L, 1);
        }
        if (load(*it, true)) {
            Logger::warning("Script reloaded: %s", it->path.filename().string().c_str());
            if (m_running) {
                if (!it->started) {
                    startScript(*it);
                } else if (keptOld >= 0) {
                    lua_getref(L, keptOld);
                    call(*it, "on_reload", 1);
                }
            }
        }
        if (keptOld >= 0) lua_unref(L, keptOld);
    }
}

void ScriptHost::poll() {
    if (!L) return;
    if (m_debugger) m_debugger->poll();
    const auto now = std::chrono::steady_clock::now();
    NativeModule::instance().poll(std::chrono::duration<float>(now - m_lastNativePoll).count());
    m_lastNativePoll = now;
    if (now - m_lastScan < std::chrono::milliseconds(500)) return;
    m_lastScan = now;
    scan();
}

void ScriptHost::start() {
    if (!L || m_running) return;
    m_running = true;
    resetVars();
    m_messages.clear();
    m_tweens.clear();
    m_prevHour = m_engine->dayNightCycle().timeOfDay() * 24.0f;
    const bool resume = m_resume;
    m_resume = false;
    resetScriptObjectData(L); // obj.data de um jogo anterior não vale
    if (resume && !m_resumeVars.empty()) importVars(m_resumeVars);
    // Depois das variáveis: as de um jogo carregado que guardam objeto são
    // refeitas aqui, já com os objetos do mapa.
    mapReady();
    openTelemetryFromEnv(); // antes do on.start, que ja' pode emitir
    NativeModule::instance().start();
    for (auto& s : m_scripts) {
        if (!resume) {
            startScript(s);
            continue;
        }
        s.started = true; // continua: só os relógios dos eventos recomeçam
        for (auto& h : s.handlers) {
            h.timer = 0.0;
            h.flag = false;
        }
    }
}

void ScriptHost::dispatch(Script& s, float dt, float prevHour, float hour) {
    PlayerController* player = m_engine->m_playerController;
    for (auto& h : s.handlers) {
        if (s.failed) return;
        switch (h.kind) {
            case EventKind::Start:
                break;
            case EventKind::Update:
                lua_pushnumber(L, dt);
                callRef(s, h.fnRef, 1);
                break;
            case EventKind::Every:
                h.timer += dt;
                if (h.number > 0.0 && h.timer >= h.number) {
                    h.timer -= h.number * std::floor(h.timer / h.number);
                    callRef(s, h.fnRef, 0);
                }
                break;
            case EventKind::Key:
                if (Input::isKeyPressed(scriptKeyFromName(h.text.c_str()))) callRef(s, h.fnRef, 0);
                break;
            case EventKind::Near: {
                const int idx = scriptFindModel(h.text);
                if (!player || idx < 0) break;
                const ModelInstance& inst = m_engine->modelRenderer().getInstances()[static_cast<size_t>(idx)];
                const bool near = distanceToBox(player->pos(), inst.worldAabbMin, inst.worldAabbMax) <= h.number;
                if (near && !h.flag) callRef(s, h.fnRef, 0);
                h.flag = near;
                break;
            }
            case EventKind::Hour:
                if (crossedHour(prevHour, hour, static_cast<float>(h.number))) callRef(s, h.fnRef, 0);
                break;
            case EventKind::Enter:
            case EventKind::Leave: {
                // Área de gatilho: o pé do jogador dentro da caixa.
                const int a = m_engine->findArea(h.text);
                const bool inside = player && a >= 0 && m_engine->areas()[static_cast<size_t>(a)].contains(player->pos());
                if (h.kind == EventKind::Enter ? (inside && !h.flag) : (!inside && h.flag)) callRef(s, h.fnRef, 0);
                h.flag = inside;
                break;
            }
        }
    }
}

// ERUPTION_TEST_SCRIPT_PROFILE=1 (bancada): tempo de CPU dos handlers por
// arquivo de script; a cada segundo o log lista os mais caros (soma e a
// chamada mais longa). Separa o custo do jogo (scripts) do custo do motor.
static const bool kScriptProfile = std::getenv("ERUPTION_TEST_SCRIPT_PROFILE") != nullptr;

void ScriptHost::profileCall(const Script& s, double ms) {
    if (!kScriptProfile) return;
    auto& p = m_profile[s.path.filename().string()];
    p.first += ms;
    p.second = std::max(p.second, ms);
}

void ScriptHost::flushProfile() {
    if (!kScriptProfile || m_profile.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_profileSince < std::chrono::seconds(1)) return;
    std::vector<std::pair<std::string, std::pair<double, double>>> rows(m_profile.begin(), m_profile.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    std::string line;
    double total = 0.0;
    for (const auto& r : rows) total += r.second.first;
    for (size_t i = 0; i < rows.size() && i < 6; ++i) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s %.1f ms (max %.1f)", i ? " | " : "", rows[i].first.c_str(),
                      rows[i].second.first, rows[i].second.second);
        line += buf;
    }
    ERUPTION_LOG_WARN("[SCRIPTPROF] 1 s: total %.1f ms | %s", total, line.c_str());
    m_profile.clear();
    m_profileSince = now;
}

void ScriptHost::update(float dt) {
    if (!L || !m_running) return;
    flushProfile();
    const auto t0 = std::chrono::steady_clock::now();
    EventLog::instance().advance(dt); // t e f dos eventos deste quadro
    updateTweens(dt);
    for (auto it = m_messages.begin(); it != m_messages.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f) it = m_messages.erase(it);
        else ++it;
    }
    const float hour = m_engine->dayNightCycle().timeOfDay() * 24.0f;
    NativeModule::instance().update(dt);
    for (auto& s : m_scripts) {
        lua_pushnumber(L, dt);
        call(s, "on_update", 1);
        dispatch(s, dt, m_prevHour, hour);
    }
    m_inBehaviors = true;
    updateBehaviors(dt);
    m_inBehaviors = false;
    // game.load pedido por um script de objeto: agora, com a lista parada.
    if (!m_deferredLoad.empty()) {
        const std::string slot = std::move(m_deferredLoad);
        m_deferredLoad.clear();
        std::string error;
        if (!loadGame(slot, error)) Logger::error("game.load: %s", error.c_str());
    }
    m_prevHour = hour;
    m_lastUpdateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void ScriptHost::stop() {
    if (!L || !m_running) return;
    stopBehaviors();
    for (auto& s : m_scripts) {
        if (s.started) call(s, "on_stop", 0);
        s.started = false;
        s.failed = false;
    }
    NativeModule::instance().stop();
    m_running = false;
    m_tweens.clear();
    releaseScriptObjects(); // o que os scripts criaram some com o Stop
    m_pendingVars.clear();  // jogo carregado que não chegou ao mapa novo não vale no próximo Play
    // Etiquetas, pais e ligado/desligado voltam ao que a cena declara.
    m_engine->sceneObjects().restoreAuthored(*m_engine);
    m_engine->audio().stopAll(); // Stop do editor: o som do jogo para junto
    m_engine->vision().reset();  // e a visao de equipe (fontes do jogo) desliga
    EventLog::instance().close();  // e o registro de eventos sai completo
    m_messages.clear();
    m_lastUpdateMs = 0.0f;
}

void ScriptHost::drawMessages(float x, float y, float w, float h) const {
    if (m_messages.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float by = y + h - 40.0f;
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
        const float alpha = std::min(1.0f, it->remaining / 0.3f);
        const ImVec2 ts = ImGui::CalcTextSize(it->text.c_str(), nullptr, false, w * 0.7f);
        const ImVec2 p0(x + (w - ts.x) * 0.5f - 16.0f, by - ts.y - 12.0f);
        const ImVec2 p1(x + (w + ts.x) * 0.5f + 16.0f, by);
        dl->AddRectFilled(p0, p1, IM_COL32(15, 15, 16, static_cast<int>(210 * alpha)), 6.0f);
        dl->AddRect(p0, p1, IM_COL32(255, 60, 40, static_cast<int>(200 * alpha)), 6.0f);
        dl->AddText(nullptr, 0.0f, ImVec2(p0.x + 16.0f, p0.y + 6.0f), IM_COL32(240, 240, 240, static_cast<int>(255 * alpha)),
                    it->text.c_str(), nullptr, w * 0.7f);
        by = p0.y - 8.0f;
    }
}

fs::path ScriptHost::createScript(const std::string& name) {
    std::string base = name.empty() ? "new_script" : name;
    fs::path p = m_dir / (base + ".luau");
    for (int i = 2; fs::exists(p); ++i) p = m_dir / (base + "_" + std::to_string(i) + ".luau");
    std::ofstream f(p);
    f << "--!strict\n"
         "-- Runs while the game is playing. Save the file to reload it.\n"
         "local M = {}\n"
         "\n"
         "-- Kept when the file is reloaded.\n"
         "M.state = {\n"
         "    time = 0,\n"
         "}\n"
         "\n"
         "function M.on_start()\n"
         "    log(\"started\")\n"
         "end\n"
         "\n"
         "function M.on_update(dt: number)\n"
         "    M.state.time += dt\n"
         "end\n"
         "\n"
         "function M.on_stop()\n"
         "end\n"
         "\n"
         "return M\n";
    return p;
}

void ScriptHost::writeTypeDefinitions() {
    // Tipos da API para o VS Code (extensão luau-lsp) e a configuração da
    // pasta, regravados só quando mudam.
    auto writeIfChanged = [](const fs::path& p, const std::string& content) {
        if (fs::exists(p) && readFile(p) == content) return;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary) << content;
    };
    writeIfChanged(m_dir / "eruption.d.luau", scriptApiDefinitions());
    writeIfChanged(m_dir / ".luaurc", "{\n    \"languageMode\": \"strict\"\n}\n");
    writeIfChanged(m_dir / ".vscode" / "settings.json",
                   "{\n"
                   "    \"luau-lsp.types.definitionFiles\": [\"eruption.d.luau\"],\n"
                   "    \"luau-lsp.platform.type\": \"standard\",\n"
                   "    \"luau-lsp.sourcemap.enabled\": false\n"
                   "}\n");
    writeIfChanged(m_dir / ".vscode" / "extensions.json",
                   "{\n    \"recommendations\": [\"JohnnyMorganz.luau-lsp\"]\n}\n");
    // Depurador: a extensão tools/vscode-eruption liga este tipo à porta do editor.
    writeIfChanged(m_dir / ".vscode" / "launch.json",
                   "{\n"
                   "    \"version\": \"0.2.0\",\n"
                   "    \"configurations\": [\n"
                   "        { \"type\": \"eruption\", \"request\": \"attach\", \"name\": \"Debug game scripts\", \"port\": " +
                       std::to_string(m_debugger ? m_debugger->port() : 4711) + " }\n"
                   "    ]\n"
                   "}\n");
}

} // namespace eruption
