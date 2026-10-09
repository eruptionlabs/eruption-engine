#pragma once

#include "math/Types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct lua_State;

namespace eruption {

class Engine;

// Scripts de jogo em Luau. Cada arquivo .luau da pasta de scripts roda num
// ambiente isolado (sandbox) e pode:
//   - devolver uma tabela com funções opcionais on_start(), on_update(dt),
//     on_stop() e on_reload(antigo); o campo `state` dela sobrevive à recarga;
//   - registrar eventos no topo do arquivo: on.start, on.update, on.every,
//     on.key, on.near, on.hour (é o formato que a folha de eventos gera).
//
// Um erro desliga só aquele script até o arquivo ser salvo de novo. Uma
// chamada que passa de kWatchdogMs é interrompida, para um laço infinito não
// travar o jogo.
class ScriptHost {
public:
    bool init(Engine& engine, const std::filesystem::path& dir);
    void shutdown();

    // Confere mudanças nos arquivos (no máximo duas vezes por segundo).
    void poll();
    void start();
    void update(float dt);
    void stop();

    bool running() const { return m_running; }
    bool nativeCode() const { return m_native; }
    float lastUpdateMs() const { return m_lastUpdateMs; }
    size_t scriptCount() const { return m_scripts.size(); }
    size_t failedCount() const;
    const std::filesystem::path& dir() const { return m_dir; }

    // Cria um script novo a partir do modelo e devolve o caminho.
    std::filesystem::path createScript(const std::string& name);

    // Mensagens de ui.message na área do jogo (retângulo em pixels de tela).
    void drawMessages(float x, float y, float w, float h) const;

    static constexpr int kWatchdogMs = 250;

    // ---- usado pela API (ScriptApi.cpp)
    enum class EventKind { Start, Update, Every, Key, Near, Hour };
    struct Handler {
        EventKind kind = EventKind::Start;
        int fnRef = -1;
        double number = 0.0;   // segundos (every), distância (near), hora (hour)
        std::string text;      // tecla (key) ou nome do modelo (near)
        double timer = 0.0;
        bool flag = false;     // near: estava perto no frame anterior
    };
    static ScriptHost* current() { return s_current; }
    // Falso fora do carregamento de um arquivo (on.* só vale no topo).
    bool addHandler(Handler h);
    void showMessage(const std::string& text, float seconds);
    void addTween(uint32_t model, const Vec3& move, float yawDegrees, float seconds);
    int varsRef() const { return m_varsRef; }

private:
    struct Script {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime{};
        int moduleRef = -1;
        std::vector<Handler> handlers;
        bool failed = false;   // erro em execução: desligado até salvar de novo
        bool started = false;
    };
    struct Tween {
        uint32_t model = 0;
        Mat4 start{1.0f};
        Vec3 move{0.0f};
        float yaw = 0.0f;
        float duration = 0.0f;
        float t = 0.0f;
    };
    struct Message {
        std::string text;
        float remaining = 0.0f;
    };

    bool load(Script& s, bool keepState);
    bool call(Script& s, const char* fn, int nargs);
    bool callRef(Script& s, int fnRef, int nargs);
    void startScript(Script& s);
    void releaseHandlers(std::vector<Handler>& hs);
    void unload(Script& s);
    void scan();
    void updateTweens(float dt);
    void dispatch(Script& s, float dt, float prevHour, float hour);
    void resetVars();
    void writeTypeDefinitions();

    static ScriptHost* s_current;
    Engine* m_engine = nullptr;
    lua_State* L = nullptr;
    std::filesystem::path m_dir;
    std::vector<Script> m_scripts;
    std::vector<Handler>* m_loadingHandlers = nullptr;
    std::vector<Tween> m_tweens;
    std::vector<Message> m_messages;
    int m_varsRef = -1;
    float m_prevHour = 0.0f;
    bool m_running = false;
    bool m_native = false;
    float m_lastUpdateMs = 0.0f;
    std::chrono::steady_clock::time_point m_lastScan{};
};

} // namespace eruption
