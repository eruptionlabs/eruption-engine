// Fuzz com estado da sessão de rede (doc 24, item 4). Cada entrada é um
// roteiro: o primeiro byte escolhe o alvo (servidor antes do hello, servidor
// depois do hello, ou o nosso cliente diante de um servidor hostil) e o resto
// é uma sequência de operações de um par que já passou pelo aperto de mão
// (token válido): mensagem confiável, sem garantia, carga crua do netcode,
// corpo de pacote dos canais forjado, avanço do relógio e mensagens quase
// certas (hello, evento liberado) com o fim mutado. A ordem importa: é o que
// o fuzz de um pacote só não alcança (falhas de máquina de estados).
//
// Confere, além de ASan/UBSan:
//  - o servidor só entrega ao jogo evento liberado ("ok"), dentro do teto de
//    bytes, de jogador que passou pelo hello;
//  - nunca há mais jogadores que o máximo; o cliente nunca fica "connected"
//    com id fora da faixa.
//
// Com libFuzzer (clang -fsanitize=fuzzer) usa só LLVMFuzzerTestOneInput.
// Sem ele (GCC), o main abaixo muta roteiros aleatórios por N segundos:
//   tools/fuzz/run_net_fuzz.sh [segundos] [semente] session
#include "../../tests/net_hostile_peer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace eruption;
using namespace eruption::hostile;

namespace {

struct Script {
    const uint8_t* p;
    const uint8_t* end;
    bool more() const { return p < end; }
    uint8_t byte() { return p < end ? *p++ : 0; }
    std::vector<uint8_t> bytes(size_t maxLen) {
        const size_t want = byte() % (maxLen + 1); // antes de medir o que resta: byte() avança p
        const size_t n = std::min<size_t>(want, static_cast<size_t>(end - p));
        std::vector<uint8_t> v(p, p + n);
        p += n;
        return v;
    }
};

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "invariante quebrada: %s\n", what);
    std::abort();
}

// Operações comuns aos dois lados: `conn` é a conexão do par hostil, `raw`
// manda carga crua do netcode (antes dos canais).
template <typename Arena, typename RawSend>
void play(Arena& a, Connection& conn, RawSend raw, Script& s) {
    int ops = 0;
    while (s.more() && ops++ < 64) {
        const uint8_t op = s.byte();
        switch (op % 7) {
            case 0: {
                const int channel = s.byte() & 1;
                const std::vector<uint8_t> m = s.bytes(200);
                if (!m.empty()) conn.sendReliable(channel, m.data(), static_cast<int>(m.size()));
                break;
            }
            case 1: {
                const std::vector<uint8_t> m = s.bytes(200);
                if (!m.empty()) conn.sendUnreliable(m.data(), static_cast<int>(m.size()));
                break;
            }
            case 2: {
                const std::vector<uint8_t> m = s.bytes(255);
                if (!m.empty()) raw(m);
                break;
            }
            case 3: {
                const uint16_t seq = static_cast<uint16_t>(s.byte() | (s.byte() << 8));
                const std::vector<uint8_t> pkt = forgeChannelPacket(s.bytes(200), a.time, seq);
                if (!pkt.empty()) raw(pkt);
                break;
            }
            case 4: a.step(1 + s.byte() % 30); break;
            case 5: {
                std::vector<uint8_t> m = helloMessage(hashOf("conteudo"), "f");
                const std::vector<uint8_t> tail = s.bytes(8);
                if (!tail.empty()) m.resize(m.size() - std::min(m.size() - 1, size_t(tail[0] % 8)));
                m.insert(m.end(), tail.begin(), tail.end());
                conn.sendReliable(0, m.data(), static_cast<int>(m.size()));
                break;
            }
            default: {
                const std::vector<uint8_t> m = eventMessage(s.byte() & 1 ? "ok" : "ko", s.bytes(24));
                conn.sendReliable(1, m.data(), static_cast<int>(m.size()));
                break;
            }
        }
    }
    a.step(30);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Script s{data, data + size};
    const uint8_t mode = s.byte() % 3;
    if (mode < 2) {
        ServerArena a;
        if (!a.start()) fail("cliente hostil não conectou");
        if (mode == 1 && !a.hello()) fail("hello certo recusado");
        play(a, a.raw.conn, [&](const std::vector<uint8_t>& m) { a.raw.client.send(m.data(), static_cast<int>(m.size())); }, s);
        for (const NetEvent& e : a.events) {
            if (e.type != NetEvent::Type::Message) continue;
            if (e.name != "ok") fail("evento não liberado chegou ao jogo");
            if (e.data.size() > 16) fail("evento acima do teto chegou ao jogo");
            if (e.player != 1) fail("evento com remetente errado");
        }
        if (a.server.players().size() > 1) fail("mais jogadores que o possível");
    } else {
        ClientArena a;
        if (!a.start()) fail("cliente não conectou ao servidor hostil");
        play(a, a.conn, [&](const std::vector<uint8_t>& m) { a.server.send(0, m.data(), static_cast<int>(m.size())); }, s);
        const int id = a.client.myId();
        if (a.client.role() == NetSession::Role::Connected && (id < 1 || id > kNetMaxPlayers)) fail("id fora da faixa");
    }
    return 0;
}

#ifndef ERUPTION_LIBFUZZER
int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1u;
    std::mt19937 rng(seed);
    std::vector<std::vector<uint8_t>> corpus;
    for (int k = 0; k < 64; ++k) {
        std::vector<uint8_t> v(1 + rng() % 512);
        for (uint8_t& b : v) b = static_cast<uint8_t>(rng());
        corpus.push_back(v);
    }
    const auto start = std::chrono::steady_clock::now();
    uint64_t runs = 0;
    for (;;) {
        std::vector<uint8_t> in = corpus[rng() % corpus.size()];
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; ++e) {
            switch (rng() % 5) {
                case 0: if (!in.empty()) in[rng() % in.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
                case 1: if (!in.empty()) in[rng() % in.size()] = static_cast<uint8_t>(rng()); break;
                case 2: if (in.size() > 1) in.resize(rng() % in.size()); break;
                case 3: in.push_back(static_cast<uint8_t>(rng())); break;
                default: {
                    const std::vector<uint8_t>& other = corpus[rng() % corpus.size()];
                    in.insert(in.end(), other.begin(), other.begin() + static_cast<long>(rng() % (other.size() + 1)));
                    if (in.size() > 2048) in.resize(2048);
                }
            }
        }
        LLVMFuzzerTestOneInput(in.data(), in.size());
        if (rng() % 16 == 0 && corpus.size() < 2048) corpus.push_back(in);
        ++runs;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (t >= seconds) break;
    }
    std::printf("fuzz da sessão: %llu roteiros em %.0f s, sem falha\n", static_cast<unsigned long long>(runs), seconds);
    return 0;
}
#endif
