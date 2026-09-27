#pragma once

#include "math/Types.hpp"
#include <vector>
#include <cstdint>

namespace eruption {

// Gera smooth normals com auto-smooth (split normals baseado em angulo)
// Retorna: array de normals por vertice por face (split normals / loop normals)
// O numero de saida pode ser maior que vertexCount (vertices duplicados nas sharp edges)
struct SmoothNormalResult {
    std::vector<Vec3> normals;           // Normal para cada loop
    std::vector<uint32_t> vertexIndices; // Qual vertice original cada loop pertence
    uint32_t loopCount = 0;
};

SmoothNormalResult calculateSmoothByAngleNormals(
    const std::vector<Vec3>& vertexPositions,
    const std::vector<uint32_t>& indices,
    float sharpAngleThreshold = 35.0f  // graus — tipico do Blender
);

} // namespace eruption
