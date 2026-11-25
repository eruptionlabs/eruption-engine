// ESTADO DO CHAO POR CELULA (incluido por ambient.frag e directional.frag depois
// do LightingUBO `lights`; as duas passadas tem de concordar).
//
// Binding 20: imagem RGBA8 de 2W x W em espaco de mundo, escrita pela CPU
// (core/GroundState.cpp). Metade esquerda: R = queimado, G = geada, B = molhado,
// A = neve (0..1, ja' com o ciclo de vida e a borda). Metade direita: a altura do
// chao naquela celula (16 bits em R e G), a mascara que impede a marca de subir
// na copa de uma arvore ou no teto de uma casa. lights.groundStateWin.w = 0
// desliga tudo (nem le a textura): a imagem sai igual a de antes.
//
// Modula o MATERIAL ja' desenhado (albedo, rugosidade, normal), nao pinta por
// cima: serve ao terreno e a qualquer modelo de chao, e o ruido de mundo abaixo
// quebra a borda da celula em manchas (sem ele a marca sairia um disco liso).
layout(set = 0, binding = 20) uniform sampler2D groundStateTex;

float gsHash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float gsNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(gsHash(i), gsHash(i + vec2(1.0, 0.0)), f.x),
               mix(gsHash(i + vec2(0.0, 1.0)), gsHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Ruido de duas escalas pela celula (a janela, em u por celula, sai do UBO).
float gsDetail(vec3 wp, float texelWorld) {
    return 0.6 * gsNoise(wp.xz / (texelWorld * 0.55)) + 0.4 * gsNoise(wp.xz / (texelWorld * 0.17) + 17.0);
}

// A quantidade (0..1) vira mancha: no miolo (1) fica cheia, na borda (0..1) o
// ruido decide onde ainda ha' marca. `rough` 0 = borda firme, 1 = muito rasgada.
float gsBreak(float a, float n, float rough) {
    return smoothstep(n * 0.6 * rough, n * 0.6 * rough + 0.35, a);
}

// (queimado, geada, molhado, neve) em wp, ja' mascarados pela altura do chao.
vec4 groundStateAt(vec3 wp) {
    if (lights.groundStateWin.w <= 0.0) return vec4(0.0);
    vec2 uv = (wp.xz - lights.groundStateWin.xy) * lights.groundStateWin.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(0.0);
    vec2 sz = vec2(textureSize(groundStateTex, 0));
    float W = sz.y;
    // Esquerda, filtrada: cuidado para nao puxar o texel da direita na borda.
    vec2 uvL = vec2(clamp(uv.x * W, 0.5, W - 0.5) / sz.x, clamp(uv.y * W, 0.5, W - 0.5) / sz.y);
    vec4 s = textureLod(groundStateTex, uvL, 0.0);
    if (s.x + s.y + s.z + s.w <= 0.0) return vec4(0.0);
    // Altura do chao na celula (vizinho mais proximo, 16 bits).
    ivec2 tc = ivec2(clamp(floor(uv * W), vec2(0.0), vec2(W - 1.0)));
    vec4 h = texelFetch(groundStateTex, ivec2(tc.x + int(W), tc.y), 0);
    float ground = lights.groundStateH.x + (floor(h.r * 255.0 + 0.5) * 256.0 + floor(h.g * 255.0 + 0.5)) / 65535.0 * lights.groundStateH.y;
    float dy = wp.y - ground;
    float up = lights.groundStateH.z, down = lights.groundStateH.w;
    float gate = (1.0 - smoothstep(up, up * 1.7, dy)) * smoothstep(-down * 1.7, -down, dy);
    return s * gate;
}

// O molhado entra no que ja' molha (pbrWetSurface).
float groundStateWet(vec4 s, vec3 wp) {
    if (s.z <= 0.0) return 0.0;
    float texelWorld = 1.0 / (lights.groundStateWin.z * float(textureSize(groundStateTex, 0).y));
    return gsBreak(s.z, gsDetail(wp, texelWorld), 0.5);
}

// Queimado, geada e neve: albedo, rugosidade e normal. `s` vem de groundStateAt;
// `ao` = oclusao/cavidade do G-buffer (1 = aberto): a neve enche primeiro os baixos.
void groundStateCover(inout vec3 albedoLinear, inout float roughness, inout float metallic, inout vec3 N, vec4 s, vec3 wp, float ao) {
    if (s.x + s.y + s.w <= 0.0) return;
    float texelWorld = 1.0 / (lights.groundStateWin.z * float(textureSize(groundStateTex, 0).y));
    float n = gsDetail(wp, texelWorld);
    float n2 = gsNoise(wp.xz / (texelWorld * 0.9) + 5.3);
    const vec3 upV = vec3(0.0, 1.0, 0.0);
    // Queimado: carvao preto no miolo, borda marrom, cinza clara salpicada e
    // a folhagem verde vai para marrom escuro (le' de longe na camera do jogo).
    if (s.x > 0.0) {
        float halo = gsBreak(min(s.x * 2.6, 1.0), n2, 0.7);
        float core = gsBreak(s.x * 1.1 - 0.15, n, 0.9);
        float ring = halo * (1.0 - core); // anel marrom em volta do carvao
        float ash = smoothstep(0.50, 0.72, gsNoise(wp.xz / (texelWorld * 0.13) + 31.0)) * ring;
        float green = clamp((albedoLinear.g - max(albedoLinear.r, albedoLinear.b)) * 6.0, 0.0, 1.0);
        albedoLinear = mix(albedoLinear, albedoLinear * vec3(0.35, 0.22, 0.12), halo);
        albedoLinear = mix(albedoLinear, vec3(0.30, 0.13, 0.045) + albedoLinear * 0.08, ring);
        albedoLinear = mix(albedoLinear, vec3(0.42, 0.40, 0.38), ash * 0.8);
        albedoLinear = mix(albedoLinear, vec3(0.002), core);
        roughness = mix(roughness, 0.95, max(halo * 0.7, core));
        metallic *= 1.0 - max(halo, core);
    }
    // Geada: fosca, branco-azulada, com cristais finos; a normal fica quase
    // plana e a rugosidade alta (sem brilho de metal).
    if (s.y > 0.0) {
        float up = smoothstep(0.25, 0.75, N.y);
        float k = gsBreak(s.y, n, 0.8) * up;
        float crystal = gsNoise(wp.xz / (texelWorld * 0.05)) * 0.6 + gsNoise(wp.xz / (texelWorld * 0.021) + 9.0) * 0.4;
        vec3 fcol = mix(vec3(0.58, 0.68, 0.80), vec3(0.88, 0.94, 1.0), smoothstep(0.35, 0.8, crystal));
        albedoLinear = mix(albedoLinear, fcol, k * 0.7);
        roughness = mix(roughness, 0.9, k * 0.95);
        metallic *= 1.0 - k;
        N = normalize(mix(N, upV, k * 0.9));
    }
    // Neve: macia, branca e fosca; cobre primeiro os relevos baixos (cavidade).
    if (s.w > 0.0) {
        float up = smoothstep(0.3, 0.8, N.y);
        float cav = clamp(1.0 - ao, 0.0, 1.0);
        float a = clamp(s.w * (0.85 + 0.9 * cav), 0.0, 1.0);
        float k = gsBreak(a, n, 0.7) * up;
        float soft = 0.93 + 0.07 * gsNoise(wp.xz / (texelWorld * 0.09));
        albedoLinear = mix(albedoLinear, vec3(0.80, 0.83, 0.88) * soft, k * 0.97);
        roughness = mix(roughness, 0.95, k);
        metallic *= 1.0 - k;
        N = normalize(mix(N, upV, k * 0.92));
    }
}
