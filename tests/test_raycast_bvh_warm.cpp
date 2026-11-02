// PISO-GRANDE: prova do pico da 1a consulta de piso numa malha grande (ex. uma
// montanha grande de terreno) e de que aquecer o RaycastBvh em
// segundo plano (fora do quadro que travaria) resolve sem mudar a resposta.
//
// "Antes": a malha so' ganha BVH na 1a vez que e' usada (ModelRenderer::
// raycastInstance), igual ao codigo original - aqui simulado chamando
// RaycastBvh::build() direto no momento da consulta.
// "Depois": a mesma construcao comeca numa thread em segundo plano (como
// warmRaycastBvh faz no fim do loadMap) e so' entra no mapa quando termina
// (pollRaycastBvhJobs); a consulta que chega bem depois (como a 1a consulta
// de piso do script, varios quadros apos o mapa carregar) so' espera o
// future se ele ainda estiver rodando - normalmente ja' esta pronta.
#include "renderer/RaycastBvh.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

using namespace eruption;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", what);
    if (!ok) ++g_fail;
}

// Malha sintetica do tamanho de uma montanha grande de terreno (dezenas de
// milhares de triangulos): uma grade NxN de quads, cada um com um pouco de
// relevo, parecido com um terreno escaneado de GLB.
void buildMountainMesh(std::vector<ModelVertex>& vertices, std::vector<uint32_t>& indices, int n) {
    vertices.reserve(static_cast<size_t>(n + 1) * (n + 1));
    for (int z = 0; z <= n; ++z) {
        for (int x = 0; x <= n; ++x) {
            ModelVertex v{};
            const float fx = static_cast<float>(x), fz = static_cast<float>(z);
            v.position = Vec3(fx * 2.0f, std::sin(fx * 0.05f) * std::cos(fz * 0.05f) * 50.0f, fz * 2.0f);
            vertices.push_back(v);
        }
    }
    indices.reserve(static_cast<size_t>(n) * n * 6);
    const auto idx = [n](int x, int z) { return static_cast<uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            indices.push_back(idx(x, z));
            indices.push_back(idx(x + 1, z));
            indices.push_back(idx(x + 1, z + 1));
            indices.push_back(idx(x, z));
            indices.push_back(idx(x + 1, z + 1));
            indices.push_back(idx(x, z + 1));
        }
    }
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main() {
    std::printf("RaycastBvhWarm\n");
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    buildMountainMesh(vertices, indices, 300); // 300x300 quads = 180000 triangulos
    const size_t triCount = indices.size() / 3;
    std::printf("  malha sintetica: %zu triangulos\n", triCount);

    const Vec3 origin(300.0f, 1000.0f, 300.0f), dir(0.0f, -1.0f, 0.0f);

    // ANTES (codigo original): a 1a consulta de piso constroi a BVH no meio
    // do quadro, sem aviso previo.
    double beforeMs = 0.0;
    float beforeT = -1.0f;
    {
        const auto t0 = std::chrono::steady_clock::now();
        RaycastBvh bvh;
        bvh.build(vertices, indices); // e' exatamente o que raycastInstance fazia na 1a chamada
        beforeT = bvh.raycastClosest(origin, dir);
        beforeMs = msSince(t0);
    }
    std::printf("  ANTES: 1a consulta = %.1f ms (t=%.2f)\n", beforeMs, beforeT);
    check(beforeMs > 50.0, "ANTES: 1a consulta e' cara (malha grande sem aquecimento)");

    // DEPOIS (warmRaycastBvh + pollRaycastBvhJobs): a construcao comeca numa
    // thread em segundo plano no fim do loadMap; o script so' chega na 1a
    // consulta de piso varios quadros depois (aqui, so' espera a thread
    // terminar por conta propria - sem instrumentar quadro).
    double afterMs = 0.0;
    float afterT = -1.0f;
    {
        auto vcopy = std::make_shared<std::vector<ModelVertex>>(vertices);
        auto icopy = std::make_shared<std::vector<uint32_t>>(indices);
        std::future<RaycastBvh> job = std::async(std::launch::async, [vcopy, icopy] {
            RaycastBvh bvh;
            bvh.build(*vcopy, *icopy);
            return bvh;
        });
        // Simula os quadros entre o fim do loadMap e a 1a consulta do script
        // (no jogo real, centenas de quadros; aqui, so' o suficiente pra
        // garantir que o aquecimento, que leva menos que o build isolado
        // acima, ja' tenha terminado numa maquina com mais de um nucleo).
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(beforeMs) + 50));
        const auto t0 = std::chrono::steady_clock::now();
        const bool ready = job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        RaycastBvh bvh = job.get(); // pronta: so' pega o resultado (pollRaycastBvhJobs ja' teria feito isto)
        afterT = bvh.raycastClosest(origin, dir);
        afterMs = msSince(t0);
        check(ready, "DEPOIS: aquecimento em segundo plano ja' tinha terminado antes da 1a consulta");
    }
    std::printf("  DEPOIS: 1a consulta = %.3f ms (t=%.2f)\n", afterMs, afterT);
    check(afterMs < 5.0, "DEPOIS: 1a consulta fora do quadro que aquece nao trava mais");
    check(beforeT == afterT, "mesma resposta de altura (ANTES == DEPOIS)");

    if (g_fail) std::printf("%d falha(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
