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
// A saida e' o bloom + os raios, e entra no lugar do bloom no composite (que
// ja' soma o bloom a cena); a forca vem dividida pela intensidade do bloom.

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_bloom;   // saida do upsample do bloom
layout(set = 0, binding = 1) uniform sampler2D u_bright;  // bright pass (meia resolucao)

layout(push_constant) uniform GodRaysPush {
    vec4 lights[4]; // xy = luz na tela (uv), z = raio do nucleo (uv, em altura), w = forca
    vec4 params;    // x = amostras, y = decaimento por amostra, z = peso, w = alcance (uv, em altura)
    vec4 params2;   // x = aspecto (largura/altura), y = quantas luzes
} push;

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
    outColor = vec4(bloom + rays, 1.0);
}
