#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in flat uint inTexIndex;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

void main() {
    uint texIdx = nonuniformEXT(inTexIndex);
    vec4 texColor = texture(u_textures[texIdx], inTexCoord);
    if (texColor.a < 0.5) discard;
    // Depth is written automatically
}
