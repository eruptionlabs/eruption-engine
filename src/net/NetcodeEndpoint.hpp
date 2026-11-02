#pragma once

#include "net/PacketLink.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct netcode_server_t;
struct netcode_client_t;
struct netcode_address_t;

namespace eruption {

// Conexão segura cliente/servidor do padrão netcode 1.02 (third_party/netcode,
// BSD-3): token de conexão com validade, cifra autenticada em todo pacote
// (ChaCha20-Poly1305), janela anti-replay, resposta nunca maior que o pedido
// antes de validar o cliente. Aqui só o embrulho: os pacotes passam por um
// PacketLink nosso (UDP, memória, simulador ou túnel).

constexpr int kNetMaxPayload = 1100;     // carga por pacote (o netcode cabe em 1200 B)
constexpr int kNetConnectTokenBytes = 2048;
constexpr int kNetUserDataBytes = 256;

using NetKey = std::array<uint8_t, 32>;
NetKey makeNetKey(); // aleatória, do gerador do sistema (libsodium)

struct ConnectTokenRequest {
    std::vector<std::string> publicAddresses;   // por onde o cliente chega (relay: o da VPS)
    std::vector<std::string> internalAddresses; // vazio = os mesmos
    int expireSeconds = 30;
    int timeoutSeconds = 5;
    uint64_t clientId = 0;
    uint64_t protocolId = 0;
    std::array<uint8_t, kNetUserDataBytes> userData{};
};
// Falso se os endereços não fazem sentido. `token` sai com kNetConnectTokenBytes.
bool makeConnectToken(const ConnectTokenRequest& request, const NetKey& key, std::vector<uint8_t>& token);

class NetServer {
public:
    NetServer();
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // `publicAddress` é o endereço que os tokens trazem. O link tem que
    // viver mais que o servidor.
    bool start(const std::string& publicAddress, uint64_t protocolId, const NetKey& key, int maxClients,
               PacketLink& link, double time);
    void stop();
    bool running() const { return m_server != nullptr; }
    void update(double time);

    int maxClients() const { return m_maxClients; }
    bool connected(int client) const;
    uint64_t clientId(int client) const;
    void send(int client, const uint8_t* data, int bytes);
    // Um pacote por chamada; falso quando acabou.
    bool receive(int client, std::vector<uint8_t>& out, uint64_t* sequence = nullptr);
    void disconnect(int client);

    std::function<void(int client, bool connected)> onConnection;

private:
    static void sendOverride(void* ctx, ::netcode_address_t* to, const uint8_t* data, int bytes);
    static int receiveOverride(void* ctx, ::netcode_address_t* from, uint8_t* data, int maxBytes);
    static void connectionCallback(void* ctx, int client, int connected);

    netcode_server_t* m_server = nullptr;
    PacketLink* m_link = nullptr;
    int m_maxClients = 0;
};

class NetClient {
public:
    enum class State {
        TokenExpired = -6,
        InvalidToken = -5,
        TimedOut = -4,
        ResponseTimedOut = -3,
        RequestTimedOut = -2,
        Denied = -1,
        Disconnected = 0,
        SendingRequest = 1,
        SendingResponse = 2,
        Connected = 3,
    };

    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // `localAddress` só diz a família (IPv4/IPv6) e a porta do link.
    bool connect(const std::vector<uint8_t>& token, const std::string& localAddress, PacketLink& link, double time);
    void disconnect();
    void update(double time);
    State state() const;
    int clientIndex() const;
    void send(const uint8_t* data, int bytes);
    bool receive(std::vector<uint8_t>& out, uint64_t* sequence = nullptr);

private:
    static void sendOverride(void* ctx, ::netcode_address_t* to, const uint8_t* data, int bytes);
    static int receiveOverride(void* ctx, ::netcode_address_t* from, uint8_t* data, int maxBytes);

    netcode_client_t* m_client = nullptr;
    PacketLink* m_link = nullptr;
};

const char* toString(NetClient::State state);

} // namespace eruption
