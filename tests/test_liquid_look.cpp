// Campos de aparencia de uma agua de contorno no .env (formats/LiquidLook).
//
// O que se verifica:
//   1. entrada sem campos nao muda nada (tudo "usa o menu")
//   2. cada campo chega ao valor certo
//   3. numeros fora da faixa sao presos a ela
//   4. tipo errado e cor incompleta sao ignorados, sem lancar
//
//   g++ -std=c++17 -I src -I <glm> -I <nlohmann>/include tests/test_liquid_look.cpp src/formats/LiquidLook.cpp
#include "formats/LiquidLook.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>

using namespace eruption;
using nlohmann::json;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FALHOU: %s (linha %d)\n", #c, __LINE__); g_fail = 1; } } while (0)

int main() {
    {
        const WaterLook lk = parseWaterLook(json::parse(R"({"name":"x","kind":"water"})"));
        CHECK(!lk.hasDeep && !lk.hasShallow && !lk.hasScatter);
        CHECK(lk.transparency < 0.0f && lk.refraction < 0.0f && lk.reflectivity < 0.0f);
        CHECK(lk.clarity < 0.0f && lk.shoreFoam < 0.0f && lk.caustics < 0.0f && lk.foam == -1);
    }
    {
        const WaterLook lk = parseWaterLook(json::parse(R"({
            "deepColor":[0.02,0.1,0.12],"shallowColor":[0.3,0.5,0.4],"scatterColor":[0.01,0.05,0.06],
            "transparency":0.6,"refraction":0.1,"reflectivity":0.7,"roughness":0.2,
            "waveAmplitude":0.5,"waveFrequency":1.5,"waveSpeed":2.0,
            "clarity":12.5,"shoreFoam":3.0,"caustics":1.2,"foam":false})"));
        CHECK(lk.hasDeep && lk.deep.y == 0.1f && lk.deep.z == 0.12f);
        CHECK(lk.hasShallow && lk.shallow.x == 0.3f);
        CHECK(lk.hasScatter && lk.scatter.z == 0.06f);
        CHECK(lk.transparency == 0.6f && lk.refraction == 0.1f);
        CHECK(lk.reflectivity == 0.7f && lk.roughness == 0.2f);
        CHECK(lk.waveAmplitude == 0.5f && lk.waveFrequency == 1.5f && lk.waveSpeed == 2.0f);
        CHECK(lk.clarity == 12.5f && lk.shoreFoam == 3.0f && lk.caustics == 1.2f);
        CHECK(lk.foam == 0);
        CHECK(parseWaterLook(json::parse(R"({"foam":true})")).foam == 1);
    }
    {
        const WaterLook lk = parseWaterLook(json::parse(R"({
            "transparency":5,"refraction":-1,"waveAmplitude":99,"waveFrequency":0,
            "clarity":0,"shoreFoam":100,"caustics":-3})"));
        CHECK(lk.transparency == 1.0f && lk.refraction == 0.0f && lk.waveAmplitude == 4.0f);
        CHECK(lk.waveFrequency == 0.01f && lk.clarity == 0.1f && lk.shoreFoam == 8.0f && lk.caustics == 0.0f);
    }
    {
        const WaterLook lk = parseWaterLook(json::parse(R"({
            "deepColor":[0.1,0.2],"shallowColor":"azul","scatterColor":[0.1,"x",0.3],
            "transparency":"alto","foam":1,"clarity":null})"));
        CHECK(!lk.hasDeep && !lk.hasShallow && !lk.hasScatter);
        CHECK(lk.transparency < 0.0f && lk.foam == -1 && lk.clarity < 0.0f);
    }
    std::printf(g_fail ? "FALHOU\n" : "OK\n");
    return g_fail;
}
