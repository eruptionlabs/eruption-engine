// Backend ws do navegador (spec 26, etapa 2). Dá a um WebSocketLink um fluxo
// de bytes confiável sobre a API WebSocket do navegador (Emscripten). O
// navegador não abre UDP, então é por aqui que o build web fala com a ponte
// ws↔UDP da LAN (ou, depois, com um relay wss). A cifra do netcode continua
// ponta a ponta; este nível só carrega bytes opacos, enquadrados pelo
// WebSocketLink (4 bytes de tamanho + pacote).
//
// Só compila no alvo wasm; no desktop/Android este arquivo fica vazio e o
// makeWsClient nativo (que devolve nulo) mora em WebSocketLink.cpp.
#ifdef __EMSCRIPTEN__

#include "net/WebSocketLink.hpp"

#include "core/Logger.hpp"

#include <emscripten/websocket.h>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace eruption {

namespace {

// Uma conexão WebSocket do navegador vista como StreamBackend. Os retornos de
// chamada do navegador rodam no mesmo laço do motor (sem threads neste build),
// então não há corrida: o que chega entre dois quadros está no buffer quando
// o read() do quadro seguinte roda.
class WasmWsBackend final : public StreamBackend {
public:
    explicit WasmWsBackend(const std::string& url) {
        EmscriptenWebSocketCreateAttributes attr;
        emscripten_websocket_init_create_attributes(&attr);
        attr.url = url.c_str();
        attr.createOnMainThread = EM_TRUE;
        m_sock = emscripten_websocket_new(&attr);
        if (m_sock <= 0) {
            ERUPTION_LOG_ERROR("net: WebSocket não abriu para %s", url.c_str());
            m_closed = true;
            return;
        }
        emscripten_websocket_set_onopen_callback(m_sock, this, onOpen);
        emscripten_websocket_set_onmessage_callback(m_sock, this, onMessage);
        emscripten_websocket_set_onerror_callback(m_sock, this, onError);
        emscripten_websocket_set_onclose_callback(m_sock, this, onClose);
    }

    ~WasmWsBackend() override {
        if (m_sock > 0) {
            emscripten_websocket_close(m_sock, 1000, "fim");
            emscripten_websocket_delete(m_sock);
        }
    }

    WasmWsBackend(const WasmWsBackend&) = delete;
    WasmWsBackend& operator=(const WasmWsBackend&) = delete;

    void write(const uint8_t* data, size_t bytes) override {
        if (m_closed || bytes == 0) return;
        if (m_open) {
            emscripten_websocket_send_binary(m_sock, const_cast<uint8_t*>(data), static_cast<uint32_t>(bytes));
        } else {
            m_pending.insert(m_pending.end(), data, data + bytes); // antes de abrir: enfileira
        }
    }

    size_t read(uint8_t* out, size_t max) override {
        const size_t n = std::min(max, m_in.size());
        for (size_t i = 0; i < n; ++i) {
            out[i] = m_in.front();
            m_in.pop_front();
        }
        return n;
    }

    bool connected() const override { return !m_closed; }

private:
    static WasmWsBackend* self(void* p) { return static_cast<WasmWsBackend*>(p); }

    static EM_BOOL onOpen(int, const EmscriptenWebSocketOpenEvent*, void* userData) {
        WasmWsBackend* b = self(userData);
        b->m_open = true;
        if (!b->m_pending.empty()) {
            emscripten_websocket_send_binary(b->m_sock, b->m_pending.data(), static_cast<uint32_t>(b->m_pending.size()));
            b->m_pending.clear();
        }
        return EM_TRUE;
    }

    static EM_BOOL onMessage(int, const EmscriptenWebSocketMessageEvent* e, void* userData) {
        WasmWsBackend* b = self(userData);
        if (e && !e->isText && e->numBytes > 0) b->m_in.insert(b->m_in.end(), e->data, e->data + e->numBytes);
        return EM_TRUE;
    }

    static EM_BOOL onError(int, const EmscriptenWebSocketErrorEvent*, void* userData) {
        self(userData)->m_closed = true;
        return EM_TRUE;
    }

    static EM_BOOL onClose(int, const EmscriptenWebSocketCloseEvent*, void* userData) {
        self(userData)->m_closed = true;
        return EM_TRUE;
    }

    EMSCRIPTEN_WEBSOCKET_T m_sock = 0;
    std::deque<uint8_t> m_in;       // bytes recebidos, ainda não lidos pelo WebSocketLink
    std::vector<uint8_t> m_pending; // escrito antes de a conexão abrir
    bool m_open = false;
    bool m_closed = false;
};

} // namespace

std::unique_ptr<StreamBackend> makeWsClient(const std::string& url) {
    if (!emscripten_websocket_is_supported()) {
        ERUPTION_LOG_ERROR("net: este navegador não tem WebSocket");
        return nullptr;
    }
    return std::make_unique<WasmWsBackend>(url);
}

} // namespace eruption

#endif // __EMSCRIPTEN__
