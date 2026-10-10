// FrameUBO da cena (o mesmo buffer dos sprites e do terreno), so' a parte
// que as particulas usam. A ordem precisa bater com FrameUBO em SpriteRenderer.hpp.
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
    vec4 u_sunDir;        // xyz = direcao em que a luz viaja, w = intensidade
    vec4 u_sunColor;
    vec4 u_ambientSky;    // xyz cor, w intensidade
    vec4 u_ambientGround;
};
