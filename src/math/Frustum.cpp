#include "math/Frustum.hpp"
#include <limits>
#include <algorithm>

namespace eruption {

void Frustum::extractFromMatrix(const Mat4& m) {
    // Extract planes from view-projection matrix
    // Left
    m_planes[Left].normal.x = m[0][3] + m[0][0];
    m_planes[Left].normal.y = m[1][3] + m[1][0];
    m_planes[Left].normal.z = m[2][3] + m[2][0];
    m_planes[Left].distance = m[3][3] + m[3][0];
    m_planes[Left].normalize();

    // Right
    m_planes[Right].normal.x = m[0][3] - m[0][0];
    m_planes[Right].normal.y = m[1][3] - m[1][0];
    m_planes[Right].normal.z = m[2][3] - m[2][0];
    m_planes[Right].distance = m[3][3] - m[3][0];
    m_planes[Right].normalize();

    // Bottom
    m_planes[Bottom].normal.x = m[0][3] + m[0][1];
    m_planes[Bottom].normal.y = m[1][3] + m[1][1];
    m_planes[Bottom].normal.z = m[2][3] + m[2][1];
    m_planes[Bottom].distance = m[3][3] + m[3][1];
    m_planes[Bottom].normalize();

    // Top
    m_planes[Top].normal.x = m[0][3] - m[0][1];
    m_planes[Top].normal.y = m[1][3] - m[1][1];
    m_planes[Top].normal.z = m[2][3] - m[2][1];
    m_planes[Top].distance = m[3][3] - m[3][1];
    m_planes[Top].normalize();

    // Near (Vulkan: z >= 0)
    m_planes[Near].normal.x = m[0][2];
    m_planes[Near].normal.y = m[1][2];
    m_planes[Near].normal.z = m[2][2];
    m_planes[Near].distance = m[3][2];
    m_planes[Near].normalize();

    // Far (Vulkan: z <= w)
    m_planes[Far].normal.x = m[0][3] - m[0][2];
    m_planes[Far].normal.y = m[1][3] - m[1][2];
    m_planes[Far].normal.z = m[2][3] - m[2][2];
    m_planes[Far].distance = m[3][3] - m[3][2];
    m_planes[Far].normalize();
}

bool Frustum::containsPoint(const Vec3& point) const {
    for (int i = 0; i < 6; i++) {
        if (m_planes[i].distanceToPoint(point) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersectsAABB(const Vec3& min, const Vec3& max) const {
    for (int i = 0; i < 6; i++) {
        const Plane& p = m_planes[i];
        // Find the positive vertex (the vertex most in the direction of the plane normal)
        Vec3 positiveVertex = min;
        if (p.normal.x >= 0.0f) positiveVertex.x = max.x;
        if (p.normal.y >= 0.0f) positiveVertex.y = max.y;
        if (p.normal.z >= 0.0f) positiveVertex.z = max.z;

        if (p.distanceToPoint(positiveVertex) < 0.0f) {
            return false; // Positive vertex is outside
        }
    }
    return true;
}

bool Frustum::intersectsAABB(const AABB& aabb) const {
    return intersectsAABB(aabb.min, aabb.max);
}

bool Frustum::intersectsSphere(const Vec3& center, float radius) const {
    for (int i = 0; i < 6; i++) {
        if (m_planes[i].distanceToPoint(center) < -radius) {
            return false;
        }
    }
    return true;
}

float Frustum::rayIntersectAABB(const Vec3& origin, const Vec3& dir, const Vec3& aabbMin, const Vec3& aabbMax) {
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();

    for (int i = 0; i < 3; i++) {
        if (std::abs(dir[i]) < 0.0001f) {
            // Ray is parallel to slab. No hit if origin not within slab.
            if (origin[i] < aabbMin[i] || origin[i] > aabbMax[i])
                return -1.0f;
        } else {
            float invD = 1.0f / dir[i];
            float t1 = (aabbMin[i] - origin[i]) * invD;
            float t2 = (aabbMax[i] - origin[i]) * invD;
            if (t1 > t2) std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax)
                return -1.0f;
        }
    }
    if (tMin > 0.0f)
        return tMin;
    // Ray starts inside AABB — return 0 as the "closest" hit distance.
    return 0.0f;
}

} // namespace eruption
