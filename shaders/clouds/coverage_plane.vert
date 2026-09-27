#version 450

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    mat4 invViewProj;
    vec3 cameraPos;
    float time;
    vec3 sunDir;
    float sunIntensity;
    vec2 screenSize;
    uint frameIndex;
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_worldPos;
layout(location = 2) out vec3 v_viewDir;

void main() {
    v_uv = a_uv;
    v_worldPos = a_pos;
    v_viewDir = a_pos - cameraPos;
    gl_Position = viewProj * vec4(a_pos, 1.0);
}
