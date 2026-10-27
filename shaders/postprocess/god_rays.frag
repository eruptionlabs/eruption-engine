#version 450

// RAIOS DE LUZ DAS LUZES PONTUAIS, em espaco de tela e meia resolucao.
// Base: Mitchell, "Volumetric Light Scattering as a Post-Process" (GPU Gems 3,
// cap. 13, 2007) - marcha radial na tela ate' a fonte, com decaimento.
//
// Para cada luz forte visivel (tocha, lago de lava; ate' 4, escolhidas na
// Engine), marcha da luz ate' o pixel lendo o bright pass do bloom. So' conta
// o brilho perto da luz (o nucleo): sem isso qualquer superficie ensolarada
// no caminho virava raio apontando para a tocha. O que tapa a chama (grade,
// folha, coluna) e' escuro no bright pass e abre a fresta entre os raios.
//
// SOL (preset "god_ray_sun"): a mesma marcha, do pixel ate' o sol, lendo uma
// mascara de oclusao montada na hora - o pixel de ceu (profundidade no fundo)
// com a cor da cena, pesado por um halo em volta do sol. O que fica entre a
// camera e o ceu (modelo, folha, sprite) zera a mascara e abre a fresta
// escura entre os raios. Ceu so' pelo brilho seria fragil: o bright pass
// corta o ceu claro mas abaixo do limiar.
//
// LENS FLARE DO SOL (preset "lens_flare"): reflexos internas da lente,
// analiticos, sem textura - fantasmas (discos coloridos na reta do sol ao
// centro da tela, do lado oposto), halo (anel em volta do sol, com
// dispersao de cor) e estrela (linhas finas pelo sol; "lens_flare_rays"
// linhas, 1 = um risco so'). Tudo pesado pela fracao do disco do sol que e'
// ceu na profundidade (12 pontos): o que passa na frente apaga o flare aos
// poucos, e a cor vem do ceu em volta do sol (nuvem e por do sol tingem).
// Base: Kawase, "Practical Implementation of High Dynamic Range Rendering"
// (GDC 2004) e Chapman, "Pseudo Lens Flare" (2013).
//
// A saida e' o bloom + os raios, e entra no lugar do bloom no composite (que
// ja' soma o bloom a cena); a forca vem dividida pela intensidade do bloom.

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_bloom;   // saida do upsample do bloom
layout(set = 0, binding = 1) uniform sampler2D u_bright;  // bright pass (meia resolucao)
layout(set = 0, binding = 2) uniform sampler2D u_depth;   // profundidade da cena (ceu = 1)
layout(set = 0, binding = 3) uniform sampler2D u_scene;   // cena HDR (cor do ceu perto do sol)

layout(push_constant) uniform GodRaysPush {
    vec4 lights[4]; // xy = luz na tela (uv), z = raio do nucleo (uv, em altura), w = forca
    vec4 params;    // x = amostras, y = decaimento por amostra, z = peso, w = alcance (uv, em altura)
    vec4 params2;   // x = aspecto (largura/altura), y = quantas luzes, z = forca do flare (0 = sem), w = linhas da estrela
    vec4 sun;       // xy = sol na tela (uv), z = raio do nucleo (uv, em altura), w = forca (0 = sem sol)
} push;

// Mascara de oclusao do sol num ponto da tela: ceu visivel x nucleo x cor.
vec3 sunOcclusionMask(vec2 uv, vec2 aspect) {
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(0.0);
    if (texture(u_depth, uv).r < 0.9999) return vec3(0.0);
    float d = length((uv - push.sun.xy) * aspect) / push.sun.z;
    float halo = max(1.0 - d, 0.0);
    return min(texture(u_scene, uv).rgb, vec3(4.0)) * (halo * halo);
}

// Lens flare do sol no pixel `uv` (distancias em alturas de tela).
vec3 lensFlare(vec2 uv, vec2 aspect) {
    const vec2 sun = push.sun.xy;
    // Visibilidade e cor: pontos em espiral no disco do sol (raio 0,012).
    vec3 tint = vec3(0.0);
    float vis = 0.0;
    for (int i = 0; i < 12; ++i) {
        const float a = float(i) * 2.39996;
        const vec2 p = sun + vec2(cos(a), sin(a)) * (0.012 * sqrt((float(i) + 0.5) / 12.0)) / aspect;
        if (texture(u_depth, p).r >= 0.9999) {
            vis += 1.0;
            tint += min(texture(u_scene, p).rgb, vec3(4.0));
        }
    }
    if (vis <= 0.0) return vec3(0.0);
    tint /= vis;
    // Some perto da borda da tela em vez de cortar quando o sol sai.
    const vec2 border = min(sun, 1.0 - sun) * aspect;
    vis *= smoothstep(0.0, 0.08, min(border.x, border.y)) / 12.0;

    const vec2 P = (uv - sun) * aspect;
    const float d = length(P);
    // Fantasmas: t = 1 e' o centro da tela, 2 o ponto espelhado do sol.
    const vec2 axis = vec2(0.5) - sun;
    const float gt[5] = float[5](0.45, 0.8, 1.35, 1.7, 2.3);
    const float gr[5] = float[5](0.025, 0.05, 0.035, 0.08, 0.12);
    const vec3 gc[5] = vec3[5](vec3(1.0, 0.7, 0.35), vec3(0.4, 1.0, 0.6), vec3(0.75, 0.45, 1.0),
                               vec3(0.35, 0.6, 1.0), vec3(1.0, 0.85, 0.6));
    vec3 ghosts = vec3(0.0);
    for (int k = 0; k < 5; ++k) {
        const float g = length((uv - (sun + axis * gt[k])) * aspect) / gr[k];
        // Disco com a borda um pouco mais clara (abertura poligonal da lente).
        ghosts += gc[k] * (smoothstep(1.0, 0.8, g) * (0.5 + 0.5 * smoothstep(0.4, 0.95, g)));
    }
    // Halo: anel de raio 0,35 em volta do sol, vermelho por fora.
    const vec3 hr = vec3(0.357, 0.35, 0.343);
    const vec3 halo = exp(-(d - hr) * (d - hr) / 0.00018);
    // Estrela: distancia a' linha mais perto (periodo pi / linhas), largura fixa.
    const float lines = push.params2.w;
    const float period = 3.14159265 / lines;
    const float phi = mod(atan(P.y, P.x) + 6.28318531, period);
    const float off = d * sin(min(phi, period - phi));
    const float star = exp(-off * off / 0.000006) * exp(-d / 0.16);
    const float core = exp(-d / 0.012);
    return tint * vis * push.params2.z * (ghosts * 0.3 + halo * 0.06 + star * 1.2 + core * 0.4);
}

void main() {
    vec3 bloom = texture(u_bloom, fragUV).rgb;
    vec3 rays = vec3(0.0);
    const int samples = clamp(int(push.params.x), 4, 64);
    const int count = clamp(int(push.params2.y), 0, 4);
    const vec2 aspect = vec2(push.params2.x, 1.0);
    for (int l = 0; l < count; ++l) {
        const vec4 L = push.lights[l];
        const vec2 toPixel = fragUV - L.xy;
        const float dist = length(toPixel * aspect);
        if (dist >= push.params.w) continue;
        // Marcha so' a corda do nucleo, da luz na direcao do pixel: fora dele o
        // caminho nao soma, e um passo fixo ate' o pixel pularia o nucleo
        // inteiro nos pixels longe (raio serrilhado). O decaimento pesa mais o
        // centro da chama que a borda.
        const float len = min(dist, L.z);
        const vec2 stepUv = toPixel * (len / max(dist, 1e-6) / float(samples));
        vec3 acc = vec3(0.0);
        float decay = 1.0;
        for (int i = 0; i < samples; ++i) {
            const float t = float(i) + 0.5;
            const float r = len * t / float(samples);
            const float core = 1.0 - smoothstep(L.z * 0.5, L.z, r);
            acc += texture(u_bright, L.xy + stepUv * t).rgb * (core * decay);
            decay *= push.params.y;
        }
        float fall = 1.0 - dist / push.params.w;
        fall *= fall;
        rays += acc * (push.params.z * L.w * fall / float(samples));
    }
    if (push.sun.w > 0.0) {
        // Marcha do pixel ao sol (Mitchell): a mascara so' tem valor perto do
        // sol, entao o rastro que chega ao pixel e' o feixe que passou pelas
        // frestas.
        // Alcance do feixe = o dos raios das luzes (params.w). Alem dele o
        // pixel nao recebe nada; sem isso o rastro chegava igual a' metade da
        // tela (veu, sem fresta).
        const vec2 toSun = push.sun.xy - fragUV;
        const float dist = length(toSun * aspect);
        const float reach = push.params.w;
        if (dist < reach) {
            // So' a corda ate' o nucleo: o resto do caminho e' mascara zero, e
            // o passo curto e' o que resolve folha e galho.
            const float len = min(dist, push.sun.z);
            const vec2 dir = toSun / max(dist, 1e-6);
            const vec2 start = push.sun.xy - dir * len;
            const vec2 stepUv = dir * (len / float(samples));
            // Ruido intercalado (Jimenez 2014) por pixel: troca bandas por grao fino.
            const float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            vec3 acc = vec3(0.0);
            float decay = 1.0;
            for (int i = 0; i < samples; ++i) {
                acc += sunOcclusionMask(start + stepUv * (float(i) + jitter), aspect) * decay;
                decay *= push.params.y;
            }
            const float fall = 1.0 - dist / reach;
            rays += acc * (push.params.z * push.sun.w * fall / float(samples));
        }
    }
    if (push.params2.z > 0.0) rays += lensFlare(fragUV, aspect);
    outColor = vec4(bloom + rays, 1.0);
}
