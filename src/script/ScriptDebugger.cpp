#include "script/ScriptDebugger.hpp"

#include "core/Logger.hpp"
#include "script/ScriptHost.hpp"

#include <GLFW/glfw3.h>
#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace eruption {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// variablesReference: tabelas abertas são 1..; o resto codifica o que é.
constexpr int kLocalsBase = 1000000;   // + nível da pilha
constexpr int kUpvaluesBase = 2000000; // + nível da pilha
constexpr int kGameVars = 3000000;

} // namespace

ScriptDebugger* ScriptDebugger::s_active = nullptr;

ScriptDebugger::ScriptDebugger(ScriptHost& host) : m_host(host) {}

ScriptDebugger::~ScriptDebugger() { shutdown(); }

std::string ScriptDebugger::normalize(const std::string& path) {
    std::error_code ec;
    std::string p = path;
    if (!p.empty() && p[0] == '@') p.erase(0, 1);
    const fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return (ec ? fs::path(p) : abs).generic_string();
}

bool ScriptDebugger::listen(int port) {
    m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen < 0) return false;
    const int one = 1;
    ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // só esta máquina
    if (::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(m_listen, 1) != 0) {
        Logger::warning("Script debugger: port %d is busy; VS Code debugging is off", port);
        ::close(m_listen);
        m_listen = -1;
        return false;
    }
    ::fcntl(m_listen, F_SETFL, ::fcntl(m_listen, F_GETFL, 0) | O_NONBLOCK);
    m_port = port;
    return true;
}

void ScriptDebugger::shutdown() {
    if (m_client >= 0) {
        event("terminated", "{}");
        ::close(m_client);
    }
    if (m_listen >= 0) ::close(m_listen);
    m_client = m_listen = -1;
    if (s_active == this) s_active = nullptr;
}

void ScriptDebugger::accept() {
    if (m_listen < 0 || m_client >= 0) return;
    const int fd = ::accept(m_listen, nullptr, nullptr);
    if (fd < 0) return;
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    m_client = fd;
    m_inbox.clear();
    m_seq = 1;
    s_active = this;
    lua_Callbacks* cb = lua_callbacks(m_host.L);
    cb->debugbreak = &ScriptDebugger::onBreak;
    cb->debugstep = &ScriptDebugger::onStep;
    // Pontos de parada e variáveis locais pedem o interpretador sem otimização.
    m_host.reloadForDebugging();
    Logger::warning("Script debugger: VS Code connected");
}

void ScriptDebugger::poll() {
    accept();
    if (m_client >= 0) readMessages(0);
}

bool ScriptDebugger::readMessages(int timeoutMs) {
    if (m_client < 0) return false;
    pollfd pfd{m_client, POLLIN, 0};
    if (::poll(&pfd, 1, timeoutMs) > 0) {
        char buf[8192];
        for (;;) {
            const ssize_t n = ::recv(m_client, buf, sizeof(buf), 0);
            if (n > 0) {
                m_inbox.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                // VS Code fechou: tira os pontos de parada e solta o jogo.
                ::close(m_client);
                m_client = -1;
                for (auto& [file, lines] : m_breakpoints) lines.clear();
                for (auto& s : m_host.m_scripts) {
                    if (s.chunkRef < 0) continue;
                    lua_getref(m_host.L, s.chunkRef);
                    applyBreakpoints(m_host.L, -1, s.path);
                    lua_pop(m_host.L, 1);
                }
                m_step = Step::None;
                m_resume = true;
                Logger::warning("Script debugger: VS Code disconnected");
                return false;
            }
            break;
        }
    }
    // Mensagens: "Content-Length: N\r\n\r\n" + N bytes de JSON.
    for (;;) {
        const size_t headerEnd = m_inbox.find("\r\n\r\n");
        if (headerEnd == std::string::npos) break;
        const size_t lenPos = m_inbox.find("Content-Length:");
        if (lenPos == std::string::npos || lenPos > headerEnd) {
            m_inbox.erase(0, headerEnd + 4);
            continue;
        }
        const size_t len = std::strtoul(m_inbox.c_str() + lenPos + 15, nullptr, 10);
        if (m_inbox.size() < headerEnd + 4 + len) break;
        const std::string body = m_inbox.substr(headerEnd + 4, len);
        m_inbox.erase(0, headerEnd + 4 + len);
        handle(body);
    }
    return true;
}

void ScriptDebugger::send(const std::string& body) {
    if (m_client < 0) return;
    const std::string msg = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < msg.size()) {
        const ssize_t n = ::send(m_client, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{m_client, POLLOUT, 0};
            ::poll(&pfd, 1, 100);
        } else {
            return;
        }
    }
}

void ScriptDebugger::respond(const std::string& requestJson, bool ok, const std::string& bodyJson, const std::string& message) {
    const json req = json::parse(requestJson, nullptr, false);
    json r;
    r["seq"] = m_seq++;
    r["type"] = "response";
    r["request_seq"] = req.value("seq", 0);
    r["command"] = req.value("command", std::string());
    r["success"] = ok;
    if (!message.empty()) r["message"] = message;
    r["body"] = json::parse(bodyJson, nullptr, false);
    send(r.dump());
}

void ScriptDebugger::event(const std::string& name, const std::string& bodyJson) {
    json e;
    e["seq"] = m_seq++;
    e["type"] = "event";
    e["event"] = name;
    e["body"] = json::parse(bodyJson, nullptr, false);
    send(e.dump());
}

void ScriptDebugger::applyBreakpoints(lua_State* L, int funcIndex, const fs::path& file) {
    if (!lua_isfunction(L, funcIndex)) return;
    const std::string key = normalize(file.string());
    auto it = m_breakpoints.find(key);
    // Desliga as linhas de antes e liga as atuais (a função pode ser a mesma).
    const int idx = lua_absindex(L, funcIndex);
    for (int line : m_applied[key]) lua_breakpoint(L, idx, line, 0);
    m_applied[key].clear();
    if (it == m_breakpoints.end()) return;
    for (int line : it->second) {
        const int actual = lua_breakpoint(L, idx, line, 1);
        if (actual > 0) m_applied[key].insert(actual);
    }
}

std::string ScriptDebugger::sourceOf(lua_State* L, int level, int* line) const {
    lua_Debug ar{};
    if (!lua_getinfo(L, level, "sl", &ar)) return {};
    if (line) *line = ar.currentline;
    return ar.source ? ar.source : "";
}

bool ScriptDebugger::shouldStop(lua_State* L) {
    int line = -1;
    const std::string src = sourceOf(L, 0, &line);
    if (line < 0 || src.empty() || src[0] != '@') return false; // dentro de função C
    const int depth = lua_stackdepth(L);
    const bool moved = line != m_stepLine || src != m_stepSource;
    if (m_step == Step::In) return moved || depth != m_stepDepth;
    if (m_step == Step::Over) return depth < m_stepDepth || (depth == m_stepDepth && moved);
    if (m_step == Step::Out) return depth < m_stepDepth;
    return false;
}

void ScriptDebugger::onBreak(lua_State* L, lua_Debug*) {
    ScriptDebugger* d = s_active;
    if (!d || d->m_client < 0) return;
    if (d->m_step == Step::None) {
        d->pause(L, "breakpoint");
        return;
    }
    // Parada de um passo (ou um ponto do usuário no caminho).
    if (d->shouldStop(L)) d->pause(L, "step");
}

void ScriptDebugger::onStep(lua_State* L, lua_Debug*) {
    ScriptDebugger* d = s_active;
    if (!d || d->m_step == Step::None) return;
    if (d->m_step == Step::Pause) {
        d->pause(L, "pause");
        return;
    }
    if (d->shouldStop(L)) d->pause(L, "step");
}

void ScriptDebugger::armStep(lua_State* L) {
    // Linhas da função atual (próxima e, em laço, as de antes) e da que chamou.
    auto arm = [&](int level, int fromLine) {
        lua_Debug ar{};
        if (!lua_getinfo(L, level, "slf", &ar)) return;
        if (!ar.source || ar.source[0] != '@' || !lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        const int ref = lua_ref(L, -1);
        const int first = std::max(1, ar.linedefined), last = std::max(fromLine, first) + 400;
        for (int line = first; line <= last; ++line) {
            const int actual = lua_breakpoint(L, -1, line, 1);
            if (actual == line) m_temp.push_back({ref, actual});
        }
        lua_pop(L, 1);
        m_temp.push_back({ref, -1}); // guarda a referência para soltar depois
    };
    if (m_step != Step::Out) arm(0, m_stepLine);
    arm(1, 0);
}

void ScriptDebugger::clearTemp(lua_State* L) {
    if (m_temp.empty()) return;
    std::set<int> refs;
    for (const auto& t : m_temp) {
        refs.insert(t.funcRef);
        if (t.line < 0) continue;
        lua_getref(L, t.funcRef);
        lua_breakpoint(L, -1, t.line, 0);
        lua_pop(L, 1);
    }
    for (int ref : refs) lua_unref(L, ref);
    m_temp.clear();
    // Uma linha temporária pode ter sido também do usuário: marca as dele de novo.
    for (auto& s : m_host.m_scripts) {
        if (s.chunkRef < 0) continue;
        lua_getref(m_host.L, s.chunkRef);
        applyBreakpoints(m_host.L, -1, s.path);
        lua_pop(m_host.L, 1);
    }
}

void ScriptDebugger::releaseTables() {
    for (int ref : m_tableRefs) lua_unref(m_host.L, ref);
    m_tableRefs.clear();
}

void ScriptDebugger::pause(lua_State* L, const char* reason) {
    if (m_paused) return;
    lua_singlestep(L, 0);
    clearTemp(L);
    m_step = Step::None;
    m_paused = true;
    m_resume = false;
    m_pausedL = L;
    json body;
    body["reason"] = reason;
    body["threadId"] = 1;
    body["allThreadsStopped"] = true;
    event("stopped", body.dump());
    // O jogo fica parado aqui até o VS Code mandar continuar (a janela segue
    // respondendo ao sistema).
    while (!m_resume && m_client >= 0) {
        readMessages(15);
        glfwPollEvents();
    }
    releaseTables();
    m_paused = false;
    m_pausedL = nullptr;
    if (m_step != Step::None) {
        m_stepSource = sourceOf(L, 0, &m_stepLine);
        m_stepDepth = lua_stackdepth(L);
        lua_singlestep(L, 1); // vale para as funções chamadas daqui em diante
        armStep(L);           // e as linhas desta função e da que chamou
    }
    ScriptHost::resetWatchdog(); // o tempo parado não conta como laço infinito
}

namespace {

// Valor no topo da pilha como texto (e abre tabelas para o VS Code expandir).
json describe(lua_State* L, std::vector<int>& refs) {
    json v;
    v["variablesReference"] = 0;
    switch (lua_type(L, -1)) {
        case LUA_TNIL: v["value"] = "nil"; v["type"] = "nil"; break;
        case LUA_TBOOLEAN: v["value"] = lua_toboolean(L, -1) ? "true" : "false"; v["type"] = "boolean"; break;
        case LUA_TNUMBER: {
            char b[64];
            std::snprintf(b, sizeof(b), "%.14g", lua_tonumber(L, -1));
            v["value"] = b;
            v["type"] = "number";
            break;
        }
        case LUA_TSTRING: v["value"] = "\"" + std::string(lua_tostring(L, -1)) + "\""; v["type"] = "string"; break;
        case LUA_TVECTOR: {
            const float* f = lua_tovector(L, -1);
            char b[96];
            std::snprintf(b, sizeof(b), "vector(%g, %g, %g)", f[0], f[1], f[2]);
            v["value"] = b;
            v["type"] = "vector";
            break;
        }
        case LUA_TTABLE: {
            const int n = lua_objlen(L, -1);
            v["value"] = n > 0 ? "table [" + std::to_string(n) + "]" : std::string("table");
            v["type"] = "table";
            lua_pushvalue(L, -1);
            refs.push_back(lua_ref(L, -1));
            lua_pop(L, 1);
            v["variablesReference"] = static_cast<int>(refs.size());
            break;
        }
        case LUA_TFUNCTION: v["value"] = "function"; v["type"] = "function"; break;
        default: v["value"] = luaL_typename(L, -1); v["type"] = luaL_typename(L, -1); break;
    }
    return v;
}

} // namespace

void ScriptDebugger::handle(const std::string& text) {
    const json req = json::parse(text, nullptr, false);
    if (!req.is_object() || req.value("type", std::string()) != "request") return;
    const std::string cmd = req.value("command", std::string());
    const json args = req.value("arguments", json::object());
    lua_State* L = m_pausedL ? m_pausedL : m_host.L;

    if (cmd == "initialize") {
        respond(text, true, R"({"supportsConfigurationDoneRequest":true,"supportsEvaluateForHovers":true})");
        event("initialized", "{}");
    } else if (cmd == "attach" || cmd == "launch" || cmd == "configurationDone" || cmd == "setExceptionBreakpoints") {
        respond(text, true, "{}");
    } else if (cmd == "setBreakpoints") {
        const std::string file = normalize(args.value("source", json::object()).value("path", std::string()));
        std::set<int>& lines = m_breakpoints[file];
        lines.clear();
        for (const auto& b : args.value("breakpoints", json::array())) lines.insert(b.value("line", 0));
        // Marca já nos scripts carregados; a linha real vai na resposta.
        std::map<int, int> actual;
        for (auto& s : m_host.m_scripts) {
            if (s.chunkRef < 0 || normalize(s.path.string()) != file) continue;
            lua_getref(m_host.L, s.chunkRef);
            applyBreakpoints(m_host.L, -1, s.path);
            for (int line : lines) actual[line] = lua_breakpoint(m_host.L, -1, line, 1);
            lua_pop(m_host.L, 1);
        }
        json out = json::array();
        for (int line : lines) {
            const auto it = actual.find(line);
            const bool loaded = it != actual.end();
            out.push_back({{"verified", !loaded || it->second > 0}, {"line", loaded && it->second > 0 ? it->second : line}});
        }
        respond(text, true, json{{"breakpoints", out}}.dump());
    } else if (cmd == "threads") {
        respond(text, true, R"({"threads":[{"id":1,"name":"Game scripts"}]})");
    } else if (cmd == "stackTrace") {
        json frames = json::array();
        if (m_paused) {
            lua_Debug ar{};
            for (int level = 0; lua_getinfo(L, level, "sln", &ar); ++level) {
                if (!ar.source || ar.source[0] != '@') continue; // função C
                const std::string path = normalize(ar.source);
                frames.push_back({{"id", level + 1},
                                  {"name", ar.name ? ar.name : (level == 0 ? "(this line)" : "(file)")},
                                  {"source", {{"name", fs::path(path).filename().string()}, {"path", path}}},
                                  {"line", ar.currentline},
                                  {"column", 1}});
            }
        }
        respond(text, true, json{{"stackFrames", frames}, {"totalFrames", frames.size()}}.dump());
    } else if (cmd == "scopes") {
        const int level = args.value("frameId", 1) - 1;
        json scopes = json::array({{{"name", "Locals"}, {"variablesReference", kLocalsBase + level}, {"expensive", false}},
                                   {{"name", "Upvalues"}, {"variablesReference", kUpvaluesBase + level}, {"expensive", false}},
                                   {{"name", "Game vars"}, {"variablesReference", kGameVars}, {"expensive", false}}});
        respond(text, true, json{{"scopes", scopes}}.dump());
    } else if (cmd == "variables") {
        const int ref = args.value("variablesReference", 0);
        json vars = json::array();
        auto add = [&](const std::string& name) {
            json v = describe(L, m_tableRefs);
            v["name"] = name;
            vars.push_back(v);
        };
        if (m_paused && ref >= kLocalsBase && ref < kUpvaluesBase) {
            const int level = ref - kLocalsBase;
            for (int n = 1;; ++n) {
                const char* name = lua_getlocal(L, level, n);
                if (!name) break;
                if (name[0] != '(') add(name);
                lua_pop(L, 1);
            }
        } else if (m_paused && ref >= kUpvaluesBase && ref < kGameVars) {
            lua_Debug ar{};
            if (lua_getinfo(L, ref - kUpvaluesBase, "f", &ar)) {
                for (int n = 1;; ++n) {
                    const char* name = lua_getupvalue(L, -1, n);
                    if (!name) break;
                    add(*name ? name : "?");
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        } else if (ref == kGameVars && m_host.varsRef() >= 0) {
            lua_getref(L, m_host.varsRef());
            lua_pushnil(L);
            while (lua_next(L, -2) && vars.size() < 500) {
                lua_pushvalue(L, -2);
                const std::string key = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?";
                lua_pop(L, 1);
                add(key);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        } else if (ref >= 1 && ref <= static_cast<int>(m_tableRefs.size())) {
            lua_getref(L, m_tableRefs[static_cast<size_t>(ref - 1)]);
            lua_pushnil(L);
            while (lua_next(L, -2) && vars.size() < 500) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) key = lua_tostring(L, -2);
                else if (lua_type(L, -2) == LUA_TNUMBER) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "[%g]", lua_tonumber(L, -2));
                    key = b;
                } else {
                    key = std::string("[") + luaL_typename(L, -2) + "]";
                }
                add(key);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        respond(text, true, json{{"variables", vars}}.dump());
    } else if (cmd == "evaluate") {
        // Nome de variável local (ou do jogo) do quadro: o que o mouse aponta.
        const std::string expr = args.value("expression", std::string());
        const int level = args.value("frameId", 1) - 1;
        bool found = false;
        json v;
        if (m_paused) {
            for (int n = 1; !found; ++n) {
                const char* name = lua_getlocal(L, level, n);
                if (!name) break;
                if (expr == name) {
                    v = describe(L, m_tableRefs);
                    found = true;
                }
                lua_pop(L, 1);
            }
        }
        if (found) respond(text, true, json{{"result", v["value"]}, {"variablesReference", v["variablesReference"]}}.dump());
        else respond(text, false, "{}", "Only local variable names can be shown here.");
    } else if (cmd == "continue") {
        m_step = Step::None;
        m_resume = true;
        respond(text, true, R"({"allThreadsContinued":true})");
    } else if (cmd == "next" || cmd == "stepIn" || cmd == "stepOut") {
        m_step = cmd == "next" ? Step::Over : cmd == "stepIn" ? Step::In : Step::Out;
        m_resume = true;
        respond(text, true, "{}");
    } else if (cmd == "pause") {
        if (!m_paused) {
            m_step = Step::Pause;
            lua_singlestep(m_host.L, 1);
        }
        respond(text, true, "{}");
    } else if (cmd == "disconnect" || cmd == "terminate") {
        respond(text, true, "{}");
        m_step = Step::None;
        m_resume = true;
    } else {
        respond(text, false, "{}", "Not supported: " + cmd);
    }
}

} // namespace eruption
