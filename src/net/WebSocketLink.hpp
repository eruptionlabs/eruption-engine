#pragma once

#include "net/PacketLink.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace eruption {

// Transporte do navegador (spec 26). O navegador não abre UDP, então fala com
// o relay por um fluxo de bytes confiável e ordenado (WebSocket seguro). Este
// `PacketLink` dá semântica de datagrama a esse fluxo: cada pacote do netcode
// vira um quadro com 4 bytes de tamanho na frente, e a leitura remonta um
// pacote por chamada. A cifra continua ponta a ponta no netcode; este nível só
// enquadra bytes opacos.
//
// É ponto a ponto: o navegador só conversa com o relay, então `to` é ignorado
// no envio e `from` na leitura é sempre o endereço do par (o relay/host).
//
// O fluxo por baixo é trocável (`StreamBackend`): no wasm, a API WebSocket do
// navegador; no standalone/relay, um WebSocket mínimo sobre TCP; nos testes, um
// par em memória. O enquadramento é o mesmo nos três.

// Um fluxo de bytes confiável e ordenado, bidirecional, visto de uma ponta.
class StreamBackend {
public:
    virtual ~StreamBackend() = default;
    // Enfileira bytes para a outra ponta (tudo ou nada; um fluxo não corta no meio).
    virtual void write(const uint8_t* data, size_t bytes) = 0;
    // Copia até `max` bytes já chegados para `out`; devolve quantos (0 se nada).
    virtual size_t read(uint8_t* out, size_t max) = 0;
    virtual void update(double time) { (void)time; }
    // Falso quando o fluxo caiu (a conexão fechou): a sessão então falha.
    virtual bool connected() const { return true; }
};

class WebSocketLink final : public PacketLink {
public:
    // `peer`: o endereço que a leitura reporta e que o envio mira (o relay/host).
    WebSocketLink(std::unique_ptr<StreamBackend> backend, const NetAddress& peer);

    void send(const NetAddress& to, const uint8_t* data, int bytes) override;
    int receive(NetAddress& from, uint8_t* out, int maxBytes) override;
    void update(double time) override;

    bool connected() const { return m_backend && m_backend->connected(); }
    // O par que a leitura reporta e que o envio mira. O relay troca uma conexão
    // recém-aceita (par ainda indefinido) pelo endereço virtual da vaga depois
    // de ler o quadro de papel (ver Relay).
    void setPeer(const NetAddress& peer) { m_peer = peer; }
    const NetAddress& peer() const { return m_peer; }

private:
    std::unique_ptr<StreamBackend> m_backend;
    NetAddress m_peer;
    std::vector<uint8_t> m_in; // bytes recebidos ainda não remontados em quadros
};

// Par em memória para testes: dois backends cross-wired, sem rede. O que um
// escreve o outro lê. `latency` opcional imita o atraso de ida do trecho
// browser→relay. Não há perda: um fluxo confiável (TCP/ws) nunca perde bytes —
// a perda mora nos trechos de datagrama (UDP), com LinkSim, na etapa 1.
class MemoryStream {
public:
    // Cria o par (ponta A, ponta B).
    static std::pair<std::unique_ptr<StreamBackend>, std::unique_ptr<StreamBackend>> pair(double latency = 0.0);
};

// Porta padrão onde a ponte ws↔UDP da LAN escuta (spec 26, caso sem VPS). O
// anfitrião roda net.host na porta de jogo (kNetDefaultPort); uma ponte à parte
// escuta ws nesta porta e repassa para o host. O navegador só fala ws.
constexpr uint16_t kWebBridgePort = 41921;

// Abre um fluxo ws de cliente até `url` ("ws://ip:porta"). No wasm usa a API
// WebSocket do navegador (Emscripten); no desktop devolve nulo (o cliente
// nativo usa UDP direto). Spec 26, etapas 1b/2.
std::unique_ptr<StreamBackend> makeWsClient(const std::string& url);

} // namespace eruption
