#version 450

layout(location = 0) in vec3 iWorldPos;
layout(location = 1) in vec3 iNormal;
layout(location = 2) in vec3 iLocalPos;
layout(location = 3) in float iSeed;
layout(location = 4) in float iHeight;
layout(location = 5) in float iWetness;
layout(location = 6) in float iSnowAccum;
layout(location = 7) in float iEdgeFade;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 sunDir;
    vec4 params;        // x=time, y=displacementScale, z=coverage, w=opacity
    vec4 colorTop;
    vec4 colorBottom;
    vec4 weather;       // x=stormTint, y=lightningFlash, z=unused, w=unused
    vec4 windDir;       // xyz=wind direction, w=windSpeed
} push;

float hash31(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float noise31(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n = i.x + i.y * 57.0 + 113.0 * i.z;
    return mix(
        mix(mix(hash31(vec3(n, 0.0, 0.0)), hash31(vec3(n + 1.0, 0.0, 0.0)), f.x),
            mix(hash31(vec3(n + 57.0, 0.0, 0.0)), hash31(vec3(n + 58.0, 0.0, 0.0)), f.x), f.y),
        mix(mix(hash31(vec3(n + 113.0, 0.0, 0.0)), hash31(vec3(n + 114.0, 0.0, 0.0)), f.x),
            mix(hash31(vec3(n + 170.0, 0.0, 0.0)), hash31(vec3(n + 171.0, 0.0, 0.0)), f.x), f.y),
        f.z);
}

void main() {
    float coverage = push.params.z;
    if (coverage < 0.001) discard;

    float dist = length(iWorldPos - push.cameraPos.xyz);

    float r = length(iLocalPos);
    float shell = 1.0 - smoothstep(0.55, 1.5, r);
    if (shell < 0.08) discard;

    // Flatten the bottom so clouds look like cumulus instead of soap bubbles.
    // Keep a visible base when viewed from below (camera under the cloud layer).
    float baseCut = smoothstep(-0.9, -0.55, iHeight);
    shell *= baseCut;
    if (shell < 0.08) discard;

    // Single cheap noise sample (no triplanar) to keep fragment cost minimal.
    float scale = 0.08 + iSeed * 0.04;
    float t = push.params.x * 0.03;
    float noise = noise31(iWorldPos * scale + vec3(t, t * 0.3, t * 0.7));

    // Coverage controls how much of the fluffy detail is filled in.
    float density = shell * (0.55 + coverage * 0.45 + noise * 0.25);
    density = clamp(density, 0.0, 1.0);

    // Fake self-shadow / ambient occlusion
    float rAO = clamp(r, 0.0, 1.0);
    float ao = 1.0 - smoothstep(0.0, 0.9, rAO) * 0.35;

    // Fake self-shadow: lower parts darker, top lit.
    float heightGradient = smoothstep(-1.0, 0.6, iHeight);

    // Very simple lighting
    vec3 N = normalize(iNormal);
    vec3 L = normalize(push.sunDir.xyz);
    float NdotL = max(dot(N, L), 0.0);
    float lit = (0.45 + 0.55 * NdotL) * ao;

    vec3 baseColor = mix(push.colorBottom.rgb, push.colorTop.rgb, heightGradient);
    vec3 color = baseColor * lit;

    // Rain / storm darkening
    vec3 rainColor = vec3(0.20, 0.22, 0.26);
    color = mix(color, rainColor, iWetness * 0.85);

    // Storm tint grey-blue
    vec3 stormColor = vec3(0.35, 0.38, 0.45);
    color = mix(color, stormColor, push.weather.x * 0.35);

    // Lightning flash
    color += vec3(0.85, 0.92, 1.0) * push.weather.y * 0.5;

    // Distance fade so distant clouds merge with sky.
    float fade = 1.0 - smoothstep(1000.0, 2500.0, dist);

    float opacity = push.params.w;
    float alpha = density * fade * iEdgeFade * opacity;
    alpha = clamp(alpha, 0.0, 1.0);

    outColor = vec4(color, alpha);
}
