#version 450

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 outWorldPos;

// Bloco por draw no set 1 (PushRing, UBO de offset dinamico): passa de 128 B.
layout(std140, set = 1, binding = 0) uniform PushConstants {
    vec3 lightPos;
    float radius;
    vec3 lightColor;
    float intensity;
    uint shadowCubemapIndex;
    mat4 invViewProj;
    mat4 viewProj;
    mat4 modelMatrix;
};

void main() {
    vec3 worldPos = (modelMatrix * vec4(inPosition, 1.0)).xyz;
    gl_Position = viewProj * vec4(worldPos, 1.0);
    outWorldPos = worldPos;
}
