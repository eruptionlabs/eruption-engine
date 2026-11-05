#version 450
#extension GL_GOOGLE_include_directive : require
// Sombra instanciada de caster SOLIDO pelo stream de posicao (spec 52, OE-03):
// 12 B por vertice (entrada 0) em vez do vertice inteiro de 88 B. Sem fragment
// shader (ShadowRenderer::m_shadowInstStreamOpaquePipeline).

layout(location = 0) in vec3 inPosition;

#include "shadow/shadow_inst_common.glsl"

void main() {
    gl_Position = shadowClipPos(inPosition);
}
