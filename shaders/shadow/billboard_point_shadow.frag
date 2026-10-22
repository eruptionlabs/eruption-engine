#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// Decal da sombra de luz pontual: ver billboard_point_shadow.vert.
layout(location = 0) in flat vec4 v_light;
layout(location = 1) in flat vec4 v_anchor;
layout(location = 2) in flat vec4 v_axis;
layout(location = 3) in flat vec4 v_offs;
layout(location = 4) in flat vec4 v_plane;
layout(location = 5) in flat uint v_texIndex;
layout(location = 6) in flat uint v_flags;
layout(location = 7) in flat vec2 v_axisTop;

layout(location = 0) out vec4 outShadow;

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

// Cobertura do sprite neste UV (paleta: indice 0 e' transparente).
float silhouette(vec2 uv) {
    vec4 albedo = texture(u_textures[nonuniformEXT(v_texIndex)], uv);
    if ((v_flags & 256u) != 0u) return albedo.r * 255.0 < 0.5 ? 0.0 : 1.0;
    return albedo.a;
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(u_sceneDepth, px, 0).r;
    if (depth >= 1.0) discard; // ceu
    vec2 suv = (vec2(px) + 0.5) / vec2(textureSize(u_sceneDepth, 0));
    vec4 viewPos = u_inverseProjection * vec4(suv * 2.0 - 1.0, depth, 1.0);
    vec3 P = (u_inverseView * vec4(viewPos.xyz / viewPos.w, 1.0)).xyz;

    vec3 L = v_light.xyz;
    vec3 A = v_anchor.xyz;
    vec3 rBase = vec3(v_axis.x, 0.0, v_axis.z);
    vec3 rTop = vec3(v_axisTop.x, 0.0, v_axisTop.y);
    float dil = v_offs.y;
    float baseY = A.y + v_offs.x - 0.5 * v_axis.w; // base do quadro

    // Onde o raio luz -> P corta o quadro: W = L + s (P - L). So' faz sombra
    // com o quadro entre a luz e P (s < 1) e P a no maximo 2x a distancia
    // luz-quadro (s >= 0,5). Com a luz abaixo do topo do sprite (tocha na
    // altura do peito) a sombra iria ao infinito: com 5x ela virava um risco
    // fino atravessando a sala, e com 3x a silhueta animada e comprida
    // tremulava no chao (bancada do jogo: +32% de pixel trocando de sinal
    // contra so' o disco).
    // Quadro torcido (ver o vertice): o eixo horizontal vai do da base ao do
    // alto com a altura. O corte do raio com ele converge em 3 passos (a
    // torcao e' suave): corta no eixo atual, le' a altura, refaz o eixo.
    vec3 r = rBase;
    float s = 0.0;
    for (int i = 0; i < 3; ++i) {
        vec3 n = vec3(-r.z, 0.0, r.x); // normal do quadro (vertical)
        float den = dot(n, P - L);
        if (abs(den) < 1e-4) discard;
        s = dot(n, A - L) / den;
        float h = clamp((L.y + s * (P.y - L.y) - baseY) / (v_axis.w * dil), 0.0, 1.0);
        r = normalize(mix(rBase, rTop, h));
    }
    if (s < 0.5 || s >= 1.0) discard;

    // O proprio sprite (pixel no plano dele, acima do pe') nao se sombreia.
    float folga = 0.02 * v_axis.y;
    if (abs(dot(v_plane.xyz, P) - v_plane.w) < folga && P.y - A.y - v_offs.z > folga) discard;

    // Posicao no quadro (-0,5..0,5), desfazendo a dilatacao em torno da base.
    vec3 W = L + s * (P - L);
    float qx = (dot(W - A, r) - v_anchor.w) / v_axis.y / dil;
    float qy = ((W.y - A.y - v_offs.x) / v_axis.w + 0.5) / dil - 0.5;
    // A sola continua um pouco abaixo da base (ate' 1/4 da altura): o chao
    // desenhado oscila ~0,5 u em volta da altura do pe' (relevo deslocado x
    // grade de colisao), e com a luz rasante o raio que passa abaixo da base
    // abria um vao claro entre o pe' e a sombra (1 u de diferenca vira ~4 u
    // com a tocha a 100 u). Abaixo da base vale a linha da sola.
    if (qy < -0.5 && qy > -0.75) qy = -0.4999;
    if (abs(qx) > 0.5 || abs(qy) > 0.5) discard;

    vec2 uv = vec2(qx + 0.5, 0.5 - qy);
    // Sprite PNG/ERUPTSPR (bit 11): X invertido, como em sprite_vert.glsl.
    bool flipU = (v_flags & 1u) != 0u;
    if ((v_flags & 2048u) != 0u) flipU = !flipU;
    if (flipU) uv.x = 1.0 - uv.x;
    if ((v_flags & 2u) != 0u) uv.y = 1.0 - uv.y;

    float alpha = silhouette(uv);
    // Luz de lado: uniao com o espelho (ver o vertice).
    if (v_offs.w > 0.0) alpha = max(alpha, v_offs.w * silhouette(vec2(1.0 - uv.x, uv.y)));
    if (alpha < 0.01) discard;

    // Clareia do pe' para a ponta (80% mais clara no topo) e some perto do
    // teto de 2x: a sombra de luz proxima se espalha com a distancia.
    float strength = v_light.w * mix(1.0, 0.2, qy + 0.5) * (1.0 - smoothstep(1.7, 2.0, 1.0 / s));
    float darken = 1.0 - alpha * strength * 0.75;
    outShadow = vec4(darken, darken, darken, 1.0);
}
