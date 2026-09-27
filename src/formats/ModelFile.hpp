#pragma once

#include "utils/BinaryReader.hpp"
#include "math/Types.hpp"
#include <vector>
#include <string>
#include <optional>

namespace eruption {

// Source-agnostic in-memory model representation.
// Populated by the GLTF parser; only GLTF input is supported by the engine runtime.

struct ModelFace {
    uint16_t vertIds[3];
    uint16_t uvIds[3];
    uint16_t textureId;
    bool twoSided;
    int32_t smoothGroup;
};

struct BoundingBox {
    Vec3 min = Vec3(1e10f);
    Vec3 max = Vec3(-1e10f);
    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 range() const { return (max - min) * 0.5f; }
};

struct ModelScaleKeyframe {
    int32_t frame = 0;
    Vec3 scale = Vec3(1.0f);
    float data = 0.0f;
};

struct ModelRotKeyframe {
    int32_t frame = 0;
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
};

struct ModelPosKeyframe {
    int32_t frame = 0;
    Vec3 position = Vec3(0.0f);
    int32_t data = 0;
};

struct ModelAnimTextureKeyframe {
    int32_t frame = 0;
    float offset = 0.0f;
};

struct ModelAnimTexture {
    int32_t textureId = 0;
    int32_t type = 0;
    std::vector<ModelAnimTextureKeyframe> keyframes;
};

struct ModelFileNode {
    std::string name;
    std::string parentName;
    uint32_t parentIndex = 0xFFFFFFFF;

    std::vector<Vec3> vertices;
    std::vector<Vec2> texCoords;
    std::vector<Vec3> normals;
    std::vector<uint32_t> colors;       // BGRA
    std::vector<uint32_t> indices;
    std::vector<ModelFace> faces;
    std::vector<uint16_t> perVertexTexIds; // per-expanded-vertex texture index into node's texture list

    // Instancing de geometria: quando vários nós referenciam a MESMA mesh do
    // arquivo de origem, só o primeiro ("dono") materializa vértices/índices;
    // os demais deixam os arrays vazios e apontam para o dono aqui. O renderer
    // reusa o mesmo ModelMeshGPU e só troca a matriz por instância, que é o que
    // ModelInstance{meshIndex, transform} já sabia fazer.
    // -1 = nó tem geometria própria (comportamento de todo asset antigo).
    int32_t geometryShareIndex = -1;

    // Optional per-vertex texture crossfade, authored in the GLB via the
    // _BLEND_TEX_INDEX / _BLEND_WEIGHT vertex attributes. perVertexBlendTexIds is
    // a local index into textureNames (same space as perVertexTexIds) naming the
    // texture to fade toward; perVertexBlendWeights is 0..1 (0 = no blend).
    // Both empty unless the primitive carried _BLEND_WEIGHT, so meshes/maps
    // without the attributes are completely unaffected.
    std::vector<uint16_t> perVertexBlendTexIds;
    std::vector<float>    perVertexBlendWeights;

    // Optional world-space splat mask, authored via _BLEND_MASK_TEX_INDEX /
    // _BLEND_MASK_UV. perVertexBlendMaskTexIds is a local index into
    // textureNames (same space as perVertexTexIds/perVertexBlendTexIds)
    // naming the splat-mask texture; perVertexBlendMaskUV is the world-space
    // UV to sample it at. Both empty unless the primitive carried
    // _BLEND_MASK_UV, so meshes/maps without the attribute are unaffected.
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

    // Optional self-lit glow authored via _EMISSIVE_STRENGTH (0..1). Empty
    // unless the primitive carried the attribute.
    std::vector<float> perVertexEmissive;

    std::vector<uint32_t> textureIds;     // v1.x-v2.2: indices to global texture list
    std::vector<std::string> textureNames; // v2.3+: per-node texture names (runtime albedo slot)
    std::vector<std::string> pbrTextureNames; // v2.3+: original material names for PBR lookup

    Vec3 position;
    Vec3 rotationAxis;
    float rotationAngle = 0.0f;
    Vec3 scale = Vec3(1.0f);
    Mat4 offsetMatrix = Mat4(1.0f);
    Vec3 offset;

    // Animation keyframes
    std::vector<ModelScaleKeyframe> scaleKeyframes;
    std::vector<ModelRotKeyframe> rotKeyframes;
    std::vector<ModelPosKeyframe> posKeyframes;
    std::vector<ModelAnimTexture> animTextures;

    // Transformation results
    Mat4 nodeMatrix = Mat4(1.0f);   // ParentMatrix * T * R * S (with parent compensation)
    Mat4 renderMatrix = Mat4(1.0f); // nodeMatrix * [T(offset) * BasisMatrix]
    BoundingBox box;
};

struct ModelNodeAnimState {
    Vec3 scale = Vec3(1.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    Vec3 position = Vec3(0.0f);
};

struct ModelAnimation {
    float duration = 0.0f;
    float frameRate = 30.0f; // v2.2+
    std::vector<std::vector<Mat4>> frameMatrices;
};

struct EmbeddedTexture {
    std::string name;
    std::vector<uint8_t> pixels; // RGBA8 (decoded on demand)
    std::vector<uint8_t> encodedData; // raw encoded bytes from the source file (e.g. PNG/JPEG inside GLB)
    int width = 0;
    int height = 0;
    int channels = 0;

    bool isDecoded() const { return !pixels.empty() && width > 0 && height > 0; }
    bool hasEncodedData() const { return !encodedData.empty(); }
};

struct ModelFile {
    uint8_t versionMajor = 0;
    uint8_t versionMinor = 0;
    uint16_t version = 0;

    std::string name;
    std::string filePath; // original path in pack (for matching with source world)
    std::vector<ModelFileNode> nodes;
    std::vector<std::string> textures;
    std::vector<std::string> rootNodeNames; // v2.2+

    // Textures embedded inside the model file (e.g. GLB images).
    std::vector<EmbeddedTexture> embeddedTextures;

    float alpha = 1.0f;

    std::optional<ModelAnimation> animation;

    BoundingBox box;
    void calculateBoundingBox();

    // When true, the renderer should not auto-center this model using its
    // bounding box (used for single-file GLB maps that are authored in world
    // space and already contain a ground plane).
    bool skipMainOffset = false;
};

// Compute interpolated animation state for a node at the given animation frame
ModelNodeAnimState interpolateNode(const ModelFileNode& node, float currentFrame);

// Compute animated render matrices for all nodes in a model file at the given animation frame.
// Returns a vector of renderMatrix (one per node) that can replace node.renderMatrix during rendering.
std::vector<Mat4> computeAnimatedRenderMatrices(const ModelFile& model, float currentFrame);

// Animated texture (UV) state for a single texture slot on a node.
// Types:
//   0 = translate U, 1 = translate V, 2 = scale U, 3 = scale V, 4 = rotation
struct ModelTextureAnimState {
    Vec2 translate = Vec2(0.0f);
    Vec2 scale = Vec2(1.0f);
    float rotation = 0.0f; // radians
    bool active = false;
};

// Evaluate animated texture state for a specific textureId on a node.
// `animLen` is the model's animation length in frames (use 1 for static models).
ModelTextureAnimState interpolateTextureAnimation(const ModelFileNode& node, int32_t textureId,
                                                float currentFrame, int32_t animLen);

} // namespace eruption
