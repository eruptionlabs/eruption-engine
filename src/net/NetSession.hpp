#pragma once

#include "net/Blake2b.hpp"
#include "net/Channels.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace eruption {

class PacketLink;
using RelayKey = std::array<uint8_t, 32>;

// Partida em rede: host (servidor + jogador local), servidor dedicado ou
// cliente. Não sabe de socket (recebe PacketLink) nem de Lua (eventos por
// fila). O host P2P e o dedicado rodam o mesmo papel de servidor; o jogador
// local do host entra por um link em memória e passa pela mesma validação
// que os remotos.
//
// Protocolo de sessão sobre os canais:
//  canal 0 (sistema): hello{hash de conteúdo, nome} do cliente; welcome,
//    recusa{motivo}, entrou{id, nome}, saiu{id, motivo} do servidor;
//  canal 1 (jogo): evento{nome, dados}. Do cliente só nomes liberados com
//    allowFromClient, dentro do teto de bytes e de eventos por segundo.
// Hash de conteúdo diferente (mapa, faixas, versão do script) -> recusa
// "conteudo_diferente", que o cliente recebe como falha clara.
constexpr uint16_t kNetDefaultPort = 41920;
// Versão do protocolo do motor: incompatível de verdade nem conecta (o
// netcode recusa o token). Conteúdo diferente conecta e recebe a recusa
// "conteudo_diferente" pelo canal de sistema.
constexpr uint64_t kNetProtocolId = 0x4552555054000001ull;
constexpr int kNetMaxPlayers = 16;
constexpr size_t kNetMaxNameBytes = 24;
constexpr size_t kNetMaxEventNameBytes = 32;
constexpr size_t kNetDefaultEventBytes = 256;
constexpr size_t kNetMaxEventBytes = 900; // cabe numa mensagem confiável com o nome

struct NetEvent {
    enum class Type { Joined, Left, Failed, Message };
    Type type = Type::Message;
    int player = 0;               // Joined/Left: o jogador; Message: quem mandou (0 = servidor)
    std::string name;             // Joined: nome do jogador; Message: nome do evento
    std::string reason;           // Left/Failed
    std::vector<uint8_t> data;    // Message: dados codificados (NetValue)
};

struct NetPlayer {
    int id = 0;
    std::string name;
    float pingMs = 0.0f;
};

struct NetHostOptions {
    int maxPlayers = 4;
    uint16_t port = kNetDefaultPort; // 0 = o sistema escolhe
    bool dedicated = false;          // sem jogador local
    bool ipv6 = false;
    std::string playerName;          // do jogador local do host
    ContentHash content{};
};

struct NetSessionStats {
    ConnectionStats link;       // cliente: a conexão com o servidor; servidor: a pior
    int players = 0;
    uint64_t tokensIssued = 0;
    uint64_t tokensRefused = 0;
    uint64_t requestsDropped = 0; // pedidos de conexão barrados antes de decifrar
    uint64_t floodDropped = 0;    // pacotes acima do teto por endereço
    uint64_t requestsAdmitted = 0; // pedidos de conexão que o netcode decifrou
    uint64_t kicks = 0;
    std::string lastKick;          // motivo da última vez que o servidor derrubou alguém
};

class NetSession {
public:
    enum class Role { Offline, Connecting, Connected, Hosting, Server };

    NetSession();
    ~NetSession();
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    // Abre UDP na porta pedida. Falso se a porta não abriu.
    bool host(const NetHostOptions& options, double time);
    // "ip", "ip:porta", "[ipv6]:porta" ou "localhost".
    bool join(const std::string& destination, const std::string& playerName, const ContentHash& content, double time);
    // Mesmos papéis sobre um link dado (testes em memória, simulador, relay).
    bool hostOn(PacketLink& link, const NetHostOptions& options, double time);
    // Host atrás de CGNAT: abre a sala num relay (Relay.hpp). Os clientes
    // discam publicAddress() ("ip-da-vps:porta-da-sala") quando ficar pronto.
    bool hostRelay(const std::string& relay, const RelayKey& key, const NetHostOptions& options, double time);
    // Mesmo papel, com o link até o relay dado (testes em memória).
    bool hostRelayOn(PacketLink& toRelay, const std::string& relay, const RelayKey& key, const NetHostOptions& options,
                     double time);
    // Pelo relay: o endereço da sala (vazio até o relay responder). Sem relay: vazio.
    std::string publicAddress() const;
    bool joinOn(PacketLink& link, const std::string& destination, const std::string& playerName,
                const ContentHash& content, double time);
    // Entra por um fluxo ws (navegador). Separa o transporte (ws, porta da
    // ponte) do endereço de jogo que entra no token (porta do host). Aceita
    // "ws://ip:porta" e, no wasm, "ip"/"ip:porta" (spec 26). Devolve falso e
    // enfileira uma falha clara se o transporte web não existe (desktop).
    bool joinWeb(const std::string& destination, const std::string& playerName, const ContentHash& content,
                 double time);
    void leave();
    void update(double time);

    Role role() const;
    bool isServer() const { return role() == Role::Hosting || role() == Role::Server; }
    int myId() const;
    std::vector<NetPlayer> players() const;
    // Host: "ip:porta" para o outro jogador discar (porta real, mesmo com 0).
    uint16_t port() const { return m_port; }

    // Servidor: libera um evento vindo do cliente (o resto é infração).
    void allowFromClient(const std::string& name, size_t maxBytes = kNetDefaultEventBytes);
    // Servidor -> todos (o jogador local do host também recebe).
    bool send(const std::string& name, const std::vector<uint8_t>& data);
    // Servidor -> um jogador.
    bool sendTo(int player, const std::string& name, const std::vector<uint8_t>& data);
    // Cliente (inclusive o jogador local do host) -> servidor.
    bool ask(const std::string& name, const std::vector<uint8_t>& data);

    bool poll(NetEvent& event);
    NetSessionStats stats() const;

private:
    struct Server;
    struct Client;

    std::unique_ptr<Server> m_server;
    std::unique_ptr<Client> m_client;
    std::deque<NetEvent> m_events;
    std::map<std::string, size_t> m_fromClient;
    uint16_t m_port = 0;
};

const char* toString(NetSession::Role role);

} // namespace eruption
