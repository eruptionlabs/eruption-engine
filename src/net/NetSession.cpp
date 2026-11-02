#include "net/NetSession.hpp"

#include "core/Logger.hpp"
#include "net/NetcodeEndpoint.hpp"
#include "net/PacketLink.hpp"
#include "net/Relay.hpp"
#include "net/TokenGate.hpp"
#include "net/WebSocketLink.hpp"
#include "net/Wire.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace eruption {

namespace {

constexpr uint64_t kProtocolId = kNetProtocolId;
constexpr int kSystemChannel = 0;
constexpr int kEventChannel = 1;
constexpr double kHelloTimeout = 5.0;
constexpr double kRejectLinger = 1.0;  // espera o ack da recusa antes de derrubar
constexpr double kEventRate = 30.0;    // eventos/s por cliente
constexpr double kEventBurst = 60.0;
constexpr int kTokenExpire = 30;
constexpr int kTokenTimeout = 8;

enum SystemMessage : int32_t { kHello = 0, kWelcome = 1, kReject = 2, kJoined = 3, kLeft = 4, kSystemTypes = 7 };
constexpr size_t kMaxReasonBytes = 32;

// Endereços do link em memória do jogador local do host, num prefixo que a
// internet descarta (100::/64, RFC 6666): nunca colide com um par real.
const char* kLoopServer = "[100::1]:1";
const char* kLoopClient = "[100::2]:1";

bool isLoopback(const NetAddress& a) {
    static const uint8_t prefix[8] = {0x01, 0x00, 0, 0, 0, 0, 0, 0};
    return a.type == NetAddress::IPv6 && std::memcmp(a.bytes, prefix, 8) == 0;
}

// Junta o link de rede e o link em memória do jogador local. Pacote da rede
// que diz vir do prefixo da memória é forjado: descartado.
class MuxLink final : public PacketLink {
public:
    MuxLink(PacketLink& net, PacketLink* local) : m_net(net), m_local(local) {}
    void send(const NetAddress& to, const uint8_t* data, int bytes) override {
        if (isLoopback(to)) {
            if (m_local) m_local->send(to, data, bytes);
        } else {
            m_net.send(to, data, bytes);
        }
    }
    int receive(NetAddress& from, uint8_t* out, int maxBytes) override {
        if (m_local) {
            const int n = m_local->receive(from, out, maxBytes);
            if (n > 0) return n;
        }
        for (;;) {
            const int n = m_net.receive(from, out, maxBytes);
            if (n <= 0 || !isLoopback(from)) return n;
        }
    }
    void update(double time) override {
        m_net.update(time);
        if (m_local) m_local->update(time);
    }

private:
    PacketLink& m_net;
    PacketLink* m_local;
};

uint64_t randomId() {
    std::random_device rd;
    return ((static_cast<uint64_t>(rd()) << 32) ^ rd()) | 1u;
}

// Nome visível: sem caracteres de controle, cortado numa borda de UTF-8.
std::string cleanName(const std::string& in) {
    std::string out;
    for (char c : in)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) out += c;
    if (out.size() > kNetMaxNameBytes) {
        size_t cut = kNetMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out.empty() ? "jogador" : out;
}

// "localhost", "ip", "ip:porta", "[v6]:porta", "v6" -> endereço com porta.
bool resolveDestination(const std::string& text, std::string& out) {
    std::string t = text == "localhost" ? "127.0.0.1" : text;
    NetAddress a;
    if (!NetAddress::parse(t, a)) return false;
    if (a.port == 0) a.port = kNetDefaultPort;
    out = a.toString();
    return true;
}

std::vector<uint8_t> finishMessage(BitWriter& w, std::vector<uint8_t>& buffer) {
    const size_t n = w.finish();
    buffer.resize(w.ok() ? n : 0);
    return buffer;
}

std::vector<uint8_t> eventMessage(const std::string& name, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buffer(kNetMaxEventNameBytes + data.size() + 8);
    BitWriter w(buffer.data(), buffer.size());
    w.writeString(name, kNetMaxEventNameBytes);
    w.alignToByte();
    w.writeBytes(data.data(), data.size());
    return finishMessage(w, buffer);
}

bool parseEvent(const std::vector<uint8_t>& msg, std::string& name, std::vector<uint8_t>& data) {
    BitReader r(msg.data(), msg.size());
    if (!r.readString(name, kNetMaxEventNameBytes) || name.empty()) return false;
    r.alignToByte();
    data.resize(r.bitsRemaining() / 8);
    return r.readBytes(data.data(), data.size()) && r.ok();
}

std::vector<uint8_t> reasonMessage(SystemMessage type, int player, const char* reason) {
    std::vector<uint8_t> buffer(64);
    BitWriter w(buffer.data(), buffer.size());
    w.writeInt(type, 0, kSystemTypes);
    if (type == kLeft) w.writeInt(player, 0, kNetMaxPlayers);
    w.writeString(reason, kMaxReasonBytes);
    return finishMessage(w, buffer);
}

std::vector<uint8_t> joinedMessage(int player, const std::string& name) {
    std::vector<uint8_t> buffer(64);
    BitWriter w(buffer.data(), buffer.size());
    w.writeInt(kJoined, 0, kSystemTypes);
    w.writeInt(player, 0, kNetMaxPlayers);
    w.writeString(name, kNetMaxNameBytes);
    return finishMessage(w, buffer);
}

// Mesma faixa da escrita (0..máx, para caber o 0 = servidor); jogador é >= 1.
int readPlayerId(BitReader& r) {
    const int id = r.readInt(0, kNetMaxPlayers);
    if (id < 1) r.fail();
    return id;
}

const char* failReasonFor(NetClient::State state, bool wasConnected) {
    switch (state) {
        case NetClient::State::TokenExpired: return "token_vencido";
        case NetClient::State::InvalidToken: return "token_invalido";
        case NetClient::State::TimedOut: return "caiu";
        case NetClient::State::ResponseTimedOut:
        case NetClient::State::RequestTimedOut: return "sem_resposta";
        case NetClient::State::Denied: return "cheio";
        default: return wasConnected ? "servidor_saiu" : "sem_resposta";
    }
}

} // namespace

// ---------------------------------------------------------------- servidor

struct NetSession::Server {
    struct Slot {
        std::unique_ptr<Connection> conn;
        bool hello = false;
        std::string name;
        double since = 0.0;
        double rejectAt = -1.0;
        const char* leaveReason = "saiu";
        double eventTokens = kEventBurst;
        double eventLast = 0.0;
    };

    NetSession& owner;
    std::unique_ptr<UdpLink> udp;
    std::unique_ptr<RelayLink> relay; // usa udp (ou o link dado); vem antes do mux
    MemoryHub hub;
    std::unique_ptr<PacketLink> loopServer, loopClient;
    std::unique_ptr<MuxLink> mux;
    std::unique_ptr<TokenGate> gate;
    NetServer server;
    std::array<Slot, kNetMaxPlayers> slots;
    NetKey key{};
    std::string identity;
    ContentHash content{};
    bool dedicated = false;
    double time = 0.0;
    uint64_t kicks = 0;
    const char* lastKick = "";

    explicit Server(NetSession& o) : owner(o) {}
    // O netcode avisa cada desconexão ao parar; aqui os slots já estariam
    // destruídos (vêm depois na ordem dos membros).
    ~Server() {
        server.onConnection = nullptr;
        server.stop();
    }

    int playerId(int slot) const { return slot + 1; }

    bool issueToken(const std::string& publicAddress, std::vector<uint8_t>& token) {
        ConnectTokenRequest req;
        req.publicAddresses = {publicAddress};
        req.internalAddresses = {identity};
        req.expireSeconds = kTokenExpire;
        req.timeoutSeconds = kTokenTimeout;
        req.clientId = randomId();
        req.protocolId = kProtocolId;
        return makeConnectToken(req, key, token);
    }

    bool start(PacketLink& net, const NetHostOptions& options, uint16_t port, double now) {
        dedicated = options.dedicated;
        content = options.content;
        time = now;
        key = makeNetKey();
        // Só um rótulo dentro do token (o netcode confere que é o mesmo dos
        // dois lados); pelo relay a porta real só vem depois, então 1.
        identity = (options.ipv6 ? "[::]:" : "0.0.0.0:") + std::to_string(port ? port : 1);
        NetAddress loopS, loopC;
        NetAddress::parse(kLoopServer, loopS);
        NetAddress::parse(kLoopClient, loopC);
        if (!dedicated) {
            loopServer = hub.createLink(loopS);
            loopClient = hub.createLink(loopC);
        }
        mux = std::make_unique<MuxLink>(net, loopServer.get());
        gate = std::make_unique<TokenGate>(*mux, port, kProtocolId, [this](const std::string& dialed, std::vector<uint8_t>& token) {
            return issueToken(dialed, token);
        });
        server.onConnection = [this](int slot, bool connected) { onConnection(slot, connected); };
        const int maxClients = std::clamp(options.maxPlayers, 1, kNetMaxPlayers);
        return server.start(identity, kProtocolId, key, maxClients, *gate, now);
    }

    void onConnection(int slot, bool connected) {
        if (slot < 0 || slot >= kNetMaxPlayers) return;
        Slot& s = slots[static_cast<size_t>(slot)];
        if (connected) {
            s = Slot();
            s.conn = std::make_unique<Connection>();
            s.since = time;
            s.eventLast = time;
            s.conn->init([this, slot](const uint8_t* d, int n) { server.send(slot, d, n); }, time);
            return;
        }
        if (s.hello) {
            broadcast(kSystemChannel, reasonMessage(kLeft, playerId(slot), s.leaveReason), slot);
            if (dedicated) {
                NetEvent e;
                e.type = NetEvent::Type::Left;
                e.player = playerId(slot);
                e.reason = s.leaveReason;
                owner.m_events.push_back(std::move(e));
            }
        }
        s = Slot();
    }

    void broadcast(int channel, const std::vector<uint8_t>& msg, int except = -1) {
        if (msg.empty()) return;
        for (int i = 0; i < kNetMaxPlayers; ++i) {
            Slot& s = slots[static_cast<size_t>(i)];
            if (i != except && s.hello && s.rejectAt < 0.0 && s.conn)
                s.conn->sendReliable(channel, msg.data(), static_cast<int>(msg.size()));
        }
    }

    // Manda o motivo e derruba quando a recusa for confirmada (ou em 1 s).
    void kick(int slot, const char* reason) {
        Slot& s = slots[static_cast<size_t>(slot)];
        if (s.rejectAt >= 0.0) return;
        ++kicks;
        lastKick = reason;
        ERUPTION_LOG_WARN("net: jogador %d derrubado (%s)", playerId(slot), reason);
        s.leaveReason = reason;
        s.rejectAt = time;
        if (s.conn && !s.conn->failed()) {
            const std::vector<uint8_t> msg = reasonMessage(kReject, 0, reason);
            s.conn->sendReliable(kSystemChannel, msg.data(), static_cast<int>(msg.size()));
        }
    }

    void welcome(int slot) {
        Slot& s = slots[static_cast<size_t>(slot)];
        std::vector<uint8_t> buffer(64 + kNetMaxPlayers * (kNetMaxNameBytes + 4));
        BitWriter w(buffer.data(), buffer.size());
        w.writeInt(kWelcome, 0, kSystemTypes);
        w.writeInt(playerId(slot), 0, kNetMaxPlayers);
        int count = 0;
        for (const Slot& o : slots) count += o.hello && o.rejectAt < 0.0 ? 1 : 0;
        w.writeInt(count, 0, kNetMaxPlayers);
        for (int i = 0; i < kNetMaxPlayers; ++i) {
            const Slot& o = slots[static_cast<size_t>(i)];
            if (!o.hello || o.rejectAt >= 0.0) continue;
            w.writeInt(playerId(i), 0, kNetMaxPlayers);
            w.writeString(o.name, kNetMaxNameBytes);
        }
        const std::vector<uint8_t> msg = finishMessage(w, buffer);
        s.conn->sendReliable(kSystemChannel, msg.data(), static_cast<int>(msg.size()));
    }

    void handleSystem(int slot, const std::vector<uint8_t>& msg) {
        Slot& s = slots[static_cast<size_t>(slot)];
        BitReader r(msg.data(), msg.size());
        const int32_t type = r.readInt(0, kSystemTypes);
        if (s.hello || type != kHello) {
            kick(slot, "mensagem_invalida");
            return;
        }
        ContentHash hash{};
        r.readBytes(hash.data(), hash.size());
        std::string name;
        r.readString(name, kNetMaxNameBytes);
        if (!r.ok()) {
            kick(slot, "mensagem_invalida");
            return;
        }
        if (hash != content) {
            kick(slot, "conteudo_diferente");
            return;
        }
        s.name = cleanName(name);
        s.hello = true;
        welcome(slot); // a lista já inclui o próprio jogador
        broadcast(kSystemChannel, joinedMessage(playerId(slot), s.name), slot);
        if (dedicated) {
            NetEvent e;
            e.type = NetEvent::Type::Joined;
            e.player = playerId(slot);
            e.name = s.name;
            owner.m_events.push_back(std::move(e));
        }
    }

    void handleEvent(int slot, const std::vector<uint8_t>& msg) {
        Slot& s = slots[static_cast<size_t>(slot)];
        NetEvent e;
        if (!s.hello || !parseEvent(msg, e.name, e.data)) {
            kick(slot, "mensagem_invalida");
            return;
        }
        auto it = owner.m_fromClient.find(e.name);
        if (it == owner.m_fromClient.end() || e.data.size() > it->second) {
            kick(slot, "mensagem_invalida");
            return;
        }
        s.eventTokens = std::min(kEventBurst, s.eventTokens + (time - s.eventLast) * kEventRate);
        s.eventLast = time;
        if (s.eventTokens < 1.0) {
            kick(slot, "excesso");
            return;
        }
        s.eventTokens -= 1.0;
        e.type = NetEvent::Type::Message;
        e.player = playerId(slot);
        owner.m_events.push_back(std::move(e));
    }

    void update(double now) {
        time = now;
        if (relay && relay->ready() && gate->port() != relay->roomPort()) gate->setPort(relay->roomPort());
        server.update(now);
        std::vector<uint8_t> packet;
        for (int i = 0; i < server.maxClients(); ++i) {
            Slot& s = slots[static_cast<size_t>(i)];
            if (!server.connected(i) || !s.conn) continue;
            while (server.receive(i, packet)) s.conn->receivePacket(packet.data(), static_cast<int>(packet.size()));
            int channel = 0;
            std::vector<uint8_t> msg;
            while (s.rejectAt < 0.0 && s.conn->receiveReliable(channel, msg))
                channel == kSystemChannel ? handleSystem(i, msg) : handleEvent(i, msg);
            while (s.conn->receiveUnreliable(msg)) {
                // Entrada e estado sem garantia chegam no item 4 (replicação).
            }
            if (s.conn->failed()) kick(i, s.conn->failReason());
            if (!s.hello && s.rejectAt < 0.0 && now - s.since > kHelloTimeout) kick(i, "sem_hello");
            s.conn->update(now);
            if (s.rejectAt >= 0.0 && (s.conn->pendingReliable() == 0 || s.conn->failed() || now - s.rejectAt > kRejectLinger))
                server.disconnect(i);
        }
    }
};

// ---------------------------------------------------------------- cliente

struct NetSession::Client {
    enum class Phase { Token, Connecting, Hello, Ready, Done };

    NetSession& owner;
    std::unique_ptr<UdpLink> udp;
    std::unique_ptr<PacketLink> owned; // transporte próprio do cliente (ex.: WebSocketLink do navegador)
    PacketLink* link = nullptr;
    TokenRequest tokenRequest;
    NetClient client;
    Connection conn;
    Phase phase = Phase::Token;
    std::string name;
    ContentHash content{};
    std::string localAddress;
    int myId = 0;
    std::map<int, std::string> players;
    std::string rejectReason;

    explicit Client(NetSession& o) : owner(o) {}

    void fail(const std::string& reason) {
        NetEvent e;
        if (phase == Phase::Ready) {
            e.type = NetEvent::Type::Left;
            e.player = myId;
        } else {
            e.type = NetEvent::Type::Failed;
        }
        e.reason = reason;
        owner.m_events.push_back(std::move(e));
        phase = Phase::Done;
        conn.reset();
        client.disconnect();
        players.clear();
    }

    void sendHello() {
        std::vector<uint8_t> buffer(64 + content.size());
        BitWriter w(buffer.data(), buffer.size());
        w.writeInt(kHello, 0, kSystemTypes);
        w.writeBytes(content.data(), content.size());
        w.writeString(name, kNetMaxNameBytes);
        const std::vector<uint8_t> msg = finishMessage(w, buffer);
        conn.sendReliable(kSystemChannel, msg.data(), static_cast<int>(msg.size()));
    }

    void pushJoined(int id, const std::string& who) {
        NetEvent e;
        e.type = NetEvent::Type::Joined;
        e.player = id;
        e.name = who;
        owner.m_events.push_back(std::move(e));
    }

    // Falso: mensagem malformada do servidor (o cliente sai).
    bool handleSystem(const std::vector<uint8_t>& msg) {
        BitReader r(msg.data(), msg.size());
        const int32_t type = r.readInt(0, kSystemTypes);
        if (type == kReject) {
            std::string reason;
            if (!r.readString(reason, kMaxReasonBytes)) return false;
            rejectReason = reason;
            return true;
        }
        if (type == kWelcome && phase == Phase::Hello) {
            const int id = readPlayerId(r);
            const int count = r.readInt(0, kNetMaxPlayers);
            std::map<int, std::string> list;
            for (int i = 0; i < count && r.ok(); ++i) {
                const int pid = readPlayerId(r);
                std::string who;
                r.readString(who, kNetMaxNameBytes);
                list[pid] = who;
            }
            if (!r.ok()) return false;
            myId = id;
            players = std::move(list);
            phase = Phase::Ready;
            for (const auto& p : players) pushJoined(p.first, p.second);
            return true;
        }
        if (phase != Phase::Ready) return false;
        if (type == kJoined) {
            const int pid = readPlayerId(r);
            std::string who;
            r.readString(who, kNetMaxNameBytes);
            if (!r.ok()) return false;
            players[pid] = who;
            pushJoined(pid, who);
            return true;
        }
        if (type == kLeft) {
            const int pid = readPlayerId(r);
            std::string reason;
            r.readString(reason, kMaxReasonBytes);
            if (!r.ok()) return false;
            players.erase(pid);
            NetEvent e;
            e.type = NetEvent::Type::Left;
            e.player = pid;
            e.reason = reason;
            owner.m_events.push_back(std::move(e));
            return true;
        }
        return false;
    }

    void update(double now) {
        if (phase == Phase::Done) return;
        if (phase == Phase::Token) {
            tokenRequest.update(now);
            if (tokenRequest.state() == TokenRequest::State::Failed) {
                fail("sem_resposta");
                return;
            }
            if (tokenRequest.state() != TokenRequest::State::Done) return;
            if (!client.connect(tokenRequest.token(), localAddress, *link, now)) {
                fail("token_invalido");
                return;
            }
            phase = Phase::Connecting;
        }
        client.update(now);
        const NetClient::State state = client.state();
        if (state == NetClient::State::Connected && phase == Phase::Connecting) {
            conn.init([this](const uint8_t* d, int n) { client.send(d, n); }, now);
            sendHello();
            phase = Phase::Hello;
        }
        if (state != NetClient::State::Connected && state != NetClient::State::SendingRequest &&
            state != NetClient::State::SendingResponse) {
            fail(!rejectReason.empty() ? rejectReason : failReasonFor(state, phase != Phase::Connecting));
            return;
        }
        if (phase < Phase::Hello) return;
        std::vector<uint8_t> packet;
        while (client.receive(packet)) conn.receivePacket(packet.data(), static_cast<int>(packet.size()));
        int channel = 0;
        std::vector<uint8_t> msg;
        while (conn.receiveReliable(channel, msg)) {
            if (channel == kSystemChannel) {
                if (!handleSystem(msg)) {
                    fail("servidor_invalido");
                    return;
                }
                if (!rejectReason.empty()) {
                    fail(rejectReason);
                    return;
                }
                continue;
            }
            NetEvent e;
            if (phase != Phase::Ready || !parseEvent(msg, e.name, e.data)) {
                fail("servidor_invalido");
                return;
            }
            e.type = NetEvent::Type::Message;
            e.player = 0;
            owner.m_events.push_back(std::move(e));
        }
        while (conn.receiveUnreliable(msg)) {
        }
        if (conn.failed()) {
            fail(conn.failReason());
            return;
        }
        conn.update(now);
    }
};

// ---------------------------------------------------------------- sessão

NetSession::NetSession() = default;
NetSession::~NetSession() { leave(); }

bool NetSession::host(const NetHostOptions& options, double time) {
    leave();
    auto udp = std::make_unique<UdpLink>();
    NetAddress bind;
    NetAddress::parse(options.ipv6 ? "[::]:0" : "0.0.0.0:0", bind);
    bind.port = options.port;
    if (!udp->open(bind)) {
        ERUPTION_LOG_ERROR("net: porta %u não abriu", static_cast<unsigned>(options.port));
        return false;
    }
    NetHostOptions real = options;
    real.port = udp->localAddress().port;
    UdpLink& link = *udp;
    if (!hostOn(link, real, time)) return false;
    m_server->udp = std::move(udp);
    ERUPTION_LOG_INFO("net: partida aberta na porta %u (%s)", static_cast<unsigned>(m_port),
                      options.dedicated ? "dedicado" : "host");
    return true;
}

bool NetSession::hostOn(PacketLink& link, const NetHostOptions& options, double time) {
    leave();
    m_server = std::make_unique<Server>(*this);
    if (!m_server->start(link, options, options.port, time)) {
        m_server.reset();
        return false;
    }
    m_port = options.port;
    if (options.dedicated) return true;
    // Jogador local do host: token direto, link em memória.
    std::vector<uint8_t> token;
    if (!m_server->issueToken(kLoopServer, token)) {
        leave();
        return false;
    }
    m_client = std::make_unique<Client>(*this);
    m_client->link = m_server->loopClient.get();
    m_client->name = cleanName(options.playerName);
    m_client->content = options.content;
    m_client->localAddress = kLoopClient;
    if (!m_client->client.connect(token, kLoopClient, *m_client->link, time)) {
        leave();
        return false;
    }
    m_client->phase = Client::Phase::Connecting;
    return true;
}

bool NetSession::hostRelay(const std::string& relay, const RelayKey& key, const NetHostOptions& options, double time) {
    leave();
    NetAddress relayAddr;
    if (!NetAddress::parse(relay, relayAddr)) return false;
    if (relayAddr.port == 0) relayAddr.port = kRelayDefaultPort;
    auto udp = std::make_unique<UdpLink>();
    NetAddress bind;
    NetAddress::parse(relayAddr.type == NetAddress::IPv6 ? "[::]:0" : "0.0.0.0:0", bind);
    if (!udp->open(bind)) return false;
    UdpLink& link = *udp;
    if (!hostRelayOn(link, relayAddr.toString(), key, options, time)) return false;
    m_server->udp = std::move(udp);
    ERUPTION_LOG_INFO("net: abrindo sala no relay %s", relayAddr.toString().c_str());
    return true;
}

bool NetSession::hostRelayOn(PacketLink& toRelay, const std::string& relay, const RelayKey& key,
                             const NetHostOptions& options, double time) {
    leave();
    NetAddress relayAddr;
    if (!NetAddress::parse(relay, relayAddr) || relayAddr.port == 0) return false;
    auto relayLink = std::make_unique<RelayLink>(toRelay, relayAddr, key);
    RelayLink& link = *relayLink;
    NetHostOptions o = options;
    o.port = 0; // a porta do token vira a da sala quando o relay responder
    if (!hostOn(link, o, time)) return false;
    m_server->relay = std::move(relayLink);
    return true;
}

std::string NetSession::publicAddress() const {
    return m_server && m_server->relay ? m_server->relay->publicAddress() : std::string();
}

bool NetSession::join(const std::string& destination, const std::string& playerName, const ContentHash& content,
                      double time) {
    leave();
    // Transporte web (spec 26): o navegador não abre UDP, então no wasm QUALQUER
    // destino entra pelo fluxo ws (a ponte/relay repassa para o host); no
    // desktop, só o esquema "ws://"/"wss://" explícito — o resto é UDP direto.
#ifdef __EMSCRIPTEN__
    const bool web = true;
#else
    const bool web = destination.rfind("ws://", 0) == 0 || destination.rfind("wss://", 0) == 0;
#endif
    if (web) return joinWeb(destination, playerName, content, time);
    std::string dest;
    NetAddress destAddr;
    if (!resolveDestination(destination, dest) || !NetAddress::parse(dest, destAddr)) {
        NetEvent e;
        e.type = NetEvent::Type::Failed;
        e.reason = "endereco_invalido";
        m_events.push_back(std::move(e));
        return false;
    }
    auto udp = std::make_unique<UdpLink>();
    NetAddress bind;
    NetAddress::parse(destAddr.type == NetAddress::IPv6 ? "[::]:0" : "0.0.0.0:0", bind);
    if (!udp->open(bind)) return false;
    UdpLink& link = *udp;
    if (!joinOn(link, dest, playerName, content, time)) return false;
    m_client->udp = std::move(udp);
    m_client->localAddress = m_client->udp->localAddress().toString();
    return true;
}

bool NetSession::joinOn(PacketLink& link, const std::string& destination, const std::string& playerName,
                        const ContentHash& content, double time) {
    leave();
    std::string dest;
    if (!resolveDestination(destination, dest)) return false;
    m_client = std::make_unique<Client>(*this);
    m_client->link = &link;
    m_client->name = cleanName(playerName);
    m_client->content = content;
    NetAddress d;
    NetAddress::parse(dest, d);
    m_client->localAddress = d.type == NetAddress::IPv6 ? "[::]:0" : "0.0.0.0:0";
    if (!m_client->tokenRequest.start(dest, link, time)) {
        m_client.reset();
        return false;
    }
    return true;
}

bool NetSession::joinWeb(const std::string& destination, const std::string& playerName, const ContentHash& content,
                         double time) {
    leave();
    auto failWith = [this](const char* reason) {
        NetEvent e;
        e.type = NetEvent::Type::Failed;
        e.reason = reason;
        m_events.push_back(std::move(e));
        return false;
    };
    // Tira o esquema e um eventual caminho/consulta, ficando com "host[:porta]".
    std::string d = destination;
    bool explicitScheme = false;
    if (d.rfind("ws://", 0) == 0) {
        d = d.substr(5);
        explicitScheme = true;
    } else if (d.rfind("wss://", 0) == 0) {
        d = d.substr(6);
        explicitScheme = true;
    }
    const size_t slash = d.find('/');
    if (slash != std::string::npos) d = d.substr(0, slash);
    NetAddress a;
    if (!NetAddress::parse(d, a)) return failWith("endereco_invalido");
    // Dois endereços a partir de um só destino:
    //  - transporte (ws): com "ws://ip:P", a porta P é a do ws; sem esquema, a
    //    porta sempre é a da ponte (kWebBridgePort), e a porta digitada é a do
    //    JOGO.
    //  - jogo: a porta do host, que entra no token e que o WebSocketLink
    //    reporta como par — o TokenGate confere a PORTA discada contra a do
    //    host, então ela tem de ser a de jogo (kNetDefaultPort por padrão).
    NetAddress wsAddr = a;
    NetAddress gameAddr = a;
    if (explicitScheme) {
        wsAddr.port = a.port ? a.port : kWebBridgePort;
        gameAddr.port = kNetDefaultPort;
    } else {
        wsAddr.port = kWebBridgePort;
        gameAddr.port = a.port ? a.port : kNetDefaultPort;
    }
    const std::string url = "ws://" + wsAddr.toString();
    const std::string dialed = gameAddr.toString();
    std::unique_ptr<StreamBackend> backend = makeWsClient(url);
    if (!backend) return failWith("transporte_web_indisponivel");
    m_client = std::make_unique<Client>(*this);
    m_client->owned = std::make_unique<WebSocketLink>(std::move(backend), gameAddr);
    m_client->link = m_client->owned.get();
    m_client->name = cleanName(playerName);
    m_client->content = content;
    m_client->localAddress = gameAddr.type == NetAddress::IPv6 ? "[::]:0" : "0.0.0.0:0";
    if (!m_client->tokenRequest.start(dialed, *m_client->link, time)) {
        m_client.reset();
        return false;
    }
    ERUPTION_LOG_INFO("net: entrando pelo navegador em %s (jogo %s)", url.c_str(), dialed.c_str());
    return true;
}

void NetSession::leave() {
    m_client.reset(); // antes do servidor: o cliente local usa o link em memória dele
    m_server.reset();
    m_port = 0;
}

void NetSession::update(double time) {
    if (m_server) m_server->update(time);
    if (m_client) m_client->update(time);
}

NetSession::Role NetSession::role() const {
    if (m_server) return m_server->dedicated ? Role::Server : Role::Hosting;
    if (!m_client) return Role::Offline;
    switch (m_client->phase) {
        case Client::Phase::Ready: return Role::Connected;
        case Client::Phase::Done: return Role::Offline;
        default: return Role::Connecting;
    }
}

int NetSession::myId() const {
    if (m_client && m_client->phase == Client::Phase::Ready) return m_client->myId;
    return 0;
}

std::vector<NetPlayer> NetSession::players() const {
    std::vector<NetPlayer> out;
    if (m_server) {
        for (int i = 0; i < kNetMaxPlayers; ++i) {
            const Server::Slot& s = m_server->slots[static_cast<size_t>(i)];
            if (!s.hello || s.rejectAt >= 0.0) continue;
            NetPlayer p;
            p.id = m_server->playerId(i);
            p.name = s.name;
            p.pingMs = s.conn ? s.conn->stats().rttMs : 0.0f;
            out.push_back(std::move(p));
        }
        return out;
    }
    if (m_client && m_client->phase == Client::Phase::Ready) {
        for (const auto& kv : m_client->players) {
            NetPlayer p;
            p.id = kv.first;
            p.name = kv.second;
            if (kv.first == m_client->myId) p.pingMs = m_client->conn.stats().rttMs;
            out.push_back(std::move(p));
        }
    }
    return out;
}

void NetSession::allowFromClient(const std::string& name, size_t maxBytes) {
    if (name.empty() || name.size() > kNetMaxEventNameBytes) return;
    m_fromClient[name] = std::min(maxBytes, kNetMaxEventBytes);
}

bool NetSession::send(const std::string& name, const std::vector<uint8_t>& data) {
    if (!m_server || name.empty() || name.size() > kNetMaxEventNameBytes || data.size() > kNetMaxEventBytes) return false;
    const std::vector<uint8_t> msg = eventMessage(name, data);
    if (msg.empty()) return false;
    m_server->broadcast(kEventChannel, msg);
    return true;
}

bool NetSession::sendTo(int player, const std::string& name, const std::vector<uint8_t>& data) {
    if (!m_server || player < 1 || player > kNetMaxPlayers || name.empty() || name.size() > kNetMaxEventNameBytes ||
        data.size() > kNetMaxEventBytes)
        return false;
    Server::Slot& s = m_server->slots[static_cast<size_t>(player - 1)];
    if (!s.hello || s.rejectAt >= 0.0 || !s.conn) return false;
    const std::vector<uint8_t> msg = eventMessage(name, data);
    return !msg.empty() && s.conn->sendReliable(kEventChannel, msg.data(), static_cast<int>(msg.size()));
}

bool NetSession::ask(const std::string& name, const std::vector<uint8_t>& data) {
    if (!m_client || m_client->phase != Client::Phase::Ready || name.empty() || name.size() > kNetMaxEventNameBytes ||
        data.size() > kNetMaxEventBytes)
        return false;
    const std::vector<uint8_t> msg = eventMessage(name, data);
    return !msg.empty() && m_client->conn.sendReliable(kEventChannel, msg.data(), static_cast<int>(msg.size()));
}

bool NetSession::poll(NetEvent& event) {
    if (m_events.empty()) return false;
    event = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

NetSessionStats NetSession::stats() const {
    NetSessionStats s;
    if (m_server) {
        for (const Server::Slot& slot : m_server->slots) {
            if (!slot.conn) continue;
            const ConnectionStats c = slot.conn->stats();
            if (c.rttMs >= s.link.rttMs) s.link = c;
            s.players += slot.hello ? 1 : 0;
        }
        s.tokensIssued = m_server->gate ? m_server->gate->issued() : 0;
        s.tokensRefused = m_server->gate ? m_server->gate->refused() : 0;
        s.requestsDropped = m_server->gate ? m_server->gate->droppedRequests() : 0;
        s.floodDropped = m_server->gate ? m_server->gate->droppedFlood() : 0;
        s.requestsAdmitted = m_server->gate ? m_server->gate->admittedRequests() : 0;
        s.kicks = m_server->kicks;
        s.lastKick = m_server->lastKick;
    } else if (m_client) {
        s.link = m_client->conn.stats();
        s.players = static_cast<int>(m_client->players.size());
    }
    return s;
}

const char* toString(NetSession::Role role) {
    switch (role) {
        case NetSession::Role::Offline: return "offline";
        case NetSession::Role::Connecting: return "connecting";
        case NetSession::Role::Connected: return "connected";
        case NetSession::Role::Hosting: return "hosting";
        case NetSession::Role::Server: return "server";
    }
    return "offline";
}

} // namespace eruption
