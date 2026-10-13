#pragma once

#include "math/Types.hpp"

#include <cstdint>
#include <vector>

namespace eruption {

// Pedaco de uma malha: intervalo do buffer de indices e a caixa dos
// triangulos dele, no espaco da malha.
struct MeshCluster {
    Vec3 mn, mx;
    uint32_t first, count; // em indices (3 por triangulo)
};

// Reordena os triangulos de `idx` (sobre as posicoes em `pos`, passo
// `stride` bytes) por celula de uma grade 3D na caixa da malha, em ordem de
// Morton, e devolve um pedaco por celula ocupada. Dentro da celula a ordem
// original dos triangulos e' mantida. Usado para descartar, por pedaco, o que
// fica fora da tela numa malha grande (ModelRenderer).
std::vector<MeshCluster> buildMeshClusters(const unsigned char* pos, size_t stride, std::vector<uint32_t>& idx);

} // namespace eruption
