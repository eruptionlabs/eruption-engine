// Curva de molhar/secar do WeatherSystem (G4).
//
// Linka contra os .o ja' compilados da engine, entao testa o codigo REAL, nao
// uma reimplementacao. Construir e rodar:
//
//     ./tools/run_wetness_test.sh
//
// Este teste ja' pegou dois bugs que tinham passado por revisao:
//   - a secagem levava 15 s quando o comentario do codigo afirmava um minuto;
//   - o vento mudava o tempo de secagem em 2%, praticamente nada, quando ar
//     em movimento e' um dos termos mais fortes da evaporacao.
// Se voce mexer nas constantes de WeatherSystem::updateSurfaceWetness ou em
// WindField::evaporationFactor, rode isto antes de commitar.

#include "renderer/WeatherSystem.hpp"
#include <cstdio>
#include <cmath>

using namespace eruption;

// Roda dt fixo ate' a condicao, devolve segundos. -1 se nao convergir.
static float run(WeatherSystem& w, float dt, float limit, bool wantRise, float target) {
    float t = 0.0f;
    while (t < limit) {
        w.update(dt);
        t += dt;
        if (wantRise ? (w.surfaceWetness() >= target) : (w.surfaceWetness() <= target)) return t;
    }
    return -1.0f;
}

int main() {
    int fails = 0;
    const float dt = 1.0f / 60.0f;

    // --- molhar: chuva forte deve saturar em poucos segundos ---
    {
        WeatherSystem w;
        w.applyType(WeatherType::Rainy, 1.0f);
        w.snapToTarget();
        float t = run(w, dt, 120.0f, true, 0.60f);
        printf("molhar ate 0.60 .......... %.1f s (esperado < 15 s)\n", t);
        if (t < 0.0f || t > 15.0f) { printf("  FALHOU\n"); fails++; }
    }

    // --- secar a 20 C, sem vento: ordem de ~1 minuto ---
    {
        WeatherSystem w;
        w.applyType(WeatherType::Rainy, 1.0f);
        w.snapToTarget();
        run(w, dt, 60.0f, true, 0.90f);
        float wet0 = w.surfaceWetness();
        w.applyType(WeatherType::Clear, 1.0f);
        w.snapToTarget();
        float t = run(w, dt, 600.0f, false, 0.05f);
        printf("secar de %.2f ate 0.05 ... %.1f s (esperado 20..300 s)\n", wet0, t);
        if (t < 0.0f || t < 20.0f || t > 300.0f) { printf("  FALHOU\n"); fails++; }
    }

    // --- assimetria: secar tem que ser MUITO mais lento que molhar ---
    // Comparando a MESMA faixa (0.10 -> 0.60 e 0.60 -> 0.10). A versao anterior
    // comparava faixas diferentes e por isso era injusta com a secagem.
    {
        WeatherSystem a; a.applyType(WeatherType::Rainy, 1.0f); a.snapToTarget();
        run(a, dt, 60.0f, true, 0.10f);
        float tw = run(a, dt, 120.0f, true, 0.60f);
        WeatherSystem b; b.applyType(WeatherType::Rainy, 1.0f); b.snapToTarget();
        run(b, dt, 60.0f, true, 0.60f);
        b.applyType(WeatherType::Clear, 1.0f); b.snapToTarget();
        float td = run(b, dt, 900.0f, false, 0.10f);
        printf("assimetria ............... molhar %.1f s vs secar %.1f s (ratio %.1fx)\n",
               tw, td, (tw > 0 ? td / tw : -1.0f));
        if (tw <= 0 || td <= 0 || td < tw * 5.0f) { printf("  FALHOU: secagem nao e' lenta o bastante\n"); fails++; }
    }

    // --- congelado: agua nao evapora como liquido, o molhado tem que FICAR ---
    {
        WeatherSystem w;
        w.applyType(WeatherType::Rainy, 1.0f);
        w.snapToTarget();
        run(w, dt, 60.0f, true, 0.90f);
        w.applyType(WeatherType::Clear, 1.0f);
        w.snapToTarget();
        w.target().temperatureC = -10.0f;
        w.snapToTarget();
        float before = w.surfaceWetness();
        for (int i = 0; i < 60 * 120; ++i) w.update(dt); // 2 minutos
        printf("congelado (-10 C) ........ %.3f -> %.3f apos 120 s (deve segurar)\n",
               before, w.surfaceWetness());
        if (w.surfaceWetness() < before - 0.05f) { printf("  FALHOU: secou abaixo de zero grau\n"); fails++; }
    }

    // --- vento acelera a evaporacao ---
    {
        auto dryTime = [&](WeatherType after) {
            WeatherSystem w;
            w.applyType(WeatherType::Rainy, 1.0f); w.snapToTarget();
            run(w, dt, 60.0f, true, 0.90f);
            w.applyType(after, 1.0f); w.snapToTarget();
            return run(w, dt, 900.0f, false, 0.20f);
        };
        float calm = dryTime(WeatherType::Clear);
        float windy = dryTime(WeatherType::Sandstorm);
        printf("vento .................... calmo %.1f s vs ventando %.1f s (%.0f%% mais rapido)\n", calm, windy, (calm-windy)/calm*100.0f);
        if (calm <= 0 || windy <= 0 || windy > calm) { printf("  FALHOU: vento nao acelerou\n"); fails++; }
    }

    printf("\n%s (%d falha(s))\n", fails ? "REPROVADO" : "APROVADO", fails);
    return fails ? 1 : 0;
}
