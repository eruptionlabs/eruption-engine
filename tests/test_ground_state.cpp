// Testes do estado do chao por celula (core/GroundState): ciclo de vida,
// perfil, rasterizacao e janela. Sem GPU.
#include "core/GroundState.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace eruption::ground;

static int g_fail = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            std::printf("FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                              \
        }                                                                          \
    } while (0)

static State make(Kind k, float r, float dur, float fade) {
    State s;
    s.kind = k;
    s.radius = r;
    s.duration = dur;
    s.fade = fade;
    return s;
}

int main() {
    Kind k;
    CHECK(kindFromName("scorch", k) && k == Kind::Scorch);
    CHECK(kindFromName("ice_water", k) && k == Kind::IceWater);
    CHECK(!kindFromName("lava", k));

    // Ciclo de vida: sobe, segura, some, acaba.
    State s = make(Kind::Scorch, 100.0f, 20.0f, 10.0f);
    s.born = 5.0;
    CHECK(lifeAmount(s, 5.0) < 0.05f);
    CHECK(lifeAmount(s, 6.0) > 0.99f);
    CHECK(lifeAmount(s, 14.0) > 0.99f);
    const float mid = lifeAmount(s, 20.0);
    CHECK(mid > 0.2f && mid < 0.8f);
    CHECK(lifeAmount(s, 25.0) <= 0.001f);
    CHECK(expired(s, 25.5));
    CHECK(!expired(s, 20.0));

    // Perfil: miolo cheio, borda zero, fora zero.
    CHECK(profile(s, 0.0f, 0.0f) > 0.99f);
    CHECK(profile(s, 100.0f, 0.0f) < 0.01f);
    CHECK(profile(s, 150.0f, 0.0f) == 0.0f);
    const float p70 = profile(s, 70.0f, 0.0f);
    CHECK(p70 > 0.0f && p70 < 1.0f);

    // Degelo: a geada deixa molhado enquanto some; o queimado nao.
    State f = make(Kind::Frost, 100.0f, 20.0f, 10.0f);
    CHECK(meltWet(f, 2.0) < 0.05f);
    CHECK(meltWet(f, 15.0) > 0.1f);
    CHECK(meltWet(s, 15.0) == 0.0f);

    // Campo: o centro de uma marca cheia chega a 255 em R; fora dela, 0.
    Field field;
    field.configure(Params{64, 8.0f});
    field.follow(0.0f, 0.0f);
    State a = make(Kind::Scorch, 100.0f, 30.0f, 10.0f);
    a.x = field.originX() + 256.0f;
    a.z = field.originZ() + 256.0f;
    a.born = 0.0;
    const uint32_t id = field.add(a, 7, [](float, float) { return 3.0f; }, 32);
    CHECK(id == 7);
    CHECK(field.size() == 1);
    const Rect r = field.rasterize(2.0);
    CHECK(r.valid());
    const uint32_t W = field.imageWidth() / 2;
    const uint32_t cx = 32, cz = 32;
    const uint8_t* px = field.pixels().data();
    CHECK(px[(cz * field.imageWidth() + cx) * 4 + 0] >= 250);
    CHECK(px[(cz * field.imageWidth() + cx) * 4 + 1] == 0);
    CHECK(px[(2 * field.imageWidth() + 2) * 4 + 0] == 0);
    (void)W;
    // A altura gravada na metade direita bate com o chao pedido.
    {
        const uint8_t* h = px + (cz * field.imageWidth() + cx + 64) * 4;
        const float v = (h[0] * 256.0f + h[1]) / 65535.0f * field.heightRange() + field.heightBase();
        CHECK(std::fabs(v - 3.0f) < 0.05f);
    }
    // Duas marcas: o mais forte vence por canal (nao soma passando de 255).
    State b = a;
    b.x += 8.0f;
    field.add(b, 8, [](float, float) { return 3.0f; }, 32);
    field.rasterize(2.0);
    CHECK(field.pixels()[(cz * field.imageWidth() + cx) * 4 + 0] >= 250);

    // Parar tira da imagem e devolve o retangulo a limpar.
    CHECK(field.stop(7));
    CHECK(!field.stop(7));
    CHECK(field.stop(8));
    const Rect clear = field.rasterize(2.5);
    CHECK(clear.valid());
    CHECK(field.pixels()[(cz * field.imageWidth() + cx) * 4 + 0] == 0);

    // O teto de estados: o mais velho sai.
    for (uint32_t i = 0; i < 5; ++i) {
        State t = a;
        t.born = double(i);
        field.add(t, 100 + i, [](float, float) { return 0.0f; }, 3);
    }
    CHECK(field.size() == 3);

    // Gelo sobre a agua entra na lista da agua e nao no vazio.
    field.clear();
    CHECK(field.size() == 0);
    State ice = make(Kind::IceWater, 100.0f, 30.0f, 10.0f);
    ice.x = 10.0f;
    ice.z = 20.0f;
    field.add(ice, 1, [](float, float) { return 0.0f; }, 8);
    float areas[kMaxIceAreas][4];
    CHECK(field.iceAreas(3.0, areas, kMaxIceAreas) == 1);
    CHECK(areas[0][0] == 10.0f && areas[0][1] == 20.0f && areas[0][3] > 0.9f);

    // Gelo que aguenta peso: sem gelo vivo nada levanta; congelado = 1 no miolo,
    // 0 fora do disco; histerese; afundar suave no degelo; walkable=false e strength baixa.
    {
        Field f;
        f.configure(Params{64, 8.0f});
        CHECK(!f.hasIce() && f.iceLift(0.0f, 0.0f) == 0.0f);
        State g = make(Kind::IceWater, 100.0f, 40.0f, 20.0f); // segura ate' 20 s, degela ate' 40
        g.born = 0.0;
        f.add(g, 1, [](float, float) { return 0.0f; }, 8);
        CHECK(f.hasIce());
        f.tick(0.0);
        CHECK(f.iceLift(0.0f, 0.0f) == 0.0f); // recem-criado: ainda nao solido
        f.tick(2.0);
        CHECK(f.iceLift(0.0f, 0.0f) == 1.0f);   // miolo congelado
        CHECK(f.iceLift(300.0f, 0.0f) == 0.0f); // fora do disco: agua
        // A borda: o perfil cai de 1 (0,45 r) a 0 (r); em 0,6 do perfil o gelo entra.
        float edgeIn = -1.0f, edgeOut = -1.0f;
        for (float d = 0.0f; d <= 100.0f; d += 0.5f) {
            const float v = lifeAmount(f.states()[0], 2.0) * profile(f.states()[0], d, 0.0f);
            if (v >= kIceSolidOff) edgeIn = d;
            if (v >= kIceSolidOn) edgeOut = d;
        }
        CHECK(edgeIn > edgeOut);                       // a solida (histerese) vai mais longe do que a de entrada
        CHECK(f.iceLift(edgeOut - 1.0f, 0.0f) == 1.0f);
        CHECK(f.iceLift(edgeIn + 3.0f, 0.0f) < 1.0f);  // alem da borda solida: nao sustenta
        // Histerese no tempo: um ponto na faixa entre kIceSolidOff e kIceSolidOn fica
        // congelado enquanto o estado esta' solido, mas nao entra num estado que nunca foi.
        const float dMid = 0.5f * (edgeIn + edgeOut);
        CHECK(f.iceLift(dMid, 0.0f) == 1.0f);
        Field h;
        h.configure(Params{64, 8.0f});
        State g2 = g;
        g2.strength = 0.5f; // nunca chega a kIceSolidOn
        h.add(g2, 2, [](float, float) { return 0.0f; }, 8);
        h.tick(2.0);
        CHECK(h.iceLift(0.0f, 0.0f) == 0.0f);
        // Degelo: cai de 1 a 0 sem salto, monotono, so' enquanto ha' gelo.
        float prev = 1.0f, worst = 0.0f;
        bool anySink = false;
        for (double t = 20.0; t <= 40.0; t += 0.05) {
            f.tick(t);
            const float l = f.iceLift(0.0f, 0.0f);
            CHECK(l <= prev + 1e-6f);
            worst = std::max(worst, prev - l);
            anySink |= l > 0.01f && l < 0.99f;
            prev = l;
        }
        CHECK(anySink);       // passa por valores intermediarios: afunda suave
        CHECK(worst < 0.1f);  // nenhum quadro de 0,05 s derruba mais de 10%
        CHECK(prev == 0.0f || f.size() == 0);
        // walkable = false: so' visual.
        Field v;
        v.configure(Params{64, 8.0f});
        State g3 = g;
        g3.walkable = false;
        v.add(g3, 3, [](float, float) { return 0.0f; }, 8);
        v.tick(2.0);
        CHECK(!v.hasIce() && v.iceLift(0.0f, 0.0f) == 0.0f);
    }

    // A janela anda com o foco, em passos.
    Field w;
    w.configure(Params{64, 8.0f});
    CHECK(w.follow(0.0f, 0.0f));
    const float ox = w.originX();
    CHECK(!w.follow(10.0f, 10.0f));
    CHECK(w.originX() == ox);
    CHECK(w.follow(5000.0f, 0.0f));
    CHECK(w.originX() > ox);

    if (g_fail == 0) std::printf("test_ground_state: ok\n");
    return g_fail == 0 ? 0 : 1;
}
