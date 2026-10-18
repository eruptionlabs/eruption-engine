#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : enable
// Pre-passe de profundidade das malhas com recorte por alfa (folhagem; ver
// ModelRenderer::render). So' descarta, nas MESMAS condicoes de
// model_frag.glsl (dither do crossfade/realce, alfa < 0,1 e chave magenta
// depois da mistura de 2 materiais), e deixa a profundidade. O G-buffer vem
// depois com o teste <= e sem escrever profundidade: cada pixel paga o shader
// inteiro uma vez so', em vez de uma vez por folha empilhada.
// Malha com splat (chao) nao passa por aqui: a CPU a deixa de fora.

// A interface inteira do model.vert, mesmo o que nao e' lido aqui: com ela
// igual a do G-buffer o driver monta o vertex shader das duas pipelines do
// mesmo jeito (e a validacao nao reclama de saida sem consumidor).
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in flat uint inTexIndex;
layout(location = 4) in flat uint inMatId;
layout(location = 5) in vec4 inColor;
layout(location = 6) in flat uint inPbrIndex;
layout(location = 7) in flat uint inNormalIndex;
layout(location = 8) in flat uint inBlendTexIndex;
layout(location = 9) in flat uint inBlendPbrIndex;
layout(location = 10) in flat uint inBlendNormalIndex;
layout(location = 11) in float inBlendWeight;
layout(location = 12) in flat uint inBlendMaskIndex;
layout(location = 13) in vec2 inBlendMaskUV;
layout(location = 14) in float inEmissiveStrength;
layout(location = 15) in flat uvec2 inSplatTex;
layout(location = 16) in flat uvec2 inSplatTex2;
layout(location = 17) in flat uint inBlendMaskIndex2;
layout(location = 18) in flat float inSway;
layout(location = 19) in vec4 inDispInfo;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    float alpha;
    float metallicScale;
    float roughnessScale;
    float _pad;
    vec4 uvScale;
} push;

#include "gbuffer/model_frame_ubo.glsl"
#include "gbuffer/see_through.glsl"

const float dither[16] = float[](
    0.0, 0.5, 0.125, 0.625,
    0.75, 0.25, 0.875, 0.375,
    0.1875, 0.6875, 0.0625, 0.5625,
    0.9375, 0.4375, 0.8125, 0.3125
);

void main() {
    if (push.alpha < 0.99) {
        float thr = abs(push.alpha);
        bool invert = push.alpha < 0.0;
        int x = int(gl_FragCoord.x) % 4;
        int y = int(gl_FragCoord.y) % 4;
        bool keep = invert ? (dither[x + y * 4] > thr) : (dither[x + y * 4] <= thr);
        if (!keep) discard;
    }
    if (seeThroughDiscard(inWorldPos)) discard;
    vec4 texColor = texture(u_textures[nonuniformEXT(inTexIndex)], inTexCoord);
    if (texColor.a < 0.1) discard;
    if (inBlendWeight > 0.0) {
        vec4 bColor = texture(u_textures[nonuniformEXT(inBlendTexIndex)], inTexCoord);
        float blendT = inBlendWeight;
        if (inBlendMaskIndex != 0u) blendT = texture(u_textures[nonuniformEXT(inBlendMaskIndex)], inBlendMaskUV).r;
        float hA = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        float hB = dot(bColor.rgb,  vec3(0.299, 0.587, 0.114));
        blendT = clamp(blendT + (hB - hA) * 0.35, 0.0, 1.0);
        texColor.rgb = mix(texColor.rgb, bColor.rgb, blendT);
    }
    float magentaness = min(texColor.r, texColor.b) - texColor.g;
    if (magentaness > 0.35) discard;
}
