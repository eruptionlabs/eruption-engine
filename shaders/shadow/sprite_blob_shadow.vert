#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 worldPosAndScale;
    vec4 groundNormalAndAlpha;
    uint debugMode;
} pc;

const vec2 QUAD[6] = vec2[](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
    vec2(1.0, -1.0),  vec2(1.0, 1.0),  vec2(-1.0, 1.0)
);

layout(location = 0) out vec2 outUV;
layout(location = 1) out float outAlpha;
layout(location = 2) out flat uint outDebugMode;

void main() {
    vec3 normal = normalize(pc.groundNormalAndAlpha.xyz);
    vec3 worldPos = pc.worldPosAndScale.xyz;
    float scale = pc.worldPosAndScale.w;

    vec3 tangent;
    if (abs(normal.y) > 0.999) {
        tangent = vec3(1.0, 0.0, 0.0);
    } else {
        tangent = normalize(cross(normal, vec3(0.0, 1.0, 0.0)));
    }
    vec3 bitangent = cross(normal, tangent);

    vec2 q = QUAD[gl_VertexIndex];
    vec3 localPos = tangent * q.x * scale + bitangent * q.y * scale * 0.6;

    vec4 finalWorld = vec4(worldPos + localPos, 1.0);
    finalWorld.y += 0.05;

    vec4 clip = pc.viewProj * finalWorld;
    gl_Position = vec4(clip.xy, clip.w * 0.5, clip.w);

    outUV = q * 0.5 + 0.5;
    outAlpha = pc.groundNormalAndAlpha.w;
    outDebugMode = pc.debugMode;
}
