// shaders/common/uniforms.glsl
// Shared structures for Eruption Engine shaders

// Per-frame UBO
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
};

// Material SSBO
struct Material {
    vec4 albedoTint;
    float roughness;
    float metallic;
    float emissive;
    uint albedoTex;
    uint normalTex;
    uint roughnessTex;
    uint metallicTex;
};
layout(set = 0, binding = 1, std430) readonly buffer Materials {
    Material u_materials[];
};

// Light SSBO
struct Light {
    vec3 position;
    float radius;
    vec3 color;
    float intensity;
    vec3 direction;
    float innerCone;
    float outerCone;
    uint type;        // 0=point, 1=directional, 2=spot
    uint shadowIndex;
    uint _pad[2];
};
layout(set = 0, binding = 2, std430) readonly buffer Lights {
    Light u_lights[];
};
