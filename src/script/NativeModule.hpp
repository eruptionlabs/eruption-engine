#pragma once

#include <cstdint>
#include <filesystem>
#include <deque>
#include <future>
#include <mutex>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;
struct EruptionModule;
struct EruptionHost;
struct EruptionValue;

namespace eruption {

class Engine;

// Módulo de jogo em C++ (include/eruption/native_module.h). Os fontes ficam em
// <projeto>/native; no desktop o motor compila numa biblioteca e troca a
// versão em uso quando um arquivo muda, sem parar o jogo. O estado do módulo
// mora aqui (sobrevive à troca); erro de compilação deixa a versão anterior
// rodando e a mensagem no console. Jogo exportado (desktop): carrega a última
// versão compilada (.eruption/native/game.so), sem compilar. Android/iOS: ainda
// não (o plano é linkar o mesmo código no jogo).
// Android (ERUPTION_NATIVE_STATIC): os fontes de native/ entram na biblioteca
// do jogo pelo CMake; o módulo é o eruption_module() linkado, sem recarga.
// API 2: valores (EruptionValue) entre módulo e scripts, e eventos do módulo
// (emit, thread-safe, fila drenada no quadro) para native.on.
class NativeModule {
public:
    // Valor copiado (dono da memória) que atravessa a ponte e a fila de eventos.
    struct Val {
        uint32_t type = 0; // EruptionValueType
        double number = 0.0;
        std::string bytes; // STRING / BYTES
        std::vector<std::pair<Val, Val>> pairs;
    };

    static NativeModule& instance();

    void init(Engine& engine, const std::filesystem::path& projectRoot, const std::filesystem::path& engineRoot);
    void shutdown(); // para o jogo (stop), descarrega (unload) e solta os ouvintes Luau
    // Vigia native/ (uma vez por segundo), termina a compilação e troca o módulo.
    void poll(float dt);
    void start();
    void stop();
    void update(float dt);

    bool loaded() const { return m_module != nullptr; }
    bool building() const { return m_build.valid(); }
    // Luau: native.<nome>(...) -> número. Falso se o nome não foi exposto.
    bool call(const std::string& name, const double* args, int count, double& out) const;
    // Luau: qualquer valor (API 2). Falso se o nome não foi exposto com expose_value.
    bool callValue(const std::string& name, const std::vector<Val>& args, Val& out) const;
    // Tabela `native` dos scripts e o bloco dela nas declarações de tipo.
    static void registerApi(lua_State* L);
    // Ouvinte Luau de native.on: guardado como referência no registro.
    void addListener(const std::string& event, int ref);
    static const char* apiDefinitions();

private:
    struct BuildResult {
        bool ok = false;
        std::filesystem::path library;
        std::string output;
        double ms = 0.0; // tempo da compilação e ligação (ou da cópia, se o link foi reaproveitado)
        std::string linkKey; // chave do link (ver startBuild), gravada ao lado de game.so
    };
    using NativeFn = double (*)(::EruptionHost*, const double*, int);
    using NativeValueFn = void (*)(::EruptionHost*, const ::EruptionValue*, int, ::EruptionValue*);

    void closeModule();
    bool activate(const ::EruptionModule* mod, const std::filesystem::path& file, void* handle);
    void drainEvents();
    bool queueEvent(const char* event, const ::EruptionValue* value);

    bool sourcesChanged();
    void startBuild();
    bool swapTo(const std::filesystem::path& library);
    void unloadCurrent(bool reloading);

    Engine* m_engine = nullptr;
    std::filesystem::path m_sources, m_buildDir, m_includeDir;
    std::future<BuildResult> m_build;
    bool m_rebuildPending = false;
    float m_pollTimer = 0.0f;
    std::uintmax_t m_signature = 0;
    uint32_t m_buildCount = 0;

    void* m_handle = nullptr;
    const EruptionModule* m_module = nullptr;
    std::filesystem::path m_loadedFile;
    std::vector<unsigned char> m_state;
    bool m_hasState = false;
    bool m_running = false;
    double m_time = 0.0;
    std::unordered_map<std::string, NativeFn> m_exposed;
    std::unordered_map<std::string, NativeValueFn> m_exposedV;

    lua_State* m_L = nullptr; // estado principal dos scripts (para native.on)
    std::unordered_map<std::string, std::vector<int>> m_listeners;
    std::mutex m_eventMutex;
    std::deque<std::pair<std::string, Val>> m_events;
    size_t m_droppedEvents = 0;

    friend struct NativeHostCalls;
};

} // namespace eruption
