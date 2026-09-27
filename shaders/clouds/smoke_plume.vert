#version 450

// Coluna de fumaça: mesmo caminho da nuvem local volumétrica (cubo unitário
// escalado por push constants, ray-march analítico no fragment). Reusa o
// mesmo VBO de cubo e o mesmo pipeline layout do cloud_billboard.

layout(location = 0) in vec3 a_pos; // canto do cubo unitário [0,1]

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

// Precisa bater EXATAMENTE com smoke_plume.frag.
layout(push_constant) uniform PushConstants {
    vec4 boxMin;   // xyz = AABB da pluma
    vec4 boxMax;
    vec4 origin;   // xyz = boca do emissor, w = raio da base
    vec4 shape;    // x = altura, y = spread, z = subida (m/s), w = densidade
    vec4 wind;     // xy = vento advectivo (m/s), z = turbulência (1/m), w = tempo
    vec4 color;    // rgb = cor base, a = brasa na base
    vec4 misc;     // x = seed, y = escala de passos, z = targetScale, w = flags
} pc;

void main() {
    vec3 world = mix(pc.boxMin.xyz, pc.boxMax.xyz, a_pos);
    v_worldPos = world;
    gl_Position = viewProj * vec4(world, 1.0);
}
