#version 450
#extension GL_GOOGLE_include_directive : require
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

#define ERUPTION_SLOT_DIRECT // refração, profundidade e espuma são render targets
#include "common/slot_textures.glsl"
#include "common/shore_waves.glsl"

layout(set = 1, binding = 0) uniform WaterUBO {
    vec4 baseColorDeep;
    vec4 baseColorShallow;
    vec4 waterParams;         // x=transparency, y=refractionStrength, z=reflectivity, w=roughness
    vec4 normalParams;        // x=scale, y=waveSpeed, z=strength, w=lamina da praia (1/0)
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
    vec4 waveSpeedSteep[3];   // x=speed, y=steepness; zw = nuvens do ceu: [0] cobertura, escala; [1] velocidade, maciez; [2] tempo do ceu
    ivec4 textureSlots;       // x=screenTex, y=depthTex, z=foamMask, w=waterTex
    ivec4 skyboxSlot;         // x=skyboxTex, y=forca das nuvens no reflexo (milesimos), z=nuvens do ceu ligadas, w=unused
    mat4 invViewProj;
    vec4 extraParams;         // x=absorptionCoeff, y=maxPxScale, z=enableCaustics, w=unused
    vec4 foamSurfaceParams;   // x=foamScale, y=foamSpeed, z=foamRoughness, w=enableSurfaceFoam
    vec4 normalAdvanced;      // x=normalOctaves, y=lavaEmissive, zw=unused
    vec4 causticsParams;      // x=intensity, y=depthAttenuation, z=lava flowSpeed, w=lava crustWiden
    vec4 weatherParams;       // x=sunOcclusion, y=moonOcclusion, z=stormDarken, w=rainIntensity
    vec4 lavaCrust;           // xyz=tinta da crosta, w=deslocamento da abertura (crustCoverage)
    vec4 styleParams;         // x=diorama (0/1), y=forca do reflexo, z=brilhos, w=aneis de chuva
    vec4 styleParams2;        // x=espuma de margem, y=nivel do SSPR, z=distorcao do reflexo, w=clareza
    vec4 styleDeep;           // xyz=cor funda, w=inclinacao das ondas
    ivec4 styleSlots;         // x=slot do reflexo planar (-1 = sem), y=slot do reflexo espelhado (-1 = sem)
    vec4 stepRipples[16];     // ondinhas de passo: xy=ponto no mundo (x,z), z=instante do nascimento (s), w=forca (0 = vazio)
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
// Reflexo por marcha de raio em espaco de tela (rgb = cor, a = acerto)
// ============================================================================
// O raio anda com passo que cresce com a distancia (fino perto do casco,
// largo longe) e, ao cruzar a cena, refina por busca binaria. O acerto
// compara distancias a' camera em unidades de mundo.
vec4 marchReflection(vec3 origin, vec3 ssrDir, int depthTexSlot, int screenTexSlot) {
    vec3 ssrColor = vec3(0.0);
    float ssrHit = 0.0;
    if (ssrDir.y > 0.0) {
        const int ssrSteps = 40;
        float t = 0.6, dt = 0.8, tPrev = 0.0;
        for (int i = 0; i < ssrSteps; i++) {
            vec3 rayPos = origin + ssrDir * t;
            vec4 clip = pc.viewProj * vec4(rayPos, 1.0);
            if (clip.w <= 0.0) break;
            vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float sd = slotTexture(depthTexSlot, uv).r;
            if (sd < 1.0) {
                float rayDist = length(rayPos - ubo.cameraPos.xyz);
                float sceneDist = length(reconstructWorldPos(uv, sd) - ubo.cameraPos.xyz);
                if (rayDist > sceneDist) {
                    float lo = tPrev, hi = t;
                    for (int k = 0; k < 6; k++) {
                        float mid = 0.5 * (lo + hi);
                        vec3 mp = origin + ssrDir * mid;
                        vec4 mc = pc.viewProj * vec4(mp, 1.0);
                        vec2 muv = mc.xy / mc.w * 0.5 + 0.5;
                        float md = slotTexture(depthTexSlot, muv).r;
                        float sdist = length(reconstructWorldPos(muv, md) - ubo.cameraPos.xyz);
                        if (length(mp - ubo.cameraPos.xyz) > sdist) hi = mid; else lo = mid;
                    }
                    vec3 hp = origin + ssrDir * hi;
                    vec4 hc = pc.viewProj * vec4(hp, 1.0);
                    vec2 huv = hc.xy / hc.w * 0.5 + 0.5;
                    float hd = slotTexture(depthTexSlot, huv).r;
                    float gap = length(hp - ubo.cameraPos.xyz) - length(reconstructWorldPos(huv, hd) - ubo.cameraPos.xyz);
                    if (gap < max(dt * 2.0, 3.0)) {
                        ssrColor = slotTexture(screenTexSlot, huv).rgb;
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
    return vec4(ssrColor, ssrHit);
}

// Ceu refletido: a imagem do ceu desta camera (ceu-para-textura) onde a
// direcao cai dentro dela, senao o degrade do ciclo do dia.
// NUVENS DO CEU NO REFLEXO, calculadas aqui com o mesmo ruido e os mesmos
// parametros da cupula (skybox.frag, bloco Clouds; o clima muda cobertura e
// velocidade em Render.cpp e o WaterRenderer repassa). A cupula apaga as
// nuvens perto do horizonte (horizonFade, ate' ~17 graus com "cloudy"), e com
// a camera baixa a agua reflete justamente essa faixa: nenhuma nuvem chegava
// ao reflexo. A textura do ceu tambem so' cobre o que a camera ve. Aqui as
// nuvens sao uma camada plana a 300 u: cada ponto da agua reflete o ponto da
// camada onde o raio refletido a cruza (perspectiva certa). Pela direcao
// apenas, com a camera baixa o lago inteiro via o mesmo pedaco de ceu e o
// efeito virava um clareado uniforme. x = mascara da nuvem, y = sombreado
// (1 no topo iluminado, 0,25 na base).
float nuvemHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float nuvemRuido(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = nuvemHash(i);
    float b = nuvemHash(i + vec2(1.0, 0.0));
    float c = nuvemHash(i + vec2(0.0, 1.0));
    float d = nuvemHash(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}
float nuvemFbm(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 4; i++) { v += amp * nuvemRuido(p); p *= 2.0; amp *= 0.5; }
    return v;
}
vec2 skyCloudsReflected(vec3 R, vec3 wp) {
    if (ubo.skyboxSlot.z == 0) return vec2(0.0);
    float cobertura = ubo.waveSpeedSteep[0].z, escala = ubo.waveSpeedSteep[0].w;
    float veloc = ubo.waveSpeedSteep[1].z, macio = ubo.waveSpeedSteep[1].w;
    float t = ubo.waveSpeedSteep[2].z;
    const float kAltura = 300.0;
    vec2 xz = wp.xz + R.xz * (kAltura / max(R.y, 0.08));
    vec2 uv = xz * 0.004 + vec2(t * veloc, t * veloc * 0.25);
    float n = nuvemFbm(uv * escala);
    float limiar = mix(0.62, 0.40, cobertura);
    float largura = mix(0.02, 0.12, macio);
    float mascara = pow(smoothstep(limiar - largura, limiar + largura, n), 0.7);
    float sombra = 1.0 - smoothstep(limiar - largura, limiar + largura + 0.18, n) * 0.75;
    return vec2(mascara, sombra);
}

vec3 skyReflection(vec3 reflectDir, float roughness, float storm, float rainIntensity, float moonOcc, int skyboxTexSlot) {
    float skyT = clamp(reflectDir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 skyGradient = mix(ubo.skyHorizon.rgb, ubo.skyTop.rgb, skyT);
    // Storm darkens and desaturates sky reflection
    skyGradient = mix(skyGradient, vec3(0.04, 0.04, 0.05), storm * 0.75);
    // Rain further reduces reflection color so water doesn't look foamy bright
    skyGradient = mix(skyGradient, vec3(0.06, 0.07, 0.08) * moonOcc, rainIntensity * 0.3);

    vec3 reflectionColor = skyGradient;
    if (skyboxTexSlot >= 0) {
        // A textura do ceu e' renderizada pela camera espelhada (y -> -y):
        // o reflexo para cima cai na tela dela (ProceduralSkyBackend::renderToTexture).
        vec4 skyClip = pc.viewProj * vec4(reflectDir.x, -reflectDir.y, reflectDir.z, 0.0);
        vec2 skyUV = skyClip.xy / skyClip.w * 0.5 + 0.5;
        if (skyClip.w > 0.0 && all(greaterThanEqual(skyUV, vec2(0.0))) && all(lessThanEqual(skyUV, vec2(1.0)))) {
            vec3 skyTexColor = slotTexture(skyboxTexSlot, skyUV).rgb;
            // Rough surfaces blur reflection back to gradient
            reflectionColor = mix(skyTexColor, skyGradient, roughness);
        }
    }
    return reflectionColor;
}

// ============================================================================
// AGUA DE DIORAMA (preset "water_hd2d"): so' funcoes analiticas, sem textura
// ============================================================================
float dioHash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec3 dioHash32(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}

// Ondas: 8 trens de onda em leque em volta do vento (waveDir0), cada um
// exp(sin(fase) - 1) - crista estreita e cavado largo, como a agua real - com
// a derivada exata (nada de diferenca finita de ruido, que era 3x o ruido
// por pixel e chiava). Cada trem some quando o comprimento de onda fica
// abaixo de ~4 pixels: o que sumiu da inclinacao volta como rugosidade (z),
// entao ao longe a agua fica lisa e o brilho do sol so' alarga, em vez de
// virar chuvisco de pontos. Retorna (dh/dx, dh/dz, variancia perdida).
// Direcao agua -> terra mais perto no ponto (grade do Engine::rebuildShoreMask,
// slot em skyboxSlot.w, origem em skyTop.w/skyHorizon.w, 1/lado em
// screenSize.w), ou zero.
vec2 shoreFlow(vec2 p) {
    if (ubo.skyboxSlot.w < 0 || ubo.screenSize.w <= 0.0) return vec2(0.0);
    vec2 uv = (p - vec2(ubo.skyTop.w, ubo.skyHorizon.w)) * ubo.screenSize.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec2(0.0);
    return slotTextureLod(ubo.skyboxSlot.w, uv, 0.0).rg;
}

// tTrem: o tempo que faz os trens andarem pelo vento. No flow map (shoreFlow)
// vai 0 - o desenho fica parado e quem anda e' o deslocamento para a terra.
vec3 dioramaWaves(vec2 p, float t, float footprint, float tTrem) {
    float scale = max(ubo.normalParams.x, 1e-3);
    float slope = ubo.normalParams.z * ubo.styleDeep.w * 2.6;
    float speed = ubo.waveParams.z;
    vec2 wind = ubo.waveDirAmp[0].xy;
    float baseAng = dot(wind, wind) > 1e-6 ? atan(wind.y, wind.x) : 0.0;
    vec2 g = vec2(0.0);
    float lost = 0.0;
    // Torcao lenta do dominio (meio comprimento da onda maior): sem ela os
    // 8 trens se cruzam numa grade regular, cara de metal martelado. A
    // derivada da torcao fica de fora (ela e' 20x mais larga que as ondas).
    float wl = 1.0 / (scale * 0.33);
    p += wl * 0.5 * vec2(snoise(p * scale * 0.06 + vec2(t * 0.02, 0.0)),
                         snoise(p * scale * 0.06 + vec2(5.2, 1.3 - t * 0.02)));
    float k = 6.2831853 * scale * 0.33;
    float amp = 0.42;
    for (int i = 0; i < 8; ++i) {
        float fi = float(i);
        float ang = baseAng + (fract(fi * 0.618034) - 0.5) * 2.6;
        vec2 d = vec2(cos(ang), sin(ang));
        float omega = speed * sqrt(9.8 * k) * 0.9; // dispersao de agua funda
        float ph = dot(d, p) * k - omega * tTrem + fi * 1.7;
        float dh = exp(sin(ph) - 1.0) * cos(ph);
        float s = slope * amp;
#ifdef ERUPTION_SLOT_WEB
        // Navegador: sem TAA. Uma onda so' entra com >= ~9 px por ciclo (o desktop aceita 4 px, e o TAA
        // do desktop apaga o resto); o que some vira rugosidade (lost), como no desktop.
        float keep = smoothstep(3.5, 9.0, 6.2831853 / (k * footprint));
#else
        float keep = smoothstep(1.5, 4.0, 6.2831853 / (k * footprint));
#endif
        g += d * (dh * s * keep);
        lost += 0.25 * s * s * (1.0 - keep);
        k *= 1.42;
        amp *= 0.78;
    }
    return vec3(g, lost);
}

// Aneis de chuva: grade de celulas em 3 camadas; cada celula tem uma gota por
// ciclo com chance = intensidade da chuva, num ponto novo a cada ciclo. O
// anel e' um pacote de onda que abre e some; entra na NORMAL (o anel aparece
// pelo reflexo e pelo brilho, como na agua real), mais um ponto de respingo
// no primeiro instante. Com ceu encoberto o reflexo e' quase liso e a
// normal sozinha nao aparece: a crista do anel tambem acende um pouco (w),
// o fio claro que o diorama desenha. Retorna (dh/dx, dh/dz, respingo, crista).
vec4 rainRipples(vec2 p, float t, float footprint, float intensity) {
    vec4 acc = vec4(0.0);
    float cell = 4.0 / max(ubo.normalParams.x, 1e-3);
    for (int layer = 0; layer < 3; ++layer) {
        float fl = float(layer);
        float c = cell * (1.0 + 0.31 * fl);
        vec2 q = p / c + vec2(0.37, 0.71) * fl * 3.1;
        vec2 id = floor(q);
        vec2 f = fract(q);
        vec3 h = dioHash32(id + fl * 19.7);
        float tt = t / (0.9 + 0.5 * h.z) + h.x * 7.0;
        float cyc = floor(tt);
        if (dioHash12(id * 1.37 + cyc * 0.91 + fl * 3.3) > intensity) continue;
        float life = fract(tt);
        vec2 center = 0.3 + 0.4 * dioHash32(id + cyc * 3.17 + fl).xy;
        vec2 v = (f - center) * c;
        float r = length(v);
        float w = c * 0.045;
        float x = r - life * c * 0.3;
        float lam = w * 3.0;
        float fade = (1.0 - life) * (1.0 - life);
        float env = exp(-(x * x) / (w * w * 2.5)) * fade;
        float keep = smoothstep(1.5, 4.0, lam / footprint);
        float wave = sin(x * 6.2831853 / lam);
        acc.xy += (v / max(r, 1e-4)) * (wave * env * keep);
        acc.z += (1.0 - smoothstep(0.0, w * 1.6, r)) * (1.0 - smoothstep(0.0, 0.15, life));
        acc.w += max(cos(x * 6.2831853 / lam), 0.0) * env * keep;
    }
    return acc;
}

// Ondinhas de passo: cada passo na agua (fila de 16 fontes no UBO) abre um
// anel que cresce, amortece e some em ~1,3 s, com um segundo anel mais fraco
// logo atras. Entra na normal como os aneis de chuva (que seguem aleatorios);
// o teste de distancia/idade sai cedo para as fontes vazias, velhas ou longe
// do pixel. Retorna (dh/dx, dh/dz, 0, crista).
const float kPassoVida = 1.3;   // s
const float kPassoRaio = 3.5;   // u, raio final do anel
vec4 stepRipples(vec2 p, float t, float footprint) {
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 16; ++i) {
        vec4 s = ubo.stepRipples[i];
        float age = t - s.z;
        if (s.w <= 0.0 || age < 0.0 || age > kPassoVida) continue;
        vec2 v = p - s.xy;
        float d2 = dot(v, v);
        float rmax = kPassoRaio + 1.0;
        if (d2 > rmax * rmax) continue;
        float life = age / kPassoVida;
        float r = kPassoRaio * (1.0 - (1.0 - life) * (1.0 - life)); // abre rapido, depois desacelera
        float d = sqrt(d2);
        float lam = 0.55;
        float w = 0.28;
        float x = d - r;
        float x2 = d - r * 0.6; // segundo anel, mais fraco, atras do primeiro
        float env = exp(-(x * x) / (w * w * 2.0)) + 0.4 * exp(-(x2 * x2) / (w * w * 2.0));
        float fade = (1.0 - life) * (1.0 - life) * smoothstep(0.0, 0.06, life);
        env *= fade * s.w / sqrt(1.0 + 2.0 * d);
        float keep = smoothstep(1.5, 4.0, lam / footprint);
        float wave = sin(x * 6.2831853 / lam);
        acc.xy += (v / max(d, 1e-4)) * (wave * env * keep);
        acc.w += max(cos(x * 6.2831853 / lam), 0.0) * env * keep;
    }
    return acc;
}

// Brilhos do sol: cada celula tem uma faceta de inclinacao sorteada (troca
// num ciclo proprio); acende onde a faceta manda o sol para a camera. O
// ponto tem no minimo ~1,2 pixel (com a energia dividida), para nao virar
// chuvisco de 1 pixel que pisca entre quadros.
float sunSparkle(vec2 p, vec3 n, vec3 V, vec3 L, float t, float footprint) {
#ifdef ERUPTION_SLOT_WEB
    // Navegador: o brilho e' do tempo, nao da camera. A faceta do desktop so' acende quando o espelho exato do sol
    // cai no pixel (um cone de ~0,1 rad): sem TAA isso vira ponto que liga e desliga a cada passo da camera. Aqui
    // cada celula do MUNDO pisca no proprio ritmo (a fase vem do tempo) e a camera so' escolhe, de forma suave e
    // larga, em que parte da agua o rastro do sol aparece (normal suavizada, sem a onda fina).
    vec3 Rs = reflect(-V, normalize(mix(vec3(0.0, 1.0, 0.0), n, 0.3)));
    float path = smoothstep(0.55, 0.95, dot(Rs, L));
    float sum = 0.0;
    // Duas grades giradas uma contra a outra e com posicao sorteada na celula: uma so' dava um tabuleiro de pontos.
    for (int layer = 0; layer < 2; ++layer) {
        float cellW = (layer == 0 ? 0.55 : 0.43) / max(ubo.normalParams.x, 1e-3);
        vec2 pl = layer == 0 ? p : vec2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y) + 17.3;
        vec2 qW = pl / cellW;
        vec2 idW = floor(qW), fW = fract(qW);
        vec3 hW = dioHash32(idW + float(layer) * 31.7);
        float ttW = t * (0.35 + 0.5 * hW.z) + hW.x * 10.0;
        vec3 h2W = dioHash32(idW + floor(ttW) * 7.31 + float(layer) * 31.7);
        float on = step(0.55, h2W.y);
        float blinkW = sin(fract(ttW) * 3.14159265);
        float rW = cellW * 0.05;
        float rD = max(rW, footprint * 1.6);
        vec2 c = 0.2 + 0.6 * h2W.zx;
        float spotW = 1.0 - smoothstep(rD * 0.4, rD, length((fW - c) * cellW));
        // LOD: com a celula do tamanho do pixel (ou menor) cada pixel cai numa celula sorteada e a grade vira ruido de
        // pixel que muda a cada passo de giro da camera. Ao longe o brilho tende para a media (energia da faceta
        // espalhada: ~0,0004 por grade), de forma continua com a distancia.
        float lodS = smoothstep(2.0, 6.0, cellW / footprint);
        sum += mix(4.4e-4, on * blinkW * blinkW * spotW * (rW * rW) / (rD * rD), lodS);
    }
    return sum * path;
#endif
    float cell = 0.55 / max(ubo.normalParams.x, 1e-3);
    vec2 q = p / cell;
    vec2 id = floor(q), f = fract(q);
    vec3 h = dioHash32(id);
    float tt = t * (0.35 + 0.5 * h.z) + h.x * 10.0;
    vec3 h2 = dioHash32(id + floor(tt) * 7.31);
    vec2 slope = (h2.xy - 0.5) * 0.5;
    vec3 nf = normalize(n + vec3(-slope.x, 0.0, -slope.y));
    float a = dot(reflect(-V, nf), L);
    float facet = smoothstep(0.9950, 0.9992, a);
    float blink = sin(fract(tt) * 3.14159265);
    float rWorld = cell * 0.07;
#ifdef ERUPTION_SLOT_WEB
    float rDraw = max(rWorld, footprint * 1.6); // sem TAA: ponto de ~3 px, nao de 1 px que pisca
#else
    float rDraw = max(rWorld, footprint * 0.6);
#endif
    float spot = 1.0 - smoothstep(rDraw * 0.4, rDraw, length((f - (0.25 + 0.5 * h2.zx)) * cell));
    return facet * blink * blink * spot * (rWorld * rWorld) / (rDraw * rDraw);
}

// Espuma de margem pela profundidade vertical (depthV): a linha de contato
// rente ao chao, rendada por ruido, e faixas que andam para a margem e se
// desfazem. Vale tambem em volta de tronco, pedra e personagem dentro d'agua:
// a cena logo atras da superficie fica rasa ali.
// Onda quebrando (vagalhao que vira a lamina na areia) e o encontro de dois
// lobos, que continua um pouco mar adentro. Renda parada no chao, semente da
// onda do lobo maior.
float shoreBreakFoam(vec2 p, float depthV, ShoreOnda ond) {
    float d = depthV - shoreRecuoDe(ond.e1);
    if (d < 0.0 || ond.quebra + ond.e2 <= 0.0) return 0.0;
    float s = max(ubo.normalParams.x, 1e-3);
    vec2 semente = ond.semente * 0.77;
    float lace = (snoise(p * s * 0.9 + semente) * 0.5 + 0.5) * 0.6 + (snoise(p * s * 2.6 - semente.yx) * 0.5 + 0.5) * 0.4;
    float quebra = clamp(ond.quebra, 0.0, 1.0) * smoothstep(0.2, 0.55, lace + 0.25);
    float encontro = shoreEncontro(ond) * smoothstep(0.3, 0.6, lace + 0.1) * (1.0 - smoothstep(0.15, 0.6, d));
    return clamp(max(quebra, encontro * 0.8), 0.0, 1.0);
}
// Mesma onda da lamina na areia: os lobos de common/shore_waves.glsl,
// avaliados no proprio ponto (o pe' do lobo; no chao debaixo d'agua d = 0
// e o pbr_common.glsl chega no mesmo ponto). Sem isso a espuma do lado da
// agua andava no seu proprio relogio e o branco na linha d'agua nao
// acompanhava a espuma da praia. A espuma da linha d'agua aparece com a
// onda recuada e vai junto com a borda (repuxo, shoreRecuoDe).
float shoreFoamMask(vec2 p, float depthV, ShoreOnda ond, float width) {
    if (width <= 0.0) return 0.0;
    depthV -= shoreRecuoDe(ond.e1);
    if (depthV < 0.0) return 0.0;
    // 1 com a onda recuada (a espuma esta' na linha d'agua), 0 com ela no
    // alto da praia (a espuma subiu junto com a lamina).
    float recuada = 1.0 - smoothstep(0.15, 0.5, ond.e1);
    float x = depthV / width;
    if (x > 2.0) return 0.0;
    float s = max(ubo.normalParams.x, 1e-3);
    // Desenho parado no chao e novo a cada onda (semente da onda do lobo
    // maior, a mesma da renda na areia): antes o ruido andava 0,05-0,09 u/s
    // num sentido fixo e a espuma escorria de lado.
    vec2 semente = ond.semente * 0.77;
    float n1 = snoise(p * s * 0.9 + semente) * 0.5 + 0.5;
    float n2 = snoise(p * s * 2.6 - semente.yx) * 0.5 + 0.5;
    float lace = n1 * 0.6 + n2 * 0.4;
    float line = (1.0 - smoothstep(0.05, 0.25 + 0.35 * lace, x)) * smoothstep(0.25, 0.55, lace + 0.2);
    // So' com a crista: a linha branca parada na linha d'agua com a onda
    // recuada aparecia ANTES da onda (espuma, e a onda chegando depois). Agora
    // ela estoura no instante em que a crista chega 'a linha d'agua e o lobo
    // comeca a subir (ond.quebra, shoreCristaEm), e sobe junto com a lamina.
    line *= clamp(ond.quebra * 2.2, 0.0, 1.0) * (0.4 + 0.6 * recuada);
    return clamp(max(line, shoreBreakFoam(p, depthV + shoreRecuoDe(ond.e1), ond)), 0.0, 1.0);
}

// Opacidade da agua de diorama na borda (shadeDiorama): 0 na linha d'agua,
// 1 a 0,35 u de profundidade (o mesmo trecho em que a lamina do chao some
// debaixo do plano, pbr_common.glsl shoreSwash: com 1,5 u sobrava uma faixa
// de areia sem a cor de nenhuma das duas). A malha d'agua e' montada por tile do terreno e
// o contorno dela e' uma escada; sem refracao (ou com ela fraca) essa escada
// cortava a beira (lamina, espuma, faixa clara) em blocos. Transparente na
// borda, quem aparece ali e' o chao com a beira da onda por baixo.
float g_dioramaAlpha = 1.0;

vec3 shadeDiorama() {
    float time  = ubo.waterLevelAndPlanes.w;
    float level = vLevel;
    vec2 viewport = ubo.screenSize.xy;
    vec2 suv = getScreenUV();
    int screenTexSlot = ubo.textureSlots.x;
    int depthTexSlot  = ubo.textureSlots.y;
    float rain  = clamp(ubo.weatherParams.w, 0.0, 1.0);
    float storm = ubo.weatherParams.z;
    vec3 V = normalize(ubo.cameraPos.xyz - vWorldPos);
    float viewDist = length(ubo.cameraPos.xyz - vWorldPos);
    vec3 L = normalize(-ubo.sunDirIntensity.xyz);
    float sunI = ubo.sunDirIntensity.w * ubo.weatherParams.x * (1.0 - storm * 0.6);

    // Tamanho do pixel no plano (o maior eixo: conservador no rasante).
    vec2 dpx = dFdx(vWorldPos.xz), dpy = dFdy(vWorldPos.xz);
    float footprint = max(max(length(dpx), length(dpy)), 1e-4);
#ifdef ERUPTION_SLOT_WEB
    // Navegador: o mesmo tamanho de pixel no plano, mas analitico (distancia x angulo do pixel / inclinacao do
    // raio) e nao pela derivada de tela. A derivada e' por bloco de 2x2 pixels e salta de um bloco para o outro;
    // como ela decide quais ondas entram (keep), a agua tremia a cada movimento da camera.
    {
        float pxAng = 2.0 / (length(vec3(pc.viewProj[0][1], pc.viewProj[1][1], pc.viewProj[2][1])) * max(viewport.y, 1.0));
        footprint = max(viewDist * pxAng / max(V.y, 0.12), 1e-4);
    }
#endif

    // Profundidade vertical sob este pixel (espuma, calmaria, borda).
    float sceneDepth = slotTextureLod(depthTexSlot, suv, 0.0).r;
    vec3 floorPos = reconstructWorldPos(suv, sceneDepth);
    float depthV = sceneDepth >= 1.0 ? 1e4 : max(0.0, level - floorPos.y);
    float shoreW = max(ubo.foamParams.x, 0.05) * 6.0 * ubo.styleParams2.x;
    // Borda do lado da agua: reflexo e brilho entram junto com a opacidade
    // (g_dioramaAlpha, 0,35 u). Quem faz a transicao para a areia e' a
    // transparencia + a lamina do chao. Com 2,5 u a agua ja' opaca ficava sem
    // o ceu refletido perto da borda: uma faixa escura (~105 de 255 contra o
    // mar a ~140) entre a lamina e o mar.
    // Onda da praia aqui (lobos + crista chegando), so' no raso: o tempo da
    // volta em 10000 s como o da lamina (Render.cpp setShoreWater), senao a
    // onda sorteada de cada lobo nao seria a mesma dos dois lados.
    ShoreOnda ondaB;
    ondaB.e1 = 1.0; ondaB.e2 = 0.0; ondaB.sobe1 = 0.0; ondaB.sobe2 = 0.0; ondaB.ex1 = 1.0; ondaB.cuspide = 1.0e4; ondaB.semente = vec2(0.0);
    ondaB.seca = 0.0; ondaB.crista = 0.0; ondaB.face = 0.0; ondaB.quebra = 0.0;
    // Com a lamina (normalParams.w) ou com a espuma de contato: lago e
    // piscina sem nenhuma das duas ficam parados.
    const bool ondasPraia = ubo.foamParams.z > 0.5 || ubo.normalParams.w > 0.5;
    // Inclinacao do fundo pela derivada da profundidade na tela, levada ao
    // mundo pela inversa de [dpx; dpy] (fluxo uniforme). Serve a tres coisas:
    // a rampa (crista so' sobre fundo em rampa: parede de piscina e fundo
    // chato nao fazem onda quebrar), e a direcao da costa (a profundidade
    // cresce para o mar) que da' o eixo das elipses dos lobos.
    vec2 dDep = vec2(dFdx(depthV), dFdy(depthV));
    float detF = dpx.x * dpy.y - dpx.y * dpy.x;
    vec2 gDep = abs(detF) > 1e-8
        ? vec2(dpy.y * dDep.x - dpx.y * dDep.y, -dpy.x * dDep.x + dpx.x * dDep.y) / detF : vec2(0.0);
    float rampa = length(gDep);
    float naPraia = smoothstep(0.02, 0.05, rampa) * (1.0 - smoothstep(0.35, 0.7, rampa));
    // Terra a partir da agua (o mesmo `fora` da areia) e tangente da costa.
    vec2 foraA = rampa > 0.01 && depthV < 1.0e3 ? -gDep / rampa : vec2(0.0);
    vec2 costaA = vec2(-foraA.y, foraA.x);
    if (ondasPraia && depthV < 2.6) {
        float tOnda = mod(time, 10000.0);
        // Perto da linha d'agua (repuxo, espuma de contato) a onda do proprio
        // ponto, como o fundo debaixo d'agua na areia (d = 0).
        if (depthV < 0.8) ondaB = shoreOndaEm(vWorldPos.xz, tOnda, -1.0, costaA);
        // Crista: lobos no pe' do pixel (a linha d'agua para a terra, a ~1:8).
        shoreCristaEm(ondaB, vWorldPos.xz + foraA * min(8.0 * depthV, 26.0), tOnda, depthV, costaA);
    }
    ondaB.crista *= naPraia; ondaB.face *= naPraia; ondaB.quebra *= naPraia;
    ondaB.e2 *= naPraia; // sem linha de encontro de lobos na parede
    float recuoBorda = ondasPraia ? shoreRecuoDe(ondaB.e1) : 0.0;
    float edge = smoothstep(recuoBorda, recuoBorda + 0.35, depthV);
    // Inclinacao da crista no plano: derivada na tela (fora do if, fluxo
    // uniforme) levada ao mundo pela inversa de [dpx; dpy]. A crista empina
    // (0,25 u numa frente de ~1 u) e a face reflete outro pedaco do ceu.
    float cristaH = ondaB.crista * 0.32;
    vec2 dCr = vec2(dFdx(cristaH), dFdy(cristaH));
    vec2 gradCrista = abs(detF) > 1e-8
        ? vec2(dpy.y * dCr.x - dpx.y * dCr.y, -dpy.x * dCr.x + dpx.x * dCr.y) / detF : vec2(0.0);
    gradCrista *= 1.0 - smoothstep(0.6, 1.2, length(gradCrista)); // pixel grande: sem degrau

    // ---- Normal: ondas + aneis de chuva; a agua rasa da margem acalma ----
    // Ondulacoes sempre para a areia: flow map de duas fases (o desenho
    // parado, arrastado pela direcao agua->terra de cada ponto; uma fase
    // aparece enquanto a outra volta ao zero, meia volta defasadas, com
    // deslocamento de semente diferente para nao pulsar no mesmo lugar).
    // Sem o campo (mapa sem terra na grade, lava) segue o vento como antes.
    // O comprimento do campo cai a 0 na divisa entre duas margens: ali a agua
    // so' para, sem trocar de ramo (antes um limiar trocava de caminho e
    // deixava emenda).
    vec2 fluxo = shoreFlow(vWorldPos.xz);
    vec3 w;
    if (ubo.skyboxSlot.w >= 0 && ubo.screenSize.w > 0.0) {
        const float kCicloFluxo = 3.0; // s
        float compr = 1.0 / (max(ubo.normalParams.x, 1e-3) * 0.33);
        // Velocidade amarrada ao repuxo da areia: o alcance maximo (16 u na
        // praia rasa, pbr_common shoreFront) subido na metade da janela de
        // subida (kShoreSubida do periodo) - ~2,5 u/s. Antes vinha do
        // comprimento da ondulacao (~7-13 u/s num mapa de praia) e a agua corria
        // muito mais forte do que a onda que invade a areia.
        vec2 vel = fluxo * (16.0 / (kShorePeriodo * kShoreSubida) * 0.5);
        float f0 = fract(time / kCicloFluxo), f1 = fract(time / kCicloFluxo + 0.5);
        vec3 w0 = dioramaWaves(vWorldPos.xz - vel * (f0 * kCicloFluxo), time, footprint, 0.0);
        vec3 w1 = dioramaWaves(vWorldPos.xz - vel * (f1 * kCicloFluxo) + compr * vec2(0.37, 0.61), time, footprint, 0.0);
        w = mix(w0, w1, abs(1.0 - 2.0 * f0));
    } else {
        w = dioramaWaves(vWorldPos.xz, time, footprint, time);
    }
    vec2 grad = w.xy;
    float splash = 0.0, ringLight = 0.0;
    if (rain > 0.001 && ubo.styleParams.w > 0.0) {
        vec4 rr = rainRipples(vWorldPos.xz, time, footprint, rain);
        grad += rr.xy * 0.5 * ubo.styleParams.w;
        splash = rr.z;
        ringLight = min(rr.w, 1.0) * ubo.styleParams.w;
    }
    {
        vec4 sr = stepRipples(vWorldPos.xz, time, footprint);
        grad += sr.xy * 5.0;
        ringLight += min(sr.w, 1.0) * 0.5;
    }
    grad *= mix(0.35, 1.0, smoothstep(0.0, shoreW * 1.5, depthV));
    grad += gradCrista;
    vec3 N = normalize(vec3(-grad.x, 1.0, -grad.y));
    float NoV = clamp(dot(N, V), 0.0, 1.0);

    // ---- Refracao: fundo deslocado pela onda; amostra que cai em algo
    // acima d'agua (o tronco, a margem) volta para a nao deslocada ----
    vec2 refrOff = N.xz * ubo.waterParams.y * 0.3 * edge;
    vec2 pxOff = refrOff * viewport * 0.5;
    if (length(pxOff) > ubo.extraParams.y) refrOff *= ubo.extraParams.y / length(pxOff);
    vec2 ruv = clamp(suv + refrOff, vec2(0.001), vec2(0.999));
    float rd = slotTextureLod(depthTexSlot, ruv, 0.0).r;
    vec3 rFloor = reconstructWorldPos(ruv, rd);
    if (rd < 1.0 && rFloor.y > level) { ruv = suv; rd = sceneDepth; rFloor = floorPos; }
    vec3 bg = slotTextureLod(screenTexSlot, ruv, 0.0).rgb;
    float path = rd >= 1.0 ? 1e4 : length(rFloor - vWorldPos);

    // Luz que chega na agua (para o espalhamento e a espuma).
    vec3 amb = max(ubo.ambientColor.rgb * ubo.ambientColor.w, vec3(0.04));
    vec3 sunL = ubo.sunColor.rgb * sunI;
    vec3 inLight = amb + sunL * max(L.y, 0.0) * 0.6;

    // Causticas no fundo raso.
    if (ubo.extraParams.z > 0.5) {
        float c = causticsPattern(rFloor.xz * max(ubo.normalParams.x, 1e-3) * 4.0, time);
        bg *= 1.0 + c * ubo.causticsParams.x * exp(-depthV * ubo.causticsParams.y) * sunI * (1.0 - storm * 0.7);
    }

    // ---- Corpo d'agua: absorcao por canal ao longo do raio dentro d'agua
    // (o vermelho some primeiro: raso transparente esverdeado, fundo azul
    // petroleo) e o espalhamento da cor funda no lugar do que foi absorvido.
    vec3 sigma = vec3(1.0, 0.36, 0.22) / ubo.styleParams2.w;
    vec3 T = exp(-sigma * path);
    vec3 under = bg * T + ubo.styleDeep.rgb * inLight * (1.0 - T);

    // ---- Reflexo: ceu, e por cima os objetos (planar; se esta agua nao
    // esta' no plano do SSPR, marcha de raio) ----
    // A rugosidade do json (feita para a agua de antes, que nao tinha onda
    // de verdade) vira micro-rugosidade: a onda ja' espalha o sol.
    float rough = clamp(ubo.waterParams.w, 0.02, 1.0);
    float roughEff = clamp(sqrt(rough * rough * 0.12 + w.z * 2.0 + rain * 0.02), 0.02, 1.0);
#ifdef ERUPTION_SLOT_WEB
    roughEff = max(roughEff, 0.16 + 0.25 * clamp(footprint, 0.0, 1.0)); // lobo do sol largo: sem fio de brilho que cintila
#endif
    vec3 R = reflect(-V, N);
    vec3 refl = skyReflection(R, rough, storm, rain, ubo.weatherParams.y, ubo.skyboxSlot.x);
    vec3 objCol = vec3(0.0);
    float objHit = 0.0;
    if (ubo.styleSlots.x >= 0 && abs(level - ubo.styleParams2.y) < 0.5) {
        // A onda desloca o ponto da superficie de onde o reflexo e' lido;
        // proporcional a' distancia, o desvio na tela nao depende do zoom.
        float D = ubo.styleParams2.z * 0.05 * viewDist;
#ifdef ERUPTION_SLOT_WEB
        // Navegador, longe: o desvio em pixels e' fixo (D ~ distancia) e a onda larga o espalha por dezenas de pixels,
        // e a imagem espelhada tem furos que o ceu preenche: manchas azuis que mudam a cada giro. Cai a 35% ao longe.
        float longeW = smoothstep(500.0, 1200.0, viewDist);
        D *= 1.0 - 0.65 * longeW;
#endif
        vec4 cp = pc.viewProj * vec4(vWorldPos + vec3(N.x, 0.0, N.z) * D, 1.0);
        vec2 puv = cp.xy / cp.w * 0.5 + 0.5;
        vec2 px = 1.0 / viewport;
        // Cinco amostras em cruz: tapam os furos do espelhamento (a imagem
        // espelhada estica e deixa pixel sem dono) e suavizam a borda.
        vec4 acc = vec4(0.0);
        const vec2 taps[5] = vec2[](vec2(0.0), vec2(0.0, 1.5), vec2(0.0, -1.5), vec2(1.5, 0.0), vec2(-1.5, 0.0));
        for (int i = 0; i < 5; ++i) {
#ifdef ERUPTION_SLOT_WEB
            // Longe os furos da imagem espelhada passam de 1,5 px: a cruz alarga junto (3x a 1200 u).
            vec4 s = slotTextureLod(ubo.styleSlots.x, clamp(puv + taps[i] * px * (1.0 + 2.0 * longeW), vec2(0.0), vec2(1.0)), 0.0);
#else
            vec4 s = slotTextureLod(ubo.styleSlots.x, clamp(puv + taps[i] * px, vec2(0.0), vec2(1.0)), 0.0);
#endif
            acc += vec4(s.rgb * s.a, s.a);
        }
        if (acc.a > 1e-3) objCol = acc.rgb / acc.a;
        objHit = clamp(acc.a / 2.5, 0.0, 1.0);
        // Reflexo espelhado (WaterReflectionPass, styleSlots.y): a cena
        // desenhada pela camera espelhada, com o que esta' fora da tela. Entra
        // so' onde o SSPR nao cobriu, no mesmo ponto deslocado pela onda.
        if (ubo.styleSlots.y >= 0 && objHit < 0.999) {
            vec4 m = slotTextureLod(ubo.styleSlots.y, clamp(puv, vec2(0.0), vec2(1.0)), 0.0);
            float h = m.a * (1.0 - objHit);
            if (h > 1e-3) {
                objCol = (objCol * objHit + m.rgb * h) / (objHit + h);
                objHit += h;
            }
        }
    } else {
        vec4 m = marchReflection(vWorldPos, reflect(-V, normalize(mix(vec3(0.0, 1.0, 0.0), N, 0.25))),
                                 depthTexSlot, screenTexSlot);
        objCol = m.rgb;
        objHit = m.a;
    }
    objHit *= 1.0 - rough * 0.5;
    refl = mix(refl, objCol, objHit);

    // Fresnel: o do ceu fica perto do fisico (F0 ate' 0,12: com 0,25 o ceu
    // claro lavava o lago inteiro de branco-azulado); o dos objetos e' o
    // dobro - estilizado, como no diorama, onde o reflexo da arvore e do
    // personagem e' o que conta.
    float F0 = mix(0.02, 0.12, clamp(ubo.waterParams.z, 0.0, 1.0));
    float F = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);
    F = mix(F, min(F * 2.2, 1.0), objHit);
    F = clamp(F * ubo.styleParams.y, 0.0, 1.0) * (1.0 - rain * 0.25) * (1.0 - storm * 0.5) * edge;

    // ---- Sol: GGX com o Fresnel da agua (o termo sem Fresnel estourava em
    // mancha branca), mais os brilhos ----
    vec3 H = normalize(L + V);
    float VoH = clamp(dot(V, H), 0.0, 1.0);
    float Fh = 0.02 + 0.98 * pow(1.0 - VoH, 5.0);
    float spec = ggxSpecular(N, H, roughEff) * Fh / (4.0 * max(NoV, 0.15)) * step(0.0, dot(N, L));
    vec3 sunSpec = sunL * spec * (1.0 - rain * 0.6) * edge;
    if (ubo.styleParams.z > 0.0 && sunI > 0.001) {
        float sp = sunSparkle(vWorldPos.xz, N, V, L, time, footprint);
        sunSpec += sunL * sp * 3.0 * ubo.styleParams.z * (1.0 - rain * 0.8) * edge;
    }
    sunSpec = min(sunSpec, vec3(4.0));

    // ---- Espuma: margem/contato, mascara de contato da CPU, respingo ----
    float foam = 0.0;
    if (ubo.foamParams.z > 0.5) {
        foam = shoreFoamMask(vWorldPos.xz, depthV, ondaB, shoreW);
        vec2 foamUV = (vWorldPos.xz / vec2(128.0, 128.0)) + 0.5;
        foam = max(foam, slotTextureLod(ubo.textureSlots.z, foamUV, 0.0).r * ubo.foamParams.y);
    } else if (ondasPraia) {
        // So' a lamina: sem a linha de contato (a agua segue pela areia), mas
        // a onda quebrando e o encontro dos lobos sao da agua.
        foam = shoreBreakFoam(vWorldPos.xz, depthV, ondaB);
    }
    foam = max(foam, splash * rain * ubo.foamParams.w);
    foam = clamp(foam, 0.0, 1.0);
    vec3 foamCol = vec3(0.90, 0.95, 0.97) * (amb + sunL * max(L.y, 0.0) * 0.85);

    vec3 color = mix(under, refl, F) + sunSpec;
    // Face da crista antes de quebrar: lamina fina contra a luz, verde-agua
    // mais clara (a luz atravessa a parede d'agua que esta' rolando).
    color += inLight * vec3(0.08, 0.22, 0.20) * clamp(ondaB.face, 0.0, 1.0) * edge;
    // Nuvens do ceu na agua, estilizadas (forca em water_style.cloud_reflection).
    // Com piso no Fresnel (0,25): olhando de cima o reflexo cai a ~3% e as
    // nuvens sumiriam de novo.
    if (ubo.skyboxSlot.y > 0) {
        // Normal suavizada (30% da onda): com a onda inteira as formas das
        // nuvens viravam ruido de ondulacao e o efeito lia como um clareado
        // uniforme. Nuvem clareia, vao entre nuvens escurece (com cobertura
        // alta a mascara cobre quase tudo e so' clarear nao mostra forma).
        vec3 Rn = reflect(-V, normalize(mix(vec3(0.0, 1.0, 0.0), N, 0.3)));
        vec2 nv = skyCloudsReflected(Rn, vWorldPos);
        float k = max(F, 0.25) * (float(ubo.skyboxSlot.y) * 0.001)
                * edge * (1.0 - objHit) * (1.0 - rough * 0.5);
        color = max(color + refl * (nv.x * mix(0.35, 0.95, nv.y) - 0.3) * k, vec3(0.0));
    }
    // Crista dos aneis de chuva: o ceu refletido mais claro num fio.
    color += skyReflection(vec3(0.0, 1.0, 0.0), rough, storm, rain, ubo.weatherParams.y, -1) * ringLight * 0.35 * edge;
    // So' pela profundidade: com max(.., espuma) a espuma da linha deixava a
    // borda opaca e a escada da malha aparecia em branco.
    g_dioramaAlpha = smoothstep(recuoBorda, recuoBorda + 0.35, depthV);
    return mix(color, foamCol, foam);
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
// unidades de F2 - F1. x = calor da fenda, que cai do meio (1) ate' a borda
// da placa (0) em degrade: o meio do canal amarelo, a borda vermelho-escura
// (borda dura de cor unica lia como recorte de papel). O tamanho do pixel
// alarga a borda (sem serrilhar a fenda fina ao longe). y = distancia a'
// fenda (relevo).
vec2 lavaCrack(vec2 p, float width) {
    vec2 f = lavaCells(p);
    float d = f.y - f.x;
    float aa = fwidth(d);
    float x = 1.0 - smoothstep(0.0, width + aa, d);
    return vec2(x * (0.55 + 0.45 * x), d);
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
    // crustCoverage baixo alarga as fendas (causticsParams.w, 0 = padrao):
    // placas menores entre canais largos, a mesma forma.
    float alarga = 1.0 + ubo.causticsParams.w;
    float wBig = (0.03 + 0.20 * wn * wn) * calor * (1.0 + 4.0 * open) * alarga;
    vec2 big = lavaCrack(pw * 0.018, wBig);
    // A fenda media afina ate' sumir ao longo dela (wn2 < 0,35): fenda de
    // largura constante e continua le' como curva de nivel.
    float wn2 = snoise(p * 0.045 + 1.3) * 0.5 + 0.5;
    float wMid = 0.06 * smoothstep(0.35, 0.9, wn2) * (1.0 + 2.0 * open) * alarga;
    vec2 mid = lavaCrack(pm * 0.055 + 9.1, wMid);
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
    // A borda quente quebra mais a crosta numa faixa estreita, sem acender
    // por igual; rente a' margem a lava esfria e vira crosta (a margem real
    // e' mais fria que o meio).
    float borda = ubo.normalAdvanced.z * smoothstep(0.8, 0.95, radial) * (1.0 - smoothstep(0.95, 1.0, radial))
                + ubo.normalAdvanced.w * (1.0 - smoothstep(0.0, 0.35, radial));
    float margemFria = smoothstep(0.93, 1.0, radial);

    // Onde a crosta se abre: manchas grandes e lentas, mais a borda quente.
    // Lava ativa tem de 30 a 50% da area incandescente nos canais.
    // crustCoverage do .env desloca o limiar (lavaCrust.w, 0 = padrao).
    float abre = ubo.lavaCrust.w;
    float open = smoothstep(0.55 - abre, 0.85 - abre, snoise(xz * 0.0045 + vec2(t * 0.002, 0.0)) * 0.5 + 0.5);
    open = clamp(open + 0.12 * borda, 0.0, 1.0) * (1.0 - margemFria);

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
    // Torcao media pequena: a fenda da placa media fica poligonal, com quina
    // (rachadura de crosta real), em vez de ondular paralela as vizinhas.
    vec2 warpMid = 1.2 * vec2(snoise(xz * 0.07 + 2.1), snoise(xz * 0.07 + 5.9));
    float wn = snoise(xz * 0.011 + 3.7) * 0.5 + 0.5;
    // Rachadura fina so' perto: longe ela vira menos de um pixel.
    float fineW = LITE ? 0.0 : 1.0 - smoothstep(70.0, 140.0, length(ubo.cameraPos.xyz - worldPos));
    vec2 c0 = lavaCrust(xz - vel * (ph0 - 0.5) * period, warpBig, warpMid, wn, open, calor, fineW);
    vec2 c1 = lavaCrust(xz - vel * (ph1 - 0.5) * period + vec2(37.3, 11.9), warpBig, warpMid, wn, open, calor, fineW);
    vec2 c = mix(c1, c0, w0);
    float heat = clamp(c.x * min(calor, 1.6), 0.0, 1.0) * (1.0 - 0.6 * margemFria);
    // Liquido exposto: quente, com conveccao lenta por baixo.
    // Liquido exposto: conveccao lenta (manchas mais e menos quentes) e as
    // dobras de pele que esfriam por cima (cordas da pahoehoe), arrastadas
    // pelo fluxo.
    vec2 m = xz - vel * t * 0.5;
    // Teto 0,92: numa caverna escura a exposicao automatica sobe, e um
    // nucleo mais quente estourava num bloco branco chapado. A pele que
    // esfria por cima (filamentos e placas finas) cobre boa parte dele.
    float molten = 0.7 + 0.22 * snoise(m * 0.02 + vec2(0.0, t * 0.03));
    float ropes = 1.0 - smoothstep(0.0, 0.22, abs(snoise(m * vec2(0.045, 0.09) + 4.4)));
    float skin = smoothstep(0.55, 0.8, snoise(m * 0.06 + 8.8) * 0.5 + 0.5);
    // Fendas alargadas (crustCoverage baixo) levam menos pele: a pele fria
    // tambem e' crosta, e sem afina-la a cobertura parava em ~38%.
    molten *= 1.0 - 0.6 / max(1.0 + ubo.causticsParams.w, 1.0) * max(ropes, skin);
    // Na regiao aberta as fendas largas viram o proprio liquido: o calor
    // delas passa a ser o do liquido, com a pele por cima (sem isso a
    // regiao inteira era fenda a calor 1, um bloco chapado).
    heat = mix(heat, min(heat, molten), open);

    // Brilho de corpo negro: cresce como uma potencia alta da temperatura,
    // para so' o que esta' quente passar do limiar do bloom.
    float kelvin = mix(850.0, 1480.0, heat);
    // Potencia 4: a exposicao automatica de uma caverna escura multiplica a
    // cena ~10x; com a curva mais baixa todo calor acima do medio saturava
    // no mesmo creme, e com uma mais alta a borda da fenda apagava e a lava
    // deixava de iluminar a sala.
    vec3 glow = blackbody(kelvin) * (3.5 * pow(heat, 4.0) * max(calor, 0.3));

    // CROSTA: basalto escuro e rugoso, iluminado pelo sol e pelo ambiente.
    // Normal pelas derivadas de tela da altura (Mikkelsen, "Bump Mapping
    // Unparametrized Surfaces on the GPU", 2010), sobre o plano da lava.
    float rough = snoise(xz * 0.5) * 0.1;
    float h = c.y * 2.6 + rough * 1.6;
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
    float tone = (0.035 + 0.035 * smoothstep(-0.4, 0.8, snoise(xz * 0.03 + 5.0) + c.y * 0.6)) * (0.8 + 0.4 * grain);
    // Basalto frio: cinza-carvao neutro, so' um pouco quente (a gradacao das
    // cavernas puxa para o azul); o prateado vem do especular. Perto do
    // calor, mais quente.
    vec3 albedo = tone * mix(vec3(1.0, 0.96, 0.92), vec3(1.0, 0.85, 0.75), smoothstep(0.1, 0.5, heat))
                * ubo.lavaCrust.rgb;
    // Ambiente sem a cor dele: o de uma caverna roxa pintava a rocha
    // preta de lilas e azul.
    float amb = dot(ubo.ambientColor.rgb, vec3(0.2126, 0.7152, 0.0722)) * ubo.ambientColor.w;
    vec3 lit = ubo.sunColor.rgb * sunI * max(dot(n, l), 0.0) + vec3(amb);
    // Vidro vulcanico: pontos lisos (o grao mais claro) dao brilhos pequenos.
    float glint = smoothstep(0.7, 0.95, grain);
    float spec = ggxSpecular(n, normalize(l + v), mix(0.55, 0.22, glint)) * 0.05 * max(dot(n, l), 0.0);
    vec3 crust = albedo * lit + ubo.sunColor.rgb * sunI * spec;
    // A crosta perto da fenda e' iluminada por ela (luz vermelha de baixo).
    crust += blackbody(1100.0) * albedo * 3.0 * smoothstep(0.1, 0.6, heat);
    float molt = smoothstep(0.35, 0.75, heat);
    if (pc.debugMask.y == 2) return molt < 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    return mix(crust, vec3(0.0), molt * 0.85) + glow;
}

void main() {
    // LAVA SAI CEDO. A cor dela sobrescrevia tudo no fim, mas antes o pixel
    // pagava o shader de agua inteiro (ondas, reflexo em espaco de tela de 40
    // passos, refracao, espuma, causticas): o passe Water custava 3,1 ms no
    // lago de lava de um andar de caverna (4060, 1080p) para uma imagem que
    // nao usava nada daquilo.
    if (int(ubo.extraParams.w + 0.5) == 1) {
        fragColor = vec4(pc.debugMask.y == 1 ? vec3(0.0, 1.0, 0.0) : lavaColor(vWorldPos, vTexCoord.x), 1.0);
        return;
    }
    // AGUA TEXTURADA (liquidKind 4, modo classico): so' o quadro da animacao
    // (textureSlots.w), ladrilhado a lavaCrust.x unidades, translucido a
    // lavaCrust.y. Sem reflexo, refracao, Fresnel, espuma nem causticas.
    if (int(ubo.extraParams.w + 0.5) == 4 && ubo.textureSlots.w >= 0) {
        vec3 tex = pow(slotTexture(ubo.textureSlots.w, vWorldPos.xz / ubo.lavaCrust.x).rgb, vec3(2.2));
        fragColor = vec4(pc.debugMask.y == 1 ? vec3(0.0, 1.0, 0.0) : tex, ubo.lavaCrust.y);
        return;
    }
    if (!LITE && ubo.styleParams.x > 0.5) {
        vec3 c = shadeDiorama();
        if (any(isnan(c)) || any(isinf(c))) c = vec3(0.05, 0.1, 0.15);
        fragColor = vec4(pc.debugMask.y == 1 ? vec3(0.0, 1.0, 0.0) : c, pc.debugMask.y == 1 ? 1.0 : g_dioramaAlpha);
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
        float h  = slotTexture(waterTexSlot, uv).r;
        float hx = slotTexture(waterTexSlot, uv + vec2(0.01, 0.0)).r;
        float hy = slotTexture(waterTexSlot, uv + vec2(0.0, 0.01)).r;
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
        vec3 localN = sampleProceduralNormal(vWorldPos.xz, normalScale, waveSpeed, time, normalStr,
                                             LITE ? 1 : normalOctaves);
        vec3 pn = localN.x * vTangent + localN.y * vNormal + localN.z * vBitangent;
        float pnLen = length(pn);
        perturbedNormal = (pnLen > 1e-5) ? (pn / pnLen) : vNormal;
    }
    {
        // Ondinhas de passo tambem na agua sem o estilo diorama (normal no mundo).
        vec2 fp = vec2(length(dFdx(vWorldPos.xz)), length(dFdy(vWorldPos.xz)));
        vec4 sr = stepRipples(vWorldPos.xz, time, max(max(fp.x, fp.y), 1e-3));
        perturbedNormal = normalize(perturbedNormal + vec3(-sr.x, 0.0, -sr.y) * 5.0);
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

    vec3 background;
    if (LITE) {
        background = slotTexture(screenTexSlot, bUV).rgb; // sem a dispersao por canal
    } else {
        float bgR = slotTexture(screenTexSlot, rUV).r;
        float bgG = slotTexture(screenTexSlot, gUV).g;
        float bgB = slotTexture(screenTexSlot, bUV).b;
        background = vec3(bgR, bgG, bgB);
    }

    // ---- Depth-based coloring using VERTICAL thickness (corrected) ----
    float sceneDepth = slotTexture(depthTexSlot, ssr).r;
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
    vec3 ssrColor = vec3(0.0);
    float ssrHit = 0.0;
    if (!LITE) {
        vec4 m = marchReflection(vWorldPos, reflect(-viewDir, ssrNormal), depthTexSlot, screenTexSlot);
        ssrColor = m.rgb;
        ssrHit = m.a;
    }

    float storm = stormDarken;
    vec3 reflectionColor = skyReflection(reflectDir, roughness, storm, rainIntensity, moonOcc, skyboxTexSlot);

    // Blend SSR over sky reflection so objects are visible on the water surface.
    reflectionColor = mix(reflectionColor, ssrColor, ssrHit * (1.0 - roughness * 0.7));

    // ---- Unified Foam System ----
    float totalFoam = 0.0;
    // A) Surface foam (Worley) — only on wave crests
    if (!LITE && enableSurfaceFoam) {
        float surfaceFoam = foamSurface(vWorldPos.xz, foamScale, foamSpeed, time);
        // Crista = ponto alto da onda de Gerstner do vertice. A mascara pela
        // normal (waveCrestMask) dava 1 em toda superficie quase plana, e a
        // espuma cobria o lago inteiro de branco; sem ondas de vertice, sem crista.
        float crestAmp = ubo.waveParams.x * 1.5;
        float crest = crestAmp > 1e-3 ? smoothstep(0.45, 0.9, (vWorldPos.y - vLevel) / crestAmp) : 0.0;
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
        float contactFoam = slotTexture(foamMaskSlot, foamUV).r * contactStr;
        totalFoam = max(totalFoam, contactFoam);
    }
    // D) Rain splash contact foam
    if (!LITE && rainIntensity > 0.001 && rainSplashFoam > 0.001) {
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
    if (!LITE && enableCaustics) {
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

    // LAVA CLASSICA (liquidKind 3, "lavaStyle": "classic" no .env): o visual de
    // antes das placas de crosta - duas camadas de ruido, calor e brilho do
    // sol; passa pela agua inteira (so' sunSpecular entra na cor).
    if (int(ubo.extraParams.w + 0.5) == 3) {
        float t = ubo.waterLevelAndPlanes.w;
        vec2 fw = vWorldPos.xz * 0.010;
        float crust = snoise(fw + vec2(t * 0.013, t * 0.021)) * 0.5 + 0.5;
        float veins = snoise(fw * 2.7 - vec2(t * 0.027, t * 0.017)) * 0.5 + 0.5;
        float heat = clamp(pow(1.0 - crust, 2.2) * 0.75 + pow(veins, 5.0) * 0.85, 0.0, 1.0);
        vec3 crustCol = vec3(0.055, 0.030, 0.028);
        vec3 hotCol = vec3(1.0, 0.30, 0.055);
        vec3 coreCol = vec3(1.0, 0.80, 0.32);
        vec3 lava = mix(crustCol, hotCol, heat);
        lava = mix(lava, coreCol, pow(heat, 4.0));
        color = lava + hotCol * heat * 1.9 + sunSpecular * 0.15;
    }

    // NaN/Inf final guard
    if (any(isnan(color)) || any(isinf(color))) {
        color = vec3(0.05, 0.1, 0.15);
    }

    // Diagnostico de cobertura do disco (ERUPTION_LIQUID_DEBUG=1): pinta a
    // superficie de CIANO chapado. Ciano e nao magenta de proposito - magenta
    // e' chroma-key de descarte no model.frag e o pixel sumiria da tela em
    // vez de marcar. Serve pra medir num print de cima quanto da cratera o
    // disco de fato cobre.
    if (pc.debugMask.y == 1) {
        // VERDE puro, nao ciano: o caso de teste do autor pinta a textura
        // de lava do TERRENO (a cratera) de ciano ao mesmo tempo, pra medir
        // na MESMA foto quanto o disco liquido cobre da cratera. Com as
        // duas na mesma cor as duas manchas se fundem e a comparacao fica
        // impossivel de ler a olho ou de segmentar por cor.
        color = vec3(0.0, 1.0, 0.0);
    }

    fragColor = vec4(color, 1.0);
}
