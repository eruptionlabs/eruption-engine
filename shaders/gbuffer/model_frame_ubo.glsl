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
    float u_framePad0; // reservado (era o vies de mip da altura; o blur esta' nos mips)
    // Banda perto da grade fina (model.tesc/model.tese, FrameUBO::tessNear*).
    float u_tessNearEdgePixels;
    float u_tessNearDistance;
    float u_tessNearSpacing;
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
    // Juncao malha x terreno (FrameUBO::gb*, gbuffer/ground_blend.glsl).
    vec4 u_gbMaskU;
    vec4 u_gbMaskV;
    vec4 u_gbUvU;
    vec4 u_gbUvV;
    vec4 u_gbHeight;
    uvec4 u_gbSplat;
    uvec4 u_gbSlots;
    uvec4 u_gbSlots2;
    // Recorte por volume (FrameUBO::cutaway, gbuffer/see_through.glsl).
    vec4 u_cutaway[12];
    // Faixa do ver atraves (FrameUBO::seeThroughBand, gbuffer/see_through.glsl).
    vec4 u_seeThroughBand;
    // Elipse por personagem perto do foco (FrameUBO::seeThroughExtra).
    vec4 u_seeThroughExtra[12];
    // x = forca do alfa-teste por hash (gbuffer/alpha_hash.glsl), 0 desliga.
    vec4 u_alphaHash;
    // x = espacamento minimo (u) do filtro da altura (FrameUBO::reliefParams);
    // y = relief_cavity, z = relief_cavity_contrast, w = normal_strength.
    vec4 u_reliefParams;
    // Sprites (FrameUBO::spriteParams): x = sprite_filter (0 nearest, 1
    // bilinear nitido), y = sprite_normal_strength (normal map autoral).
    vec4 u_spriteParams;
    // Luz de borda do lado do sol (FrameUBO::spriteRim): rgb = tinta, w =
    // forca (0 desliga; characters.set_rim vezes a chave sprite_rim do preset).
    vec4 u_spriteRim;
    // Debug de spline no mapa (FrameUBO::splineDbg): [0] = liga, n pontos,
    // maior valor, eixo maximo; [1..2] = x dos pontos; [3..4] = curva em 8
    // amostras; [5..12] = cor de cada ponto.
    vec4 u_splineDbg[13];
    // POM do chao por preset (FrameUBO::reliefPom/reliefPom2): x = espessura
    // da camada (u; 0 desliga), y = passos maximos, z = minimos, w = distancia
    // onde some. reliefPom2: x = contraste da altura, y = escurecimento da fresta,
    // z = filtro da grade, w = relief_sun_emboss (lado do sol claro, oposto escuro).
    vec4 u_reliefPom;
    vec4 u_reliefPom2;
    // Relevo sem tesselacao (so' fragmento; 0 = desliga, saida identica):
    // u_reliefPom3: x = relief_height_normal (espessura u da altura que vira
    // normal), y = relief_self_shadow (forca), z = passos da marcha ao sol,
    // w = relief_fragment_distance (u onde a normal da altura e a auto-sombra somem).
    // u_reliefPom4: x = relief_pom_near_boost (espessura extra do POM colado na
    // camera), y = relief_pom_near_distance (u), z = relief_self_shadow_height
    // (espessura u da camada), w = relief_self_shadow_length (alcance maximo u).
    vec4 u_reliefPom3;
    vec4 u_reliefPom4;
    // Calibracao de albedo (FrameUBO::albedoCalib, gbuffer/albedo_calib.glsl).
    vec4 u_albedoCalib;
    // Brilho dos objetos perto de luzes (FrameUBO::glow*): u_glowInfo.x = quantas
    // das 8 valem (0 desliga), u_glowPos = xyz + raio, u_glowColor = rgb + forca.
    vec4 u_glowInfo;
    vec4 u_glowPos[8];
    vec4 u_glowColor[8];
    // Cintilacao da emissiva das regioes (FrameUBO::emiFlicker): pares (slot,
    // fator), dois por vec4; u_emiFlickerInfo.x = quantos pares valem.
    vec4 u_emiFlickerInfo;
    vec4 u_emiFlicker[32];
    // Rajadas locais de vento (so' o vertex shader de vegetacao le).
    vec4 u_gustInfo;
    vec4 u_gusts[16];
    // Tempo de cada rajada, para a mola da copa: (duracao s, idade s, 0, 0).
    vec4 u_gustsT[8];
    vec4 u_gustsX[8]; // (giro, para dentro, escala radial, 0) por rajada
};
