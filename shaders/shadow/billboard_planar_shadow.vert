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
// Opacidade desta sombra (1 com o sol; a fracao da luz com luz pontual).
layout(location = 5) out float v_strength;

void main() {
    // Only billboards that cast shadows should render a drop shadow.
    if ((i_flags & 4u) == 0u || (i_flags & 512u) == 0u) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        v_uv = vec2(0.0);
        v_texIndex = i_texIndex;
        v_flags = i_flags;
        v_paletteIndex = i_paletteIndex;
        v_softness = 0.0;
        v_strength = 0.0;
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

    // Encolhe/estica em torno do PE' (base do quadro), nao do centro: com o
    // centro, 0,94 levantava a base ~3% da altura do sprite, e a luz pontual
    // baixa e distante esticava essa folga no chao - a sombra comecava 4-5 u
    // longe do pe', com um vao entre os dois.
    float dilation = max(planarParams.x, 0.01);
    vec2 localUV = vec2(a_position.x * dilation, (a_position.y + 0.5) * dilation - 0.5);

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

    v_strength = 1.0;

    // SOMBRA DE LUZ PONTUAL (shadowLightDir.w < 0: xyz = posicao da luz,
    // -w = opacidade). A sombra planar de Blinn ("Me and My (Fake) Shadow",
    // IEEE CG&A, 1988) com projecao central em vez de paralela: cada vertice
    // vai para onde o raio luz->vertice corta o plano do pe. O quadro do
    // sprite e' remontado de frente para a LUZ (e nao para a camera): de
    // lado para ela a silhueta projetada virava um risco.
    if (shadowLightDir.w < 0.0) {
        vec3 L = shadowLightDir.xyz;
        vec2 away = feetWorldPos.xz - L.xz;
        vec3 rightW = length(away) > 1e-3 ? normalize(vec3(-away.y, 0.0, away.x))
                                          : normalize((u_inverseView * vec4(viewRight, 0.0)).xyz);
        vec3 w = i_anchorPoint.xyz + rightW * (xOff + localUV.x * scaleX) +
                 vec3(0.0, 1.0, 0.0) * (-yOff + localUV.y * scaleY - centerY);
        float above = max(L.y - groundY, 1.0);
        float h = max(w.y - groundY, 0.0);
        // Luz abaixo do topo do sprite (tocha na altura do peito): a sombra
        // iria ao infinito. O teto de 2x a distancia luz-vertice segura o
        // comprimento: com 5x ela virava um risco fino atravessando a sala, e
        // com 3x a silhueta animada e comprida tremulava no chao (bancada do
        // jogo: +32% de pixel trocando de sinal contra so' o disco).
        float t = above / max(above - h, above / 2.0);
        projectedPos = vec3(L.x + (w.x - L.x) * t, groundY + 0.05, L.z + (w.z - L.z) * t);
        // Clareia do pe' para a ponta (80% mais clara no topo do sprite): a
        // sombra de luz proxima se espalha com a distancia ao objeto.
        v_strength = clamp(-shadowLightDir.w, 0.0, 1.0) * (h > 1.0 ? 0.2 : 1.0);
    }
    v_softness = clamp(planarParams.y, 0.0, 1.0);

    gl_Position = viewProjection * vec4(projectedPos, 1.0);

    v_uv = a_texCoord;
    // Sprite PNG/ERUPTSPR (bit 11): o eixo X do billboard é invertido
    // (convenção herdada), então a arte sairia espelhada; FlipX inverte de novo.
    bool flipU = (i_flags & 1u) != 0;
    if ((i_flags & 2048u) != 0) flipU = !flipU;
    if (flipU) v_uv.x = 1.0 - v_uv.x;
    if ((i_flags & 2u) != 0) v_uv.y = 1.0 - v_uv.y;

    v_texIndex = i_texIndex;
    v_flags = i_flags;
    v_paletteIndex = i_paletteIndex;
}
