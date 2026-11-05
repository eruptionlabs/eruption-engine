// Visao de equipe (core/Vision): regra exata do visible(), raster com borda
// suave e meia-vida, janela que anda sem tremer, zonas block/reveal e custo.
#include "core/Vision.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace eruption;

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", what);
    if (!ok) ++g_fail;
}

// Valor (0..1) do texel que cobre o ponto de mundo (x, z).
float at(const Vision& v, float x, float z) {
    const float texel = v.windowSize() / static_cast<float>(v.texels());
    const int i = static_cast<int>(std::floor((x - v.origin().x) / texel));
    const int j = static_cast<int>(std::floor((z - v.origin().y) / texel));
    if (i < 0 || j < 0 || i >= static_cast<int>(v.texels()) || j >= static_cast<int>(v.texels())) return -1.0f;
    return v.image()[static_cast<size_t>(j) * v.texels() + i] / 255.0f;
}

} // namespace

int main() {
    std::printf("=== visao de equipe ===\n");
    Vision v;
    v.setWindow(1024.0f, 256);
    check(v.visible(Vec2(5000.0f, 0.0f)), "desligada: tudo visivel");

    v.setEnabled(true);
    v.setFade(0.0f);
    check(!v.visible(Vec2(0.0f)), "ligada e sem fonte: nada visivel");
    v.setSource("heroi", Vec2(0.0f), 40.0f, 1);
    check(v.visible(Vec2(39.0f, 0.0f)) && !v.visible(Vec2(41.0f, 0.0f)), "o raio e' a linha da visao");
    v.setSource("outra_equipe", Vec2(200.0f, 0.0f), 40.0f, 2);
    check(!v.visible(Vec2(200.0f, 0.0f)), "fonte de outra equipe nao conta");
    v.setTeam(2);
    check(v.visible(Vec2(200.0f, 0.0f)) && !v.visible(Vec2(0.0f)), "trocar de equipe troca as fontes");
    v.setTeam(1);
    check(v.removeSource("outra_equipe") && v.sourceCount() == 1 && v.visible(Vec2(0.0f)), "remover mantem as outras");
    v.setSource("heroi", Vec2(300.0f, 0.0f), 40.0f, 1);
    check(v.sourceCount() == 1 && v.visible(Vec2(300.0f, 0.0f)) && !v.visible(Vec2(0.0f)), "o mesmo id move a fonte");
    v.setSource("heroi", Vec2(0.0f), 40.0f, 1);

    v.raster(Vec2(0.0f), 1.0f / 60.0f);
    check(at(v, 0.0f, 0.0f) > 0.99f && at(v, 30.0f, 0.0f) > 0.99f, "dentro do disco: 1");
    check(at(v, 60.0f, 0.0f) == 0.0f, "fora do disco: 0");
    float in = 0.0f, out = 0.0f;
    for (float x = 0.0f; x < 80.0f; x += 0.25f) {
        const float a = at(v, x, 0.5f);
        if (a > 0.95f) in = x;
        if (a > 0.05f) out = x;
    }
    std::printf("    borda de %.1f u (de 95%% a 5%%) num raio de 40\n", out - in);
    check(out - in >= 0.04f * 40.0f, "borda >= 4% do raio");
    // Texels de 4 u: os centros mais perto do raio estao a 38 e 42.
    check(at(v, 38.0f, 0.5f) > 0.5f && at(v, 42.0f, 0.5f) < 0.5f, "o raio fica no meio da borda");

    // Janela andando 7 texels: o mundo nao muda, o valor no mesmo ponto tambem nao.
    v.setFade(0.15f);
    for (int i = 0; i < 120; ++i) v.raster(Vec2(0.0f), 1.0f / 60.0f);
    const float antes = at(v, 38.0f, 3.0f);
    v.raster(Vec2(7.0f * 4.0f, 0.0f), 1.0f / 60.0f);
    check(std::fabs(at(v, 38.0f, 3.0f) - antes) < 1.5f / 255.0f, "janela anda sem tremer a borda");

    // Meia-vida: uma fonte nova chega a ~50% em 0,15 s.
    v.setSource("sentinela", Vec2(150.0f, 150.0f), 30.0f, 1);
    for (int i = 0; i < 9; ++i) v.raster(Vec2(0.0f), 1.0f / 60.0f);
    const float meia = at(v, 150.0f, 150.0f);
    std::printf("    apos 0,15 s: %.2f\n", meia);
    check(meia > 0.4f && meia < 0.6f, "meia-vida de 0,15 s");

    // Zonas.
    v.setFade(0.0f);
    v.setZone("covil", Vec2(100.0f, 0.0f), 30.0f, false);
    v.setSource("heroi", Vec2(60.0f, 0.0f), 60.0f, 1);
    check(!v.visible(Vec2(100.0f, 0.0f)), "block: fonte de fora nao ve dentro");
    check(v.visible(Vec2(60.0f, 10.0f)), "block: fora da zona a fonte ve");
    v.raster(Vec2(0.0f), 1.0f / 60.0f);
    check(at(v, 100.0f, 0.0f) == 0.0f, "block: o raster tambem escurece dentro");
    v.setSource("heroi", Vec2(95.0f, 0.0f), 60.0f, 1);
    check(v.visible(Vec2(100.0f, 0.0f)) && v.visible(Vec2(140.0f, 0.0f)), "block: de dentro ve dentro e fora");
    v.raster(Vec2(0.0f), 1.0f / 60.0f);
    check(at(v, 100.0f, 0.0f) > 0.99f, "block: o raster de dentro clareia");
    v.setZone("covil", Vec2(400.0f, 0.0f), 30.0f, false);
    check(v.visible(Vec2(100.0f, 0.0f)), "mover a zona libera o lugar antigo");
    check(v.removeZone("covil") && v.zoneCount() == 0, "remover a zona");
    v.setZone("praca", Vec2(-300.0f, 0.0f), 50.0f, true);
    check(v.visible(Vec2(-300.0f, 0.0f)), "reveal: visivel sem fonte");
    v.raster(Vec2(0.0f), 1.0f / 60.0f);
    check(at(v, -300.0f, 0.0f) > 0.99f, "reveal: o raster clareia");

    // Minimapa: escuro no alfa, regra exata, nada fora do retangulo do mapa.
    {
        Vision m;
        m.setEnabled(true);
        m.setDim(0.35f);
        m.setSource("heroi", Vec2(0.0f), 40.0f, 1);
        std::vector<uint8_t> sombra;
        m.rasterShade(Vec2(-128.0f), 256.0f, 64, Vec4(-100.0f, -100.0f, 100.0f, 100.0f), sombra);
        auto px = [&](float x, float z) { return sombra[static_cast<size_t>((z + 128.0f) / 4.0f) * 64 + static_cast<size_t>((x + 128.0f) / 4.0f)]; };
        check(px(0.0f, 0.0f) == 0, "minimapa: na visao nao escurece");
        check(px(80.0f, 0.0f) == 166, "minimapa: fora escurece (1 - dim)");
        check(px(120.0f, 0.0f) == 0, "minimapa: fora do mapa fica transparente");
    }

    v.setDim(0.0f);
    check(v.dim() >= 0.05f, "dim nunca chega ao preto");
    v.reset();
    check(!v.enabled() && v.sourceCount() == 0 && v.zoneCount() == 0 && v.texels() == 256, "reset volta ao padrao e guarda a janela");

    // Custo: 200 fontes de raio 40 e 10 zonas na janela de 1024 u.
    Vision p;
    p.setEnabled(true);
    p.setWindow(1024.0f, 256);
    unsigned seed = 12345u;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
    for (int i = 0; i < 200; ++i)
        p.setSource("f" + std::to_string(i), Vec2(rnd() * 1024.0f - 512.0f, rnd() * 1024.0f - 512.0f), 40.0f, 1);
    for (int i = 0; i < 10; ++i)
        p.setZone("z" + std::to_string(i), Vec2(rnd() * 1024.0f - 512.0f, rnd() * 1024.0f - 512.0f), 30.0f, i % 2 == 0);
    for (int i = 0; i < 10; ++i) p.raster(Vec2(0.0f), 1.0f / 60.0f);
    // Melhor de 5 rodadas: a maquina carregada estica uma rodada, nao as 5.
    const int frames = 200;
    double ms = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) p.raster(Vec2(static_cast<float>(i % 3) * 4.0f, 0.0f), 1.0f / 60.0f);
        ms = std::min(ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames);
    }
    int vis = 0;
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 500; ++i) vis += p.visible(Vec2(rnd() * 2000.0f - 1000.0f, rnd() * 2000.0f - 1000.0f)) ? 1 : 0;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count();
    std::printf("    raster: %.3f ms por quadro; 500 visible(): %.1f us (%d visiveis)\n", ms, us, vis);
#ifdef __OPTIMIZE__
    // -O2: ~0,35 ms nesta maquina (200 fontes juntas numa janela, o pior caso).
    check(ms < 1.0, "raster de 200 fontes abaixo de 1 ms");
#else
    std::printf("    (sem otimizacao: o custo so' vale no build otimizado, ctest)\n");
#endif

    std::printf(g_fail ? "HOUVE FALHA\n" : "tudo certo\n");
    return g_fail ? 1 : 0;
}
