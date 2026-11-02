#pragma once
// Pares hostis para teste e fuzz da sessão (tests/test_net_matrix.cpp,
// tools/fuzz/net_session_fuzz.cpp): passam pelo aperto de mão de verdade
// (token lacrado, netcode) e depois mandam o que quiserem por dentro da
// conexão autenticada. É o atacante que interessa: quem tem um token válido.
//  - HostileClient contra um NetSession servidor;
//  - HostileServer (NetServer + TokenGate próprios) contra um NetSession cliente.
// Formato das mensagens de sistema igual ao de src/net/NetSession.cpp.
#include "net/Channels.hpp"
#include "net/NetSession.hpp"
#include "net/NetcodeEndpoint.hpp"
#include "net/PacketLink.hpp"
#include "net/TokenGate.hpp"
#include "net/Wire.hpp"

#include <memory>
#include <string>
#include <vector>

namespace eruption {
namespace hostile {

constexpr double kDt = 1.0 / 60.0;
constexpr const char* kServerAddr = "10.0.0.1:41920";
constexpr const char* kClientAddr = "10.0.0.66:7000";
enum : int32_t { kHello = 0, kWelcome = 1, kReject = 2, kJoined = 3, kLeft = 4 };

inline NetAddress addr(const char* text) {
    NetAddress a;
    NetAddress::parse(text, a);
    return a;
}

inline ContentHash hashOf(const std::string& text) {
    ContentHash h{};
    Blake2b b(32);
    b.update(text.data(), text.size());
    b.final(h.data());
    return h;
}

inline std::vector<uint8_t> finish(BitWriter& w, std::vector<uint8_t>& buf) {
    buf.resize(w.finish());
    return buf;
}

inline std::vector<uint8_t> helloMessage(const ContentHash& hash, const std::string& name) {
    std::vector<uint8_t> buf(128);
    BitWriter w(buf.data(), buf.size());
    w.writeInt(kHello, 0, 7);
    w.writeBytes(hash.data(), hash.size());
    w.writeString(name, 24);
    return finish(w, buf);
}

// Mensagem de sistema de um tipo qualquer com bytes arbitrários depois.
inline std::vector<uint8_t> systemMessage(int32_t type, const std::vector<uint8_t>& tail) {
    std::vector<uint8_t> buf(tail.size() + 8);
    BitWriter w(buf.data(), buf.size());
    w.writeInt(type, 0, 7);
    w.writeBytes(tail.data(), tail.size());
    return finish(w, buf);
}

inline std::vector<uint8_t> eventMessage(const std::string& name, const std::vector<uint8_t>& data, size_t nameLimit = 32) {
    std::vector<uint8_t> buf(name.size() + data.size() + 16);
    BitWriter w(buf.data(), buf.size());
    w.writeString(name, nameLimit);
    w.alignToByte();
    w.writeBytes(data.data(), data.size());
    return finish(w, buf);
}

inline std::string rejectReason(const std::vector<uint8_t>& msg) {
    BitReader r(msg.data(), msg.size());
    if (r.readInt(0, 7) != kReject) return {};
    std::string reason;
    r.readString(reason, 32);
    return r.ok() ? reason : std::string();
}

// Corpo de pacote dos canais forjado, com o cabeçalho real do reliable:
// gera um pacote com 1 mensagem sem garantia de n bytes (corpo = 3 + n + 1)
// e troca o corpo. A sequência do reliable (bytes 1-2, little-endian) vai
// adiante da conexão de verdade, senão o reliable descarta como repetido.
inline std::vector<uint8_t> forgeChannelPacket(const std::vector<uint8_t>& body, double time, uint16_t sequence = 30000) {
    Connection sender;
    std::vector<uint8_t> captured;
    sender.init([&](const uint8_t* d, int len) { captured.assign(d, d + len); }, time);
    std::vector<uint8_t> filler(body.empty() ? 1 : body.size(), 0);
    sender.sendUnreliable(filler.data(), static_cast<int>(filler.size()));
    sender.update(time + 0.01);
    if (captured.size() < filler.size() + 4) return {};
    std::vector<uint8_t> out(captured.begin(), captured.end() - static_cast<long>(filler.size() + 4));
    out[1] = static_cast<uint8_t>(sequence);
    out[2] = static_cast<uint8_t>(sequence >> 8);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

struct HostileClient {
    std::unique_ptr<PacketLink> link;
    TokenRequest token;
    NetClient client;
    Connection conn;
    bool connecting = false;
    std::vector<std::pair<int, std::vector<uint8_t>>> received;

    void start(MemoryHub& hub, double time) {
        link = hub.createLink(addr(kClientAddr));
        token.start(kServerAddr, *link, time);
    }
    bool ready() const { return conn.active(); }
    void update(double time) {
        if (!connecting) {
            token.update(time);
            if (token.state() != TokenRequest::State::Done) return;
            client.connect(token.token(), "0.0.0.0:0", *link, time);
            connecting = true;
        }
        client.update(time);
        if (client.state() == NetClient::State::Connected && !conn.active())
            conn.init([this](const uint8_t* d, int n) { client.send(d, n); }, time);
        if (!conn.active()) return;
        std::vector<uint8_t> pkt;
        while (client.receive(pkt)) conn.receivePacket(pkt.data(), static_cast<int>(pkt.size()));
        int ch = 0;
        std::vector<uint8_t> msg;
        while (conn.receiveReliable(ch, msg)) received.emplace_back(ch, msg);
        conn.update(time);
    }
    std::string kickedFor() const {
        for (const auto& m : received)
            if (m.first == 0) {
                const std::string r = rejectReason(m.second);
                if (!r.empty()) return r;
            }
        return {};
    }
    bool welcomed() const {
        for (const auto& m : received) {
            BitReader r(m.second.data(), m.second.size());
            if (m.first == 0 && r.readInt(0, 7) == kWelcome) return true;
        }
        return false;
    }
};

// Servidor dedicado de verdade + cliente hostil, ligados em memória.
struct ServerArena {
    MemoryHub hub;
    std::unique_ptr<PacketLink> serverLink;
    NetSession server;
    HostileClient raw;
    std::vector<NetEvent> events;
    double time = 0.0;

    bool start(const std::string& allowed = "ok", size_t maxBytes = 16) {
        serverLink = hub.createLink(addr(kServerAddr));
        NetHostOptions o;
        o.port = 41920;
        o.dedicated = true;
        o.maxPlayers = 2;
        o.content = hashOf("conteudo");
        if (!server.hostOn(*serverLink, o, time)) return false;
        server.allowFromClient(allowed, maxBytes);
        raw.start(hub, time);
        for (int i = 0; i < 300 && !raw.ready(); ++i) step();
        return raw.ready();
    }
    void step(int frames = 1) {
        for (int f = 0; f < frames; ++f) {
            time += kDt;
            server.update(time);
            raw.update(time);
            NetEvent e;
            while (server.poll(e)) events.push_back(std::move(e));
        }
    }
    bool hello() {
        const std::vector<uint8_t> m = helloMessage(hashOf("conteudo"), "x");
        raw.conn.sendReliable(0, m.data(), static_cast<int>(m.size()));
        for (int i = 0; i < 60 && !raw.welcomed(); ++i) step();
        return raw.welcomed();
    }
    int messages(const std::string& name) const {
        int n = 0;
        for (const NetEvent& e : events) n += e.type == NetEvent::Type::Message && e.name == name;
        return n;
    }
};

// Cliente de verdade (NetSession) + servidor hostil.
struct ClientArena {
    MemoryHub hub;
    std::unique_ptr<PacketLink> serverLink, clientLink;
    std::unique_ptr<TokenGate> gate;
    NetServer server;
    NetKey key = makeNetKey();
    Connection conn;
    NetSession client;
    std::vector<NetEvent> events;
    double time = 0.0;

    bool start() {
        serverLink = hub.createLink(addr(kServerAddr));
        clientLink = hub.createLink(addr(kClientAddr));
        gate = std::make_unique<TokenGate>(*serverLink, 41920, kNetProtocolId,
                                           [this](const std::string& dialed, std::vector<uint8_t>& token) {
                                               ConnectTokenRequest req;
                                               req.publicAddresses = {dialed};
                                               req.internalAddresses = {"0.0.0.0:41920"};
                                               req.clientId = 7;
                                               req.protocolId = kNetProtocolId;
                                               return makeConnectToken(req, key, token);
                                           });
        if (!server.start("0.0.0.0:41920", kNetProtocolId, key, 2, *gate, time)) return false;
        client.joinOn(*clientLink, kServerAddr, "alvo", hashOf("conteudo"), time);
        for (int i = 0; i < 300 && !conn.active(); ++i) step();
        return conn.active();
    }
    void step(int frames = 1) {
        for (int f = 0; f < frames; ++f) {
            time += kDt;
            server.update(time);
            if (server.connected(0) && !conn.active())
                conn.init([this](const uint8_t* d, int n) { server.send(0, d, n); }, time);
            if (conn.active()) {
                std::vector<uint8_t> pkt;
                while (server.receive(0, pkt)) conn.receivePacket(pkt.data(), static_cast<int>(pkt.size()));
                int ch = 0;
                std::vector<uint8_t> msg;
                while (conn.receiveReliable(ch, msg)) {
                }
                conn.update(time);
            }
            client.update(time);
            NetEvent e;
            while (client.poll(e)) events.push_back(std::move(e));
        }
    }
};

} // namespace hostile
} // namespace eruption
