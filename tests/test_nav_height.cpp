// Alturas de canto da grade de navegacao: interpolacao bilinear, bordas e
// ausencia de altura (.nav v1 ou fora da grade).
#include "formats/NavGridTypes.hpp"
#include <cmath>
#include <cstdio>

using namespace eruption;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Valor guardado = -Y de mundo (mesma convencao do .ter).
static NavGridFile makeGrid(bool corners) {
    NavGridFile g;
    g.width = 2; g.height = 2; g.cornerHeights = corners;
    g.cells.resize(4);
    for (auto& c : g.cells) { for (float& h : c.height) h = 0.0f; c.type = TerrainType::Walkable; }
    // celula (0,0): rampa em x, de Y=0 (x=0) a Y=10 (x=1)
    g.cells[0].height[0] = 0.0f;   g.cells[0].height[1] = -10.0f;
    g.cells[0].height[2] = 0.0f;   g.cells[0].height[3] = -10.0f;
    // celula (1,1): cantos 0,10,20,30 em Y
    g.cells[3].height[0] = 0.0f;   g.cells[3].height[1] = -10.0f;
    g.cells[3].height[2] = -20.0f; g.cells[3].height[3] = -30.0f;
    return g;
}

int main() {
    float y = 123.0f;
    const NavGridFile g = makeGrid(true);

    CHECK(g.cornerHeightAt(0.0f, 0.0f, y) && near(y, 0.0f));
    CHECK(g.cornerHeightAt(0.5f, 0.5f, y) && near(y, 5.0f));
    CHECK(g.cornerHeightAt(0.25f, 0.9f, y) && near(y, 2.5f));
    CHECK(g.cornerHeightAt(1.0f - 1e-5f, 0.3f, y) && near(y, 10.0f)); // canto direito da celula 0
    // celula (1,1): bilinear dos 4 cantos
    CHECK(g.cornerHeightAt(1.0f, 1.0f, y) && near(y, 0.0f));
    CHECK(g.cornerHeightAt(1.5f, 1.5f, y) && near(y, 15.0f));
    CHECK(g.cornerHeightAt(1.5f, 1.0f, y) && near(y, 5.0f));
    CHECK(g.cornerHeightAt(1.0f, 1.5f, y) && near(y, 10.0f));
    // bordas: a borda final pertence a ultima celula
    CHECK(g.cornerHeightAt(2.0f, 2.0f, y) && near(y, 30.0f));
    CHECK(g.cornerHeightAt(2.0f, 1.0f, y) && near(y, 10.0f));
    // fora da grade: nil
    y = 123.0f;
    CHECK(!g.cornerHeightAt(-0.01f, 0.5f, y));
    CHECK(!g.cornerHeightAt(0.5f, -0.01f, y));
    CHECK(!g.cornerHeightAt(2.01f, 0.5f, y));
    CHECK(!g.cornerHeightAt(0.5f, 2.01f, y));
    CHECK(!g.cornerHeightAt(NAN, 0.5f, y));
    CHECK(near(y, 123.0f)); // saida intocada sem altura
    // mapa sem alturas de canto (.nav v1) ou grade vazia: nil
    CHECK(!makeGrid(false).cornerHeightAt(0.5f, 0.5f, y));
    CHECK(!NavGridFile{}.cornerHeightAt(0.0f, 0.0f, y));
    CHECK(kNavFileVersionCornerHeights == 2);

    if (!g_fail) std::printf("nav_height: ok\n");
    return g_fail;
}
