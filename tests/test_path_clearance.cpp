// Folga de parede do caminho (core/PathClearance) numa grade sintetica:
// corredor de 5 celulas de largura com uma quina em L, celula de 10 u.
#include "core/PathClearance.hpp"

#include <cstdio>

using namespace eruption;

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", what);
    if (!ok) ++g_fail;
}

constexpr float kStep = 10.0f;

// Anda: faixa horizontal z em [0, 4] com x em [0, 20], e faixa vertical
// x em [16, 20] com z em [0, 20] (um L). Fora disso, parede.
bool walk(int x, int z) {
    const bool horiz = x >= 0 && x <= 20 && z >= 0 && z <= 4;
    const bool vert = x >= 16 && x <= 20 && z >= 0 && z <= 20;
    return horiz || vert;
}

} // namespace

int main() {
    std::printf("PathClearance\n");
    // Sem folga: so' a celula do ponto conta.
    check(pointHasClearance(0.0f, 0.0f, kStep, 0.0f, walk), "folga 0: celula da borda anda");
    check(!pointHasClearance(0.0f, -10.0f, kStep, 0.0f, walk), "folga 0: celula de fora nao anda");
    // Centro do corredor (z = 20, linha 2) fica a 25 u das paredes (linhas -1 e 5,
    // borda em z = -5 e z = 45 -> 25 u).
    check(pointHasClearance(50.0f, 20.0f, kStep, 24.0f, walk), "centro do corredor com folga 24");
    check(!pointHasClearance(50.0f, 20.0f, kStep, 26.0f, walk), "centro do corredor sem folga 26");
    // Encostado na parede de baixo (linha 0, borda a 5 u).
    check(pointHasClearance(50.0f, 0.0f, kStep, 4.0f, walk), "beira com folga 4");
    check(!pointHasClearance(50.0f, 0.0f, kStep, 6.0f, walk), "beira sem folga 6");
    // Quina de dentro do L: a celula (15, 5) e' parede; seu canto fica em
    // (155, 45). O ponto (160, 40) esta' a ~7 u dele em diagonal.
    check(!pointHasClearance(160.0f, 40.0f, kStep, 8.0f, walk), "quina de dentro bloqueia folga 8");
    check(pointHasClearance(160.0f, 40.0f, kStep, 7.0f, walk), "quina de dentro libera folga 7");
    // Trecho que corta a quina: de (100, 10) a (180, 60), todo em celulas que
    // andam e com as pontas a 15 u ou mais da parede, mas passa a meio u do
    // canto (155, 45).
    check(pointHasClearance(100.0f, 10.0f, kStep, 12.0f, walk) && pointHasClearance(180.0f, 60.0f, kStep, 12.0f, walk),
          "pontas do atalho livres com folga 12");
    check(segmentHasClearance(100.0f, 10.0f, 180.0f, 60.0f, kStep, 0.0f, walk), "atalho anda sem folga");
    check(!segmentHasClearance(100.0f, 10.0f, 180.0f, 60.0f, kStep, 12.0f, walk), "atalho pela quina reprovado com folga");
    check(segmentHasClearance(100.0f, 20.0f, 180.0f, 20.0f, kStep, 12.0f, walk), "trecho reto no corredor aprovado");
    check(segmentHasClearance(180.0f, 20.0f, 180.0f, 120.0f, kStep, 12.0f, walk), "trecho reto no braco aprovado");
    // O meio do trecho entre dois centros livres pode chegar mais perto da
    // parede que as pontas: amostrado a um quarto de celula, nao passa.
    check(!segmentHasClearance(150.0f, 40.0f, 170.0f, 60.0f, kStep, 6.0f, walk), "diagonal rente a quina reprovada");
    if (g_fail) std::printf("%d falha(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
