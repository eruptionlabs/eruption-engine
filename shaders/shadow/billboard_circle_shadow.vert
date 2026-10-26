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

// DECAL NO CHAO (nao mais um quadro deitado no pe'). O quadro plano a +0,12 do
// pe' brigava com o chao (z-fight em listras) e sumia aos pedacos onde o chao
// sob a pegada nao e' plano: declive, degrau, laje mais alta que a grade. Agora
// o vertice so' cobre, na tela, a caixa da pegada com folga em altura, e o
// fragmento le' a profundidade da cena, reconstroi o ponto visivel e avalia a
// pegada NELE (decal em espaco de tela, Kim, "Screen Space Decals in Warhammer
// 40,000: Space Marine", SIGGRAPH 2012). Sem plano, nao ha' o que brigar, e a
// sombra acompanha o relevo.
layout(location = 0) out flat vec4 v_feet;   // xyz = pe', w = raio em X
layout(location = 1) out flat vec4 v_shape;  // x = raio em Z, y = maciez, z = folga acima, w = folga abaixo
layout(location = 2) out flat vec4 v_plane;  // plano do proprio billboard: normal (xyz), dot(normal, ancora)
// Direcao ao sol (xyz) e w = 1 quando o disco so' aparece fora do sol
// ("map_contact"); w = 0, o disco de sempre.
layout(location = 3) out flat vec4 v_sun;

void main() {
    v_sun = shadowLightDir;
    // Only billboards that cast shadows should render a drop shadow.
    if ((i_flags & 4u) == 0u || (i_flags & 512u) == 0u) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        v_feet = vec4(0.0);
        v_shape = vec4(0.0);
        v_plane = vec4(0.0);
        v_sun = vec4(0.0);
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

    // A PEGADA SEGUE A ALTURA, NAO A CELULA. A largura do quadro inclui o
    // que o boneco faz com os bracos - a estocada de uma lanca, a asa de um
    // dragao, a folga do recorte -, e uma sombra da largura da celula saia
    // duas, tres vezes mais larga que o corpo (medido: um cavaleiro de celula
    // 312 px para um corpo de ~120). Um corpo em pe' tem uns 55% da altura de
    // largura; a sombra fica no menor dos dois. Sprite alto e fino nao muda.
    float pegada = min(scaleX, scaleY * 0.55);

    // Elipse no plano XZ: Z comprimido (0,6) como o sprite visto de cima.
    float rx = 0.5 * pegada * dilation;
    float rz = rx * 0.6;
    // Folga em altura: o chao sob a pegada pode subir meio raio (pedra, borda
    // de laje) e descer um raio (degrau abaixo do pe').
    float acima = rx * 0.5;
    float abaixo = rx;

    // Retangulo da tela que cobre os 8 cantos da caixa.
    vec2 mn = vec2(1e9), mx = vec2(-1e9);
    bool atras = false;
    for (int k = 0; k < 8; ++k) {
        vec3 c = feetWorldPos + vec3((k & 1) != 0 ? rx : -rx, (k & 2) != 0 ? acima : -abaixo, (k & 4) != 0 ? rz : -rz);
        vec4 clip = viewProjection * vec4(c, 1.0);
        if (clip.w <= 1e-3) { atras = true; break; }
        vec2 ndc = clip.xy / clip.w;
        mn = min(mn, ndc);
        mx = max(mx, ndc);
    }
    if (atras) { mn = vec2(-1.0); mx = vec2(1.0); }
    gl_Position = vec4(mix(mn, mx, a_position + vec2(0.5)), 0.0, 1.0);

    // Plano do billboard (os mesmos eixos do sprite.vert): o pixel que cai nele
    // e' o proprio sprite, que nao recebe a propria sombra.
    float tiltRad = radians(u_spriteTilt);
    vec3 tiltedUp = vec3(0.0, cos(tiltRad), -sin(tiltRad));
    vec3 rightW = (u_inverseView * vec4(-1.0, 0.0, 0.0, 0.0)).xyz;
    vec3 upW = (u_inverseView * vec4(tiltedUp, 0.0)).xyz;
    vec3 n = normalize(cross(rightW, upW));

    v_feet = vec4(feetWorldPos, rx);
    v_shape = vec4(rz, clamp(circleParams.y, 0.0, 1.0), acima, abaixo);
    v_plane = vec4(n, dot(n, i_anchorPoint.xyz));
}
