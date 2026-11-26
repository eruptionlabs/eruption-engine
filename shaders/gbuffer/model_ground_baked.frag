#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#define ERUPTION_SLOT_DIRECT // a imagem assada e' um render target (ligacao direta sem bindless)
// CHAO ASSADO (preset "ground_bake"), desenho: o albedo do chao vem de UMA
// leitura da imagem que model_ground_bake.frag assou (UV = a da mascara do
// splat, em espaco de mundo), no lugar das 5 do caminho LITE + GROUND_MIN
// (base, 2 mascaras, 2 camadas). Normal do vertice e material constante,
// como no GROUND_MIN. Sem spec constants; o slot da imagem vem em push.uvScale.x.
// PowerVR GE8320 (mobile_legacy a 800x360): cada leitura do chao custava ~15 ms.
layout(location = 0) in vec3 inWorldPos;
layout(location = 2) in vec3 inNormal;
layout(location = 5) in vec4 inColor;
layout(location = 13) in vec2 inBlendMaskUV;
layout(location = 18) in flat float inSway;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPBR;
layout(location = 3) out uint outMaterialID;
layout(location = 4) out vec4 outEmissive;

#include "common/slot_textures.glsl"
#include "gbuffer/model_push.glsl"
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
    if (seeThroughDiscard(inWorldPos, inNormal, inSway < -0.5)) discard;
    vec3 N = (length(inNormal) > 1e-8) ? normalize(inNormal) : vec3(0.0, 1.0, 0.0);
    vec3 aG = slotTexture(uint(push.uvScale.x + 0.5), inBlendMaskUV).rgb;
    if (inColor.a < 0.999) aG *= inColor.rgb;
    outAlbedo = vec4(aG, 1.0);
    outNormal = vec4(N * 0.5 + 0.5, 1.0);
    outPBR = vec4(0.5, 0.85, 0.0, 0.0);
    outMaterialID = uint(clamp((inWorldPos.y + 500.0) / 2000.0, 0.0, 1.0) * 255.0 + 0.5);
    outEmissive = vec4(0.0, 0.0, 0.0, 1.0);
}
