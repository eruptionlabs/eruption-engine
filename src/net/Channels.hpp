#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

struct reliable_endpoint_t;

namespace eruption {

// Canais de uma conexão já autenticada (o netcode garante origem e
// integridade; aqui a outra ponta ainda pode ser hostil). Cada pacote leva:
//  - mensagens sem garantia (estado, entrada): vão uma vez, no próximo pacote;
//  - mensagens confiáveis e ordenadas por canal (RPC, sistema): reenviadas
//    até o ack do reliable (third_party/reliable, BSD-3) confirmar o pacote
//    que as levou, entregues na ordem de envio.
// Pacote malformado, mensagem fora da janela ou fila cheia derrubam a
// conexão (failed()): quem passou pela cifra e manda lixo é hostil.
constexpr int kNetChannels = 2;            // 0 = sistema, 1 = eventos do jogo
constexpr int kNetMaxMessageBytes = 1024;
constexpr int kNetMessageWindow = 256;     // mensagens confiáveis em voo por canal
constexpr int kNetMaxMessagesPerPacket = 64;
constexpr int kNetMaxPacketBytes = 4096;   // acima de ~1 KB o reliable fragmenta

struct ConnectionStats {
    float rttMs = 0.0f;
    float jitterMs = 0.0f;
    float lossPercent = 0.0f;
    float kbpsOut = 0.0f;
    float kbpsIn = 0.0f;
    uint64_t packetsOut = 0;
    uint64_t packetsIn = 0;
};

class Connection {
public:
    // Recebe o pacote pronto (ou um fragmento) para o netcode cifrar e mandar.
    using Transmit = std::function<void(const uint8_t* data, int bytes)>;

    Connection();
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool init(Transmit transmit, double time);
    void reset();
    bool active() const { return m_endpoint != nullptr; }

    // Falso (e conexão marcada como falha) se a fila do canal está cheia.
    bool sendReliable(int channel, const uint8_t* data, int bytes);
    // Vai no próximo pacote e não volta; falso se não cabe.
    bool sendUnreliable(const uint8_t* data, int bytes);

    // Pacote decifrado vindo do netcode.
    void receivePacket(const uint8_t* data, int bytes);
    // Monta e manda um pacote se houver o que mandar ou ack a devolver.
    void update(double time);

    bool receiveReliable(int& channel, std::vector<uint8_t>& out);
    bool receiveUnreliable(std::vector<uint8_t>& out);

    bool failed() const { return m_failReason != nullptr; }
    const char* failReason() const { return m_failReason; }
    // Mensagens confiáveis ainda sem ack (o servidor espera zerar antes de
    // derrubar quem recebeu o motivo da recusa).
    int pendingReliable() const;
    ConnectionStats stats() const;

private:
    struct SentMessage {
        bool used = false;
        uint16_t id = 0;
        double lastSent = -1.0;
        std::vector<uint8_t> data;
    };
    struct SendChannel {
        std::array<SentMessage, kNetMessageWindow> window;
        uint16_t next = 0;
        uint16_t oldest = 0;
    };
    struct RecvChannel {
        std::array<std::vector<uint8_t>, kNetMessageWindow> window;
        std::array<bool, kNetMessageWindow> present{};
        uint16_t next = 0;
    };
    struct PacketRecord {
        bool used = false;
        uint16_t sequence = 0;
        int count = 0;
        std::array<uint8_t, kNetMaxMessagesPerPacket> channel{};
        std::array<uint16_t, kNetMaxMessagesPerPacket> id{};
    };
    struct Ready {
        int channel;
        std::vector<uint8_t> data;
    };

    static void transmitCallback(void* ctx, uint64_t id, uint16_t sequence, uint8_t* data, int bytes);
    static int processCallback(void* ctx, uint64_t id, uint16_t sequence, uint8_t* data, int bytes);
    bool parsePacket(const uint8_t* data, int bytes);
    void processAcks();
    void fail(const char* reason);
    double resendDelay() const;

    reliable_endpoint_t* m_endpoint = nullptr;
    Transmit m_transmit;
    double m_time = 0.0;
    bool m_ackOwed = false;
    const char* m_failReason = nullptr;
    std::array<SendChannel, kNetChannels> m_send;
    std::array<RecvChannel, kNetChannels> m_recv;
    std::vector<PacketRecord> m_packets; // por sequência % tamanho
    std::vector<std::vector<uint8_t>> m_unreliableOut;
    std::deque<std::vector<uint8_t>> m_unreliableIn;
    std::deque<Ready> m_ready;
    std::vector<uint8_t> m_scratch;
};

} // namespace eruption
