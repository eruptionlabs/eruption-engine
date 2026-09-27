#include "formats/ModelDataConverter.hpp"

namespace eruption {

ModelAsset convertModelToAsset(const ModelFile& model) {
    ModelAsset out;
    out.filePath = model.filePath;
    out.versionMajor = model.versionMajor;
    out.versionMinor = model.versionMinor;
    out.boxMin = model.box.min;
    out.boxMax = model.box.max;
    out.skipMainOffset = model.skipMainOffset;
    out.textures = model.textures;

    if (model.animation.has_value()) {
        out.animDuration = model.animation->duration;
        out.frameRate = model.animation->frameRate;
    }

    out.nodes.reserve(model.nodes.size());
    for (const auto& node : model.nodes) {
        ModelNode n;
        n.name = node.name;
        n.parentIndex = node.parentIndex;
        n.renderMatrix = node.renderMatrix;
        n.offsetMatrix = node.offsetMatrix;
        n.offset = node.offset;
        n.position = node.position;
        n.rotation = glm::angleAxis(node.rotationAngle, node.rotationAxis);
        n.scale = node.scale;

        n.vertices.reserve(node.vertices.size());
        for (size_t i = 0; i < node.vertices.size(); ++i) {
            ModelVertex v{};
            v.position = node.vertices[i];
            v.texCoord = (i < node.texCoords.size()) ? node.texCoords[i] : Vec2(0.0f);
            v.normal = (i < node.normals.size()) ? node.normals[i] : Vec3(0.0f, 1.0f, 0.0f);
            v.color = (i < node.colors.size()) ? node.colors[i] : 0xFFFFFFFF;
            v.texIndex = 0;
            v.matId = 0;
            n.vertices.push_back(v);
        }

        n.indices = node.indices;
        n.perVertexTexIds.reserve(node.perVertexTexIds.size());
        for (uint16_t id : node.perVertexTexIds) n.perVertexTexIds.push_back(id);
        n.perVertexBlendTexIds = node.perVertexBlendTexIds;
        n.perVertexBlendWeights = node.perVertexBlendWeights;
        n.perVertexBlendMaskTexIds = node.perVertexBlendMaskTexIds;
        n.perVertexBlendMaskTexIds2 = node.perVertexBlendMaskTexIds2;
        for (int sIdx = 0; sIdx < 8; ++sIdx) {
            n.perVertexSplatTexIds[sIdx] = node.perVertexSplatTexIds[sIdx];
        }
        n.perVertexBlendMaskUV = node.perVertexBlendMaskUV;
        n.perVertexEmissive = node.perVertexEmissive;
        n.aabbMin = node.box.min;
        n.aabbMax = node.box.max;
        n.geometryShareIndex = node.geometryShareIndex;

        n.scaleKeyframes.reserve(node.scaleKeyframes.size());
        for (const auto& k : node.scaleKeyframes) {
            n.scaleKeyframes.push_back({static_cast<float>(k.frame), k.scale});
        }
        n.rotKeyframes.reserve(node.rotKeyframes.size());
        for (const auto& k : node.rotKeyframes) {
            n.rotKeyframes.push_back({static_cast<float>(k.frame), k.rotation});
        }
        n.posKeyframes.reserve(node.posKeyframes.size());
        for (const auto& k : node.posKeyframes) {
            n.posKeyframes.push_back({static_cast<float>(k.frame), k.position});
        }

        n.animTextures.reserve(node.animTextures.size());
        for (const auto& a : node.animTextures) {
            AnimTexture at;
            at.textureId = a.textureId;
            at.type = a.type;
            at.keyframes.reserve(a.keyframes.size());
            for (const auto& k : a.keyframes) {
                at.keyframes.push_back({static_cast<float>(k.frame), k.offset});
            }
            n.animTextures.push_back(std::move(at));
        }

        n.textureNames = node.textureNames;
        n.pbrTextureNames = node.pbrTextureNames;
        n.textureIds = node.textureIds;

        out.nodes.push_back(std::move(n));
    }

    return out;
}

} // namespace eruption
