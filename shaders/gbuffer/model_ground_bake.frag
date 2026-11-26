#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
// Passe de assar do chao (ver model_ground_bake.vert): o albedo do caminho
// GROUND_MIN do model_frag.glsl - a base e as 2 camadas de maior peso entre as
// 8 do splat, pela mascara - gravado numa textura em vez de calculado a cada
// pixel da tela. As derivadas da UV aqui sao as do assado, entao o mip lido de
// cada camada e' o que casa com o texel da imagem.
layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in flat uint inTexIndex;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inMaskUV;
layout(location = 4) in flat uint inMaskIndex;
layout(location = 5) in flat uint inMaskIndex2;
layout(location = 6) in flat uvec2 inSplatTex;
layout(location = 7) in flat uvec2 inSplatTex2;

layout(location = 0) out vec4 outAlbedo;

#include "common/slot_textures.glsl"

// A mesma escolha das duas camadas de maior peso do model_frag.glsl (topDuas).
void topDuas(uint s, float p, inout uint s0, inout float p0, inout uint s1, inout float p1) {
    if ((s & 0x3FFFu) == 0u || p <= 0.0) return;
    if (p > p0) { s1 = s0; p1 = p0; s0 = s; p0 = p; }
    else if (p > p1) { s1 = s; p1 = p; }
}

void main() {
    vec3 aG = slotTexture(inTexIndex, inTexCoord).rgb;
    if (inMaskIndex != 0u) {
        vec4 wg = slotTexture(inMaskIndex, inMaskUV);
        uint g0 = 0u, g1 = 0u;
        float q0 = 0.0, q1 = 0.0;
        topDuas( inSplatTex.x        & 0xFFFFu, wg.r, g0, q0, g1, q1);
        topDuas((inSplatTex.x >> 16) & 0xFFFFu, wg.g, g0, q0, g1, q1);
        topDuas( inSplatTex.y        & 0xFFFFu, wg.b, g0, q0, g1, q1);
        topDuas((inSplatTex.y >> 16) & 0xFFFFu, wg.a, g0, q0, g1, q1);
        float wgSum = wg.r + wg.g + wg.b + wg.a;
        vec4 wg2 = inMaskIndex2 != 0u ? slotTexture(inMaskIndex2, inMaskUV) : vec4(0.0);
        topDuas( inSplatTex2.x        & 0xFFFFu, wg2.r, g0, q0, g1, q1);
        topDuas((inSplatTex2.x >> 16) & 0xFFFFu, wg2.g, g0, q0, g1, q1);
        topDuas( inSplatTex2.y        & 0xFFFFu, wg2.b, g0, q0, g1, q1);
        topDuas((inSplatTex2.y >> 16) & 0xFFFFu, wg2.a, g0, q0, g1, q1);
        wgSum += wg2.r + wg2.g + wg2.b + wg2.a;
        vec3 acc = vec3(0.0);
        float sum = 0.0;
        if (g0 != 0u) { acc += slotTexture(g0 & 0x3FFFu, inTexCoord).rgb * q0; sum += q0; }
        if (g1 != 0u) { acc += slotTexture(g1 & 0x3FFFu, inTexCoord).rgb * q1; sum += q1; }
        float wB = max(0.0, 1.0 - wgSum);
        acc += aG * wB;
        sum += wB;
        aG = acc / max(sum, 1e-4);
    }
    if (inColor.a < 0.999) aG *= inColor.rgb;
    outAlbedo = vec4(aG, 1.0);
}
