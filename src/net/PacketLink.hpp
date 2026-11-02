#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace eruption {

// Endereço IPv4 ou IPv6 com porta (mesmo formato do netcode: "1.2.3.4:5",
// "[::1]:5").
struct NetAddress {
    enum Type : uint8_t { None = 0, IPv4 = 1, IPv6 = 2 };
    Type type = None;
    uint16_t port = 0;
    uint8_t bytes[16] = {}; // IPv4 nos 4 primeiros

    static bool parse(const std::string& text, NetAddress& out);
    std::string toString() const;
    bool operator==(const NetAddress& o) const;
    bool operator!=(const NetAddress& o) const { return !(*this == o); }
    bool operator<(const NetAddress& o) const;
};

// Por onde os pacotes do netcode passam (o netcode não abre socket: tudo
// entra por send_packet_override/receive_packet_override). Assim o mesmo
// protocolo roda sobre UDP de verdade, sobre a rede em memória dos testes,
// pelo simulador de perda e, depois, pelo túnel do relay.
class PacketLink {
public:
    virtual ~PacketLink() = default;
    virtual void send(const NetAddress& to, const uint8_t* data, int bytes) = 0;
    // Um pacote por chamada; 0 quando não há nada.
    virtual int receive(NetAddress& from, uint8_t* out, int maxBytes) = 0;
    // Uma vez por frame, antes de receber (o simulador solta os atrasados aqui).
    virtual void update(double time) { (void)time; }
};

// Socket UDP sem bloqueio (Linux, Android; Windows ainda não).
class UdpLink final : public PacketLink {
public:
    UdpLink() = default;
    ~UdpLink() override;
    UdpLink(const UdpLink&) = delete;
    UdpLink& operator=(const UdpLink&) = delete;

    // Porta 0 = o sistema escolhe; localAddress() devolve a escolhida.
    bool open(const NetAddress& bindAddress);
    void close();
    const NetAddress& localAddress() const { return m_local; }

    void send(const NetAddress& to, const uint8_t* data, int bytes) override;
    int receive(NetAddress& from, uint8_t* out, int maxBytes) override;

private:
    int m_fd = -1;
    NetAddress m_local;
};

// Rede em memória para testes: cada link tem um endereço; o que um envia
// cai na fila do destino. `tap` vê todo pacote (origem, destino, bytes) e
// pode descartá-lo (devolve false) — é como os testes medem amplificação e
// injetam replay.
class MemoryHub {
public:
    using Tap = std::function<bool(const NetAddress& from, const NetAddress& to, std::vector<uint8_t>& packet)>;

    std::unique_ptr<PacketLink> createLink(const NetAddress& address);
    void setTap(Tap tap) { m_tap = std::move(tap); }
    // Entrega um pacote como se viesse de `from` (testes de replay e forja).
    void inject(const NetAddress& from, const NetAddress& to, const uint8_t* data, int bytes);

private:
    class Link;
    struct Queued {
        NetAddress from;
        std::vector<uint8_t> data;
    };
    void deliver(const NetAddress& from, const NetAddress& to, const uint8_t* data, int bytes);

    std::map<NetAddress, std::deque<Queued>> m_queues;
    Tap m_tap;
};

} // namespace eruption
