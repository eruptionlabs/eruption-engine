// Pedacos de malha (utils/MeshClusters): a arvore cortada pela borda da tela
// desenha so' os pedacos que tocam a visao, entao um pedaco que perca
// triangulo, se sobreponha a outro ou tenha caixa menor que os triangulos
// dele faz geometria sumir. O teste confere as tres coisas em malhas
// sorteadas.
//
//     ./tools/run_tests.sh

#include "utils/MeshClusters.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>

using namespace eruption;

static int g_fail = 0;
#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("  FALHOU: %s\n", msg);              \
            ++g_fail;                                        \
        }                                                    \
    } while (0)

int main() {
    std::mt19937 rng(77);
    for (int round = 0; round < 20; ++round) {
        // Nuvem de vertices numa caixa achatada (copa de arvore) e triangulos
        // ligando vertices proximos, com tamanhos de 10 a 60 mil triangulos.
        std::uniform_real_distribution<float> ux(-400.0f, 400.0f), uy(0.0f, 900.0f), uz(-300.0f, 300.0f);
        const size_t verts = 2000 + size_t(round) * 1500;
        std::vector<Vec3> pos(verts);
        for (auto& p : pos) p = Vec3(ux(rng), uy(rng), uz(rng));
        std::uniform_int_distribution<uint32_t> uv(0, uint32_t(verts - 1));
        const size_t tris = 10000 + size_t(round) * 2500;
        std::vector<uint32_t> idx(tris * 3);
        for (auto& i : idx) i = uv(rng);
        const std::vector<uint32_t> before = idx;

        std::vector<MeshCluster> cl =
            buildMeshClusters(reinterpret_cast<const unsigned char*>(pos.data()), sizeof(Vec3), idx);

        // 1. Mesmos triangulos, so' em outra ordem.
        auto triKeys = [](const std::vector<uint32_t>& v) {
            std::vector<std::array<uint32_t, 3>> t(v.size() / 3);
            for (size_t i = 0; i < t.size(); ++i) t[i] = {v[i * 3], v[i * 3 + 1], v[i * 3 + 2]};
            std::sort(t.begin(), t.end());
            return t;
        };
        CHECK(idx.size() == before.size() && triKeys(idx) == triKeys(before), "triangulos mudaram");

        // 2. Pedacos contiguos, sem buraco nem sobreposicao, cobrindo tudo.
        uint32_t next = 0;
        bool contiguous = true;
        for (const MeshCluster& c : cl) {
            if (c.first != next || c.count == 0 || c.count % 3 != 0) contiguous = false;
            next = c.first + c.count;
        }
        CHECK(contiguous && next == idx.size(), "pedacos nao cobrem o buffer exatamente");

        // 3. A caixa de cada pedaco contem os triangulos dele.
        bool boxed = true;
        for (const MeshCluster& c : cl)
            for (uint32_t i = c.first; i < c.first + c.count; ++i) {
                const Vec3& p = pos[idx[i]];
                if (glm::any(glm::lessThan(p, c.mn)) || glm::any(glm::greaterThan(p, c.mx))) boxed = false;
            }
        CHECK(boxed, "triangulo fora da caixa do proprio pedaco");
        CHECK(cl.size() >= 4, "malha grande saiu num pedaco so'");
    }
    if (g_fail == 0) std::printf("OK\n");
    else std::printf("%d falha(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
