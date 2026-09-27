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
    mat4 viewProjection;
    vec4 shadowLightDir;
    vec2 planarParams; // x = dilation, y = softness
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out flat uint v_texIndex;
layout(location = 2) out flat uint v_flags;
layout(location = 3) out flat uint v_paletteIndex;
layout(location = 4) out float v_softness;

void main() {
    // Only billboards that cast shadows should render a drop shadow.
    if ((i_flags & 4u) == 0u || (i_flags & 512u) == 0u) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        v_uv = vec2(0.0);
        v_texIndex = i_texIndex;
        v_flags = i_flags;
        v_paletteIndex = i_paletteIndex;
        v_softness = 0.0;
        return;
    }

    float scaleX = length(vec3(i_modelRow0.x, i_modelRow0.y, i_modelRow0.z));
    float scaleY = length(vec3(i_modelRow1.x, i_modelRow1.y, i_modelRow1.z));

    // Same view-space axes and tilt used in sprite.vert.
    vec3 viewRight = vec3(-1.0, 0.0, 0.0);
    vec3 viewUp    = vec3(0.0, 1.0, 0.0);

    float tiltRad = radians(u_spriteTilt);
    vec3 tiltedUp = viewUp * cos(tiltRad) + vec3(0.0, 0.0, -1.0) * sin(tiltRad);

    float xOff = i_texRect.x;
    float yOff = i_texRect.y;
    float centerY = i_texRect.w;

    float dilation = max(planarParams.x, 0.01);
    vec2 localUV = a_position * dilation;

    // Build local offset in VIEW space using the SAME math as sprite.vert
    vec3 localPos = viewRight * (xOff + localUV.x * scaleX) +
                    tiltedUp  * (-yOff + localUV.y * scaleY - centerY);

    vec4 anchorView = u_view * vec4(i_anchorPoint.xyz, 1.0);
    vec4 viewPos    = anchorView + vec4(localPos, 0.0);
    vec3 worldPos   = (u_inverseView * viewPos).xyz;

    // Recover the foot position on the ground plane. PNG billboards are already
    // anchored at the feet; legacy sprites store centerY in texRect.w.
    vec3 feetWorldPos = i_anchorPoint.xyz;
    if ((i_flags & 2048u) == 0u) {
        feetWorldPos.y -= centerY;
    }
    float groundY = feetWorldPos.y;

    // PROJECTED PLANAR SHADOW:
    // Project onto the horizontal plane that passes through the sprite's feet,
    // using a light-direction shear (oblique projection).
    vec3 lightDir = normalize(shadowLightDir.xyz);
    float denom = lightDir.y;
    if (abs(denom) < 0.001) denom = sign(denom) * 0.001;

    vec3 projectedPos = worldPos;
    projectedPos.y = groundY + 0.05; // Slightly above ground to avoid z-fighting
    projectedPos.xz -= lightDir.xz * (worldPos.y - groundY) / denom;

    v_softness = clamp(planarParams.y, 0.0, 1.0);

    gl_Position = viewProjection * vec4(projectedPos, 1.0);

    v_uv = a_texCoord;
    if ((i_flags & 1u) != 0) v_uv.x = 1.0 - v_uv.x;
    if ((i_flags & 2u) != 0) v_uv.y = 1.0 - v_uv.y;

    v_texIndex = i_texIndex;
    v_flags = i_flags;
    v_paletteIndex = i_paletteIndex;
}
