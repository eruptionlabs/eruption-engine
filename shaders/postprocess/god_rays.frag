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
    vec4 params2;   // x = aspecto (largura/altura), y = quantas luzes
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
    outColor = vec4(bloom + rays, 1.0);
}
