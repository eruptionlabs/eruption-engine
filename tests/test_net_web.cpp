// Etapa 0 do transporte do navegador (spec 26): prova que o netcode e a
// NetSession (aperto de mão, hash de conteúdo, eventos nos dois sentidos)
// funcionam quando o cliente fala por um fluxo confiável enquadrado
// (WebSocketLink) em vez de UDP, e que uma ponte mínima leva os datagramas
// opacos entre esse fluxo e o lado UDP do host — tudo em memória, sem rede
// real. A ponte aqui faz o papel que o relay fará na etapa 1.
//   tools/run_tests.sh  (ou o alvo eruption-net-web-tests)
#include "net/Blake2b.hpp"
#include "net/LinkSim.hpp"
#include "net/NetSession.hpp"
#include "net/NetValue.hpp"
#include "net/PacketLink.hpp"
#include "net/Relay.hpp"
#include "net/WebSocketLink.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

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

std::vector<uint8_t> encode(const NetValue& v) {
    std::vector<uint8_t> out;
    encodeNetValue(v, out, 64);
    return out;
}

// Um "navegador" de mentira: NetSession cujo link é um WebSocketLink, ligado
// ao host por uma ponte que recorta os datagramas e os injeta no hub (o lado
// UDP). latency = atraso de ida do trecho browser→relay.
struct FakeBrowser {
    std::unique_ptr<WebSocketLink> link;   // o que a NetSession usa
    std::unique_ptr<WebSocketLink> bridge; // a outra ponta do fluxo, na ponte
    std::unique_ptr<PacketLink> hub;       // link do hub no endereço virtual do cliente
    NetSession session;
    NetAddress virt;
    NetAddress host;
    std::vector<NetEvent> events;

    void connect(MemoryHub& h, const NetAddress& hostAddr, const NetAddress& virtAddr, const std::string& name,
                 const ContentHash& content, double latency, double time) {
        host = hostAddr;
        virt = virtAddr;
        auto ends = MemoryStream::pair(latency);
        link = std::make_unique<WebSocketLink>(std::move(ends.first), hostAddr);
        bridge = std::make_unique<WebSocketLink>(std::move(ends.second), virtAddr);
        hub = h.createLink(virtAddr);
        session.joinOn(*link, hostAddr.toString(), name, content, time);
    }
    void pump(double time) {
        link->update(time);
        bridge->update(time);
        hub->update(time);
        uint8_t buf[4096];
        NetAddress from;
        int n;
        while ((n = bridge->receive(from, buf, sizeof(buf))) > 0) hub->send(host, buf, n); // cliente -> host
        while ((n = hub->receive(from, buf, sizeof(buf))) > 0) bridge->send(virt, buf, n);  // host -> cliente
        session.update(time);
        NetEvent e;
        while (session.poll(e)) events.push_back(e);
    }
    int ecos() const {
        int n = 0;
        for (const NetEvent& e : events) n += (e.type == NetEvent::Type::Message && e.name == "eco");
        return n;
    }
    bool failedWith(const char* reason) const {
        for (const NetEvent& e : events)
            if (e.type == NetEvent::Type::Failed && e.reason == reason) return true;
        return false;
    }
};

void testBrowserJoinsAndExchanges() {
    std::printf("navegador entra e troca eventos (atraso de 120 ms)\n");
    MemoryHub hub;
    const NetAddress hostAddr = addr("10.0.0.1:41920");
    auto hostLink = hub.createLink(hostAddr);
    NetSession host;
    NetHostOptions o;
    o.port = 41920;
    o.dedicated = true;
    o.content = hashOf("mapa+scripts");
    CHECK(host.hostOn(*hostLink, o, 0.0));
    host.allowFromClient("ping", 64);

    FakeBrowser b;
    b.connect(hub, hostAddr, addr("10.9.9.9:7000"), "Browser", hashOf("mapa+scripts"), 0.12, 0.0);

    double time = 0.0;
    std::vector<NetEvent> hostEvents;
    auto step = [&] {
        time += kDt;
        hostLink->update(time);
        host.update(time);
        NetEvent e;
        while (host.poll(e)) hostEvents.push_back(e);
        b.pump(time);
    };
    auto until = [&](auto pred, double seconds) {
        for (double end = time + seconds; time < end;) {
            step();
            if (pred()) return true;
        }
        return false;
    };

    CHECK(until([&] { return b.session.role() == NetSession::Role::Connected; }, 8.0));
    CHECK(host.players().size() == 1);

    // O host responde "eco" a cada "ping" do navegador.
    host.allowFromClient("ping", 64);
    const std::vector<uint8_t> payload = encode(NetValue::makeNumber(42));
    int pings = 0;
    auto exchange = [&] {
        for (const NetEvent& e : hostEvents)
            if (e.type == NetEvent::Type::Message && e.name == "ping") {
                host.sendTo(e.player, "eco", e.data);
            }
        hostEvents.clear();
        return b.ecos() >= 3;
    };
    for (int i = 0; i < 300 && b.ecos() < 3; ++i) {
        if (b.session.role() == NetSession::Role::Connected && pings < 5 && i % 12 == 0) {
            b.session.ask("ping", payload);
            ++pings;
        }
        step();
        exchange();
    }
    CHECK(b.ecos() >= 3);
    std::printf("  conectou, %d ecos recebidos\n", b.ecos());
}

void testBrowserContentMismatch() {
    std::printf("navegador com conteúdo diferente é recusado\n");
    MemoryHub hub;
    const NetAddress hostAddr = addr("10.0.0.1:41920");
    auto hostLink = hub.createLink(hostAddr);
    NetSession host;
    NetHostOptions o;
    o.port = 41920;
    o.dedicated = true;
    o.content = hashOf("mapa-A");
    CHECK(host.hostOn(*hostLink, o, 0.0));

    FakeBrowser b;
    b.connect(hub, hostAddr, addr("10.9.9.9:7001"), "Browser", hashOf("mapa-B"), 0.0, 0.0);

    double time = 0.0;
    for (int i = 0; i < 600; ++i) {
        time += kDt;
        hostLink->update(time);
        host.update(time);
        b.pump(time);
        if (b.failedWith("conteudo_diferente")) break;
    }
    CHECK(b.failedWith("conteudo_diferente"));
    CHECK(b.session.role() == NetSession::Role::Offline);
}

// ----------------------------------------------- critério de pronto (etapa 1)
// Host nativo (UDP) + cliente nativo (UDP) + "falso navegador" (ws), todos na
// mesma sala pelo relay DE VERDADE (RelayServer), com 5% de perda e 120 ms de
// atraso nos trechos UDP (LinkSim). O trecho ws é confiável (TCP/ws). Prova
// que qualquer combinação joga junta e que o relay faz a ponte ws<->UDP sem
// deixar de ser cego.

RelayKey randomRelayKey() {
    RelayKey k{};
    const NetKey n = makeNetKey();
    std::memcpy(k.data(), n.data(), 32);
    return k;
}

// Entrega ao relay as conexões ws que o teste enfileira (o papel do aceitador
// TCP/ws real da etapa 1b).
struct QueueAcceptor final : StreamAcceptor {
    std::deque<std::unique_ptr<StreamBackend>> ready;
    std::unique_ptr<StreamBackend> accept() override {
        if (ready.empty()) return nullptr;
        auto b = std::move(ready.front());
        ready.pop_front();
        return b;
    }
};

void testDoneCriterionThroughRelay() {
    std::printf("critério de pronto: host+nativo+navegador pelo relay, 5%% perda + 120 ms nos trechos UDP\n");
    const char* kRelayIp = "203.0.113.10";
    const std::string control = std::string(kRelayIp) + ":41921";
    const RelayKey key = randomRelayKey();

    MemoryHub hub;
    std::vector<std::unique_ptr<PacketLink>> bases; // links do hub, embrulhados em LinkSim (perda nas duas pontas)
    uint32_t seed = 1;
    LinkSimConfig udp;
    udp.lossPercent = 5.0f;
    udp.latencyMs = 120.0f;

    RelayServer relay;
    RelayServer::Options ro;
    double time = 0.0;
    relay.start(
        ro, key,
        [&](uint16_t port) -> std::unique_ptr<PacketLink> {
            auto base = hub.createLink(addr(std::string(kRelayIp) + ":" + std::to_string(port)));
            PacketLink* b = base.get();
            bases.push_back(std::move(base));
            return std::unique_ptr<PacketLink>(new LinkSim(*b, udp, seed++)); // relay->par com perda/atraso
        },
        time);
    auto acc = std::make_unique<QueueAcceptor>();
    QueueAcceptor* acceptor = acc.get();
    relay.setStreamAcceptor(std::move(acc));

    // Host nativo atrás de CGNAT: abre a sala no relay por UDP (par->relay com perda).
    auto hostBase = hub.createLink(addr("198.51.100.5:5000"));
    LinkSim hostSim(*hostBase, udp, 100);
    NetSession host;
    NetHostOptions o;
    o.content = hashOf("mapa");
    o.playerName = "Host";
    o.maxPlayers = 8;
    CHECK(host.hostRelayOn(hostSim, control, key, o, time));
    host.allowFromClient("ping", 64);
    for (int i = 0; i < 300 && host.publicAddress().empty(); ++i) {
        time += kDt;
        hostSim.update(time);
        relay.update(time);
        host.update(time);
    }
    CHECK(!host.publicAddress().empty());
    const std::string room = host.publicAddress();
    const NetAddress roomAddr = addr(room);
    CHECK(room.rfind(kRelayIp, 0) == 0); // a sala é o endereço da VPS, nunca o do host
    std::printf("  sala: %s\n", room.c_str());

    // Cliente nativo (UDP, com perda) disca a sala.
    auto cliBase = hub.createLink(addr("192.0.2.20:6000"));
    LinkSim cliSim(*cliBase, udp, 200);
    NetSession cli;
    CHECK(cli.joinOn(cliSim, room, "Nativo", hashOf("mapa"), time));

    // "Falso navegador": fluxo ws confiável até o relay. Manda o quadro de papel
    // (entrar na sala) e roda a NetSession por cima do WebSocketLink.
    auto ends = MemoryStream::pair(0.0);
    acceptor->ready.push_back(std::move(ends.first)); // a ponta que o relay aceita
    WebSocketLink browserLink(std::move(ends.second), roomAddr);
    const uint16_t rp = roomAddr.port;
    const uint8_t directive[3] = {kStreamJoin, static_cast<uint8_t>(rp), static_cast<uint8_t>(rp >> 8)};
    browserLink.send(roomAddr, directive, 3);
    NetSession browser;
    CHECK(browser.joinOn(browserLink, room, "Browser", hashOf("mapa"), time));

    std::vector<NetEvent> hostEvents, browserEvents, cliEvents;
    auto step = [&] {
        time += kDt;
        hostSim.update(time);
        cliSim.update(time);
        relay.update(time); // pompa os LinkSim das salas/controle e as conexões ws
        host.update(time);
        cli.update(time);
        browserLink.update(time);
        browser.update(time);
        NetEvent e;
        while (host.poll(e)) hostEvents.push_back(e);
        while (cli.poll(e)) cliEvents.push_back(e);
        while (browser.poll(e)) browserEvents.push_back(e);
        for (const NetEvent& ev : hostEvents)
            if (ev.type == NetEvent::Type::Message && ev.name == "ping") host.sendTo(ev.player, "eco", ev.data);
        hostEvents.clear();
    };
    auto until = [&](auto pred, double seconds) {
        for (double end = time + seconds; time < end;) {
            step();
            if (pred()) return true;
        }
        return false;
    };

    // Os dois conectam e o host vê 3 jogadores (local + nativo + navegador).
    CHECK(until(
        [&] {
            return cli.role() == NetSession::Role::Connected && browser.role() == NetSession::Role::Connected &&
                   host.players().size() == 3;
        },
        20.0));
    CHECK(relay.rooms() == 1);
    CHECK(relay.streamConnections() == 1);

    // Troca estável nos dois sentidos, sob perda: cada cliente manda ping, recebe eco.
    const std::vector<uint8_t> payload = encode(NetValue::makeNumber(42));
    auto ecos = [](const std::vector<NetEvent>& evs) {
        int n = 0;
        for (const NetEvent& e : evs) n += (e.type == NetEvent::Type::Message && e.name == "eco");
        return n;
    };
    int tick = 0, cliPings = 0, browserPings = 0;
    const bool exchanged = until(
        [&] {
            ++tick;
            if (cli.role() == NetSession::Role::Connected && cliPings < 6 && tick % 15 == 0) {
                cli.ask("ping", payload);
                ++cliPings;
            }
            if (browser.role() == NetSession::Role::Connected && browserPings < 6 && tick % 17 == 0) {
                browser.ask("ping", payload);
                ++browserPings;
            }
            return ecos(cliEvents) >= 3 && ecos(browserEvents) >= 3;
        },
        20.0);
    CHECK(exchanged);
    std::printf("  nativo %d ecos, navegador %d ecos; perda UDP: nativo %llu, host %llu\n", ecos(cliEvents),
                ecos(browserEvents), static_cast<unsigned long long>(cliSim.droppedCount()),
                static_cast<unsigned long long>(hostSim.droppedCount()));

    // A perda foi mesmo exercida nos trechos UDP (senão o teste não provaria nada).
    CHECK(cliSim.droppedCount() > 0 || hostSim.droppedCount() > 0);

    // A sala fecha quando o host sai; o relay larga as conexões ws órfãs.
    host.leave();
    for (int i = 0; i < 60; ++i) step();
    CHECK(relay.rooms() == 0);
}

} // namespace

int main() {
    testBrowserJoinsAndExchanges();
    testBrowserContentMismatch();
    testDoneCriterionThroughRelay();
    if (g_failures) {
        std::printf("FALHOU: %d\n", g_failures);
        return 1;
    }
    std::printf("tudo certo\n");
    return 0;
}
