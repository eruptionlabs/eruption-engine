#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require
// Sprite no passe espelhado da agua (ver sprite_mirror.vert): o mesmo corte
// de alfa em 0,5, a paleta e a tinta do sprite.frag, sem normal abaulada,
// luz de borda nem normal map - o reflexo e' pequeno e desfocado pela
// agua. Escreve o G-buffer que o reflection_composite.frag acende com sol e
// ambiente, como faz com os modelos.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 3) in vec4 v_tint;
layout(location = 4) in flat uint v_paletteIndex;
layout(location = 5) in vec3 v_normal;

#define ERUPTION_SLOT_NEAREST // atlas de sprite e paleta sem filtro
#include "common/slot_textures.glsl"

layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_pbr;
layout(location = 3) out uint o_materialId;
layout(location = 4) out vec4 o_emissive;

// Fade por pontilhado (characters.fade): o mesmo Bayer 4x4 do sprite.frag.
bool fadeCut(float a) {
    const float bayer[16] = float[](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                    3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    return a < (bayer[p.y * 4 + p.x] + 0.5) / 16.0;
}

void main() {
    vec4 albedo = slotTexture(v_texIndex, v_uv);
    if ((v_flags & 256u) != 0u) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        albedo = slotTexture(v_paletteIndex, vec2((idx + 0.5) / 256.0, 0.5));
        albedo.a = 1.0;
    }
    if (albedo.a < 0.5 || fadeCut(v_tint.a)) discard;
    albedo.rgb *= v_tint.rgb;

    o_albedo = vec4(albedo.rgb, 1.0); // a = AO do composite (1 = sem oclusao)
    o_normal = vec4(normalize(v_normal) * 0.5 + 0.5, 1.0);
    o_pbr = vec4(0.75, 1.0, 0.0, 0.0); // marca de sprite, so' difuso
    o_materialId = (v_flags >> 3) & 0x1Fu;
    o_emissive = vec4(0.0, 0.0, 0.0, 1.0);
}
