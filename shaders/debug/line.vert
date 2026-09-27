#version 450
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform PushConstants { mat4 viewProj; } pc;
void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    outColor = vec4(inColor, 1.0);
}
