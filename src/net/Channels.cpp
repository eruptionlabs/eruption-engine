#include "net/Channels.hpp"

#include "core/Logger.hpp"
#include "net/Wire.hpp"

#include <reliable.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace eruption {

namespace {

constexpr int kPacketRecords = 512;          // > janela de acks do reliable (256)
constexpr int kMaxUnreliablePerPacket = 16;
constexpr size_t kMaxUnreliableQueued = 64;  // entrada sem garantia que ninguém leu
constexpr int kReliableBudget = 1000;        // bytes de mensagens confiáveis por pacote

int reliablePrintf(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    ERUPTION_LOG_WARN("reliable: %s", line);
    return n;
}

size_t slot(uint16_t id) { return id % kNetMessageWindow; }

// Bits de cabeçalho de uma mensagem confiável, sem o alinhamento.
int reliableHeaderBits() {
    return bitsForRange(kNetChannels - 1) + 16 + bitsForRange(kNetMaxMessageBytes - 1);
}

} // namespace

Connection::Connection() : m_packets(kPacketRecords) {}
Connection::~Connection() { reset(); }

bool Connection::init(Transmit transmit, double time) {
    reset();
    reliable_set_printf_function(reliablePrintf);
    reliable_log_level(RELIABLE_LOG_LEVEL_ERROR);
    reliable_config_t config;
    reliable_default_config(&config);
    std::snprintf(config.name, sizeof(config.name), "eruption");
    config.context = this;
    config.max_packet_size = kNetMaxPacketBytes;
    config.fragment_above = 1024;
    config.fragment_size = 1024;
    config.max_fragments = (kNetMaxPacketBytes + 1023) / 1024;
    config.fragment_reassembly_buffer_size = 16;
    config.transmit_packet_function = transmitCallback;
    config.process_packet_function = processCallback;
    m_endpoint = reliable_endpoint_create(&config, time);
    if (!m_endpoint) {
        ERUPTION_LOG_ERROR("net: reliable_endpoint_create falhou");
        return false;
    }
    m_transmit = std::move(transmit);
    m_time = time;
    return true;
}

void Connection::reset() {
    if (m_endpoint) reliable_endpoint_destroy(m_endpoint);
    m_endpoint = nullptr;
    m_transmit = nullptr;
    m_ackOwed = false;
    m_failReason = nullptr;
    m_send = {};
    m_recv = {};
    std::fill(m_packets.begin(), m_packets.end(), PacketRecord{});
    m_unreliableOut.clear();
    m_unreliableIn.clear();
    m_ready.clear();
}

void Connection::fail(const char* reason) {
    if (!m_failReason) m_failReason = reason;
}

bool Connection::sendReliable(int channel, const uint8_t* data, int bytes) {
    if (!m_endpoint || failed() || channel < 0 || channel >= kNetChannels || bytes <= 0 ||
        bytes > kNetMaxMessageBytes)
        return false;
    SendChannel& c = m_send[static_cast<size_t>(channel)];
    if (static_cast<uint16_t>(c.next - c.oldest) >= kNetMessageWindow) {
        fail("fila_cheia");
        return false;
    }
    SentMessage& m = c.window[slot(c.next)];
    m.used = true;
    m.id = c.next;
    m.lastSent = -1.0;
    m.data.assign(data, data + bytes);
    ++c.next;
    return true;
}

bool Connection::sendUnreliable(const uint8_t* data, int bytes) {
    if (!m_endpoint || failed() || bytes <= 0 || bytes > kNetMaxPacketBytes - 64) return false;
    if (static_cast<int>(m_unreliableOut.size()) >= kMaxUnreliablePerPacket) return false;
    // O sem-garantia de um pacote cabe no pacote com folga para as confiáveis.
    size_t queued = static_cast<size_t>(bytes);
    for (const std::vector<uint8_t>& m : m_unreliableOut) queued += m.size() + 3;
    if (queued > static_cast<size_t>(kNetMaxPacketBytes - kReliableBudget - 64)) return false;
    m_unreliableOut.emplace_back(data, data + bytes);
    return true;
}

void Connection::receivePacket(const uint8_t* data, int bytes) {
    if (!m_endpoint || failed() || bytes <= 0) return;
    // O reliable não altera o pacote, mas a assinatura pede não-const.
    m_scratch.assign(data, data + bytes);
    reliable_endpoint_receive_packet(m_endpoint, m_scratch.data(), bytes);
    processAcks();
}

void Connection::transmitCallback(void* ctx, uint64_t, uint16_t, uint8_t* data, int bytes) {
    Connection* self = static_cast<Connection*>(ctx);
    if (self->m_transmit) self->m_transmit(data, bytes);
}

int Connection::processCallback(void* ctx, uint64_t, uint16_t, uint8_t* data, int bytes) {
    Connection* self = static_cast<Connection*>(ctx);
    if (self->failed()) return 0;
    if (!self->parsePacket(data, bytes)) {
        self->fail("pacote_invalido");
        return 0;
    }
    self->m_ackOwed = true;
    return 1;
}

bool Connection::parsePacket(const uint8_t* data, int bytes) {
    BitReader r(data, static_cast<size_t>(bytes));
    const int unreliable = r.readInt(0, kMaxUnreliablePerPacket);
    for (int i = 0; i < unreliable && r.ok(); ++i) {
        const int len = r.readInt(1, kNetMaxPacketBytes);
        r.alignToByte();
        std::vector<uint8_t> msg(static_cast<size_t>(std::min<size_t>(static_cast<size_t>(len), r.bitsRemaining() / 8)));
        if (msg.size() != static_cast<size_t>(len) || !r.readBytes(msg.data(), msg.size())) return false;
        if (m_unreliableIn.size() >= kMaxUnreliableQueued) m_unreliableIn.pop_front();
        m_unreliableIn.push_back(std::move(msg));
    }
    const int reliable = r.readInt(0, kNetMaxMessagesPerPacket);
    for (int i = 0; i < reliable && r.ok(); ++i) {
        const int channel = r.readInt(0, kNetChannels - 1);
        const uint16_t id = static_cast<uint16_t>(r.readBits(16));
        const int len = r.readInt(1, kNetMaxMessageBytes);
        r.alignToByte();
        if (!r.ok() || r.bitsRemaining() / 8 < static_cast<size_t>(len)) return false;
        RecvChannel& c = m_recv[static_cast<size_t>(channel)];
        const int16_t ahead = static_cast<int16_t>(static_cast<uint16_t>(id - c.next));
        if (ahead >= kNetMessageWindow) return false; // a outra ponta nunca manda além da janela
        if (ahead < 0 || c.present[slot(id)]) {
            // Reenvio do que já chegou: pula os bytes.
            std::vector<uint8_t> skip(static_cast<size_t>(len));
            if (!r.readBytes(skip.data(), skip.size())) return false;
            continue;
        }
        std::vector<uint8_t>& dst = c.window[slot(id)];
        dst.resize(static_cast<size_t>(len));
        if (!r.readBytes(dst.data(), dst.size())) return false;
        c.present[slot(id)] = true;
    }
    if (!r.ok()) return false;
    // Só entrega depois do pacote inteiro validar.
    for (int ch = 0; ch < kNetChannels; ++ch) {
        RecvChannel& c = m_recv[static_cast<size_t>(ch)];
        while (c.present[slot(c.next)]) {
            c.present[slot(c.next)] = false;
            m_ready.push_back({ch, std::move(c.window[slot(c.next)])});
            c.window[slot(c.next)].clear();
            ++c.next;
        }
    }
    return true;
}

void Connection::processAcks() {
    int count = 0;
    const uint16_t* acks = reliable_endpoint_get_acks(m_endpoint, &count);
    for (int i = 0; i < count; ++i) {
        PacketRecord& p = m_packets[acks[i] % kPacketRecords];
        if (!p.used || p.sequence != acks[i]) continue;
        for (int k = 0; k < p.count; ++k) {
            SendChannel& c = m_send[p.channel[static_cast<size_t>(k)]];
            SentMessage& m = c.window[slot(p.id[static_cast<size_t>(k)])];
            if (m.used && m.id == p.id[static_cast<size_t>(k)]) {
                m.used = false;
                m.data.clear();
            }
        }
        p.used = false;
    }
    reliable_endpoint_clear_acks(m_endpoint);
    for (SendChannel& c : m_send)
        while (c.oldest != c.next && !c.window[slot(c.oldest)].used) ++c.oldest;
}

double Connection::resendDelay() const {
    const double rtt = m_endpoint ? reliable_endpoint_rtt(m_endpoint) / 1000.0 : 0.0;
    return std::clamp(rtt * 1.25 + 0.02, 0.1, 1.0);
}

void Connection::update(double time) {
    if (!m_endpoint) return;
    m_time = time;
    reliable_endpoint_update(m_endpoint, time);
    if (failed()) return;

    // Escolhe as mensagens confiáveis devidas, a mais antiga primeiro.
    struct Pick {
        uint8_t channel;
        uint16_t id;
    };
    std::array<Pick, kNetMaxMessagesPerPacket> picks{};
    int picked = 0;
    int budget = kReliableBudget;
    const double delay = resendDelay();
    for (int ch = 0; ch < kNetChannels && picked < kNetMaxMessagesPerPacket; ++ch) {
        SendChannel& c = m_send[static_cast<size_t>(ch)];
        for (uint16_t id = c.oldest; id != c.next && picked < kNetMaxMessagesPerPacket; ++id) {
            SentMessage& m = c.window[slot(id)];
            if (!m.used || (m.lastSent >= 0.0 && time - m.lastSent < delay)) continue;
            const int cost = static_cast<int>(m.data.size()) + 5;
            if (cost > budget && picked > 0) break;
            budget -= cost;
            picks[static_cast<size_t>(picked++)] = {static_cast<uint8_t>(ch), id};
        }
    }
    if (picked == 0 && m_unreliableOut.empty() && !m_ackOwed) return;

    std::vector<uint8_t> buffer(kNetMaxPacketBytes);
    BitWriter w(buffer.data(), buffer.size());
    w.writeInt(static_cast<int32_t>(m_unreliableOut.size()), 0, kMaxUnreliablePerPacket);
    for (const std::vector<uint8_t>& msg : m_unreliableOut) {
        w.writeInt(static_cast<int32_t>(msg.size()), 1, kNetMaxPacketBytes);
        w.alignToByte();
        w.writeBytes(msg.data(), msg.size());
    }
    m_unreliableOut.clear();
    // O que não couber depois do sem-garantia fica para o próximo pacote.
    int fit = 0;
    for (; fit < picked; ++fit) {
        const SentMessage& m = m_send[picks[static_cast<size_t>(fit)].channel].window[slot(picks[static_cast<size_t>(fit)].id)];
        const size_t need = static_cast<size_t>(reliableHeaderBits() + 8) + m.data.size() * 8 + 16;
        if (w.bitsWritten() + need > buffer.size() * 8 - 16) break;
    }
    w.writeInt(fit, 0, kNetMaxMessagesPerPacket);
    const uint16_t sequence = reliable_endpoint_next_packet_sequence(m_endpoint);
    PacketRecord& record = m_packets[sequence % kPacketRecords];
    record.used = fit > 0;
    record.sequence = sequence;
    record.count = fit;
    for (int i = 0; i < fit; ++i) {
        const Pick& p = picks[static_cast<size_t>(i)];
        SentMessage& m = m_send[p.channel].window[slot(p.id)];
        w.writeInt(p.channel, 0, kNetChannels - 1);
        w.writeBits(p.id, 16);
        w.writeInt(static_cast<int32_t>(m.data.size()), 1, kNetMaxMessageBytes);
        w.alignToByte();
        w.writeBytes(m.data.data(), m.data.size());
        m.lastSent = time;
        record.channel[static_cast<size_t>(i)] = p.channel;
        record.id[static_cast<size_t>(i)] = p.id;
    }
    const size_t bytes = w.finish();
    if (!w.ok()) {
        fail("pacote_grande");
        return;
    }
    m_ackOwed = false;
    reliable_endpoint_send_packet(m_endpoint, buffer.data(), static_cast<int>(bytes));
}

bool Connection::receiveReliable(int& channel, std::vector<uint8_t>& out) {
    if (m_ready.empty()) return false;
    channel = m_ready.front().channel;
    out = std::move(m_ready.front().data);
    m_ready.pop_front();
    return true;
}

bool Connection::receiveUnreliable(std::vector<uint8_t>& out) {
    if (m_unreliableIn.empty()) return false;
    out = std::move(m_unreliableIn.front());
    m_unreliableIn.pop_front();
    return true;
}

int Connection::pendingReliable() const {
    int n = 0;
    for (const SendChannel& c : m_send) n += static_cast<uint16_t>(c.next - c.oldest);
    return n;
}

ConnectionStats Connection::stats() const {
    ConnectionStats s;
    if (!m_endpoint) return s;
    s.rttMs = reliable_endpoint_rtt(m_endpoint);
    s.jitterMs = reliable_endpoint_jitter_avg_vs_min_rtt(m_endpoint);
    s.lossPercent = reliable_endpoint_packet_loss(m_endpoint);
    float acked = 0.0f;
    reliable_endpoint_bandwidth(m_endpoint, &s.kbpsOut, &s.kbpsIn, &acked);
    const uint64_t* counters = reliable_endpoint_counters(m_endpoint);
    s.packetsOut = counters[RELIABLE_ENDPOINT_COUNTER_NUM_PACKETS_SENT];
    s.packetsIn = counters[RELIABLE_ENDPOINT_COUNTER_NUM_PACKETS_RECEIVED];
    return s;
}

} // namespace eruption
