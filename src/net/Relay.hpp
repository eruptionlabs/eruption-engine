#pragma once

#include "net/NetcodeEndpoint.hpp"
#include "net/PacketLink.hpp"
#include "net/WebSocketLink.hpp" // StreamBackend, WebSocketLink (ponte ws do navegador)

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace eruption {

// Túnel por uma VPS para quem está atrás de CGNAT hospedar partida (spec 22
// §13, doc 24 item 5). O host abre uma sala no relay; o relay dá uma porta
// UDP pública para a sala; os clientes discam "ip-da-vps:porta-da-sala" e
// nunca veem o IP do host.
//
// O relay é cego: os pacotes do jogo seguem cifrados de ponta a ponta pelo
// netcode (o relay não tem as chaves, não lê nem forja). O canal host<->relay
// é autenticado com a chave do relay (XChaCha20-Poly1305, Monocypher) e um
// contador que só cresce (pacote repetido ou velho é descartado). Regras do
// relay:
//  - nunca responde a quem não autenticou (nem erro): não amplifica nada;
//  - só manda pacote para o host registrado da sala e para endereços que
//    já mandaram pacote para aquela sala; nunca para endereço privado,
//    loopback ou multicast (a menos que --allow-private, para teste em LAN);
//  - sala sem sinal do host por 10 s fecha; teto de salas, de clientes por
//    sala e de pacotes por segundo por endereço de cliente.
//
// O host vê cada cliente com um endereço virtual no prefixo 100:0:0:1::/64
// (RFC 6666, nunca roteado): o resto do motor (token, netcode, limites por
// endereço) funciona igual.

using RelayKey = std::array<uint8_t, 32>;

constexpr uint16_t kRelayDefaultPort = 41921;
constexpr int kRelayOverheadBytes = 4 + 1 + 24 + 16 + 2; // mágica + tipo + nonce (prefixo + contador) + mac + vaga

// Janela anti-replay de 64 pacotes (como no IPsec): aceita contador novo, ou
// até 64 atrás que ainda não veio (o UDP reordena).
struct RelayReplayWindow {
    uint64_t last = 0;
    uint64_t bits = 0;
    bool seen = false;
    bool accept(uint64_t counter);
};

bool parseRelayKey(const std::string& hex, RelayKey& out);
std::string relayKeyToHex(const RelayKey& key);

// Transporte do navegador (spec 26, etapa 1): o navegador não abre UDP, então
// fala com o relay por um fluxo ws. O relay aceita essas conexões, dá a cada
// uma um endereço virtual interno e as trata como mais um par — os mesmos
// bytes opacos do netcode/ERRL que o UDP carregaria. O relay segue cego.
//
// O primeiro quadro de uma conexão ws diz o papel: hospedar (a sala nasce pelo
// ERRL, como no UDP) ou entrar numa sala já aberta (porta da sala, 2 bytes LE).
// Depois dele, a conexão carrega só netcode/ERRL.
enum : uint8_t { kStreamHost = 'H', kStreamJoin = 'J' };

// Fonte de conexões de fluxo (ws) já aceitas. No standalone é um aceitador
// TCP/ws (etapa 1b); nos testes, entrega pares em memória prontos. accept()
// devolve nulo quando não há conexão nova pendente.
class StreamAcceptor {
public:
    virtual ~StreamAcceptor() = default;
    virtual std::unique_ptr<StreamBackend> accept() = 0;
    virtual void update(double time) { (void)time; }
};

class RelayServer {
public:
    using LinkFactory = std::function<std::unique_ptr<PacketLink>(uint16_t port)>;

    struct Options {
        uint16_t controlPort = kRelayDefaultPort;
        uint16_t firstRoomPort = 41930;
        int maxRooms = 64;
        int maxClientsPerRoom = 32;
        bool allowPrivate = false;
    };

    RelayServer() = default;
    ~RelayServer();
    RelayServer(const RelayServer&) = delete;
    RelayServer& operator=(const RelayServer&) = delete;

    // `factory` abre um link na porta pedida (UDP de verdade ou memória nos testes).
    bool start(const Options& options, const RelayKey& key, LinkFactory factory, double time);
    // Opcional: conexões ws do navegador (spec 26). Sem acceptor, o relay é só UDP.
    void setStreamAcceptor(std::unique_ptr<StreamAcceptor> acceptor);
    void update(double time);

    int streamConnections() const { return static_cast<int>(m_ws.size()); }

    int rooms() const { return static_cast<int>(m_rooms.size()); }
    uint64_t forwarded() const { return m_forwarded; }
    uint64_t rejected() const { return m_rejected; }
    uint64_t refusedDestinations() const { return m_refusedDestinations; }

private:
    struct Client {
        NetAddress address;
        double lastSeen = 0.0;
        double tokens = 0.0;
    };
    struct Room {
        NetAddress host;
        uint64_t session = 0;
        std::array<uint8_t, 16> hostPrefix{}; // prefixo do nonce do host nesta sessão
        RelayReplayWindow fromHost;
        uint64_t relayCounter = 0;  // nosso contador para o host
        double lastHost = 0.0;
        uint16_t port = 0;
        std::unique_ptr<PacketLink> link;
        std::map<uint16_t, Client> clients; // vaga -> cliente
    };

    // Uma conexão ws já com papel definido: host (caminho de controle) ou
    // cliente de uma sala. O endereço é virtual e interno (nunca sai na rede).
    struct Stream {
        std::unique_ptr<WebSocketLink> link;
        NetAddress address;    // virtual (ver wsAddress)
        uint16_t roomPort = 0; // 0 = host; senão a sala do cliente
    };

    void handleControl(const NetAddress& from, const uint8_t* data, int bytes);
    void handleRoomPacket(Room& room, const NetAddress& from, const uint8_t* data, int bytes);
    void sendToHost(Room& room, uint8_t type, uint16_t slot, const uint8_t* payload, int bytes);
    // Manda por `base` (UDP), ou pela conexão ws se `to` for de uma delas.
    void emit(PacketLink& base, const NetAddress& to, const uint8_t* data, int bytes);
    void pumpStreams(double time); // aceita, lê o papel e entrega os quadros ws
    NetAddress wsAddress();         // próximo endereço virtual de conexão ws
    bool destinationAllowed(const NetAddress& a) const;
    uint16_t freeRoomPort() const;

    Options m_options;
    RelayKey m_key{};
    std::array<uint8_t, 16> m_prefix{}; // prefixo do nosso nonce
    LinkFactory m_factory;
    std::unique_ptr<PacketLink> m_control;
    std::map<uint16_t, std::unique_ptr<Room>> m_rooms; // porta -> sala
    std::unique_ptr<StreamAcceptor> m_acceptor;
    std::vector<Stream> m_ws;                              // conexões ws com papel
    std::vector<std::pair<std::unique_ptr<WebSocketLink>, double>> m_wsPending; // sem papel (quando aceitas)
    uint32_t m_wsCounter = 0;
    double m_time = 0.0;
    std::vector<uint8_t> m_buffer;
    uint64_t m_forwarded = 0;
    uint64_t m_rejected = 0;
    uint64_t m_refusedDestinations = 0;
};

// Lado do host: um PacketLink que fala com o relay. O netcode do host manda
// e recebe por ele como se fosse UDP; os endereços dos clientes são virtuais.
class RelayLink final : public PacketLink {
public:
    RelayLink(PacketLink& toRelay, const NetAddress& relayControl, const RelayKey& key);
    ~RelayLink() override; // avisa o relay para fechar a sala
    RelayLink(const RelayLink&) = delete;
    RelayLink& operator=(const RelayLink&) = delete;

    void send(const NetAddress& to, const uint8_t* data, int bytes) override;
    int receive(NetAddress& from, uint8_t* out, int maxBytes) override;
    void update(double time) override;

    bool ready() const { return m_roomPort != 0; }
    // "ip-da-vps:porta-da-sala" que os clientes discam (vazio até registrar).
    std::string publicAddress() const;
    uint16_t roomPort() const { return m_roomPort; }

private:
    void sendControl(uint8_t type, uint16_t slot, const uint8_t* payload, int bytes);

    PacketLink& m_relay;
    NetAddress m_control;
    RelayKey m_key;
    uint64_t m_session = 0;
    std::array<uint8_t, 16> m_prefix{};
    uint64_t m_counter = 0;       // nosso
    RelayReplayWindow m_fromRelay;
    uint16_t m_roomPort = 0;
    double m_time = 0.0;
    double m_lastSend = -1e9;
    std::vector<uint8_t> m_buffer;
};

// Endereço virtual de uma vaga do relay (e o inverso; falso se não é um).
NetAddress relayVirtualAddress(uint16_t slot);
bool relaySlotOf(const NetAddress& a, uint16_t& slot);

} // namespace eruption
