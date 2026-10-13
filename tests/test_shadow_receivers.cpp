// Recorte de caster por receptor (Frustum::sweptBoxOutside, usado pela sombra
// em ModelRenderer::renderShadow): um caster so' sai do atlas quando a sombra
// dele nao pode cair em nada que a camera ve'. A regra que importa e' a de
// nunca sumir sombra visivel, entao o teste confere contra forca bruta: para
// caixas e luzes sorteadas, se ALGUM ponto da caixa ou da sombra esticada
// cai dentro da visao, o caster tem de ficar.
//
//     ./tools/run_tests.sh

#include "math/Frustum.hpp"

#include <cmath>
#include <cstdio>
#include <random>

using namespace eruption;

static int g_fail = 0;
#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("  FALHOU: %s\n", msg);              \
            ++g_fail;                                        \
        }                                                    \
    } while (0)

static Frustum cameraFrustum() {
    // Camera em (0, 100, -300) olhando para a origem, como a do jogo.
    const Mat4 view = glm::lookAt(Vec3(0.0f, 100.0f, -300.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 1.0f, 2000.0f);
    Frustum f;
    f.extractFromMatrix(proj * view);
    return f;
}

int main() {
    const Frustum f = cameraFrustum();
    const Vec3 down = glm::normalize(Vec3(0.3f, -1.0f, 0.2f));

    std::printf("=== casos fixos ===\n");
    // Dentro da visao: fica.
    CHECK(!f.sweptBoxOutside(Vec3(-5.0f), Vec3(5.0f), down, 0.0f), "caixa no centro da visao");
    // Muito acima da visao, sol a pino: a sombra desce ate' o chao visivel.
    CHECK(!f.sweptBoxOutside(Vec3(-5.0f, 900.0f, -5.0f), Vec3(5.0f, 910.0f, 5.0f), Vec3(0.0f, -1.0f, 0.0f), 0.0f),
          "caixa acima da tela com sol a pino");
    // Atras da camera, luz indo para longe da cena: sai.
    CHECK(f.sweptBoxOutside(Vec3(-5.0f, 0.0f, -900.0f), Vec3(5.0f, 10.0f, -890.0f), Vec3(0.0f, -0.2f, -1.0f), 0.0f),
          "caixa atras da camera com a luz indo para tras");
    // Ao lado da visao, luz entrando nela: fica.
    CHECK(!f.sweptBoxOutside(Vec3(-900.0f, 0.0f, 0.0f), Vec3(-890.0f, 10.0f, 10.0f),
                             glm::normalize(Vec3(1.0f, -0.1f, 0.0f)), 0.0f),
          "caixa ao lado com a luz entrando na visao");
    // A mesma caixa com a luz saindo: sai.
    CHECK(f.sweptBoxOutside(Vec3(-900.0f, 0.0f, 0.0f), Vec3(-890.0f, 10.0f, 10.0f),
                            glm::normalize(Vec3(-1.0f, -0.1f, 0.0f)), 0.0f),
          "caixa ao lado com a luz saindo da visao");
    // Folga: fora por pouco, mas dentro da folga, fica.
    CHECK(!f.sweptBoxOutside(Vec3(-900.0f, 0.0f, 0.0f), Vec3(-890.0f, 10.0f, 10.0f),
                             glm::normalize(Vec3(-1.0f, -0.1f, 0.0f)), 5000.0f),
          "folga grande mantem o caster");

    std::printf("=== sorteio contra forca bruta ===\n");
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pos(-1500.0f, 1500.0f), size(1.0f, 120.0f), unit(-1.0f, 1.0f);
    int kept = 0, culled = 0, wrong = 0;
    for (int n = 0; n < 20000; ++n) {
        const Vec3 c(pos(rng), pos(rng) * 0.3f + 100.0f, pos(rng));
        const Vec3 h(size(rng), size(rng), size(rng));
        const Vec3 mn = c - h, mx = c + h;
        Vec3 d(unit(rng), -std::abs(unit(rng)) - 0.05f, unit(rng));
        d = glm::normalize(d);
        if (!f.sweptBoxOutside(mn, mx, d, 0.0f)) { ++kept; continue; }
        ++culled;
        // Descartado: nenhum ponto da caixa nem do arrasto pode estar dentro.
        bool inside = false;
        for (int s = 0; s < 400 && !inside; ++s) {
            const Vec3 t(float(s % 7) / 6.0f, float((s / 7) % 7) / 6.0f, float((s / 49) % 7) / 6.0f);
            const Vec3 p = mn + (mx - mn) * t;
            for (float k = 0.0f; k <= 6000.0f && !inside; k += 25.0f) inside = f.containsPoint(p + d * k);
        }
        if (inside) ++wrong;
    }
    std::printf("  %d mantidos, %d descartados, %d descartados com sombra visivel\n", kept, culled, wrong);
    CHECK(wrong == 0, "caster descartado com sombra dentro da visao");
    CHECK(culled > 1000, "o teste quase nunca descarta (sorteio inutil)");

    if (g_fail == 0) std::printf("OK\n");
    else std::printf("%d falha(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
