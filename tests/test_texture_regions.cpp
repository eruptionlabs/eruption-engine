// Regioes por textura (formats/TextureRegions): rasterizacao, mascara
// emissiva, horario, pincel/borracha, varinha e laco magnetico.
//
//   g++ -std=c++17 -O2 -I src tests/test_texture_regions.cpp src/formats/TextureRegions.cpp
#include "formats/TextureRegions.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace eruption::texregion;

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  FALHOU: %s\n", msg); g_fail = 1; } } while (0)

static float sum(const std::vector<float>& v) { float s = 0; for (float x : v) s += x; return s; }

int main() {
    const int W = 64, H = 64;
    // Retangulo: area = 25% da imagem.
    Region rect; rect.shape = Shape::Rect; rect.pts = {0.25f, 0.25f, 0.75f, 0.75f};
    std::vector<float> cov;
    rasterize(rect, W, H, cov);
    CHECK(std::fabs(sum(cov) - W * H * 0.25f) < W * 2.0f, "area do retangulo");
    // Poligono triangulo: metade do quadrado.
    Region tri; tri.shape = Shape::Poly; tri.pts = {0, 0, 1, 0, 0, 1};
    rasterize(tri, W, H, cov);
    CHECK(std::fabs(sum(cov) - W * H * 0.5f) < W * 2.0f, "area do triangulo");
    Region el; el.shape = Shape::Ellipse; el.pts = {0.5f, 0.5f, 0.25f, 0.25f};
    rasterize(el, W, H, cov);
    CHECK(std::fabs(sum(cov) - 3.14159f * 16 * 16) < 40.0f, "area da elipse");
    Region all; all.shape = Shape::All;
    rasterize(all, W, H, cov);
    CHECK(sum(cov) == W * H, "Ctrl+A cobre tudo");

    // Albedo: metade esquerda escura, direita clara.
    std::vector<uint8_t> img(W * H * 4, 255);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W / 2; ++x)
            for (int c = 0; c < 3; ++c) img[(y * W + x) * 4 + c] = 10;
    ImageRegions ir; ir.image = "x";
    Region win; win.shape = Shape::All; win.glow = Glow::Dark; win.color[0] = 1; win.color[1] = 1; win.color[2] = 1;
    ir.regions.push_back(win);
    std::vector<uint8_t> rgb;
    buildEmissive(img.data(), W, H, 4, ir, -1.0f, rgb);
    const int dark = rgb[(32 * W + 10) * 3], light = rgb[(32 * W + 54) * 3];
    CHECK(dark > 200, "pixel escuro brilha forte");
    CHECK(light < 60 && light > 10, "pixel claro fica no piso");
    ir.regions[0].glow = Glow::Light;
    buildEmissive(img.data(), W, H, 4, ir, -1.0f, rgb);
    CHECK(rgb[(32 * W + 54) * 3] > 200 && rgb[(32 * W + 10) * 3] < 10, "vidro de lampiao: claro brilha");
    ir.regions[0].glow = Glow::Dark;

    // Horario: 18h..6h atravessa a meia-noite.
    ir.regions[0].onHour = 18; ir.regions[0].offHour = 6;
    buildEmissive(img.data(), W, H, 4, ir, 12.0f, rgb);
    CHECK(rgb[(32 * W + 10) * 3] == 0, "apagada ao meio-dia");
    buildEmissive(img.data(), W, H, 4, ir, 23.0f, rgb);
    CHECK(rgb[(32 * W + 10) * 3] > 200, "acesa as 23h");
    buildEmissive(img.data(), W, H, 4, ir, 3.0f, rgb);
    CHECK(rgb[(32 * W + 10) * 3] > 200, "acesa as 3h");
    ir.enabled = false;
    buildEmissive(img.data(), W, H, 4, ir, -1.0f, rgb);
    CHECK(rgb[(32 * W + 10) * 3] == 0, "imagem desligada nao brilha");

    // Pincel e borracha.
    Region br; br.shape = Shape::Bitmap; br.bw = br.bh = 32; br.bitmap.assign(32 * 32, 0);
    strokeLine(br, 4, 16, 28, 16, 3, true);
    CHECK(br.bitmap[16 * 32 + 16] == 255 && br.bitmap[2 * 32 + 16] == 0, "pincel pinta so' no traco");
    strokeLine(br, 14, 16, 18, 16, 3, false);
    CHECK(br.bitmap[16 * 32 + 16] == 0 && br.bitmap[16 * 32 + 6] == 255, "borracha apaga so' o trecho");
    Region conv = rect; toBitmap(conv, 32, 32);
    CHECK(conv.shape == Shape::Bitmap && conv.bitmap[16 * 32 + 16] == 255 && conv.bitmap[0] == 0, "vetor vira bitmap");

    // Varinha: tolerancia separa as metades.
    Region wand; wandFill(img.data(), W, H, 4, 5, 5, 20, true, wand);
    CHECK(wand.bitmap[10 * W + 10] == 255 && wand.bitmap[10 * W + 50] == 0, "varinha para na borda");
    // Modo global: um quadrado escuro isolado dentro da area clara tambem entra;
    // no modo contiguo (clique na area clara) nao.
    std::vector<uint8_t> island(img);
    for (int y = 10; y < 14; ++y)
        for (int x = 50; x < 54; ++x)
            for (int c = 0; c < 3; ++c) island[(y * W + x) * 4 + c] = 10;
    Region contig; wandFill(island.data(), W, H, 4, 5, 5, 20, true, contig);
    Region global; wandFill(island.data(), W, H, 4, 5, 5, 20, false, global);
    CHECK(contig.bitmap[11 * W + 51] == 0, "contiguo nao pega a ilha escura");
    CHECK(global.bitmap[11 * W + 51] == 255 && global.bitmap[10 * W + 10] == 255, "global pega a ilha e a area");

    // Laco magnetico: borda vertical em x = 40/64; do topo ate' embaixo o
    // caminho deve ficar colado nela, nao seguir a reta pelo meio.
    std::vector<uint8_t> edge(W * H * 4, 255);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < 40; ++x)
            for (int c = 0; c < 3; ++c) edge[(y * W + x) * 4 + c] = 20;
    LiveWire lw;
    lw.setImage(edge.data(), W, H, 4, 512);
    lw.setSeed(40.5f / W, 2.5f / H);
    // Cursor um pouco fora da borda: o caminho tem de grudar nela.
    const std::vector<float> path = lw.pathTo(46.0f / W, 60.5f / H);
    CHECK(path.size() > 20, "caminho existe");
    float worst = 0;
    for (size_t i = 0; i + 1 < path.size(); i += 2)
        if (path[i + 1] * H > 8 && path[i + 1] * H < 52) worst = std::fmax(worst, std::fabs(path[i] * W - 40.0f));
    CHECK(worst < 2.5f, "laco gruda na borda");
    std::printf("  desvio maximo do laco: %.2f px\n", worst);

    std::printf(g_fail ? "FALHOU\n" : "OK\n");
    return g_fail;
}
