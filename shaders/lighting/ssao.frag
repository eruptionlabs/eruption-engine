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
    vec4 ssaoParams;        // x=enabled, y=strength, z=radius, w=unused
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
    for (int i = 0; i < KERNEL; ++i) {
        vec2 d = rot * kDisc[i];
        // Lift the disc into the hemisphere: sample point above the surface.
        float lift = 0.35 + 0.4 * float(i) / float(KERNEL);
        vec3 dir = normalize(T * d.x + B * d.y + N * lift);
        vec3 sw = P + dir * radius * (0.25 + 0.75 * float(i + 1) / float(KERNEL));

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

    outAO = clamp(1.0 - occ * lights.ssaoParams.y * 2.2, 0.0, 1.0);
}
