#pragma once

#include "formats/TerrainParser.hpp" // TerrainVertex

#include <cstddef>
#include <cstdint>
#include <cstring>

// STREAM DE VERTICE DA SOMBRA (spec 52, OE-03): o que o passe de sombra le' do
// vertice, separado do vertice inteiro de 88 B. Layout das entradas das
// pipelines de stream (ShadowRenderer::createStreamPipelines):
//   posicao: R32G32B32_SFLOAT no offset 0, passo 12 (os mesmos bits do vertice)
//   attr:    R32G32_SFLOAT (UV) no offset 0 + R16G16_UINT (texIndex, matId) no 8,
//            passo 12 (so' o caster com teste de alfa le')
namespace eruption::svs {

constexpr size_t kPosStride = 12;
constexpr size_t kAttrStride = 12;
constexpr size_t kAttrUv = 0;
constexpr size_t kAttrTexMat = 8;

static_assert(sizeof(TerrainVertex::position) == 12, "posicao do stream e' float x3");
static_assert(sizeof(TerrainVertex::texCoord) == 8, "UV do stream e' float x2");
static_assert(offsetof(TerrainVertex, matId) == offsetof(TerrainVertex, texIndex) + 2,
              "texIndex e matId vao aos pares (R16G16_UINT)");

// `count` vertices (TerrainVertex crus, sem alinhamento garantido) para os dois
// streams; `pos` com count * kPosStride bytes, `attr` com count * kAttrStride.
inline void pack(const void* vertices, size_t count, uint8_t* pos, uint8_t* attr) {
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (size_t i = 0; i < count; ++i, v += sizeof(TerrainVertex)) {
        std::memcpy(pos + i * kPosStride, v + offsetof(TerrainVertex, position), 12);
        std::memcpy(attr + i * kAttrStride + kAttrUv, v + offsetof(TerrainVertex, texCoord), 8);
        std::memcpy(attr + i * kAttrStride + kAttrTexMat, v + offsetof(TerrainVertex, texIndex), 4);
    }
}

} // namespace eruption::svs
