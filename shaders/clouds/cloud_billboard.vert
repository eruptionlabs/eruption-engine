#version 450

// Volumetric cloud box: a unit cube [0,1]^3 scaled to the layer's bounding
// box via push constants. The fragment shader ray-marches the volume inside.

layout(location = 0) in vec3 a_pos; // unit cube corner [0,1]

layout(location = 0) out vec3 v_worldPos;

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

// Must match cloud_billboard.frag exactly.
layout(push_constant) uniform PushConstants {
    vec4 boxMin;    // xyz used
    vec4 boxMax;    // xyz used
    float seed;
    float rainIntensity;
    float stormTint;
    float tintAltitude;
    int blobCount;
    float pad0;
    float pad1;
    float pad2;
} pc;

void main() {
    vec3 world = mix(pc.boxMin.xyz, pc.boxMax.xyz, a_pos);
    v_worldPos = world;
    gl_Position = viewProj * vec4(world, 1.0);
}
