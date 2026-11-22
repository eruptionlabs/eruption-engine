// Debug por spline (utils/SplineCurve.hpp): paleta fixa por indice de ponto,
// degrade entre pontos vizinhos, e a flag e' de cada spline.
#include "utils/SplineCurve.hpp"
#include <cstdio>

using namespace eruption;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU: %s (linha %d)\n", #c, __LINE__); ++g_fail; } } while (0)
static bool same(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-5f; }

int main() {
    // Ordem fixa: o ponto 0 e' azul e o 1 e' roxo.
    CHECK(SplineCurve::debugPointColor(0).b > 0.9f && SplineCurve::debugPointColor(0).r < 0.3f);
    CHECK(SplineCurve::debugPointColor(1).r > 0.5f && SplineCurve::debugPointColor(1).b > 0.9f);
    CHECK(SplineCurve::debugPointColor(2).r > 0.9f && SplineCurve::debugPointColor(2).g < 0.3f);
    // Volta ao azul na 9a; nenhum par vizinho igual; paleta sem repeticao interna.
    CHECK(same(SplineCurve::debugPointColor(8), SplineCurve::debugPointColor(0)));
    for (size_t i = 0; i < 200; ++i) CHECK(!same(SplineCurve::debugPointColor(i), SplineCurve::debugPointColor(i + 1)));
    for (size_t i = 0; i < 8; ++i)
        for (size_t j = i + 1; j < 8; ++j) CHECK(!same(SplineCurve::debugPointColor(i), SplineCurve::debugPointColor(j)));

    SplineCurve c({0.0f, 0.5f, 1.0f}, {1.0f, 4.0f, 2.0f});
    // Cada spline tem o seu debug.
    SplineCurve d({0.0f, 1.0f}, {1.0f, 1.0f});
    CHECK(!c.debug && !d.debug);
    c.debug = true;
    CHECK(c.debug && !d.debug);
    // Degrade: no ponto = a cor dele; no meio = a media dos dois; fora = a da ponta.
    CHECK(same(c.debugColorAt(0.0f), SplineCurve::debugPointColor(0)));
    CHECK(same(c.debugColorAt(0.5f), SplineCurve::debugPointColor(1)));
    CHECK(same(c.debugColorAt(0.25f), (SplineCurve::debugPointColor(0) + SplineCurve::debugPointColor(1)) * 0.5f));
    CHECK(same(c.debugColorAt(2.0f), SplineCurve::debugPointColor(2)));
    CHECK(c.debugMaxValue() == 4.0f);
    // O flag nao vai para o JSON.
    CHECK(c.toJson().dump().find("debug") == std::string::npos);
    if (g_fail == 0) std::printf("spline_debug OK\n");
    return g_fail ? 1 : 0;
}
