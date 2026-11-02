// Testes do leitor/escritor de bits da rede (src/net/Wire): ida e volta,
// faixas, quantização, limites do pacote e entrada hostil. Sem socket:
//   tools/run_tests.sh  (ou o alvo eruption-net-tests com ERUPTION_BUILD_TESTS)
#include "net/Wire.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
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

void testRoundTrip() {
    std::printf("ida e volta\n");
    uint8_t buf[256] = {};
    BitWriter w(buf, sizeof(buf));
    w.writeBits(5, 3);
    w.writeBool(true);
    w.writeBits(0xDEADBEEFu, 32);
    w.writeInt(-7, -10, 10);
    w.writeInt(1000, 1000, 1000); // faixa de um valor: zero bits
    w.writeFloat(3.25f);
    w.writeQuantized(1.234f, -10.0f, 10.0f, 0.01f);
    w.writeString("payload", 32);
    const uint8_t raw[3] = {1, 2, 3};
    w.writeBytes(raw, 3);
    w.writeBits(1, 1);
    const size_t size = w.finish();
    CHECK(w.ok());

    BitReader r(buf, size);
    CHECK(r.readBits(3) == 5);
    CHECK(r.readBool());
    CHECK(r.readBits(32) == 0xDEADBEEFu);
    CHECK(r.readInt(-10, 10) == -7);
    CHECK(r.readInt(1000, 1000) == 1000);
    CHECK(r.readFloat() == 3.25f);
    CHECK(std::fabs(r.readQuantized(-10.0f, 10.0f, 0.01f) - 1.23f) < 0.0051f);
    std::string s;
    CHECK(r.readString(s, 32) && s == "payload");
    uint8_t back[3] = {};
    CHECK(r.readBytes(back, 3) && back[0] == 1 && back[2] == 3);
    CHECK(r.readBits(1) == 1);
    CHECK(r.ok());
}

void testWriterOverflow() {
    std::printf("escrita além do buffer\n");
    uint8_t buf[2] = {};
    BitWriter w(buf, sizeof(buf));
    w.writeBits(0xFF, 8);
    w.writeBits(0xFF, 8);
    CHECK(w.ok());
    w.writeBits(1, 1);
    CHECK(!w.ok());

    uint8_t buf2[8] = {};
    BitWriter w2(buf2, sizeof(buf2));
    w2.writeInt(11, 0, 10); // fora da faixa
    CHECK(!w2.ok());
    BitWriter w3(buf2, sizeof(buf2));
    w3.writeString("longo demais", 4);
    CHECK(!w3.ok());
}

void testReaderHostile() {
    std::printf("leitura hostil\n");
    // Faixa 0..5 usa 3 bits; 7 é forjado.
    uint8_t buf[4] = {0x07, 0, 0, 0};
    BitReader r(buf, 1);
    CHECK(r.readInt(0, 5) == 0);
    CHECK(!r.ok());
    CHECK(r.readBits(1) == 0); // depois do erro, tudo zero

    // Ler além do fim.
    BitReader r2(buf, 1);
    r2.readBits(8);
    CHECK(r2.ok());
    r2.readBits(1);
    CHECK(!r2.ok());

    // NaN e infinito não passam.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float bad : {nan, inf}) {
        uint8_t fb[4] = {};
        BitWriter w(fb, 4);
        w.writeFloat(bad);
        BitReader rf(fb, w.finish());
        CHECK(rf.readFloat() == 0.0f);
        CHECK(!rf.ok());
    }

    // Comprimento de texto forjado maior que o pacote: falha sem alocar.
    uint8_t sb[2] = {0xFF, 0x00}; // 255 com maxLength 255
    BitReader rs(sb, 2);
    std::string s;
    CHECK(!rs.readString(s, 255));
    CHECK(s.empty());

    // Comprimento acima do máximo declarado.
    uint8_t sb2[16] = {9};
    BitReader rs2(sb2, 16);
    CHECK(!rs2.readString(s, 8));

    // Enchimento de alinhamento diferente de zero.
    uint8_t ab[2] = {0xFE, 0x00};
    BitReader ra(ab, 2);
    ra.readBits(1);
    ra.alignToByte();
    CHECK(!ra.ok());

    // Quantização forjada acima do número de passos.
    uint8_t qb[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    BitReader rq(qb, 4);
    rq.readQuantized(0.0f, 1.0f, 0.3f); // 4 passos, 3 bits: 7 é forjado
    CHECK(!rq.ok());

    // Buffer nulo.
    BitReader rn(nullptr, 100);
    rn.readBits(1);
    CHECK(!rn.ok());
}

void testQuantization() {
    std::printf("quantização\n");
    uint32_t steps = 0;
    CHECK(quantizedSteps(0.0f, 1.0f, 0.1f, steps) && steps == 10);
    CHECK(quantizedSteps(0.0f, 0.0f, 0.1f, steps) && steps == 0);
    CHECK(!quantizedSteps(1.0f, 0.0f, 0.1f, steps));
    CHECK(!quantizedSteps(0.0f, 1.0f, 0.0f, steps));
    CHECK(!quantizedSteps(0.0f, 1e30f, 1e-6f, steps));
    CHECK(bitsForRange(0) == 0 && bitsForRange(1) == 1 && bitsForRange(255) == 8 && bitsForRange(256) == 9);
    CHECK(bitsForRange(0xFFFFFFFFu) == 32);

    // Erro máximo de meio passo em toda a faixa, e bordas exatas.
    uint8_t buf[8];
    float worst = 0.0f;
    for (int i = 0; i <= 2000; ++i) {
        const float v = -512.0f + static_cast<float>(i) * 0.512f;
        BitWriter w(buf, sizeof(buf));
        w.writeQuantized(v, -512.0f, 512.0f, 1.0f / 128.0f);
        BitReader r(buf, w.finish());
        worst = std::max(worst, std::fabs(r.readQuantized(-512.0f, 512.0f, 1.0f / 128.0f) - v));
        CHECK(r.ok());
    }
    CHECK(worst <= 0.5f / 128.0f + 1e-5f);
}

void testRandomStress() {
    std::printf("estresse aleatório (100k campos)\n");
    std::mt19937 rng(1234);
    std::vector<uint8_t> buf(1 << 19); // 100k campos de até 32 bits cabem com folga
    struct Field {
        int kind;
        int32_t a, b, v;
        uint32_t bits;
    };
    std::vector<Field> fields;
    BitWriter w(buf.data(), buf.size());
    for (int i = 0; i < 100000 && w.ok(); ++i) {
        Field f{static_cast<int>(rng() % 2), 0, 0, 0, 0};
        if (f.kind == 0) {
            f.bits = 1 + rng() % 32;
            f.v = static_cast<int32_t>(rng() & (f.bits == 32 ? 0xFFFFFFFFu : ((1u << f.bits) - 1u)));
            w.writeBits(static_cast<uint32_t>(f.v), static_cast<int>(f.bits));
        } else {
            f.a = static_cast<int32_t>(rng() % 2001) - 1000;
            f.b = f.a + static_cast<int32_t>(rng() % 5000);
            f.v = f.a + static_cast<int32_t>(rng() % static_cast<uint32_t>(f.b - f.a + 1));
            w.writeInt(f.v, f.a, f.b);
        }
        fields.push_back(f);
    }
    CHECK(w.ok());
    BitReader r(buf.data(), w.finish());
    bool same = true;
    for (const Field& f : fields) {
        const int32_t got = f.kind == 0 ? static_cast<int32_t>(r.readBits(static_cast<int>(f.bits))) : r.readInt(f.a, f.b);
        same = same && got == f.v;
    }
    CHECK(same);
    CHECK(r.ok());
}

} // namespace

int main() {
    testRoundTrip();
    testWriterOverflow();
    testReaderHostile();
    testQuantization();
    testRandomStress();
    if (g_failures) {
        std::printf("%d falha(s)\n", g_failures);
        return 1;
    }
    std::printf("tudo certo\n");
    return 0;
}
