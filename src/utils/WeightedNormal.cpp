#include "utils/WeightedNormal.hpp"

#include <glm/glm.hpp>
#include <cmath>

namespace eruption {

std::vector<Vec3> calculateWeightedNormals(
    const std::vector<Vec3>& vertices,
    const std::vector<uint32_t>& indices,
    bool useAngleWeight,
    bool useAreaWeight
) {
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const uint32_t triangleCount = static_cast<uint32_t>(indices.size()) / 3;

    std::vector<Vec3> normals(vertexCount, Vec3(0.0f));

    struct FaceData {
        Vec3 normal;
        float area;
        Vec3 angles;
    };

    std::vector<FaceData> faceData(triangleCount);

    for (uint32_t t = 0; t < triangleCount; t++) {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];

        const Vec3& v0 = vertices[i0];
        const Vec3& v1 = vertices[i1];
        const Vec3& v2 = vertices[i2];

        Vec3 edge1 = v1 - v0;
        Vec3 edge2 = v2 - v0;
        Vec3 faceNormal = glm::cross(edge1, edge2);

        float area = glm::length(faceNormal) * 0.5f;

        if (glm::length(faceNormal) > 0.0001f) {
            faceNormal = glm::normalize(faceNormal);
        }

        float angle0 = std::acos(glm::clamp(-glm::dot(glm::normalize(v1 - v0), glm::normalize(v2 - v0)), -1.0f, 1.0f));
        float angle1 = std::acos(glm::clamp(-glm::dot(glm::normalize(v0 - v1), glm::normalize(v2 - v1)), -1.0f, 1.0f));
        float angle2 = std::acos(glm::clamp(-glm::dot(glm::normalize(v0 - v2), glm::normalize(v1 - v2)), -1.0f, 1.0f));

        faceData[t] = { faceNormal, area, Vec3(angle0, angle1, angle2) };
    }

    for (uint32_t t = 0; t < triangleCount; t++) {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];

        const FaceData& fd = faceData[t];

        float w0 = 1.0f, w1 = 1.0f, w2 = 1.0f;

        if (useAreaWeight) {
            w0 *= fd.area;
            w1 *= fd.area;
            w2 *= fd.area;
        }

        if (useAngleWeight) {
            w0 *= fd.angles.x;
            w1 *= fd.angles.y;
            w2 *= fd.angles.z;
        }

        normals[i0] += fd.normal * w0;
        normals[i1] += fd.normal * w1;
        normals[i2] += fd.normal * w2;
    }

    for (uint32_t i = 0; i < vertexCount; i++) {
        if (glm::length(normals[i]) > 0.0001f) {
            normals[i] = glm::normalize(normals[i]);
        } else {
            normals[i] = Vec3(0.0f, 1.0f, 0.0f);
        }
    }

    return normals;
}

} // namespace eruption
