// Faixa por spline (formats/Ribbon): amostragem, consulta e malha.
//
// O que se verifica, e por que cada um importa:
//   1. o comprimento e o passo batem com a curva      — UV e parametro t
//                                                      seguem o arco, sem
//                                                      esticar nas curvas
//   2. a consulta acha o mais proximo de TODOS        — quem re-simula a
//      (contra forca bruta, inclusive em grampo)       fisica precisa da
//                                                      mesma resposta sempre
//   3. lado, borda e normal com o sinal certo         — "fora da faixa" e o
//                                                      empurrao de volta
//   4. a malha assenta no chao e a altura volta na    — quem anda nela pisa na
//      consulta                                        superficie desenhada
//   5. mesma entrada, mesmo hash                      — dois aparelhos conferem
//                                                      que montaram a mesma faixa
//   6. entrada ruim da' erro, nao faixa torta
//   7. custo da consulta                              — 16 objetos x varias
//                                                      re-simulacoes por quadro
//
//   g++ -std=c++17 -O2 -I src tests/test_ribbon.cpp src/formats/Ribbon.cpp
#include "formats/Ribbon.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace eruption;

static int g_falhas = 0;

static void checa(bool ok, const char* titulo) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", titulo);
    if (!ok) ++g_falhas;
}

static std::vector<float> circulo(float raio, int n) {
    std::vector<float> p;
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * M_PI * i / n;
        // Arredondado a 1/16: o cos da libm muda no ultimo bit entre
        // plataformas, e o hash tem de comparar so' a faixa.
        p.push_back(static_cast<float>(std::round(raio * std::cos(a) * 16.0) / 16.0));
        p.push_back(static_cast<float>(std::round(raio * std::sin(a) * 16.0) / 16.0));
    }
    return p;
}

// Forca bruta independente da grade: distancia ao segmento mais proximo.
static float maisPerto(const Ribbon& r, float x, float z, int& seg) {
    const auto& s = r.samples();
    const int n = static_cast<int>(s.size()), segs = r.closed() ? n : n - 1;
    float best = -1.0f;
    for (int i = 0; i < segs; ++i) {
        const RibbonSample &a = s[static_cast<size_t>(i)], &b = s[static_cast<size_t>((i + 1) % n)];
        const float ex = b.x - a.x, ez = b.z - a.z, l2 = ex * ex + ez * ez;
        float u = l2 > 0.0f ? ((x - a.x) * ex + (z - a.z) * ez) / l2 : 0.0f;
        u = std::fmin(1.0f, std::fmax(0.0f, u));
        const float dx = x - (a.x + ex * u), dz = z - (a.z + ez * u), d2 = dx * dx + dz * dz;
        if (best < 0.0f || d2 < best) {
            best = d2;
            seg = i;
        }
    }
    return best;
}

int main() {
    std::printf("=== faixa por spline ===\n");
    std::string erro;
    const float raio = 200.0f, largura = 40.0f;
    RibbonParams prm;
    prm.closed = true;
    prm.step = 4.0f;
    Ribbon anel;
    checa(anel.build(circulo(raio, 16), {largura}, prm, erro), "monta um anel de 16 pontos");
    const float perimetro = static_cast<float>(2.0 * M_PI * raio);
    checa(std::fabs(anel.length() - perimetro) / perimetro < 0.005f, "comprimento a menos de 0,5% do perimetro");
    {
        const auto& s = anel.samples();
        const float passo = anel.length() / static_cast<float>(s.size());
        float pior = 0.0f;
        for (size_t i = 0; i < s.size(); ++i) {
            const RibbonSample &a = s[i], &b = s[(i + 1) % s.size()];
            const float d = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
            pior = std::fmax(pior, std::fabs(d - passo) / passo);
        }
        std::printf("    %zu amostras, passo %.3f m, maior desvio %.3f%%\n", s.size(), passo, pior * 100.0f);
        checa(pior < 0.01f, "amostras a passo constante (desvio < 1%)");
    }

    // Lado: andando no sentido do angulo crescente, a direita e' para fora.
    RibbonQuery q = anel.query(raio + 5.0f, 0.0f);
    checa(q.ok && std::fabs(q.lateral - 5.0f) < 0.05f && std::fabs(q.edge - (largura * 0.5f - 5.0f)) < 0.05f,
          "5 m para fora do centro: lateral +5, borda a 15");
    checa(q.nx < -0.99f, "normal da borda aponta para dentro da faixa");
    q = anel.query(raio + 30.0f, 0.0f);
    checa(q.edge < 0.0f, "fora da faixa: borda negativa");
    q = anel.query(0.0f, raio);
    checa(std::fabs(q.t - 0.25f) < 0.002f, "um quarto de volta: t = 0,25");

    // Grampo: duas pernas a 30 m uma da outra, com largura 20 (celulas compartilhadas).
    Ribbon grampo;
    RibbonParams aberto;
    aberto.step = 2.0f;
    checa(grampo.build({0, 0, 0, 200, 30, 230, 60, 200, 60, 0}, {20.0f}, aberto, erro), "monta um grampo aberto");
    int difere = 0, total = 0;
    uint32_t semente = 12345u;
    auto aleatorio = [&](float a, float b) {
        semente = semente * 1664525u + 1013904223u;
        return a + (b - a) * static_cast<float>(semente >> 8) / 16777216.0f;
    };
    for (const Ribbon* r : {&anel, &grampo})
        for (int i = 0; i < 4000; ++i) {
            const float x = aleatorio(-300.0f, 300.0f), z = aleatorio(-300.0f, 300.0f);
            int seg = -1;
            const float d2 = maisPerto(*r, x, z, seg);
            const RibbonQuery rq = r->query(x, z);
            ++total;
            if (rq.segment == seg) continue;
            // Outro segmento so' vale se estiver exatamente tao perto (empate).
            const auto& s = r->samples();
            const int n = static_cast<int>(s.size());
            const RibbonSample &p = s[static_cast<size_t>(rq.segment)], &o = s[static_cast<size_t>((rq.segment + 1) % n)];
            const float ex = o.x - p.x, ez = o.z - p.z, l2 = ex * ex + ez * ez;
            float u = ((x - p.x) * ex + (z - p.z) * ez) / l2;
            u = std::fmin(1.0f, std::fmax(0.0f, u));
            const float dx = x - (p.x + ex * u), dz = z - (p.z + ez * u);
            if (dx * dx + dz * dz > d2) ++difere;
        }
    std::printf("    %d de %d pontos com segmento mais longe que a forca bruta\n", difere, total);
    checa(difere == 0, "consulta = forca bruta em 8000 pontos (anel e grampo)");
    bool igual = true;
    for (int i = 0; i < 200; ++i) {
        const float x = aleatorio(-50.0f, 100.0f), z = aleatorio(-50.0f, 250.0f);
        const RibbonQuery a = grampo.query(x, z), b = grampo.query(x, z);
        igual = igual && a.segment == b.segment && a.lateral == b.lateral && a.s == b.s;
    }
    checa(igual, "consulta pura: repetir da' os mesmos bits");

    // Malha num chao inclinado: altura = 0,1 x + 3, com folga de 0,2.
    std::vector<RibbonVertex> v;
    std::vector<uint32_t> idx;
    auto chao = [](float x, float) { return 0.1f * x + 3.0f; };
    anel.buildMesh(chao, 0.2f, v, idx);
    bool assenta = true;
    for (const RibbonVertex& p : v) assenta = assenta && std::fabs(p.py - (0.1f * p.px + 3.2f)) < 1e-3f;
    checa(assenta && !v.empty() && idx.size() % 3 == 0, "malha assentada no chao com a folga");
    bool normais = true;
    for (const RibbonVertex& p : v) normais = normais && p.ny > 0.99f && std::fabs(p.nx + 0.0995f) < 0.002f;
    checa(normais, "normal da malha segue o declive do chao");
    q = anel.query(raio - 7.0f, 13.0f);
    checa(std::fabs(q.height - (0.1f * (raio - 7.0f) + 3.2f)) < 0.05f, "a consulta devolve a altura da superficie");
    checa(v.back().v > v.front().v && std::fabs(v.back().v - anel.length() / prm.uvLength) < 1e-4f,
          "anel fechado: a ultima fileira repete a primeira com v = comprimento");

    RibbonQuery pt = anel.point(0.25f);
    checa(pt.ok && std::fabs(pt.x) < 0.5f && std::fabs(pt.z - raio) < 0.5f && pt.tx < -0.99f,
          "ponto em t = 0,25: no alto do anel, andando para -x");
    checa(std::fabs(pt.height - 3.2f) < 0.06f, "ponto em t devolve a altura do centro da faixa");
    const RibbonQuery volta = anel.point(1.25f);
    checa(volta.x == pt.x && volta.z == pt.z, "anel: t = 1,25 da' a volta e cai em 0,25");
    const RibbonQuery fim = grampo.point(2.0f);
    checa(std::fabs(fim.x - 60.0f) < 1e-3f && std::fabs(fim.z) < 1e-3f, "faixa aberta: t > 1 prende no fim");
    const RibbonQuery perto = anel.query(pt.x, pt.z);
    checa(std::fabs(perto.t - 0.25f) < 1e-3f, "consulta no ponto de t devolve o mesmo t");

    // Mesma entrada, mesmo hash.
    Ribbon outro;
    outro.build(circulo(raio, 16), {largura}, prm, erro);
    outro.buildMesh(chao, 0.2f, v, idx);
    std::printf("    hash do anel: %016llx\n", static_cast<unsigned long long>(anel.hash()));
    checa(outro.hash() == anel.hash(), "mesma entrada, mesmo hash");
    outro.build(circulo(raio + 0.001f, 16), {largura}, prm, erro);
    checa(outro.hash() != anel.hash(), "entrada diferente, hash diferente");

    Ribbon ruim;
    checa(!ruim.build({1, 1}, {10.0f}, aberto, erro), "um ponto so' e' recusado");
    checa(!ruim.build({0, 0, 0, 0, 0, 0}, {10.0f}, aberto, erro), "pontos repetidos nao viram faixa");
    checa(!ruim.build({0, 0, 10, 0}, {10.0f, 2.0f, 3.0f}, aberto, erro), "larguras em numero errado sao recusadas");
    checa(!ruim.build({0, 0, NAN, 0}, {10.0f}, aberto, erro), "ponto nao finito e' recusado");

    // Custo: pontos na faixa e perto dela, como objetos andando nela.
    std::vector<float> pts;
    for (int i = 0; i < 4096; ++i) {
        const float a = aleatorio(0.0f, 6.2831853f), d = aleatorio(-25.0f, 25.0f);
        pts.push_back((raio + d) * std::cos(a));
        pts.push_back((raio + d) * std::sin(a));
    }
    const int voltas = 200;
    float soma = 0.0f;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < voltas; ++k)
        for (size_t i = 0; i < pts.size(); i += 2) soma += anel.query(pts[i], pts[i + 1]).edge;
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
                      (voltas * (pts.size() / 2));
    std::printf("    %.0f ns por consulta (soma %.1f)\n", ns, static_cast<double>(soma));
    checa(ns < 1000.0, "consulta abaixo de 1 us");

    std::printf("%s\n", g_falhas ? "FALHOU" : "tudo certo");
    return g_falhas ? 1 : 0;
}
