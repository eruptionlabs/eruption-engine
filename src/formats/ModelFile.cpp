#include "formats/ModelFile.hpp"
#include "core/Logger.hpp"
#include "utils/WeightedNormal.hpp"
#include "utils/SmoothByAngle.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace eruption {

template<typename Keyframe>
static std::pair<const Keyframe*, const Keyframe*> findSurroundingKeys(const std::vector<Keyframe>& keys, float currentFrame) {
    if (keys.empty()) return {nullptr, nullptr};
    if (keys.size() == 1) return {&keys[0], &keys[0]};

    // Clamp to first/last for simplicity. For cyclic animation, fmod handles wrapping at the top level.
    if (currentFrame <= keys.front().frame) {
        return {&keys.front(), &keys.front()};
    }
    if (currentFrame >= keys.back().frame) {
        return {&keys.back(), &keys.back()};
    }

    for (size_t i = 1; i < keys.size(); i++) {
        if (currentFrame < keys[i].frame) {
            return {&keys[i - 1], &keys[i]};
        }
    }
    return {&keys.back(), &keys.back()};
}

ModelNodeAnimState interpolateNode(const ModelFileNode& node, float currentFrame) {
    ModelNodeAnimState state;
    state.scale = node.scale;
    state.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    state.position = node.position;

    // Scale keyframes
    if (!node.scaleKeyframes.empty()) {
        if (node.scaleKeyframes.size() >= 2) {
            auto [prev, next] = findSurroundingKeys(node.scaleKeyframes, currentFrame);
            if (prev && next && prev != next) {
                float t = (currentFrame - prev->frame) / static_cast<float>(next->frame - prev->frame);
                state.scale = glm::mix(prev->scale, next->scale, t);
            } else if (prev) {
                state.scale = prev->scale;
            }
        } else {
            state.scale = node.scaleKeyframes[0].scale;
        }
    }

    // Rotation keyframes (SLERP)
    if (!node.rotKeyframes.empty()) {
        if (node.rotKeyframes.size() >= 2) {
            auto [prev, next] = findSurroundingKeys(node.rotKeyframes, currentFrame);
            if (prev && next && prev != next) {
                float t = (currentFrame - prev->frame) / static_cast<float>(next->frame - prev->frame);
                state.rotation = glm::slerp(prev->rotation, next->rotation, t);
            } else if (prev) {
                state.rotation = prev->rotation;
            }
        } else {
            state.rotation = node.rotKeyframes[0].rotation;
        }
    } else {
        // Fallback to static rotation
        if (glm::length(node.rotationAxis) > 0.001f) {
            state.rotation = glm::quat(glm::angleAxis(node.rotationAngle, node.rotationAxis));
        }
    }

    // Position keyframes
    if (!node.posKeyframes.empty()) {
        if (node.posKeyframes.size() >= 2) {
            auto [prev, next] = findSurroundingKeys(node.posKeyframes, currentFrame);
            if (prev && next && prev != next) {
                float t = (currentFrame - prev->frame) / static_cast<float>(next->frame - prev->frame);
                state.position = glm::mix(prev->position, next->position, t);
            } else if (prev) {
                state.position = prev->position;
            }
        } else {
            state.position = node.posKeyframes[0].position;
        }
    }

    return state;
}

std::vector<Mat4> computeAnimatedRenderMatrices(const ModelFile& model, float currentFrame) {
    std::vector<Mat4> renderMatrices(model.nodes.size(), Mat4(1.0f));
    std::vector<ModelNodeAnimState> states;
    states.reserve(model.nodes.size());
    for (const auto& node : model.nodes) {
        states.push_back(interpolateNode(node, currentFrame));
    }

    std::function<void(uint32_t, const Mat4&)> recurse = [&](uint32_t idx, const Mat4& parentMatrix) {
        const ModelFileNode& node = model.nodes[idx];
        const auto& state = states[idx];

        Mat4 nodeMatrix = parentMatrix;

        Vec3 pos = node.posKeyframes.empty() ? node.position : state.position;
        nodeMatrix = glm::translate(nodeMatrix, pos);

        if (!node.rotKeyframes.empty()) {
            nodeMatrix = nodeMatrix * glm::mat4_cast(state.rotation);
        } else {
            if (glm::length(node.rotationAxis) > 0.001f) {
                nodeMatrix = glm::rotate(nodeMatrix, node.rotationAngle, node.rotationAxis);
            }
        }

        Vec3 scl = node.scaleKeyframes.empty() ? node.scale : state.scale;
        nodeMatrix = glm::scale(nodeMatrix, scl);

        Mat4 renderMatrix;
        if (model.version >= 0x0202) {
            renderMatrix = nodeMatrix * node.offsetMatrix;
        } else {
            renderMatrix = nodeMatrix;
            if (model.nodes.size() > 1) {
                renderMatrix = glm::translate(renderMatrix, node.offset);
            }
            renderMatrix = renderMatrix * node.offsetMatrix;
        }
        renderMatrices[idx] = renderMatrix;

        for (uint32_t i = 0; i < model.nodes.size(); i++) {
            if (model.nodes[i].parentIndex == idx) {
                recurse(i, nodeMatrix);
            }
        }
    };

    for (uint32_t i = 0; i < model.nodes.size(); i++) {
        if (model.nodes[i].parentIndex == 0xFFFFFFFF || model.nodes[i].parentIndex == i) {
            recurse(i, Mat4(1.0f));
        }
    }

    return renderMatrices;
}

ModelTextureAnimState interpolateTextureAnimation(const ModelFileNode& node, int32_t textureId,
                                                float currentFrame, int32_t animLen) {
    ModelTextureAnimState result;
    result.active = false;

    if (node.animTextures.empty()) {
        return result;
    }

    int32_t cycle = glm::max(1, animLen);
    float tick = fmodf(currentFrame, static_cast<float>(cycle));
    if (tick < 0.0f) tick += cycle;

    for (const auto& anim : node.animTextures) {
        if (anim.textureId != textureId || anim.keyframes.empty()) {
            continue;
        }

        const auto& keys = anim.keyframes;

        const ModelAnimTextureKeyframe* prevKey = nullptr;
        const ModelAnimTextureKeyframe* nextKey = nullptr;
        for (size_t i = 0; i < keys.size(); i++) {
            if (tick < keys[i].frame) {
                nextKey = &keys[i];
                prevKey = (i == 0) ? &keys.back() : &keys[i - 1];
                break;
            }
        }
        if (!nextKey) {
            prevKey = &keys.back();
            nextKey = &keys.front();
        }

        float prevTick = static_cast<float>(prevKey->frame);
        float nextTick = static_cast<float>(nextKey->frame);
        if (nextTick <= prevTick) nextTick += static_cast<float>(cycle);
        if (tick < prevTick) tick += static_cast<float>(cycle);

        float interval = 0.0f;
        if (nextTick > prevTick) {
            interval = (tick - prevTick) / (nextTick - prevTick);
        }
        float value = glm::mix(prevKey->offset, nextKey->offset, interval);

        switch (anim.type) {
        case 0: result.translate.x += value; result.active = true; break;
        case 1: result.translate.y += value; result.active = true; break;
        case 2: result.scale.x = value; result.active = true; break;
        case 3: result.scale.y = value; result.active = true; break;
        case 4: result.rotation = value; result.active = true; break;
        default: break;
        }
    }
    return result;
}

[[maybe_unused]] static void postProcessNode(ModelFileNode& node) {
    if (node.vertices.empty() || node.faces.empty()) return;

    std::vector<Vec3> expandedVerts;
    std::vector<Vec2> expandedUVs;
    std::vector<Vec3> expandedNormals;
    std::vector<uint32_t> expandedColors;
    std::vector<uint32_t> expandedIndices;
    std::vector<uint32_t> faceTexIds;
    std::vector<bool> faceTwoSided;

    expandedVerts.reserve(node.faces.size() * 3);
    expandedUVs.reserve(node.faces.size() * 3);
    expandedNormals.reserve(node.faces.size() * 3);
    expandedColors.reserve(node.faces.size() * 3);
    expandedIndices.reserve(node.faces.size() * 3);
    node.perVertexTexIds.reserve(node.faces.size() * 3);
    faceTexIds.reserve(node.faces.size());
    faceTwoSided.reserve(node.faces.size());

    std::vector<Vec3> faceNormals;
    faceNormals.reserve(node.faces.size());
    for (const auto& face : node.faces) {
        Vec3 v0 = node.vertices[face.vertIds[0]];
        Vec3 v1 = node.vertices[face.vertIds[1]];
        Vec3 v2 = node.vertices[face.vertIds[2]];
        Vec3 n = glm::cross(v1 - v0, v2 - v0);
        float len = glm::length(n);
        if (len > 1e-5f) {
            n /= len;
        } else {
            n = Vec3(0, 1, 0);
        }
        faceNormals.push_back(n);
    }

    for (size_t f = 0; f < node.faces.size(); ++f) {
        const auto& face = node.faces[f];

        uint32_t texRef = 0;
        if (!node.textureNames.empty()) {
            texRef = face.textureId < node.textureNames.size() ? face.textureId : 0;
        } else {
            texRef = face.textureId < node.textureIds.size() ? node.textureIds[face.textureId] : 0;
        }
        faceTexIds.push_back(texRef);
        faceTwoSided.push_back(face.twoSided);

        for (int i = 0; i < 3; ++i) {
            uint16_t vid = face.vertIds[i];
            uint16_t uvid = face.uvIds[i];

            Vec3 normal = faceNormals[f];
            int count = 1;

            for (size_t f2 = 0; f2 < node.faces.size(); ++f2) {
                if (f == f2) continue;
                const auto& face2 = node.faces[f2];
                if (face.smoothGroup != face2.smoothGroup || face.smoothGroup == 0) continue;

                bool sharesVertex = false;
                for (int j = 0; j < 3; ++j) {
                    if (face2.vertIds[j] == vid) {
                        sharesVertex = true;
                        break;
                    }
                }
                if (sharesVertex) {
                    normal += faceNormals[f2];
                    count++;
                }
            }

            if (count > 1) {
                float len = glm::length(normal);
                if (len > 1e-5f) {
                    normal /= len;
                } else {
                    normal = faceNormals[f];
                }
            }

            expandedVerts.push_back(node.vertices[vid]);
            expandedUVs.push_back(node.texCoords[uvid]);
            expandedNormals.push_back(normal);
            if (!node.colors.empty() && uvid < node.colors.size()) {
                expandedColors.push_back(node.colors[uvid]);
            } else {
                expandedColors.push_back(0xFFFFFFFF);
            }
            expandedIndices.push_back(static_cast<uint32_t>(expandedIndices.size()));
            node.perVertexTexIds.push_back(face.textureId);
        }
    }

    node.vertices = std::move(expandedVerts);
    node.texCoords = std::move(expandedUVs);
    node.normals = std::move(expandedNormals);
    node.colors = std::move(expandedColors);
    node.indices = std::move(expandedIndices);
    node.faces.clear();
}

static void calcNodeBoundingBox(ModelFile& model, uint32_t nodeIdx, const Mat4& parentMatrix) {
    ModelFileNode& node = model.nodes[nodeIdx];

    node.nodeMatrix = parentMatrix;
    node.nodeMatrix = glm::translate(node.nodeMatrix, node.position);

    if (node.rotKeyframes.empty()) {
        if (glm::length(node.rotationAxis) > 0.001f) {
            node.nodeMatrix = glm::rotate(node.nodeMatrix, node.rotationAngle, node.rotationAxis);
        }
    } else {
        node.nodeMatrix = node.nodeMatrix * glm::mat4_cast(node.rotKeyframes[0].rotation);
    }

    node.nodeMatrix = glm::scale(node.nodeMatrix, node.scale);

    if (model.version >= 0x0202) {
        node.renderMatrix = node.nodeMatrix * node.offsetMatrix;
    } else {
        node.renderMatrix = node.nodeMatrix;
        if (model.nodes.size() > 1) {
            node.renderMatrix = glm::translate(node.renderMatrix, node.offset);
        }
        node.renderMatrix = node.renderMatrix * node.offsetMatrix;
    }

    node.box = BoundingBox();
    for (const auto& v : node.vertices) {
        Vec4 transformed = node.renderMatrix * Vec4(v, 1.0f);
        Vec3 tv(transformed);
        node.box.min = glm::min(node.box.min, tv);
        node.box.max = glm::max(node.box.max, tv);
    }

    for (uint32_t i = 0; i < model.nodes.size(); i++) {
        if (model.nodes[i].parentIndex == nodeIdx) {
            calcNodeBoundingBox(model, i, node.nodeMatrix);
        }
    }
}

void ModelFile::calculateBoundingBox() {
    if (nodes.empty()) return;

    for (uint32_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].parentIndex == 0xFFFFFFFF || nodes[i].parentIndex == i) {
            calcNodeBoundingBox(*this, i, Mat4(1.0f));
        }
    }

    box = BoundingBox();
    for (const auto& node : nodes) {
        if (node.box.min.x > 1e9f) continue;
        box.min = glm::min(box.min, node.box.min);
        box.max = glm::max(box.max, node.box.max);
    }
}

} // namespace eruption
