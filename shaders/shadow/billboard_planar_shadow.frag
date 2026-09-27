#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 3) in flat uint v_paletteIndex;
layout(location = 4) in float v_softness;

layout(location = 0) out vec4 outShadow;

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_viewProjection;
    mat4 u_inverseView;
    mat4 u_inverseProjection;
    vec3 u_cameraPos;
    float u_time;
    vec2 u_screenResolution;
    float u_nearPlane;
    float u_farPlane;
    uint u_frameIndex;
    uint u_debugMode;
    float u_spriteExposure;
    float u_giIntensity;
    float u_giAmbientFloor;
    float u_spriteTilt;
    float u_shadowHeightScale;
    float u_spriteNormalYMix;
    float u_normalMapScale;
    float u_normalMapInvertY;
    float u_normalSmoothing;
    float u_defaultRoughness;
    float u_defaultMetallic;
    vec4 u_sunDir;
    vec4 u_sunColor;
    vec4 u_ambientSky;
    vec4 u_ambientGround;
};

void main() {
    if ((v_flags & 4u) == 0u) discard;

    uint texIdx = nonuniformEXT(v_texIndex);
    vec4 albedo = texture(u_textures[texIdx], v_uv);

    if ((v_flags & 256u) != 0) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        albedo = vec4(1.0, 1.0, 1.0, 1.0);
    }

    if (albedo.a < 0.01) discard;

    // Keep shadow intensity strong enough to be visible after tone mapping.
    float intensity = 1.0;

    // Dark multiplicative shadow without touching the lit-image alpha,
    // because the post-processor uses alpha for transparency blending.
    // 0.75 gives a strong but not black center (25% remaining light).
    float darken = 1.0 - albedo.a * intensity * 0.75;
    outShadow = vec4(darken, darken, darken, 1.0);
}
