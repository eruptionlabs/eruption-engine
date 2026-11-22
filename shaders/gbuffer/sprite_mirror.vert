#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
// Sprite no passe espelhado da agua (WaterReflectionPass). O billboard do
// G-buffer principal e' um quadro virado para a camera, inclinado para tras
// com ela; espelhado como esta' sairia achatado. Aqui o personagem e' um
// CARTAO EM PE' (vertical) na ancora, de frente para a camera na horizontal,
// com o tamanho do quadro (escala do modelo e deslocamentos do texRect, como
// em sprite_vert.glsl). Desenhado em coordenadas de mundo pela matriz do
// espelho, no mesmo G-buffer e com a mesma profundidade do cenario: o relevo
// esconde o reflexo como esconde o de uma arvore, sem desvanecer por altura.

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texCoord;
layout(location = 2) in vec4 i_modelRow0;
layout(location = 3) in vec4 i_modelRow1;
layout(location = 4) in vec4 i_modelRow2;
layout(location = 5) in vec4 i_modelRow3;
layout(location = 6) in vec4 i_anchorPoint;
layout(location = 7) in vec4 i_texRect;
layout(location = 8) in uint i_texIndex;
layout(location = 11) in uint i_flags;
layout(location = 12) in uint i_paletteIndex;
layout(location = 13) in vec4 i_tintColor;

layout(push_constant) uniform Push {
    mat4 viewProjection; // espelhada (com o plano obliquo e o x negado)
    vec4 right;          // xyz = direita do billboard, horizontal; w = mistura da normal com +Y
    vec4 facing;         // xyz = horizontal em direcao a' camera; w = altura minima da ancora
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out flat uint v_texIndex;
layout(location = 2) out flat uint v_flags;
layout(location = 3) out vec4 v_tint;
layout(location = 4) out flat uint v_paletteIndex;
layout(location = 5) out vec3 v_normal;

void main() {
    mat4 model = mat4(i_modelRow0, i_modelRow1, i_modelRow2, i_modelRow3);
    float scaleX = length(model[0].xyz);
    float scaleY = length(model[1].xyz);
    vec3 finalPos;
    vec3 normal;
    if ((i_flags & 4u) != 0u) {
        float up = -i_texRect.y + a_position.y * scaleY - i_texRect.w;
        // Reflexo de altura inteira: a ancora no fundo raso sobe ao nivel d'agua
        // (w = -1e30 no modo fisico, sem efeito).
        vec3 anchor = vec3(i_anchorPoint.x, max(i_anchorPoint.y, pc.facing.w), i_anchorPoint.z);
        finalPos = anchor + pc.right.xyz * (i_texRect.x + a_position.x * scaleX) + vec3(0.0, up, 0.0);
        normal = normalize(mix(pc.facing.xyz, vec3(0.0, 1.0, 0.0), pc.right.w));
    } else {
        // Quadro de mundo (sem billboard): como no G-buffer principal.
        vec3 r = normalize(model[0].xyz);
        vec3 u = normalize(model[1].xyz);
        finalPos = vec3(model[3]) + a_position.x * r * scaleX + a_position.y * u * scaleY;
        normal = normalize(cross(r, u));
    }
    gl_Position = pc.viewProjection * vec4(finalPos, 1.0);
    v_normal = normal;

    // O mesmo giro de U do sprite_vert.glsl (PNG/ERUPTSPR inverte o eixo X).
    v_uv = a_texCoord;
    bool flipU = (i_flags & 1u) != 0u;
    if ((i_flags & 2048u) != 0u) flipU = !flipU;
    if (flipU) v_uv.x = 1.0 - v_uv.x;
    if ((i_flags & 2u) != 0u) v_uv.y = 1.0 - v_uv.y;
    v_texIndex = i_texIndex;
    v_flags = i_flags;
    v_tint = i_tintColor;
    v_paletteIndex = i_paletteIndex;
}
