#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 worldPosAndScale;
    vec4 groundNormalAndAlpha;
    uint debugMode;
} pc;

layout(location = 0) in vec2 inUV;
layout(location = 1) in float inAlpha;
layout(location = 2) in flat uint inDebugMode;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(float(inDebugMode), 0.0, 0.0, 1.0);
}
