// Stream de vertice da sombra (spec 52, OE-03): o empacotamento copia os MESMOS
// bits do vertice inteiro (sombra igual) nos offsets que as pipelines leem.
//
//   1. posicao, UV, texIndex e matId bit a bit iguais, vertice a vertice
//   2. o resto do vertice (normal, cor, splat, indices) nao vaza para o stream
//   3. passos e offsets batem com os formatos das entradas (12 + 12 B)
//
//   g++ -std=c++17 -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_LEFT_HANDED -I src
//       -I build/_deps/glm-src tests/test_shadow_stream.cpp
#include "renderer/ShadowStreamPack.hpp"

#include <cstdio>
#include <vector>

using namespace eruption;

static int g_falhas = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("  FALHA linha %d: %s\n", __LINE__, #cond);                  \
            ++g_falhas;                                                              \
        }                                                                            \
    } while (0)

int main() {
    std::printf("1. bits da posicao, UV e indices\n");
    std::vector<TerrainVertex> v(1000);
    for (size_t i = 0; i < v.size(); ++i) {
        const float f = static_cast<float>(i);
        v[i].position = Vec3(f * 1.25f - 2000.0f, 1e-7f * f, 3.0e4f - f); // inclui subnormal-ish e grande
        v[i].texCoord = Vec2(f / 7.0f, -f / 3.0f);
        v[i].normal = Vec3(0.0f, 1.0f, 0.0f);
        v[i].color = 0xA5A5A5A5u;
        v[i].texIndex = static_cast<uint16_t>(i * 13);
        v[i].matId = static_cast<uint16_t>(65535 - i);
        v[i].pbrIndex = 0x7777;
    }
    std::vector<uint8_t> pos(v.size() * svs::kPosStride), attr(v.size() * svs::kAttrStride);
    svs::pack(v.data(), v.size(), pos.data(), attr.data());
    int difPos = 0, difUv = 0, difIdx = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        float p[3], uv[2];
        uint16_t tm[2];
        std::memcpy(p, &pos[i * svs::kPosStride], 12);
        std::memcpy(uv, &attr[i * svs::kAttrStride + svs::kAttrUv], 8);
        std::memcpy(tm, &attr[i * svs::kAttrStride + svs::kAttrTexMat], 4);
        difPos += std::memcmp(p, &v[i].position, 12) != 0;
        difUv += std::memcmp(uv, &v[i].texCoord, 8) != 0;
        difIdx += (tm[0] != v[i].texIndex) + (tm[1] != v[i].matId);
    }
    CHECK(difPos == 0);
    CHECK(difUv == 0);
    CHECK(difIdx == 0);

    std::printf("2. nada alem disso no stream\n");
    // 0xA5 (cor) e 0x77 (pbrIndex) nao aparecem onde so' ha' indices pequenos.
    uint16_t tm0[2];
    std::memcpy(tm0, &attr[svs::kAttrTexMat], 4);
    CHECK(tm0[0] == 0 && tm0[1] == 65535);

    std::printf("3. passos e offsets das entradas\n");
    CHECK(svs::kPosStride == 3 * sizeof(float));                    // R32G32B32_SFLOAT
    CHECK(svs::kAttrUv + 2 * sizeof(float) == svs::kAttrTexMat);    // R32G32_SFLOAT, depois R16G16_UINT
    CHECK(svs::kAttrTexMat + 2 * sizeof(uint16_t) == svs::kAttrStride);
    CHECK(sizeof(TerrainVertex) == 88); // 88 -> 12 (solido) ou 24 (alfa) bytes buscados
    std::printf("  vertice %zu B -> solido %zu B, alfa %zu B\n", sizeof(TerrainVertex), svs::kPosStride,
                svs::kPosStride + svs::kAttrStride);

    std::printf("4. hash dos vertices de origem (handle de VkBuffer reusado)\n");
    {
        std::vector<TerrainVertex> a(37, TerrainVertex{}), b(37, TerrainVertex{});
        const uint64_t ha = svs::hashVertices(a.data(), a.size() * sizeof(TerrainVertex));
        CHECK(ha == svs::hashVertices(b.data(), b.size() * sizeof(TerrainVertex)));  // mesmos bytes, mesmo hash
        b[20].position.x = 1.0f;                                                       // um vertice muda
        CHECK(ha != svs::hashVertices(b.data(), b.size() * sizeof(TerrainVertex)));
        CHECK(ha != svs::hashVertices(a.data(), (a.size() - 1) * sizeof(TerrainVertex))); // outro tamanho
    }

    std::printf(g_falhas ? "FALHOU (%d)\n" : "OK\n", g_falhas);
    return g_falhas ? 1 : 0;
}
