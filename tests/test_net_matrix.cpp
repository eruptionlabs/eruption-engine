// Matriz estado x mensagem da sessão (doc 24, item 4): um cliente com token
// válido manda cada tipo de mensagem em cada estado (antes e depois do
// hello) e o servidor tem de fazer exatamente o esperado: aceitar, ou
// derrubar com o motivo certo, sem nunca entregar ao jogo o que não foi
// liberado. E o inverso: um servidor hostil contra o nosso cliente.
//   tools/run_tests.sh  (ou o alvo eruption-net-matrix-tests)
#include "net_hostile_peer.hpp"

#include <cstdio>
#include <functional>

using namespace eruption;
using namespace eruption::hostile;

namespace {

int g_failures = 0;
int g_cells = 0;

void check(bool ok, const std::string& what) {
    ++g_cells;
    if (ok) return;
    std::printf("  FALHOU: %s\n", what.c_str());
    ++g_failures;
}

enum class Expect { Accept, Deliver, Kick };

struct Cell {
    const char* name;
    std::function<void(ServerArena&)> send;
    Expect before;            // antes do hello
    const char* beforeReason;
    Expect after;             // depois do hello
    const char* afterReason;
    int maxDelivered = 0; // eventos que o balde deixa passar antes de derrubar
};

void sendReliable(ServerArena& a, int channel, const std::vector<uint8_t>& m) {
    a.raw.conn.sendReliable(channel, m.data(), static_cast<int>(m.size()));
}

void runCell(const Cell& c, bool afterHello) {
    ServerArena a;
    if (!a.start()) {
        check(false, std::string(c.name) + ": não conectou");
        return;
    }
    if (afterHello && !a.hello()) {
        check(false, std::string(c.name) + ": hello não aceito");
        return;
    }
    const size_t eventsBefore = a.events.size();
    c.send(a);
    a.step(90);
    const Expect want = afterHello ? c.after : c.before;
    const char* reason = afterHello ? c.afterReason : c.beforeReason;
    const std::string label = std::string(c.name) + (afterHello ? " [depois do hello]" : " [antes do hello]");
    // O motivo chega ao cliente pela recusa; quando a conexão dele já falhou
    // (pacote inválido) não há como mandar: vale o que o servidor registrou.
    std::string kicked = a.raw.kickedFor();
    if (kicked.empty() && a.server.stats().kicks > 0) kicked = a.server.stats().lastKick;
    int delivered = 0;
    for (size_t i = eventsBefore; i < a.events.size(); ++i)
        delivered += a.events[i].type == NetEvent::Type::Message;
    switch (want) {
        case Expect::Accept:
            check(kicked.empty() && a.raw.welcomed(), label + ": devia aceitar (motivo '" + kicked + "')");
            break;
        case Expect::Deliver:
            check(kicked.empty() && delivered == 1, label + ": devia entregar 1 evento (" + std::to_string(delivered) + ")");
            break;
        case Expect::Kick:
            check(kicked == reason, label + ": devia derrubar com '" + reason + "', veio '" + kicked + "'");
            check(delivered <= c.maxDelivered, label + ": entregou evento ao jogo");
            break;
    }
}

std::vector<uint8_t> bytes(size_t n, uint8_t v) { return std::vector<uint8_t>(n, v); }

void serverMatrix() {
    std::printf("servidor: estado x mensagem\n");
    const ContentHash good = hashOf("conteudo");
    const std::vector<Cell> cells = {
        {"hello certo", [&](ServerArena& a) { sendReliable(a, 0, helloMessage(good, "z")); },
         Expect::Accept, "", Expect::Kick, "mensagem_invalida"},
        {"hello com conteúdo diferente", [](ServerArena& a) { sendReliable(a, 0, helloMessage(hashOf("outro"), "z")); },
         Expect::Kick, "conteudo_diferente", Expect::Kick, "mensagem_invalida"},
        {"hello cortado", [](ServerArena& a) { sendReliable(a, 0, systemMessage(kHello, bytes(10, 1))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"welcome vindo do cliente", [](ServerArena& a) { sendReliable(a, 0, systemMessage(kWelcome, bytes(8, 0))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"recusa vinda do cliente", [](ServerArena& a) { sendReliable(a, 0, systemMessage(kReject, bytes(4, 0))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"entrou vindo do cliente", [](ServerArena& a) { sendReliable(a, 0, systemMessage(kJoined, bytes(4, 0))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"saiu vindo do cliente", [](ServerArena& a) { sendReliable(a, 0, systemMessage(kLeft, bytes(4, 0))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"tipo de sistema desconhecido", [](ServerArena& a) { sendReliable(a, 0, systemMessage(6, bytes(4, 0))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"evento liberado", [](ServerArena& a) { sendReliable(a, 1, eventMessage("ok", bytes(4, 2))); },
         Expect::Kick, "mensagem_invalida", Expect::Deliver, ""},
        {"evento não liberado", [](ServerArena& a) { sendReliable(a, 1, eventMessage("hack", bytes(4, 2))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"evento acima do teto", [](ServerArena& a) { sendReliable(a, 1, eventMessage("ok", bytes(17, 2))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"evento sem nome", [](ServerArena& a) { sendReliable(a, 1, eventMessage("", bytes(4, 2))); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"evento com nome longo", [](ServerArena& a) { sendReliable(a, 1, eventMessage(std::string(40, 'a'), {}, 64)); },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "mensagem_invalida"},
        {"enxurrada de eventos", [](ServerArena& a) {
             for (int i = 0; i < 100; ++i) sendReliable(a, 1, eventMessage("ok", bytes(1, 0)));
         },
         Expect::Kick, "mensagem_invalida", Expect::Kick, "excesso", 61},
        {"mensagem fora da janela", [](ServerArena& a) {
             uint8_t body[16];
             BitWriter w(body, sizeof(body));
             w.writeInt(0, 0, 16);
             w.writeInt(1, 0, kNetMaxMessagesPerPacket);
             w.writeInt(1, 0, kNetChannels - 1);
             w.writeBits(300, 16);
             w.writeInt(1, 1, kNetMaxMessageBytes);
             w.alignToByte();
             w.writeBits(9, 8);
             const size_t n = w.finish();
             const std::vector<uint8_t> pkt = forgeChannelPacket(std::vector<uint8_t>(body, body + n), a.time);
             a.raw.client.send(pkt.data(), static_cast<int>(pkt.size()));
         },
         Expect::Kick, "pacote_invalido", Expect::Kick, "pacote_invalido"},
        {"corpo de pacote truncado", [](ServerArena& a) {
             const std::vector<uint8_t> pkt = forgeChannelPacket({0xFF, 0xFF}, a.time);
             a.raw.client.send(pkt.data(), static_cast<int>(pkt.size()));
         },
         Expect::Kick, "pacote_invalido", Expect::Kick, "pacote_invalido"},
    };
    for (const Cell& c : cells) {
        runCell(c, false);
        runCell(c, true);
    }
    // Estado parado: conectou e nunca mandou hello.
    ServerArena idle;
    idle.start();
    idle.step(6 * 60);
    check(idle.raw.kickedFor() == "sem_hello", "conectou sem hello: devia cair com 'sem_hello'");
}

void clientMatrix() {
    std::printf("cliente: servidor hostil\n");
    struct Case {
        const char* name;
        std::vector<uint8_t> msg;
        int channel;
        const char* reason; // Failed (antes do welcome) com este motivo
    };
    std::vector<uint8_t> welcomeBadId;
    {
        std::vector<uint8_t> buf(16);
        BitWriter w(buf.data(), buf.size());
        w.writeInt(kWelcome, 0, 7);
        w.writeInt(0, 0, 16); // id 0 não é jogador
        w.writeInt(0, 0, 16);
        welcomeBadId = finish(w, buf);
    }
    std::vector<uint8_t> welcomeCut = systemMessage(kWelcome, {});
    const std::vector<Case> cases = {
        {"welcome com id 0", welcomeBadId, 0, "servidor_invalido"},
        {"welcome cortado", welcomeCut, 0, "servidor_invalido"},
        {"entrou antes do welcome", systemMessage(kJoined, bytes(4, 0)), 0, "servidor_invalido"},
        {"hello vindo do servidor", helloMessage(hashOf("x"), "y"), 0, "servidor_invalido"},
        {"evento antes do welcome", eventMessage("oi", {}), 1, "servidor_invalido"},
        {"tipo desconhecido", systemMessage(7, bytes(3, 0)), 0, "servidor_invalido"},
    };
    for (const Case& c : cases) {
        ClientArena a;
        if (!a.start()) {
            check(false, std::string(c.name) + ": não conectou");
            continue;
        }
        a.conn.sendReliable(c.channel, c.msg.data(), static_cast<int>(c.msg.size()));
        a.step(60);
        bool failed = false;
        for (const NetEvent& e : a.events) failed = failed || (e.type == NetEvent::Type::Failed && e.reason == c.reason);
        check(failed && a.client.role() == NetSession::Role::Offline,
              std::string("cliente, ") + c.name + ": devia falhar com '" + c.reason + "'");
    }
    // Recusa com motivo chega ao jogo como falha clara.
    ClientArena r;
    r.start();
    std::vector<uint8_t> buf(64);
    BitWriter w(buf.data(), buf.size());
    w.writeInt(kReject, 0, 7);
    w.writeString("cheio_de_razao", 32);
    const std::vector<uint8_t> reject = finish(w, buf);
    r.conn.sendReliable(0, reject.data(), static_cast<int>(reject.size()));
    r.step(60);
    bool got = false;
    for (const NetEvent& e : r.events) got = got || (e.type == NetEvent::Type::Failed && e.reason == "cheio_de_razao");
    check(got, "cliente: recusa do servidor devia virar on_fail com o motivo");
}

} // namespace

int main() {
    serverMatrix();
    clientMatrix();
    std::printf("%d casas conferidas\n", g_cells);
    if (g_failures) {
        std::printf("FALHOU: %d\n", g_failures);
        return 1;
    }
    std::printf("tudo certo\n");
    return 0;
}
