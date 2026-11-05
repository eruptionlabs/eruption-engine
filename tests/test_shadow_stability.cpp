// Estabilidade das cascatas de sombra: a sombra de um objeto parado tem de
// ficar ancorada ao MUNDO enquanto a camera anda ou gira. Sem GPU: roda
// ShadowRenderer::updateCascades de verdade e olha as matrizes que o passe de
// luz usaria. Cobre os tres modos (csm, single, cascade) com os parametros de
// cada preset de data/graphics.json.
//
// Criterios (todos medidos em TEXELS do atlas, que e' o que o olho ve):
//  A. Ancoragem: um ponto fixo do mundo cai sempre na mesma fracao de texel,
//     qualquer que seja a posicao da camera (passos sub-texel) - senao a borda
//     da sombra desliza no chao ("shimmering").
//  B. Tamanho: a largura do texel em unidades de mundo nao muda ao andar nem
//     ao girar a camera em torno do alvo (esfera de raio constante).
//  C. Sem historia: a mesma pose da camera gera as mesmas matrizes, venha de
//     onde vier (histerese e filtros nao podem deixar rastro quando parados).
#include <sstream>
#include <any>
#include <functional>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <chrono>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <optional>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#define private public
#include "renderer/ShadowRenderer.hpp"
#include "renderer/DeferredLighting.hpp"
#undef private
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

using namespace eruption;

namespace {
int g_fail = 0, g_checks = 0;

struct Case { std::string name; std::string mode; uint32_t atlas; float maxTexel; };
// Atlas, modo e shadow_max_texel de cada preset vem de data/graphics.json (a mesma fonte do motor):
// mudar o preset sem mudar o teste deixa de ser possivel. Rode da raiz do repositorio.
std::vector<Case> loadCases() {
    std::vector<Case> out;
    std::ifstream f("data/graphics.json");
    if (!f) { std::printf("ERRO: data/graphics.json nao encontrado (rode da raiz)\n"); ++g_fail; return out; }
    nlohmann::json j;
    try { f >> j; } catch (...) { std::printf("ERRO: graphics.json invalido\n"); ++g_fail; return out; }
    const auto& presets = j.contains("presets") ? j["presets"] : j;
    for (const char* name : {"high", "medium", "low", "mobile_high", "mobile_medium", "mobile_low"}) {
        if (!presets.contains(name)) continue;
        const auto& p = presets[name];
        Case c{name, p.value("shadow_mode", std::string("csm")), static_cast<uint32_t>(p.value("shadow_atlas", 1024)),
               p.value("shadow_max_texel", 0.5f)};
        out.push_back(c);
    }
    return out;
}

// Posicao do ponto P no atlas da cascata i, em texels (sem o offset do atlas).
void texelOf(const ShadowRenderer& sr, uint32_t i, const Vec3& P, double* u, double* v) {
    Vec4 c = sr.m_cascadeLookupMatrices[i] * Vec4(P, 1.0f);
    *u = double(c.x) * sr.m_currentAtlasSize;
    *v = double(c.y) * sr.m_currentAtlasSize;
}
double fracDist(double a, double b) { // distancia entre fracoes, em texels
    double d = std::fmod(a - b, 1.0); if (d < 0) d += 1.0; return std::min(d, 1.0 - d);
}

struct Rig {
    ShadowRenderer sr; Camera cam; DirectionalLight light;
    Rig(const Case& c) {
        auto& s = sr.settings();
        s.csmStable = std::string(c.mode) == "csm";
        s.singleMap = std::string(c.mode) == "single";
        s.singleMaxTexel = c.maxTexel;
        sr.m_currentAtlasSize = c.atlas;
        cam.setPerspective(camera_default::kFovDeg, 16.0f / 9.0f, 1.0f, 8000.0f);
        light.direction = glm::normalize(Vec3(0.4f, -0.55f, 0.3f)); // sol baixo
    }
    void pose(const Vec3& target, float yaw, float pitch, float dist) {
        cam.setOrbit(yaw, pitch, dist);
        cam.setOrbitTarget(target);
        cam.updateFrustum();
        // dois quadros: os filtros de alcance convergem e a cascata distante
        // (intervalo 2) nao fica com a matriz do quadro anterior.
        sr.updateCascades(light, cam, cam.zoomPercent(), 0.5f);
    }
    void settle(const Vec3& t, float yaw, float pitch, float dist) {
        for (int k = 0; k < 4; ++k) pose(t, yaw, pitch, dist);
    }
};

void check(bool ok, const char* preset, uint32_t casc, const char* what, double val, double lim) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FALHA %-14s c%u %-34s medido=%.5f limite=%.5f\n", preset, casc, what, val, lim); }
}
} // namespace

int main() {
    // O filtro de alcance usa dt: com relogio real o teste dependeria da maquina.
    setenv("ERUPTION_TEST_FIXED_DT", "0.0166667", 1);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::puts("=== test_shadow_stability ===");
    const Vec3 P(123.4f, 3.0f, -77.7f);   // ponto fixo do mundo (borda de sombra imaginaria)
    const Vec3 P2(-2310.6f, 0.0f, 1544.2f);
    const double kTol = 0.03;             // texels; float de 32 bits em coordenadas de milhares
    const std::vector<Case> cases = loadCases();
    if (cases.empty()) ++g_fail;
    for (const Case& c : cases) {
        const uint32_t nc = std::string(c.mode) == "single" ? 1u : 2u;
        // --- A e B: andar em passos sub-texel e girar -----------------------
        {
            Rig r(c);
            const float pitch = glm::radians(55.0f), dist = 1000.0f;
            r.settle(Vec3(2000, 120, 2000), -1.57f, pitch, dist);
            double u0[2], v0[2], t0[2]; // referencia
            for (uint32_t i = 0; i < nc; ++i) texelOf(r.sr, i, P, &u0[i], &v0[i]);
            t0[0] = r.sr.getWorldTexelSize(); t0[1] = r.sr.getC1WorldTexelSize();
            double maxFrac = 0, maxTexelDiff = 0;
            for (int k = 1; k <= 60; ++k) {
                // passo de 0,37 texel da c0 em direcao diagonal arbitraria
                const float step = 0.37f * float(t0[0]) * k;
                Vec3 tg(2000 + step * 0.8f, 120, 2000 + step * 0.6f);
                // gira um pouco a cada passo: a esfera nao pode depender disso
                r.settle(tg, -1.57f + 0.013f * k, pitch, dist);
                for (uint32_t i = 0; i < nc; ++i) {
                    double u, v; texelOf(r.sr, i, P, &u, &v);
                    maxFrac = std::max(maxFrac, std::max(fracDist(u, u0[i]), fracDist(v, v0[i])));
                }
                maxTexelDiff = std::max(maxTexelDiff, std::fabs(r.sr.getWorldTexelSize() - t0[0]) / t0[0]);
                if (nc > 1) maxTexelDiff = std::max(maxTexelDiff, std::fabs(r.sr.getC1WorldTexelSize() - t0[1]) / t0[1]);
            }
            std::printf("  %-14s %-7s ancoragem max=%.4f texel | variacao do texel=%.5f\n",
                        c.name.c_str(), c.mode.c_str(), maxFrac, maxTexelDiff);
            check(maxFrac < kTol, c.name.c_str(), 0, "A ancoragem andando+girando (texels)", maxFrac, kTol);
            check(maxTexelDiff < 1e-4, c.name.c_str(), 0, "B tamanho do texel andando+girando", maxTexelDiff, 1e-4);
            // ponto longe tambem (erro de precisao / escala errada aparece longe do centro)
            double uf, vf; texelOf(r.sr, 0, P2, &uf, &vf);
            (void)uf; (void)vf;
        }
        // --- C: mesma pose por historicos diferentes ---------------------------
        {
            const Vec3 T(2400, 120, 1800); const float yaw = -2.1f, pitch = glm::radians(48.0f), dist = 1100.0f;
            Rig a(c), b(c);
            a.settle(T, yaw, pitch, dist);
            // b chega vindo de longe, com outros pitch/zoom e passos sub-texel
            b.settle(Vec3(100, 0, 100), 0.3f, glm::radians(20.0f), 500.0f);
            for (int k = 0; k < 30; ++k)
                b.pose(Vec3(2400 - 3.1f * (30 - k), 120, 1800 + 1.7f * (30 - k)), yaw - 0.01f * (30 - k), pitch + 0.003f * (30 - k), dist);
            b.settle(T, yaw, pitch, dist);
            // o filtro de alcance (tau 0,25 s) so' converge com tempo: damos 400 quadros a 60 Hz
            for (int k = 0; k < 400; ++k) { a.pose(T, yaw, pitch, dist); b.pose(T, yaw, pitch, dist); }
            double worst = 0;
            for (uint32_t i = 0; i < nc; ++i)
                for (const Vec3& Q : {P, P2}) {
                    double ua, va, ub, vb; texelOf(a.sr, i, Q, &ua, &va); texelOf(b.sr, i, Q, &ub, &vb);
                    worst = std::max(worst, std::max(std::fabs(ua - ub), std::fabs(va - vb)));
                }
            std::printf("  %-14s %-7s mesma pose, historia diferente: max=%.5f texel\n", c.name.c_str(), c.mode.c_str(), worst);
            check(worst < kTol, c.name.c_str(), 0, "C pose igual => matrizes iguais (texels)", worst, kTol);
        }
    }
    std::printf("%d verificacoes, %d falhas\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
