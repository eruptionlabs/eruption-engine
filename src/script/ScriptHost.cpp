#include "script/ScriptHost.hpp"

#include "script/ScriptApi.hpp"
#include "core/Engine.hpp"
#include "core/Input.hpp"
#include "core/Logger.hpp"
#include "game/PlayerController.hpp"

#include <lua.h>
#include <lualib.h>
#include <luacode.h>
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

void interruptCallback(lua_State* L, int gc) {
    if (gc >= 0 || !g_inCall) return;
    const auto elapsed = std::chrono::steady_clock::now() - g_callStart;
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

bool ScriptHost::init(Engine& engine, const fs::path& dir) {
    m_engine = &engine;
    m_dir = dir;
    std::error_code ec;
    fs::create_directories(m_dir, ec);

    L = luaL_newstate();
    if (!L) return false;
    s_current = this;
    luaL_openlibs(L);
    registerScriptApi(L, engine);
    lua_callbacks(L)->interrupt = interruptCallback;
    // Globais e bibliotecas ficam somente leitura; cada script ganha um
    // ambiente próprio por cima delas.
    luaL_sandbox(L);
    resetVars();

    m_native = luau_codegen_supported() != 0;
    if (m_native) luau_codegen_create(L);

    writeTypeDefinitions();
    scan();
    Logger::warning("Scripts: %zu file(s) in %s, %s", m_scripts.size(), m_dir.string().c_str(),
                    m_native ? "native code" : "interpreter");
    return true;
}

void ScriptHost::shutdown() {
    if (m_running) stop();
    for (auto& s : m_scripts) unload(s);
    m_scripts.clear();
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
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t bcSize = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &opts, &bcSize);
    const std::string chunk = "@" + s.path.generic_string();

    // Thread própria com ambiente isolado (sandbox) para o módulo.
    lua_State* T = lua_newthread(L);
    luaL_sandboxthread(T);
    const int loadStatus = luau_load(T, chunk.c_str(), bytecode, bcSize, 0);
    std::free(bytecode);
    if (loadStatus != 0) {
        Logger::error("%s", lua_tostring(T, -1));
        lua_pop(L, 1); // thread
        return false;
    }
    if (m_native) luau_codegen_compile(T, -1);

    std::vector<Handler> handlers;
    m_loadingHandlers = &handlers;
    lua_pushcfunction(T, errorHandler, "errorHandler");
    lua_insert(T, -2);
    g_callStart = std::chrono::steady_clock::now();
    g_inCall = true;
    const int status = lua_pcall(T, 0, 1, -2);
    g_inCall = false;
    m_loadingHandlers = nullptr;
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
    g_inCall = true;
    const int status = lua_pcall(L, nargs, 0, errIdx);
    g_inCall = false;
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
    const auto now = std::chrono::steady_clock::now();
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
    for (auto& s : m_scripts) startScript(s);
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
        }
    }
}

void ScriptHost::update(float dt) {
    if (!L || !m_running) return;
    const auto t0 = std::chrono::steady_clock::now();
    updateTweens(dt);
    for (auto it = m_messages.begin(); it != m_messages.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f) it = m_messages.erase(it);
        else ++it;
    }
    const float hour = m_engine->dayNightCycle().timeOfDay() * 24.0f;
    for (auto& s : m_scripts) {
        lua_pushnumber(L, dt);
        call(s, "on_update", 1);
        dispatch(s, dt, m_prevHour, hour);
    }
    m_prevHour = hour;
    m_lastUpdateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void ScriptHost::stop() {
    if (!L || !m_running) return;
    for (auto& s : m_scripts) {
        if (s.started) call(s, "on_stop", 0);
        s.started = false;
        s.failed = false;
    }
    m_running = false;
    m_tweens.clear();
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
}

} // namespace eruption
