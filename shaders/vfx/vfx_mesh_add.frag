#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : enable
// MALHA ADITIVA (coluna de luz, anel de energia): soma luz no lit, sem luz da
// cena e sem escrever profundidade, como a particula aditiva. No G-buffer ela
// saia opaca e iluminada (um cilindro marrom de dia). Mistura do passe e'
// premultiplicada: alfa 0 na saida = soma pura.
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

#include "common/slot_textures.glsl"

layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in uint v_tex;
layout(location = 7) flat in float v_emissive;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_reactive;

void main() {
    vec4 c = v_color;
    if (v_tex != 0xFFFFFFFFu) c *= slotTexture(v_tex, v_uv);
    // Brilho proprio do emissor ("emissive"), igual ao sprite aditivo.
    vec3 rgb = c.rgb * c.a * max(v_emissive, 0.0);
    o_color = vec4(rgb, 0.0);
    // Mascara reativa (TAA/upscaler): o quanto a malha pesa no pixel.
    o_reactive = vec4(clamp(max(rgb.r, max(rgb.g, rgb.b)), 0.0, 1.0));
}
