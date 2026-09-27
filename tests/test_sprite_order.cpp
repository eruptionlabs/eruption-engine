// Verifica que a ordem de desenho de SpriteSystem::topologicalSort() e'
// IDENTICA antes e depois da reestruturacao Hot/Gpu (Ofensor #2 do relatorio
// de memoria: frustumCull/topologicalSort tocavam Sprite completo, 92B, num
// grafo de adjacencia que itera por bucket de hash espacial - ordem NAO
// sequencial, pessima pro prefetcher).
//
// DIFERENTE dos outros tres testes deste diretorio (test_surface_wetness,
// test_climate_fuzzy, test_weather_overrides): este NAO esta' em
// tools/run_tests.sh. SpriteSystem.cpp puxa VulkanContext/GLFW/X11 mesmo sem
// chamar init() (m_mappedInstances fica nullptr e uploadInstances() e' no-op
// pelo guard ja' existente, entao roda sem GPU nenhuma) - mas o link ainda
// precisa resolver esses simbolos, o que tornaria a suite rapida lenta.
// Fica aqui como registro da metodologia de verificacao usada no commit.
//
// Link manual (a partir da raiz do repo, com build/ ja' compilado):
//   OBJ=build/CMakeFiles/eruption-engine.dir/src
//   g++ -std=c++17 -DGLM_FORCE_DEPTH_ZERO_TO_ONE -I src -I include \
//       -I build/_deps/glm-src -I build/_deps/nlohmann_json-src/single_include \
//       -I build/_deps/vma-src/include \
//       tests/test_sprite_order.cpp \
//       "$OBJ/renderer/SpriteSystem.cpp.o" "$OBJ/math/Frustum.cpp.o" \
//       "$OBJ/renderer/VmaImplementation.cpp.o" "$OBJ/utils/Profiler.cpp.o" \
//       "$OBJ/renderer/VulkanContext.cpp.o" "$OBJ/core/Logger.cpp.o" \
//       build/_deps/glfw-build/src/libglfw3.a \
//       -lvulkan -ldl -lpthread -lX11 -lXrandr -lXi -lXcursor -lXinerama -lXxf86vm \
//       -o /tmp/test_sprite_order && /tmp/test_sprite_order
//
// Resultado medido: HASH_ORDEM identico (6365d69abecbbb46) entre o codigo
// anterior a este commit e o codigo apos, com 220 sprites sinteticos
// (sobreposicao garantida, n>100 para exercitar o ramo topologico, flags
// NoDepthSort misturadas).

// Verifica que a ordem de desenho (topologicalSort) e' IDENTICA antes e depois
// da reestruturacao Hot/Gpu (Ofensor #2). Nao depende de VMA/Vulkan: sem
// chamar init(), m_mappedInstances fica nullptr e uploadInstances() e'
// no-op (guard existente), entao processSprites() roda so' a parte de CPU
// (frustumCull + topologicalSort + buildBatches-stub).
#include "renderer/SpriteSystem.hpp"
#include "math/Frustum.hpp"
#include <cstdio>
#include <random>
#include <glm/gtc/matrix_transform.hpp>

using namespace eruption;

int main() {
    SpriteSystem sys; // sem init() - so' os campos CPU importam aqui

    // 220 sprites, posicoes e tamanhos que garantem MUITA sobreposicao (grade
    // apertada, sprites maiores que a celula) para exercitar o ramo
    // topologico (n > 100) e o teste isBehind() de verdade.
    std::mt19937 rng(42); // seed fixa: mesma entrada nas duas execucoes
    std::uniform_real_distribution<float> jitter(-0.3f, 0.3f);
    std::vector<Sprite> input;
    for (int i = 0; i < 220; ++i) {
        Sprite s;
        int gx = i % 15, gy = i / 15;
        s.position = Vec3(float(gx) * 1.5f + jitter(rng),
                          float(i) * 0.05f, // altura varia -> desempata profundidade
                          float(gy) * 1.5f + jitter(rng));
        s.size = Vec2(2.0f, 2.0f); // maior que o passo da grade -> sobreposicao garantida
        s.sortOrder = jitter(rng);
        s.flags = (i % 17 == 0) ? SpriteFlags::NoDepthSort : SpriteFlags::None;
        input.push_back(s);
    }
    sys.submitSprites(input);

    // Frustum enorme: todo mundo passa no cull.
    Mat4 proj = glm::ortho(-1000.0f, 1000.0f, -1000.0f, 1000.0f, -1000.0f, 1000.0f);
    Mat4 view = glm::lookAt(Vec3(0, 100, 0), Vec3(0, 0, 0), Vec3(0, 0, -1));
    Frustum f; f.extractFromMatrix(proj * view);

    sys.processSprites(f, Vec3(0, 100, 0), Vec3(0, -1, 0));

    const auto& visible = sys.getVisibleSprites();
    const auto& sorted = sys.getSortedSprites();
    printf("visiveis=%zu sorted=%zu\n", visible.size(), sorted.size());
    if (sorted.size() != visible.size()) { printf("FALHOU: tamanho diferente\n"); return 1; }

    // Imprime uma assinatura da ordem: posicao dos primeiros/ultimos 5 e um
    // hash simples de toda a sequencia de posicoes, para comparar entre as
    // duas execucoes (codigo antigo vs novo) sem depender de nenhuma
    // infraestrutura de diff externa.
    uint64_t hash = 1469598103934665603ull;
    for (const auto& s : sorted) {
        auto mix = [&](float v) {
            uint32_t bits; memcpy(&bits, &v, 4);
            hash ^= bits; hash *= 1099511628211ull;
        };
        mix(s.position.x); mix(s.position.y); mix(s.position.z);
    }
    printf("HASH_ORDEM=%llx\n", (unsigned long long)hash);
    for (int i = 0; i < 5; ++i)
        printf("  [%d] pos=(%.3f,%.3f,%.3f) flags=%u\n", i,
               sorted[i].position.x, sorted[i].position.y, sorted[i].position.z,
               (uint32_t)sorted[i].flags);
    printf("  ...\n");
    for (size_t i = sorted.size() - 5; i < sorted.size(); ++i)
        printf("  [%zu] pos=(%.3f,%.3f,%.3f) flags=%u\n", i,
               sorted[i].position.x, sorted[i].position.y, sorted[i].position.z,
               (uint32_t)sorted[i].flags);
    return 0;
}
