#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec3 vNormal;
layout(location = 3) in vec3 vTangent;
layout(location = 4) in vec3 vBitangent;
layout(location = 5) flat in float vLevel;

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(set = 1, binding = 0) uniform WaterUBO {
    vec4 baseColorDeep;
    vec4 baseColorShallow;
    vec4 waterParams;         // x=transparency, y=refractionStrength, z=reflectivity, w=roughness
    vec4 normalParams;        // x=scale, y=waveSpeed, z=strength, w=unused
    vec4 waveParams;          // x=amplitude, y=frequency, z=speed, w=unused
    vec4 foamParams;          // x=edgeDepth, y=contactStrength, z=enable, w=unused
    vec4 skyTop;
    vec4 skyHorizon;
    vec4 sunDirIntensity;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 cameraPos;
    vec4 waterLevelAndPlanes; // x=waterLevel, y=nearPlane, z=farPlane, w=time
    vec4 screenSize;          // x=viewportW, y=viewportH, z=cameraPitch, w=unused
    vec4 waveDirAmp[3];
    vec4 waveSpeedSteep[3];
    ivec4 textureSlots;       // x=screenTex, y=depthTex, z=foamMask, w=waterTex
    ivec4 skyboxSlot;         // x=skyboxTex, yzw=unused
    mat4 invViewProj;
    vec4 extraParams;         // x=absorptionCoeff, y=maxPxScale, z=enableCaustics, w=unused
    vec4 foamSurfaceParams;   // x=foamScale, y=foamSpeed, z=foamRoughness, w=enableSurfaceFoam
    vec4 normalAdvanced;      // x=normalOctaves, y=lavaEmissive, zw=unused
    vec4 causticsParams;      // x=intensity, y=depthAttenuation, z=lava flowSpeed, w=unused
    vec4 weatherParams;       // x=sunOcclusion, y=moonOcclusion, z=stormDarken, w=rainIntensity
} ubo;

layout(push_constant) uniform Push {
    mat4 viewProj;
    ivec4 debugMask;   // x = bits de A/B do menu, y = diagnostico de cobertura
} pc;

vec2 getScreenUV() {
    return gl_FragCoord.xy / ubo.screenSize.xy;
}

float linearizeDepth(float depth, float near, float far) {
    return near * far / (far - depth * (far - near));
}

vec3 reconstructWorldPos(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = ubo.invViewProj * clip;
    return world.xyz / world.w;
}

bool validVec3(vec3 v) {
    return !any(isnan(v)) && !any(isinf(v));
}

// ============================================================================
// Simplex Noise 2D (Stefan Gustavson / Ian McEwan, public domain)
// ============================================================================
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x*34.0)+1.0)*x); }

float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439,
                       -0.577350269189626, 0.024390243902439);
    vec2 i  = floor(v + dot(v, C.yy));
    vec2 x0 = v -   i + dot(i, C.xx);
    vec2 i1;
    i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289(i);
    vec3 p = permute( permute( i.y + vec3(0.0, i1.y, 1.0 ))
                           + i.x + vec3(0.0, i1.x, 1.0 ));
    vec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy),
                            dot(x12.zw,x12.zw)), 0.0);
    m = m*m;
    m = m*m;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * ( a0*a0 + h*h );
    vec3 g;
    g.x  = a0.x  * x0.x  + h.x  * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

// ============================================================================
// FBM (Fractal Brownian Motion) with Simplex
// ============================================================================
float fbmSimplex(vec2 p, int octaves, float time, float speed) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    vec2 shift = vec2(100.0);
    mat2 rot = mat2(cos(0.5), sin(0.5), -sin(0.5), cos(0.5));
    for (int i = 0; i < 4; i++) {
        if (i >= octaves) break;
        value += amplitude * snoise(p * frequency + time * speed + float(i) * shift);
        p = rot * p * 2.0 + shift;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return value;
}

// ============================================================================
// Worley Noise (Cellular/Voronoi) — Distance to nearest feature point
// ============================================================================
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// Celulas da crosta da lava: (F1, F2) do Voronoi. Quem move a crosta e' o
// fluxo (lavaColor); os pontos ficam parados na celula (sem dois senos por
// vizinho, que eram metade do custo do Voronoi).
vec2 lavaCells(vec2 p) {
    vec2 ip = floor(p), fp = fract(p);
    float f1 = 8.0, f2 = 8.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 g = vec2(float(x), float(y));
            vec2 h = hash22(ip + g);
            vec2 r = g + 0.12 + 0.76 * h - fp;
            float d = dot(r, r);
            if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) { f2 = d; }
        }
    }
    return sqrt(vec2(f1, f2));
}

float worley(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float minDist = 1.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 point = neighbor + hash22(i + neighbor);
            float dist = length(neighbor + point - f);
            minDist = min(minDist, dist);
        }
    }
    return minDist;
}

// ============================================================================
// Multi-octave Normal from FBM Simplex
// ============================================================================
vec3 sampleProceduralNormal(vec2 worldXZ, float scale, float speed, float time, float strength, int octaves) {
    vec2 uv = worldXZ * scale;
    float h  = fbmSimplex(uv, octaves, time, speed);
    float hx = fbmSimplex(uv + vec2(0.005, 0.0), octaves, time, speed);
    float hy = fbmSimplex(uv + vec2(0.0, 0.005), octaves, time, speed);
    float derivScale = 200.0;
    vec3 n = vec3((h - hx) * derivScale, 1.0, (h - hy) * derivScale);
    float nLen = length(n);
    if (nLen < 1e-5) return vec3(0.0, 1.0, 0.0);
    n = n / nLen;
    vec3 result = vec3(n.x * strength, n.y, n.z * strength);
    float rLen = length(result);
    return (rLen > 1e-5) ? (result / rLen) : vec3(0.0, 1.0, 0.0);
}

// ============================================================================
// Surface Foam (Worley-based)
// ============================================================================
float foamSurface(vec2 worldXZ, float scale, float speed, float time) {
    vec2 p = worldXZ * scale + vec2(time * speed * 0.15, time * speed * 0.12);
    float dist = worley(p);
    // Fine foam: cell edges (high contrast)
    float foamFine = 1.0 - smoothstep(0.0, 0.12, dist);
    // Gross foam: cell interior (inverted, smoothed)
    float foamGrossa = smoothstep(0.25, 0.55, 1.0 - dist);
    // Unified
    return max(foamFine, foamGrossa * 0.6);
}

// ============================================================================
// Wave Crest Mask (where foam is allowed to appear)
// ============================================================================
float waveCrestMask(vec3 perturbedNormal, float threshold) {
    // High Y in perturbed normal means the surface is facing up = crest/top
    return smoothstep(threshold - 0.1, threshold + 0.1, perturbedNormal.y);
}

// ============================================================================
// Rain splash contact foam (high-frequency sparkling ripples)
// ============================================================================
float rainSplashPattern(vec2 worldXZ, float t) {
    // Each drop creates a small expanding ring; many overlapping drops
    vec2 p = worldXZ * 2.5;
    vec2 cell = floor(p);
    float splash = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = cell + vec2(float(x), float(y));
            vec2 rnd = fract(sin(vec2(dot(neighbor, vec2(127.1, 311.7)),
                                      dot(neighbor, vec2(269.5, 183.3)))) * 43758.5453);
            float phase = fract(rnd.x + t * (0.8 + rnd.y * 0.6));
            float radius = phase * 0.9; // expanding ring
            vec2 center = neighbor + rnd * 0.8 + 0.1;
            float d = length(p - center);
            float ring = exp(-d * d * 12.0) * sin(phase * 3.14159 * 2.0);
            splash += max(ring, 0.0);
        }
    }
    return clamp(splash, 0.0, 1.0);
}

// ============================================================================
// Procedural Caustics (circular waves + noise perturbation)
// ============================================================================
float causticsPattern(vec2 worldXZ, float t) {
    vec2 p = worldXZ * 0.8;
    float phaseNoise = sin(p.x * 2.3 + p.y * 1.7) * 0.3 + sin(p.x * 1.1 - p.y * 2.9) * 0.2;
    float c = 0.0;
    c += sin(length(p - vec2( 0.3,  0.7)) * 4.0 - t * 1.5 + phaseNoise);
    c += sin(length(p - vec2(-0.5, -0.2)) * 5.2 + t * 1.2 + phaseNoise) * 0.7;
    c += sin(length(p - vec2( 0.8, -0.6)) * 3.7 - t * 0.9 + phaseNoise) * 0.5;
    c += sin(length(p - vec2(-0.2,  0.4)) * 6.1 + t * 1.8 + phaseNoise) * 0.35;
    c += sin(length(p - vec2( 0.6,  0.1)) * 7.5 - t * 2.1 + phaseNoise) * 0.2;
    return pow(max(c * 0.25 + 0.6, 0.0), 3.0);
}

// ============================================================================
// GGX Specular
// ============================================================================
float ggxSpecular(vec3 N, vec3 H, float roughness) {
    float a = max(roughness * roughness, 1e-4);
    float a2 = a * a;
    float NoH = max(dot(N, H), 0.0);
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    d = max(d, 1e-5);
    return a2 / (3.14159265 * d * d);
}

// ============================================================================
// Main
// ============================================================================
// GPU fraca (VulkanContext::shaderLite): sem o reflexo em espaço de tela
// (40 passos + busca binária por pixel), com normal procedural de uma oitava
// e sem espuma de superfície (Worley), respingo de chuva e cáusticas.
layout(constant_id = 0) const bool LITE = false;

// Cor da LAVA (liquidKind 1): toda propria, nao le' nada da agua (reflexo,
// refracao, espuma, causticas). Por isso o main sai logo depois dela.
//
// O que a lava real mostra (filmagens de pahoehoe e aa): crosta preta e
// cinza em placas de tamanhos diferentes, rugosa, que a luz do sol revela;
// a incandescencia aparece nas fendas largas, onde a crosta se abre e nas
// regioes em que ela se quebra em jangadas sobre o liquido, nunca numa rede
// uniforme. A cor vem da temperatura (corpo negro), e a crosta anda.

// Cromaticidade de corpo negro pela lei de Planck em tres comprimentos de
// onda (610, 550 e 465 nm), normalizada pelo vermelho: ~1000 K da' vermelho
// escuro, ~1450 K laranja. O brilho fica com quem chama (HDR), e o tone
// mapping leva o mais quente para o amarelo.
vec3 blackbody(float kelvin) {
    const vec3 lam = vec3(610e-9, 550e-9, 465e-9);
    vec3 e = exp(min(1.4388e-2 / (lam * kelvin), vec3(80.0))) - 1.0;
    vec3 r = 1.0 / (pow(lam / lam.x, vec3(5.0)) * e);
    return r / r.x;
}

// Fenda de uma escala: F2 - F1 do Voronoi (Worley 1996) com largura em
// unidades de F2 - F1 e borda alargada pelo tamanho do pixel (sem serrilhar
// a fenda fina ao longe). x = fenda, y = distancia a' fenda (relevo).
vec2 lavaCrack(vec2 p, float width) {
    vec2 f = lavaCells(p);
    float d = f.y - f.x;
    float aa = fwidth(d);
    return vec2(1.0 - smoothstep(width - aa, width + aa, d), d);
}

// Uma fase da crosta: calor (x) e altura (y) no ponto p (unidades do mundo,
// ja' advectado). `warpBig`/`warpMid`: torcao do dominio em duas escalas
// (sem ela a placa sai poligono de borda reta, o Voronoi cru le' como
// ladrilho); vem pronta e vale para as duas fases. `wn`: largura de cada
// fenda, que varia ao longo dela, de fio a canal (uma largura so' le' como
// rejunte de piso). `open`: 0 crosta inteira, 1 quebrada em jangadas.
// `fineW`: peso da rachadura fina (0 longe da camera).
vec2 lavaCrust(vec2 p, vec2 warpBig, vec2 warpMid, float wn, float open, float calor, float fineW) {
    vec2 pw = p + warpBig;
    vec2 pm = pw + warpMid;
    float wBig = (0.03 + 0.20 * wn * wn) * calor * (1.0 + 4.0 * open);
    vec2 big = lavaCrack(pw * 0.018, wBig);
    vec2 mid = lavaCrack(pm * 0.055 + 9.1, (0.015 + 0.05 * wn) * (1.0 + 2.0 * open));
    float heat = max(big.x, mid.x * 0.6);
    float h = smoothstep(0.0, 0.35, big.y) * 0.7 + smoothstep(0.0, 0.25, mid.y) * 0.3;
    if (fineW > 0.0) {
        // Rachaduras finas dentro da placa: mais frias, quase so' relevo.
        vec2 fine = lavaCrack(pm * 0.16 + 21.3, 0.04 + 0.08 * open);
        heat = max(heat, fine.x * 0.38 * fineW);
        h -= fine.x * 0.25 * fineW;
    }
    // A placa esfria de dentro para fora: a borda dela ainda brilha fraco.
    heat = max(heat, exp(-big.y / max(wBig * 2.5, 1e-3)) * 0.42);
    return vec2(heat, h);
}

vec3 lavaColor(vec3 worldPos, float radial) {
    float t = ubo.waterLevelAndPlanes.w;
    // `emissive` do mapa: 3.0 e' o valor de referencia; abaixo disso a
    // fenda afina e esfria, acima ela alarga e esquenta.
    float calor = ubo.normalAdvanced.y > 0.0 ? ubo.normalAdvanced.y / 3.0 : 1.0;
    float flow = ubo.causticsParams.z;
    vec2 xz = worldPos.xz;

    // CALOR DE BORDA (shoreHeat / centerHeat do .env). `radial`: 0 no
    // centro, 1 na margem.
    float borda = ubo.normalAdvanced.z * smoothstep(0.65, 1.0, radial)
                + ubo.normalAdvanced.w * (1.0 - smoothstep(0.0, 0.35, radial));

    // Onde a crosta se abre: manchas grandes e lentas, mais a borda quente.
    float open = smoothstep(0.7, 0.95, snoise(xz * 0.0045 + vec2(t * 0.002, 0.0)) * 0.5 + 0.5);
    open = clamp(open + 0.12 * borda, 0.0, 1.0);

    // FLUXO: a crosta anda num campo sem divergencia (rotacional de um ruido
    // largo), por duas fases cruzadas: cada fase advecta a crosta por meio
    // periodo e reinicia quando o peso dela e' zero (Vlachos, "Water Flow in
    // Portal 2", SIGGRAPH 2010). Periodo longo: no cruzamento a crosta parece
    // rachar e soldar de novo, como acontece.
    vec2 q = xz * 0.0035;
    float e = 0.35;
    float n0 = snoise(q);
    vec2 vel = vec2(snoise(q + vec2(0.0, e)) - n0, -(snoise(q + vec2(e, 0.0)) - n0)) / e;
    vel *= 2.2 * flow;
    const float period = 16.0;
    float ph0 = fract(t / period), ph1 = fract(t / period + 0.5);
    float w0 = 1.0 - abs(2.0 * ph0 - 1.0);
    // Torcao e largura calculadas uma vez, no ponto parado: a placa se
    // deforma um pouco enquanto anda, o que a lava tambem faz.
    vec2 warpBig = 16.0 * vec2(snoise(xz * 0.017), snoise(xz * 0.017 + 7.3));
    vec2 warpMid = 4.5 * vec2(snoise(xz * 0.07 + 2.1), snoise(xz * 0.07 + 5.9));
    float wn = snoise(xz * 0.011 + 3.7) * 0.5 + 0.5;
    // Rachadura fina so' perto: longe ela vira menos de um pixel.
    float fineW = LITE ? 0.0 : 1.0 - smoothstep(70.0, 140.0, length(ubo.cameraPos.xyz - worldPos));
    vec2 c0 = lavaCrust(xz - vel * (ph0 - 0.5) * period, warpBig, warpMid, wn, open, calor, fineW);
    vec2 c1 = lavaCrust(xz - vel * (ph1 - 0.5) * period + vec2(37.3, 11.9), warpBig, warpMid, wn, open, calor, fineW);
    vec2 c = mix(c1, c0, w0);
    float heat = clamp(c.x * min(calor, 1.6) + 0.06 * borda, 0.0, 1.0);
    // Liquido exposto: quente, com conveccao lenta por baixo.
    // Liquido exposto: conveccao lenta (manchas mais e menos quentes) e as
    // dobras de pele que esfriam por cima (cordas da pahoehoe), arrastadas
    // pelo fluxo.
    vec2 m = xz - vel * t * 0.5;
    float molten = 0.72 + 0.28 * snoise(m * 0.02 + vec2(0.0, t * 0.03));
    float ropes = 1.0 - smoothstep(0.0, 0.14, abs(snoise(m * vec2(0.045, 0.09) + 4.4)));
    molten *= 1.0 - 0.45 * ropes;
    heat = max(heat, open * open * molten * smoothstep(0.15, 0.6, heat + 0.3));

    // Brilho de corpo negro: cresce como uma potencia alta da temperatura,
    // para so' o que esta' quente passar do limiar do bloom.
    float kelvin = mix(850.0, 1480.0, heat);
    vec3 glow = blackbody(kelvin) * (3.0 * pow(heat, 3.5) * max(calor, 0.3));

    // CROSTA: basalto escuro e rugoso, iluminado pelo sol e pelo ambiente.
    // Normal pelas derivadas de tela da altura (Mikkelsen, "Bump Mapping
    // Unparametrized Surfaces on the GPU", 2010), sobre o plano da lava.
    float rough = snoise(xz * 0.5) * 0.1;
    float h = c.y * 1.4 + rough;
    vec2 dpx = dFdx(xz), dpy = dFdy(xz);
    float hx = dFdx(h), hy = dFdy(h);
    float det = dpx.x * dpy.y - dpx.y * dpy.x;
    vec2 grad = abs(det) > 1e-8 ? vec2(hx * dpy.y - hy * dpx.y, hy * dpx.x - hx * dpy.x) / det : vec2(0.0);
    grad = clamp(grad, vec2(-3.0), vec2(3.0));
    vec3 n = normalize(vec3(-grad.x, 1.0, -grad.y));
    vec3 l = normalize(-ubo.sunDirIntensity.xyz);
    vec3 v = normalize(ubo.cameraPos.xyz - worldPos);
    float sunI = ubo.sunDirIntensity.w * ubo.weatherParams.x;
    // Basalto: cinza quente, mais claro no topo da placa, com granulacao
    // fina (vesiculas) que quebra a superficie lisa de ardosia.
    float grain = snoise(xz * 2.3) * 0.5 + 0.5;
    float tone = (0.022 + 0.04 * smoothstep(-0.4, 0.8, snoise(xz * 0.03 + 5.0) + c.y * 0.6)) * (0.65 + 0.7 * grain);
    vec3 albedo = tone * vec3(1.0, 0.9, 0.82);
    // Ambiente sem a cor dele: a caverna roxa pintava a rocha preta de lilas.
    float amb = dot(ubo.ambientColor.rgb, vec3(0.2126, 0.7152, 0.0722)) * ubo.ambientColor.w;
    vec3 lit = ubo.sunColor.rgb * sunI * max(dot(n, l), 0.0) + vec3(amb);
    float spec = ggxSpecular(n, normalize(l + v), 0.55) * 0.04 * max(dot(n, l), 0.0);
    vec3 crust = albedo * lit + ubo.sunColor.rgb * sunI * spec;
    // A crosta perto da fenda e' iluminada por ela (luz vermelha de baixo).
    crust += blackbody(1100.0) * albedo * 3.0 * smoothstep(0.1, 0.6, heat);
    float molt = smoothstep(0.35, 0.75, heat);
    return mix(crust, vec3(0.0), molt * 0.85) + glow;
}

void main() {
    // LAVA SAI CEDO. A cor dela sobrescrevia tudo no fim, mas antes o pixel
    // pagava o shader de agua inteiro (ondas, reflexo em espaco de tela de 40
    // passos, refracao, espuma, causticas): o passe Water custava 3,1 ms no
    // lago de lava de um andar de caverna (4060, 1080p) para uma imagem que
    // nao usava nada daquilo.
    if (int(ubo.extraParams.w + 0.5) == 1) {
        fragColor = vec4(pc.debugMask.y != 0 ? vec3(0.0, 1.0, 0.0) : lavaColor(vWorldPos, vTexCoord.x), 1.0);
        return;
    }
    float time          = ubo.waterLevelAndPlanes.w;
    float nearPlane     = ubo.waterLevelAndPlanes.y;
    float farPlane      = ubo.waterLevelAndPlanes.z;
    float waterLevel    = vLevel; // o desta agua (ubo.waterLevelAndPlanes.x e' o do mapa aberto)
    vec2  viewport      = ubo.screenSize.xy;

    vec3  baseDeep      = ubo.baseColorDeep.rgb;
    vec3  baseShallow   = ubo.baseColorShallow.rgb;
    float transparency  = clamp(ubo.waterParams.x, 0.0, 1.0);
    float refrStrength  = ubo.waterParams.y;
    float reflectivity  = clamp(ubo.waterParams.z, 0.0, 1.0);
    float roughness     = clamp(ubo.waterParams.w, 0.0, 1.0);

    float normalScale   = ubo.normalParams.x;
    float waveSpeed     = ubo.waveParams.z;
    float normalStr     = clamp(ubo.normalParams.z, 0.0, 2.0);
    int   normalOctaves = clamp(int(ubo.normalAdvanced.x + 0.5), 1, 4);

    float foamEdgeDepth = ubo.foamParams.x;
    float contactStr    = ubo.foamParams.y;
    bool  enableFoam    = ubo.foamParams.z > 0.5;
    float rainSplashFoam = ubo.foamParams.w;

    float foamScale     = ubo.foamSurfaceParams.x;
    float foamSpeed     = ubo.foamSurfaceParams.y;
    float foamRoughness = clamp(ubo.foamSurfaceParams.z, 0.0, 1.0);
    bool  enableSurfaceFoam = ubo.foamSurfaceParams.w > 0.5;

    int screenTexSlot   = ubo.textureSlots.x;
    int depthTexSlot    = ubo.textureSlots.y;
    int foamMaskSlot    = ubo.textureSlots.z;
    int waterTexSlot    = ubo.textureSlots.w;
    int skyboxTexSlot   = ubo.skyboxSlot.x;

    float absorptionCoeff = ubo.extraParams.x;
    float maxPxScale      = ubo.extraParams.y;
    bool  enableCaustics  = ubo.extraParams.z > 0.5;
    float causticsIntensity = ubo.causticsParams.x;
    float causticsDepthAtt  = ubo.causticsParams.y;

    // ---- Perturbed normal (multi-octave Simplex or texture) ----
    vec3 perturbedNormal;
    if (waterTexSlot >= 0) {
        vec2 texScroll = vec2(time * waveSpeed * 0.1, time * waveSpeed * 0.07);
        vec2 uv = vWorldPos.xz * normalScale + texScroll;
        float h  = texture(u_textures[nonuniformEXT(waterTexSlot)], uv).r;
        float hx = texture(u_textures[nonuniformEXT(waterTexSlot)], uv + vec2(0.01, 0.0)).r;
        float hy = texture(u_textures[nonuniformEXT(waterTexSlot)], uv + vec2(0.0, 0.01)).r;
        float derivScale = 8.0;
        vec3 localN = normalize(vec3((h - hx) * derivScale, 1.0, (h - hy) * derivScale));
        vec3 pn = localN.x * vTangent + localN.y * vNormal + localN.z * vBitangent;
        float pnLen = length(pn);
        perturbedNormal = (pnLen > 1e-5) ? (pn / pnLen) : vNormal;
        perturbedNormal.y = max(perturbedNormal.y, 0.01);
        perturbedNormal = normalize(perturbedNormal);
        vec3 ps = vec3(perturbedNormal.x * normalStr, perturbedNormal.y, perturbedNormal.z * normalStr);
        ps.y = max(ps.y, 0.01);
        float psLen = length(ps);
        perturbedNormal = (psLen > 1e-5) ? (ps / psLen) : vNormal;
    } else {
        vec3 localN = sampleProceduralNormal(vWorldPos.xz, normalScale, waveSpeed, time, normalStr, normalOctaves);
        vec3 pn = localN.x * vTangent + localN.y * vNormal + localN.z * vBitangent;
        float pnLen = length(pn);
        perturbedNormal = (pnLen > 1e-5) ? (pn / pnLen) : vNormal;
    }
    if (!validVec3(perturbedNormal)) perturbedNormal = vNormal;

    // ---- View & Light ----
    vec3 viewDir  = normalize(ubo.cameraPos.xyz - vWorldPos);
    vec3 lightDir = normalize(-ubo.sunDirIntensity.xyz);
    vec3 halfDir  = normalize(lightDir + viewDir);
    // Weather influence on lighting
    float sunOcc = ubo.weatherParams.x;
    float moonOcc = ubo.weatherParams.y;
    float stormDarken = ubo.weatherParams.z;
    float rainIntensity = ubo.weatherParams.w;
    float weatherDarken = clamp(stormDarken + rainIntensity * 0.5, 0.0, 1.0);

    float sunIntensity = ubo.sunDirIntensity.w * sunOcc * (1.0 - stormDarken * 0.6);

    // ---- Fresnel (Schlick) IOR 1.33 -> F0 = 0.02 ----
    float F0 = 0.02;
    float cosTheta = clamp(dot(viewDir, perturbedNormal), 0.0, 1.0);
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);

    // Fresnel com F0 artistico: o fisico (0,02) some na camera de diorama, que
    // olha a agua de cima, e a agua parecia sem reflexo nenhum. reflectivity
    // leva o F0 de 0,02 (fisico) a 0,25; o raso-rasante continua indo a 1.
    float F0art = mix(F0, 0.25, reflectivity);
    float reflAmount = F0art + (1.0 - F0art) * pow(1.0 - cosTheta, 5.0);
    reflAmount *= step(0.001, reflectivity);
    // Rain flattens and dims reflections; storm nearly kills specular mirror
    reflAmount *= (1.0 - rainIntensity * 0.5) * (1.0 - stormDarken * 0.7);

    // ---- Screen-space background ----
    vec2 ssr = getScreenUV();

    // Refraction distortion based on wave normal
    vec2 refrOff = perturbedNormal.xz * refrStrength * 0.3;
    vec2 pxOff = refrOff * viewport * 0.5;
    float maxPx = maxPxScale;
    if (length(pxOff) > maxPx) refrOff *= maxPx / length(pxOff);

    vec2 rUV = clamp(ssr + refrOff * vec2(1.0, 0.7), vec2(0.001), vec2(0.999));
    vec2 gUV = clamp(ssr + refrOff * vec2(0.7, 1.0), vec2(0.001), vec2(0.999));
    vec2 bUV = clamp(ssr + refrOff * vec2(1.0, 1.0), vec2(0.001), vec2(0.999));

    float bgR = texture(u_textures[nonuniformEXT(screenTexSlot)], rUV).r;
    float bgG = texture(u_textures[nonuniformEXT(screenTexSlot)], gUV).g;
    float bgB = texture(u_textures[nonuniformEXT(screenTexSlot)], bUV).b;
    vec3 background = vec3(bgR, bgG, bgB);

    // ---- Depth-based coloring using VERTICAL thickness (corrected) ----
    float sceneDepth = texture(u_textures[nonuniformEXT(depthTexSlot)], ssr).r;
    vec3 floorWorldPos = reconstructWorldPos(ssr, sceneDepth);
    float thickness = max(0.0, waterLevel - floorWorldPos.y);
    float depthFactor = clamp(thickness / 8.0, 0.0, 1.0);

    vec3 waterColor = mix(baseShallow, baseDeep, depthFactor);

    // Subtle sky ambient influence
    vec3 skyAmbient = mix(ubo.skyHorizon.rgb, ubo.skyTop.rgb, 0.3);
    waterColor = mix(waterColor, skyAmbient, 0.15);

    // ---- Beer-Lambert absorption on refracted background ----
    float absorb = exp(-thickness * absorptionCoeff);
    background = mix(baseDeep, background, absorb);

    // ---- Reflection (screen-space objects + sky texture + gradient fallback) ----
    vec3 reflectDir = reflect(-viewDir, perturbedNormal);

    // Reflexo de tela dos objetos sobre a agua. O raio sai de uma normal
    // suavizada (a das ondas, ruidosa, espalhava os acertos em pontinhos),
    // anda com passo que cresce com a distancia (fino perto do casco, largo
    // longe) e, ao cruzar a cena, refina por busca binaria. O acerto compara
    // distancias a' camera em unidades de mundo.
    vec3 ssrNormal = normalize(mix(vNormal, perturbedNormal, 0.25));
    vec3 ssrDir = reflect(-viewDir, ssrNormal);
    vec3 ssrColor = vec3(0.0);
    float ssrHit = 0.0;
    if (ssrDir.y > 0.0) {
        const int ssrSteps = 40;
        float t = 0.6, dt = 0.8, tPrev = 0.0;
        for (int i = 0; i < ssrSteps; i++) {
            vec3 rayPos = vWorldPos + ssrDir * t;
            vec4 clip = pc.viewProj * vec4(rayPos, 1.0);
            if (clip.w <= 0.0) break;
            vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float sd = texture(u_textures[nonuniformEXT(depthTexSlot)], uv).r;
            if (sd < 1.0) {
                float rayDist = length(rayPos - ubo.cameraPos.xyz);
                float sceneDist = length(reconstructWorldPos(uv, sd) - ubo.cameraPos.xyz);
                if (rayDist > sceneDist) {
                    float lo = tPrev, hi = t;
                    for (int k = 0; k < 6; k++) {
                        float mid = 0.5 * (lo + hi);
                        vec3 mp = vWorldPos + ssrDir * mid;
                        vec4 mc = pc.viewProj * vec4(mp, 1.0);
                        vec2 muv = mc.xy / mc.w * 0.5 + 0.5;
                        float md = texture(u_textures[nonuniformEXT(depthTexSlot)], muv).r;
                        float sdist = length(reconstructWorldPos(muv, md) - ubo.cameraPos.xyz);
                        if (length(mp - ubo.cameraPos.xyz) > sdist) hi = mid; else lo = mid;
                    }
                    vec3 hp = vWorldPos + ssrDir * hi;
                    vec4 hc = pc.viewProj * vec4(hp, 1.0);
                    vec2 huv = hc.xy / hc.w * 0.5 + 0.5;
                    float hd = texture(u_textures[nonuniformEXT(depthTexSlot)], huv).r;
                    float gap = length(hp - ubo.cameraPos.xyz) - length(reconstructWorldPos(huv, hd) - ubo.cameraPos.xyz);
                    if (gap < max(dt * 2.0, 3.0)) {
                        ssrColor = texture(u_textures[nonuniformEXT(screenTexSlot)], huv).rgb;
                        vec2 edge = smoothstep(vec2(0.0), vec2(0.08), huv) * smoothstep(vec2(0.0), vec2(0.08), 1.0 - huv);
                        ssrHit = edge.x * edge.y * (1.0 - smoothstep(0.6, 1.0, float(i) / float(ssrSteps)));
                    }
                    break;
                }
            }
            tPrev = t;
            t += dt;
            dt *= 1.12;
        }
    }

    float skyT = clamp(reflectDir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 skyGradient = mix(ubo.skyHorizon.rgb, ubo.skyTop.rgb, skyT);

    float storm = stormDarken;
    // Storm darkens and desaturates sky reflection
    skyGradient = mix(skyGradient, vec3(0.04, 0.04, 0.05), storm * 0.75);
    // Rain further reduces reflection color so water doesn't look foamy bright
    skyGradient = mix(skyGradient, vec3(0.06, 0.07, 0.08) * moonOcc, rainIntensity * 0.3);

    vec3 reflectionColor = skyGradient;
    if (skyboxTexSlot >= 0) {
        vec4 skyClip = pc.viewProj * vec4(reflectDir, 0.0);
        vec2 skyUV = skyClip.xy / skyClip.w * 0.5 + 0.5;
        if (all(greaterThanEqual(skyUV, vec2(0.0))) && all(lessThanEqual(skyUV, vec2(1.0)))) {
            vec3 skyTexColor = texture(u_textures[nonuniformEXT(skyboxTexSlot)], skyUV).rgb;
            // Rough surfaces blur reflection back to gradient
            reflectionColor = mix(skyTexColor, skyGradient, roughness);
        }
    }

    // Blend SSR over sky reflection so objects are visible on the water surface.
    reflectionColor = mix(reflectionColor, ssrColor, ssrHit * (1.0 - roughness * 0.7));

    // ---- Unified Foam System ----
    float totalFoam = 0.0;
    // A) Surface foam (Worley) — only on wave crests
    if (enableSurfaceFoam) {
        float surfaceFoam = foamSurface(vWorldPos.xz, foamScale, foamSpeed, time);
        float crest = waveCrestMask(perturbedNormal, 0.75);
        totalFoam = max(totalFoam, surfaceFoam * crest);
    }
    // B) Edge foam (depth-based)
    if (enableFoam) {
        float edgeFoam = 1.0 - smoothstep(0.0, foamEdgeDepth * 1.5, thickness);
        totalFoam = max(totalFoam, edgeFoam);
    }
    // C) Contact foam (CPU mask)
    if (enableFoam) {
        vec2 foamUV = (vWorldPos.xz / vec2(128.0, 128.0)) + 0.5;
        float contactFoam = texture(u_textures[nonuniformEXT(foamMaskSlot)], foamUV).r * contactStr;
        totalFoam = max(totalFoam, contactFoam);
    }
    // D) Rain splash contact foam
    if (rainIntensity > 0.001 && rainSplashFoam > 0.001) {
        float splash = rainSplashPattern(vWorldPos.xz, time * 3.0) * rainIntensity * rainSplashFoam;
        totalFoam = max(totalFoam, splash);
    }
    totalFoam = clamp(totalFoam, 0.0, 1.0);

    vec3 foamColor = vec3(0.95, 0.97, 0.98);

    // ---- Roughness varies by foam (foam is rougher than water) ----
    float finalRoughness = mix(roughness, foamRoughness, totalFoam);

    // ---- Sun specular (GGX + glitter) with variable roughness ----
    // Storm reduces and broadens highlights, making water look flatter/darker
    float specular = ggxSpecular(perturbedNormal, halfDir, finalRoughness);
    vec3 sunSpecular = ubo.sunColor.rgb * specular * sunIntensity * 2.0 * (1.0 - storm * 0.8) * (1.0 - rainIntensity * 0.6);

    float sunReflectDot = clamp(dot(reflectDir, lightDir), 0.0, 1.0);
    float sunGlitter = pow(sunReflectDot, mix(2048.0, 64.0, finalRoughness));
    sunSpecular += ubo.sunColor.rgb * sunGlitter * sunIntensity * 0.5 * (1.0 - storm * 0.9) * (1.0 - rainIntensity * 0.7);
    sunSpecular = min(sunSpecular, vec3(2.0)); // clamp to prevent blowout

    // ---- Caustics (stronger in shallow water, animated) ----
    float caustics = 0.0;
    vec3 causticsColor = vec3(0.0);
    if (enableCaustics) {
        caustics = causticsPattern(vWorldPos.xz, time) * exp(-thickness * causticsDepthAtt) * sunIntensity * (1.0 - storm * 0.7);
        causticsColor = ubo.sunColor.rgb * caustics * causticsIntensity;
    }

    // ---- Ambient (brightened so opaque water is not black) ----
    vec3 ambient = ubo.ambientColor.rgb * ubo.ambientColor.w;
    ambient = max(ambient, vec3(0.15));
    // Under heavy rain/storm, ambient is dampened so water doesn't glow
    ambient *= (1.0 - weatherDarken * 0.35);
    vec3 litWater = waterColor * (ambient + 0.2);

    // ---- Composition ----
    vec3 color = mix(litWater, background, transparency);
    color = mix(color, reflectionColor, clamp(reflAmount, 0.0, 1.0));
    color += sunSpecular;
    color += causticsColor;
    color = mix(color, foamColor, totalFoam);

    // NaN/Inf final guard
    if (any(isnan(color)) || any(isinf(color))) {
        color = vec3(0.05, 0.1, 0.15);
    }

    // Diagnostico de cobertura do disco (ERUPTION_LIQUID_DEBUG=1): pinta a
    // superficie de CIANO chapado. Ciano e nao magenta de proposito - magenta
    // e' chroma-key de descarte no model.frag e o pixel sumiria da tela em
    // vez de marcar. Serve pra medir num print de cima quanto da caldeira o
    // disco de fato cobre.
    if (pc.debugMask.y != 0) {
        // VERDE puro, nao ciano: o caso de teste do autor pinta a textura
        // de lava do TERRENO (a caldeira) de ciano ao mesmo tempo, pra medir
        // na MESMA foto quanto o disco liquido cobre da caldeira. Com as
        // duas na mesma cor as duas manchas se fundem e a comparacao fica
        // impossivel de ler a olho ou de segmentar por cor.
        color = vec3(0.0, 1.0, 0.0);
    }

    fragColor = vec4(color, 1.0);
}
