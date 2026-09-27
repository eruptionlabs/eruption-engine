#version 450

// Coluna de fumaça vulcânica, ray-marchada dentro de uma AABB — o mesmo
// caminho da nuvem local volumétrica (cloud_billboard.frag), mas com uma
// densidade ANALÍTICA de pluma em vez do SSBO de blobs:
//
//   * a parcela nasce na boca (origin) e sobe a `rise` m/s
//     -> na altura h a parcela tem idade t = h*height/rise
//   * é advectada pelo vento do clima: centro(h) = origin.xz + vento*t
//   * o raio cresce com a altura: r(h) = radius * (1 + spread*h)
//   * dissipa com a distância da fonte (densidade cai com h e com o raio)
//   * o ruído 3D é amostrado em (y - rise*t_global): o padrão SOBE junto com
//     a coluna, que é o que dá a leitura de fumaça em movimento sem simulação
//
// Sem textura, sem SSBO, sem bake de compute: 1 draw + 1 scissor por emissor.

layout(location = 0) in vec3 v_worldPos;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    mat4 invViewProj;
    vec3 cameraPos;
    float time;
    vec3 sunDir;
    float sunIntensity;
    vec2 screenSize;
    uint frameIndex;
};

// Profundidade da cena (set 2, o mesmo já ligado para o plano/puff).
layout(set = 2, binding = 0) uniform sampler2D sceneDepth;

// Precisa bater EXATAMENTE com smoke_plume.vert.
layout(push_constant) uniform PushConstants {
    vec4 boxMin;
    vec4 boxMax;
    vec4 origin;   // xyz = boca, w = raio da base
    vec4 shape;    // x = altura, y = spread, z = subida (m/s), w = densidade
    vec4 wind;     // xy = vento advectivo (m/s), z = turbulência (1/m), w = ritmo
    vec4 color;    // rgb = cor base, a = brasa na base
    vec4 misc;     // x = seed, y = passos máximos, z = targetScale, w = flags
} pc;

int positiveMod(int v, int m) {
    int r = v % m;
    return r < 0 ? r + m : r;
}

float hash3D(int ix, int iy, int iz, uint seed) {
    uint n = uint(ix) * 374761393u + uint(iy) * 668265263u + uint(iz) * 982451653u + seed * 1013904223u;
    n = (n ^ (n >> 13u)) * 1274126177u;
    return float(n) / float(0xFFFFFFFFu);
}

float valueNoise3D(vec3 p, uint seed) {
    int ix = int(floor(p.x));
    int iy = int(floor(p.y));
    int iz = int(floor(p.z));
    vec3 f = fract(p);

    // Período grande o bastante para nunca repetir dentro de uma pluma.
    const int P = 4096;
    int ix0 = positiveMod(ix, P), iy0 = positiveMod(iy, P), iz0 = positiveMod(iz, P);
    int ix1 = positiveMod(ix + 1, P), iy1 = positiveMod(iy + 1, P), iz1 = positiveMod(iz + 1, P);

    float a = hash3D(ix0, iy0, iz0, seed);
    float b = hash3D(ix1, iy0, iz0, seed);
    float c = hash3D(ix0, iy1, iz0, seed);
    float d = hash3D(ix1, iy1, iz0, seed);
    float e = hash3D(ix0, iy0, iz1, seed);
    float g = hash3D(ix1, iy0, iz1, seed);
    float h = hash3D(ix0, iy1, iz1, seed);
    float i = hash3D(ix1, iy1, iz1, seed);

    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y),
               mix(mix(e, g, f.x), mix(h, i, f.x), f.y), f.z);
}

// Peso de uma oitava pelo passo do ray-march (Nyquist).
//
// `cellStep` e' o passo do march medido em CELULAS daquela oitava. Acima de
// ~0.5 celula por passo a oitava nao pode mais ser resolvida: ela nao aparece
// como detalhe, aparece como ALIASING. Com o march regular isso vira listra
// (a pluma fica "esticada", em fiapos verticais); com o jitter ligado, o mesmo
// aliasing vira granulado por pixel - o quadriculado que cobria a fumaça.
// Medido nesta cena: passo de 31 u contra celula de 40 u, ou seja NENHUMA
// oitava era resolvivel e tudo o que se via de textura era o erro de amostragem.
//
// Apagar a oitava e' o certo: e' o mip-map do ruido procedural. O que sobra e'
// a forma analitica (bojos, erosao da borda, dissipacao), que nao depende do
// passo.
float octaveWeight(float cellStep) {
    return 1.0 - smoothstep(0.35, 0.85, cellStep);
}

// fbm de 2 ou 3 oitavas (a terceira só entra em qualidade alta), filtrado pelo
// passo. `stepCells` = passo do march em celulas da PRIMEIRA oitava.
// As oitavas somam em torno da media (0.5) em vez de renormalizar: quando
// todas somem o ruido vira 0.5 liso, sem salto.
float fbmSmoke(vec3 p, uint seed, bool detail, float stepCells) {
    float w0 = octaveWeight(stepCells) / 1.5;
    float w1 = 0.5 * octaveWeight(stepCells * 2.03) / 1.5;
    float n = 0.5 + w0 * (valueNoise3D(p, seed) - 0.5)
                 + w1 * (valueNoise3D(p * 2.03, seed + 131u) - 0.5);
    if (detail) {
        // A 3a oitava entra reescalando as duas primeiras para 1/1.75, do
        // mesmo jeito que a normalizacao antiga fazia.
        float k = 1.5 / 1.75;
        float w2 = 0.25 * octaveWeight(stepCells * 4.07) / 1.75;
        n = 0.5 + k * (n - 0.5) + w2 * (valueNoise3D(p * 4.07, seed + 262u) - 0.5);
    }
    return clamp(n, 0.0, 1.0);
}

float interleavedGradientNoise(vec2 fragCoord) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(floor(fragCoord), magic.xy)));
}

// Densidade da pluma no ponto p. hOut = fração da altura (0 na boca, 1 no topo).
// stepWorld = comprimento do passo do ray-march em unidades de mundo; e' o que
// diz quanta textura de ruido a imagem consegue carregar sem virar aliasing.
float smokeDensity(vec3 p, bool detail, float stepWorld, out float hOut) {
    hOut = 0.0;
    float height = pc.shape.x;
    float h = (p.y - pc.origin.y) / height;
    if (h < 0.0 || h > 1.0) return 0.0;
    hOut = h;

    float rise = max(pc.shape.z, 0.01);
    float age = h * height / rise;                 // idade da parcela (s)
    vec2 center = pc.origin.xz + pc.wind.xy * age; // advecção pelo vento

    // Meandro: a coluna serpenteia devagar em vez de subir num tubo reto.
    float sway = 0.10 * pc.origin.w * h;
    center += vec2(sin(age * 0.55 + pc.misc.x), cos(age * 0.41 + pc.misc.x * 1.7)) * sway;

    // Raio: cresce com a altura + bojos empilhados (a coluna real sobe em
    // rolos, não como um cone geométrico liso).
    float r = pc.origin.w * (1.0 + pc.shape.y * h) *
              (1.0 + 0.18 * sin(h * 7.0 + pc.misc.x) + 0.10 * sin(h * 17.0 - pc.misc.x * 0.7));
    vec2 q = p.xz - center;
    float d = length(q) / max(r, 0.01);
    if (d > 1.55) return 0.0;

    // Ruído amostrado no referencial da parcela: sobe com a coluna
    // (y - rise*time) e acompanha a advecção horizontal (q). O eixo vertical
    // é comprimido (0.6): os rolos de fumaça são esticados na vertical.
    float turb = pc.wind.z;
    vec3 np = vec3(q.x, (p.y - rise * time * pc.wind.w) * 0.6, q.y) * turb;
    // Passo em celulas da primeira oitava (a celula mede 1/turb em mundo).
    float stepCells = stepWorld * turb;
    float n = fbmSmoke(np, uint(pc.misc.x) + 977u, detail, stepCells);
    // Contraste: o fbm cru fica concentrado em torno de 0.5 e a coluna sai
    // como algodão uniforme. Empurrar para 0/1 é o que cria os rolos.
    n = smoothstep(0.34, 0.72, n);

    // Erosão da borda pelo ruído: billows/rolos em vez de um cilindro liso.
    // A amplitude cresce com a altura (perto da boca o jato é coerente).
    // O deslocamento é LIMITADO a +-0.5 (em unidades de raio): a AABB no
    // C++ usa exatamente esse teto (kSmokeRadialExtent), então o volume
    // nunca é cortado pela caixa e a caixa não fica inflada à toa.
    float dd = d + (n - 0.5) * (0.5 + 0.5 * h);
    float mask = 1.0 - smoothstep(0.62, 1.0, dd);
    if (mask <= 0.0) return 0.0;
    // Modulação interna: sem ela o volume é uma bolha de algodão uniforme.
    mask *= 0.30 + 0.95 * n;

    // Dissipação com a distância da fonte: some no topo e perde massa
    // conforme a coluna se abre.
    float fade = (1.0 - smoothstep(0.38, 1.0, h)) / (1.0 + 0.9 * pc.shape.y * h);
    // Pulso de emissão: bufadas saindo da boca.
    float puff = 0.85 + 0.15 * sin(age * 1.7 - time * 1.3 * pc.wind.w + pc.misc.x);

    return pc.shape.w * mask * fade * puff;
}

void main() {
    vec3 ro = cameraPos;
    vec3 rd = normalize(v_worldPos - cameraPos);

    // Interseção analítica raio/AABB (robusta com a câmera dentro da caixa).
    vec3 invD = 1.0 / rd;
    vec3 tA = (pc.boxMin.xyz - ro) * invD;
    vec3 tB = (pc.boxMax.xyz - ro) * invD;
    vec3 tMinV = min(tA, tB);
    vec3 tMaxV = max(tA, tB);
    float tEnter = max(max(tMinV.x, tMinV.y), max(tMinV.z, 0.0));
    float tExit = min(min(tMaxV.x, tMaxV.y), tMaxV.z);
    if (tExit <= tEnter) discard;

    int flags = int(pc.misc.w + 0.5);
    bool sunTap = (flags & 1) != 0;
    bool noDepthClip = (flags & 2) != 0;
    bool isFire = (flags & 8) != 0;
    bool detail = (flags & 4) != 0;

    // Oclusão pela cena: mesma abordagem suave do puff (a precisão de
    // profundidade a quilômetros corta o volume visivelmente se for dura).
    float tScene = 1e30;
    vec2 suv = gl_FragCoord.xy / (screenSize * pc.misc.z);
    float sceneZ = noDepthClip ? 1.0 : textureLod(sceneDepth, suv, 0.0).r;
    if (sceneZ < 1.0) {
        vec4 wp = invViewProj * vec4(suv * 2.0 - 1.0, sceneZ, 1.0);
        if (abs(wp.w) < 1e-6) wp.w = 1e-6;
        wp.xyz /= wp.w;
        tScene = dot(wp.xyz - ro, rd);
        if (tScene <= tEnter) discard; // pluma inteiramente atrás do terreno
    }
    tExit = min(tExit, tScene);
    if (tExit <= tEnter) discard;
    float fadeLen = max(6.0, tScene * 0.01);

    float marchLen = tExit - tEnter;
    // Passo alvo ~ 4% da altura da coluna, MAS nunca maior que meia celula de
    // ruido: acima disso a textura da fumaça nao chega na imagem, so' o
    // aliasing dela chega. A pluma grande do parana_field tem celula de 40 u e
    // 640 u de altura - o alvo antigo (25,6 u) perdia toda a primeira oitava.
    // O teto de passos do preset continua mandando, entao isto nao pode
    // estourar o custo: no maximo aproveita melhor os passos que ja' existem.
    float cell = 1.0 / max(pc.wind.z, 1e-4);
    float stepTarget = max(min(pc.shape.x * 0.04, cell * 0.5), 1.5);
    int steps = int(clamp(marchLen / stepTarget, 8.0, max(pc.misc.y, 8.0)));
    float dt = marchLen / float(steps);
    // Fagulhas e chama tem celula bem menor que a fumaça (1/0,157 = 6,4 u e
    // 1/0,34 = 2,9 u contra 40 u): com passo grande elas nao viram brasa, viram
    // tracinho alinhado com o raio. Somem junto com as oitavas finas.
    float sparkFade = octaveWeight(dt * 0.157);
    float fireFade = octaveWeight(dt * 0.34);

    vec2 fullResCoord = floor(gl_FragCoord.xy * max(1.0, 1.0 / pc.misc.z));
    float jitter = interleavedGradientNoise(fullResCoord);

    // sunDir aponta DO sol PARA o chão (y < 0): a amostra de auto-sombra
    // anda no sentido oposto, em direção ao sol.
    vec3 sunStep = -normalize(sunDir) * (pc.shape.x * 0.22);
    float sunLevel = clamp(sunIntensity, 0.15, 1.5);

    // Paleta: núcleo escuro (fuligem), borda iluminada pelo sol. Os fatores
    // são baixos de propósito: a cena é escura (terra vulcânica) e a
    // auto-exposição estoura qualquer coisa perto de 1.0 em HDR — fumaça
    // "clara demais" vira uma bolha branca.
    vec3 dark = pc.color.rgb * 0.085;
    vec3 lightC = pc.color.rgb * (0.14 + 0.29 * sunLevel);
    vec3 ember = vec3(1.0, 0.33, 0.06) * pc.color.a;
    vec3 sparkTint = vec3(1.0, 0.24, 0.045) * pc.color.a;
    float sparkRise = max(pc.shape.z, 0.01) * 0.85;

    vec4 accum = vec4(0.0);
    for (int i = 0; i < steps; ++i) {
        float t = tEnter + (float(i) + jitter) * dt;
        vec3 p = ro + rd * t;
        float h;
        float dens = smokeDensity(p, detail, dt, h);
        dens *= clamp((tScene - t) / fadeLen, 0.0, 1.0);
        if (dens > 0.002) {
            // Sombreamento direcional barato: uma amostra na direção do sol
            // (a fumaça recebe sombra de si mesma) + gradiente vertical.
            float shadowH;
            float sunT = exp(-dens * 2.5);
            if (sunTap) {
                float dsun = smokeDensity(p + sunStep, false, dt, shadowH);
                sunT = exp(-(dens * 0.6 + dsun * 2.2));
            }
            float lit = clamp(sunT * (0.35 + 0.65 * h) + 0.08, 0.0, 1.0);
            vec3 base = mix(dark, lightC, lit);
            base += ember * exp(-h * 16.0);

            if (isFire) {
                float ft = time * (2.3 + 1.7 * fract(pc.misc.x * 0.013));
                vec3 fp = p * 0.19 - vec3(0.0, ft * 0.55, 0.0);
                float f1 = valueNoise3D(fp, uint(pc.misc.x) + 421u);
                float f2 = valueNoise3D(fp * 2.6 + 11.0, uint(pc.misc.x) + 733u);
                float tongue = clamp(f1 * 0.62 + f2 * 0.38, 0.0, 1.0);
                float taper = exp(-h * 2.9);
                float heat = clamp((tongue - (0.30 + 0.55 * h)) * 3.4, 0.0, 1.0) * taper * fireFade;
                float core = pow(heat, 3.0);
                vec3 sulfur = vec3(1.0, 0.86, 0.30);
                vec3 flame = vec3(1.0, 0.42, 0.08);
                vec3 fireCol = mix(flame, sulfur, core);
                base = mix(base, fireCol, clamp(heat * 1.25, 0.0, 1.0));
                base += fireCol * heat * (3.2 + 4.0 * core);
            }

            vec3 sp = p - vec3(0.0, time * sparkRise, 0.0);
            float s1 = valueNoise3D(sp * 0.085, uint(pc.misc.x) + 977u);
            float s2 = valueNoise3D(sp * 0.230, uint(pc.misc.x) + 613u);
            float spark = smoothstep(0.74, 0.95, s1 * 0.5 + s2 * 0.5);
            spark *= exp(-h * 2.6) * clamp(dens * 3.0, 0.0, 1.0) * sparkFade;
            base += sparkTint * spark * 5.5;

            float a = 1.0 - exp(-dens * dt * 0.45);
            accum.rgb += (1.0 - accum.a) * base * a;
            accum.a += (1.0 - accum.a) * a;
            if (accum.a > 0.97) break;
        }
    }
    if (accum.a <= 0.004) discard;

    outColor = accum;
}
