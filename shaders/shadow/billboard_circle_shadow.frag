#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in flat vec4 v_feet;   // xyz = pe', w = raio em X
layout(location = 1) in flat vec4 v_shape;  // x = raio em Z, y = maciez, z = folga acima, w = folga abaixo
layout(location = 2) in flat vec4 v_plane;  // plano do proprio billboard

layout(location = 0) out vec4 outShadow;

// Set 0 reserved for bindless textures (not used but required to match pipeline layout)
layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

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

// Profundidade da cena (so' leitura, depois do passe forward). D32 sem filtro.
layout(set = 2, binding = 0) uniform sampler2D u_sceneDepth;

void main() {
    // Ponto visivel neste pixel (decal: ver billboard_circle_shadow.vert).
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(u_sceneDepth, px, 0).r;
    if (depth >= 1.0) discard; // ceu
    vec2 uv = (vec2(px) + 0.5) / vec2(textureSize(u_sceneDepth, 0));
    vec4 viewPos = u_inverseProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 P = (u_inverseView * vec4(viewPos.xyz / viewPos.w, 1.0)).xyz;
    vec3 local = P - v_feet.xyz;

    // Distancia na elipse da pegada, na escala do quadro antigo (raio 0,5).
    float dist = 0.5 * length(vec2(local.x / v_feet.w, local.z / v_shape.x));
    if (dist > 0.5) discard;

    // O proprio sprite: o pixel no plano dele, acima do pe' (a beira do pe'
    // fica, senao abria uma fresta entre o pe' e a sombra).
    float folgaPlano = 0.04 * v_feet.w;
    if (abs(dot(v_plane.xyz, P) - v_plane.w) < folgaPlano && local.y > folgaPlano) discard;

    // Fora da faixa de altura: chao muito acima (parede) ou muito abaixo.
    float altura = smoothstep(-v_shape.w, -0.5 * v_shape.w, local.y) *
                   (1.0 - smoothstep(0.5 * v_shape.z, v_shape.z, local.y));
    if (altura <= 0.0) discard;

    // Soft transition control
    float edge = 0.5;
    float innerRadius = 0.5 * (1.0 - v_shape.y);
    float fade = (1.0 - smoothstep(innerRadius, edge, dist)) * altura;

    // Keep shadow intensity strong enough to be visible after tone mapping.
    float intensity = 1.0;

    // Dark multiplicative shadow without touching the lit-image alpha,
    // because the post-processor uses alpha for transparency blending.
    // 0.75 gives a strong but not black center (25% remaining light).
    float darken = 1.0 - fade * intensity * 0.75;
    outShadow = vec4(darken, darken, darken, 1.0);
}
