#pragma once

#include "math/Frustum.hpp"
#include "math/Types.hpp"

#include <cstddef>
#include <cstdint>

namespace eruption {

// SoA representation of 6 frustum planes, aligned for SIMD loads.
struct FrustumPlanesSoA {
    alignas(32) float nx[6];
    alignas(32) float ny[6];
    alignas(32) float nz[6];
    alignas(32) float d[6];
};

// Convert Frustum planes to a SIMD-friendly SoA layout.
FrustumPlanesSoA convertFrustumToSoA(const Frustum& frustum);

// Cull count AABBs given as SoA float arrays.
// visibleMask must have at least ((count + 7) / 8) bytes.
// Bit j of byte i is set iff AABB (i*8 + j) intersects the frustum.
// All input arrays must be 32-byte aligned for the AVX2 path (the scalar
// fallback does not require alignment).
void cullAABBSoA(const float* minX, const float* minY, const float* minZ,
                 const float* maxX, const float* maxY, const float* maxZ,
                 uint32_t count,
                 const FrustumPlanesSoA& planes,
                 uint8_t* visibleMask);

} // namespace eruption
