// Corpo do fragmento das particulas de malha. Tres saidas:
//   G-buffer (padrao), sem o alvo de velocidade (ERUPTION_NO_VELOCITY) e
//   forward com luz simples (VFX_MESH_FORWARD, previa do editor).
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in uint v_tex;
layout(location = 4) in vec3 v_worldPos;
layout(location = 5) in vec4 v_clip;
layout(location = 6) in vec4 v_prevClip;
layout(location = 7) flat in float v_emissive;

#ifdef VFX_MESH_FORWARD
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_reactive;
#else
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPBR;
layout(location = 3) out uint outMaterialID;
layout(location = 4) out vec4 outEmissive;
#ifndef ERUPTION_NO_VELOCITY
layout(location = 5) out vec2 outVelocity;
#endif
#endif

void main() {
    // Some sumindo por pontilhado (malha e' opaca: sem mistura).
    const float bayer[16] = float[](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 px = ivec2(gl_FragCoord.xy) & 3;
    if (v_color.a < (bayer[px.y * 4 + px.x] + 0.5) / 16.0) discard;
    vec3 albedo = v_color.rgb;
    if (v_tex != 0xFFFFFFFFu) albedo *= texture(u_textures[nonuniformEXT(v_tex)], v_uv).rgb;
    vec3 N = normalize(v_normal);
#ifdef VFX_MESH_FORWARD
    // Previa sem escrita de profundidade: face de tras some (malhas convexas).
    if (dot(N, frame.camPos.xyz - v_worldPos) < 0.0) discard;
#endif
    if (!gl_FrontFacing) N = -N;
    // Brilho proprio acima de 1 (brasa, magma): o resto vira emissivo.
    vec3 glow = albedo * max(v_emissive - 1.0, 0.0) * 0.5;
#ifdef VFX_MESH_FORWARD
    vec3 L = -normalize(frame.sunDir.xyz);
    vec3 light = frame.ambient.rgb * frame.ambient.w + frame.sunColor.rgb * frame.sunDir.w * max(dot(N, L), 0.0);
    o_color = vec4(albedo * light + glow, 1.0);
    o_reactive = vec4(0.0);
#else
    outAlbedo = vec4(pow(clamp(albedo, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0); // o G-buffer guarda sRGB
    outNormal = vec4(N * 0.5 + 0.5, 1.0);
    outPBR = vec4(1.0, 0.8, 0.0, 0.0); // fonte real, rugosa, nao metal, seca
    outMaterialID = uint(clamp((v_worldPos.y + 500.0) / 2000.0, 0.0, 1.0) * 255.0 + 0.5);
    outEmissive = vec4(min(glow, vec3(1.0)), 1.0);
#ifndef ERUPTION_NO_VELOCITY
    outVelocity = (v_prevClip.xy / v_prevClip.w - v_clip.xy / v_clip.w) * 0.5;
#endif
#endif
}
