// Testes da conexão segura (src/net/NetcodeEndpoint + PacketLink + LinkSim):
// conexão, servidor cheio, token vencido, chave e protocolo errados, replay,
// pacote adulterado, anti-amplificação, rede ruim (5% de perda, 100 ms) e
// UDP de verdade em 127.0.0.1. Sem GPU nem motor:
//   tools/run_tests.sh  (ou o alvo eruption-net-tests com ERUPTION_BUILD_TESTS)
#include "net/LinkSim.hpp"
#include "net/NetcodeEndpoint.hpp"
#include "net/PacketLink.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <thread>

using namespace eruption;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::printf("  FALHOU (linha %d): %s\n", line, what);
    ++g_failures;
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr uint64_t kProtocol = 0x4552555054494f4eull;
constexpr double kDt = 1.0 / 60.0;
const char* kServerAddr = "10.0.0.1:40000";

NetAddress addr(const char* text) {
    NetAddress a;
    NetAddress::parse(text, a);
    return a;
}

std::vector<uint8_t> token(const NetKey& key, uint64_t clientId, int expireSeconds = 30, uint64_t protocol = kProtocol,
                           const char* server = kServerAddr) {
    ConnectTokenRequest r;
    r.publicAddresses = {server};
    r.clientId = clientId;
    r.protocolId = protocol;
    r.expireSeconds = expireSeconds;
    std::vector<uint8_t> t;
    makeConnectToken(r, key, t);
    return t;
}

// Roda servidor e clientes até todos saírem do aperto de mão (ou o prazo acabar).
void pump(NetServer& server, std::vector<NetClient*> clients, double& time, double seconds) {
    const double end = time + seconds;
    while (time < end) {
        for (NetClient* c : clients) c->update(time);
        server.update(time);
        bool settled = true;
        for (NetClient* c : clients) {
            const NetClient::State s = c->state();
            settled = settled && (s == NetClient::State::Connected || static_cast<int>(s) < 0);
        }
        if (settled) return;
        time += kDt;
    }
}

void testConnectAndExchange() {
    std::printf("conexão e troca de pacotes\n");
    MemoryHub hub;
    auto serverLink = hub.createLink(addr(kServerAddr));
    auto link1 = hub.createLink(addr("10.0.0.2:50000"));
    auto link2 = hub.createLink(addr("10.0.0.3:50000"));
    const NetKey key = makeNetKey();
    NetServer server;
    double time = 0.0;
    CHECK(server.start(kServerAddr, kProtocol, key, 4, *serverLink, time));
    int connections = 0;
    server.onConnection = [&](int, bool on) { connections += on ? 1 : 0; };
    NetClient c1, c2;
    CHECK(c1.connect(token(key, 101), "10.0.0.2:50000", *link1, time));
    CHECK(c2.connect(token(key, 102), "10.0.0.3:50000", *link2, time));
    pump(server, {&c1, &c2}, time, 5.0);
    CHECK(c1.state() == NetClient::State::Connected);
    CHECK(c2.state() == NetClient::State::Connected);
    CHECK(connections == 2);
    CHECK(server.clientId(c1.clientIndex()) == 101);
    CHECK(server.clientId(c2.clientIndex()) == 102);

    const uint8_t hello[5] = {'o', 'l', 'a', '!', 0};
    c1.send(hello, 5);
    server.send(c2.clientIndex(), hello, 5);
    for (int k = 0; k < 3; ++k) {
        time += kDt;
        c1.update(time);
        c2.update(time);
        server.update(time);
    }
    std::vector<uint8_t> got;
    CHECK(server.receive(c1.clientIndex(), got) && got.size() == 5 && std::memcmp(got.data(), hello, 5) == 0);
    CHECK(c2.receive(got) && got.size() == 5);
    // Carga maior que o limite não sai.
    std::vector<uint8_t> big(kNetMaxPayload + 1, 7);
    c1.send(big.data(), static_cast<int>(big.size()));
    time += kDt;
    c1.update(time);
    server.update(time);
    CHECK(!server.receive(c1.clientIndex(), got));
}

void testServerFull() {
    std::printf("servidor cheio recusa\n");
    MemoryHub hub;
    auto serverLink = hub.createLink(addr(kServerAddr));
    auto link1 = hub.createLink(addr("10.0.0.2:50000"));
    auto link2 = hub.createLink(addr("10.0.0.3:50000"));
    const NetKey key = makeNetKey();
    NetServer server;
    double time = 0.0;
    server.start(kServerAddr, kProtocol, key, 1, *serverLink, time);
    NetClient c1, c2;
    c1.connect(token(key, 1), "10.0.0.2:50000", *link1, time);
    pump(server, {&c1}, time, 5.0);
    c2.connect(token(key, 2), "10.0.0.3:50000", *link2, time);
    pump(server, {&c1, &c2}, time, 5.0);
    CHECK(c1.state() == NetClient::State::Connected);
    CHECK(c2.state() == NetClient::State::Denied);
}

void testBadTokens() {
    std::printf("chave errada, protocolo errado, servidor errado, token vencido\n");
    const NetKey key = makeNetKey();
    const NetKey other = makeNetKey();
    struct Case {
        const char* name;
        std::vector<uint8_t> tok;
    };
    std::vector<Case> cases = {
        {"chave", token(other, 1)},
        {"protocolo", token(key, 1, 30, kProtocol + 1)},
        {"servidor", token(key, 1, 30, kProtocol, "10.9.9.9:40000")},
    };
    for (Case& c : cases) {
        MemoryHub hub;
        auto serverLink = hub.createLink(addr(kServerAddr));
        auto link = hub.createLink(addr("10.0.0.2:50000"));
        NetServer server;
        double time = 0.0;
        server.start(kServerAddr, kProtocol, key, 2, *serverLink, time);
        size_t serverPackets = 0;
        hub.setTap([&](const NetAddress& from, const NetAddress&, std::vector<uint8_t>&) {
            serverPackets += from == addr(kServerAddr) ? 1 : 0;
            return true;
        });
        NetClient client;
        client.connect(c.tok, "10.0.0.2:50000", *link, time);
        pump(server, {&client}, time, 8.0);
        const bool refused = client.state() != NetClient::State::Connected;
        if (!refused) std::printf("    caso '%s' conectou\n", c.name);
        CHECK(refused);
        // "servidor" vai para outro endereço: o hub nem entrega. Os outros
        // chegam e o servidor não responde nada a quem não prova o token.
        CHECK(serverPackets == 0);
    }

    // Token vencido: validade de 1 s, usado depois de 2 s (relógio de parede).
    MemoryHub hub;
    auto serverLink = hub.createLink(addr(kServerAddr));
    auto link = hub.createLink(addr("10.0.0.2:50000"));
    NetServer server;
    double time = 0.0;
    server.start(kServerAddr, kProtocol, key, 2, *serverLink, time);
    const std::vector<uint8_t> old = token(key, 9, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    NetClient client;
    client.connect(old, "10.0.0.2:50000", *link, time);
    pump(server, {&client}, time, 8.0);
    CHECK(client.state() != NetClient::State::Connected);
}

void testReplayTamperAmplification() {
    std::printf("replay, adulteração e amplificação\n");
    MemoryHub hub;
    const NetAddress serverAddress = addr(kServerAddr);
    const NetAddress clientAddress = addr("10.0.0.2:50000");
    auto serverLink = hub.createLink(serverAddress);
    auto link = hub.createLink(clientAddress);
    const NetKey key = makeNetKey();
    NetServer server;
    double time = 0.0;
    server.start(kServerAddr, kProtocol, key, 2, *serverLink, time);

    NetClient client;
    bool handshake = true;
    // Anti-amplificação: antes de o cliente estar validado, cada resposta do
    // servidor tem de ser no máximo do tamanho do pacote que a provocou.
    size_t lastRequest = 0;
    bool amplified = false;
    std::vector<uint8_t> captured;
    bool capture = false, tamper = false;
    hub.setTap([&](const NetAddress& from, const NetAddress&, std::vector<uint8_t>& p) {
        if (handshake) {
            if (from == clientAddress) lastRequest = p.size();
            else if (p.size() > lastRequest) amplified = true;
        }
        if (from == clientAddress && capture && p.size() > 20) {
            captured = p;
            capture = false;
        }
        if (from == clientAddress && tamper && p.size() > 20) {
            p[p.size() / 2] ^= 0x40;
            tamper = false;
        }
        return true;
    });
    client.connect(token(key, 7), "10.0.0.2:50000", *link, time);
    pump(server, {&client}, time, 5.0);
    handshake = false;
    CHECK(client.state() == NetClient::State::Connected);
    CHECK(!amplified);

    auto step = [&] {
        time += kDt;
        client.update(time);
        server.update(time);
    };
    std::vector<uint8_t> got;
    while (server.receive(client.clientIndex(), got)) {}

    // Replay: o mesmo pacote cifrado reinjetado 5 vezes só entra uma.
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    capture = true;
    client.send(payload, 8);
    step();
    CHECK(!captured.empty());
    for (int k = 0; k < 5; ++k) hub.inject(clientAddress, serverAddress, captured.data(), static_cast<int>(captured.size()));
    step();
    int copies = 0;
    while (server.receive(client.clientIndex(), got))
        copies += got.size() == 8 && std::memcmp(got.data(), payload, 8) == 0 ? 1 : 0;
    CHECK(copies == 1);

    // Adulterado: um bit trocado no meio e o pacote não autentica.
    tamper = true;
    client.send(payload, 8);
    step();
    CHECK(!server.receive(client.clientIndex(), got));
    CHECK(server.connected(client.clientIndex())); // pacote ruim não derruba a conexão
}

void testLossyNetwork() {
    std::printf("rede ruim: 5%% de perda, 100 ms de ida e volta, jitter, duplicação, reordenação\n");
    LinkSimConfig sim;
    std::string error;
    CHECK(LinkSimConfig::parse("loss=5,lat=50,jitter=10,dup=2,reorder=3,burst=2", sim, &error));
    CHECK(!LinkSimConfig::parse("loss=5,xyz=1", sim, &error));
    CHECK(!LinkSimConfig::parse("loss=500", sim, &error));
    LinkSimConfig::parse("loss=5,lat=50,jitter=10,dup=2,reorder=3,burst=2", sim, &error);

    MemoryHub hub;
    auto serverRaw = hub.createLink(addr(kServerAddr));
    auto clientRaw = hub.createLink(addr("10.0.0.2:50000"));
    LinkSim serverLink(*serverRaw, sim, 1);
    LinkSim clientLink(*clientRaw, sim, 2);
    const NetKey key = makeNetKey();
    NetServer server;
    double time = 0.0;
    server.start(kServerAddr, kProtocol, key, 2, serverLink, time);
    NetClient client;
    client.connect(token(key, 3), "10.0.0.2:50000", clientLink, time);
    pump(server, {&client}, time, 10.0);
    CHECK(client.state() == NetClient::State::Connected);

    const int total = 2000;
    std::set<uint32_t> seen;
    int received = 0;
    std::vector<uint8_t> got;
    for (int k = 0; k < total + 30; ++k) {
        if (k < total) {
            uint8_t p[4];
            std::memcpy(p, &k, 4);
            client.send(p, 4);
        }
        time += kDt;
        client.update(time);
        server.update(time);
        while (server.receive(client.clientIndex(), got)) {
            uint32_t id = 0;
            if (got.size() == 4) std::memcpy(&id, got.data(), 4);
            seen.insert(id);
            ++received;
        }
    }
    const double delivered = 100.0 * static_cast<double>(seen.size()) / total;
    std::printf("    entregues %.1f%% (perdidos no simulador: %llu de %llu), duplicados entregues: %d\n", delivered,
                static_cast<unsigned long long>(clientLink.droppedCount()),
                static_cast<unsigned long long>(clientLink.sentCount()), received - static_cast<int>(seen.size()));
    CHECK(delivered > 90.0 && delivered < 99.5);
    CHECK(received == static_cast<int>(seen.size())); // duplicata não passa da janela anti-replay
    CHECK(client.state() == NetClient::State::Connected);
}

void testRealUdp() {
    std::printf("UDP de verdade em 127.0.0.1\n");
    UdpLink serverLink, clientLink;
    CHECK(serverLink.open(addr("127.0.0.1:0")));
    CHECK(clientLink.open(addr("127.0.0.1:0")));
    const std::string serverAddress = serverLink.localAddress().toString();
    const NetKey key = makeNetKey();
    NetServer server;
    double time = 0.0;
    CHECK(server.start(serverAddress, kProtocol, key, 2, serverLink, time));
    NetClient client;
    CHECK(client.connect(token(key, 5, 30, kProtocol, serverAddress.c_str()), clientLink.localAddress().toString(),
                         clientLink, time));
    for (int k = 0; k < 300 && client.state() != NetClient::State::Connected; ++k) {
        client.update(time);
        server.update(time);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        time += kDt;
    }
    CHECK(client.state() == NetClient::State::Connected);
    const uint8_t p[3] = {9, 8, 7};
    client.send(p, 3);
    std::vector<uint8_t> got;
    bool ok = false;
    for (int k = 0; k < 100 && !ok; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        time += kDt;
        client.update(time);
        server.update(time);
        ok = server.receive(client.clientIndex(), got) && got.size() == 3 && got[0] == 9;
    }
    CHECK(ok);
    NetAddress v6;
    CHECK(NetAddress::parse("[::1]:40000", v6) && v6.type == NetAddress::IPv6 && v6.toString() == "[::1]:40000");
    CHECK(!NetAddress::parse("1.2.3.4:99999", v6));
    CHECK(!NetAddress::parse("nada", v6));
}

} // namespace

int main() {
    testConnectAndExchange();
    testServerFull();
    testBadTokens();
    testReplayTamperAmplification();
    testLossyNetwork();
    testRealUdp();
    if (g_failures) {
        std::printf("%d falha(s)\n", g_failures);
        return 1;
    }
    std::printf("tudo certo\n");
    return 0;
}
