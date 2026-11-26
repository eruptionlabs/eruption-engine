// Testes do estado do chao sobre a vegetacao (core/GroundVegetation): a tabela
// data/ground_state.json, a mistura pela cobertura, a amostra da imagem do estado
// na raiz da planta e o degelo (o efeito some junto com a neve). Sem GPU.
// Uso: test_ground_vegetation [data/ground_state.json]
#include "core/GroundState.hpp"
#include "core/GroundVegetation.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace eruption::ground;

static int g_fail = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            std::printf("FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                              \
        }                                                                          \
    } while (0)

static std::string le(const char* path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool perto(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

int main(int argc, char** argv) {
    // Tabela vazia = neutra: nada muda sem o arquivo.
    VegetationTable vazia;
    CHECK(vazia.neutral());
    const float cheio[4] = {1, 1, 1, 1};
    const VegEffect n0 = vazia.blend(cheio);
    CHECK(perto(n0.ampScale, 1.0f) && perto(n0.freqScale, 1.0f) && perto(n0.densityBoost, 0.0f) && perto(n0.albedoLift, 0.0f));

    CHECK(vegChannelForName("scorch") == 0 && vegChannelForName("frost") == 1 && vegChannelForName("ice_water") == 1);
    CHECK(vegChannelForName("wet") == 2 && vegChannelForName("snow") == 3 && vegChannelForName("lava") == -1);

    // JSON invalido: recusa e mantem a tabela.
    VegetationTable t;
    CHECK(!t.loadFromJson("{ nao e json"));
    CHECK(t.neutral());

    // A tabela do motor: neve forte, geada media, molhado leve, queimado seco.
    const std::string real = le(argc > 1 ? argv[1] : "data/ground_state.json");
    CHECK(!real.empty() && t.loadFromJson(real));
    CHECK(!t.neutral());
    const VegEffect snow = t.channel(3), frost = t.channel(1), wet = t.channel(2), scorch = t.channel(0);
    CHECK(snow.ampScale >= 0.2f && snow.ampScale <= 0.3f);       // amplitude x0,2..0,3
    CHECK(snow.freqScale < frost.freqScale && frost.freqScale < wet.freqScale && wet.freqScale < 1.0f);
    CHECK(snow.ampScale < frost.ampScale && frost.ampScale < wet.ampScale && wet.ampScale < 1.0f);
    CHECK(snow.densityBoost > frost.densityBoost && frost.densityBoost > wet.densityBoost && wet.densityBoost > 0.0f);
    CHECK(scorch.ampScale >= 1.0f && scorch.freqScale > 1.0f && scorch.densityBoost < 0.0f);

    // Interpolacao pela cobertura: 0 = neutro, 1 = a linha da tabela, no meio entre os dois.
    float so_neve[4] = {0, 0, 0, 0};
    VegEffect e = t.blend(so_neve);
    CHECK(perto(e.ampScale, 1.0f) && perto(e.densityBoost, 0.0f));
    so_neve[3] = 1.0f;
    e = t.blend(so_neve);
    CHECK(perto(e.ampScale, snow.ampScale) && perto(e.freqScale, snow.freqScale) && perto(e.densityBoost, snow.densityBoost));
    so_neve[3] = 0.5f;
    e = t.blend(so_neve);
    CHECK(perto(e.ampScale, 0.5f * (1.0f + snow.ampScale)) && perto(e.densityBoost, 0.5f * snow.densityBoost));
    // Monotonico: mais cobertura, menos balanco e copa mais cheia.
    float prevAmp = 2.0f, prevDens = -1.0f;
    for (int i = 0; i <= 10; ++i) {
        so_neve[3] = float(i) / 10.0f;
        e = t.blend(so_neve);
        CHECK(e.ampScale <= prevAmp + 1e-6f && e.densityBoost >= prevDens - 1e-6f);
        prevAmp = e.ampScale;
        prevDens = e.densityBoost;
    }
    // Neve e molhado juntos: os fatores multiplicam.
    const float junto[4] = {0, 0, 1, 1};
    e = t.blend(junto);
    CHECK(perto(e.ampScale, snow.ampScale * wet.ampScale));
    // Cobertura fora de 0..1 e' limitada.
    const float fora[4] = {0, 0, 0, 7.0f};
    CHECK(perto(t.blend(fora).ampScale, snow.ampScale));

    // O projeto sobrepoe so' o que declara; "ice_water" vale se nao ha' "frost".
    VegetationTable p = t;
    CHECK(p.loadFromJson(R"({"vegetation":{"snow":{"sway_amp_scale":0.5}}})"));
    CHECK(perto(p.channel(3).ampScale, 0.5f) && perto(p.channel(3).freqScale, snow.freqScale));
    VegetationTable gelo;
    CHECK(gelo.loadFromJson(R"({"vegetation":{"ice_water":{"sway_amp_scale":0.1}}})"));
    CHECK(perto(gelo.channel(1).ampScale, 0.1f));

    // Da imagem do estado ate' o efeito: neve num disco, a copa dentro balanca menos que a de fora.
    Field field;
    field.configure(Params{64, 1.0f});
    field.follow(0.0f, 0.0f);
    float c[4];
    field.coverAt(0.0f, 0.0f, c);
    CHECK(c[0] == 0.0f && c[3] == 0.0f);
    State s;
    s.kind = Kind::Snow;
    s.x = 0.0f;
    s.z = 0.0f;
    s.radius = 10.0f;
    s.duration = 100.0f;
    s.fade = 60.0f;
    s.born = 0.0;
    field.add(s, 1, [](float, float) { return 0.0f; }, 8);
    field.rasterize(5.0); // regime
    field.coverAt(0.0f, 0.0f, c);
    CHECK(c[3] > 0.9f);
    const VegEffect dentro = t.blend(c);
    CHECK(dentro.ampScale < 0.35f && dentro.densityBoost > 0.4f);
    field.coverAt(25.0f, 25.0f, c); // fora do disco
    CHECK(c[3] == 0.0f);
    CHECK(perto(t.blend(c).ampScale, 1.0f));
    field.coverAt(1e9f, -1e9f, c); // fora da janela
    CHECK(c[3] == 0.0f);

    // Degelo: o fade do estado devolve o balanco aos poucos, sem salto.
    float antes = dentro.ampScale;
    bool voltou = false;
    for (double now = 40.0; now <= 100.0; now += 5.0) {
        field.rasterize(now);
        field.coverAt(0.0f, 0.0f, c);
        const VegEffect d = t.blend(c);
        CHECK(d.ampScale >= antes - 1e-3f); // so' sobe
        CHECK(d.ampScale - antes < 0.25f);  // sem degrau
        antes = d.ampScale;
        if (now >= 100.0) voltou = true;
    }
    CHECK(voltou && antes > 0.9f);

    if (g_fail == 0) std::printf("OK: vegetacao sob estado do chao\n");
    return g_fail == 0 ? 0 : 1;
}
