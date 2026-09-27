#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <cfloat>

namespace eruption {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Mat4 = glm::mat4;
using Quat = glm::quat;

// ------------------------------------------------------------------
// Generic model vertex (matches TerrainVertex layout used by renderer)
// ------------------------------------------------------------------
struct ModelVertex {
    Vec3 position;
    Vec2 texCoord;
    Vec3 normal;
    uint32_t texIndex;
    uint32_t matId;
    uint32_t color;
    uint32_t pbrIndex = 0;
    uint32_t normalIndex = 0;
    uint32_t blendTexIndex = 0;
    uint32_t blendPbrIndex = 0;
    uint32_t blendNormalIndex = 0;
    float blendWeight = 0.0f;
};

// ------------------------------------------------------------------
// Generic animation keyframes
// ------------------------------------------------------------------
struct ScaleKeyframe {
    float frame = 0.0f;
    Vec3 scale = Vec3(1.0f);
};

struct RotKeyframe {
    float frame = 0.0f;
    Quat rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
};

struct PosKeyframe {
    float frame = 0.0f;
    Vec3 position = Vec3(0.0f);
};

struct AnimTextureKeyframe {
    float frame = 0.0f;
    float offset = 0.0f;
};

struct AnimTexture {
    int32_t textureId = 0;
    int32_t type = 0; // 0=translateU, 1=translateV, 2=scaleU, 3=scaleV, 4=rotate
    std::vector<AnimTextureKeyframe> keyframes;
};

// ------------------------------------------------------------------
// Generic model node (equivalent to LegacyModelNode, but format-agnostic)
// ------------------------------------------------------------------
struct ModelNode {
    std::string name;
    uint32_t parentIndex = 0xFFFFFFFF;
    Mat4 renderMatrix = Mat4(1.0f);
    Vec3 position = Vec3(0.0f);
    Quat rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    Vec3 scale = Vec3(1.0f);

    // Geometry
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> perVertexTexIds;
    // Per-vertex texture crossfade authored in the GLB (see ModelFileNode).
    // Empty unless the source primitive carried _BLEND_WEIGHT.
    std::vector<uint16_t> perVertexBlendTexIds;
    std::vector<float>    perVertexBlendWeights;
    // Optional world-space splat mask (see ModelFileNode). Empty unless the
    // source primitive carried _BLEND_MASK_UV.
    std::vector<uint16_t> perVertexBlendMaskTexIds;
    // Segunda mascara (dobra o splat de 4 pra 8 camadas + base = 9). Reusa
    // perVertexBlendMaskUV - e' so' posicao de mundo, a mesma UV serve pras
    // duas mascaras. Vazio = sem segunda mascara (GLB so' com o splat de 4,
    // ou sem splat nenhum).
    std::vector<uint16_t> perVertexBlendMaskTexIds2;
    // Splat de ate' 8 camadas: slot local de textura de cada camada, por
    // vertice. [0..3] vem da primeira mascara (feature original), [4..7] da
    // segunda. Vazio = mapa sem splat (todo GLB anterior a essa feature).
    std::vector<uint16_t> perVertexSplatTexIds[8];
    std::vector<Vec2>     perVertexBlendMaskUV;
    // Optional self-lit glow (see ModelFileNode). Empty unless the source
    // primitive carried _EMISSIVE_STRENGTH.
    std::vector<float> perVertexEmissive;
    Vec3 aabbMin = Vec3(FLT_MAX);
    Vec3 aabbMax = Vec3(-FLT_MAX);

    // Instancing de geometria: -1 = nó tem geometria própria. >= 0 aponta para
    // o índice do nó "dono" da malha; este nó desenha o mesmo ModelMeshGPU com
    // outra matriz. Ver ModelFileNode::geometryShareIndex.
    int32_t geometryShareIndex = -1;

    // Animation tracks
    std::vector<ScaleKeyframe> scaleKeyframes;
    std::vector<RotKeyframe> rotKeyframes;
    std::vector<PosKeyframe> posKeyframes;
    std::vector<AnimTexture> animTextures;

    // Offset / basis matrix used during animation and static render matrix construction
    Mat4 offsetMatrix = Mat4(1.0f);
    Vec3 offset = Vec3(0.0f);

    // Texture references (resolved by the adapter)
    std::vector<std::string> textureNames;    // per-node names (e.g. v2.3+)
    std::vector<std::string> pbrTextureNames; // original material names for PBR map lookup
    std::vector<uint32_t> textureIds;         // indices into ModelAsset::textures
};

// ------------------------------------------------------------------
// Generic model asset (equivalent to ModelFile, but format-agnostic)
// ------------------------------------------------------------------
struct ModelAsset {
    std::string filePath;
    uint32_t versionMajor = 0;
    uint32_t versionMinor = 0;

    Vec3 boxMin = Vec3(0.0f);
    Vec3 boxMax = Vec3(0.0f);

    // When true, keep the authored world-space coordinates instead of
    // centering the model via its bounding box.
    bool skipMainOffset = false;

    std::vector<std::string> textures; // global texture name list
    std::vector<ModelNode> nodes;

    // Animation metadata
    float animDuration = 0.0f;
    float frameRate = 30.0f;
};

// ------------------------------------------------------------------
// Generic instance descriptor (equivalent to LegacyWorldModel, but format-agnostic)
// ------------------------------------------------------------------
struct ModelInstanceDesc {
    std::string modelPath;
    Vec3 position = Vec3(0.0f);
    Vec3 rotation = Vec3(0.0f); // Euler angles in degrees
    Vec3 scale = Vec3(1.0f);
    int32_t animType = 0;
    float animSpeed = 1.0f;
};

} // namespace eruption
