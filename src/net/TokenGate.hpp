#pragma once

#include "net/PacketLink.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>

namespace eruption {

// Entrega de token na LAN / IP direto: o cliente sabe só "ip:porta" e pede
// um token de conexão ao host pela mesma porta UDP. O host é quem emite os
// tokens (tem a chave privada da sessão), então o pedido vem antes do
// netcode:
//  - o token do netcode leva em claro as duas chaves da sessão daquele
//    cliente; quem o lesse leria e forjaria o tráfego dele a partida toda.
//    Por isso a resposta vai lacrada: o cliente manda uma chave X25519
//    efêmera no pedido, o host cifra o token para ela (X25519 + BLAKE2b +
//    XChaCha20-Poly1305, Monocypher) com uma chave efêmera própria. Quem só
//    escuta não lê nada. Quem está no meio do caminho (ativo) ainda pode se
//    passar pelo host: fecha com a senha da sala (CPace), na fase 2;
//  - pedido sempre MAIOR que a resposta (anti-amplificação; um IP forjado
//    não ganha mais bytes do que mandou);
//  - token bucket por IP e teto global de tokens por segundo.
// Pacote de token tem tamanho fixo e prefixo mágico; o netcode nunca manda
// pacote desse tamanho, então a separação é exata.
//
// É também a porta de entrada de tudo que chega ao servidor, antes de
// qualquer decifragem (doc 24, itens 2 e 3):
//  - pedido de conexão do netcode só com o tamanho exato, a versão e o
//    protocolo certos, e dentro do balde por IP e do teto global (decifrar o
//    token custa CPU; um IP forjado não faz o servidor decifrar à vontade);
//  - teto de pacotes por segundo por endereço: pacote forjado com o endereço
//    de um jogador só gasta a cota dele (derruba no máximo aquele jogador,
//    nunca o servidor), e limita as tentativas de forjar a cifra a
//    ~300/s por conexão (2^36 tentativas, o limite de integridade do
//    ChaCha20-Poly1305 da RFC 9001, levariam mais de 7 anos);
//  - tabelas de balde com teto: cheias, endereço novo fica de fora e os que
//    já estão continuam (nunca zera o limite de quem está atacando).
constexpr int kTokenRequestBytes = 2304;
constexpr int kTokenReplyBytes = 4 + 8 + 32 + 16 + 2048; // mágica + nonce + chave do host + mac + token

class TokenGate final : public PacketLink {
public:
    // Recebe o endereço que o cliente discou ("ip:porta", conferido) e
    // devolve o token, ou falso para recusar.
    using Issue = std::function<bool(const std::string& dialed, std::vector<uint8_t>& token)>;

    TokenGate(PacketLink& inner, uint16_t port, uint64_t protocolId, Issue issue);

    void send(const NetAddress& to, const uint8_t* data, int bytes) override;
    int receive(NetAddress& from, uint8_t* out, int maxBytes) override;
    void update(double time) override;

    // Pelo relay, a porta que o cliente disca é a da sala (só se sabe depois
    // que o relay responde).
    void setPort(uint16_t port) { m_port = port; }
    uint16_t port() const { return m_port; }
    uint64_t issued() const { return m_issued; }
    uint64_t refused() const { return m_refused; }
    uint64_t droppedRequests() const { return m_droppedRequests; } // pedidos de conexão barrados
    uint64_t droppedFlood() const { return m_droppedFlood; }       // pacotes acima do teto do endereço
    uint64_t admittedRequests() const { return m_admittedRequests; } // pedidos que o netcode vai decifrar

private:
    struct Bucket {
        double tokens = 0.0;
        double last = 0.0;
    };
    struct AddressHash {
        size_t operator()(const NetAddress& a) const;
    };
    struct Buckets {
        std::unordered_map<NetAddress, Bucket, AddressHash> map;
        double lastPrune = -1e9;
    };
    bool take(Buckets& table, const NetAddress& key, double burst, double rate);
    bool allowToken(const NetAddress& from);
    bool admitConnectRequest(const NetAddress& from, const uint8_t* data, int bytes);
    void logDrops();
    void handleRequest(const NetAddress& from, const uint8_t* data, int bytes);

    PacketLink& m_inner;
    uint16_t m_port;
    Issue m_issue;
    double m_time = 0.0;
    uint64_t m_protocolId;
    double m_globalTokens = 0.0;
    double m_globalConnects = 0.0;
    Buckets m_tokenBuckets;   // por IP
    Buckets m_connectBuckets; // por IP
    Buckets m_floodBuckets;   // por endereço (IP + porta)
    double m_lastLog = 0.0;
    uint64_t m_loggedRequests = 0, m_loggedFlood = 0;
    uint64_t m_droppedRequests = 0, m_droppedFlood = 0, m_admittedRequests = 0;
    std::vector<uint8_t> m_buffer;
    uint64_t m_issued = 0;
    uint64_t m_refused = 0;
};

// Lado do cliente: pede o token e espera a resposta (reenvia a cada 0,5 s).
class TokenRequest {
public:
    TokenRequest() = default;
    ~TokenRequest();
    TokenRequest(const TokenRequest&) = delete;
    TokenRequest& operator=(const TokenRequest&) = delete;

    enum class State { Idle, Waiting, Done, Failed };

    bool start(const std::string& dialed, PacketLink& link, double time, double timeoutSeconds = 5.0);
    void update(double time);
    State state() const { return m_state; }
    const std::vector<uint8_t>& token() const { return m_token; }

private:
    void sendRequest();

    PacketLink* m_link = nullptr;
    NetAddress m_server;
    uint8_t m_secret[32] = {};
    uint8_t m_public[32] = {};
    std::string m_dialed;
    uint64_t m_nonce = 0;
    double m_deadline = 0.0;
    double m_lastSend = -1.0;
    State m_state = State::Idle;
    std::vector<uint8_t> m_token;
};

} // namespace eruption
