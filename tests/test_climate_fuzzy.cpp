// Valida os envelopes climaticos de data/climate_types.json.
// Linka contra os .o da engine, entao testa o codigo REAL.
//     ./tools/run_climate_test.sh
#include "renderer/ClimateFuzzy.hpp"
#include <cstdio>
#include <map>

using namespace eruption;

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "data/climate_types.json";
    ClimateFuzzySet set;
    if (!set.loadFromFile(path)) { printf("FALHOU: nao carregou %s\n", path); return 1; }

    int fails = 0;
    printf("\n--- envelopes: %zu ---\n", set.envelopes().size());

    // 1. Nucleos nao podem se sobrepor: dois climas 100% certos no mesmo estado.
    auto ov = set.findOverlaps();
    printf("nucleos sobrepostos ...... %zu\n", ov.size());
    for (const auto& o : ov)
        printf("    %s <-> %s\n", weatherTypeName(o.a), weatherTypeName(o.b));
    if (!ov.empty()) fails++;

    // 2. Buracos: estados que nenhum clima cobre.
    float gaps = set.coverageGaps(9);
    printf("espaco nao coberto ....... %.1f%%\n", gaps * 100.0f);
    if (gaps > 0.35f) { printf("    FALHOU: buraco demais\n"); fails++; }

    // 3. Escolher um clima na mao tem que devolver ele mesmo: o centroide do
    //    envelope precisa classificar de volta como o proprio tipo, senao
    //    clicar num clima levaria o mundo para outro.
    int mismatch = 0;
    for (const auto& e : set.envelopes()) {
        if (!e.enabled) continue;
        float m = 0.0f;
        WeatherType got = set.classify(e.centroid(), &m);
        if (got != e.type) {
            if (mismatch < 8)
                printf("    centroide de %-16s classificou como %-16s (pert %.2f)\n",
                       weatherTypeName(e.type), weatherTypeName(got), m);
            mismatch++;
        }
    }
    printf("centroide->proprio tipo .. %d divergencia(s)\n", mismatch);
    if (mismatch > 0) fails++;

    // 4. Continuidade: andar pelo espaco nao pode dar salto de pertinencia.
    //    E' isso que garante transicao suave em vez de troca em degrau.
    float worst = 0.0f;
    int flips = 0;
    WeatherSignature s;
    s.humidity = 0.8f; s.wind = 0.3f; s.turbulence = 0.4f;
    float prev = -1.0f;
    WeatherType prevT = WeatherType::Count;
    for (int i = 0; i <= 400; ++i) {
        s.thermalEnergy = -1.0f + 2.0f * float(i) / 400.0f;
        float m = 0.0f; WeatherType t = set.classify(s, &m);
        if (prev >= 0.0f) {
            float d = (m > prev) ? (m - prev) : (prev - m);
            if (d > worst) worst = d;
        }
        if (prevT != WeatherType::Count && t != prevT) flips++;
        prev = m; prevT = t;
    }
    printf("maior salto de confianca .. %.3f por passo (esperado < 0.05)\n", worst);
    if (worst > 0.05f) { printf("    FALHOU: confianca descontinua\n"); fails++; }
    // Varrer a temperatura de artico a tropical deve atravessar poucos climas,
    // nao oscilar entre eles.
    printf("trocas de clima na varredura %d (esperado <= 8)\n", flips);
    if (flips > 8) { printf("    FALHOU: clima oscilando\n"); fails++; }

    // 5. Sanidade fisica: estados conhecidos tem que cair no clima certo.
    struct Caso { const char* nome; float t, h, v, i; WeatherCategory esperada; };
    const Caso casos[] = {
        {"deserto quente e seco",   0.85f, 0.05f, 0.10f, 0.10f, WeatherCategory::Heat},
        {"tropical saturado inst.", 0.60f, 0.95f, 0.60f, 0.92f, WeatherCategory::Rain},
        {"artico umido ventando",  -0.85f, 0.90f, 0.90f, 0.65f, WeatherCategory::Snow},
        {"polar saturado parado",  -0.30f, 0.95f, 0.05f, 0.05f, WeatherCategory::Fog},
    };
    for (const auto& c : casos) {
        WeatherSignature q; q.thermalEnergy=c.t; q.humidity=c.h; q.wind=c.v; q.turbulence=c.i;
        float m=0.0f; WeatherType got = set.classify(q, &m);
        bool ok = weatherTypeCategory(got) == c.esperada;
        printf("%-26s -> %-16s %s\n", c.nome, weatherTypeName(got), ok ? "ok" : "INESPERADO");
        if (!ok) fails++;
    }

    // 6. Escolher um clima tem que MUDAR o estado atmosferico, nao so' a cara.
    //    Era o sintoma que motivou tudo isto: escolher nevasca nao esfriava o
    //    mundo e escolher onda de calor nao o esquentava.
    {
        printf("\n--- escolher clima muda o estado ---\n");
        struct Esp { WeatherType t; const char* nome; float tempMin, tempMax; float ventoMin; };
        const Esp esp[] = {
            {WeatherType::Heatwave, "onda de calor", 30.0f,  60.0f, 0.0f},
            {WeatherType::Blizzard, "nevasca",      -60.0f, -5.0f,  0.6f},
            {WeatherType::Foggy,    "neblina",      -20.0f, 20.0f,  0.0f},
            {WeatherType::Sandstorm,"areia",         30.0f,  60.0f, 0.6f},
        };
        for (const auto& x : esp) {
            const ClimateEnvelope* e = set.find(x.t);
            if (!e) { printf("  %-14s SEM ENVELOPE\n", x.nome); fails++; continue; }
            WeatherSignature c = e->centroid();
            float tempC = signatureTempToCelsius(c.thermalEnergy);
            bool okT = tempC >= x.tempMin && tempC <= x.tempMax;
            bool okV = c.wind >= x.ventoMin;
            printf("  %-14s %6.1f C  vento %.2f   %s\n", x.nome, tempC, c.wind,
                   (okT && okV) ? "ok" : "FORA DO ESPERADO");
            if (!okT || !okV) fails++;
        }
    }

    printf("\n%s (%d falha(s))\n", fails ? "REPROVADO" : "APROVADO", fails);
    return fails ? 1 : 0;
}
