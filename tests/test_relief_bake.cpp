// Relevo assado (renderer/ReliefBake): mediana, subdivisao e deslocamento.
//
// O que se verifica, e por que cada um importa:
//   1. mediana de mapa constante e' o mesmo mapa; um ponto isolado some e um
//      degrau (paredao) fica no lugar               — o filtro escolhido por
//                                                      preservar cristas
//   2. nenhuma aresta passa do espacamento          — a grade do preset
//   3. toda aresta interna e' de 2 triangulos e a borda soma o perimetro
//                                                    — sem fenda nem T
//   4. vertice anda pela normal (h - 0,5) * amplitude — o mesmo da tesselacao
//   5. costura de UV (dois vertices no mesmo ponto) anda junta
//   6. o teto de triangulos e' respeitado
//   7. o registro refaz a malha (meios + deslocamento) e os filhos de cada
//      triangulo saem juntos                       — cache e celulas do chao
// Imprime o tempo de uma malha do tamanho do chao do mapa base de teste.
//
//   g++ -std=c++17 -I src tests/test_relief_bake.cpp src/renderer/ReliefBake.cpp
#include "renderer/ReliefBake.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

using namespace eruption;

static int g_falhas = 0;

static void checa(bool ok, const char* titulo) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", titulo);
    if (!ok) ++g_falhas;
}

static ReliefVertex vtx(float x, float z, float u, float v) {
    ReliefVertex r;
    r.pos[0] = x;
    r.pos[2] = z;
    r.uv[0] = u;
    r.uv[1] = v;
    return r;
}

// Quadrado plano de lado L em dois triangulos (CCW visto de cima).
static void quadrado(float L, std::vector<ReliefVertex>& vs, std::vector<uint32_t>& is) {
    vs = {vtx(0, 0, 0, 0), vtx(L, 0, 1, 0), vtx(L, L, 1, 1), vtx(0, L, 0, 1)};
    is = {0, 2, 1, 0, 3, 2};
}

static void testaMediana() {
    std::printf("mediana\n");
    std::vector<uint8_t> c(16 * 16, 77);
    checa(reliefMedian(c.data(), 16, 16, 2) == c, "mapa constante fica igual");
    std::vector<uint8_t> p(16 * 16, 100);
    p[5 * 16 + 5] = 255;
    checa(reliefMedian(p.data(), 16, 16, 1)[5 * 16 + 5] == 100, "ponto isolado some");
    std::vector<uint8_t> d(16 * 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) d[y * 16 + x] = x < 8 ? 20 : 220;
    const auto m = reliefMedian(d.data(), 16, 16, 2);
    checa(m[3 * 16 + 7] == 20 && m[3 * 16 + 8] == 220, "degrau fica no lugar");
}

static void testaFiltroGrade() {
    std::printf("filtro da grade\n");
    std::vector<uint8_t> c(16 * 16, 77);
    checa(reliefBoxBlur(c.data(), 16, 16, 3) == c, "mapa constante fica igual");
    // Listras de 1 texel (mais finas que a janela de 3) viram a media.
    std::vector<uint8_t> s(16 * 16);
    for (int i = 0; i < 16 * 16; ++i) s[i] = (i % 2) ? 200 : 0;
    const auto b = reliefBoxBlur(reliefBoxBlur(s.data(), 16, 16, 1).data(), 16, 16, 1);
    int pior = 0;
    for (uint8_t v : b) pior = std::max(pior, std::abs(int(v) - 100));
    checa(pior <= 12, "listra mais fina que a grade some");
    std::vector<uint8_t> r = {0, 255, 0, 255};
    checa(std::abs(reliefSampleBilinear(r, 2, 2, 0.5f, 0.25f) - 0.5f) < 0.01f, "bilinear no meio da aresta");
    checa(std::abs(reliefSampleBilinear(r, 2, 2, 0.25f, 0.25f)) < 0.01f, "bilinear no centro do texel");
}

static void testaSubdivisao() {
    std::printf("subdivisao\n");
    std::vector<ReliefVertex> vs;
    std::vector<uint32_t> is;
    quadrado(10.0f, vs, is);
    ReliefBakeParams p;
    p.spacing = 1.0f;
    const ReliefBakeStats st = bakeRelief(vs, is, p, nullptr);
    float maior = 0.0f;
    std::map<std::pair<uint32_t, uint32_t>, int> uso;
    for (size_t t = 0; t < is.size(); t += 3)
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = is[t + e], b = is[t + (e + 1) % 3];
            const float dx = vs[a].pos[0] - vs[b].pos[0], dz = vs[a].pos[2] - vs[b].pos[2];
            maior = std::max(maior, std::sqrt(dx * dx + dz * dz));
            ++uso[{std::min(a, b), std::max(a, b)}];
        }
    checa(maior <= 1.0f + 1e-4f, "nenhuma aresta passa de 1 u");
    bool par = true;
    float borda = 0.0f;
    for (const auto& [e, n] : uso) {
        if (n > 2) par = false;
        if (n == 1) {
            const float dx = vs[e.first].pos[0] - vs[e.second].pos[0], dz = vs[e.first].pos[2] - vs[e.second].pos[2];
            borda += std::sqrt(dx * dx + dz * dz);
        }
    }
    checa(par && std::fabs(borda - 40.0f) < 1e-3f, "sem fenda: borda = perimetro, interna em 2 triangulos");
    std::printf("    %zu -> %zu triangulos em %d rodadas\n", st.trianglesIn, st.trianglesOut, st.rounds);
}

static void testaDeslocamento() {
    std::printf("deslocamento\n");
    std::vector<ReliefVertex> vs;
    std::vector<uint32_t> is;
    quadrado(4.0f, vs, is);
    // Costura: o vertice 1 duplicado com outro UV, num triangulo a parte.
    vs.push_back(vtx(4, 0, 0.25f, 0.25f));
    vs.push_back(vtx(5, 0, 0.5f, 0.0f));
    vs.push_back(vtx(5, 1, 0.5f, 0.5f));
    is.insert(is.end(), {4, 6, 5});
    ReliefBakeParams p;
    p.spacing = 100.0f;
    p.amplitude = 8.0f;
    auto h = [](uint32_t, float u, float) { return u; }; // altura = u
    bakeRelief(vs, is, p, h);
    checa(std::fabs(vs[0].pos[1] - (0.0f - 0.5f) * 8.0f) < 1e-4f, "h = 0 desce meia amplitude");
    checa(std::fabs(vs[2].pos[1] - (1.0f - 0.5f) * 8.0f) < 1e-4f, "h = 1 sobe meia amplitude");
    checa(std::fabs(vs[1].pos[1] - vs[4].pos[1]) < 1e-5f, "costura de UV anda junta");
}

static void testaTeto() {
    std::printf("teto\n");
    std::vector<ReliefVertex> vs;
    std::vector<uint32_t> is;
    quadrado(100.0f, vs, is);
    ReliefBakeParams p;
    p.spacing = 0.1f;
    p.maxTriangles = 5000;
    const ReliefBakeStats st = bakeRelief(vs, is, p, nullptr);
    checa(st.capped && st.trianglesOut <= 5000, "para no teto");
}

static void testaRegistro() {
    std::printf("registro\n");
    std::vector<ReliefVertex> vs;
    std::vector<uint32_t> is;
    quadrado(6.0f, vs, is);
    const std::vector<ReliefVertex> orig = vs;
    ReliefBakeParams p;
    p.spacing = 1.0f;
    p.amplitude = 4.0f;
    auto h = [](uint32_t, float u, float v) { return 0.5f + 0.4f * std::sin(9.0f * u) * std::cos(7.0f * v); };
    ReliefBakeRecord rec;
    bakeRelief(vs, is, p, h, &rec);
    // Refaz: meios na ordem, depois o deslocamento.
    std::vector<ReliefVertex> re = orig;
    for (size_t i = 0; i * 2 < rec.parents.size(); ++i) {
        const ReliefVertex &a = re[rec.parents[2 * i]], &b = re[rec.parents[2 * i + 1]];
        ReliefVertex m = a;
        for (int k = 0; k < 3; ++k) m.pos[k] = 0.5f * (a.pos[k] + b.pos[k]);
        re.push_back(m);
    }
    float pior = 0.0f;
    for (size_t i = 0; i < re.size() && i < vs.size(); ++i)
        for (int k = 0; k < 3; ++k)
            pior = std::max(pior, std::fabs(re[i].pos[k] + rec.dir[3 * i + k] * rec.offset[i] - vs[i].pos[k]));
    checa(re.size() == vs.size() && pior < 1e-4f && rec.indices == is, "meios + deslocamento refazem a malha");
    uint32_t soma = 0;
    for (uint32_t c : rec.children) soma += c;
    checa(rec.children.size() == 2 && soma * 3 == is.size(), "filhos por triangulo somam a saida");
    // O 1o triangulo de entrada (0, 2, 1) fica abaixo da diagonal x = z: os
    // filhos dele, os primeiros children[0], tambem.
    bool juntos = true;
    for (uint32_t t = 0; t < rec.children[0]; ++t) {
        float cx = 0.0f, cz = 0.0f;
        for (int k = 0; k < 3; ++k) {
            cx += re[is[t * 3 + k]].pos[0];
            cz += re[is[t * 3 + k]].pos[2];
        }
        if (cz > cx + 1e-3f) juntos = false;
    }
    checa(juntos, "filhos de um triangulo saem juntos");
}

static void tempoChao() {
    // Grade de 2000 x 270 u com arestas de ~7 u (43 890 triangulos no chao do
    // mapa base de teste), partida ate' 2 u e deslocada por um mapa 1024.
    std::vector<ReliefVertex> vs;
    std::vector<uint32_t> is;
    const int nx = 286, nz = 39;
    for (int z = 0; z <= nz; ++z)
        for (int x = 0; x <= nx; ++x) vs.push_back(vtx(x * 7.0f, z * 7.0f, x * 0.17f, z * 0.17f));
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x) {
            const uint32_t a = uint32_t(z * (nx + 1) + x), b = a + 1, c = a + uint32_t(nx + 1), d = c + 1;
            is.insert(is.end(), {a, c, b, b, c, d});
        }
    std::vector<uint8_t> mapa(1024 * 1024);
    for (size_t i = 0; i < mapa.size(); ++i) mapa[i] = uint8_t((i * 2654435761u) >> 24);
    std::vector<uint8_t> med = reliefMedian(mapa.data(), 1024, 1024, 4);
    ReliefBakeParams p;
    p.spacing = 2.0f;
    p.amplitude = 12.0f;
    auto h = [&](uint32_t, float u, float v) { return reliefSampleNearest(med, 1024, 1024, u, v); };
    const ReliefBakeStats st = bakeRelief(vs, is, p, h);
    std::printf("tempo (chao de 2000 x 270 u, espacamento 2 u): %zu -> %zu triangulos, %zu vertices, %.0f ms\n",
                st.trianglesIn, st.trianglesOut, st.verticesOut, st.ms);
}

// Grade do chao assado: o desnivel e a altura de antes batem com as duas
// malhas, fora dela nao ha' amostra, e o que nao mexeu nao vira grade.
static void testaPiso() {
    std::vector<ReliefVertex> vs;
    std::vector<uint32_t> is;
    quadrado(40.0f, vs, is);
    for (ReliefVertex& v : vs) v.pos[1] = 0.1f * v.pos[0]; // rampa de 10%
    const std::vector<ReliefVertex> antes = vs;
    const std::vector<uint32_t> isAntes = is;
    ReliefBakeParams p;
    p.spacing = 2.0f;
    p.amplitude = 4.0f;
    // Altura em degraus suaves pelo UV: o assado sobe e desce.
    bakeRelief(vs, is, p, [](uint32_t, float u, float v) { return 0.5f + 0.4f * std::sin(9.0f * u) * std::cos(7.0f * v); });
    const ReliefPositions pa{antes.data(), antes.size(), sizeof(ReliefVertex)};
    const ReliefPositions pd{vs.data(), vs.size(), sizeof(ReliefVertex)};
    const ReliefFloorGrid g = reliefFloorGrid(pa, isAntes, pd, is, 0.5f);
    checa(!g.empty() && g.bytes() == size_t(g.nx) * size_t(g.nz) * 4, "piso: grade de 4 bytes por ponto");
    // Altura exata da malha assada em (x, z), triangulo a triangulo.
    auto topo = [&](float x, float z) {
        float best = -1e30f;
        for (size_t t = 0; t + 2 < is.size(); t += 3) {
            const float *a = vs[is[t]].pos, *b = vs[is[t + 1]].pos, *c = vs[is[t + 2]].pos;
            const float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(det) < 1e-9f) continue;
            const float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det;
            const float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det;
            const float l3 = 1.0f - l1 - l2;
            if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
            best = std::max(best, l1 * a[1] + l2 * b[1] + l3 * c[1]);
        }
        return best;
    };
    float pior = 0.0f, piorBase = 0.0f, maiorDesnivel = 0.0f;
    for (int i = 0; i < 400; ++i) {
        const float x = 1.0f + 38.0f * float((i * 37) % 400) / 400.0f, z = 1.0f + 38.0f * float(i) / 400.0f;
        float d = 0.0f, b = 0.0f;
        if (!g.sample(x, z, d, b)) {
            pior = 1e9f;
            continue;
        }
        pior = std::max(pior, std::fabs(b + d - topo(x, z)));
        piorBase = std::max(piorBase, std::fabs(b - 0.1f * x));
        maiorDesnivel = std::max(maiorDesnivel, std::fabs(d));
    }
    std::printf("  piso: desnivel ate' %.2f u, erro da grade %.3f u, base %.4f u\n", maiorDesnivel, pior, piorBase);
    checa(maiorDesnivel > 0.5f && pior < 0.1f, "piso: grade segue a malha assada (< 0,1 u)");
    checa(piorBase < 0.01f, "piso: altura de antes preservada");
    float d = 0.0f, b = 0.0f;
    checa(!g.sample(-5.0f, 10.0f, d, b) && !g.sample(10.0f, 60.0f, d, b), "piso: fora da malha nao amostra");
    checa(reliefFloorGrid(pa, isAntes, pa, isAntes, 0.5f).empty(), "piso: sem desnivel, sem grade");
}

int main() {
    testaMediana();
    testaFiltroGrade();
    testaSubdivisao();
    testaDeslocamento();
    testaTeto();
    testaRegistro();
    testaPiso();
    tempoChao();
    if (g_falhas) {
        std::printf("FALHAS: %d\n", g_falhas);
        return 1;
    }
    std::printf("RELEVO ASSADO: TUDO CERTO\n");
    return 0;
}
