#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eruption {

// Conversa com a API de mensagens da Anthropic usando a chave do usuário.
// O pedido sai pelo `curl` do sistema (TLS sem dependência nova no motor);
// chave e corpo vão em arquivos temporários só do usuário, nunca na linha
// de comando (que outros processos conseguem ler).
class AiClient {
public:
    struct Turn {
        bool user = true;
        std::string text;   // o que a janela mostra
        bool error = false;
    };
    ~AiClient();
    // Guarda a chave em ~/.config/eruption/anthropic_key (permissão 600).
    static std::string loadKey();
    static bool saveKey(const std::string& key);

    void setSystemPrompt(std::string prompt) { m_system = std::move(prompt); }
    std::string model = "claude-opus-5-5";
    bool send(const std::string& key, const std::string& text);
    bool busy() const { return m_busy; }
    std::vector<Turn> turns();
    void clear();

private:
    void run(std::string key, nlohmann::json body);

    std::string m_system;
    nlohmann::json m_messages = nlohmann::json::array(); // histórico como a API o recebe
    std::vector<Turn> m_turns;
    std::mutex m_mutex;
    std::atomic<bool> m_busy{false};
    std::thread m_thread;
};

} // namespace eruption
