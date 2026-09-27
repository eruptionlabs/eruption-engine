#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable

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
    mat4 lightSpaceMatrix;
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out flat uint v_texIndex;
layout(location = 2) out flat uint v_flags;
layout(location = 3) out flat uint v_paletteIndex;

void main() {
    mat4 model = mat4(i_modelRow0, i_modelRow1, i_modelRow2, i_modelRow3);
    
    vec3 worldPos = vec3(model[3]);
    float scaleX = length(model[0].xyz);
    float scaleY = length(model[1].xyz);
    
    vec3 finalPos;
    
    if ((i_flags & 4u) != 0) {
        // BILLBOARDS DO NOT CAST SHADOWS INTO THE DIRECTIONAL SHADOW MAP.
        // They use a separate Projected Planar Shadow pass instead.
        // We emit a degenerate triangle so nothing is drawn.
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        v_uv = vec2(0.0);
        v_texIndex = i_texIndex;
        v_flags = i_flags;
        v_paletteIndex = i_paletteIndex;
        return;
    }
    
    vec3 right = normalize(model[0].xyz);
    vec3 up = normalize(model[1].xyz);
    finalPos = worldPos + (a_position.x * right * scaleX + a_position.y * up * scaleY);
    
    gl_Position = lightSpaceMatrix * vec4(finalPos, 1.0);

    v_uv = a_texCoord;

    if ((i_flags & 1u) != 0) v_uv.x = 1.0 - v_uv.x;
    if ((i_flags & 2u) != 0) v_uv.y = 1.0 - v_uv.y;

    v_texIndex = i_texIndex;
    v_flags = i_flags;
    v_paletteIndex = i_paletteIndex;
}
