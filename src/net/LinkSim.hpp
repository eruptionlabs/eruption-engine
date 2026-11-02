#pragma once

#include "net/PacketLink.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace eruption {

// Rede ruim de propósito, para teste e bancada. Vale para o que ESTE ponto
// envia (atraso de ida); simule nos dois lados para ter o RTT inteiro.
struct LinkSimConfig {
    float lossPercent = 0.0f;    // pacotes perdidos
    float burstLength = 1.0f;    // perda em rajada (Gilbert–Elliott): tamanho médio; 1 = independente
    float latencyMs = 0.0f;      // atraso fixo de ida
    float jitterMs = 0.0f;       // ± uniforme sobre o atraso
    float duplicatePercent = 0.0f;
    float reorderPercent = 0.0f; // estes ganham até 50 ms a mais e chegam fora de ordem

    bool active() const;
    // "loss=5,lat=100,jitter=10,dup=1,reorder=2,burst=3"; chave desconhecida ou
    // valor fora da faixa é erro (a bancada não pode rodar com teste errado calado).
    static bool parse(const std::string& text, LinkSimConfig& out, std::string* error = nullptr);
    std::string toString() const;
};

// Decorador: perde, atrasa, duplica e reordena o que é enviado pelo link de
// dentro. Determinístico pela semente.
class LinkSim final : public PacketLink {
public:
    LinkSim(PacketLink& inner, const LinkSimConfig& config, uint32_t seed);

    void send(const NetAddress& to, const uint8_t* data, int bytes) override;
    int receive(NetAddress& from, uint8_t* out, int maxBytes) override { return m_inner.receive(from, out, maxBytes); }
    void update(double time) override;

    uint64_t sentCount() const { return m_sent; }
    uint64_t droppedCount() const { return m_dropped; }

private:
    struct Pending {
        double due;
        NetAddress to;
        std::vector<uint8_t> data;
    };
    bool dropNext();
    double delayFor(bool reorder);

    PacketLink& m_inner;
    LinkSimConfig m_config;
    std::mt19937 m_rng;
    std::vector<Pending> m_pending;
    double m_now = 0.0;
    bool m_badState = false;
    uint64_t m_sent = 0;
    uint64_t m_dropped = 0;
};

} // namespace eruption
