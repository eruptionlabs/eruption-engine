#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;
struct EruptionModule;
struct EruptionHost;

namespace eruption {

class Engine;

// Módulo de jogo em C++ (include/eruption/native_module.h). Os fontes ficam em
// <projeto>/native; no desktop o motor compila numa biblioteca e troca a
// versão em uso quando um arquivo muda, sem parar o jogo. O estado do módulo
// mora aqui (sobrevive à troca); erro de compilação deixa a versão anterior
// rodando e a mensagem no console. Jogo exportado (desktop): carrega a última
// versão compilada (.eruption/native/game.so), sem compilar. Android/iOS: ainda
// não (o plano é linkar o mesmo código no jogo).
class NativeModule {
public:
    static NativeModule& instance();

    void init(Engine& engine, const std::filesystem::path& projectRoot, const std::filesystem::path& engineRoot);
    void shutdown();
    // Vigia native/ (uma vez por segundo), termina a compilação e troca o módulo.
    void poll(float dt);
    void start();
    void stop();
    void update(float dt);

    bool loaded() const { return m_module != nullptr; }
    bool building() const { return m_build.valid(); }
    // Luau: native.<nome>(...) -> número. Falso se o nome não foi exposto.
    bool call(const std::string& name, const double* args, int count, double& out) const;
    // Tabela `native` dos scripts e o bloco dela nas declarações de tipo.
    static void registerApi(lua_State* L);
    static const char* apiDefinitions();

private:
    struct BuildResult {
        bool ok = false;
        std::filesystem::path library;
        std::string output;
    };
    using NativeFn = double (*)(::EruptionHost*, const double*, int);

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

    friend struct NativeHostCalls;
};

} // namespace eruption
