#pragma once

#include "net/NetValue.hpp"

#include <memory>
#include <string>
#include <vector>

namespace eruption {

// Rede do jogo: partida hospedada por um jogador (P2P) ou servidor dedicado,
// o mesmo protocolo nos dois modos. Esta é a fachada que o script (net.*)
// usa. Módulo opcional `net` (cmake/modules.json): desligado, NetNull.cpp
// mantém a interface e nada abre socket; net.host/net.join falham com
// "rede_desligada".
struct NetPlayerInfo {
    int id = 0;
    std::string name;
    float pingMs = 0.0f;
};

struct NetScriptEvent {
    enum class Kind { Join, Leave, Fail, Message };
    Kind kind = Kind::Message;
    int player = 0;      // Join/Leave: o jogador; Message: quem mandou (0 = servidor)
    std::string name;    // Join: nome do jogador; Message: nome do evento
    std::string reason;  // Leave/Fail
    NetValue value;      // Message
};

struct NetStatsInfo {
    float pingMs = 0.0f, jitterMs = 0.0f, lossPercent = 0.0f, kbpsIn = 0.0f, kbpsOut = 0.0f;
    int players = 0;
};

class NetSystem {
public:
    static NetSystem& instance();

    NetSystem();
    ~NetSystem();
    NetSystem(const NetSystem&) = delete;
    NetSystem& operator=(const NetSystem&) = delete;

    // Falso com o módulo desligado.
    bool available() const;
    // `content`: pedaços que os dois lados têm de ter iguais (mapa, faixas,
    // scripts, versão do jogo); viram um hash BLAKE2b que vai no hello.
    // `relay` ("ip:porta" de uma VPS com tools/relay) e `relayKey` (64
    // dígitos hex): hospeda pelo relay, para quem está atrás de CGNAT; o
    // endereço da sala aparece em address() quando o relay responder.
    bool host(int maxPlayers, int port, bool dedicated, const std::string& playerName,
              const std::vector<std::string>& content, const std::string& relay, const std::string& relayKey,
              std::string& error);
    bool join(const std::string& destination, const std::string& playerName, const std::vector<std::string>& content);
    void leave();
    // Uma vez por frame (o relógio da rede é a soma dos dt).
    void update(double dt);

    // "offline" | "connecting" | "connected" | "hosting" | "server"
    const char* state() const;
    bool isServer() const;
    int myId() const;
    int port() const;
    // Host: "ip:porta" que outro jogador da LAN disca (o primeiro IPv4 que
    // não é loopback; 127.0.0.1 se não houver rede). Pelo relay, o endereço
    // da sala na VPS (vazio até o relay responder).
    std::string address() const;
    std::vector<NetPlayerInfo> players() const;

    void allowFromClient(const std::string& name, size_t maxBytes);
    bool send(const std::string& name, const NetValue& value);
    bool sendTo(int player, const std::string& name, const NetValue& value);
    bool ask(const std::string& name, const NetValue& value);
    bool poll(NetScriptEvent& event);
    NetStatsInfo stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace eruption
