#version 450
#extension GL_GOOGLE_include_directive : require
// Sombra instanciada com TESTE DE ALFA pelos streams (spec 52, OE-03): posicao
// (entrada 0, 12 B) + UV e texIndex/matId (entrada 2, 12 B) em vez do vertice
// inteiro de 88 B. Mesmo shadow.frag.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 3) in uvec2 inTexMat; // texIndex, matId (R16G16)

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out flat uint outTexIndex;

#include "shadow/shadow_inst_common.glsl"

void main() {
    gl_Position = shadowClipPos(inPosition);
    outTexCoord = inTexCoord;
    outTexIndex = inTexMat.x;
}
