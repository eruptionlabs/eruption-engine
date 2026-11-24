// Teste de unidade: forca da sintese de PBR por textura (PbrSynthStrength).
#include "utils/PbrSynthStrength.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace eruption;
static int g_fail = 0;
static void check(const char* what, bool ok) {
    if (!ok) { std::printf("FALHOU: %s\n", what); g_fail = 1; }
}

// Imagem de lado `side` com ate' `colors` cores distintas (xorshift).
static std::vector<uint8_t> makeImage(int side, int colors) {
    std::vector<uint8_t> px(size_t(side) * side * 4, 255);
    uint32_t s = 12345u;
    auto next = [&] { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    std::vector<uint32_t> palette(colors);
    for (int i = 0; i < colors; ++i) palette[i] = 0x404040u + (next() % 0xB0B0B0u);
    for (size_t i = 0; i < size_t(side) * side; ++i) {
        const uint32_t c = palette[next() % colors];
        px[i * 4 + 0] = uint8_t(c >> 16);
        px[i * 4 + 1] = uint8_t(c >> 8);
        px[i * 4 + 2] = uint8_t(c);
    }
    return px;
}

int main() {
    // Pintada indexada 256 px: quase chapada. Foto 256 px: cheia.
    {
        auto painted = makeImage(256, 250);
        auto photo = makeImage(256, 60000);
        PbrSynthSignals a, b;
        const float sp = pbrAutoStrength(painted.data(), 256, 256, 4, &a);
        const float sf = pbrAutoStrength(photo.data(), 256, 256, 4, &b);
        check("pintada 256: forca baixa", sp > 0.0f && sp <= 0.11f);
        check("foto 256: forca cheia", sf >= 0.99f);
        check("cores contadas (pintada)", a.colors >= 200 && a.colors <= 250);
    }
    // Pintada em 1024 volta ao cheio; pintada minuscula zera.
    {
        auto big = makeImage(1024, 250);
        auto tiny = makeImage(32, 250);
        check("pintada 1024: cheia", pbrAutoStrength(big.data(), 1024, 1024, 4) >= 0.99f);
        check("pintada 32: zero", pbrAutoStrength(tiny.data(), 32, 32, 4) <= 0.001f);
    }
    // Preto de fundo nao conta como cor.
    {
        std::vector<uint8_t> blk(256 * 256 * 3, 0);
        PbrSynthSignals s;
        pbrAutoStrength(blk.data(), 256, 256, 3, &s);
        check("so' preto: 0 cores", s.colors == 0);
    }
    // Sidecar.
    {
        const PbrSidecar sc = parsePbrSidecar(R"({"strength":0.5,"materials":{"telha":0.0,"muro":1.5,"x":"a"}})");
        check("sidecar presente", sc.present);
        check("sidecar modelo", sc.strength == 0.5f);
        check("sidecar material", sc.forMaterial("telha") == 0.0f);
        check("sidecar clamp", sc.forMaterial("muro") == 1.0f);
        check("sidecar tipo errado ignorado", sc.materials.size() == 2);
        check("sidecar cai no modelo", sc.forMaterial("outro") == 0.5f);
        const PbrSidecar ws = parsePbrSidecar(R"({  "strength" : 0.5, "materials":{"telha":0, "muro":1.5} })");
        check("hash independe de espacos", ws.hash == sc.hash);
        const PbrSidecar other = parsePbrSidecar(R"({"strength":0.6})");
        check("hash muda com o valor", other.hash != sc.hash);
        check("json invalido = ausente", !parsePbrSidecar("{").present);
        check("sem chaves = automatico", parsePbrSidecar("{}").forMaterial("a") < 0.0f);
    }
    setPbrSynthesisStrength(2.0f);
    check("global limitado a 1", pbrSynthesisStrength() == 1.0f);
    setPbrSynthesisStrength(1.0f);
    if (!g_fail) std::printf("test_pbr_strength: OK\n");
    return g_fail;
}
