#pragma once

#include "math/Types.hpp"
#include <vector>
#include <cstdint>

namespace eruption {

// Calcula weighted vertex normals para uma mesh
// vertexPositions: array de posicoes dos vertices
// indices: array de indices (triangles)
// angleWeight: se true, pondera tambem pelo angulo do vertice na face
// areaWeight: se true, pondera pela area da face
// retorna: array de normais por vertice
std::vector<Vec3> calculateWeightedNormals(
    const std::vector<Vec3>& vertexPositions,
    const std::vector<uint32_t>& indices,
    bool angleWeight = true,
    bool areaWeight = true
);

} // namespace eruption
