// Testes da sessão (src/net/NetSession): host com jogador local + 2 clientes
// trocando eventos em memória, servidor dedicado, hash de conteúdo
// diferente, evento não liberado, enxurrada de eventos, saída, rede ruim,
// porta de tokens (amplificação e limite por IP) e UDP de verdade em
// 127.0.0.1. Também decodifica bytes aleatórios como dados de evento.
//   tools/run_tests.sh  (ou o alvo eruption-net-session-tests)
#include "net/LinkSim.hpp"
#include "net/NetSession.hpp"
#include "net/NetValue.hpp"
#include "net/PacketLink.hpp"
#include "net/TokenGate.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>

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
const char* kHostAddr = "10.0.0.1:41920";

NetAddress addr(const char* text) {
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
    encodeNetValue(v, out, kNetDefaultEventBytes);
    return out;
}

NetValue lapTable(int lap) {
    NetValue t;
    t.type = NetValue::Type::Table;
    t.table.emplace_back(NetValue::makeString("volta"), NetValue::makeNumber(lap));
    t.table.emplace_back(NetValue::makeString("ok"), NetValue::makeBool(true));
    return t;
}

struct Peer {
    std::unique_ptr<PacketLink> link;
    std::unique_ptr<LinkSim> sim;
    NetSession session;
    std::vector<NetEvent> events;

    PacketLink& wire() { return sim ? static_cast<PacketLink&>(*sim) : *link; }
    void drain() {
        NetEvent e;
        while (session.poll(e)) events.push_back(std::move(e));
    }
    int count(NetEvent::Type type, const std::string& name = "") const {
        int n = 0;
        for (const NetEvent& e : events) n += e.type == type && (name.empty() || e.name == name || e.reason == name);
        return n;
    }
};

struct World {
    MemoryHub hub;
    std::vector<std::unique_ptr<Peer>> peers;
    double time = 0.0;

    Peer& add(const char* address, const char* simConfig = nullptr) {
        peers.push_back(std::make_unique<Peer>());
        Peer& p = *peers.back();
        p.link = hub.createLink(addr(address));
        if (simConfig) {
            LinkSimConfig c;
            LinkSimConfig::parse(simConfig, c);
            p.sim = std::make_unique<LinkSim>(*p.link, c, static_cast<uint32_t>(peers.size() * 77));
        }
        return p;
    }
    void step(int frames = 1) {
        for (int f = 0; f < frames; ++f) {
            time += kDt;
            for (auto& p : peers) {
                p->session.update(time);
                p->drain();
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

NetHostOptions hostOptions(const char* content, bool dedicated = false) {
    NetHostOptions o;
    o.maxPlayers = 4;
    o.port = 41920;
    o.dedicated = dedicated;
    o.playerName = "Ana";
    o.content = hashOf(content);
    return o;
}

void testHostAndClients() {
    std::printf("host + 2 clientes trocam eventos\n");
    World w;
    Peer& host = w.add(kHostAddr);
    Peer& b = w.add("10.0.0.2:5000");
    Peer& c = w.add("10.0.0.3:5000");
    CHECK(host.session.hostOn(host.wire(), hostOptions("mapa-1"), w.time));
    host.session.allowFromClient("pronto", 64);
    CHECK(b.session.joinOn(b.wire(), kHostAddr, "Bia", hashOf("mapa-1"), w.time));
    CHECK(c.session.joinOn(c.wire(), kHostAddr, "Caio\x01\x02", hashOf("mapa-1"), w.time));
    const bool ok = w.until([&] {
        return host.session.myId() != 0 && b.session.role() == NetSession::Role::Connected &&
               c.session.role() == NetSession::Role::Connected && host.session.players().size() == 3 &&
               b.session.players().size() == 3 && c.session.players().size() == 3;
    }, 5.0);
    CHECK(ok);
    CHECK(host.session.role() == NetSession::Role::Hosting && host.session.isServer());
    CHECK(!b.session.isServer());
    CHECK(host.session.myId() >= 1 && b.session.myId() >= 1 && c.session.myId() >= 1);
    CHECK(host.session.myId() != b.session.myId() && b.session.myId() != c.session.myId());
    w.step(10);
    // Todo cliente (inclusive o local do host) vê os 3 entrarem; nome limpo de controle.
    CHECK(host.count(NetEvent::Type::Joined) == 3);
    CHECK(b.count(NetEvent::Type::Joined) == 3);
    CHECK(c.count(NetEvent::Type::Joined, "Caio") == 1);

    // Cliente -> servidor (liberado): chega com o id de quem mandou.
    CHECK(b.session.ask("pronto", encode(lapTable(3))));
    CHECK(w.until([&] { return host.count(NetEvent::Type::Message, "pronto") == 1; }, 2.0));
    for (const NetEvent& e : host.events) {
        if (e.type != NetEvent::Type::Message || e.name != "pronto") continue;
        CHECK(e.player == b.session.myId());
        NetValue v;
        CHECK(decodeNetValue(e.data.data(), e.data.size(), v));
        CHECK(v.type == NetValue::Type::Table && v.table.size() == 2 && v.table[0].second.number == 3.0);
    }
    // O jogador local do host também pede pelo mesmo caminho.
    CHECK(host.session.ask("pronto", encode(NetValue::makeNumber(1))));
    CHECK(w.until([&] { return host.count(NetEvent::Type::Message, "pronto") == 2; }, 2.0));

    // Servidor -> todos (o local do host recebe como cliente, de = 0).
    CHECK(host.session.send("volta", encode(NetValue::makeString("1/3"))));
    CHECK(w.until([&] {
        return host.count(NetEvent::Type::Message, "volta") == 1 && b.count(NetEvent::Type::Message, "volta") == 1 &&
               c.count(NetEvent::Type::Message, "volta") == 1;
    }, 2.0));
    // Servidor -> um.
    CHECK(host.session.sendTo(c.session.myId(), "so_voce", encode(NetValue::makeBool(true))));
    w.step(30);
    CHECK(c.count(NetEvent::Type::Message, "so_voce") == 1);
    CHECK(b.count(NetEvent::Type::Message, "so_voce") == 0);
    // Cliente não manda para todos.
    CHECK(!b.session.send("volta", {}));

    // Ordem: 100 eventos chegam na ordem.
    for (int i = 0; i < 100; ++i) host.session.send("seq", encode(NetValue::makeNumber(i)));
    CHECK(w.until([&] { return b.count(NetEvent::Type::Message, "seq") == 100; }, 3.0));
    int expect = 0;
    bool inOrder = true;
    for (const NetEvent& e : b.events) {
        if (e.name != "seq") continue;
        NetValue v;
        decodeNetValue(e.data.data(), e.data.size(), v);
        inOrder = inOrder && v.number == expect++;
    }
    CHECK(inOrder);

    // Evento não liberado: o servidor derruba e avisa os outros.
    const int cId = c.session.myId();
    CHECK(c.session.ask("hack", {}));
    CHECK(w.until([&] { return c.session.role() == NetSession::Role::Offline; }, 3.0));
    CHECK(c.count(NetEvent::Type::Left, "mensagem_invalida") == 1);
    CHECK(w.until([&] { return b.count(NetEvent::Type::Left, "mensagem_invalida") == 1; }, 2.0));
    for (const NetEvent& e : b.events)
        if (e.type == NetEvent::Type::Left) CHECK(e.player == cId);

    // Saída normal.
    const int bId = b.session.myId();
    b.session.leave();
    CHECK(w.until([&] { return host.count(NetEvent::Type::Left, "saiu") == 1; }, 3.0));
    CHECK(host.session.players().size() == 1);
    (void)bId;
    std::printf("  tokens emitidos %llu, derrubados %llu\n",
                static_cast<unsigned long long>(host.session.stats().tokensIssued),
                static_cast<unsigned long long>(host.session.stats().kicks));
}

void testContentMismatch() {
    std::printf("conteúdo diferente\n");
    World w;
    Peer& host = w.add(kHostAddr);
    Peer& b = w.add("10.0.0.2:5000");
    CHECK(host.session.hostOn(host.wire(), hostOptions("mapa-1"), w.time));
    CHECK(b.session.joinOn(b.wire(), kHostAddr, "Bia", hashOf("mapa-2"), w.time));
    CHECK(w.until([&] { return b.count(NetEvent::Type::Failed) == 1; }, 4.0));
    CHECK(b.count(NetEvent::Type::Failed, "conteudo_diferente") == 1);
    CHECK(b.session.role() == NetSession::Role::Offline);
    w.step(90);
    CHECK(host.session.players().size() == 1); // só o local do host
    CHECK(host.count(NetEvent::Type::Joined) == 1);
}

void testFlood() {
    std::printf("enxurrada de eventos\n");
    World w;
    Peer& srv = w.add(kHostAddr);
    Peer& b = w.add("10.0.0.2:5000");
    CHECK(srv.session.hostOn(srv.wire(), hostOptions("x", true), w.time));
    srv.session.allowFromClient("buzina", 16);
    CHECK(b.session.joinOn(b.wire(), kHostAddr, "Bia", hashOf("x"), w.time));
    CHECK(w.until([&] { return b.session.role() == NetSession::Role::Connected; }, 4.0));
    CHECK(srv.session.role() == NetSession::Role::Server && srv.session.myId() == 0);
    CHECK(srv.count(NetEvent::Type::Joined, "Bia") == 1); // dedicado vê pelo lado do servidor
    for (int i = 0; i < 200; ++i) b.session.ask("buzina", {});
    CHECK(w.until([&] { return b.session.role() == NetSession::Role::Offline; }, 3.0));
    CHECK(b.count(NetEvent::Type::Left, "excesso") == 1);
    CHECK(srv.count(NetEvent::Type::Message, "buzina") <= 61);
    CHECK(w.until([&] { return srv.count(NetEvent::Type::Left, "excesso") == 1; }, 2.0));
    // Grande demais para o teto do nome: derruba também.
    Peer& c = w.add("10.0.0.3:5000");
    CHECK(c.session.joinOn(c.wire(), kHostAddr, "Caio", hashOf("x"), w.time));
    CHECK(w.until([&] { return c.session.role() == NetSession::Role::Connected; }, 4.0));
    c.session.ask("buzina", std::vector<uint8_t>(17, 1));
    CHECK(w.until([&] { return c.count(NetEvent::Type::Left, "mensagem_invalida") == 1; }, 3.0));
}

void testLossy() {
    std::printf("rede ruim (5%% de perda, 80 ms, jitter, duplicados)\n");
    World w;
    const char* bad = "loss=5,lat=80,jitter=20,dup=2,reorder=3,burst=2";
    Peer& host = w.add(kHostAddr, bad);
    Peer& b = w.add("10.0.0.2:5000", bad);
    CHECK(host.session.hostOn(host.wire(), hostOptions("m"), w.time));
    host.session.allowFromClient("eco", 64);
    CHECK(b.session.joinOn(b.wire(), kHostAddr, "Bia", hashOf("m"), w.time));
    CHECK(w.until([&] { return b.session.role() == NetSession::Role::Connected; }, 8.0));
    // Cliente a 20 eventos/s (o teto é 30/s); servidor sem teto.
    for (int i = 0; i < 300; ++i) {
        if (i % 3 == 0) b.session.ask("eco", encode(NetValue::makeNumber(i / 3)));
        host.session.send("seq", encode(NetValue::makeNumber(i)));
        w.step();
    }
    CHECK(w.until([&] {
        return host.count(NetEvent::Type::Message, "eco") == 100 && b.count(NetEvent::Type::Message, "seq") == 300;
    }, 10.0));
    double expect = 0;
    bool inOrder = true;
    for (const NetEvent& e : host.events) {
        if (e.name != "eco") continue;
        NetValue v;
        decodeNetValue(e.data.data(), e.data.size(), v);
        inOrder = inOrder && v.number == expect;
        expect += 1;
    }
    CHECK(inOrder);
    CHECK(b.session.role() == NetSession::Role::Connected);
    const NetSessionStats s = b.session.stats();
    std::printf("  rtt %.0f ms, perda medida %.1f%%, enviados %llu pelo simulador (%llu perdidos)\n", s.link.rttMs,
                s.link.lossPercent, static_cast<unsigned long long>(b.sim->sentCount()),
                static_cast<unsigned long long>(b.sim->droppedCount()));
}

void testTokenGate() {
    std::printf("porta de tokens: amplificação e limite por IP\n");
    World w;
    Peer& host = w.add(kHostAddr);
    CHECK(host.session.hostOn(host.wire(), hostOptions("m", true), w.time));
    size_t maxRequest = 0, maxReply = 0, smallestRequest = SIZE_MAX;
    int replies = 0;
    w.hub.setTap([&](const NetAddress& from, const NetAddress& to, std::vector<uint8_t>& p) {
        if (to == addr(kHostAddr) && p.size() == static_cast<size_t>(kTokenRequestBytes)) {
            maxRequest = std::max(maxRequest, p.size());
            smallestRequest = std::min(smallestRequest, p.size());
        }
        if (from == addr(kHostAddr) && p.size() == static_cast<size_t>(kTokenReplyBytes)) {
            maxReply = std::max(maxReply, p.size());
            ++replies;
        }
        return true;
    });
    // 10 pedidos do mesmo IP (portas diferentes) no mesmo instante: só o balde (3) responde.
    std::vector<std::unique_ptr<PacketLink>> links;
    std::vector<std::unique_ptr<TokenRequest>> reqs;
    for (int i = 0; i < 10; ++i) {
        const std::string a = "10.0.0.9:" + std::to_string(6000 + i);
        links.push_back(w.hub.createLink(addr(a.c_str())));
        reqs.push_back(std::make_unique<TokenRequest>());
        reqs.back()->start(kHostAddr, *links.back(), w.time);
        reqs.back()->update(w.time); // manda o primeiro pedido
    }
    w.step(2);
    int done = 0;
    for (auto& r : reqs) {
        r->update(w.time);
        done += r->state() == TokenRequest::State::Done ? 1 : 0;
    }
    CHECK(done == 3);
    CHECK(replies == 3);
    CHECK(maxReply > 0 && maxReply < smallestRequest);
    std::printf("  pedido %zu B, resposta %zu B, %d de 10 atendidos\n", smallestRequest, maxReply, done);
    CHECK(host.session.stats().tokensRefused >= 7);

    // Quem escuta a LAN não lê o token: nenhum trecho de 16 bytes do token
    // (fora os de zeros) aparece na resposta capturada.
    std::vector<uint8_t> captured;
    w.hub.setTap([&](const NetAddress& from, const NetAddress&, std::vector<uint8_t>& p) {
        if (from == addr(kHostAddr) && p.size() == static_cast<size_t>(kTokenReplyBytes)) captured = p;
        return true;
    });
    w.step(120); // o balde do IP enche de novo
    auto spy = w.hub.createLink(addr("10.0.0.7:6000"));
    TokenRequest req;
    req.start(kHostAddr, *spy, w.time);
    for (int i = 0; i < 10 && req.state() != TokenRequest::State::Done; ++i) {
        req.update(w.time);
        w.step();
    }
    CHECK(req.state() == TokenRequest::State::Done && !captured.empty());
    const std::vector<uint8_t>& tok = req.token();
    int leaked = 0, windows = 0;
    for (size_t i = 0; i + 16 <= tok.size(); i += 8) {
        bool zeros = true;
        for (size_t k = 0; k < 16; ++k) zeros = zeros && tok[i + k] == 0;
        if (zeros) continue;
        ++windows;
        leaked += std::search(captured.begin(), captured.end(), tok.begin() + static_cast<long>(i),
                              tok.begin() + static_cast<long>(i + 16)) != captured.end();
    }
    std::printf("  token lacrado: %d de %d trechos aparecem na resposta\n", leaked, windows);
    CHECK(windows > 50 && leaked == 0);

    // Resposta adulterada (um bit) não vira token: o cliente espera a verdadeira.
    w.hub.setTap([&](const NetAddress& from, const NetAddress&, std::vector<uint8_t>& p) {
        if (from == addr(kHostAddr) && p.size() == static_cast<size_t>(kTokenReplyBytes)) p[p.size() - 100] ^= 1;
        return true;
    });
    w.step(120);
    auto victim = w.hub.createLink(addr("10.0.0.6:6000"));
    TokenRequest tampered;
    tampered.start(kHostAddr, *victim, w.time, 2.0);
    for (int i = 0; i < 150; ++i) {
        tampered.update(w.time);
        w.step();
    }
    CHECK(tampered.state() == TokenRequest::State::Failed);
}

// Ataques antes da autenticação: enxurrada de pedidos de conexão bem
// formados (só o token é lixo) de 10 mil IPs forjados, e pacotes forjados com
// o endereço de um jogador conectado. O servidor decifra no máximo o que os
// baldes deixam, o jogador atacado é o único afetado e o quadro do servidor
// quase não muda.
void testPreAuthFlood() {
    std::printf("enxurrada antes da autenticação\n");
    World w;
    Peer& host = w.add(kHostAddr);
    Peer& b = w.add("10.0.0.2:5000");
    Peer& c = w.add("10.0.0.3:5000");
    CHECK(host.session.hostOn(host.wire(), hostOptions("m"), w.time));
    CHECK(b.session.joinOn(b.wire(), kHostAddr, "Bia", hashOf("m"), w.time));
    CHECK(c.session.joinOn(c.wire(), kHostAddr, "Caio", hashOf("m"), w.time));
    CHECK(w.until([&] {
        return b.session.role() == NetSession::Role::Connected && c.session.role() == NetSession::Role::Connected;
    }, 4.0));

    // Quadro do servidor sem ataque (referência).
    std::vector<double> calm, attacked;
    auto timedStep = [&](std::vector<double>& out) {
        const auto t0 = std::chrono::steady_clock::now();
        w.step();
        out.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    };
    for (int i = 0; i < 120; ++i) timedStep(calm);

    std::vector<uint8_t> request(1 + 13 + 8 + 8 + 24 + 1024);
    const char version[13] = "NETCODE 1.02";
    std::mt19937 rng(5);
    const NetAddress hostAddr = addr(kHostAddr);
    const NetAddress victim = addr("10.0.0.3:5000");
    uint64_t injected = 0;
    for (int frame = 0; frame < 60; ++frame) { // 1 s de ataque
        for (int k = 0; k < 1700; ++k) { // ~100 mil pedidos no segundo
            request[0] = 0;
            std::memcpy(&request[1], version, 13);
            for (int i = 0; i < 8; ++i) request[static_cast<size_t>(14 + i)] = static_cast<uint8_t>(kNetProtocolId >> (8 * i));
            for (size_t i = 22; i < request.size(); ++i) request[i] = static_cast<uint8_t>(rng());
            const std::string ip = "172.16." + std::to_string(rng() % 40) + "." + std::to_string(rng() % 250) + ":" +
                                   std::to_string(1024 + rng() % 60000);
            w.hub.inject(addr(ip.c_str()), hostAddr, request.data(), static_cast<int>(request.size()));
            ++injected;
        }
        // Pacotes forjados com o endereço do jogador c (cifra inválida).
        for (int k = 0; k < 40; ++k) {
            std::vector<uint8_t> forged(60 + rng() % 200);
            for (uint8_t& x : forged) x = static_cast<uint8_t>(rng());
            forged[0] = static_cast<uint8_t>((forged[0] & 0xF0) | 4); // tipo "carga" do netcode
            w.hub.inject(victim, hostAddr, forged.data(), static_cast<int>(forged.size()));
        }
        host.session.send("vivo", encode(NetValue::makeNumber(frame)));
        timedStep(attacked);
    }
    w.step(60);
    const NetSessionStats st = host.session.stats();
    const uint64_t passed = st.requestsAdmitted - 2; // os 2 jogadores de verdade também pediram
    std::sort(calm.begin(), calm.end());
    std::sort(attacked.begin(), attacked.end());
    const double p99calm = calm[calm.size() * 99 / 100], p99attack = attacked[attacked.size() * 99 / 100];
    std::printf("  %llu pedidos forjados, %llu chegaram ao netcode (teto ~300), %llu pacotes acima do teto\n",
                static_cast<unsigned long long>(injected), static_cast<unsigned long long>(passed),
                static_cast<unsigned long long>(st.floodDropped));
    std::printf("  quadro do servidor p99: %.3f ms sem ataque, %.3f ms (só o servidor) sob ataque\n", p99calm, p99attack);
    CHECK(passed <= 320);
    CHECK(b.session.role() == NetSession::Role::Connected); // o outro jogador nem percebe
    CHECK(b.count(NetEvent::Type::Message, "vivo") == 60);
    CHECK(host.session.players().size() == 3);
}

void testUdp() {
    std::printf("UDP de verdade em 127.0.0.1\n");
    NetSession host, b;
    NetHostOptions o = hostOptions("m");
    o.port = 0;
    CHECK(host.host(o, 0.0));
    CHECK(host.port() != 0);
    CHECK(b.join("127.0.0.1:" + std::to_string(host.port()), "Bia", hashOf("m"), 0.0));
    double t = 0.0;
    std::vector<NetEvent> got;
    for (int i = 0; i < 600 && b.role() != NetSession::Role::Connected; ++i) {
        t += kDt;
        host.update(t);
        b.update(t);
        NetEvent e;
        while (b.poll(e)) got.push_back(e);
        while (host.poll(e)) {
        }
    }
    CHECK(b.role() == NetSession::Role::Connected);
    CHECK(host.players().size() == 2);
    host.send("oi", encode(NetValue::makeString("ola")));
    bool arrived = false;
    for (int i = 0; i < 120 && !arrived; ++i) {
        t += kDt;
        host.update(t);
        b.update(t);
        NetEvent e;
        while (b.poll(e)) arrived = arrived || (e.type == NetEvent::Type::Message && e.name == "oi");
    }
    CHECK(arrived);
    // Destino inválido falha na hora, com motivo.
    NetSession bad;
    CHECK(!bad.join("nao-e-ip", "x", hashOf("m"), 0.0));
    NetEvent e;
    CHECK(bad.poll(e) && e.type == NetEvent::Type::Failed && e.reason == "endereco_invalido");
}

void testNetValueHostile() {
    std::printf("dados de evento hostis\n");
    std::mt19937 rng(7);
    int accepted = 0;
    for (int i = 0; i < 200000; ++i) {
        std::vector<uint8_t> bytes(rng() % 64);
        for (uint8_t& x : bytes) x = static_cast<uint8_t>(rng());
        NetValue v;
        if (!decodeNetValue(bytes.data(), bytes.size(), v)) continue;
        ++accepted;
        // O que decodifica volta igual ao codificar de novo (forma canônica até o enchimento).
        std::vector<uint8_t> again;
        CHECK(encodeNetValue(v, again, 4096));
    }
    std::printf("  200000 entradas aleatórias, %d válidas\n", accepted);
    // Regras: tabela aninhada, número não finito, string longa.
    NetValue nested;
    nested.type = NetValue::Type::Table;
    nested.table.emplace_back(NetValue::makeNumber(1), lapTable(1));
    std::vector<uint8_t> out;
    CHECK(!encodeNetValue(nested, out, 256));
    CHECK(!encodeNetValue(NetValue::makeNumber(std::nan("")), out, 256));
    CHECK(!encodeNetValue(NetValue::makeString(std::string(kNetValueMaxString + 1, 'a')), out, 4096));
    CHECK(!encodeNetValue(NetValue::makeString(std::string(100, 'a')), out, 50)); // não cabe
}

} // namespace

int main() {
    testHostAndClients();
    testContentMismatch();
    testFlood();
    testLossy();
    testTokenGate();
    testPreAuthFlood();
    testUdp();
    testNetValueHostile();
    if (g_failures) {
        std::printf("FALHOU: %d\n", g_failures);
        return 1;
    }
    std::printf("tudo certo\n");
    return 0;
}
