#version 450
#extension GL_EXT_nonuniform_qualifier : enable
// Cobertura dos sprites na resolucao de RENDER (1 = pixel de sprite visivel),
// desenhada com o MESMO vertex shader do G-buffer e teste de depth contra o
// depth do G-buffer. Vira a mascara reativa do FSR: ali o FSR nao usa
// historico, porque o sprite e' redesenhado nitido depois (sprite_layer.frag)
// e o historico so' deixaria rastro de um sprite que se moveu.
// A logica de descarte e' a do sprite.frag: tem que bater pixel a pixel.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 6) in flat uint v_paletteIndex;

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

layout(location = 0) out float o_mask;

void main() {
    vec4 albedo = texture(nonuniformEXT(u_textures[v_texIndex]), v_uv);
    if ((v_flags & 256u) != 0) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        albedo.a = 1.0;
    }
    if (albedo.a < 0.01) discard;
    o_mask = 1.0;
}
