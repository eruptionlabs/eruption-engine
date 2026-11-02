#include "net/Relay.hpp"

#include "core/Logger.hpp"

#include <monocypher.h>

#include <algorithm>
#include <cstring>

namespace eruption {

namespace {

constexpr uint8_t kMagic[4] = {'E', 'R', 'R', 'L'};
enum : uint8_t { kRegister = 1, kRegistered = 2, kData = 3, kClose = 4 };
constexpr size_t kTypeAt = 4;
constexpr size_t kNonceAt = 5;
constexpr size_t kMacAt = kNonceAt + 24;
constexpr size_t kBodyAt = kMacAt + 16;
constexpr size_t kSlotBytes = 2;
constexpr int kMaxPacket = 4096;
constexpr double kRoomTimeout = 10.0;
constexpr double kClientTimeout = 30.0;
constexpr double kStreamHandshake = 5.0; // conexão ws muda (sem o quadro de papel) cai
constexpr double kKeepAlive = 1.0;
constexpr double kClientBurst = 600.0;
constexpr double kClientRate = 300.0;
static_assert(kBodyAt + kSlotBytes == static_cast<size_t>(kRelayOverheadBytes), "layout do relay");

// Nonce = prefixo aleatório de 16 bytes da ponta (fixo na sessão) +
// contador de 64 bits; o contador vai no próprio nonce, autenticado.
void makeNonce(const uint8_t prefix[16], uint64_t counter, uint8_t nonce[24]) {
    std::memcpy(nonce, prefix, 16);
    for (int i = 0; i < 8; ++i) nonce[16 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

uint64_t nonceCounter(const uint8_t* nonce) {
    uint64_t c = 0;
    for (int i = 7; i >= 0; --i) c = (c << 8) | nonce[16 + i];
    return c;
}

int seal(uint8_t type, const uint8_t prefix[16], uint64_t counter, const RelayKey& key, uint16_t slot,
         const uint8_t* payload, int bytes, std::vector<uint8_t>& out) {
    const size_t body = kSlotBytes + static_cast<size_t>(bytes);
    out.resize(kBodyAt + body);
    std::memcpy(out.data(), kMagic, 4);
    out[kTypeAt] = type;
    makeNonce(prefix, counter, out.data() + kNonceAt);
    std::vector<uint8_t> plain(body);
    plain[0] = static_cast<uint8_t>(slot);
    plain[1] = static_cast<uint8_t>(slot >> 8);
    if (bytes > 0) std::memcpy(plain.data() + kSlotBytes, payload, static_cast<size_t>(bytes));
    crypto_aead_lock(out.data() + kBodyAt, out.data() + kMacAt, key.data(), out.data() + kNonceAt, out.data(), 5,
                     plain.data(), plain.size());
    return static_cast<int>(out.size());
}

// Abre no lugar; devolve o tamanho da carga (depois da vaga) ou -1.
int unseal(const uint8_t* data, int bytes, const RelayKey& key, uint8_t& type, uint64_t& counter, uint16_t& slot,
         std::vector<uint8_t>& plain) {
    if (bytes < static_cast<int>(kBodyAt + kSlotBytes) || bytes > kMaxPacket || std::memcmp(data, kMagic, 4) != 0)
        return -1;
    const size_t body = static_cast<size_t>(bytes) - kBodyAt;
    plain.resize(body);
    if (crypto_aead_unlock(plain.data(), data + kMacAt, key.data(), data + kNonceAt, data, 5, data + kBodyAt, body) != 0)
        return -1;
    type = data[kTypeAt];
    counter = nonceCounter(data + kNonceAt);
    slot = static_cast<uint16_t>(plain[0] | (plain[1] << 8));
    return static_cast<int>(body - kSlotBytes);
}

void randomBytes(uint8_t* out, size_t n) {
    const NetKey k = makeNetKey();
    std::memcpy(out, k.data(), std::min(n, k.size()));
}

bool samePrefix(const uint8_t* nonce, const uint8_t prefix[16]) { return std::memcmp(nonce, prefix, 16) == 0; }

} // namespace

// ---------------------------------------------------------------- utilidades

bool RelayReplayWindow::accept(uint64_t counter) {
    if (!seen || counter > last) {
        const uint64_t shift = seen ? counter - last : 64;
        bits = shift >= 64 ? 0 : bits << shift;
        bits |= 1;
        last = counter;
        seen = true;
        return true;
    }
    const uint64_t back = last - counter;
    if (back >= 64 || ((bits >> back) & 1u)) return false;
    bits |= uint64_t(1) << back;
    return true;
}

bool parseRelayKey(const std::string& hex, RelayKey& out) {
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        unsigned v = 0;
        for (size_t k = 0; k < 2; ++k) {
            const char c = hex[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

std::string relayKeyToHex(const RelayKey& key) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : key) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

NetAddress relayVirtualAddress(uint16_t slot) {
    NetAddress a;
    a.type = NetAddress::IPv6;
    a.port = 1;
    a.bytes[0] = 0x01;
    a.bytes[7] = 0x01;
    a.bytes[14] = static_cast<uint8_t>(slot >> 8);
    a.bytes[15] = static_cast<uint8_t>(slot);
    return a;
}

bool relaySlotOf(const NetAddress& a, uint16_t& slot) {
    static const uint8_t prefix[8] = {0x01, 0, 0, 0, 0, 0, 0, 0x01};
    if (a.type != NetAddress::IPv6 || a.port != 1 || std::memcmp(a.bytes, prefix, 8) != 0) return false;
    slot = static_cast<uint16_t>((a.bytes[14] << 8) | a.bytes[15]);
    return true;
}

// ---------------------------------------------------------------- relay

RelayServer::~RelayServer() { crypto_wipe(m_key.data(), m_key.size()); }

bool RelayServer::start(const Options& options, const RelayKey& key, LinkFactory factory, double time) {
    m_options = options;
    m_key = key;
    m_factory = std::move(factory);
    m_time = time;
    m_buffer.resize(kMaxPacket + 64);
    randomBytes(m_prefix.data(), m_prefix.size());
    m_control = m_factory(options.controlPort);
    if (!m_control) {
        ERUPTION_LOG_ERROR("relay: porta de controle %u não abriu", static_cast<unsigned>(options.controlPort));
        return false;
    }
    ERUPTION_LOG_INFO("relay: controle na porta %u, salas a partir de %u (até %d)",
                      static_cast<unsigned>(options.controlPort), static_cast<unsigned>(options.firstRoomPort),
                      options.maxRooms);
    return true;
}

void RelayServer::setStreamAcceptor(std::unique_ptr<StreamAcceptor> acceptor) { m_acceptor = std::move(acceptor); }

// Endereço virtual de uma conexão ws: IPv6 0100:...:02xx, porta 2. Fica numa
// faixa de descarte (RFC 6666), nunca roteada, distinta da vaga vista pelo
// host (porta 1). Só serve para o relay casar envios de volta à conexão certa.
NetAddress RelayServer::wsAddress() {
    NetAddress a;
    a.type = NetAddress::IPv6;
    a.port = 2;
    a.bytes[0] = 0x01;
    a.bytes[7] = 0x02;
    const uint32_t id = ++m_wsCounter;
    a.bytes[12] = static_cast<uint8_t>(id >> 24);
    a.bytes[13] = static_cast<uint8_t>(id >> 16);
    a.bytes[14] = static_cast<uint8_t>(id >> 8);
    a.bytes[15] = static_cast<uint8_t>(id);
    return a;
}

void RelayServer::pumpStreams(double time) {
    if (m_acceptor) {
        m_acceptor->update(time);
        while (std::unique_ptr<StreamBackend> backend = m_acceptor->accept())
            m_wsPending.emplace_back(std::make_unique<WebSocketLink>(std::move(backend), NetAddress{}), time);
    }
    uint8_t buf[kMaxPacket];
    // Pendentes: espera o quadro de papel (host, ou entrar numa sala).
    for (size_t i = 0; i < m_wsPending.size();) {
        WebSocketLink& link = *m_wsPending[i].first;
        link.update(time);
        NetAddress from;
        const int n = link.receive(from, buf, sizeof(buf));
        bool attached = false;
        if (n == 1 && buf[0] == kStreamHost) {
            Stream s;
            s.address = wsAddress();
            s.roomPort = 0;
            m_wsPending[i].first->setPeer(s.address);
            s.link = std::move(m_wsPending[i].first);
            m_ws.push_back(std::move(s));
            attached = true;
        } else if (n == 3 && buf[0] == kStreamJoin) {
            const uint16_t port = static_cast<uint16_t>(buf[1] | (buf[2] << 8));
            if (m_rooms.count(port)) {
                Stream s;
                s.address = wsAddress();
                s.roomPort = port;
                m_wsPending[i].first->setPeer(s.address);
                s.link = std::move(m_wsPending[i].first);
                m_ws.push_back(std::move(s));
                attached = true;
            }
        }
        // Virou conexão, ou o quadro de papel foi inválido/a sala não existe, ou
        // a conexão caiu, ou passou da janela sem dizer nada: sai dos pendentes.
        const bool drop = attached || n != 0 || !link.connected() || time - m_wsPending[i].second > kStreamHandshake;
        if (drop) m_wsPending.erase(m_wsPending.begin() + static_cast<long>(i));
        else ++i;
    }
    // Conexões com papel: entrega os quadros ao relay como se fossem UDP.
    for (size_t i = 0; i < m_ws.size();) {
        Stream& s = m_ws[i];
        s.link->update(time);
        if (!s.link->connected()) {
            m_ws.erase(m_ws.begin() + static_cast<long>(i));
            continue;
        }
        NetAddress from;
        int n;
        while ((n = s.link->receive(from, buf, sizeof(buf))) > 0) {
            if (s.roomPort == 0) {
                handleControl(s.address, buf, n);
            } else {
                auto it = m_rooms.find(s.roomPort);
                if (it != m_rooms.end()) handleRoomPacket(*it->second, s.address, buf, n);
            }
        }
        ++i;
    }
}

bool RelayServer::destinationAllowed(const NetAddress& a) const {
    if (m_options.allowPrivate) return a.type != NetAddress::None && a.port != 0;
    if (a.port == 0) return false;
    const uint8_t* b = a.bytes;
    if (a.type == NetAddress::IPv4) {
        if (b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224) return false;      // própria rede, privada, loopback, multicast/reservada
        if (b[0] == 169 && b[1] == 254) return false;                                  // link-local
        if (b[0] == 172 && (b[1] & 0xF0) == 16) return false;                          // 172.16/12
        if (b[0] == 192 && b[1] == 168) return false;                                  // 192.168/16
        if (b[0] == 100 && (b[1] & 0xC0) == 64) return false;                          // CGNAT 100.64/10
        return true;
    }
    if (a.type == NetAddress::IPv6) {
        static const uint8_t zero[16] = {};
        if (std::memcmp(b, zero, 15) == 0) return false;                              // :: e ::1
        if ((b[0] & 0xFE) == 0xFC || b[0] == 0xFF) return false;                       // fc00::/7, multicast
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false;                       // fe80::/10
        if (b[0] == 0x01 && std::memcmp(b + 1, zero, 7) == 0) return false;            // 100::/64 (descarte)
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(b, mapped, 12) == 0) {                                         // ::ffff:a.b.c.d
            NetAddress v4;
            v4.type = NetAddress::IPv4;
            v4.port = a.port;
            std::memcpy(v4.bytes, b + 12, 4);
            return destinationAllowed(v4);
        }
        return true;
    }
    return false;
}

uint16_t RelayServer::freeRoomPort() const {
    for (int i = 0; i < m_options.maxRooms; ++i) {
        const int port = m_options.firstRoomPort + i;
        if (port > 65535) break;
        if (!m_rooms.count(static_cast<uint16_t>(port))) return static_cast<uint16_t>(port);
    }
    return 0;
}

void RelayServer::sendToHost(Room& room, uint8_t type, uint16_t slot, const uint8_t* payload, int bytes) {
    std::vector<uint8_t> out;
    seal(type, m_prefix.data(), ++room.relayCounter, m_key, slot, payload, bytes, out);
    emit(*m_control, room.host, out.data(), static_cast<int>(out.size()));
}

void RelayServer::emit(PacketLink& base, const NetAddress& to, const uint8_t* data, int bytes) {
    for (Stream& s : m_ws)
        if (s.address == to) {
            s.link->send(to, data, bytes);
            return;
        }
    base.send(to, data, bytes);
}

void RelayServer::handleControl(const NetAddress& from, const uint8_t* data, int bytes) {
    uint8_t type = 0;
    uint64_t counter = 0;
    uint16_t slot = 0;
    std::vector<uint8_t> plain;
    const int n = unseal(data, bytes, m_key, type, counter, slot, plain);
    if (n < 0) {
        ++m_rejected; // sem chave: calado, nada volta
        return;
    }
    const uint8_t* nonce = data + kNonceAt;
    const uint8_t* payload = plain.data() + kSlotBytes;
    Room* room = nullptr;
    for (auto& kv : m_rooms)
        if (kv.second->host == from) room = kv.second.get();

    if (type == kRegister) {
        if (n != 8 || !destinationAllowed(from)) {
            ++m_rejected;
            return;
        }
        uint64_t session = 0;
        for (int i = 7; i >= 0; --i) session = (session << 8) | payload[i];
        if (room && room->session != session) { // o host reabriu: a sala velha fecha
            m_rooms.erase(room->port);
            room = nullptr;
        }
        if (!room) {
            const uint16_t port = freeRoomPort();
            std::unique_ptr<PacketLink> link = port ? m_factory(port) : nullptr;
            if (!link) {
                ++m_rejected;
                return;
            }
            auto r = std::make_unique<Room>();
            r->host = from;
            r->session = session;
            r->port = port;
            r->link = std::move(link);
            room = r.get();
            std::memcpy(room->hostPrefix.data(), nonce, 16);
            m_rooms.emplace(port, std::move(r));
            ERUPTION_LOG_INFO("relay: sala aberta na porta %u", static_cast<unsigned>(port));
        }
        if (!samePrefix(nonce, room->hostPrefix.data()) || !room->fromHost.accept(counter)) {
            ++m_rejected;
            return;
        }
        room->lastHost = m_time;
        const uint8_t port[2] = {static_cast<uint8_t>(room->port), static_cast<uint8_t>(room->port >> 8)};
        sendToHost(*room, kRegistered, 0, port, 2);
        return;
    }
    if (!room) {
        ++m_rejected;
        return;
    }
    if (!samePrefix(nonce, room->hostPrefix.data()) || !room->fromHost.accept(counter)) {
        ++m_rejected;
        return;
    }
    room->lastHost = m_time;
    if (type == kClose) {
        m_rooms.erase(room->port);
        return;
    }
    if (type != kData) {
        ++m_rejected;
        return;
    }
    // O host só alcança vagas que mandaram pacote para a sala.
    auto it = room->clients.find(slot);
    if (it == room->clients.end() || !destinationAllowed(it->second.address)) {
        ++m_refusedDestinations;
        return;
    }
    emit(*room->link, it->second.address, payload, n);
    ++m_forwarded;
}

void RelayServer::handleRoomPacket(Room& room, const NetAddress& from, const uint8_t* data, int bytes) {
    if (!destinationAllowed(from)) {
        ++m_refusedDestinations;
        return;
    }
    uint16_t slot = 0;
    Client* client = nullptr;
    for (auto& kv : room.clients)
        if (kv.second.address == from) {
            slot = kv.first;
            client = &kv.second;
        }
    if (!client) {
        for (auto it = room.clients.begin(); it != room.clients.end();)
            it = m_time - it->second.lastSeen > kClientTimeout ? room.clients.erase(it) : std::next(it);
        if (static_cast<int>(room.clients.size()) >= m_options.maxClientsPerRoom) {
            ++m_rejected;
            return;
        }
        uint16_t free = 1;
        while (room.clients.count(free)) ++free;
        slot = free;
        client = &room.clients[slot];
        client->address = from;
        client->tokens = kClientBurst;
        client->lastSeen = m_time;
    }
    client->tokens = std::min(kClientBurst, client->tokens + (m_time - client->lastSeen) * kClientRate);
    client->lastSeen = m_time;
    if (client->tokens < 1.0) {
        ++m_rejected;
        return;
    }
    client->tokens -= 1.0;
    sendToHost(room, kData, slot, data, bytes);
    ++m_forwarded;
}

void RelayServer::update(double time) {
    m_time = time;
    if (!m_control) return;
    m_control->update(time);
    NetAddress from;
    int n;
    while ((n = m_control->receive(from, m_buffer.data(), static_cast<int>(m_buffer.size()))) > 0)
        handleControl(from, m_buffer.data(), n);
    for (auto it = m_rooms.begin(); it != m_rooms.end();) {
        Room& room = *it->second;
        if (time - room.lastHost > kRoomTimeout) {
            ERUPTION_LOG_INFO("relay: sala da porta %u fechou (host sem sinal)", static_cast<unsigned>(room.port));
            it = m_rooms.erase(it);
            continue;
        }
        room.link->update(time);
        while ((n = room.link->receive(from, m_buffer.data(), static_cast<int>(m_buffer.size()))) > 0)
            handleRoomPacket(room, from, m_buffer.data(), n);
        ++it;
    }
    pumpStreams(time);
}

// ---------------------------------------------------------------- host

RelayLink::RelayLink(PacketLink& toRelay, const NetAddress& relayControl, const RelayKey& key)
    : m_relay(toRelay), m_control(relayControl), m_key(key), m_buffer(kMaxPacket + 64) {
    uint8_t s[8];
    randomBytes(s, 8);
    for (int i = 7; i >= 0; --i) m_session = (m_session << 8) | s[i];
    randomBytes(m_prefix.data(), m_prefix.size());
}

RelayLink::~RelayLink() {
    if (m_roomPort) sendControl(kClose, 0, nullptr, 0);
    crypto_wipe(m_key.data(), m_key.size());
}

void RelayLink::sendControl(uint8_t type, uint16_t slot, const uint8_t* payload, int bytes) {
    std::vector<uint8_t> out;
    seal(type, m_prefix.data(), ++m_counter, m_key, slot, payload, bytes, out);
    m_relay.send(m_control, out.data(), static_cast<int>(out.size()));
}

void RelayLink::send(const NetAddress& to, const uint8_t* data, int bytes) {
    uint16_t slot = 0;
    if (!relaySlotOf(to, slot) || bytes <= 0 || bytes > kMaxPacket - kRelayOverheadBytes) return;
    sendControl(kData, slot, data, bytes);
}

void RelayLink::update(double time) {
    m_time = time;
    m_relay.update(time);
    if (time - m_lastSend >= kKeepAlive) {
        m_lastSend = time;
        uint8_t session[8];
        for (int i = 0; i < 8; ++i) session[i] = static_cast<uint8_t>(m_session >> (8 * i));
        sendControl(kRegister, 0, session, 8);
    }
}

int RelayLink::receive(NetAddress& from, uint8_t* out, int maxBytes) {
    for (;;) {
        NetAddress src;
        const int n = m_relay.receive(src, m_buffer.data(), static_cast<int>(m_buffer.size()));
        if (n <= 0) return 0;
        if (src != m_control) continue; // só o relay fala por aqui
        uint8_t type = 0;
        uint64_t counter = 0;
        uint16_t slot = 0;
        std::vector<uint8_t> plain;
        const int len = unseal(m_buffer.data(), n, m_key, type, counter, slot, plain);
        if (len < 0 || !m_fromRelay.accept(counter)) continue;
        if (type == kRegistered && len == 2) {
            m_roomPort = static_cast<uint16_t>(plain[kSlotBytes] | (plain[kSlotBytes + 1] << 8));
            continue;
        }
        if (type != kData || len > maxBytes || slot == 0) continue;
        std::memcpy(out, plain.data() + kSlotBytes, static_cast<size_t>(len));
        from = relayVirtualAddress(slot);
        return len;
    }
}

std::string RelayLink::publicAddress() const {
    if (!m_roomPort) return {};
    NetAddress a = m_control;
    a.port = m_roomPort;
    return a.toString();
}

} // namespace eruption
