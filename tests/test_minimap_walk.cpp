// Minimapa por grade de andar: conversao grade -> cor e leitura da config.
#include "core/MinimapWalk.hpp"
#include <cstdio>
#include <fstream>

using namespace eruption;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int main() {
    uint8_t c[3] = {1, 2, 3};
    CHECK(parseHexColor("#d9c7a0", c) && c[0] == 0xd9 && c[1] == 0xc7 && c[2] == 0xa0);
    CHECK(parseHexColor("6B4A2B", c) && c[0] == 0x6b && c[2] == 0x2b);
    uint8_t keep[3] = {9, 9, 9};
    CHECK(!parseHexColor("#12345", keep) && !parseHexColor("#zzzzzz", keep) && keep[0] == 9);

    MinimapWalkConfig d;
    CHECK(!d.enabled); // padrao desligado
    CHECK(!MinimapWalkConfig::load("/nao/existe.json").enabled);

    // 4x4: metade esquerda andavel, direita bloqueada, ultima linha fora.
    auto probe = [](float u, float v) { return v > 0.75f ? 0 : (u < 0.5f ? 1 : 2); };
    std::vector<uint8_t> px;
    bakeWalkMinimap(d, 4, probe, px);
    CHECK(px.size() == 64);
    CHECK(px[0] == 0xd9 && px[1] == 0xc7 && px[2] == 0xa0 && px[3] == 255);
    const uint8_t* b = &px[(0 * 4 + 3) * 4];
    CHECK(b[0] == 0x6b && b[1] == 0x4a && b[2] == 0x2b && b[3] == 255);
    CHECK(px[(3 * 4) * 4 + 3] == 0); // fora do mapa: transparente

    MinimapWalkConfig e = d; e.edge = true;
    bakeWalkMinimap(e, 4, probe, px);
    CHECK(px[(0 * 4 + 2) * 4] == 0xd9 || px[(0 * 4 + 2) * 4] == 0x6b * 6 / 10); // x=2 encosta numa andavel
    CHECK(px[(0 * 4 + 2) * 4] == 0x6b * 6 / 10);
    CHECK(px[(0 * 4 + 3) * 4] == 0x6b); // x=3 nao encosta

    const char* path = "/tmp/_mmwalk_test.json";
    { std::ofstream f(path); f << R"({"minimap_walk_enabled":true,"minimap_walk_color":"#ffffff","minimap_block_color":"bad","minimap_walk_style":"edge"})"; }
    MinimapWalkConfig l = MinimapWalkConfig::load(path);
    CHECK(l.enabled && l.edge && l.walk[0] == 255 && l.block[0] == 0x6b);
    std::remove(path);
    if (!g_fail) std::printf("test_minimap_walk: OK\n");
    return g_fail;
}
