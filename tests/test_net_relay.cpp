// Testes do relay (src/net/Relay): host atrás de CGNAT abre sala numa VPS,
// clientes entram pelo endereço da sala e jogam; e o que o relay nunca pode
// fazer: responder a quem não tem a chave, mandar pacote a endereço que não
// falou com a sala ou a endereço privado, aceitar pacote repetido do host,
// amplificar tráfego de origem forjada, mostrar o IP do host ao cliente.
// Endereços nas faixas de documentação (RFC 5737), tratadas como públicas.
//   tools/run_tests.sh  (ou o alvo eruption-net-relay-tests)
#include "net/NetSession.hpp"
#include "net/NetValue.hpp"
#include "net/PacketLink.hpp"
#include "net/Relay.hpp"
#include "net/TokenGate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>

using namespace eruption;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::printf("  FALHOU (linha %d): %s\n", line, what);
    ++g_failures;
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr double kDt = 1.0 / 60.0;
const char* kRelayIp = "203.0.113.10";
const char* kRelayControl = "203.0.113.10:41921";
const char* kHostAddr = "198.51.100.5:5000";

NetAddress addr(const std::string& text) {
    NetAddress a;
    NetAddress::parse(text, a);
    return a;
}

ContentHash hashOf(const char* text) {
    ContentHash h{};
    Blake2b b(32);
    b.update(text, std::strlen(text));
    b.final(h.data());
    return h;
}

RelayKey randomKey() {
    RelayKey k{};
    const NetKey n = makeNetKey();
    std::memcpy(k.data(), n.data(), 32);
    return k;
}

struct Traffic {
    std::map<NetAddress, uint64_t> bytesTo, bytesFrom;
    std::vector<std::pair<NetAddress, std::vector<uint8_t>>> toClient; // (de onde veio, pacote)
    std::vector<std::vector<uint8_t>> hostToRelay;
};

struct Net {
    // O tráfego vem antes de tudo que manda pacote ao ser destruído (o host
    // avisa o relay ao sair, e o tap ainda conta).
    Traffic traffic;
    MemoryHub hub;
    RelayServer relay;
    RelayKey key = randomKey();
    std::unique_ptr<PacketLink> hostLink;
    NetSession host;
    std::vector<std::unique_ptr<PacketLink>> clientLinks;
    std::vector<std::unique_ptr<NetSession>> clients;
    std::vector<NetEvent> hostEvents;
    std::vector<std::vector<NetEvent>> clientEvents;
    double time = 0.0;
    bool dropHostToRelay = false;

    Net() {
        hub.setTap([this](const NetAddress& from, const NetAddress& to, std::vector<uint8_t>& p) {
            traffic.bytesTo[to] += p.size();
            traffic.bytesFrom[from] += p.size();
            if (to.type == NetAddress::IPv4 && to.bytes[0] == 192 && to.bytes[1] == 0 && to.bytes[2] == 2)
                traffic.toClient.push_back({from, p}); // 192.0.2.0/24: os clientes
            if (from == addr(kHostAddr) && to == addr(kRelayControl)) traffic.hostToRelay.push_back(p);
            return true;
        });
        RelayServer::Options o;
        relay.start(o, key, [this](uint16_t port) {
            return hub.createLink(addr(std::string(kRelayIp) + ":" + std::to_string(port)));
        }, time);
        hostLink = hub.createLink(addr(kHostAddr));
    }
    NetSession& addClient(const std::string& address) {
        clientLinks.push_back(hub.createLink(addr(address)));
        clients.push_back(std::make_unique<NetSession>());
        clientEvents.emplace_back();
        return *clients.back();
    }
    void step(int frames = 1) {
        for (int f = 0; f < frames; ++f) {
            time += kDt;
            relay.update(time);
            host.update(time);
            NetEvent e;
            while (host.poll(e)) hostEvents.push_back(e);
            for (size_t i = 0; i < clients.size(); ++i) {
                clients[i]->update(time);
                while (clients[i]->poll(e)) clientEvents[i].push_back(e);
            }
        }
    }
    template <typename Pred> bool until(Pred pred, double seconds) {
        for (double end = time + seconds; time < end;) {
            step();
            if (pred()) return true;
        }
        return false;
    }
};

std::vector<uint8_t> encode(const NetValue& v) {
    std::vector<uint8_t> out;
    encodeNetValue(v, out, 256);
    return out;
}

void testPlayThroughRelay() {
    std::printf("partida pelo relay\n");
    Net n;
    NetHostOptions o;
    o.content = hashOf("m");
    o.playerName = "Host";
    CHECK(n.host.hostRelayOn(*n.hostLink, kRelayControl, n.key, o, n.time));
    n.host.allowFromClient("eco", 32);
    CHECK(n.until([&] { return !n.host.publicAddress().empty(); }, 3.0));
    const std::string room = n.host.publicAddress();
    std::printf("  sala: %s\n", room.c_str());
    CHECK(room.rfind(kRelayIp, 0) == 0);
    NetSession& a = n.addClient("192.0.2.20:6000");
    NetSession& b = n.addClient("192.0.2.21:6000");
    CHECK(a.joinOn(*n.clientLinks[0], room, "A", hashOf("m"), n.time));
    CHECK(b.joinOn(*n.clientLinks[1], room, "B", hashOf("m"), n.time));
    CHECK(n.until([&] {
        return a.role() == NetSession::Role::Connected && b.role() == NetSession::Role::Connected &&
               n.host.players().size() == 3;
    }, 5.0));
    a.ask("eco", encode(NetValue::makeNumber(7)));
    n.host.send("oi", encode(NetValue::makeString("todos")));
    CHECK(n.until([&] {
        int hostGot = 0, bGot = 0;
        for (const NetEvent& e : n.hostEvents) hostGot += e.name == "eco";
        for (const NetEvent& e : n.clientEvents[1]) bGot += e.name == "oi";
        return hostGot == 1 && bGot == 1;
    }, 3.0));
    CHECK(n.relay.rooms() == 1);

    // O cliente só recebe pacote do relay, e nenhum leva o IP do host.
    const uint8_t hostIp[4] = {198, 51, 100, 5};
    bool onlyRelay = true, leaked = false;
    for (const auto& p : n.traffic.toClient) {
        onlyRelay = onlyRelay && p.first.type == NetAddress::IPv4 && std::memcmp(p.first.bytes, addr(kRelayControl).bytes, 4) == 0;
        leaked = leaked || std::search(p.second.begin(), p.second.end(), hostIp, hostIp + 4) != p.second.end();
    }
    CHECK(!n.traffic.toClient.empty() && onlyRelay && !leaked);

    // Pacote do host repetido (replay no caminho VPS<->host): o relay recusa.
    const uint64_t rejectedBefore = n.relay.rejected();
    const std::vector<uint8_t> old = n.traffic.hostToRelay[n.traffic.hostToRelay.size() / 2];
    for (int i = 0; i < 5; ++i) n.hub.inject(addr(kHostAddr), addr(kRelayControl), old.data(), static_cast<int>(old.size()));
    n.relay.update(n.time);
    CHECK(n.relay.rejected() == rejectedBefore + 5); // as 5 cópias, nenhuma encaminhada

    // Host sai: a sala fecha na hora (aviso) e o cliente cai.
    n.host.leave();
    n.step(30);
    CHECK(n.relay.rooms() == 0);
    CHECK(n.until([&] { return a.role() == NetSession::Role::Offline; }, 15.0));
}

void testRelayRefuses() {
    std::printf("o que o relay nunca faz\n");
    Net n;
    // Sem a chave: nada volta, nenhuma sala abre.
    auto stranger = n.hub.createLink(addr("192.0.2.66:7000"));
    RelayLink wrong(*stranger, addr(kRelayControl), randomKey());
    for (int i = 0; i < 180; ++i) {
        n.time += kDt;
        wrong.update(n.time);
        n.relay.update(n.time);
    }
    CHECK(n.relay.rooms() == 0 && !wrong.ready());
    CHECK(n.traffic.bytesTo[addr("192.0.2.66:7000")] == 0);
    std::vector<uint8_t> junk(300);
    std::mt19937 rng(3);
    for (int i = 0; i < 500; ++i) {
        for (uint8_t& x : junk) x = static_cast<uint8_t>(rng());
        std::memcpy(junk.data(), "ERRL", 4);
        n.hub.inject(addr("192.0.2.66:7000"), addr(kRelayControl), junk.data(), static_cast<int>(junk.size()));
    }
    n.relay.update(n.time);
    CHECK(n.traffic.bytesTo[addr("192.0.2.66:7000")] == 0);

    // Host de verdade (com a chave) não alcança vaga que não falou com a sala.
    auto evilHostLink = n.hub.createLink(addr("198.51.100.9:5000"));
    RelayLink evil(*evilHostLink, addr(kRelayControl), n.key);
    for (int i = 0; i < 60 && !evil.ready(); ++i) {
        n.time += kDt;
        evil.update(n.time);
        n.relay.update(n.time);
        NetAddress from;
        uint8_t buf[2048];
        while (evil.receive(from, buf, sizeof(buf)) > 0) {
        }
    }
    CHECK(evil.ready());
    const uint64_t refused = n.relay.refusedDestinations();
    const uint8_t payload[20] = {1};
    for (uint16_t slot = 1; slot < 40; ++slot) evil.send(relayVirtualAddress(slot), payload, sizeof(payload));
    n.relay.update(n.time);
    CHECK(n.relay.refusedDestinations() == refused + 39);

    // Endereço privado mandando para a sala: nem entra (não vira vaga).
    const NetAddress roomAddr = addr(evil.publicAddress());
    for (const char* priv : {"10.1.2.3:9", "192.168.0.7:9", "127.0.0.1:9", "172.20.0.1:9", "100.64.1.1:9"}) {
        n.hub.inject(addr(priv), roomAddr, payload, sizeof(payload));
    }
    const uint64_t forwardedBefore = n.relay.forwarded();
    n.relay.update(n.time);
    CHECK(n.relay.forwarded() == forwardedBefore);
    CHECK(n.relay.refusedDestinations() >= refused + 39 + 5);

    // Sala sem sinal do host fecha em 10 s.
    for (int i = 0; i < 11 * 60; ++i) {
        n.time += kDt;
        n.relay.update(n.time);
    }
    CHECK(n.relay.rooms() == 0);
}

// Origem forjada mandando para a sala (token, lixo, pedido de conexão): o
// que volta para ela nunca passa do que ela mandou.
void testNoAmplification() {
    std::printf("relay não amplifica origem forjada\n");
    Net n;
    NetHostOptions o;
    o.content = hashOf("m");
    o.dedicated = true;
    CHECK(n.host.hostRelayOn(*n.hostLink, kRelayControl, n.key, o, n.time));
    CHECK(n.until([&] { return !n.host.publicAddress().empty(); }, 3.0));
    const NetAddress room = addr(n.host.publicAddress());
    const NetAddress victim = addr("192.0.2.99:5555");
    auto victimLink = n.hub.createLink(victim); // a vítima existe, só não pediu nada
    std::mt19937 rng(9);
    for (int frame = 0; frame < 120; ++frame) {
        // Pedido de token bem formado (o maior pedido que dá resposta) e lixo.
        TokenRequest req;
        req.start(n.host.publicAddress(), *victimLink, n.time);
        req.update(n.time);
        std::vector<uint8_t> junk(64 + rng() % 1000);
        for (uint8_t& x : junk) x = static_cast<uint8_t>(rng());
        n.hub.inject(victim, room, junk.data(), static_cast<int>(junk.size()));
        n.step();
    }
    const uint64_t in = n.traffic.bytesFrom[victim];
    const uint64_t out = n.traffic.bytesTo[victim];
    std::printf("  origem forjada mandou %llu B, recebeu %llu B (razão %.2f)\n", static_cast<unsigned long long>(in),
                static_cast<unsigned long long>(out), in ? double(out) / double(in) : 0.0);
    CHECK(in > 0 && out <= in);
}

} // namespace

int main() {
    testPlayThroughRelay();
    testRelayRefuses();
    testNoAmplification();
    if (g_failures) {
        std::printf("FALHOU: %d\n", g_failures);
        return 1;
    }
    std::printf("tudo certo\n");
    return 0;
}
