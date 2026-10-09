#include "editor/AiClient.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace eruption {

namespace {

namespace fs = std::filesystem;

fs::path keyPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".config";
    return base / "eruption" / "anthropic_key";
}

#ifndef _WIN32
// Arquivo temporário criado só para o usuário (mkstemp usa permissão 600).
std::string writeTemp(const std::string& content) {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    std::string tmpl = std::string(dir && *dir ? dir : "/tmp") + "/eruption-ai-XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) return {};
    size_t done = 0;
    while (done < content.size()) {
        const ssize_t n = write(fd, content.data() + done, content.size() - done);
        if (n <= 0) break;
        done += size_t(n);
    }
    close(fd);
    return name.data();
}
#endif

} // namespace

AiClient::~AiClient() {
    if (m_thread.joinable()) m_thread.join();
}

std::string AiClient::loadKey() {
    if (const char* env = std::getenv("ANTHROPIC_API_KEY"); env && *env) return env;
    std::ifstream f(keyPath());
    std::string key;
    std::getline(f, key);
    return key;
}

bool AiClient::saveKey(const std::string& key) {
    std::error_code ec;
    fs::create_directories(keyPath().parent_path(), ec);
    {
        std::ofstream f(keyPath(), std::ios::trunc);
        if (!f) return false;
        f << key << "\n";
    }
#ifndef _WIN32
    chmod(keyPath().c_str(), 0600);
#endif
    return true;
}

std::vector<AiClient::Turn> AiClient::turns() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_turns;
}

void AiClient::clear() {
    if (m_busy) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_turns.clear();
    m_messages = nlohmann::json::array();
}

bool AiClient::send(const std::string& key, const std::string& text) {
    if (m_busy || text.empty() || key.empty()) return false;
    if (m_thread.joinable()) m_thread.join();
    nlohmann::json body;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_turns.push_back({true, text, false});
        m_messages.push_back({{"role", "user"}, {"content", text}});
        body = {
            {"model", model},
            {"max_tokens", 16000},
            {"output_config", {{"effort", "medium"}}},
            // Recusa de um classificador de segurança: o servidor tenta de novo
            // num modelo de reserva dentro do mesmo pedido.
            {"fallbacks", "default"},
            // A API do motor é grande e não muda: fica em cache entre mensagens.
            {"system", nlohmann::json::array({{{"type", "text"}, {"text", m_system}, {"cache_control", {{"type", "ephemeral"}}}}})},
            {"messages", m_messages},
        };
    }
    m_busy = true;
    m_thread = std::thread([this, key, body] { run(key, body); });
    return true;
}

void AiClient::run(std::string key, nlohmann::json body) {
    std::string reply, error;
    nlohmann::json content;
#ifdef _WIN32
    error = "The AI assistant is not available on Windows yet.";
#else
    const std::string headers = "x-api-key: " + key + "\nanthropic-version: 2023-06-01\n"
                                "anthropic-beta: server-side-fallback-2026-07-01\ncontent-type: application/json\n";
    const std::string hdrFile = writeTemp(headers);
    const std::string bodyFile = writeTemp(body.dump());
    std::string out;
    if (hdrFile.empty() || bodyFile.empty()) {
        error = "Could not create a temporary file for the request.";
    } else {
        const std::string cmd = "curl -s -S --max-time 600 -H @" + hdrFile + " --data-binary @" + bodyFile +
                                " https://api.anthropic.com/v1/messages 2>&1";
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
            if (pclose(p) != 0 && out.empty()) error = "curl is not installed (it sends the request).";
        } else {
            error = "Could not start curl.";
        }
    }
    if (!hdrFile.empty()) unlink(hdrFile.c_str());
    if (!bodyFile.empty()) unlink(bodyFile.c_str());
    if (error.empty()) {
        const nlohmann::json r = nlohmann::json::parse(out, nullptr, false);
        if (r.is_discarded()) {
            error = "No answer from the API: " + out.substr(0, 300);
        } else if (r.contains("error")) {
            error = r["error"].value("message", "API error");
        } else {
            content = r.value("content", nlohmann::json::array());
            for (const auto& block : content)
                if (block.value("type", "") == "text") reply += block.value("text", "");
            if (r.value("stop_reason", "") == "refusal") {
                std::string cat;
                if (r.contains("stop_details") && r["stop_details"].is_object())
                    cat = r["stop_details"].value("category", "");
                error = "The request was declined" + (cat.empty() ? std::string(".") : " (" + cat + ").");
            } else if (r.value("stop_reason", "") == "max_tokens") {
                reply += "\n\n(The answer was cut off at the length limit.)";
            }
        }
    }
#endif
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!error.empty()) {
        m_turns.push_back({false, error, true});
        // Sem resposta válida a pergunta sai do histórico (pode ser repetida).
        if (!m_messages.empty()) m_messages.erase(m_messages.end() - 1);
    } else {
        m_turns.push_back({false, reply, false});
        // A resposta volta inteira (blocos de pensamento inclusos) na próxima mensagem.
        m_messages.push_back({{"role", "assistant"}, {"content", content}});
    }
    m_busy = false;
}

} // namespace eruption
