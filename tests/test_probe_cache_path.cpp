// Onde a grade de sondas cozida fica (utils/ProbeCachePath).
//
// O que se verifica, e por que cada um importa:
//   1. padrao no PC = cache do usuario, nunca ao lado do .glb  - o motor nao escreve dentro dos
//                                                                 assets do projeto sem ser pedido
//   2. "beside" no projeto grava ao lado do .glb              - quem empacota continua achando o arquivo
//   3. ERUPTION_PROBE_CACHE_DIR manda em tudo                 - o dono da maquina escolhe a pasta
//   4. nome estavel por caminho, distinto entre mapas         - duas pastas com o mesmo mapa nao se pisam
//   5. sem pasta de usuario (Android, sem HOME) = ao lado     - o comportamento de sempre
//
//   g++ -std=c++17 -I src tests/test_probe_cache_path.cpp src/utils/ProbeCachePath.cpp
#include "utils/ProbeCachePath.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace eruption::probecache;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU: %s (linha %d)\n", #c, __LINE__); ++g_fail; } } while (0)

int main() {
#ifdef __ANDROID__
    std::printf("teste so' para desktop\n");
    return 0;
#else
    unsetenv("ERUPTION_PROBE_CACHE_DIR");
    setenv("XDG_CACHE_HOME", "/tmp/xdg_teste", 1);
    CHECK(userDir() == "/tmp/xdg_teste/eruption/probes");
    CHECK(writePolicy("") == WritePolicy::User);
    CHECK(writePolicy("beside") == WritePolicy::Beside);
    CHECK(writePolicy("qualquer") == WritePolicy::User);

    const std::string a = userFile("/jogo/assets/external/m1/m1.glb");
    const std::string b = userFile("/outro/assets/external/m1/m1.glb");
    CHECK(a.rfind("/tmp/xdg_teste/eruption/probes/m1.glb-", 0) == 0);
    CHECK(a.size() > 7 && a.compare(a.size() - 7, 7, ".probes") == 0);
    CHECK(a == userFile("/jogo/assets/external/m1/m1.glb"));
    CHECK(a != b);
    CHECK(besideFile("/jogo/m1.glb") == "/jogo/m1.glb.probes");

    unsetenv("XDG_CACHE_HOME");
    setenv("HOME", "/home/teste", 1);
    CHECK(userDir() == "/home/teste/.cache/eruption/probes");

    // A variavel vence a chave do projeto.
    setenv("ERUPTION_PROBE_CACHE_DIR", "/tmp/sondas", 1);
    CHECK(userDir() == "/tmp/sondas");
    CHECK(userFile("/jogo/m1.glb").rfind("/tmp/sondas/m1.glb-", 0) == 0);
    CHECK(writePolicy("beside") == WritePolicy::User);

    // Sem nenhuma pasta de usuario: ao lado do .glb.
    unsetenv("ERUPTION_PROBE_CACHE_DIR");
    unsetenv("HOME");
    CHECK(userDir().empty());
    CHECK(userFile("/jogo/m1.glb").empty());
    CHECK(writePolicy("") == WritePolicy::Beside);

    if (g_fail == 0) std::printf("test_probe_cache_path: tudo certo\n");
    return g_fail == 0 ? 0 : 1;
#endif
}
