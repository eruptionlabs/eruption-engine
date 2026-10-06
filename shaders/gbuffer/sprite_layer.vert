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

// CAMADA DE SPRITES (FSR): mesmas contas do sprite.vert, mas a vista e a
// projecao vem por push constant, SEM jitter - o sprite e' desenhado depois do
// FSR, direto na resolucao de display, e nao pode tremer com o jitter. O resto
// do FrameUBO (tilt, mistura de normal) e' o mesmo do G-buffer.
layout(push_constant) uniform LayerPush {
    mat4 p_view;
    mat4 p_proj;
    vec4 p_sizes;   // xy = resolucao de render, zw = resolucao de display
    vec4 p_planes;  // x = near, y = far
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out flat uint v_texIndex;
layout(location = 2) out flat uint v_flags;
layout(location = 3) out vec4 v_tint;
layout(location = 4) out vec3 v_worldPos;
layout(location = 5) out vec3 v_normal;
layout(location = 6) out flat uint v_paletteIndex;
layout(location = 7) out flat uint v_normalTexIndex;
layout(location = 8) out flat uint v_mrahwTexIndex;

void main() {
    mat4 model = mat4(i_modelRow0, i_modelRow1, i_modelRow2, i_modelRow3);

    float scaleX = length(model[0].xyz);
    float scaleY = length(model[1].xyz);

    vec3 finalPos;
    vec3 normal;

    // bit 2: Billboard (value 4)
    if ((i_flags & 4u) != 0) {
        vec4 anchorView = p_view * vec4(i_anchorPoint.xyz, 1.0);

        vec3 viewRight = vec3(-1.0, 0.0, 0.0);
        vec3 viewUp    = vec3(0.0, 1.0, 0.0);

        float tiltRad = radians(u_spriteTilt);
        vec3 tiltedUp = viewUp * cos(tiltRad) + vec3(0.0, 0.0, -1.0) * sin(tiltRad);

        float xOff = i_texRect.x;
        float yOff = i_texRect.y;
        float centerY = i_texRect.w;

        vec3 localPos = viewRight * (xOff + a_position.x * scaleX) +
                        tiltedUp  * (-yOff + a_position.y * scaleY - centerY);

        vec4 viewPos = anchorView + vec4(localPos, 0.0);

        finalPos = (inverse(p_view) * viewPos).xyz;

        vec3 camNormal = normalize((inverse(p_view) * vec4(0.0, 0.0, 1.0, 0.0)).xyz);
        vec3 skyNormal = vec3(0.0, 1.0, 0.0);
        normal = normalize(mix(camNormal, skyNormal, u_spriteNormalYMix));

        gl_Position = p_proj * viewPos;
        gl_Position.z -= 0.0004 + i_anchorPoint.w * 0.0001;
    } else {
        vec3 worldPos = vec3(model[3]);
        vec3 right = normalize(model[0].xyz);
        vec3 up = normalize(model[1].xyz);
        finalPos = worldPos + (a_position.x * right * scaleX + a_position.y * up * scaleY);
        normal = normalize(cross(right, up));
    }

    v_normal = normal;
    v_worldPos = finalPos;

    if ((i_flags & 4u) == 0u) {
        gl_Position = p_proj * p_view * vec4(finalPos, 1.0);
        gl_Position.z -= i_anchorPoint.w * 0.00001;
    }

    v_uv = a_texCoord;

    if ((i_flags & 1u) != 0) v_uv.x = 1.0 - v_uv.x;
    if ((i_flags & 2u) != 0) v_uv.y = 1.0 - v_uv.y;

    v_texIndex = i_texIndex;
    v_normalTexIndex = i_normalTexIndex;
    v_mrahwTexIndex = i_mrahwTexIndex;
    v_flags = i_flags;
    v_tint = i_tintColor;
    v_paletteIndex = i_paletteIndex;
}
