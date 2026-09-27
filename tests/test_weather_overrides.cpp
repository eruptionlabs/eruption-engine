// Sobreposicao de aparencia por tipo de clima (data/weather_types.json).
//
// O ponto sensivel que este teste protege: a sobreposicao aplica-se POR ULTIMO,
// depois do envelope atmosferico. Se ela emitir campos que nao pretendia
// sobrepor - foi o que aconteceu na primeira versao do arquivo gerado, que
// trazia temperature_c=20 em todo tipo - ela ANULA o envelope silenciosamente
// e nevasca volta a 20 C. Por isso o teste confere tanto que o campo declarado
// muda quanto que os NAO declarados ficam intactos.
//
//     ./tools/run_tests.sh
#include "renderer/WeatherSystem.hpp"
#include <cstdio>
#include <fstream>
using namespace eruption;

int main() {
    int fails = 0;
    WeatherSystem base;
    base.climateTypes().loadFromFile("data/climate_types.json");
    base.applyType(WeatherType::Rainy, 1.0f); base.snapToTarget();
    const float rain0 = base.current().rainIntensity;
    const float temp0 = base.current().temperatureC;
    const float wind0 = base.current().windiness;
    printf("sem sobreposicao ... chuva=%.2f  temp=%.1f C  vento=%.2f\n", rain0, temp0, wind0);

    // Sobreposicao so' de chuva: nao pode mexer no resto.
    { std::ofstream f("/tmp/_ovr_test.json");
      f << R"({"types":{"rainy":{"rain_intensity":0.25}}})"; }
    WeatherSystem ovr;
    ovr.climateTypes().loadFromFile("data/climate_types.json");
    if (!ovr.loadTypeOverrides("/tmp/_ovr_test.json")) { printf("FALHOU: nao carregou\n"); return 1; }
    ovr.applyType(WeatherType::Rainy, 1.0f); ovr.snapToTarget();
    printf("com sobreposicao ... chuva=%.2f  temp=%.1f C  vento=%.2f\n",
           ovr.current().rainIntensity, ovr.current().temperatureC, ovr.current().windiness);

    if (ovr.current().rainIntensity > 0.26f || ovr.current().rainIntensity < 0.24f) {
        printf("  FALHOU: chuva nao foi sobreposta\n"); fails++; }
    if (ovr.current().temperatureC != temp0) {
        printf("  FALHOU: temperatura mudou sem ser declarada (%.1f -> %.1f)\n",
               temp0, ovr.current().temperatureC); fails++; }
    if (ovr.current().windiness != wind0) {
        printf("  FALHOU: vento mudou sem ser declarado (%.2f -> %.2f)\n",
               wind0, ovr.current().windiness); fails++; }

    // Tipo nao declarado no arquivo continua no padrao.
    WeatherSystem b2; b2.climateTypes().loadFromFile("data/climate_types.json");
    b2.applyType(WeatherType::Heatwave, 1.0f); b2.snapToTarget();
    ovr.applyType(WeatherType::Heatwave, 1.0f); ovr.snapToTarget();
    bool same = ovr.current().heatShimmer == b2.current().heatShimmer &&
                ovr.current().temperatureC == b2.current().temperatureC;
    printf("tipo nao declarado . %s\n", same ? "intocado (ok)" : "MUDOU (errado)");
    if (!same) fails++;

    // O arquivo real do repo tem que carregar.
    WeatherSystem real;
    real.climateTypes().loadFromFile("data/climate_types.json");
    printf("data/weather_types.json %s\n",
           real.loadTypeOverrides("data/weather_types.json") ? "carregou" : "FALHOU");
    if (!real.loadTypeOverrides("data/weather_types.json")) fails++;
    real.applyType(WeatherType::Blizzard, 1.0f); real.snapToTarget();
    printf("  blizzard apos arquivo real: temp=%.1f C vento=%.2f neve=%.2f\n",
           real.current().temperatureC, real.current().windiness, real.current().snowIntensity);
    if (real.current().temperatureC > -10.0f) {
        printf("  FALHOU: envelope de temperatura foi anulado pela sobreposicao\n"); fails++; }

    printf("\n%s (%d falha(s))\n", fails ? "REPROVADO" : "APROVADO", fails);
    return fails ? 1 : 0;
}
