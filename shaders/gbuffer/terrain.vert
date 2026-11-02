#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
// Indices de 16 bits aos pares (R16G16), como TerrainRenderer::createPipeline.
layout(location = 3) in uvec2 inTexMat;        // texIndex, matId
layout(location = 5) in uint inColor;
layout(location = 6) in uvec2 inPbrNormal;     // pbrIndex, normalIndex
layout(location = 8) in uvec2 inBlendTexPbr;   // blendTexIndex, blendPbrIndex
layout(location = 10) in uint inBlendNormalIndex;
#define inTexIndex inTexMat.x
#define inMatId inTexMat.y
#define inPbrIndex inPbrNormal.x
#define inNormalIndex inPbrNormal.y
#define inBlendTexIndex inBlendTexPbr.x
#define inBlendPbrIndex inBlendTexPbr.y
layout(location = 11) in float inBlendWeight;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out flat uint outTexIndex;
layout(location = 4) out flat uint outMatId;
layout(location = 5) out vec4 outColor;
layout(location = 6) out flat uint outPbrIndex;
layout(location = 7) out flat uint outNormalIndex;
layout(location = 8) out flat uint outBlendTexIndex;
layout(location = 9) out flat uint outBlendPbrIndex;
layout(location = 10) out flat uint outBlendNormalIndex;
layout(location = 11) out float outBlendWeight;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    float metallicScale;  // profile metallic factor, scales the MRAH-W red channel
    float roughnessScale; // profile roughness factor, scales the MRAH-W green channel
};

void main() {
    gl_Position = viewProjection * vec4(inPosition, 1.0);
    outWorldPos = inPosition;
    outTexCoord = inTexCoord;
    outNormal = inNormal;
    outTexIndex = inTexIndex;
    outMatId = inMatId;
    outColor = vec4(
        float(inColor & 0xFF) / 255.0,
        float((inColor >> 8) & 0xFF) / 255.0,
        float((inColor >> 16) & 0xFF) / 255.0,
        float((inColor >> 24) & 0xFF) / 255.0
    );
    outPbrIndex = inPbrIndex;
    outNormalIndex = inNormalIndex;
    outBlendTexIndex = inBlendTexIndex;
    outBlendPbrIndex = inBlendPbrIndex;
    outBlendNormalIndex = inBlendNormalIndex;
    outBlendWeight = inBlendWeight;
}
