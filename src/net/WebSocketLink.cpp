#include "net/WebSocketLink.hpp"

#include <cstring>

namespace eruption {

namespace {
constexpr size_t kLenBytes = 4;
constexpr int kMaxFrame = 4096; // igual ao teto de pacote dos canais
} // namespace

WebSocketLink::WebSocketLink(std::unique_ptr<StreamBackend> backend, const NetAddress& peer)
    : m_backend(std::move(backend)), m_peer(peer) {}

void WebSocketLink::send(const NetAddress& to, const uint8_t* data, int bytes) {
    (void)to; // ponto a ponto: só falamos com o par (relay/host)
    if (!m_backend || bytes <= 0 || bytes > kMaxFrame) return;
    uint8_t header[kLenBytes];
    const uint32_t n = static_cast<uint32_t>(bytes);
    for (size_t i = 0; i < kLenBytes; ++i) header[i] = static_cast<uint8_t>(n >> (8 * i));
    m_backend->write(header, kLenBytes);
    m_backend->write(data, static_cast<size_t>(bytes));
}

void WebSocketLink::update(double time) {
    if (m_backend) m_backend->update(time);
}

int WebSocketLink::receive(NetAddress& from, uint8_t* out, int maxBytes) {
    if (!m_backend) return 0;
    // Puxa o que chegou do fluxo para o buffer e tenta recortar um quadro.
    uint8_t chunk[kMaxFrame + kLenBytes];
    size_t got;
    while ((got = m_backend->read(chunk, sizeof(chunk))) > 0) m_in.insert(m_in.end(), chunk, chunk + got);
    if (m_in.size() < kLenBytes) return 0;
    uint32_t len = 0;
    for (size_t i = 0; i < kLenBytes; ++i) len |= static_cast<uint32_t>(m_in[i]) << (8 * i);
    if (len == 0 || len > kMaxFrame) { // quadro inválido: o fluxo está corrompido, descarta tudo
        m_in.clear();
        return 0;
    }
    if (m_in.size() < kLenBytes + len) return 0; // quadro ainda incompleto
    const int n = std::min(static_cast<int>(len), maxBytes);
    std::memcpy(out, m_in.data() + kLenBytes, static_cast<size_t>(n));
    m_in.erase(m_in.begin(), m_in.begin() + static_cast<long>(kLenBytes + len));
    from = m_peer;
    return n;
}

// ---------------------------------------------------------------- memória

namespace {

// Um canal de bytes de uma ponta para a outra, com atraso de ida opcional.
struct Pipe {
    std::deque<uint8_t> ready;                       // já liberados para leitura
    std::deque<std::pair<double, uint8_t>> inFlight; // (hora de liberar, byte)
    double latency = 0.0;
    double time = 0.0;
    void push(const uint8_t* data, size_t bytes) {
        if (latency <= 0.0)
            ready.insert(ready.end(), data, data + bytes);
        else
            for (size_t i = 0; i < bytes; ++i) inFlight.emplace_back(time + latency, data[i]);
    }
    void advance(double now) {
        time = now;
        while (!inFlight.empty() && inFlight.front().first <= now) {
            ready.push_back(inFlight.front().second);
            inFlight.pop_front();
        }
    }
    size_t pop(uint8_t* out, size_t max) {
        const size_t n = std::min(max, ready.size());
        for (size_t i = 0; i < n; ++i) {
            out[i] = ready.front();
            ready.pop_front();
        }
        return n;
    }
};

// Uma ponta: escreve no cano de saída, lê do cano de entrada. Os dois canos
// são compartilhados pelo par (shared_ptr), cross-wired.
class MemoryStreamBackend final : public StreamBackend {
public:
    MemoryStreamBackend(std::shared_ptr<Pipe> out, std::shared_ptr<Pipe> in) : m_out(std::move(out)), m_in(std::move(in)) {}
    void write(const uint8_t* data, size_t bytes) override { m_out->push(data, bytes); }
    size_t read(uint8_t* out, size_t max) override { return m_in->pop(out, max); }
    void update(double time) override {
        m_out->advance(time);
        m_in->advance(time);
    }

private:
    std::shared_ptr<Pipe> m_out, m_in;
};

} // namespace

std::pair<std::unique_ptr<StreamBackend>, std::unique_ptr<StreamBackend>> MemoryStream::pair(double latency) {
    auto ab = std::make_shared<Pipe>(); // A escreve, B lê
    auto ba = std::make_shared<Pipe>(); // B escreve, A lê
    ab->latency = latency;
    ba->latency = latency;
    return {std::make_unique<MemoryStreamBackend>(ab, ba), std::make_unique<MemoryStreamBackend>(ba, ab)};
}

#ifndef __EMSCRIPTEN__
// No desktop/Android não há WebSocket do navegador: o cliente nativo usa UDP
// direto. A implementação wasm mora em WebSocketWasm.cpp.
std::unique_ptr<StreamBackend> makeWsClient(const std::string&) { return nullptr; }
#endif

} // namespace eruption
