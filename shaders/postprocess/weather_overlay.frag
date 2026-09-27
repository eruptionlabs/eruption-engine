#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform usampler2D heightmap;
layout(set = 0, binding = 3) uniform sampler2DArray cloudCoverageArray;
layout(set = 0, binding = 4) readonly buffer LensDrops {
    vec4 drops[]; // xy = pos (0..1), z = radius, w = intensity
} lensDrops;

layout(set = 0, binding = 6) uniform WeatherUBO {
    mat4 invViewProj;
    vec4 splashClouds[32]; // xy = rain-cloud box center XZ, zw = half extents (0 = empty slot)
    vec4 splashCloudsB[32]; // x = cloud plane Y (view-ray occlusion of crowns)
    vec4 splashCloudInfo;  // x = active count
    vec4 splashOccParams;  // x = 1: binding 7 (cloud volume occlusion) is live this frame
    vec4 tornadoA;         // xyz = base do funil em mundo, w = altura
    vec4 tornadoB;         // x = raio base, y = raio topo, z = giro, w = livre
} ubo;

// Screen-space fluff occlusion (alpha) accumulated by the cloud volume pass
// this frame: exact "how much cloud puff covers this pixel" — used to hide
// crowns/splashes behind clouds exactly like rain particles, including the
// cases the analytic plane test misses (camera inside the fluff hanging
// below the cloud plane).
layout(set = 0, binding = 7) uniform sampler2D cloudVolumeOccTex;

layout(push_constant) uniform Push {
    vec4 params;      // x=heatShimmer, y=heatSpeed, z=heatScale, w=dropLensAmount
    vec4 params2;     // x=heatWorldHeight, y=stormTint, z=lightningFlash, w=time
    vec4 params3;     // x=lensDropMode, y=lensDropCount, z=weatherTier, w=lensDropRadius
    vec4 params4;     // x=rainIntensity, y=rainWind, z=rainSplashIntensity, w=rainSplashRadius
    vec4 params5;     // x=rainShadowSoftness, y=fogDensity, z=fogHeightFalloff, w=cloudBaseHeight
    vec4 params6;     // x=weatherType, y=weatherIntensity, z=windDebrisIntensity, w=atmosphericTint
    vec4 params7;     // xyz=sunVisualDir, w=rainbowIntensity
    vec4 params8;     // xyz=cameraDir, w=mirageIntensity
    vec4 params9;     // x=shootingStarIntensity, y=tornadoIntensity, z=dustDevilIntensity, w=auroraIntensity
    vec4 screenSize;  // xy=size, zw=invSize
    vec4 cameraPos;   // xyz = camera world pos
    vec4 worldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeY
    vec4 cloudWorldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeY
    vec4 cloudParams; // x=layer, y=threshold, z=fieldMode (1 = rain from follower clouds only), w=rainSplashAmount
    vec4 cloudWindOffset; // x=windOffsetX, y=windOffsetY, z=debugSplashArea, w=rainCrownGain (0=1.0)
    vec4 rainBoxCenter; // xyz = crown splash box center (character anchor displaced FORWARD along the camera dir in field mode), w = box half-extent
} push;

// WeatherType enum values (must match C++ WeatherType). Mostly used for tint color.
const int WT_Clear = 0;
const int WT_Sandstorm = 26;
const int WT_DustStorm = 27;
const int WT_DustDevil = 28;
const int WT_VolcanicAsh = 40;
const int WT_FireSmoke = 41;
const int WT_Smog = 12;
const int WT_Haze = 13;

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p;
    return fract(p);
}

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float hash31(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float noise21(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float noise31(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n = i.x + i.y * 57.0 + 113.0 * i.z;
    return mix(
        mix(mix(hash11(n), hash11(n + 1.0), f.x),
            mix(hash11(n + 57.0), hash11(n + 58.0), f.x), f.y),
        mix(mix(hash11(n + 113.0), hash11(n + 114.0), f.x),
            mix(hash11(n + 170.0), hash11(n + 171.0), f.x), f.y),
        f.z);
}

float fbm2(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * noise21(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

float fbm3(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 3; i++) {
        v += a * noise31(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

vec2 gradHash22(vec2 p) {
    float n = sin(dot(p, vec2(127.1, 311.7)));
    return fract(vec2(n, n * 1.61803398875)) * 2.0 - 1.0;
}

float gradNoise21(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    float a = dot(gradHash22(i + vec2(0.0, 0.0)), f - vec2(0.0, 0.0));
    float b = dot(gradHash22(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(gradHash22(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float d = dot(gradHash22(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbmGrad(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 3; i++) {
        v += a * gradNoise21(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

vec3 reconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldPos = ubo.invViewProj * ndc;
    if (abs(worldPos.w) < 1e-6) return vec3(0.0);
    return worldPos.xyz / worldPos.w;
}

vec3 dropRefraction(vec2 uv, vec2 center, float radius, float intensity) {
    vec2 delta = uv - center;
    float d = length(delta);
    if (d > radius) return vec3(0.0);

    float h = sqrt(max(0.0, radius * radius - d * d));
    vec3 normal = normalize(vec3(delta, h));

    float shape = smoothstep(radius, 0.0, d);
    vec2 offset = normal.xy * 0.05 * shape * intensity;
    return vec3(shape * intensity * 0.85, offset);
}

vec3 proceduralDrops(vec2 uv) {
    float amount = push.params.w;
    if (amount < 0.001) return vec3(0.0);

    vec2 jitter = vec2(hash21(uv * 1.7 + 0.3), hash21(uv * 2.3 + 0.7)) * 0.45;
    vec2 p = uv * vec2(18.0, 10.0) + jitter;
    vec2 id = floor(p);

    float n = hash21(id + 1.0);
    float threshold = mix(1.0, 0.35, smoothstep(0.0, 0.5, amount));
    if (n > threshold) return vec3(0.0);

    float radius = (0.06 + n * 0.05) * push.params3.w;
    float intensity = 0.4 + hash21(id + 2.0) * 0.6;

    float t = push.params2.w;
    vec2 center = (id + 0.5 - jitter) / vec2(18.0, 10.0);
    center += vec2(noise21(id + t * 0.2), noise21(id + 1.3 + t * 0.2)) * 0.015;

    return dropRefraction(uv, center, radius, intensity);
}

vec3 bufferedDrops(vec2 uv) {
    float amount = push.params.w;
    if (amount < 0.001) return vec3(0.0);

    int count = int(push.params3.y);
    if (count <= 0) return vec3(0.0);
    count = min(count, 128);

    vec3 accum = vec3(0.0);
    for (int i = 0; i < count; ++i) {
        vec4 d = lensDrops.drops[i];
        if (d.w <= 0.0) continue;
        vec3 r = dropRefraction(uv, d.xy, d.z, d.w);
        accum += r * (1.0 - accum.x);
    }
    // Clamp the accumulated refraction: stacked drops sum the y/z offset
    // unbounded (up to several UV units), throwing the sampled UV into the
    // clamped screen border and smearing/darkening whole regions.
    accum.x = min(accum.x, 1.0);
    accum.yz = clamp(accum.yz, vec2(-0.06), vec2(0.06));
    return accum;
}

vec3 heatShimmer(vec2 uv, vec3 worldPos, float depth) {
    float heat = push.params.x;
    if (heat < 0.001) return texture(sceneTex, uv).rgb;

    float groundH = push.params2.x;
    float h = max(0.0, worldPos.y - groundH);
    float dist = length(worldPos.xz - push.cameraPos.xz);

    float falloff = exp(-h * 0.04) * (1.0 - smoothstep(0.0, 300.0, dist));
    if (falloff < 0.01) return texture(sceneTex, uv).rgb;

    float t = push.params2.w;
    float speed = push.params.y;
    float scale = push.params.z;

    vec2 macroUV = worldPos.xz * 0.035 * scale + vec2(t * speed * 0.05, -t * speed * 0.12);
    float macro = fbmGrad(macroUV);

    vec2 microUV = worldPos.xz * 0.2 * scale + vec2(t * speed * 0.25, -t * speed * 0.4);
    float micro = gradNoise21(microUV);

    vec2 offset = vec2(macro * 0.4 + micro * 0.15, macro * 0.9 + micro * 0.35);
    float heatPower = heat * heat;
    offset *= heatPower * falloff * 0.08;

    vec2 edgeFade = abs(fragUV - 0.5) * 2.0;
    edgeFade = 1.0 - smoothstep(0.7, 1.0, edgeFade);
    offset *= min(edgeFade.x, edgeFade.y);

    vec2 rUV = clamp(uv + offset * 1.35, vec2(0.0), vec2(1.0));
    vec2 gUV = clamp(uv + offset, vec2(0.0), vec2(1.0));
    vec2 bUV = clamp(uv + offset * 0.65, vec2(0.0), vec2(1.0));

    float r = texture(sceneTex, rUV).r;
    float g = texture(sceneTex, gUV).g;
    float b = texture(sceneTex, bUV).b;
    vec3 color = vec3(r, g, b);

    float blurRadius = length(offset) * 2.0;
    vec2 poisson[4] = vec2[](
        vec2( 0.0,  1.0),
        vec2( 0.0, -1.0),
        vec2( 1.0,  0.0),
        vec2(-1.0,  0.0)
    );
    vec3 blurColor = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUV = clamp(uv + poisson[i] * blurRadius + offset, vec2(0.0), vec2(1.0));
        blurColor += texture(sceneTex, sampleUV).rgb;
    }
    blurColor *= 0.25;

    float blurBlend = clamp(length(offset) * 12.0, 0.0, 0.7);
    color = mix(color, blurColor, blurBlend);

    return color;
}

// Rainbow arc screen-space effect.
vec3 rainbow(vec2 uv, vec3 worldPos, float depth) {
    float intensity = push.params7.w;
    if (intensity < 0.001) return vec3(0.0);

    vec3 sunDir = normalize(push.params7.xyz);
    vec3 antiSolar = -sunDir;
    vec2 center = vec2(0.5, 0.65) + vec2(antiSolar.x, antiSolar.y) * 0.25;

    float radius = length(uv - center);
    float band = smoothstep(0.48, 0.45, radius) * smoothstep(0.32, 0.35, radius);
    if (band < 0.001) return vec3(0.0);

    float angle = atan(uv.y - center.y, uv.x - center.x);
    float hue = fract(angle * 0.4 + 0.7);

    vec3 spectrum;
    spectrum.r = smoothstep(0.0, 0.16, hue) * smoothstep(0.32, 0.16, hue)
               + smoothstep(0.66, 0.83, hue) * smoothstep(1.0, 0.83, hue);
    spectrum.g = smoothstep(0.16, 0.33, hue) * smoothstep(0.50, 0.33, hue)
               + smoothstep(0.83, 0.91, hue) * smoothstep(1.0, 0.91, hue);
    spectrum.b = smoothstep(0.33, 0.50, hue) * smoothstep(0.66, 0.50, hue);

    float skyFade = smoothstep(0.0, 0.35, uv.y);
    float secondary = smoothstep(0.58, 0.55, radius) * smoothstep(0.48, 0.51, radius) * 0.25;

    return spectrum * band * skyFade * intensity * 0.6
         + spectrum * secondary * skyFade * intensity * 0.15;
}

// Sand/dust atmospheric overlay. The actual 3D depth comes from the
// stratified particle layers in WeatherRenderer; this pass only adds a
// subtle world-space haze and very faint drifting dust clouds so it never
// looks like a sliding 2D PNG.
vec3 sandDust(vec3 worldPos, float depth, float t) {
    float debris = push.params6.z;
    float tint = push.params6.w;
    if (debris < 0.001 && tint < 0.001) return vec3(0.0);

    int wt = int(push.params6.x + 0.5);
    float wind = push.params4.y;
    bool isSand = (wt == WT_Sandstorm);

    // World-space slow drift.
    vec3 drift = vec3(t * (1.0 + wind * 2.0), t * 0.15, 0.0);

    // Large soft dust clouds only — no sharp screen-space grains.
    float dustClouds = fbm3(worldPos * 0.06 + drift * 0.1) * 0.5 + 0.5;
    dustClouds = smoothstep(0.45, 0.75, dustClouds);

    // Fade with distance and height so the overlay doesn't smother the horizon.
    float dist = length(worldPos.xz - push.cameraPos.xz);
    float distFade = (depth >= 1.0) ? 0.25 : (1.0 - smoothstep(30.0, 400.0, dist));
    float heightFade = (depth >= 1.0) ? 0.6 : (1.0 - smoothstep(0.0, 80.0, worldPos.y - push.cameraPos.y));

    vec3 sandColor = isSand ? vec3(0.76, 0.60, 0.35) : vec3(0.65, 0.58, 0.48);
    // Very subtle haze + faint cloud wisps driven by tint/debris.
    vec3 color = sandColor * dustClouds * 0.08 * (tint + debris * 0.3) * distFade * heightFade;

    return color * (isSand ? 0.7 : 0.55);
}

// Dust devil: swirling dust column in world space.
vec3 dustDevil(vec2 uv, vec3 worldPos, float depth, float t) {
    float intensity = push.params9.z;
    if (intensity < 0.001) return vec3(0.0);

    vec3 center = push.cameraPos.xyz + vec3(sin(t * 0.3) * 15.0, 0.0, cos(t * 0.2) * 15.0 + 20.0);
    vec2 diff = worldPos.xz - center.xz;
    float dist = length(diff);
    float angle = atan(diff.y, diff.x);

    float h = worldPos.y - push.cameraPos.y;
    float radius = 2.0 + h * 0.15;
    float swirl = dist - radius * (1.0 + sin(angle * 6.0 + t * 3.0 + h * 0.2) * 0.2);
    float mask = smoothstep(0.8, 0.0, abs(swirl) / radius);
    mask *= smoothstep(0.0, 30.0, h) * smoothstep(60.0, 20.0, h);
    mask *= smoothstep(50.0, 10.0, dist);

    float dust = fbm2(vec2(angle * 3.0, h * 0.1) + t * 0.5);
    mask *= dust;

    return vec3(0.72, 0.60, 0.38) * mask * intensity * 1.5;
}

// Mirage: faint inverted reflection of distant objects fading when close.
vec3 mirage(vec2 uv, vec3 worldPos, float depth) {
    float mirageIntensity = push.params8.w;
    float heat = push.params.x;
    if (mirageIntensity < 0.001 && heat < 0.001) return vec3(0.0);

    float dist = length(worldPos.xz - push.cameraPos.xz);
    float farFade = smoothstep(0.0, 120.0, dist) * (1.0 - smoothstep(400.0, 800.0, dist));
    if (farFade < 0.001) return vec3(0.0);

    float t = push.params2.w;
    float wobble = sin(worldPos.x * 0.05 + t) * 0.003 + sin(worldPos.z * 0.03 + t * 1.3) * 0.003;
    float yOffset = 0.015 * farFade + wobble;

    vec2 reflUV = uv - vec2(0.0, yOffset);
    reflUV = clamp(reflUV, vec2(0.0), vec2(1.0));

    vec3 reflected = texture(sceneTex, reflUV).rgb;
    float luma = dot(reflected, vec3(0.299, 0.587, 0.114));
    reflected = mix(reflected, vec3(luma), 0.3);
    reflected *= vec3(1.05, 0.95, 0.80);

    float blend = farFade * mirageIntensity * 0.35 + farFade * heat * 0.12;
    return reflected * blend;
}

// Shooting stars and meteors streaking across the sky.
vec3 shootingStars(vec2 uv, vec3 worldPos, float depth, float t) {
    float intensity = push.params9.x;
    if (intensity < 0.001) return vec3(0.0);

    float sky = smoothstep(0.0, 30.0, worldPos.y - push.cameraPos.y);
    if (depth >= 1.0) sky = 1.0;
    if (sky < 0.1) return vec3(0.0);

    int wt = int(push.params6.x + 0.5);
    bool isMeteor = (wt == 37); // WT_MeteorShower
    float count = isMeteor ? 6.0 : 18.0;
    float speed = isMeteor ? 2.5 : 1.2;
    float brightness = isMeteor ? 2.5 : 1.0;

    vec3 color = vec3(0.0);
    for (float i = 0.0; i < count; i += 1.0) {
        float seed = i * 1.618;
        float timeOffset = hash11(seed) * 100.0;
        float cycle = 10.0 / speed;
        float localT = fract((t + timeOffset) / cycle);

        vec2 spawn = vec2(hash11(seed + 1.0), hash11(seed + 2.0));
        spawn.x = fract(spawn.x + t * 0.02 * (hash11(seed + 3.0) - 0.5));

        vec2 dir = normalize(vec2(0.4 + hash11(seed + 4.0) * 0.3, -0.6 - hash11(seed + 5.0) * 0.2));
        float len = isMeteor ? 0.18 : 0.06 + hash11(seed + 6.0) * 0.06;
        float thickness = isMeteor ? 0.003 : 0.0015;

        vec2 head = spawn + dir * localT * 1.5;
        vec2 toPixel = uv - head;
        float along = dot(toPixel, dir);
        float across = length(toPixel - dir * along);

        float trail = smoothstep(-len, 0.0, along) * smoothstep(0.0, -len * 0.3, along);
        float spark = smoothstep(thickness * 2.0, 0.0, across);
        float streak = smoothstep(thickness, 0.0, across) * trail;

        float life = 1.0 - smoothstep(0.0, 0.15, localT) * (1.0 - smoothstep(0.85, 1.0, localT));

        color += vec3(0.95, 0.95, 1.0) * (streak + spark * 0.5) * life * brightness;
    }
    return color * intensity * sky * (isMeteor ? 0.8 : 0.5);
}

// FUNIL VOLUMETRICO (tornado / tromba d'agua / rajada descendente).
//
// O que havia antes era PINTURA SOBRE GEOMETRIA: reconstruia worldPos a partir
// do depth e escurecia o pixel. Dois defeitos de fundo vinham dai:
//   1. Contra o CEU nao havia geometria, entao nao havia funil - e tornado se
//      ve' justamente recortado no ceu. O efeito so' aparecia manchando o chao.
//   2. O centro era `cameraPos + offset`, ou seja colado na camera (ver o
//      comentario no PostProcessor.cpp). Ele seguia o jogador.
//
// Agora e' um volume de verdade: marcha o raio da camera contra um funil
// analitico ancorado no MUNDO (ubo.tornadoA/B), respeitando a profundidade da
// cena. So' paga quem olha para ele - a interseccao com o cilindro envolvente
// e' resolvida antes, e o pixel que erra sai na hora.
vec3 tornado(vec2 uv, vec3 worldPos, float depth, float t) {
    float intensity = push.params9.y;
    if (intensity < 0.001) return vec3(0.0);

    vec3  base   = ubo.tornadoA.xyz;   // pe' do funil, no chao
    float altura = ubo.tornadoA.w;
    float rBase  = ubo.tornadoB.x;
    float rTopo  = ubo.tornadoB.y;
    float giro   = ubo.tornadoB.z;
    if (altura <= 1.0) return vec3(0.0);

    vec3 ro = push.cameraPos.xyz;
    vec3 rd = worldPos - ro;
    float distCena = length(rd);
    if (distCena < 1e-4) return vec3(0.0);
    rd /= distCena;
    // Pixel de ceu (depth no limite) nao tem cena a ocluir.
    float tCena = (depth < 0.99999) ? distCena : 1e9;

    // Cilindro envolvente do raio maximo, com folga para o rodopio.
    float rMax = max(rBase, rTopo) * 1.35;
    vec2 oc = ro.xz - base.xz;
    float a = dot(rd.xz, rd.xz);
    if (a < 1e-6) return vec3(0.0);          // raio quase vertical
    float b = 2.0 * dot(oc, rd.xz);
    float c = dot(oc, oc) - rMax * rMax;
    float disc = b * b - 4.0 * a * c;
    if (disc <= 0.0) return vec3(0.0);       // o raio nem passa perto
    float sq = sqrt(disc);
    float tEnt = (-b - sq) / (2.0 * a);
    float tSai = (-b + sq) / (2.0 * a);
    // Recorta na faixa de altura do funil.
    if (abs(rd.y) > 1e-5) {
        float tA = (base.y            - ro.y) / rd.y;
        float tB = (base.y + altura   - ro.y) / rd.y;
        tEnt = max(tEnt, min(tA, tB));
        tSai = min(tSai, max(tA, tB));
    } else if (ro.y < base.y || ro.y > base.y + altura) {
        return vec3(0.0);
    }
    tEnt = max(tEnt, 0.0);
    tSai = min(tSai, tCena);
    if (tSai <= tEnt) return vec3(0.0);      // atras da cena, ou nao intercepta

    const int PASSOS = 20;
    float dt = (tSai - tEnt) / float(PASSOS);
    // Jitter por pixel: sem ele a marcha em passo fixo desenha aneis.
    float jitter = hash11(dot(gl_FragCoord.xy, vec2(0.113, 0.271)) + t);

    float densidade = 0.0;
    float alturaMedia = 0.0;
    float detrito = 0.0;
    for (int i = 0; i < PASSOS; ++i) {
        float ti = tEnt + (float(i) + jitter) * dt;
        vec3 p = ro + rd * ti;
        float hRel = clamp((p.y - base.y) / altura, 0.0, 1.0);

        // Perfil do funil: estreito no pe', abrindo para o topo. A potencia
        // 0,55 da' a barriga concava caracteristica em vez de um cone reto.
        float raio = mix(rBase, rTopo, pow(hRel, 0.55));

        vec2 d2 = p.xz - base.xz;
        float dist = length(d2);
        float ang = atan(d2.y, d2.x);

        // Rodopio: a torcao aumenta para baixo (o pe' gira mais rapido), que e'
        // o que da' a leitura de "sugado".
        float fase = ang * 3.0 + t * giro * (2.6 - 1.6 * hRel) - hRel * 7.0;
        float ondul = sin(fase) * 0.16 + sin(fase * 2.7 + t) * 0.07;
        float raioOnd = raio * (1.0 + ondul);

        // CASCA ESTREITA. Com parede larga (era raio*0.55) e miolo cheio a
        // densidade saturava o alfa dentro de todo o cilindro envolvente e o
        // funil virava uma mancha palida - parecia nevoa, nao tornado. Aqui a
        // parede e' fina e o miolo quase vazio, entao o que se ve' e' a BORDA,
        // que e' o que da' a forma.
        float parede = 1.0 - smoothstep(0.0, 1.0, abs(dist - raioOnd) / (raio * 0.20));
        float dentro = 1.0 - smoothstep(raioOnd * 0.20, raioOnd * 0.75, dist);
        float dens = max(parede, dentro * 0.18);

        // Ruido advectado na espiral: sem ele a casca e' lisa demais e le' como
        // cone de plastico. A coordenada gira com a altura, entao a textura
        // sobe torcendo junto com o funil.
        vec3 pn = vec3(cos(fase) * 2.2, p.y * 0.05 - t * 0.6, sin(fase) * 2.2);
        dens *= 0.45 + 0.85 * fbm3(pn);

        // Some no topo (dissolve na nuvem) e engrossa no pe' (detrito levantado).
        dens *= smoothstep(1.0, 0.72, hRel);
        dens *= mix(1.35, 1.0, smoothstep(0.0, 0.18, hRel));

        densidade += dens * dt;
        alturaMedia += hRel * dens * dt;

        // TERCEIRA CAMADA (plano original: casca + volume + detrito): poeira
        // e fragmentos presos na parede, girando junto com o funil. Hash
        // esparso por celula da espiral (fase/altura/tempo quantizados) em
        // vez de uma textura de particulas - poucas celulas "acendem" por
        // passo, e giram porque `fase` ja' inclui o rodopio dependente de
        // altura/tempo usado na parede acima. So' perto da parede visivel e
        // so' na metade de baixo (e' onde o vento rasante levanta detrito;
        // no topo vira condensacao, sem solido preso).
        float celula = floor(fase * 2.5) + floor(hRel * 14.0) * 37.0 +
                       floor(t * giro * 0.6) * 131.0;
        float achou = step(0.975, hash11(celula));
        detrito += achou * parede * smoothstep(0.55, 0.0, hRel);
    }
    if (densidade <= 1e-4) return vec3(0.0);
    alturaMedia /= densidade;

    // Opacidade por Beer-Lambert: satura em vez de estourar quando o raio
    // atravessa o funil no comprimento.
    // Coeficiente baixo de proposito: o raio pode atravessar centenas de
    // unidades dentro do cilindro, e com 0.055 o alfa saturava em 1 quase
    // sempre - todo pixel virava opaco e a silhueta sumia.
    float alfa = 1.0 - exp(-densidade * 0.020);
    alfa = clamp(alfa, 0.0, 1.0) * intensity;

    // Cor: escuro embaixo (terra e detrito) clareando para cima (condensacao),
    // puxado para o tom da tempestade.
    // Escuro embaixo (terra e detrito levantado), cinza medio em cima
    // (condensacao). O topo NAO vai a branco: clarear demais era o que fazia
    // o funil parecer nuvem baixa em vez de tornado.
    vec3 baixo = vec3(0.055, 0.050, 0.048);
    vec3 alto  = vec3(0.42, 0.43, 0.47);
    vec3 cor = mix(baixo, alto, smoothstep(0.10, 0.85, alturaMedia));

    // Detrito: poeira/fragmentos mais claros que a fumaca, presos na parede -
    // sem eles o funil e' liso demais e falta a leitura de "esta arrancando
    // coisa do chao". Capado para nao estourar quando muitas celulas acendem
    // perto da camera.
    vec3 corDetrito = vec3(0.58, 0.52, 0.40);
    cor += corDetrito * clamp(detrito * 0.6, 0.0, 1.0);

    return cor * alfa;
}

// Aurora borealis/australis curtains in the night sky.
vec3 aurora(vec2 uv, vec3 worldPos, float depth, float t) {
    float intensity = push.params9.w;
    if (intensity < 0.001) return vec3(0.0);

    float sky = smoothstep(0.0, 30.0, worldPos.y - push.cameraPos.y);
    if (depth >= 1.0) sky = 1.0;
    if (sky < 0.1) return vec3(0.0);

    float x = uv.x * 4.0 + t * 0.03;
    float y = uv.y * 2.0;
    float curtain = 0.0;
    curtain += sin(x * 3.0 + fbm2(vec2(x * 0.5, t * 0.1)) * 2.0) * 0.5 + 0.5;
    curtain *= smoothstep(0.2, 0.8, uv.y) * smoothstep(1.0, 0.5, uv.y);

    vec3 color = vec3(0.2, 0.85, 0.4) * curtain;
    color += vec3(0.6, 0.2, 0.7) * (1.0 - curtain) * 0.4;

    return color * intensity * sky * 0.4;
}

// Atmospheric tint for sand/dust/smoke/volcanic ash.
vec3 atmosphericTint(vec3 color, vec3 worldPos, float depth) {
    float tint = push.params6.w;
    float debris = push.params6.z;
    if (tint < 0.001 && debris < 0.001) return color;

    int wt = int(push.params6.x + 0.5);
    vec3 tintColor;
    if (wt == WT_Sandstorm || wt == WT_DustStorm || wt == WT_DustDevil) {
        tintColor = vec3(0.75, 0.55, 0.25);
    } else if (wt == WT_VolcanicAsh) {
        tintColor = vec3(0.35, 0.32, 0.30);
    } else if (wt == WT_FireSmoke) {
        tintColor = vec3(0.55, 0.35, 0.18);
    } else if (wt == WT_Smog || wt == WT_Haze) {
        tintColor = vec3(0.55, 0.52, 0.42);
    } else {
        tintColor = vec3(0.6, 0.55, 0.45);
    }

    float dist = length(worldPos.xz - push.cameraPos.xz);
    float fog = exp(-dist * 0.003 * tint);
    color = mix(tintColor * 0.7, color, fog);

    // Screen-space debris specks REMOVED (author request 2026-08-10): the
    // fragUV-locked noise dots were the "ridiculous bind-tela effect". Wind
    // debris now renders exclusively as world-space sand/dust particles
    // (WeatherRenderer dust slice, lateral wind box + wind-shadow occlusion).

    return color;
}

// ============================================================================
// Rain splash (unified across tiers)
// ============================================================================

float rainShadowMask(vec3 worldPos) {
    vec2 hmUV = (worldPos.xz - push.worldBounds.xy) / push.worldBounds.zw;
    if (any(lessThan(hmUV, vec2(0.0))) || any(greaterThan(hmUV, vec2(1.0)))) return 1.0;
    uint h = texture(heightmap, hmUV).r;
    float maxY = uintBitsToFloat(h) - 10000.0;
    float softness = push.params5.x;
    return smoothstep(maxY - softness, maxY, worldPos.y);
}

vec3 rainSplashCrown(vec2 worldXZ, float worldY, float t, float densityScale) {
    float rainSplash = push.params4.z;
    if (rainSplash < 0.001) return vec3(0.0);

    float radiusScale = push.params4.w;
    // splashAmount x densityScale: heavier local rain (loaded cloud) => more
    // active cells per m2, light rain => few (author feedback 2026-08-10).
    float splashAmount = clamp(push.cloudParams.w * densityScale, 0.0, 1.0);
    // SEM saida antecipada por splashAmount: ele fica baixo no caso normal, e
    // cortar aqui apaga os respingos (aprendido na pratica, nao na leitura).

    // Screen-size LOD. 1 cell = 0.625 m, so wpp * 1.6 is pixel size in cell
    // units. When the crown's narrowest feature (width 0.07 cells) drops
    // below ~2 px it aliases to nothing and only sub-pixel flash dots remain
    // — splashes read as invisible at default/far zoom (author feedback
    // 2026-08-09). Two-stage fix, both driven by growRaw (grow needed for
    // 2 px features):
    //   1. Near: grow all radii (lodGrow, capped at 2.0 so a crown never
    //      reaches past the 3x3 neighbor loop).
    //   2. Far (growRaw > ~2-4): even the grown crown aliases, so blend to a
    //      soft round splash blob that stays >= 2 px.
    // Active cells are thinned quadratically with the grow so splashes per
    // screen area stay constant instead of overlapping into mush.
    float wpp = max(length(dFdx(worldXZ)), length(dFdy(worldXZ)));
    float growRaw = 2.0 * wpp * 1.6 / 0.07;
    float lodGrow = clamp(growRaw, 1.0, 2.0);
    float blobMix = smoothstep(2.0, 4.0, growRaw);
    float blobR = min(2.0 * wpp * 1.6, 0.45); // >= 2 px sigma, capped inside the 3x3 loop reach
    float gThin = clamp(growRaw, 1.0, 4.0);
    radiusScale *= lodGrow;

    // Lean splashes by wind so they follow the rain direction.
    worldXZ -= vec2(push.params4.y, 0.0) * (worldY * 0.02);

    // World-space grid. 1.6 cells/m is already much sparser than the old
    // mobile/medium flashes, so individual impacts read as real splashes.
    vec2 p = worldXZ * 1.6;
    vec2 cell = floor(p);

    vec3 color = vec3(0.0);
    vec3 sunDir = normalize(push.params7.xyz);

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = cell + vec2(float(x), float(y));
            vec2 rnd = hash22(neighbor + 0.5);

            // NAO INVERTER. O comentario historico dizia "lower splashAmount = fewer
            // active cells", e eu "corrigi" o mix para bater com ele - resultado:
            // a chuva parou de pingar (reportado pelo autor). Na pratica
            // splashAmount fica BAIXO, e e' este mix que deixa as celulas acesas.
            // O comentario estava errado, o codigo estava certo.
            float threshold = mix(0.96, 0.22, clamp(splashAmount, 0.0, 1.0));
            if (rnd.x > threshold) continue;

            // LOD thinning: stable per cell (no temporal flicker), keeps the
            // splash count per screen area constant when the LOD grows.
            if (gThin > 1.0 && hash21(neighbor + 7.31) > 1.0 / (gThin * gThin)) continue;

            vec2 center = neighbor + rnd;
            vec2 delta = p - center;
            float d = length(delta);
            float angle = atan(delta.y, delta.x);

            // Each splash has its own lifetime phase.
            float phaseSpeed = 0.55 + rnd.y * 0.35;
            float phase = fract(rnd.x + t * phaseSpeed);
            float age = phase;

            // --- 1. Impact flash: tiny, bright, short-lived ---
            // Weight 1.0 (was 1.6): with the renormalized splashGain the old
            // weight blew past the bloom threshold and flashes read as big
            // glowing orbs (author feedback 2026-08-09).
            float flashRadius = 0.10 * radiusScale;
            float flash = exp(-d * d / (flashRadius * flashRadius + 0.0001));
            flash *= pow(1.0 - age, 3.0);

            // --- 2. Crown: radial wall of water (milk-crown) ---
            int petals = 4 + int(rnd.y * 4.0); // 4..7 petals
            float petalAngle = angle + rnd.x * 6.283185;
            float petalShape = abs(sin(float(petals) * petalAngle));
            petalShape = pow(petalShape, 3.0); // sharpen into separate petals

            float crownRadius = (0.12 + age * 0.55) * radiusScale * (0.85 + rnd.y * 0.3);
            float crownWidth = 0.07 * radiusScale;
            float crownRing = smoothstep(crownRadius + crownWidth, crownRadius, d) *
                              smoothstep(crownRadius - crownWidth * 0.5, crownRadius, d);
            float crown = crownRing * petalShape * (1.0 - age) * 0.85;

            // --- 3. Central jet: quick vertical spike ---
            float jetRadius = 0.07 * radiusScale;
            float jet = exp(-d * d / (jetRadius * jetRadius + 0.0001));
            jet *= (1.0 - age) * 0.45;

            // --- 4. Secondary droplet: small bead ejected on a parabola ---
            float dropletT = fract(phase * 1.15 + 0.12);
            float dropletAngle = rnd.x * 6.283185;
            float dropletDist = dropletT * (0.25 + rnd.y * 0.15) * radiusScale;
            vec2 dropletPos = center + vec2(cos(dropletAngle), sin(dropletAngle)) * dropletDist;
            float droplet = smoothstep(0.035 * radiusScale, 0.0, length(p - dropletPos));
            droplet *= (1.0 - dropletT) * 0.9;

            float crownIntensity = flash * 1.0 + crown + jet + droplet * 1.3;

            // --- Lighting: fake specular from sun/moon ---
            vec3 toSplash = normalize(vec3(delta.x, 0.25, delta.y));
            float sunFacing = max(dot(toSplash, sunDir), 0.0);
            float spec = pow(sunFacing, 12.0) * 0.6;

            // Base water colour, slightly blue/translucent.
            vec3 waterCol = vec3(0.88, 0.93, 1.0);
            vec3 litCol = waterCol + vec3(1.0) * spec;

            // Far LOD: soft round splash blob (>= 2 px) replaces the crown
            // that would alias away. Same lifetime pulse as the crown.
            float blob = exp(-d * d / (blobR * blobR + 1e-5)) * pow(1.0 - age, 2.0) * 1.6;
            float intensity = mix(crownIntensity, blob, blobMix);
            color += litCol * intensity;
        }
    }

    // Restrict splashes to the horizontal area of the fixed rain box.
    vec2 boxHalf = vec2(push.rainBoxCenter.w);
    vec2 delta = abs(worldXZ - push.rainBoxCenter.xz);
    float edgeFade = 5.0;
    vec2 fade = smoothstep(boxHalf - edgeFade, boxHalf, delta);
    float boxMask = 1.0 - max(fade.x, fade.y);

    // NOTE: intensity scaling by rainSplash happens at the call site, with a
    // renormalizing gain — effectiveRainSplashIntensity() is rainIntensity *
    // 0.05 (~0.035 for rainy), which multiplied straight through made crowns
    // effectively invisible (author feedback 2026-08-09).
    return clamp(color, 0.0, 1.4) * boxMask;
}

float sampleCloudCoverage(vec2 worldXZ) {
    vec2 cloudWorldSize = push.cloudWorldBounds.zw;
    vec2 uv = (worldXZ - push.cloudWorldBounds.xy) / cloudWorldSize - push.cloudWindOffset.xy;
    uv = fract(uv);
    // The coverage texture is already thresholded by the Cloud Amount slider on CPU.
    float cov = textureLod(cloudCoverageArray, vec3(uv, push.cloudParams.x), 0.0).r;

    // Erode the coarse coverage mask with cheap procedural detail so rain/splash
    // only appears under visible cloud silhouettes, matching the ray-march detail.
    float detail = fbm2(worldXZ * 0.15 + push.params2.w * 0.05);
    detail = mix(0.5, 1.0, detail);
    return cov * detail;
}

// Sample the cloud shadow projected along the sun direction, matching
// cloud_shadows.frag. Returns high values under dense clouds (shadow) and
// low values where the sun reaches the ground (clear).
float sampleCloudShadow(vec3 worldPos) {
    float cloudBaseHeight = push.params5.w;
    vec2 cloudWorldSize = push.cloudWorldBounds.zw;

    float heightFactor = max(cloudBaseHeight - worldPos.y, 0.0);

    vec3 L = normalize(push.params7.xyz);
    vec2 lightOffset = vec2(0.0);
    if (L.y > 0.01) {
        float tanAngle = min(length(L.xz) / L.y, 4.0);
        lightOffset = -normalize(L.xz + vec2(0.0001)) * heightFactor * tanAngle;
    }

    vec2 baseUV = (worldPos.xz + lightOffset - push.cloudWorldBounds.xy) / cloudWorldSize
                  - push.cloudWindOffset.xy;

    // Soft penumbra sampling; radius grows with cloud altitude.
    float sampleRadius = min(heightFactor * 0.0004, 0.0015);
    float shadow = 0.0;
    const int SAMPLE_COUNT = 8;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        float angle = float(i) / float(SAMPLE_COUNT) * 6.283185307;
        vec2 dir = vec2(cos(angle), sin(angle));
        float r = sampleRadius * (0.25 + 0.75 * fract(float(i) * 0.6180339887));
        vec2 uv = fract(baseUV + dir * r);
        float cov = textureLod(cloudCoverageArray, vec3(uv, push.cloudParams.x), 0.0).r;
        shadow += cov;
    }
    shadow /= float(SAMPLE_COUNT);

    // Boost contrast: clear areas become hard holes, dense clouds stay rainy.
    return smoothstep(0.15, 0.55, shadow);
}


// Footprint of the independent rain clouds (rain followers), uploaded per
// frame in the weather UBO. The global coverage array does NOT contain these
// clouds, so this is the only way to gate splash/wetness to pixels actually
// under a rain cloud (author feedback 2026-08-09).
float rainCloudFootprint(vec2 xz) {
    float m = 0.0;
    int n = int(ubo.splashCloudInfo.x + 0.5);
    for (int i = 0; i < n; ++i) {
        vec4 c = ubo.splashClouds[i];
        if (c.z <= 0.0 || c.w <= 0.0) continue;
        vec2 d = abs(xz - c.xy) / c.zw;
        // Edge fade 0.85..1.0: exactly the boxMask the rain fragment shader
        // uses (particle_render.frag), so crowns exist precisely where drops
        // fall — no splash outside the rain region (author feedback).
        m = max(m, 1.0 - smoothstep(0.85, 1.0, max(d.x, d.y)));
    }
    return m;
}

// Rain "loadedness" (rainRate, 0.6..1.8) of the cloud raining on this point,
// weighted by the same footprint mask — crowns get denser under heavier
// clouds and sparser under light ones, matching the local rain per m2
// (author feedback 2026-08-10).
float rainCloudRate(vec2 xz) {
    float m = 0.0;
    int n = int(ubo.splashCloudInfo.x + 0.5);
    for (int i = 0; i < n; ++i) {
        vec4 c = ubo.splashClouds[i];
        if (c.z <= 0.0 || c.w <= 0.0) continue;
        vec2 d = abs(xz - c.xy) / c.zw;
        float mask = 1.0 - smoothstep(0.85, 1.0, max(d.x, d.y));
        m = max(m, ubo.splashCloudsB[i].y * mask);
    }
    return m;
}

// View-ray cloud occlusion for the crown/splash, mirroring the rain
// particle model (particle_render.frag): a follower cloud hides the splash
// when the camera->ground ray crosses the cloud's plane inside its box —
// EXCEPT the cloud that rains on that pixel (own-cloud exemption: its volume
// is translucent fluff and its own rain/splash stays visible). Only stacked
// clouds between camera and ground occlude (author feedback 2026-08-10:
// "splash não pode aparecer entre as nuvens").
// FUNDIDA: taxa de chuva + oclusao numa varredura so' da lista de nuvens.
// Antes eram rainCloudRate() e splashCloudOcclusion(), duas funcoes chamadas
// uma atras da outra em main(), cada uma percorrendo as MESMAS ate' 40 nuvens
// seguidoras POR PIXEL, a 1080p - duas varreduras de 40 sobre 2 milhoes de
// pixels. Era o grosso do passe "Weather Overlay", que na chuva custava
// 2,11 ms, metade do Post inteiro (o autor apontou o post como gargalo na
// chuva). Com um laco so': 1,08 ms.
// A matematica de cada saida e' identica a de antes. Verificado tambem por
// imagem: original vs fundido deu RMS 4,27, enquanto DUAS execucoes do MESMO
// binario dao RMS 7,24 - a cena e' nao-deterministica (chuva animada) e a
// diferenca da fusao esta' abaixo desse ruido.
// .x = taxa (era rainCloudRate), .y = oclusao (era splashCloudOcclusion)
vec2 rainCloudRateAndOcc(vec3 worldPos) {
    vec2 outv = vec2(0.0);
    int n = int(ubo.splashCloudInfo.x + 0.5);
    float dy = worldPos.y - push.cameraPos.y;
    bool podeOcluir = abs(dy) >= 1e-3;
    for (int i = 0; i < n; ++i) {
        vec4 c = ubo.splashClouds[i];
        if (c.z <= 0.0 || c.w <= 0.0) continue;
        vec2 d = abs(worldPos.xz - c.xy) / c.zw;
        float dmax = max(d.x, d.y);
        float mask = 1.0 - smoothstep(0.85, 1.0, dmax);
        outv.x = max(outv.x, ubo.splashCloudsB[i].y * mask);

        if (!podeOcluir) continue;
        if (dmax < 1.0) continue;                 // nuvem propria: isenta
        float cloudY = ubo.splashCloudsB[i].x;
        if (cloudY <= 0.0) continue;
        float tc = (cloudY - push.cameraPos.y) / dy;
        if (tc <= 0.0 || tc >= 1.0) continue;     // plano nao esta' entre camera e pixel
        vec2 xz = push.cameraPos.xz + (worldPos.xz - push.cameraPos.xz) * tc;
        vec2 d2 = abs(xz - c.xy) / c.zw;
        outv.y = max(outv.y, 1.0 - smoothstep(0.85, 1.0, max(d2.x, d2.y)));
    }
    return outv;
}

float splashCloudOcclusion(vec3 worldPos) {
    float dy = worldPos.y - push.cameraPos.y;
    if (abs(dy) < 1e-3) return 0.0;
    float occ = 0.0;
    int n = int(ubo.splashCloudInfo.x + 0.5);
    for (int i = 0; i < n; ++i) {
        vec4 c = ubo.splashClouds[i];
        if (c.z <= 0.0 || c.w <= 0.0) continue;
        vec2 dg = abs(worldPos.xz - c.xy) / c.zw;
        if (max(dg.x, dg.y) < 1.0) continue; // own cloud: exempt
        float cloudY = ubo.splashCloudsB[i].x;
        if (cloudY <= 0.0) continue;
        float tc = (cloudY - push.cameraPos.y) / dy;
        if (tc <= 0.0 || tc >= 1.0) continue; // plane not between camera and pixel
        vec2 xz = push.cameraPos.xz + (worldPos.xz - push.cameraPos.xz) * tc;
        vec2 d = abs(xz - c.xy) / c.zw;
        // Same soft edge as the footprint/rain box (0.85..1.0).
        occ = max(occ, 1.0 - smoothstep(0.85, 1.0, max(d.x, d.y)));
    }
    return occ;
}

float computeWeatherMask(vec3 worldPos) {
    float cloudBaseHeight = push.params5.w;
    // Pixel is below the cloud layer (with a small soft transition).
    float belowCloud = 1.0 - smoothstep(cloudBaseHeight - 5.0, cloudBaseHeight + 5.0, worldPos.y);
    // cloudParams.z: 1 = field mode — rain falls ONLY under the follower
    // clouds, so gate splash/wetness by their footprints (the ONLY data
    // source that actually contains the rain clouds). 0 = global precip —
    // rain falls in the camera rain box, and the crown boxMask already
    // matches it exactly. The dormant global coverage array is never a valid
    // splash gate: it does not contain the independent rain clouds, and its
    // noise coverage leaks splash everywhere (author feedback 2026-08-09:
    // "sem oclusão do rain splash em relação à nuvem").
    float gate = 1.0;
    if (push.cloudParams.z > 0.5) {
        // Follower-box footprint only: rain now falls uniformly inside the
        // box (rainMask = union of silhouette and box in particle_render),
        // so the old fbm detail erosion just made crowns sparser than the
        // rain in a pattern unrelated to it (author feedback 2026-08-09).
        gate = rainCloudFootprint(worldPos.xz);
    }
    return belowCloud * gate;
}


void main() {
    float depth = texture(depthTex, fragUV).r;
    vec3 worldPos = reconstructWorldPos(fragUV, depth);
    vec2 uv = fragUV;

    // Global camera-under-cloud factor. When the camera is above the cloud
    // layer the rain overlay (drops, splashes, wetness) is disabled, while
    // other atmospheric effects (heat shimmer, mirage, storm tint, etc.) remain.
    float cloudBaseHeight = push.params5.w;
    float cameraUnderCloud = 1.0 - smoothstep(cloudBaseHeight - 15.0, cloudBaseHeight + 15.0, push.cameraPos.y);

    float weatherMask = computeWeatherMask(worldPos);

    // Heat shimmer first, then mirage reflection layered on top.
    vec3 color = heatShimmer(uv, worldPos, depth);
    color += mirage(uv, worldPos, depth);

    // Rainbow overlay.
    color += rainbow(uv, worldPos, depth);

    // Aurora curtains.
    color += aurora(uv, worldPos, depth, push.params2.w);

    // Shooting stars / meteors.
    color += shootingStars(uv, worldPos, depth, push.params2.w);

    // Lens drops: pure screen-space effect (drops slide down the camera lens).
    // The on/off gate is frame-uniform but world-space: drops only appear
    // while the camera itself is inside the rain box and below the cloud
    // layer. Per-pixel world masks (weatherMask) must NOT be applied here —
    // they slice the sliding drops along 3D geometry, hide them over the
    // sky (depth=1 reconstructs above the clouds) and make the mask slide
    // under the static drops as the camera moves.
    int mode = int(push.params3.x + 0.5);
    vec2 camBoxDelta = abs(push.cameraPos.xz - push.rainBoxCenter.xz);
    float boxEdge = 5.0;
    float inRainBox = 1.0 - smoothstep(push.rainBoxCenter.w - boxEdge, push.rainBoxCenter.w,
                                       max(camBoxDelta.x, camBoxDelta.y));
    float dropGate = cameraUnderCloud * inRainBox;
    vec3 drop = vec3(0.0);
    if (push.params.w * dropGate > 0.001) {
        drop = ((mode == 0) ? proceduralDrops(fragUV) : bufferedDrops(fragUV)) * dropGate;
    }
    if (drop.x > 0.0) {
        vec2 refractedUV = clamp(fragUV + drop.yz, vec2(0.0), vec2(1.0));
        vec3 refracted = texture(sceneTex, refractedUV).rgb;
        float rim = drop.x * drop.x * 0.08;
        color = mix(color, refracted * (1.0 - rim), drop.x);
    }

    // Sand/dust grain overlay.
    color += sandDust(worldPos, depth, push.params2.w);

    // Dust devil.
    color += dustDevil(uv, worldPos, depth, push.params2.w);

    // Tornado / waterspout / downburst.
    color += tornado(uv, worldPos, depth, push.params2.w);

    // Rain splash / wetness (unified across all tiers)
    float rain = push.params4.x;
    float rainSplash = push.params4.z;
    float weatherRain = max(rain, rainSplash * 0.5) * weatherMask;
    float t = push.params2.w;
    // Debug splash area (UI checkbox / cloudWindOffset.z): crowns render
    // MAGENTA instead of white, so their real placement is unmistakable
    // against the scene (author request 2026-08-09).
    bool splashViz = push.cloudWindOffset.z > 0.5;

    if (weatherRain > 0.001 && depth < 1.0) {
        // Distance falloff anchored at the character. In field mode the
        // splash box (rainBoxCenter) is displaced FORWARD with the camera
        // zoom to cover the visible ground, so the wetness anchor comes from
        // splashCloudInfo.yz (the character) and the radius from
        // splashCloudInfo.w (covers the whole crown box; 0 = legacy 350 m).
        vec2 wetAnchor = (push.cloudParams.z > 0.5) ? ubo.splashCloudInfo.yz
                                                    : push.rainBoxCenter.xz;
        float dist = length(worldPos.xz - wetAnchor);
        float wetRadius = (push.cloudParams.z > 0.5 && ubo.splashCloudInfo.w > 1.0)
                          ? ubo.splashCloudInfo.w : 350.0;
        float wetFalloff = 1.0 - smoothstep(0.0, wetRadius, dist);
        if (wetFalloff > 0.0) {
            float shadow = rainShadowMask(worldPos);

            // Reconstruct surface normal to mask splash by surface orientation.
            float depthX = texture(depthTex, fragUV + vec2(push.screenSize.z, 0.0)).r;
            float depthY = texture(depthTex, fragUV + vec2(0.0, push.screenSize.w)).r;
            vec3 worldPosX = reconstructWorldPos(fragUV + vec2(push.screenSize.z, 0.0), depthX);
            vec3 worldPosY = reconstructWorldPos(fragUV + vec2(0.0, push.screenSize.w), depthY);
            vec3 dx = worldPosX - worldPos;
            vec3 dy = worldPosY - worldPos;
            vec3 normal = normalize(cross(dx, dy));
            if (any(isinf(normal)) || any(isnan(normal))) normal = vec3(0.0, 1.0, 0.0);
            float exposure = smoothstep(-0.1, 0.55, normal.y);

            // O ESCURECIMENTO DE MOLHADO SAIU DAQUI (IGNIS, 2026-09-01).
            //
            // Este bloco escurecia a cor em ~10% e puxava para um azul
            // acinzentado. Ele existia como SUBSTITUTO do molhado, na epoca em
            // que o PBR nao reagia a chuva: era um wash 2D, sem Fresnel, sem
            // especular dependente de vista e sem tocar a BRDF - lia como
            // filtro cinza, nao como superficie molhada.
            //
            // Desde f1bc4c4 o molhado e' fisico e acontece na ILUMINACAO
            // (pbrWetSurface, em lighting/pbr_common.glsl): o albedo escurece
            // por reflexao interna total, a rugosidade cai proporcional a
            // porosidade e a agua empoca na cavidade. Manter os dois somava
            // escurecimento em cima de escurecimento, e a chuva ficava mais
            // escura do que deveria - com a parte falsa por cima da real.
            //
            // O que ESTE passe continua fazendo e' o que so' ele pode fazer,
            // por ser screen-space: respingo, coroa de impacto e gota na lente.

            // Procedural crown splash: impact flash + radial petals + central jet + secondary droplet.
            // Renormalizing gain: effectiveRainSplashIntensity is ~0.01-0.05,
            // so scale it back up to a visible 0..1.2 range (x15) instead of
            // multiplying the raw value through.
            float splashGain = min(rainSplash * 10.0, 0.8);
            // Per-preset crown gain (cloudWindOffset.w = rainCrownGain;
            // 0 = neutral 1.0) — thunderstorm uses 0.75 (author feedback:
            // crown muito forte lá).
            splashGain *= (push.cloudWindOffset.w > 0.001) ? push.cloudWindOffset.w : 1.0;
            // Crown density follows the rainRate of the cloud actually
            // raining on this pixel (field mode); 1.2 = mean loadedness, so
            // an average cloud keeps the preset density, a heavy one pours
            // up to 1.5x, a light one thins out (author feedback 2026-08-10).
            float crownDensity = 1.0;
            // Uma varredura so' das nuvens para taxa E oclusao.
            vec2 rateOcc = rainCloudRateAndOcc(worldPos);
            if (push.cloudParams.z > 0.5)
                crownDensity = clamp(rateOcc.x / 1.2, 0.0, 1.5);
            vec3 splashColor = rainSplashCrown(worldPos.xz, worldPos.y, t, crownDensity);
            float occ = rateOcc.y;
            if (ubo.splashOccParams.x > 0.5) {
                // Exact screen-space fluff occlusion (includes the volume
                // hanging below the cloud plane, which the analytic plane
                // test cannot see).
                occ = max(occ, textureLod(cloudVolumeOccTex, uv, 0.0).a);
            }
            float cloudOcc = 1.0 - occ;
            float occFactor = shadow * cloudOcc;
            vec3 splashAdd = splashColor * weatherMask * exposure * wetFalloff * shadow * splashGain * cloudOcc;
            if (splashViz) {
                // Debug: divide out ONLY the per-preset crown gain (a uniform
                // visual knob, same across weathers) — the per-weather rain
                // intensity inside splashGain MUST survive, otherwise the viz
                // flatters drizzle and thunderstorm into the same splash
                // amount (author feedback 2026-08-10). MAGENTA = crown passes
                // every gate (really visible); CYAN = crown exists
                // (footprint/box/orientation ok) but the rain occlusion
                // (heightmap shadow and/or cloud blockage) killed it — the
                // cyan map IS the splash occlusion map (author request).
                float cg = (push.cloudWindOffset.w > 0.001) ? push.cloudWindOffset.w : 1.0;
                float m = min(dot(splashAdd, vec3(1.0)) / max(cg, 1.0e-3), 1.5);
                vec3 preOcc = splashColor * weatherMask * exposure * wetFalloff;
                float mb = min(dot(preOcc, vec3(1.0)), 1.5) * (1.0 - occFactor);
                splashAdd = vec3(1.0, 0.0, 1.0) * m + vec3(0.0, 1.0, 1.0) * mb;
            }
            color += splashAdd;
        }
    }

    // Atmospheric tint / debris.
    color = atmosphericTint(color, worldPos, depth);

    float storm = push.params2.y;
    if (storm > 0.0) {
        // Storm mood tint. Was storm*0.25: at thunderstorm (0.75) that pulled
        // 19% toward dark slate and, stacked with cloud shadows + wet darkening,
        // made the whole scene unreadably dark (author feedback 2026-08-09).
        color = mix(color, vec3(0.18, 0.18, 0.22), storm * 0.10);
    }

    float lightning = push.params2.z;
    if (lightning > 0.0) {
        color += vec3(lightning * 0.4);
    }

    // Subtle global darkening when the camera is under the cloud layer and
    // it is raining, mimicking the dimming you feel beneath a storm cloud.
    float rainIntensity = push.params4.x;
    color *= 1.0 - 0.05 * rainIntensity * cameraUnderCloud;

    outColor = vec4(color, 1.0);
}
