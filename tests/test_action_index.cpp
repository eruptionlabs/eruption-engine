// safeActionIndex: o indice da acao nunca passa do fim do vetor (SIGSEGV do HUD sobre a cabeca
// com sprite de PNG, que tem UMA acao so').
#include "game/ActionIndex.hpp"
#include <cstdio>

using eruption::safeActionIndex;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU: %s (linha %d)\n", #c, __LINE__); ++g_fail; } } while (0)

int main() {
    // sprite de PNG: uma acao so'
    for (int dir = 0; dir < 8; ++dir) CHECK(safeActionIndex(dir, 1) == 0);
    CHECK(safeActionIndex(4 * 8 + 5, 1) == 0);          // visao tatica num sprite de uma acao
    // sprite de ator com 8 direcoes
    for (int dir = 0; dir < 8; ++dir) CHECK(safeActionIndex(dir, 8) == dir);
    CHECK(safeActionIndex(4 * 8 + 3, 8) == 3);          // tatica sem as acoes extras: cai na direcao
    // sprite completo (>= 40 acoes): a tatica usa a propria
    CHECK(safeActionIndex(4 * 8 + 3, 48) == 35);
    // 3 acoes e direcao 5: nao passa do fim
    CHECK(safeActionIndex(5, 3) >= 0 && safeActionIndex(5, 3) < 3);
    // sem acoes ou indice negativo: nada
    CHECK(safeActionIndex(0, 0) == -1);
    CHECK(safeActionIndex(-1, 8) == -1);
    // varredura: nunca fora do vetor
    for (std::size_t n = 1; n <= 64; ++n)
        for (int w = 0; w < 80; ++w) {
            const int r = safeActionIndex(w, n);
            CHECK(r >= 0 && static_cast<std::size_t>(r) < n);
        }
    std::printf(g_fail ? "HOUVE FALHA\n" : "tudo certo\n");
    return g_fail ? 1 : 0;
}
