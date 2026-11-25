// Testes do tornado (renderer/vfx/TornadoField, TornadoShape): ciclo de vida, fade,
// teto, curva do eixo/raio e caminho dos destrocos. Sem GPU.
#include "renderer/vfx/TornadoField.hpp"

#include <cmath>
#include <cstdio>

using namespace eruption;

static int g_fail = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            std::printf("FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                              \
        }                                                                          \
    } while (0)

int main() {
    TornadoField f;
    TornadoSpec s;
    s.pos = Vec3(10.0f, 5.0f, -3.0f);

    // Teto de tornados simultaneos.
    const uint32_t a = f.start(s, 2);
    const uint32_t b = f.start(s, 2);
    CHECK(a != 0 && b != 0 && a != b);
    CHECK(f.start(s, 2) == 0);
    CHECK(f.all().size() == 2);

    // Mover e parar.
    CHECK(f.move(a, Vec3(1.0f, 2.0f, 3.0f)));
    CHECK(f.all()[0].spec.pos.x == 1.0f);
    CHECK(!f.move(9999, Vec3(0.0f)));

    // A corda desce da nuvem: touchdown cresce de 0 a 1 em ~6 s.
    CHECK(TornadoField::touchdown(f.all()[0]) == 0.0f);
    for (int i = 0; i < 400; ++i) f.update(1.0f / 60.0f);
    CHECK(TornadoField::touchdown(f.all()[0]) > 0.99f);
    CHECK(TornadoField::intensity(f.all()[0]) == 1.0f);

    // Saida: a intensidade cai a 0 em `fade` s e o tornado sai da lista.
    CHECK(f.stop(a));
    for (int i = 0; i < 60; ++i) f.update(1.0f / 60.0f);
    const float mid = TornadoField::intensity(f.all()[0]);
    CHECK(mid > 0.0f && mid < 1.0f);
    for (int i = 0; i < 300; ++i) f.update(1.0f / 60.0f);
    CHECK(f.all().size() == 1);
    CHECK(!f.stop(a));

    // life: comeca a sair sozinho.
    TornadoSpec l = s;
    l.life = 1.0f;
    l.fade = 0.5f;
    const uint32_t c = f.start(l, 4);
    CHECK(c != 0);
    for (int i = 0; i < 100; ++i) f.update(1.0f / 60.0f);
    bool found = false;
    for (const auto& t : f.all()) found = found || t.id == c;
    CHECK(!found);

    // Forma: o eixo e' zero no chao, o raio cresce com a altura e e' continuo.
    tornado_shape::Shape sh;
    sh.seed = 1.3f;
    const Vec2 a0 = tornado_shape::axisOffset(sh, 0.0f, 12.0f);
    CHECK(std::fabs(a0.x) < 1e-5f && std::fabs(a0.y) < 1e-5f);
    float prev = tornado_shape::radiusAt(sh, 0.0f, 0.0f);
    CHECK(std::fabs(prev - sh.radiusBottom) < 0.05f * sh.radiusBottom);
    CHECK(tornado_shape::radiusAt(sh, 0.5f, 0.0f) > sh.radiusBottom);
    CHECK(tornado_shape::radiusAt(sh, 1.0f, 0.0f) > tornado_shape::radiusAt(sh, 0.5f, 0.0f));
    CHECK(tornado_shape::angularSpeed(1.0f, 0.0f, 0.2f) > tornado_shape::angularSpeed(1.0f, 1.0f, 1.0f));

    // Destrocos: nunca sao NaN, nascem perto do chao e ficam sob a base da nuvem + arremesso.
    TornadoField g;
    TornadoSpec d;
    d.pos = Vec3(0.0f, 0.0f, 0.0f);
    const uint32_t id = g.start(d);
    for (int i = 0; i < 600; ++i) g.update(1.0f / 60.0f);
    bool ok = true;
    for (uint32_t i = 0; i < 64; ++i) {
        const auto p = TornadoField::pieceAt(g.all()[0], i);
        ok = ok && std::isfinite(p.pos.x) && std::isfinite(p.pos.y) && std::isfinite(p.pos.z);
        ok = ok && p.pos.y < d.height * 1.1f && p.size > 0.0f && p.alpha >= 0.0f && p.alpha <= 1.0f;
    }
    CHECK(ok && id != 0);

    // Variacoes: redemoinho pequeno (sem nuvem, desce rapido) e vortice de neve (flocos e cristais).
    TornadoField w;
    TornadoSpec ws;
    ws.height = 22.0f; ws.radiusTop = 3.2f; ws.radiusBottom = 0.9f; ws.cloud = 0.0f;
    ws.grow = 0.5f; ws.life = 3.0f; ws.fade = 0.8f;
    ws.debrisKind = TornadoDebrisKind::Leaves;
    CHECK(w.start(ws, 8) != 0);
    CHECK(w.all()[0].spec.height == 22.0f && w.all()[0].spec.radiusBottom == 0.9f); // nao e' mais forcado a 10 / 0,5
    for (int i = 0; i < 40; ++i) w.update(1.0f / 60.0f);
    CHECK(TornadoField::touchdown(w.all()[0]) > 0.99f); // grow = 0,5 s
    for (int i = 0; i < 60 * 5; ++i) w.update(1.0f / 60.0f);
    CHECK(w.all().empty()); // life 3 s + fade 0,8 s
    for (int n = 0; n < 8; ++n) CHECK(w.start(ws, 8) != 0);
    CHECK(w.start(ws, 8) == 0); // teto de 8
    w.clear();
    TornadoSpec sn;
    sn.debrisKind = TornadoDebrisKind::Snow;
    w.start(sn, 8);
    for (int i = 0; i < 600; ++i) w.update(1.0f / 60.0f);
    bool snowKinds = true;
    for (uint32_t i = 0; i < 64; ++i) {
        const auto p = TornadoField::pieceAt(w.all()[0], i);
        snowKinds = snowKinds && (p.kind == 3 || p.kind == 4) && std::isfinite(p.pos.y);
    }
    CHECK(snowKinds);

    if (g_fail == 0) std::printf("OK tornado\n");
    return g_fail == 0 ? 0 : 1;
}
