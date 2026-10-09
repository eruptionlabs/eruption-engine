#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace eruption {

class ScriptHost;

// Depurador dos scripts para o VS Code (Debug Adapter Protocol por TCP, em
// 127.0.0.1). A extensão em tools/vscode-eruption só liga o tipo "eruption" a
// esta porta. Pontos de parada, passo a passo (próxima linha, entrar, sair),
// pausa, pilha, variáveis locais e de cima, as variáveis do jogo (vars) e as
// tabelas abertas. Parado, o jogo congela até continuar.
class ScriptDebugger {
public:
    explicit ScriptDebugger(ScriptHost& host);
    ~ScriptDebugger();
    bool listen(int port);
    void shutdown();
    // Uma vez por frame: aceita a conexão e responde ao que chegou.
    void poll();
    bool connected() const { return m_client >= 0; }
    int port() const { return m_port; }
    // Ao carregar um script: marca nele os pontos de parada do arquivo.
    void applyBreakpoints(lua_State* L, int funcIndex, const std::filesystem::path& file);

private:
    enum class Step { None, Over, In, Out, Pause };
    ScriptHost& m_host;
    int m_listen = -1, m_client = -1, m_port = 0;
    std::string m_inbox;
    int m_seq = 1;
    std::map<std::string, std::set<int>> m_breakpoints; // arquivo (caminho absoluto) -> linhas
    std::map<std::string, std::set<int>> m_applied;     // linhas marcadas de fato no bytecode
    Step m_step = Step::None;
    int m_stepDepth = 0;
    std::string m_stepSource;
    int m_stepLine = -1;
    bool m_paused = false, m_resume = false;
    lua_State* m_pausedL = nullptr;
    std::vector<int> m_tableRefs; // tabelas abertas na pausa (variablesReference)
    // Passo: o modo passo a passo do Luau só vale a partir da próxima chamada,
    // então a linha seguinte da função (e da que chamou) ganha parada temporária.
    struct TempBreak { int funcRef; int line; };
    std::vector<TempBreak> m_temp;
    void armStep(lua_State* L);
    void clearTemp(lua_State* L);
    bool shouldStop(lua_State* L);

    static ScriptDebugger* s_active;
    static void onBreak(lua_State* L, lua_Debug* ar);
    static void onStep(lua_State* L, lua_Debug* ar);

    void accept();
    bool readMessages(int timeoutMs);
    void handle(const std::string& json);
    void send(const std::string& json);
    void respond(const std::string& requestJson, bool ok, const std::string& bodyJson, const std::string& message = {});
    void event(const std::string& name, const std::string& bodyJson);
    void pause(lua_State* L, const char* reason);
    void releaseTables();
    std::string sourceOf(lua_State* L, int level, int* line) const;
    static std::string normalize(const std::string& path);
};

} // namespace eruption
