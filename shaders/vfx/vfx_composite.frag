#version 450
#extension GL_GOOGLE_include_directive : require
// Compoe a camada de particulas de meia resolucao (cor premultiplicada +
// cobertura) por cima da cena em resolucao cheia.
#define VFX_SET 0
#include "vfx/vfx_common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = texture(lowResColor, v_uv);
}
