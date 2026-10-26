#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 3) in vec4 v_tint;
layout(location = 4) in vec3 v_worldPos;
layout(location = 5) in vec3 v_normal;
layout(location = 6) in flat uint v_paletteIndex;
layout(location = 7) in flat uint v_normalTexIndex;
layout(location = 8) in flat uint v_mrahwTexIndex;
layout(location = 9) in vec3 v_billRight;
layout(location = 10) in vec3 v_billUp;
layout(location = 11) in vec2 v_quad;
layout(location = 12) in flat uint v_instance;
layout(location = 13) in flat float v_halfWidth;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_pbr;
layout(location = 3) out uint o_materialId;
layout(location = 4) out vec4 o_emissive;

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
    vec4 u_sunDir;        // xyz = direcao de viagem da luz do sol, w = intensidade
    vec4 u_sunColor;
};

// Luz pontual principal de cada sprite (Engine::spriteDominantLights; a
// mesma escolha da sombra planar das luzes). strength 0 = nenhuma perto.
struct SpriteLight {
    vec4 posStrength; // xyz = posicao, w = forca (intensidade x atenuacao no corpo)
    vec4 color;
};
layout(std430, set = 1, binding = 1) readonly buffer SpriteLights { SpriteLight u_spriteLights[]; };

// CONTRALUZ (rim light): a luz que vem de TRAS do boneco acende a borda da
// silhueta e o recorta de um fundo escuro - a terceira luz do trio classico
// de iluminacao (key/fill/back). Sprite nao tem geometria de borda: a borda
// sai do alfa da propria arte, comparando o texel com os vizinhos a ~2,5 px
// de tela. Forca = luz vindo de tras do billboard em direcao a' camera.
float contraluz(vec3 toLight, vec3 toCam) {
    return smoothstep(-0.15, 0.75, -dot(normalize(toLight), toCam));
}

float srgbToLinear(float c) {
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

vec3 srgbToLinear(vec3 c) {
    return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b));
}

mat3 computeTBN(vec3 N, vec3 worldPos, vec2 uv) {
    vec3 dPosX = dFdx(worldPos);
    vec3 dPosY = dFdy(worldPos);
    vec2 dUVX  = dFdx(uv);
    vec2 dUVY  = dFdy(uv);

    vec3 T = dPosX * dUVY.t - dPosY * dUVX.t;
    vec3 B = -dPosX * dUVY.s + dPosY * dUVX.s;

    T = normalize(T - N * dot(T, N));
    B = normalize(B - N * dot(B, N) - T * dot(B, T));

    return mat3(T, B, N);
}

void main() {
    vec4 albedo = texture(nonuniformEXT(u_textures[v_texIndex]), v_uv);
    // Derivadas antes de qualquer discard (depois dele o quad fica incompleto).
    const vec2 rimDx = dFdx(v_uv) * 2.5, rimDy = dFdy(v_uv) * 2.5;

    // bit 8: usePalette
    if ((v_flags & 256u) != 0) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        float u = (idx + 0.5) / 256.0;
        albedo = texture(nonuniformEXT(u_textures[v_paletteIndex]), vec2(u, 0.5));
        albedo.a = 1.0;
    }

    if (albedo.a < 0.01) discard;

    // Keep albedo in sRGB for both legacy and PBR paths so the base brightness
    // matches the pre-PBR branch. Only F0/specular benefits from linear-space
    // conversion; converting the whole albedo made the scene look unnaturally
    // dark compared to the legacy renderer.
    albedo.rgb *= v_tint.rgb;

    uint matId = (v_flags >> 3) & 0x1F;

    // NORMAL ABAULADA.
    //
    // O vertex entrega UMA normal para o sprite inteiro (a do billboard,
    // misturada com +Y por u_spriteNormalYMix). Com ela, a iluminacao do
    // personagem e' CONSTANTE na area toda: tocha a esquerda e tocha a
    // direita dao exatamente o mesmo resultado, e dentro de uma caverna
    // iluminada so' por tocha o boneco fica chapado, colado por cima da cena
    // em vez de dentro dela. (O caminho forward, que nem somava luz pontual,
    // e' codigo morto: renderSpritesForward nao tem chamador.)
    //
    // O sprite nao e' um plano: ele representa um corpo. Entao a normal e'
    // curvada como a de um CILINDRO em pe' — na borda esquerda aponta para a
    // esquerda, na direita para a direita — e um pouco no eixo vertical, que
    // e' o que faz a cabeca pegar a luz de cima e o pe' ficar na sombra.
    // Nao custa textura nenhuma e vale para todo sprite do acervo.
    //
    // 0.62 e 0.22: com 1.0 a borda fica perpendicular a vista e escurece ate'
    // sumir, que le' como recorte preto em volta do personagem. Nestes dois a
    // silhueta continua inteira e o lado da tocha acende.
    const float kAbaulaH = 0.62, kAbaulaV = 0.22;
    vec3 N = normalize(v_normal);
    {
        float s = clamp(v_quad.x * 2.0, -1.0, 1.0);
        float t = clamp(v_quad.y * 2.0, -1.0, 1.0);
        N = normalize(N + v_billRight * (s * kAbaulaH) + v_billUp * (t * kAbaulaV));
    }
    if (v_normalTexIndex != 0u) {
        // Generated normal maps are kept out of sprite lighting to preserve the
        // original look; alpha is still sampled by the deferred pass elsewhere.
    }

    // Sem mapa: rugosidade 1, que a luz le' como "sprite so' difuso"
    // (pbrSpriteDiffuseOnly, lighting/pbr_common.glsl).
    float roughness = 1.0;
    // Era 0.5: METADE METAL pra' TODO sprite sem mapa MRAH-W - ou seja, pra'
    // todo personagem, monstro e NPC do jogo, que sao pele, pano, couro e
    // papel. Meio-metal escurece o difuso pela metade e joga essa metade num
    // reflexo colorido pelo albedo - o personagem vira uma "banana metalica".
    // Sprite sem mapa e' dieletrico ate' prova em contrario: 0.
    float metallic = 0.0;
    if (v_mrahwTexIndex != 0u) {
        vec4 mrahw = texture(nonuniformEXT(u_textures[v_mrahwTexIndex]), v_uv);
        metallic  = mrahw.r;
        roughness = mrahw.g;
    }

    vec3 encodedNormal = N * 0.5 + 0.5;

    // Use Albedo.a to carry the Exposure multiplier for Ambient Pass (Deferred)
    o_albedo = vec4(albedo.rgb, u_spriteExposure);
    o_normal = vec4(encodedNormal, roughness);
    // Sprites use the same fallback source flag so they participate in the PBR
    // pipeline unless a real MRAH-W map is bound.
    // R = origem do material: 0,75 marca SPRITE (0,5 reserva, 1 mapa real,
    // 0,625/0,875 folhagem). Para a iluminacao e' igual a 0,5 (>= 0,5, fora
    // da folhagem); a sombra de contato usa a marca para pular o quad.
    // pbr.a: no sprite que projeta sombra no mapa (SpriteFlags::ShadowMap), a
    // meia largura do quadro em u/255 (directional.frag tira a auto-sombra com
    // ela); nos outros, 0. A largura vem do vertice (constante no quad):
    // estimada pelas derivadas, variava de pixel a pixel no recorte da arte e
    // a sombra de um sprite sobre outro saia em chuvisco.
    float halfWidth = (v_flags & 4096u) != 0u ? clamp(v_halfWidth / 255.0, 1.0 / 255.0, 1.0) : 0.0;
    o_pbr = vec4(0.75, roughness, metallic, halfWidth);
    o_materialId = matId;
    // Borda da silhueta: algum vizinho transparente. Paleta (bit 256) le' o
    // indice, nao o alfa: indice 0 = transparente.
    vec3 rim = vec3(0.0);
    if ((v_flags & 256u) == 0u) {
        float minA = min(min(texture(nonuniformEXT(u_textures[v_texIndex]), v_uv + rimDx).a, texture(nonuniformEXT(u_textures[v_texIndex]), v_uv - rimDx).a),
                         min(texture(nonuniformEXT(u_textures[v_texIndex]), v_uv + rimDy).a, texture(nonuniformEXT(u_textures[v_texIndex]), v_uv - rimDy).a));
        float edge = 1.0 - smoothstep(0.05, 0.6, minA);
        if (edge > 0.0) {
            const vec3 toCam = normalize(u_cameraPos - v_worldPos);
            SpriteLight l = u_spriteLights[v_instance];
            if (l.posStrength.w > 0.0)
                rim += l.color.rgb * l.posStrength.w * contraluz(l.posStrength.xyz - v_worldPos, toCam) * 0.35;
            // Sol (fica fora da lista): mais fraco, so' de dia.
            if (u_sunDir.w > 0.0)
                rim += u_sunColor.rgb * u_sunDir.w * contraluz(-u_sunDir.xyz, toCam) * 0.12;
            rim *= edge * albedo.rgb * 2.0;
        }
    }
    o_emissive = vec4(rim, 1.0);
}
