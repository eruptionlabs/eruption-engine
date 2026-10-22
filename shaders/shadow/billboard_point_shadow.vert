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
    vec4 shadowLightDir; // xyz = posicao da luz, w = opacidade
    vec2 planarParams;   // x = dilatacao do quadro
};

// SOMBRA DE LUZ PONTUAL COMO DECAL. Antes, o quadro do sprite era projetado
// da luz num plano horizontal na altura do pe' e desenhado sem teste de
// profundidade: no chao irregular a sombra pairava, atravessava pedra e, onde
// o chao descia, comecava longe do pe' ("a sombra nao sai do boneco"). Agora o
// vertice so' cobre, na tela, o volume de sombra (os 4 cantos do quadro
// estendidos da luz ate' o teto de 2x a distancia), e o fragmento le' a
// profundidade, refaz o raio luz -> ponto visivel e o corta no quadro: a
// silhueta cai sobre o relevo de verdade, parede e degrau inclusos (decal em
// espaco de tela, como o disco: billboard_circle_shadow.vert).
layout(location = 0) out flat vec4 v_light;  // xyz = luz, w = opacidade
layout(location = 1) out flat vec4 v_anchor; // xyz = ancora do quadro, w = xOff
layout(location = 2) out flat vec4 v_axis;   // xz = eixo horizontal na base do quadro, y = escala X, w = escala Y
layout(location = 3) out flat vec4 v_offs;   // x = -yOff - centerY, y = dilatacao, z = altura do pe' (relativa a ancora), w = espelho
layout(location = 4) out flat vec4 v_plane;  // plano do billboard desenhado: normal, dot(normal, ancora)
layout(location = 5) out flat uint v_texIndex;
layout(location = 6) out flat uint v_flags;
layout(location = 7) out flat vec2 v_axisTop; // eixo horizontal no alto do quadro (o da base e' v_axis.xz)

void main() {
    if ((i_flags & 4u) == 0u || (i_flags & 512u) == 0u) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        v_light = vec4(0.0);
        v_anchor = vec4(0.0);
        v_axis = vec4(0.0);
        v_offs = vec4(0.0);
        v_axisTop = vec2(0.0);
        v_plane = vec4(0.0);
        v_texIndex = i_texIndex;
        v_flags = i_flags;
        return;
    }

    float scaleX = length(i_modelRow0.xyz);
    float scaleY = length(i_modelRow1.xyz);
    float xOff = i_texRect.x;
    float yOff = i_texRect.y;
    float centerY = i_texRect.w;
    float dilation = max(planarParams.x, 0.01);
    vec3 A = i_anchorPoint.xyz;
    vec3 L = shadowLightDir.xyz;
    float pe = (i_flags & 2048u) == 0u ? -centerY : 0.0;

    // Eixo horizontal do quadro de sombra. O do sprite (de frente para a
    // camera) poe a base nos dois pes; com a luz de lado ele fica de quina
    // para ela e a sombra vira um risco, entao gira para ficar de frente para
    // a luz - so' nesse caso. O quadro e' torcido: a base fica sempre no eixo
    // do sprite desenhado e so' o alto gira (rTop). Com o quadro inteiro
    // girado, cada bota caia numa linha girada e nao sob a bota desenhada: a
    // sombra comecava 4-7 px longe da sola com a luz alta e perto. O sentido do
    // giro (perp do lado de camR) troca quando a luz cruza o lado: ali a
    // silhueta espelharia num quadro. Perto do lado (espelho > 0) a sombra
    // vira a uniao da silhueta com o espelho dela, igual nos dois sentidos, e
    // a troca nao aparece; na frente, atras e na diagonal cada parte cai do
    // mesmo lado do mundo que no sprite.
    vec3 camR = (u_inverseView * vec4(-1.0, 0.0, 0.0, 0.0)).xyz;
    camR = length(camR.xz) > 1e-3 ? normalize(vec3(camR.x, 0.0, camR.z)) : vec3(1.0, 0.0, 0.0);
    vec2 away = A.xz - L.xz;
    vec3 rTop = camR;
    float espelho = 0.0;
    if (length(away) > 1e-3) {
        vec2 a = normalize(away);
        vec3 perp = vec3(-a.y, 0.0, a.x);
        if (dot(perp, camR) < 0.0) perp = -perp;
        float lado = abs(dot(camR.xz, a));
        rTop = normalize(mix(camR, perp, smoothstep(0.5, 0.9, lado)));
        espelho = smoothstep(0.8, 0.98, lado);
    }

    // Retangulo da tela que cobre o volume de sombra: cada canto do quadro
    // (dilatado em torno da base) a 1x e a 2x da distancia ate' a luz.
    vec2 mn = vec2(1e9), mx = vec2(-1e9);
    bool atras = false;
    for (int k = 0; k < 8; ++k) {
        float cx = ((k & 1) != 0 ? 0.5 : -0.5) * dilation;
        float cy = ((k & 2) != 0 ? 1.0 : 0.0) * dilation - 0.5;
        vec3 r = (k & 2) != 0 ? rTop : camR;
        vec3 W = A + r * (xOff + cx * scaleX) + vec3(0.0, -yOff + cy * scaleY - centerY, 0.0);
        vec3 c = L + (W - L) * ((k & 4) != 0 ? 2.0 : 1.0);
        vec4 clip = viewProjection * vec4(c, 1.0);
        if (clip.w <= 1e-3) { atras = true; break; }
        vec2 ndc = clip.xy / clip.w;
        mn = min(mn, ndc);
        mx = max(mx, ndc);
    }
    if (atras) { mn = vec2(-1.0); mx = vec2(1.0); }
    gl_Position = vec4(mix(mn, mx, a_position + vec2(0.5)), 0.0, 1.0);

    // Plano do billboard desenhado (os eixos do sprite.vert): o pixel nele e'
    // o proprio sprite, que nao recebe a propria sombra.
    float tiltRad = radians(u_spriteTilt);
    vec3 upW = (u_inverseView * vec4(0.0, cos(tiltRad), -sin(tiltRad), 0.0)).xyz;
    vec3 rightW = (u_inverseView * vec4(-1.0, 0.0, 0.0, 0.0)).xyz;
    vec3 n = normalize(cross(rightW, upW));

    v_light = vec4(L, clamp(shadowLightDir.w, 0.0, 1.0));
    v_anchor = vec4(A, xOff);
    v_axis = vec4(camR.x, scaleX, camR.z, scaleY);
    v_axisTop = rTop.xz;
    v_offs = vec4(-yOff - centerY, dilation, pe, espelho);
    v_plane = vec4(n, dot(n, A));
    v_texIndex = i_texIndex;
    v_flags = i_flags;
}
