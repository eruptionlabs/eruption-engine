#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform BrightPush {
    float threshold;
} push;

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;

// Media de Karis no bloco 2x2 da resolucao cheia: cada amostra pesa
// 1/(1+luminancia). Area grande e clara sai igual a uma media comum; um pixel
// isolado muito forte (reflexo especular numa folha) nao domina mais o bloco.
// Junto com o teto do ganho abaixo (o brilho e' quadratico: cor x excesso),
// e' o que impede um reflexo de poucos pixels de virar uma bola branca
// enorme no bloom ("vaga-lume").
void main() {
    vec2 uv = fragUV;
    vec2 t = 0.5 / vec2(textureSize(hdrTexture, 0));
    vec3 c0 = texture(hdrTexture, uv + vec2(-t.x, -t.y)).rgb;
    vec3 c1 = texture(hdrTexture, uv + vec2( t.x, -t.y)).rgb;
    vec3 c2 = texture(hdrTexture, uv + vec2(-t.x,  t.y)).rgb;
    vec3 c3 = texture(hdrTexture, uv + vec2( t.x,  t.y)).rgb;
    const vec3 L = vec3(0.2126, 0.7152, 0.0722);
    float w0 = 1.0 / (1.0 + dot(c0, L));
    float w1 = 1.0 / (1.0 + dot(c1, L));
    float w2 = 1.0 / (1.0 + dot(c2, L));
    float w3 = 1.0 / (1.0 + dot(c3, L));
    vec3 color = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / (w0 + w1 + w2 + w3);
    float luminance = dot(color, L);
    float brightness = max(luminance - push.threshold, 0.0);
    // Teto suave do ganho: ate' ~1 acima do limiar fica praticamente como
    // antes; reflexo extremo (excesso 20+) satura perto de 4 em vez de
    // multiplicar a cor por 20 e virar uma bola branca.
    brightness = brightness / (1.0 + brightness * 0.25);
    outColor = vec4(color * brightness, 1.0);
}
