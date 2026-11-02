#include "net/Net.hpp"

#include "net/NetSession.hpp"
#include "net/Relay.hpp"

#include <monocypher.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace eruption {

namespace {

ContentHash hashContent(const std::vector<std::string>& parts) {
    // Cada pedaço com o tamanho antes: ("ab","c") e ("a","bc") dão hashes diferentes.
    Blake2b h(32);
    for (const std::string& p : parts) {
        const uint64_t n = p.size();
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(n >> (8 * i));
        h.update(len, 8);
        h.update(p.data(), p.size());
    }
    ContentHash out{};
    h.final(out.data());
    return out;
}

} // namespace

struct NetSystem::Impl {
    NetSession session;
    double time = 0.0;
    bool relayed = false;
};

NetSystem& NetSystem::instance() {
    static NetSystem s;
    return s;
}

NetSystem::NetSystem() : m(std::make_unique<Impl>()) {}
NetSystem::~NetSystem() = default;

bool NetSystem::available() const { return true; }

bool NetSystem::host(int maxPlayers, int port, bool dedicated, const std::string& playerName,
                     const std::vector<std::string>& content, const std::string& relay, const std::string& relayKey,
                     std::string& error) {
#ifdef __EMSCRIPTEN__
    // O navegador não abre UDP nem aceita conexão de entrada: não há como
    // hospedar. O jogo recebe nil + motivo e segue (spec 26, só entra por ws).
    (void)maxPlayers;
    (void)port;
    (void)dedicated;
    (void)playerName;
    (void)content;
    (void)relay;
    (void)relayKey;
    error = "sem_udp_no_navegador";
    return false;
#else
    if (port < 0 || port > 65535) {
        error = "porta_invalida";
        return false;
    }
    NetHostOptions o;
    o.maxPlayers = maxPlayers;
    o.port = static_cast<uint16_t>(port);
    o.dedicated = dedicated;
    o.playerName = playerName;
    o.content = hashContent(content);
    m->relayed = !relay.empty();
    if (m->relayed) {
        RelayKey key{};
        if (!parseRelayKey(relayKey, key)) {
            error = "chave_do_relay_invalida";
            return false;
        }
        const bool ok = m->session.hostRelay(relay, key, o, m->time);
        crypto_wipe(key.data(), key.size());
        if (!ok) {
            error = "relay_invalido";
            return false;
        }
        return true;
    }
    if (!m->session.host(o, m->time)) {
        error = "porta_ocupada";
        return false;
    }
    return true;
#endif
}

bool NetSystem::join(const std::string& destination, const std::string& playerName,
                     const std::vector<std::string>& content) {
    return m->session.join(destination, playerName, hashContent(content), m->time);
}

void NetSystem::leave() { m->session.leave(); }

void NetSystem::update(double dt) {
    if (dt > 0.0) m->time += dt;
    m->session.update(m->time);
}

const char* NetSystem::state() const { return toString(m->session.role()); }
bool NetSystem::isServer() const { return m->session.isServer(); }
int NetSystem::myId() const { return m->session.myId(); }
int NetSystem::port() const { return m->session.port(); }

std::string NetSystem::address() const {
    if (!m->session.isServer()) return {};
    if (m->relayed) return m->session.publicAddress();
    std::string ip = "127.0.0.1";
#ifndef _WIN32
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK) ||
                !(a->ifa_flags & IFF_UP))
                continue;
            char text[INET_ADDRSTRLEN] = {};
            const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
            if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) {
                ip = text;
                break;
            }
        }
        freeifaddrs(list);
    }
#endif
    return ip + ":" + std::to_string(m->session.port());
}

std::vector<NetPlayerInfo> NetSystem::players() const {
    std::vector<NetPlayerInfo> out;
    for (const NetPlayer& p : m->session.players()) out.push_back({p.id, p.name, p.pingMs});
    return out;
}

void NetSystem::allowFromClient(const std::string& name, size_t maxBytes) {
    m->session.allowFromClient(name, maxBytes);
}

namespace {

bool encodeFor(const NetValue& value, std::vector<uint8_t>& out) {
    return encodeNetValue(value, out, kNetMaxEventBytes);
}

} // namespace

bool NetSystem::send(const std::string& name, const NetValue& value) {
    std::vector<uint8_t> data;
    return encodeFor(value, data) && m->session.send(name, data);
}

bool NetSystem::sendTo(int player, const std::string& name, const NetValue& value) {
    std::vector<uint8_t> data;
    return encodeFor(value, data) && m->session.sendTo(player, name, data);
}

bool NetSystem::ask(const std::string& name, const NetValue& value) {
    std::vector<uint8_t> data;
    return encodeFor(value, data) && m->session.ask(name, data);
}

bool NetSystem::poll(NetScriptEvent& event) {
    NetEvent e;
    while (m->session.poll(e)) {
        event = NetScriptEvent();
        event.player = e.player;
        event.name = e.name;
        event.reason = e.reason;
        switch (e.type) {
            case NetEvent::Type::Joined: event.kind = NetScriptEvent::Kind::Join; return true;
            case NetEvent::Type::Left: event.kind = NetScriptEvent::Kind::Leave; return true;
            case NetEvent::Type::Failed: event.kind = NetScriptEvent::Kind::Fail; return true;
            case NetEvent::Type::Message:
                event.kind = NetScriptEvent::Kind::Message;
                // O servidor já conferiu nome e tamanho; dados que não decodificam
                // vêm de um par com bug ou hostil: o evento some.
                if (decodeNetValue(e.data.data(), e.data.size(), event.value)) return true;
                break;
        }
    }
    return false;
}

NetStatsInfo NetSystem::stats() const {
    const NetSessionStats s = m->session.stats();
    NetStatsInfo out;
    out.pingMs = s.link.rttMs;
    out.jitterMs = s.link.jitterMs;
    out.lossPercent = s.link.lossPercent;
    out.kbpsIn = s.link.kbpsIn;
    out.kbpsOut = s.link.kbpsOut;
    out.players = s.players;
    return out;
}

} // namespace eruption
