#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eruption {

// Segunda tela: um servidor HTTP pequeno na rede local que abre os scripts do
// projeto (assets/scripts/*.luau) num navegador de celular, tablet ou outro
// PC. Salvar no navegador grava o arquivo e o motor recarrega na hora.
// Só com o editor, desligado até alguém ligar; toda chamada exige o token.
class RemoteServer {
public:
    ~RemoteServer() { stop(); }
    bool start(uint16_t port, const std::filesystem::path& scriptsDir);
    void stop();
    bool running() const { return m_running; }
    uint16_t port() const { return m_port; }
    const std::string& token() const { return m_token; }
    // Endereços desta máquina na rede local (para mostrar no editor).
    static std::vector<std::string> localAddresses();
    // Linhas do console que a página mostra.
    void pushLog(const std::string& line);
    std::string error;

private:
    void loop();
    void handle(int fd);
    std::string respond(const std::string& method, const std::string& target, const std::string& body);
    bool safePath(const std::string& rel, std::filesystem::path& out) const;

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    int m_listen = -1;
    uint16_t m_port = 0;
    std::string m_token;
    std::filesystem::path m_dir;
    std::mutex m_logMutex;
    std::deque<std::string> m_log;
    uint64_t m_logSerial = 0;
};

} // namespace eruption
