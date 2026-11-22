#pragma once
// MISTURA ENTRE LADRILHOS DE CHAO. Um chao feito de casas quadradas alinhadas
// numa grade, uma malha por textura (cada casa com vertices proprios), troca
// de material na aresta da casa: o contraste vai de um pixel ao seguinte. Esta
// passada, feita uma vez na carga, preenche o crossfade por vertice que o
// modelo ja' tem (perVertexBlendTexIds / perVertexBlendWeights): em cada canto
// de casa, o peso e' a FRACAO das quatro casas em volta do canto que usam a
// textura vizinha. E' a media das casas, nao um numero escolhido: as casas que
// dividem o canto calculam o mesmo valor (A com 3 vizinhas B da' 3/4 para B;
// cada B da' 1/4 para A, e 1 - 1/4 = 3/4), entao nao ha' degrau nas arestas, e o
// centro da casa continua com a textura pura.
//
// So' age quando o padrao esta' presente (centenas de casas quadradas alinhadas,
// de duas ou mais texturas); qualquer outro mapa passa intacto. Nao ha' shader
// novo: model_frag.glsl ja' mistura pelo peso por vertice.
#include "formats/ModelFile.hpp"

namespace eruption {

struct GroundTileBlendStats {
    bool engaged = false;
    size_t tiles = 0;
    size_t blendedVertices = 0;
};

// strength 0..1 escala o peso (1 = media pura das casas, 0 = desliga).
GroundTileBlendStats applyGroundTileBlend(ModelFile& model, float strength = 1.0f);

} // namespace eruption
