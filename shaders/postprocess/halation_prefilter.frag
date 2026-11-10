#version 450
// HALATION, filtro de entrada (PostProcessor::renderHalation): a cena cheia
// em 1/4 da resolucao, so' a parte acima do limiar. Halation de filme e' a
// luz forte que atravessa a emulsao, reflete na base e volta como um halo
// largo e quente; aqui e' a mesma ideia num limiar BAIXO (ceu claro e luz
// acesa, nao so' o especular estourado que o bloom pega).
//
// Saida: cor x (excesso / luminancia) - a fracao do pixel acima do limiar,
// com a cor dele. O composite tira essa mesma fracao do pixel e soma o
// borrao dela: a energia so' muda de lugar.

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;

layout(push_constant) uniform Push {
    float threshold; // luminancia exposta
    float exposure;
} push;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

// Mesma conta do composite (localExcess no post_composite.frag).
vec3 excess(vec3 c) {
    float l = dot(c, kLuma);
    float e = max(l - push.threshold, 0.0);
    // Teto suave: o sol e o especular nao dominam o halo.
    e = e / (1.0 + e * 0.25);
    return c * (e / max(l, 1e-4));
}

void main() {
    // 4 leituras bilineares no centro de cada quadra 2x2 do bloco 4x4.
    vec2 t = 1.0 / vec2(textureSize(hdrTexture, 0));
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        vec2 o = vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0) * t;
        acc += excess(texture(hdrTexture, fragUV + o).rgb * push.exposure);
    }
    outColor = vec4(acc * 0.25, 1.0);
}
