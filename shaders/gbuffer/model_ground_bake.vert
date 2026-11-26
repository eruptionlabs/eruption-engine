#version 450
// CHAO ASSADO (preset "ground_bake"), passe de assar: o vertice vai para onde a
// sua UV de MASCARA (a do splat, em espaco de mundo, 0..1 no mapa) cai numa
// textura quadrada - uma vista de cima do chao, sem camera. O fragment
// (model_ground_bake.frag) calcula o albedo final do caminho LITE + GROUND_MIN
// e a imagem fica guardada; model_ground_baked.frag so' a le' (1 leitura em vez
// de 5). Mesmas entradas de vertice dos shaders de modelo (model_vert.glsl).
layout(location = 1) in vec2 inTexCoord;
layout(location = 3) in uvec4 inColorBlend; // color, blendWeight, blendMaskUV (bits de float)
layout(location = 4) in uvec4 inSplat;      // splatTex01, 23, 45, 67
layout(location = 5) in uvec4 inMaterial;   // emissiveStrength, texIndex|matId, ...
layout(location = 6) in uvec4 inMaterial2;  // blendNormal, blendMask, blendMask2, emissive

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out flat uint outTexIndex;
layout(location = 2) out vec4 outColor;
layout(location = 3) out vec2 outMaskUV;
layout(location = 4) out flat uint outMaskIndex;
layout(location = 5) out flat uint outMaskIndex2;
layout(location = 6) out flat uvec2 outSplatTex;
layout(location = 7) out flat uvec2 outSplatTex2;

void main() {
    const vec2 maskUV = uintBitsToFloat(inColorBlend.zw);
    gl_Position = vec4(maskUV * 2.0 - 1.0, 0.0, 1.0);
    outTexCoord = inTexCoord;
    outTexIndex = inMaterial.y & 0xFFFFu;
    const uint c = inColorBlend.x;
    outColor = vec4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float((c >> 24) & 0xFFu)) / 255.0;
    outMaskUV = maskUV;
    outMaskIndex = inMaterial2.y;
    outMaskIndex2 = inMaterial2.z;
    outSplatTex = inSplat.xy;
    outSplatTex2 = inSplat.zw;
}
