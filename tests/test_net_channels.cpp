// Testes dos canais (src/net/Channels) e do BLAKE2b (src/net/Blake2b), sem
// netcode nem socket: duas conexões ligadas por uma fila com perda, atraso,
// duplicação e reordenação. Confere entrega completa e na ordem das
// mensagens confiáveis, fila cheia, pacote hostil e os vetores da RFC 7693.
//   tools/run_tests.sh  (ou o alvo eruption-net-channels-tests)
#include "net/Blake2b.hpp"
#include "net/Channels.hpp"
#include "net/Wire.hpp"

#include <cstdio>
#include <cstring>
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

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

void testBlake2b() {
    std::printf("blake2b\n");
    uint8_t out[64];
    Blake2b h(64);
    h.update("abc", 3);
    CHECK(h.final(out));
    CHECK(hex(out, 64) ==
          "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
          "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
    Blake2b empty(64);
    CHECK(empty.final(out));
    CHECK(hex(out, 64) ==
          "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
          "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
    // Em pedaços dá o mesmo que de uma vez (cruza a borda de 128 B).
    std::string big(1000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
    uint8_t a[32], b[32];
    Blake2b one(32);
    one.update(big.data(), big.size());
    one.final(a);
    Blake2b parts(32);
    for (size_t i = 0; i < big.size(); i += 127) parts.update(big.data() + i, std::min<size_t>(127, big.size() - i));
    parts.final(b);
    CHECK(std::memcmp(a, b, 32) == 0);
    // Chave muda o resultado; parâmetros fora da faixa invalidam.
    const uint8_t key[16] = {1, 2, 3};
    Blake2b keyed(32, key, sizeof(key));
    keyed.update(big.data(), big.size());
    keyed.final(b);
    CHECK(std::memcmp(a, b, 32) != 0);
    Blake2b bad(65);
    CHECK(!bad.final(out));
}

// Fio entre duas conexões com os defeitos de uma rede ruim.
struct Wire2 {
    struct InFlight {
        double at;
        int to;
        std::vector<uint8_t> data;
    };
    std::vector<InFlight> flight;
    std::mt19937 rng{12345};
    double loss = 0.0, latency = 0.05, jitter = 0.0, dup = 0.0;
    uint64_t sent = 0, dropped = 0;

    void push(int to, const uint8_t* d, int n, double now) {
        ++sent;
        std::uniform_real_distribution<double> u(0.0, 1.0);
        if (u(rng) < loss) {
            ++dropped;
            return;
        }
        const int copies = u(rng) < dup ? 2 : 1;
        for (int c = 0; c < copies; ++c)
            flight.push_back({now + latency + u(rng) * jitter, to, std::vector<uint8_t>(d, d + n)});
    }
    void deliver(Connection* conns[2], double now) {
        for (size_t i = 0; i < flight.size();) {
            if (flight[i].at <= now) {
                InFlight f = std::move(flight[i]);
                flight.erase(flight.begin() + static_cast<long>(i));
                conns[f.to]->receivePacket(f.data.data(), static_cast<int>(f.data.size()));
            } else {
                ++i;
            }
        }
    }
};

struct Pair {
    Connection a, b;
    Wire2 wire;
    double time = 0.0;

    Pair() {
        a.init([this](const uint8_t* d, int n) { wire.push(1, d, n, time); }, 0.0);
        b.init([this](const uint8_t* d, int n) { wire.push(0, d, n, time); }, 0.0);
    }
    void step(double dt = 1.0 / 30.0) {
        time += dt;
        Connection* conns[2] = {&a, &b};
        wire.deliver(conns, time);
        a.update(time);
        b.update(time);
    }
};

std::vector<uint8_t> message(int channel, int index) {
    // Tamanho variado, conteúdo que identifica canal e índice.
    std::vector<uint8_t> m(static_cast<size_t>(4 + (index * 37) % 300));
    m[0] = static_cast<uint8_t>(channel);
    m[1] = static_cast<uint8_t>(index & 0xFF);
    m[2] = static_cast<uint8_t>(index >> 8);
    for (size_t i = 3; i < m.size(); ++i) m[i] = static_cast<uint8_t>(index + static_cast<int>(i));
    return m;
}

void reliableUnder(double loss, double jitter, double dup, const char* name) {
    std::printf("confiável e ordenado: %s\n", name);
    Pair p;
    p.wire.loss = loss;
    p.wire.jitter = jitter;
    p.wire.dup = dup;
    const int total = 2000;
    int queued[kNetChannels] = {0, 0};
    int got[kNetChannels] = {0, 0};
    bool inOrder = true, intact = true;
    for (int frame = 0; frame < 6000 && (got[0] < total || got[1] < total); ++frame) {
        // 2 mensagens por quadro por canal até a janela permitir.
        for (int ch = 0; ch < kNetChannels; ++ch)
            for (int k = 0; k < 2 && queued[ch] < total && p.a.pendingReliable() < 200; ++k) {
                const std::vector<uint8_t> m = message(ch, queued[ch]);
                if (p.a.sendReliable(ch, m.data(), static_cast<int>(m.size()))) ++queued[ch];
            }
        p.step();
        int ch = 0;
        std::vector<uint8_t> out;
        while (p.b.receiveReliable(ch, out)) {
            const int index = out.size() >= 3 ? out[1] | (out[2] << 8) : -1;
            if (index != got[ch]) inOrder = false;
            if (out != message(ch, got[ch])) intact = false;
            ++got[ch];
        }
    }
    std::printf("  entregues %d + %d de %d, pacotes %llu, perdidos %llu\n", got[0], got[1], total,
                static_cast<unsigned long long>(p.wire.sent), static_cast<unsigned long long>(p.wire.dropped));
    CHECK(got[0] == total && got[1] == total);
    CHECK(inOrder);
    CHECK(intact);
    CHECK(!p.a.failed() && !p.b.failed());
    // Depois de tudo entregue, os acks zeram a fila do lado que mandou.
    for (int i = 0; i < 120; ++i) p.step();
    CHECK(p.a.pendingReliable() == 0);
}

void testUnreliable() {
    std::printf("sem garantia\n");
    Pair p;
    int sent = 0, got = 0;
    for (int i = 0; i < 300; ++i) {
        const uint8_t msg[3] = {7, static_cast<uint8_t>(i), 9};
        if (p.a.sendUnreliable(msg, 3)) ++sent;
        p.step();
        std::vector<uint8_t> out;
        while (p.b.receiveUnreliable(out)) got += out.size() == 3 && out[0] == 7 ? 1 : 0;
    }
    for (int i = 0; i < 10; ++i) {
        p.step();
        std::vector<uint8_t> out;
        while (p.b.receiveUnreliable(out)) ++got;
    }
    CHECK(sent == 300 && got == 300);
    // Grande demais para um pacote: recusa sem derrubar.
    std::vector<uint8_t> huge(kNetMaxPacketBytes, 1);
    CHECK(!p.a.sendUnreliable(huge.data(), static_cast<int>(huge.size())));
    CHECK(!p.a.failed());
}

void testWindowFull() {
    std::printf("fila cheia derruba\n");
    Pair p;
    p.wire.loss = 1.0; // nada volta: nenhum ack
    const uint8_t m[4] = {1, 2, 3, 4};
    int ok = 0;
    for (int i = 0; i < kNetMessageWindow + 10; ++i) ok += p.a.sendReliable(1, m, 4) ? 1 : 0;
    CHECK(ok == kNetMessageWindow);
    CHECK(p.a.failed() && std::string(p.a.failReason()) == "fila_cheia");
    CHECK(!p.a.sendReliable(1, m, 4));
}

// Pacote decifrado mas hostil: a conexão cai, nada é entregue, nada estoura.
void testHostile() {
    std::printf("pacote hostil\n");
    std::mt19937 rng(99);
    int failedCount = 0, delivered = 0;
    for (int round = 0; round < 2000; ++round) {
        Connection c;
        c.init([](const uint8_t*, int) {}, 0.0);
        // Cabeçalho do reliable (prefixo 0 = sem acks extras) + corpo aleatório.
        std::vector<uint8_t> pkt(1 + 2 + 2 + 4 + (rng() % 200));
        pkt[0] = 0;
        for (size_t i = 1; i < pkt.size(); ++i) pkt[i] = static_cast<uint8_t>(rng());
        c.receivePacket(pkt.data(), static_cast<int>(pkt.size()));
        int ch;
        std::vector<uint8_t> out;
        while (c.receiveReliable(ch, out)) ++delivered;
        failedCount += c.failed() ? 1 : 0;
    }
    std::printf("  2000 pacotes aleatórios: %d derrubaram, %d mensagens passaram\n", failedCount, delivered);
    // Os que não derrubam o reliable já descartou pelo cabeçalho; o que
    // importa é que lixo nunca vira mensagem.
    CHECK(delivered == 0);
    CHECK(failedCount > 0);

    // Mensagem além da janela (id 300 com next 0): derruba.
    Pair p;
    uint8_t body[64];
    BitWriter w(body, sizeof(body));
    w.writeInt(0, 0, 16);
    w.writeInt(1, 0, kNetMaxMessagesPerPacket);
    w.writeInt(1, 0, kNetChannels - 1);
    w.writeBits(300, 16);
    w.writeInt(2, 1, kNetMaxMessageBytes);
    w.alignToByte();
    const uint8_t two[2] = {5, 6};
    w.writeBytes(two, 2);
    const size_t n = w.finish();
    // O cabeçalho é do reliable: gera um pacote de verdade com um corpo de
    // tamanho conhecido (1 mensagem sem garantia de n bytes = 3 + n + 1) e
    // troca o corpo pelo forjado.
    Connection sender;
    std::vector<uint8_t> captured;
    sender.init([&](const uint8_t* d, int len) { captured.assign(d, d + len); }, 0.0);
    sender.sendUnreliable(body, static_cast<int>(n));
    sender.update(0.1);
    CHECK(captured.size() > n + 4);
    const size_t header = captured.size() - (n + 4);
    std::vector<uint8_t> forged(captured.begin(), captured.begin() + static_cast<long>(header));
    forged.insert(forged.end(), body, body + n);
    p.b.receivePacket(forged.data(), static_cast<int>(forged.size()));
    CHECK(p.b.failed());
}

} // namespace

int main() {
    testBlake2b();
    reliableUnder(0.0, 0.0, 0.0, "rede limpa");
    reliableUnder(0.2, 0.08, 0.05, "20% de perda, jitter 80 ms, 5% duplicados");
    reliableUnder(0.5, 0.15, 0.1, "50% de perda");
    testUnreliable();
    testWindowFull();
    testHostile();
    if (g_failures) {
        std::printf("FALHOU: %d\n", g_failures);
        return 1;
    }
    std::printf("tudo certo\n");
    return 0;
}
