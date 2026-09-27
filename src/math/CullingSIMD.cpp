#include "math/CullingSIMD.hpp"

#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace eruption {

FrustumPlanesSoA convertFrustumToSoA(const Frustum& frustum) {
    FrustumPlanesSoA out{};
    for (int i = 0; i < 6; ++i) {
        const Plane& p = frustum.getPlane(static_cast<Frustum::PlaneIndex>(i));
        out.nx[i] = p.normal.x;
        out.ny[i] = p.normal.y;
        out.nz[i] = p.normal.z;
        out.d[i] = p.distance;
    }
    return out;
}

#if defined(__AVX2__)

static void cullAABBSoA_AVX2(const float* minX, const float* minY, const float* minZ,
                             const float* maxX, const float* maxY, const float* maxZ,
                             uint32_t count,
                             const FrustumPlanesSoA& planes,
                             uint8_t* visibleMask) {
    const uint32_t fullBlocks = count / 8;
    const uint32_t remainder = count & 7;

    for (uint32_t b = 0; b < fullBlocks; ++b) {
        __m256 vMinX = _mm256_load_ps(minX + b * 8);
        __m256 vMinY = _mm256_load_ps(minY + b * 8);
        __m256 vMinZ = _mm256_load_ps(minZ + b * 8);
        __m256 vMaxX = _mm256_load_ps(maxX + b * 8);
        __m256 vMaxY = _mm256_load_ps(maxY + b * 8);
        __m256 vMaxZ = _mm256_load_ps(maxZ + b * 8);

        // visible = all ones (true) for all 8 AABBs initially.
        __m256i visible = _mm256_set1_epi32(-1);

        for (int p = 0; p < 6; ++p) {
            __m256 nx = _mm256_set1_ps(planes.nx[p]);
            __m256 ny = _mm256_set1_ps(planes.ny[p]);
            __m256 nz = _mm256_set1_ps(planes.nz[p]);
            __m256 d  = _mm256_set1_ps(planes.d[p]);

            // P-vertex: select max when normal component is positive, min otherwise.
            // _mm256_blendv_ps(a,b,mask) selects b when the sign bit of mask is 1.
            // nx>=0 has sign bit 0 -> we want maxX (the 'a' argument).
            // nx<0  has sign bit 1 -> we want minX (the 'b' argument).
            __m256 pX = _mm256_blendv_ps(vMaxX, vMinX, nx);
            __m256 pY = _mm256_blendv_ps(vMaxY, vMinY, ny);
            __m256 pZ = _mm256_blendv_ps(vMaxZ, vMinZ, nz);

            // dist = dot(n, p) + d = nx*px + ny*py + nz*pz + d
            __m256 dist = _mm256_fmadd_ps(nx, pX, d);
            dist = _mm256_fmadd_ps(ny, pY, dist);
            dist = _mm256_fmadd_ps(nz, pZ, dist);

            // outside = dist < 0
            __m256i outside = _mm256_castps_si256(
                _mm256_cmp_ps(dist, _mm256_setzero_ps(), _CMP_LT_OQ));

            // visible &= ~outside
            visible = _mm256_andnot_si256(outside, visible);
        }

        // Pack the 8x32-bit mask into a single byte.
        int mask = _mm256_movemask_ps(_mm256_castsi256_ps(visible));
        visibleMask[b] = static_cast<uint8_t>(mask & 0xFF);
    }

    // Scalar tail for the remaining (<8) AABBs.
    if (remainder > 0) {
        uint32_t base = fullBlocks * 8;
        uint8_t mask = 0;
        for (uint32_t i = 0; i < remainder; ++i) {
            Vec3 mn(minX[base + i], minY[base + i], minZ[base + i]);
            Vec3 mx(maxX[base + i], maxY[base + i], maxZ[base + i]);
            bool vis = true;
            for (int p = 0; p < 6; ++p) {
                Plane plane(
                    Vec3(planes.nx[p], planes.ny[p], planes.nz[p]),
                    planes.d[p]);
                Vec3 pv = mn;
                if (plane.normal.x >= 0.0f) pv.x = mx.x;
                if (plane.normal.y >= 0.0f) pv.y = mx.y;
                if (plane.normal.z >= 0.0f) pv.z = mx.z;
                if (plane.distanceToPoint(pv) < 0.0f) {
                    vis = false;
                    break;
                }
            }
            if (vis) mask |= (1u << i);
        }
        visibleMask[fullBlocks] = mask;
    }
}

#endif // __AVX2__

static void cullAABBSoA_Scalar(const float* minX, const float* minY, const float* minZ,
                               const float* maxX, const float* maxY, const float* maxZ,
                               uint32_t count,
                               const FrustumPlanesSoA& planes,
                               uint8_t* visibleMask) {
    const uint32_t blocks = (count + 7) / 8;
    std::memset(visibleMask, 0, blocks);

    for (uint32_t i = 0; i < count; ++i) {
        Vec3 mn(minX[i], minY[i], minZ[i]);
        Vec3 mx(maxX[i], maxY[i], maxZ[i]);
        bool vis = true;
        for (int p = 0; p < 6; ++p) {
            Plane plane(
                Vec3(planes.nx[p], planes.ny[p], planes.nz[p]),
                planes.d[p]);
            Vec3 pv = mn;
            if (plane.normal.x >= 0.0f) pv.x = mx.x;
            if (plane.normal.y >= 0.0f) pv.y = mx.y;
            if (plane.normal.z >= 0.0f) pv.z = mx.z;
            if (plane.distanceToPoint(pv) < 0.0f) {
                vis = false;
                break;
            }
        }
        if (vis) {
            visibleMask[i / 8] |= static_cast<uint8_t>(1u << (i & 7));
        }
    }
}

void cullAABBSoA(const float* minX, const float* minY, const float* minZ,
                 const float* maxX, const float* maxY, const float* maxZ,
                 uint32_t count,
                 const FrustumPlanesSoA& planes,
                 uint8_t* visibleMask) {
    if (count == 0) return;

#if defined(__AVX2__)
    // The AVX2 kernel requires 32-byte aligned input arrays and a multiple-of-8
    // count handled internally. If arrays are unaligned, fall back to scalar.
    bool aligned =
        ((reinterpret_cast<uintptr_t>(minX) & 31) == 0) &&
        ((reinterpret_cast<uintptr_t>(minY) & 31) == 0) &&
        ((reinterpret_cast<uintptr_t>(minZ) & 31) == 0) &&
        ((reinterpret_cast<uintptr_t>(maxX) & 31) == 0) &&
        ((reinterpret_cast<uintptr_t>(maxY) & 31) == 0) &&
        ((reinterpret_cast<uintptr_t>(maxZ) & 31) == 0);

    if (aligned) {
        cullAABBSoA_AVX2(minX, minY, minZ, maxX, maxY, maxZ, count, planes, visibleMask);
        return;
    }
#endif

    cullAABBSoA_Scalar(minX, minY, minZ, maxX, maxY, maxZ, count, planes, visibleMask);
}

} // namespace eruption
