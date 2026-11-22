#version 450

// Dedicated SSAO pass, rendered at HALF resolution into an R8 target, then
// bilateral-blurred (ssao_blur.frag) and consumed by ambient/directional.
// World-space hemisphere sampling against the G-buffer world-position target:
// no depth linearization, no reconstruction error, works with the reverse-Z
// setup untouched.

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outAO;

layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;

// Must stay a prefix-compatible view of the C++ LightingUBO.
layout(set = 0, binding = 7) uniform LightingUBO {
    vec4 dirLightDir;
    vec4 dirLightColor;
    vec4 pointLights[128];
    vec4 pointColors[128];
    uint numPointLights;
    float giIntensity;
    float giAmbientFloor;
    uint usePbr;
    uint pbrDebugMode;
    float rainIntensity;
    float snowIntensity;
    float temperatureC;
    float pbrLightScale;
    float sunAngularRadius; // era pad0; raio angular da fonte, em radianos
    uint pad1;
    vec4 ambientSky;
    vec4 ambientGround;
    vec4 ssaoParams;        // x=enabled, y=strength, z=radius, w=raio da escala curta (0 = so' a larga)
    vec4 indirectParams;
    mat4 viewProj;
    // Cauda do LightingUBO, antes truncada aqui. invViewProj foi ACRESCENTADO
    // no fim da struct em C++, entao nenhum offset antigo mudou.
    vec4 bounceParams;
    vec4 contactParams;
    vec4 skyHorizon;
    mat4 invViewProj;
} lights;


// Posicao de mundo a partir do DEPTH - ver a nota em directional.frag.
vec3 worldFromDepth(vec2 uv, float d) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, d, 1.0);
    vec4 wp = lights.invViewProj * ndc;
    if (abs(wp.w) < 1e-6) return vec3(0.0);
    return wp.xyz / wp.w;
}
// 12-direction spiral kernel (unit disc, biased toward the centre).
const int KERNEL = 12;
const vec2 kDisc[KERNEL] = vec2[](
    vec2( 0.2310, 0.0000), vec2(-0.2116, 0.2116), vec2( 0.0000,-0.4160), vec2( 0.3536,-0.3536),
    vec2( 0.5590, 0.0000), vec2(-0.4330, 0.4330), vec2( 0.0000, 0.6830), vec2(-0.5303,-0.5303),
    vec2( 0.8080, 0.0000), vec2( 0.6187, 0.6187), vec2( 0.0000,-0.9330), vec2(-0.6894, 0.6894)
);

// Quantas direcoes do kernel usar (preset "ssao_taps", 4..12). Constante de
// especializacao (so' no fragment: regra dos GPUs moveis); 12 = o de sempre.
layout(constant_id = 0) const int SSAO_TAPS = 12;
// Direcoes da segunda escala, CURTA (preset "ssao_taps_short", raio em
// ssaoParams.w): enxerga as frestas entre pedras e o contato de objeto, que o
// raio largo (12 u) nao ve. 0 = nao existe (presets low e high: o shader e' o
// de antes, instrucao por instrucao).
layout(constant_id = 1) const int SSAO_SHORT_TAPS = 0;

float ign(vec2 px) {
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

void main() {
    if (lights.ssaoParams.x < 0.5) { outAO = 1.0; return; }

    vec3 P = worldFromDepth(inUV, texture(gbufferDepth, inUV).r);
    vec3 N = texture(gbufferNormal, inUV).rgb * 2.0 - 1.0;
    if (any(isnan(P)) || any(isinf(P)) || length(N) < 1e-4) { outAO = 1.0; return; }
    N = normalize(N);

    float radius = max(lights.ssaoParams.z, 0.05);

    // Per-pixel random rotation kills the fixed-kernel banding.
    vec2 px = inUV * vec2(textureSize(gbufferDepth, 0));
    float a = ign(px) * 6.2831853;
    float ca = cos(a), sa = sin(a);
    mat2 rot = mat2(ca, -sa, sa, ca);

    // Tangent frame around N for hemisphere placement.
    vec3 up = (abs(N.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    float occ = 0.0;
    float used = 0.0;
    for (int i = 0; i < SSAO_TAPS && i < KERNEL; ++i) {
        vec2 d = rot * kDisc[i];
        // Lift the disc into the hemisphere: sample point above the surface.
        float lift = 0.35 + 0.4 * float(i) / float(SSAO_TAPS);
        vec3 dir = normalize(T * d.x + B * d.y + N * lift);
        vec3 sw = P + dir * radius * (0.25 + 0.75 * float(i + 1) / float(SSAO_TAPS));

        vec4 clip = lights.viewProj * vec4(sw, 1.0);
        if (clip.w <= 0.0) continue;
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;

        vec3 sp = worldFromDepth(suv, texture(gbufferDepth, suv).r);
        if (any(isnan(sp)) || any(isinf(sp))) continue;

        used += 1.0;
        vec3 dv = sp - P;
        float dist = length(dv);
        if (dist < 0.02 || dist > radius) continue;
        float ndl = dot(dv / dist, N);
        // Surface-plane bias rejects self-occlusion on slopes; range falloff
        // fades distant occluders.
        if (ndl > 0.15) {
            occ += (ndl - 0.15) * (1.0 - dist / radius);
        }
    }
    if (used > 0.5) occ /= used;

    float ao = clamp(1.0 - occ * lights.ssaoParams.y * 2.2, 0.0, 1.0);

    // ESCALA CURTA, somada a larga por produto (a larga guarda parede e quina,
    // esta ganha a fresta). Mesmo kernel, raio de ~1 u e rotacao propria. Some
    // quando o pixel de meia resolucao fica maior que o raio (de longe seria
    // so' ruido).
    if (SSAO_SHORT_TAPS > 0 && lights.ssaoParams.w > 0.0) {
        float rS = max(lights.ssaoParams.w, 0.02);
        vec2 texel = 1.0 / vec2(textureSize(gbufferDepth, 0));
        vec3 Pn = worldFromDepth(inUV + vec2(texel.x * 2.0, 0.0), texture(gbufferDepth, inUV + vec2(texel.x * 2.0, 0.0)).r);
        float footprint = length(Pn - P);
        float fade = clamp(1.0 - (footprint / rS - 1.0) * 0.5, 0.0, 1.0);
        if (fade > 0.0 && !any(isnan(Pn))) {
            float aS = (ign(px + vec2(17.0, 31.0))) * 6.2831853;
            mat2 rotS = mat2(cos(aS), -sin(aS), sin(aS), cos(aS));
            float occS = 0.0, usedS = 0.0;
            for (int i = 0; i < SSAO_SHORT_TAPS && i < KERNEL; ++i) {
                vec2 d = rotS * kDisc[i];
                float lift = 0.35 + 0.4 * float(i) / float(SSAO_SHORT_TAPS);
                vec3 dir = normalize(T * d.x + B * d.y + N * lift);
                vec3 sw = P + dir * rS * (0.25 + 0.75 * float(i + 1) / float(SSAO_SHORT_TAPS));
                vec4 clip = lights.viewProj * vec4(sw, 1.0);
                if (clip.w <= 0.0) continue;
                vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
                if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;
                vec3 sp = worldFromDepth(suv, texture(gbufferDepth, suv).r);
                if (any(isnan(sp)) || any(isinf(sp))) continue;
                usedS += 1.0;
                vec3 dv = sp - P;
                float dist = length(dv);
                if (dist < 0.01 || dist > rS) continue;
                float ndl = dot(dv / dist, N);
                if (ndl > 0.15) occS += (ndl - 0.15) * (1.0 - dist / rS);
            }
            if (usedS > 0.5) occS /= usedS;
            ao *= mix(1.0, clamp(1.0 - occS * lights.ssaoParams.y * 2.2, 0.0, 1.0), fade);
        }
    }
    outAO = ao;
}
