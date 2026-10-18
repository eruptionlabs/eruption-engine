// FrameUBO como as malhas o leem (model.frag e o pre-passe model_depth.frag).
// A ordem e' a do struct FrameUBO em SpriteRenderer.hpp.
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
    // Same buffer as sprite_forward.frag - the sun data is already here.
    vec4 u_sunDir;        // xyz=travel dir, w=intensity
    vec4 u_sunColor;      // xyz=color
    vec4 u_ambientSky;
    vec4 u_ambientGround;
    // POM (docs/displacement_design.md): x=heightScale (uv), y=maxSteps,
    // z=minSteps, w=wetness global (o alpha do MRAH-W agora é SÓ altura).
    vec4 u_pomParams;
    // Cauda do UBO: precisa estar declarada aqui para os campos abaixo caiveem
    // nos offsets certos (o C++ escreve nesta ordem - ver FrameUBO).
    vec4 u_windParams;
    // RELEVO POR DISTANCIA: duas curvas editadas no F2 e enviadas como LUT de
    // 8 amostras uniformes em [0, params.x] unidades de mundo.
    // params: x = distancia maxima, y = 1 liga / 0 desliga.
    vec4 u_normalDistParams;
    vec4 u_normalScaleLut0;
    vec4 u_normalScaleLut1;
    vec4 u_normalSmoothLut0;
    vec4 u_normalSmoothLut1;
    // Tesselacao (ver model.tesc/model.tese): x = distancia maxima da curva,
    // y = liga, z = amplitude, w = fator maximo.
    vec4 u_tessParams;
    vec4 u_tessLut0;
    vec4 u_tessLut1;
    // x = reservado, y = espacamento alvo entre vertices da tesselacao (ver model.tesc).
    // z = teto de inclinacao da normal em radianos (0 desliga)
    vec4 u_tessParams2;
    float u_tessHeightBlur;
    // Fim do FrameUBO (FSR): matrizes SEM jitter e o vento do frame anterior,
    // para a velocidade de objeto (vegetacao) - ver SpriteRenderer.hpp.
    mat4 u_viewProjNoJitter;
    mat4 u_prevViewProjNoJitter;
    vec4 u_jitterNdc;
    vec4 u_prevWindParams;
    // Ver atraves (camera.see_through; ver FrameUBO::seeThrough e
    // gbuffer/see_through.glsl).
    vec4 u_seeThrough;
    vec4 u_seeThrough2;
    vec4 u_seeThrough3;
};
