#ifndef ERUPTION_NOISE_GLSL
#define ERUPTION_NOISE_GLSL

// Hash -> value-noise -> fbm, autocontido (sem textura, sem estado): pensado
// pra variacao MACRO de terreno, nao pra normal map nem agua (aquilo ja' tem
// o proprio snoise em liquid.frag, com outro proposito - superficie
// animada, nao tom estatico do chao).
float eruptionHash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float eruptionValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = eruptionHash21(i);
    float b = eruptionHash21(i + vec2(1.0, 0.0));
    float c = eruptionHash21(i + vec2(0.0, 1.0));
    float d = eruptionHash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 3 oitavas, retorna [-1,1]. freqBase em unidades de mundo (1/tamanho da
// mancha maior); lacunaridade 2.2 evita padrao de grade repetido entre
// oitavas (2.0 exato alinha demais).
float eruptionMacroFbm(vec2 worldXZ, float freqBase) {
    float sum = 0.0;
    float amp = 0.55;
    float freq = freqBase;
    for (int o = 0; o < 3; ++o) {
        sum += (eruptionValueNoise(worldXZ * freq) * 2.0 - 1.0) * amp;
        freq *= 2.2;
        amp *= 0.5;
    }
    return clamp(sum, -1.0, 1.0);
}

#endif
