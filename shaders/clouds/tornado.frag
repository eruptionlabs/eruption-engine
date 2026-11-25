#version 450

// Tornado: funil volumetrico + saia de poeira + nuvem-mae, ray-marchados numa so'
// caixa. Mesmo caminho da coluna de fumaca (smoke_plume.frag: cubo unitario, oclusao
// analitica pela profundidade da cena, alvo reduzido opcional), com densidade
// ANALITICA:
//
//   * eixo curvo (corda que balanca) e raio por altura: as MESMAS contas de
//     renderer/vfx/TornadoShape.hpp, usadas tambem pelos destrocos e pela caixa
//   * ruido 3D amostrado no referencial da parede (coordenadas normalizadas pelo raio
//     local, entao o tamanho do detalhe acompanha o funil) e torcido DEPOIS de gerado:
//     o angulo de giro depende de h e de rr (rotacao diferencial: mais rapida perto do
//     eixo e embaixo) + helice com a altura
//   * dois fluxos defasados meio periodo e misturados por pesos triangulares
//     (Vlachos, SIGGRAPH 2010): o cisalhamento do giro nunca cresce sem limite e a
//     imagem nao pulsa
//   * domain warp de uma amostra extra antes do fbm
//   * parede mais densa que o miolo; erosao da borda pelo ruido
//   * luz: 0..3 amostras de densidade em direcao ao sol (auto-sombra), miolo escuro e
//     borda clara, poeira do chao embaixo e cinza de nuvem em cima
//
// O ray-march salta o vazio por uma distancia conservadora ate' o envelope do funil
// (passo grande fora, passo proporcional ao raio local dentro).

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

layout(set = 2, binding = 0) uniform sampler2D sceneDepth;

// Precisa bater com smoke_plume.vert (que so' le' boxMin/boxMax) e com
// CloudLayerRenderer::renderTornado.
layout(push_constant) uniform PushConstants {
    vec4 boxMin;   // xyz = caixa, w = flags (1 sem corte de profundidade, 2 detalhe, 4 nuvem-mae)
    vec4 boxMax;   // xyz = caixa, w = amostras de sombra (0..3)
    vec4 origin;   // xyz = pe, w = altura
    vec4 shape;    // x = raio topo, y = raio base, z = giro, w = intensidade (saida)
    vec4 dust;     // rgb = cor da poeira, w = idade (s)
    vec4 misc;     // x = semente, y = iteracoes maximas, z = targetScale, w = descida (0..1)
    vec4 extra;    // x = raio da nuvem-mae, y = raio da saia de poeira
} pc;

const float kPeriod = 3.4; // s de cada fluxo antes de reiniciar o cisalhamento

// --- ruido -----------------------------------------------------------------

float hash3(ivec3 p) {
    uvec3 q = uvec3(p) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = (q.x ^ q.y ^ q.z) * 1597334673u;
    return float(n >> 8) * (1.0 / 16777216.0);
}

float noise3(vec3 p) {
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash3(i), b = hash3(i + ivec3(1, 0, 0));
    float c = hash3(i + ivec3(0, 1, 0)), d = hash3(i + ivec3(1, 1, 0));
    float e = hash3(i + ivec3(0, 0, 1)), g = hash3(i + ivec3(1, 0, 1));
    float h = hash3(i + ivec3(0, 1, 1)), k = hash3(i + ivec3(1, 1, 1));
    return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y),
               mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}

float interleavedGradientNoise(vec2 fragCoord) {
    return fract(52.9829189 * fract(dot(floor(fragCoord), vec2(0.06711056, 0.00583715))));
}

// --- forma (igual a TornadoShape.hpp) ----------------------------------------

float smooth01(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

vec2 axisOffset(float h, float age) {
    float H = pc.origin.w;
    float seed = pc.misc.x;
    float hh = clamp(h, 0.0, 1.3);
    vec2 lean = vec2(cos(seed), sin(seed)) * (0.14 * H * hh * hh);
    float sw = 0.055 * H * hh;
    vec2 swing = sw * vec2(sin(age * 0.45 + hh * 2.7 + seed * 1.7), cos(age * 0.36 + hh * 3.3 + seed * 0.9));
    vec2 kink = (0.010 * H * hh) * vec2(sin(age * 1.3 - hh * 8.5 + seed), cos(age * 1.1 - hh * 7.5 + seed * 2.3));
    return lean + swing + kink;
}

float radiusAt(float h, float age) {
    float t = clamp(h, 0.0, 1.3);
    float r = pc.shape.y + (pc.shape.x - pc.shape.y) * pow(min(t, 1.0), 1.6);
    r *= 1.0 + 0.9 * smooth01(0.80, 1.05, t);
    r *= 1.0 + 0.10 * sin(t * 9.0 - age * 0.7 + pc.misc.x * 2.0) * (0.3 + t);
    return r;
}

float angularSpeed(float h, float rr) {
    return 0.6 * pc.shape.z * (0.55 + 1.6 * (1.0 - clamp(h, 0.0, 1.0))) / (0.45 + rr);
}

vec2 rot2(vec2 v, float a) {
    float c = cos(a), s = sin(a);
    return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

// Pesos dos dois fluxos: triangulares, somam 1; o fluxo reinicia onde seu peso e' 0.
void flowWeights(float age, out float ph0, out float ph1, out float w0, out float w1) {
    ph0 = fract(age / kPeriod);
    ph1 = fract(age / kPeriod + 0.5);
    w0 = 1.0 - abs(2.0 * ph0 - 1.0);
    w1 = 1.0 - w0;
}

// fbm de 2 ou 3 oitavas; as oitavas finas somem com o passo (Nyquist), como na fumaca.
float fbmR(vec3 p, bool detail, float fine, out float ridge) {
    float n1 = noise3(p);
    float n2 = noise3(p * 2.07 + 17.3);
    // filamentos: a crista (1 - |2n-1|) da 2a oitava, que some junto com ela
    ridge = pow(1.0 - abs(2.0 * n2 - 1.0), 2.5) * fine;
    float n = n1 * 0.55 + n2 * 0.30 * fine;
    float norm = 0.55 + 0.30 * fine;
    if (detail) {
        n += noise3(p * 4.13 + 41.7) * 0.15 * fine;
        norm += 0.15 * fine;
    }
    return n / norm;
}

float fbm(vec3 p, bool detail, float fine) {
    float r;
    return fbmR(p, detail, fine, r);
}

// Ruido de cada termo na ultima amostra: da' o relevo (bolhas claras e escuras) na luz.
float gNoiseF = 0.5, gNoiseS = 0.5, gNoiseM = 0.5;
// Miolo de condensacao (1) x veu de poeira da casca (0) da ultima amostra do funil.
float gCore = 1.0;

// --- densidades --------------------------------------------------------------

// Densidade do funil. `lod` 0 = so' as duas primeiras oitavas, um fluxo (sombra);
// 1 = completo. rrOut = raio relativo, hOut = altura relativa.
float funnelDensity(vec3 p, float age, float lod, bool detail, float fine, out float hOut, out float rrOut) {
    float H = pc.origin.w;
    float h = (p.y - pc.origin.y) / H;
    hOut = h;
    rrOut = 2.0;
    if (h < -0.01 || h > 1.15) return 0.0;
    float hc = clamp(h, 0.0, 1.3);

    // A corda desce da nuvem (descida) e sobe na saida (intensidade).
    float reach = min(pc.misc.w, pc.shape.w);
    float hTip = 1.0 - reach;
    float tipMask = smooth01(hTip - 0.04, hTip + 0.16, h);
    if (tipMask <= 0.0) return 0.0;

    vec2 axis = pc.origin.xz + axisOffset(h, age);
    float r = radiusAt(h, age);
    // A ponta afina: perto do fim da corda o raio cai.
    r *= mix(1.0 - (0.70 * (1.0 - pc.misc.w) + 0.08), 1.0, smooth01(hTip, hTip + 0.35, h)); // so' afina enquanto desce
    // Espessura irregular: o funil incha e estrangula ao longo da altura.
    r *= 1.0 + 0.30 * (noise3(vec3(h * 6.0, age * 0.25, 3.3)) - 0.5);
    vec2 q = p.xz - axis;
    float rr = length(q) / r;
    rrOut = rr;
    if (rr > 1.7) return 0.0;

    // Dois fluxos defasados; cada um gira, sobe e se enrola em helice.
    float ph0, ph1, w0, w1;
    flowWeights(age, ph0, ph1, w0, w1);
    float om = angularSpeed(h, rr);
    float spiral = 11.0 * hc;                      // helice: o padrao se enrola com a altura
    float baseAng = pc.shape.z * 0.21 * age;       // giro global continuo
    float vscale = 0.75 * 0.5 * (pc.shape.x + pc.shape.y);
    float rise = 0.38 * pc.shape.x * pc.shape.z;   // m/s do padrao subindo
    vec2 qn = q / r;

    float nsum = 0.0, rsum = 0.0, bsum = 0.0, wsum2 = 0.0, wsum1 = 0.0;
    for (int k = 0; k < 2; ++k) {
        float w = k == 0 ? w0 : w1;
        if (w < 0.02) continue;
        float ph = k == 0 ? ph0 : ph1;
        float t = ph * kPeriod;
        vec2 rq = rot2(qn, baseAng + om * t + spiral);
        vec3 np = vec3(rq.x * 2.1, (p.y - rise * t) / vscale + float(k) * 7.7, rq.y * 2.1);
        // domain warp: uma amostra de ruido desloca a posicao antes do fbm
        float wv = noise3(np * 0.60 + 31.7) - 0.5;
        np += vec3(wv, 0.6 * wv, -wv) * 1.5;
        float rg;
        float n = fbmR(np, detail && lod > 0.5, fine, rg);
        // bojos grandes: o raio da parede incha e afunda em lobos que sobem girando
        float bg = noise3(vec3(rq.x * 1.35, (p.y - rise * t) / (vscale * 3.2) + 3.1 * float(k), rq.y * 1.35) + 5.7);
        nsum += n * w;
        bsum += bg * w;
        rsum += rg * w;
        wsum2 += w * w;
        wsum1 += w;
        if (lod < 0.5) break; // sombra: um fluxo basta
    }
    float n, ridge, bulge;
    if (lod < 0.5) {
        n = nsum / max(wsum1, 0.02);
        ridge = rsum / max(wsum1, 0.02);
        bulge = bsum / max(wsum1, 0.02);
    } else {
        float gk = inversesqrt(max(wsum2, 0.25));
        n = 0.5 + (nsum - 0.5) * gk;
        ridge = rsum;
        bulge = 0.5 + (bsum - 0.5) * gk;
    }
    n = clamp(n, 0.0, 1.0);

    gNoiseF = n;
    // Parede mais densa que o miolo; borda erodida pelo ruido (billows enrolados) e
    // rasgada pelos filamentos.
    // Estrias finas de alta frequencia girando com a parede (esticadas na vertical) e borda esfiapada.
    float sAng = atan(q.y, q.x) + baseAng + spiral + age * 0.9;
    float sn = noise3(vec3(sAng * 9.0, h * 14.0, 1.7)) * 0.65 + noise3(vec3(sAng * 21.0, h * 31.0, 8.1)) * 0.35;
    float edge = rr + (n - 0.5) * 1.10 + (ridge - 0.25) * 0.22 + (bulge - 0.5) * 1.60;
    edge += (sn - 0.5) * 0.55 * smooth01(0.35, 0.9, rr) * fine;
    // bolhas que saem da casca: onde o lobo grande passa de um limiar, o contorno vai para fora
    edge -= 0.55 * smooth01(0.55, 0.80, bulge) * smooth01(0.35, 0.75, n);
    edge -= 0.40 * (1.0 - smooth01(0.0, 0.5, hc)); // a corda perto do chao tem mais miolo denso
    float shell = 1.0 - smooth01(0.62, 1.04, edge);
    float coreFill = mix(0.55, 1.0, smooth01(0.05, 0.62, edge));
    float body = shell * coreFill;
    float streak = smooth01(0.44, 0.58, n);
    // Aneis empilhados subindo, com o periodo proporcional ao raio local (nao aliasa no alto).
    float bandAng = atan(q.y, q.x);
    float bands = 0.72 + 0.28 * sin(h * H / (0.45 * r) + bandAng * 2.0 - age * 2.2 + rr * 2.0 + pc.misc.x);
    // So' o miolo de condensacao e' denso; a casca externa vira veus finos de poeira
    // (densidade ~35%), esfiapados pelo ruido, em que se ve o cenario atras.
    float core = 1.0 - smooth01(0.10, 0.60, edge);
    gCore = core;
    float veil = 0.35 * mix(0.55, 1.0, streak);
    float d = body * (0.70 + 2.2 * streak * streak + 1.1 * ridge * streak) * bands * mix(veil, 1.0, core) * mix(0.70, 1.25, sn);

    // Na base ancorada fica fino demais para a parede; embaixo a corda e' mais densa e escura.
    d *= tipMask;
    d *= mix(2.0, 1.0, smooth01(0.0, 0.55, h)); // a corda embaixo e' mais densa
    // Encontro com a nuvem-mae: some suavemente acima da base dela.
    d *= 1.0 - smooth01(1.10, 1.20, h) * ((int(pc.boxMin.w + 0.5) & 4) != 0 ? 1.0 : 0.0);
    return d;
}

// Saia de poeira: anel que rola para fora e para cima, ao redor do pe. O ruido gira com a
// parede (espirais de poeira), e' empurrado para fora com o tempo (o rolo se abre) e tem
// lobos grandes no contorno: nao e' um nevoeiro liso, sao ondas de poeira.
float skirtDensity(vec3 p, float age, bool detail, float fine, out float gOut) {
    float Rs = pc.extra.y;
    float g = p.y - pc.origin.y;
    gOut = g;
    float hs = Rs * 0.42; // saia rente ao chao: mais larga que alta
    float gTop = Rs * 0.95; // dentro do envelope de salto do laco principal (Rs * 1.7)
    if (g < -2.0 || g > gTop) return 0.0;
    float grow = smooth01(0.55, 1.0, pc.misc.w) * pc.shape.w; // so' depois da corda tocar o chao
    if (grow <= 0.0) return 0.0;
    vec2 q = p.xz - pc.origin.xz;
    float s = length(q) / Rs;
    if (s > 1.7) return 0.0;

    float ph0, ph1, w0, w1;
    flowWeights(age, ph0, ph1, w0, w1);
    float nsum = 0.0, rsum = 0.0, bsum = 0.0, wsum2 = 0.0;
    for (int k = 0; k < 2; ++k) {
        float w = k == 0 ? w0 : w1;
        if (w < 0.02) continue;
        float t = (k == 0 ? ph0 : ph1) * kPeriod;
        // giro em volta do pe (mais rapido por dentro) e deslocamento radial para fora
        float so = max(s - 0.45 * t / kPeriod * 1.6, 0.0);
        vec2 rq = rot2(q / Rs, pc.shape.z * 0.55 * age + 1.5 * t / (0.30 + s));
        vec2 rn = rq * (so + 0.15) / max(s, 0.05);
        vec3 np = vec3(rn.x * 4.2, g / hs * 1.6 - 0.9 * t / kPeriod, rn.y * 4.2) + vec3(float(k) * 9.1);
        float rg;
        float n = fbmR(np, detail, fine, rg);
        float bg = noise3(vec3(rn.x * 1.5, g / hs * 0.5, rn.y * 1.5) + vec3(float(k) * 4.3 + 11.0));
        nsum += n * w;
        rsum += rg * w;
        bsum += bg * w;
        wsum2 += w * w;
    }
    float gk = inversesqrt(max(wsum2, 0.25));
    float n = clamp(0.5 + (nsum - 0.5) * gk, 0.0, 1.0);
    float bulge = 0.5 + (bsum - 0.5) * gk;
    float ridge = rsum;
    gNoiseS = n;

    // Envelope: domo baixo que afina para fora; o anel mais denso a ~0.6 do raio, com o
    // contorno em lobos.
    float gu = g / hs;
    float height = exp(-max(gu, 0.0) * (1.35 + 0.9 * s)) * smooth01(-0.15, 0.08, gu);
    float radial = smooth01(1.40, 0.40, s + (n - 0.5) * 0.55 + (bulge - 0.5) * 0.95);
    float ring = 0.45 + 0.55 * exp(-pow((s - 0.58) / 0.40, 2.0));
    float billow = smooth01(0.44, 0.62, n);
    float foot = exp(-pow(s / 0.30, 2.0)) * exp(-max(gu, 0.0) * 0.35);
    float dens = height * radial * ring * (0.08 + 1.5 * billow * billow + 0.7 * ridge * billow) + foot * (0.05 + 1.5 * billow * billow);
    // Bracos de poeira em espiral entrando no funil (giram com a parede).
    float arms = 0.5 + 0.5 * sin(3.0 * atan(q.y, q.x) - 7.0 * s + pc.shape.z * 0.55 * age);
    dens *= mix(0.55, 1.35, arms) * 1.30;
    dens *= mix(1.0, 0.40, smooth01(0.45, 1.15, s + (n - 0.5) * 0.5)) * (1.0 - smooth01(gTop * 0.50, gTop, g));
    return dens * grow * 1.15;
}

// Nuvem-mae: laje escura e baixa acima do funil, girando devagar, com o centro
// rebaixado (parede) e a borda fina.
float motherDensity(vec3 p, float age, bool detail, float fine, out float uOut, out float rrOut) {
    uOut = 0.0;
    rrOut = 2.0;
    float H = pc.origin.w;
    float Rm = pc.extra.x;
    float thick = 0.30 * H;
    float top = pc.origin.y + 0.96 * H;
    float u = (p.y - top) / thick;
    uOut = u;
    if (u < -0.92 || u > 1.2) return 0.0;
    vec2 c = pc.origin.xz + axisOffset(1.0, age);
    vec2 q = p.xz - c;
    float rr = length(q) / Rm;
    rrOut = rr;
    if (rr > 1.3) return 0.0;

    vec2 rq0 = rot2(q / Rm, pc.shape.z * 0.30 * age);
    float ph0, ph1, w0, w1;
    flowWeights(age, ph0, ph1, w0, w1);
    float nsum = 0.0, wsum2 = 0.0;
    for (int k = 0; k < 2; ++k) {
        float w = k == 0 ? w0 : w1;
        if (w < 0.02) continue;
        float t = (k == 0 ? ph0 : ph1) * kPeriod;
        // Giro mais rapido no centro; bracos em espiral logaritmica.
        float ang = pc.shape.z * 0.30 * age + 0.9 * t / (0.30 + rr) + 2.4 * log(rr + 0.25);
        vec2 rq = rot2(q / Rm, ang);
        vec3 np = vec3(rq.x * 5.5, u * 2.4 + 0.25 * t, rq.y * 5.5) + vec3(float(k) * 5.3 + 70.0);
        float wv = noise3(np * 0.6 + 13.1) - 0.5;
        np += vec3(wv, 0.5 * wv, -wv) * 1.5;
        nsum += fbm(np, detail, fine) * w;
        wsum2 += w * w;
    }
    float n = clamp(0.5 + (nsum - 0.5) * inversesqrt(max(wsum2, 0.25)), 0.0, 1.0);

    gNoiseM = n;
    // Cumulus pendurados: lobos que descem da base, mais fortes no miolo da laje.
    float lump = noise3(vec3(rq0.x * 2.6, 3.7, rq0.y * 2.6) + vec3(40.0));
    // Base larga e PLANA, com lobos pendurados (mammatus) e uma wall cloud rebaixada logo
    // acima do funil (rr < ~0.3, o centro gira mais rapido): borda de baixo bem definida.
    float flatBase = -0.20 - 0.26 * lump * lump;
    // Estrias (ribbing): costelas radiais que giram com a nuvem e degraus concentricos na base.
    float ribAng = atan(rq0.y, rq0.x);
    float ribs = sin(ribAng * 13.0 + (n - 0.5) * 4.0) * 0.5 + 0.5;
    flatBase += 0.07 * (ribs - 0.5) * smooth01(0.25, 0.6, rr) + 0.035 * sin(rr * 40.0 + (lump - 0.5) * 6.0);
    float baseU = flatBase + (-0.82 - flatBase) * (1.0 - smooth01(0.04, 0.32, rr));
    float topU = 0.90 * (1.0 - 0.50 * rr * rr);
    float inside = smooth01(baseU - 0.008, baseU + 0.040, u + (n - 0.5) * 0.06) *
                   (1.0 - smooth01(topU - 0.30, topU + 0.04, u + (n - 0.5) * 0.35));
    float rim = 1.0 - smooth01(0.86, 1.10, rr + (n - 0.5) * 0.30);
    // Camadas finas empilhadas perto da base: faixas claras e escuras (a luz bate nas dobras).
    float layers = 1.0 - 0.45 * smooth01(0.35, 0.9, sin(u * 34.0 + (n - 0.5) * 6.0 + rr * 5.0) * 0.5 + 0.5) * (1.0 - smooth01(-0.2, 0.5, u));
    float dens = inside * rim * layers * (0.30 + 3.8 * smooth01(0.42, 0.58, n)) * mix(0.55, 1.0, smooth01(-0.75, 0.15, u));
    return dens * pc.shape.w;
}

// Todos os termos juntos (para o raio de sombra, em qualidade baixa).
float totalDensityCheap(vec3 p, float age, float fine, bool mother) {
    float dummyH, dummyR, dummyG;
    float d = funnelDensity(p, age, 0.0, false, fine, dummyH, dummyR);
    d += skirtDensity(p, age, false, fine, dummyG) * 0.8;
    if (mother) d += motherDensity(p, age, false, fine, dummyH, dummyR);
    return d;
}

void main() {
    vec3 ro = cameraPos;
    vec3 rd = normalize(v_worldPos - cameraPos);

    vec3 invD = 1.0 / rd;
    vec3 tA = (pc.boxMin.xyz - ro) * invD;
    vec3 tB = (pc.boxMax.xyz - ro) * invD;
    vec3 tMinV = min(tA, tB);
    vec3 tMaxV = max(tA, tB);
    float tEnter = max(max(tMinV.x, tMinV.y), max(tMinV.z, 0.0));
    float tExit = min(min(tMaxV.x, tMaxV.y), tMaxV.z);
    if (tExit <= tEnter) discard;

    int flags = int(pc.boxMin.w + 0.5);
    bool noDepthClip = (flags & 1) != 0;
    bool detail = (flags & 2) != 0;
    bool motherOn = (flags & 4) != 0;
    int sunTaps = int(pc.boxMax.w + 0.5);
    // Camera acima da base da nuvem-mae (vista de cima): ela some, para o funil aparecer.
    // Vista de cima fica translucida (o topo da tempestade), nao some.
    float mFade = 1.0 - 0.60 * smooth01(pc.origin.y + 0.55 * pc.origin.w, pc.origin.y + 0.95 * pc.origin.w, cameraPos.y);

    float tScene = 1e30;
    vec2 suv = gl_FragCoord.xy / (screenSize * pc.misc.z);
    float sceneZ = noDepthClip ? 1.0 : textureLod(sceneDepth, suv, 0.0).r;
    if (sceneZ < 1.0) {
        vec4 wp = invViewProj * vec4(suv * 2.0 - 1.0, sceneZ, 1.0);
        if (abs(wp.w) < 1e-6) wp.w = 1e-6;
        wp.xyz /= wp.w;
        tScene = dot(wp.xyz - ro, rd);
        if (tScene <= tEnter) discard;
    }
    tExit = min(tExit, tScene);
    if (tExit <= tEnter) discard;
    float fadeLen = max(6.0, tScene * 0.01);

    float age = pc.dust.w;
    float H = pc.origin.w;
    float maxIter = max(pc.misc.y, 8.0);
    // Fracao do passo em raios locais: poucas iteracoes -> passo maior dentro do funil.
    float kStep = clamp(4.5 / maxIter, 0.07, 0.40);
    float minStep = max(H * 0.0012, 0.4);

    vec2 fullResCoord = floor(gl_FragCoord.xy * max(1.0, 1.0 / pc.misc.z));
    // Ruido branco (hash PCG) em vez do gradiente intercalado: este tem estrutura em diagonais
    // que, ampliada pelo alvo reduzido, vira hachura visivel na nuvem-mae.
    uvec2 jc = uvec2(fullResCoord);
    uint jh = jc.x * 1664525u + jc.y * 1013904223u + (frameIndex & 63u) * 2654435761u;
    jh ^= jh >> 16; jh *= 2246822519u; jh ^= jh >> 13; jh *= 3266489917u; jh ^= jh >> 16;
    float jitter = float(jh & 0xFFFFFFu) / 16777216.0;

    vec3 toSun = -normalize(sunDir);
    float sunLevel = clamp(sunIntensity, 0.15, 1.5);
    // Paleta: o cinza de nuvem recebe sol com a auto-sombra; o miolo e' escuro. Os
    // fatores sao baixos como os da fumaca: a exposicao automatica estoura perto de 1.
    vec3 cloudAlb = vec3(0.34, 0.38, 0.47); // condensacao: cinza-azulado escuro
    vec3 motherAlb = vec3(0.30, 0.31, 0.36);
    vec3 dustCol = pc.dust.rgb;
    vec3 ambient = vec3(0.56, 0.56, 0.60) * (0.060 + 0.070 * sunLevel);
    vec3 sunCol = vec3(1.0, 0.92, 0.80) * (0.090 + 0.30 * sunLevel);

    // Intervalo do raio dentro da nuvem-mae: cilindro vertical x laje.
    float tm0 = 1e30, tm1 = -1e30;
    if (motherOn) {
        vec2 cM = pc.origin.xz + axisOffset(1.0, age);
        float Rc = pc.extra.x * 1.3;
        float yA = pc.origin.y + 0.67 * H, yB = pc.origin.y + 1.32 * H;
        float a0 = tEnter, a1 = tExit;
        bool ok = true;
        if (abs(rd.y) < 1e-5) {
            ok = ro.y >= yA && ro.y <= yB;
        } else {
            float s1 = (yA - ro.y) / rd.y, s2 = (yB - ro.y) / rd.y;
            a0 = max(a0, min(s1, s2));
            a1 = min(a1, max(s1, s2));
        }
        vec2 oc = ro.xz - cM;
        float qa = dot(rd.xz, rd.xz), qb = dot(oc, rd.xz), qc = dot(oc, oc) - Rc * Rc;
        if (qa < 1e-8) {
            ok = ok && qc < 0.0;
        } else {
            float disc = qb * qb - qa * qc;
            if (disc < 0.0) ok = false;
            else {
                float sq = sqrt(disc);
                a0 = max(a0, (-qb - sq) / qa);
                a1 = min(a1, (-qb + sq) / qa);
            }
        }
        if (ok && a1 > a0) { tm0 = a0; tm1 = a1; }
    }

    vec4 accum = vec4(0.0);
    float t = tEnter + jitter * minStep * 2.0;
    int maxI = int(maxIter) * 4; // passos de salto sao baratos (sem densidade)
    for (int i = 0; i < maxI; ++i) {
        if (t >= tExit) break;
        vec3 p = ro + rd * t;

        // Distancia conservadora ate' o envelope do funil (xz) para saltar o vazio.
        float hh = (p.y - pc.origin.y) / H;
        float hEnv = clamp(hh, 0.0, 1.15);
        vec2 axisE = pc.origin.xz + axisOffset(hEnv, age);
        float rEnv = radiusAt(hEnv, age) * 1.7;
        float distFun = length(p.xz - axisE) - rEnv;
        float distSkirt = length(p.xz - pc.origin.xz) - pc.extra.y * 1.7;
        float yAbove = p.y - pc.origin.y;
        float dSkirtY = max(yAbove - pc.extra.y * 1.7, -yAbove - 2.0);
        // O funil e a saia saltam o vazio pela distancia ao envelope; a nuvem-mae e' um intervalo
        // exato do raio (cilindro x laje, calculado antes do laco): um salto por distancia ate'
        // um plano horizontal converge para ele sem nunca chegar (linhas horizontais na imagem).
        float distFS = min(distFun, max(distSkirt, dSkirtY));
        bool inMother = motherOn && t >= tm0 && t <= tm1;
        if (!inMother && distFS > 0.0) {
            float skip = distFS * 0.8; // raio rasante rumo ao funil: com 0.5 o salto converge sem entrar (linha no nivel da camera)
            if (motherOn && t < tm0) skip = min(skip, tm0 - t + 0.01);
            t += max(skip, minStep);
            continue;
        }

        float rLocal = radiusAt(hEnv, age);
        float dt = max(rLocal * kStep, minStep);
        // Dentro so' da nuvem-mae (fora do envelope do funil) o passo acompanha a escala dela.
        if (motherOn && distFun > 0.0 && hh > 0.60) dt = max(dt, pc.extra.x * kStep * 0.45);
        if (hh < 0.12) dt = max(min(dt, pc.extra.y * kStep), minStep);
        // O ponto de amostra balanca dentro do passo (ruido por pixel): sem isso os passos grandes
        // da nuvem-mae viram fatias horizontais; com ele viram granulado fino, que o TAA come.
        p = ro + rd * clamp(t + (jitter - 0.5) * dt, tEnter, tExit);
        // Oitavas finas somem quando o passo nao as resolve (aliasing vira granulado).
        float cellFine = dt / max(rLocal * 0.43, 1e-3); // passo em celulas da 1a oitava
        float fine = 1.0 - smooth01(0.45, 1.1, cellFine);

        float h, rrF, uM, rrM, gS;
        float dF = funnelDensity(p, age, 1.0, detail, fine, h, rrF);
        float dS = skirtDensity(p, age, detail, fine, gS);
        float dM = motherOn ? motherDensity(p, age, detail, fine, uM, rrM) * mFade * smooth01(0.50 * H, 1.10 * H, t) : 0.0; // nada de nevoa colada na camera
        float dens = dF + dS + dM;
        float coreF = gCore;
        float upF = smooth01(0.30, 0.65, hh); // em cima a condensacao e' lisa: o ruido pesa menos
        float nF = mix(smooth01(0.30, 0.70, gNoiseF), 0.55, 0.60 * upF), nS = smooth01(0.30, 0.70, gNoiseS), nM = smooth01(0.30, 0.70, gNoiseM); // as amostras de sombra abaixo sobrescrevem os globais
        dens *= clamp((tScene - t) / fadeLen, 0.0, 1.0);
        if (dens > 0.004) {
            // Auto-sombra: amostras de densidade no caminho ate' o sol.
            float occl = 0.0;
            float stepL = max(rLocal * 0.9, H * 0.02);
            for (int k = 0; k < 3; ++k) {
                if (k >= sunTaps) break;
                float s = stepL * (0.5 + float(k) * 0.9);
                occl += totalDensityCheap(p + toSun * s, age, fine, motherOn) * s;
            }
            float sunT = exp(-occl * (sunTaps > 0 ? 5.5 / max(rLocal, 1.0) : 0.0));
            if (sunTaps == 0) sunT = exp(-dens * 1.6) * 0.8 + 0.2;
            // Borda clara, miolo escuro (com gradacao): o interior recebe menos luz mesmo sem sombra.
            float coreDark = mix(0.11, 1.0, smooth01(0.05, 0.95, rrF));
            // Lado do sol claro, lado oposto escuro: a normal da parede e' a direcao radial.
            vec2 radial = normalize(p.xz - (pc.origin.xz + axisOffset(clamp(h, 0.0, 1.2), age)) + vec2(1e-3));
            float side = 0.5 + 0.5 * dot(radial, normalize(toSun.xz + vec2(1e-4)));
            float sideLit = mix(1.0, mix(0.14, 1.12, side * side), smooth01(0.20, 0.85, rrF));
            // Poeira do chao embaixo (marrom), cinza de nuvem em cima.
            float dustAmt = 0.05 + 0.95 * (1.0 - smooth01(0.06, 0.38, max(h, 0.0)));
            vec3 alb = mix(cloudAlb, dustCol * 1.3, clamp(dustAmt * 0.85, 0.0, 1.0));
            alb *= mix(1.0, 0.42, (1.0 - smooth01(0.0, 0.55, h)) * (dF / max(dens, 1e-4)));
            float wF = dF / max(dens, 1e-4), wS = dS / max(dens, 1e-4), wM = dM / max(dens, 1e-4);
            // Relevo: as bolhas do proprio ruido ficam claras na crista e escuras no vale.
            float relief = wF * mix(0.28, 1.65, nF) + wS * mix(0.18, 1.80, nS) + wM * mix(0.12, 1.90, nM);
            // A saia fica mais escura embaixo (sombra do proprio rolo) e clara no alto.
            float skirtShade = mix(0.22, 1.0, smooth01(0.0, 0.9, gS / max(pc.extra.y * 0.62, 1.0)));
            vec3 albedo = alb * wF + dustCol * 1.15 * skirtShade * wS + motherAlb * wM;
            albedo /= max(wF + wS + wM, 1e-3);
            float lit = mix(coreDark * mix(1.0, sideLit, wF), 1.0, wS + wM * 0.6);
            lit *= mix(1.0, 0.10 + 1.15 * smooth01(-0.6, 0.65, uM), wM);
            vec3 col = albedo * (ambient * mix(0.7, 1.2, relief * 0.5) + sunCol * sunT * lit) * relief;
            // Brilho na borda iluminada: contra-luz fino quando a densidade e' baixa.
            col += albedo * sunCol * (0.10 + 0.9 * pow(max(dot(rd, toSun), 0.0), 3.0)) * sunT * (1.0 - smooth01(0.0, 0.6, dens)) * wF;

            // Borda fina contra o ceu: um brilho frio e discreto onde a parede e' rasa, mais forte no lado do sol.
            float rimF = smooth01(0.62, 1.0, rrF) * wF;
            col += (vec3(0.40, 0.48, 0.60) * 0.030 + albedo * sunCol * 0.22 * side * side) * rimF * sunT;

            // Extincao por termo: o miolo de condensacao e' denso (9), a casca e' veu (3,5), a saia
            // rala (3,2) e a nuvem-mae meio translucida (7): o cenario aparece atras dos veus.
            float lowF = 1.0 - smooth01(0.0, 0.55, h); // a corda perto do chao e' mais densa e escura (terra no ar)
            float kExt = (1.8 * (1.0 - coreF) + 8.0 * coreF) * (1.0 + 0.9 * lowF);
            float k = kExt * wF + 3.0 * wS + 3.2 * wM;
            float a = 1.0 - exp(-dens * dt * (k / max(mix(rLocal, pc.extra.y * 0.5, wS), 1.0)));
            accum.rgb += (1.0 - accum.a) * col * a;
            accum.a += (1.0 - accum.a) * a;
            if (accum.a > 0.985) break;
        }
        t += dt;
    }
    if (accum.a <= 0.004) discard;
    outColor = accum;
}
