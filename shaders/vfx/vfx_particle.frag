#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : enable
// Pixel da particula: folha de quadros (com mistura), particula suave
// (some ao encostar na geometria, sem a linha de corte) e saida
// premultiplicada - aditiva e so' alfa 0, entao tudo cabe num draw.
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(location = 0) in vec2 v_uv0;
layout(location = 1) in vec2 v_uv1;
layout(location = 2) in vec4 v_color;
layout(location = 3) in float v_frameMix;
layout(location = 4) in float v_viewZ;
layout(location = 5) flat in uvec2 v_texBlend;
layout(location = 6) in float v_soft;
layout(location = 7) in vec2 v_corner;

layout(location = 0) out vec4 o_color;

layout(push_constant) uniform Push {
    uint listOffset;
    float depthScale;
} pc;

void main() {
    vec4 tex;
    if (v_texBlend.x == 0xFFFFFFFFu) {
        // Sem textura: disco suave (queda quadratica ate' a borda).
        float r2 = dot(v_corner, v_corner);
        float a = clamp(1.0 - r2, 0.0, 1.0);
        tex = vec4(1.0, 1.0, 1.0, a * a);
    } else {
        uint slot = v_texBlend.x;
        tex = texture(u_textures[nonuniformEXT(slot)], v_uv0);
        if (v_frameMix > 0.0) tex = mix(tex, texture(u_textures[nonuniformEXT(slot)], v_uv1), v_frameMix);
    }
    vec4 c = v_color * tex;

    // Meia resolucao nao tem teste de profundidade por hardware: o pixel
    // compara com a profundidade cheia no centro dele (fumaca e' suave).
    bool lowRes = pc.depthScale > 1.5;
    if (v_soft > 0.0 || lowRes) {
        float d = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy * pc.depthScale), 0).r;
        if (d < 1.0) {
            float n = u_nearPlane, f = u_farPlane;
            float sceneZ = n * f / (f - d * (f - n));
            // v_soft ja' em unidades do mundo
            if (v_soft > 0.0) c.a *= clamp((sceneZ - v_viewZ) / v_soft, 0.0, 1.0);
            else if (sceneZ < v_viewZ) discard;
        }
    }
    if (c.a <= 0.002) discard;
    // Saida premultiplicada. Aditiva: alfa 0 (soma sem escurecer o fundo).
    // "premultiplied": a textura ja' vem multiplicada pelo proprio alfa.
    if (v_texBlend.y == 1u) o_color = vec4(c.rgb * v_color.a, c.a);
    else o_color = vec4(c.rgb * c.a, v_texBlend.y == 0u ? 0.0 : c.a);
}
