#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_flags;
layout(location = 2) in float v_softness;

layout(location = 0) out vec4 outShadow;

// Set 0 reserved for bindless textures (not used but required to match pipeline layout)
layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

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
    // Distance from the center of the quad (0.5, 0.5)
    float dist = length(v_uv - vec2(0.5));

    // Max radius of the circle is 0.5
    if (dist > 0.5) {
        discard;
    }

    // Soft transition control
    float edge = 0.5;
    float innerRadius = 0.5 * (1.0 - v_softness);
    float fade = 1.0 - smoothstep(innerRadius, edge, dist);

    // Keep shadow intensity strong enough to be visible after tone mapping.
    float intensity = 1.0;

    // Dark multiplicative shadow without touching the lit-image alpha,
    // because the post-processor uses alpha for transparency blending.
    // 0.75 gives a strong but not black center (25% remaining light).
    float darken = 1.0 - fade * intensity * 0.75;
    outShadow = vec4(darken, darken, darken, 1.0);
}
