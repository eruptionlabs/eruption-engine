#pragma once

#include "math/Types.hpp"

namespace eruption {

struct AABB {
    Vec3 min;
    Vec3 max;

    bool isValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extent() const { return max - min; }
};

struct Plane {
    Vec3 normal;
    float distance;

    Plane() = default;
    Plane(const Vec3& n, float d) : normal(n), distance(d) {}

    float distanceToPoint(const Vec3& p) const {
        return glm::dot(normal, p) + distance;
    }

    void normalize() {
        float len = glm::length(normal);
        if (len > 0.0001f) {
            normal /= len;
            distance /= len;
        }
    }
};

class Frustum {
public:
    // Planes: Left, Right, Bottom, Top, Near, Far
    enum PlaneIndex { Left = 0, Right, Bottom, Top, Near, Far };

    Frustum() = default;

    // Extract planes from view-projection matrix
    void extractFromMatrix(const Mat4& viewProj);

    // Test if a point is inside
    bool containsPoint(const Vec3& point) const;

    // Test AABB intersection
    bool intersectsAABB(const Vec3& min, const Vec3& max) const;
    bool intersectsAABB(const AABB& aabb) const;

    // Test sphere intersection
    bool intersectsSphere(const Vec3& center, float radius) const;

    const Plane& getPlane(PlaneIndex index) const { return m_planes[index]; }

    // Ray-AABB intersection (slab method). Returns tMin (>0) if hit, else -1.0f.
    static float rayIntersectAABB(const Vec3& origin, const Vec3& dir, const Vec3& aabbMin, const Vec3& aabbMax);

private:
    Plane m_planes[6];
};

} // namespace eruption
