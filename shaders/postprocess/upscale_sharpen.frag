#version 450
// Upscale + nitidez, no lugar do blit BILINEAR cru que copiava a imagem de
// RENDER direto pro swapchain de DISPLAY (`VulkanContext::copyImageToImageScaled`,
// VK_FILTER_LINEAR). Bilinear e' o upscale mais cego que existe: borra
// igualmente em toda direcao, sem olhar pra onde tem borda.
//
// DUAS ETAPAS, MESMO PASSE:
//
// 1) UPSAMPLE BICUBICO (Catmull-Rom, 16 taps, textelFetch pontual). Formula
//    publica e verificavel (base de Hermite cubica, GPU Gems 2 cap. 20) -
//    ao contrario de reproduzir de memoria os pesos EXATOS do EASU da AMD
//    (FidelityFX FSR1), que sao ajustados empiricamente e eu nao tenho como
//    conferir aqui sem o codigo-fonte de referencia. Catmull-Rom entrega o
//    mesmo objetivo pratico do EASU - upscale mais nitido que bilinear, sem
//    ringing - com uma formula que da' pra auditar cada peso.
//
// 2) NITIDEZ ADAPTATIVA POR CONTRASTE LOCAL, no espirito do RCAS da AMD:
//    mede o minimo/maximo da vizinhanca em cruz e usa esse range pra
//    LIMITAR quanto o unsharp mask pode empurrar cada pixel - e' o clamp
//    que impede halo em beirada de alto contraste (a diferenca entre
//    "nitidez" e "contorno branco ao redor de tudo"). Constantes proprias,
//    nao um port byte-a-byte do RCAS - mesmo motivo do item 1.
//
// Os quatro vizinhos da nitidez usam BILINEAR barato (o sampler ja' e'
// LINEAR) em vez de repetir o bicubico 16-tap quatro vezes: a nitidez so'
// precisa de uma medida de contraste local, nao da qualidade cheia do
// upsample nos vizinhos.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_input; // linear, clamp-to-edge

layout(push_constant) uniform PushConstants {
    vec2 srcSize;     // largura, altura da imagem de ENTRADA (resolucao de render)
    vec2 invSrcSize;  // 1/srcSize
    vec2 invDstSize;  // 1/(largura,altura da imagem de SAIDA, o swapchain)
    float sharpenAmount; // 0 = so' upscale; ~0.35 e' um bom padrao
} push;

float catmullRomWeight0(float t) { return -0.5*t*t*t + t*t - 0.5*t; }
float catmullRomWeight1(float t) { return  1.5*t*t*t - 2.5*t*t + 1.0; }
float catmullRomWeight2(float t) { return -1.5*t*t*t + 2.0*t*t + 0.5*t; }
float catmullRomWeight3(float t) { return  0.5*t*t*t - 0.5*t*t; }

// Upsample bicubico Catmull-Rom, 16 taps por texelFetch (sem filtro de
// hardware nenhum interferindo nos pesos). `uv` em [0,1] relativo a imagem
// de entrada.
vec3 catmullRomSample(vec2 uv) {
    vec2 srcPos = uv * push.srcSize - 0.5;
    vec2 base = floor(srcPos);
    vec2 frac = srcPos - base;

    float wx[4] = float[](catmullRomWeight0(frac.x), catmullRomWeight1(frac.x),
                           catmullRomWeight2(frac.x), catmullRomWeight3(frac.x));
    float wy[4] = float[](catmullRomWeight0(frac.y), catmullRomWeight1(frac.y),
                           catmullRomWeight2(frac.y), catmullRomWeight3(frac.y));

    ivec2 srcMax = ivec2(push.srcSize) - ivec2(1, 1);
    vec3 result = vec3(0.0);
    for (int y = 0; y < 4; ++y) {
        vec3 rowSum = vec3(0.0);
        int py = int(base.y) - 1 + y;
        py = clamp(py, 0, srcMax.y);
        for (int x = 0; x < 4; ++x) {
            int px = int(base.x) - 1 + x;
            px = clamp(px, 0, srcMax.x);
            rowSum += texelFetch(u_input, ivec2(px, py), 0).rgb * wx[x];
        }
        result += rowSum * wy[y];
    }
    // Catmull-Rom pode devolver valor fora de [0,1] perto de borda de alto
    // contraste (overshoot e' propriedade da base cubica, nao bug) - o clamp
    // final e' o mesmo que qualquer upsample "sharp" precisa fazer.
    return max(result, vec3(0.0));
}

void main() {
    vec3 center = catmullRomSample(inUV);

    if (push.sharpenAmount <= 0.0001) {
        outColor = vec4(center, 1.0);
        return;
    }

    // Vizinhanca em cruz, 1 texel de SAIDA de distancia, amostrada com o
    // bilinear barato do sampler (nao repete o bicubico).
    vec3 n = texture(u_input, inUV + vec2(0.0, -push.invDstSize.y)).rgb;
    vec3 s = texture(u_input, inUV + vec2(0.0,  push.invDstSize.y)).rgb;
    vec3 e = texture(u_input, inUV + vec2( push.invDstSize.x, 0.0)).rgb;
    vec3 w = texture(u_input, inUV + vec2(-push.invDstSize.x, 0.0)).rgb;

    vec3 nMin = min(center, min(min(n, s), min(e, w)));
    vec3 nMax = max(center, max(max(n, s), max(e, w)));

    // Unsharp mask classico: centro menos a media da cruz.
    vec3 blur = (n + s + e + w) * 0.25;
    vec3 sharpened = center + (center - blur) * push.sharpenAmount;

    // CLAMP AO RANGE LOCAL - e' isto que separa nitidez de halo. Sem ele,
    // toda borda de alto contraste (beiral de telhado, coluna) ganha uma
    // franja branca/preta visivel a distancia.
    sharpened = clamp(sharpened, nMin, nMax);

    outColor = vec4(sharpened, 1.0);
}
