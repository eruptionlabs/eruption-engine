#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in flat uint inTexIndex;

#include "common/slot_textures.glsl"

void main() {
    uint texIdx = nonuniformEXT(inTexIndex);
    vec4 texColor = slotTexture(texIdx, inTexCoord);
    if (texColor.a < 0.5) discard;
    // Depth is written automatically
}
