#include "utils/SmoothByAngle.hpp"

#include <glm/glm.hpp>
#include <cmath>
#include <queue>

namespace eruption {

static bool facesShareEdge(uint32_t faceA, uint32_t faceB, uint32_t vertexIndex, const std::vector<uint32_t>& indices) {
    uint32_t a0 = indices[faceA * 3 + 0];
    uint32_t a1 = indices[faceA * 3 + 1];
    uint32_t a2 = indices[faceA * 3 + 2];
    uint32_t b0 = indices[faceB * 3 + 0];
    uint32_t b1 = indices[faceB * 3 + 1];
    uint32_t b2 = indices[faceB * 3 + 2];

    // Count shared vertices between the two faces
    int shared = 0;
    if (a0 == b0 || a0 == b1 || a0 == b2) shared++;
    if (a1 == b0 || a1 == b1 || a1 == b2) shared++;
    if (a2 == b0 || a2 == b1 || a2 == b2) shared++;

    // They share an edge if they share exactly 2 vertices
    // And one of them must be the vertex we're processing
    if (shared != 2) return false;

    bool vertexInA = (a0 == vertexIndex || a1 == vertexIndex || a2 == vertexIndex);
    bool vertexInB = (b0 == vertexIndex || b1 == vertexIndex || b2 == vertexIndex);
    return vertexInA && vertexInB;
}

SmoothNormalResult calculateSmoothByAngleNormals(
    const std::vector<Vec3>& vertices,
    const std::vector<uint32_t>& indices,
    float sharpAngleThreshold
) {
    const uint32_t triCount = static_cast<uint32_t>(indices.size()) / 3;
    const float cosThreshold = std::cos(glm::radians(sharpAngleThreshold));

    // Passo 1: Calcular normais de faces
    std::vector<Vec3> faceNormals(triCount);
    for (uint32_t t = 0; t < triCount; t++) {
        Vec3 v0 = vertices[indices[t * 3 + 0]];
        Vec3 v1 = vertices[indices[t * 3 + 1]];
        Vec3 v2 = vertices[indices[t * 3 + 2]];
        faceNormals[t] = glm::normalize(glm::cross(v1 - v0, v2 - v0));
    }

    struct Loop {
        uint32_t vertexIndex;
        uint32_t faceIndex;
        uint32_t vertInFace;
        Vec3 normal;
    };

    std::vector<Loop> loops;
    loops.reserve(triCount * 3);

    std::vector<std::vector<uint32_t>> vertexLoops(vertices.size());

    for (uint32_t t = 0; t < triCount; t++) {
        for (uint32_t v = 0; v < 3; v++) {
            uint32_t vi = indices[t * 3 + v];
            Loop loop;
            loop.vertexIndex = vi;
            loop.faceIndex = t;
            loop.vertInFace = v;
            vertexLoops[vi].push_back(static_cast<uint32_t>(loops.size()));
            loops.push_back(loop);
        }
    }

    std::vector<bool> loopProcessed(loops.size(), false);

    for (uint32_t vi = 0; vi < vertices.size(); vi++) {
        const auto& vloops = vertexLoops[vi];

        std::vector<std::vector<uint32_t>> smoothFans;

        for (uint32_t li : vloops) {
            if (loopProcessed[li]) continue;

            std::vector<uint32_t> fan;
            std::queue<uint32_t> toProcess;
            toProcess.push(li);
            loopProcessed[li] = true;

            while (!toProcess.empty()) {
                uint32_t current = toProcess.front();
                toProcess.pop();
                fan.push_back(current);

                uint32_t currentFace = loops[current].faceIndex;
                Vec3 currentNormal = faceNormals[currentFace];

                for (uint32_t otherLi : vloops) {
                    if (loopProcessed[otherLi]) continue;

                    uint32_t otherFace = loops[otherLi].faceIndex;
                    if (otherFace == currentFace) continue;

                    Vec3 otherNormal = faceNormals[otherFace];

                    float cosAngle = glm::dot(currentNormal, otherNormal);

                    if (cosAngle > cosThreshold) {
                        if (facesShareEdge(currentFace, otherFace, vi, indices)) {
                            toProcess.push(otherLi);
                            loopProcessed[otherLi] = true;
                        }
                    }
                }
            }

            smoothFans.push_back(std::move(fan));
        }

        // Passo 4: Calcular normal para cada smooth fan (weighted by angle)
        for (const auto& fan : smoothFans) {
            Vec3 accumulatedNormal(0.0f);

            for (uint32_t li : fan) {
                const Loop& loop = loops[li];
                uint32_t faceIdx = loop.faceIndex;

                uint32_t v0i = indices[faceIdx * 3 + 0];
                uint32_t v1i = indices[faceIdx * 3 + 1];
                uint32_t v2i = indices[faceIdx * 3 + 2];

                Vec3 e0 = glm::normalize(vertices[v1i] - vertices[v0i]); // v0 -> v1
                Vec3 e1 = glm::normalize(vertices[v2i] - vertices[v1i]); // v1 -> v2
                Vec3 e2 = glm::normalize(vertices[v0i] - vertices[v2i]); // v2 -> v0

                float angle = 0.0f;
                if (loop.vertInFace == 0) {
                    angle = std::acos(glm::clamp(glm::dot(e0, -e2), -1.0f, 1.0f));
                } else if (loop.vertInFace == 1) {
                    angle = std::acos(glm::clamp(glm::dot(e1, -e0), -1.0f, 1.0f));
                } else {
                    angle = std::acos(glm::clamp(glm::dot(e2, -e1), -1.0f, 1.0f));
                }

                accumulatedNormal += faceNormals[faceIdx] * angle;
            }

            Vec3 finalNormal = glm::length(accumulatedNormal) > 0.0001f
                ? glm::normalize(accumulatedNormal)
                : Vec3(0.0f, 1.0f, 0.0f);

            for (uint32_t li : fan) {
                loops[li].normal = finalNormal;
            }
        }
    }

    SmoothNormalResult result;
    result.normals.reserve(loops.size());
    result.vertexIndices.reserve(loops.size());

    for (const auto& loop : loops) {
        result.normals.push_back(loop.normal);
        result.vertexIndices.push_back(loop.vertexIndex);
    }

    result.loopCount = static_cast<uint32_t>(loops.size());
    return result;
}

} // namespace eruption
