#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texCoord;

layout(location = 2) in vec4 i_modelRow0;
layout(location = 3) in vec4 i_modelRow1;
layout(location = 4) in vec4 i_modelRow2;
layout(location = 5) in vec4 i_modelRow3;
layout(location = 6) in vec4 i_anchorPoint;
layout(location = 7) in vec4 i_texRect;
layout(location = 8) in uint i_texIndex;
layout(location = 9) in uint i_normalTexIndex;
layout(location = 10) in uint i_mrahwTexIndex;
layout(location = 11) in uint i_flags;
layout(location = 12) in uint i_paletteIndex;
layout(location = 13) in vec4 i_tintColor;

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

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec4 shadowLightDir;
    vec2 circleParams; // x = dilation/radius, y = softness
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out flat uint v_flags;
layout(location = 2) out float v_softness;

void main() {
    // Only billboards that cast shadows should render a drop shadow.
    if ((i_flags & 4u) == 0u || (i_flags & 512u) == 0u) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        v_uv = vec2(0.0);
        v_flags = i_flags;
        v_softness = 0.0;
        return;
    }

    float scaleX = length(vec3(i_modelRow0.x, i_modelRow0.y, i_modelRow0.z));
    float scaleY = length(vec3(i_modelRow1.x, i_modelRow1.y, i_modelRow1.z));
    if (scaleX < 0.001) scaleX = 1.0;
    if (scaleY < 0.001) scaleY = 1.0;

    float centerY = i_texRect.w;

    // Recover the foot position on the ground plane.  Legacy sprites store
    // centerY in texRect.w, so subtract it from the anchor to reach the feet.
    // PNG billboards are already anchored at the feet, so no offset is needed.
    vec3 feetWorldPos = i_anchorPoint.xyz;
    if ((i_flags & 2048u) == 0u) {
        feetWorldPos.y -= centerY;
    }

    float dilation = max(circleParams.x, 0.01);
    vec3 projectedPos = feetWorldPos;

    // Build a flat circle on the XZ ground plane.  Z is compressed (0.6) to
    // match the isometric angle used by the visible sprite.
    projectedPos.x += a_position.x * scaleX * dilation;
    projectedPos.z += a_position.y * scaleX * dilation * 0.6;
    // Lift the shadow above the ground so it passes the depth test against
    // the terrain instead of fighting with it.  The value is tuned to give
    // the shadow the same depth priority as the sprite that casts it.
    projectedPos.y += 0.12;

    gl_Position = viewProjection * vec4(projectedPos, 1.0);

    v_uv = a_position + vec2(0.5); // Map to [0, 1] range
    v_flags = i_flags;
    v_softness = clamp(circleParams.y, 0.0, 1.0);
}
