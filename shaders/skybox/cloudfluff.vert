#version 450

layout(location = 0) in vec3 iPos;
layout(location = 1) in vec3 iNormal;
layout(location = 2) in vec3 iInstancePos;
layout(location = 3) in float iSeed;
layout(location = 4) in vec3 iScale;
layout(location = 5) in float iWetness;
layout(location = 6) in float iSnowAccum;
layout(location = 7) in float iEdgeFade;

layout(location = 0) out vec3 oWorldPos;
layout(location = 1) out vec3 oNormal;
layout(location = 2) out vec3 oLocalPos;
layout(location = 3) out float oSeed;
layout(location = 4) out float oHeight;
layout(location = 5) out float oWetness;
layout(location = 6) out float oSnowAccum;
layout(location = 7) out float oEdgeFade;

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 sunDir;
    vec4 params;        // x=time, y=displacementScale, z=coverage, w=windSpeed
    vec4 colorTop;
    vec4 colorBottom;
    vec4 weather;       // x=stormTint, y=lightningFlash, z=unused, w=unused
    vec4 windDir;       // xyz=wind direction, w=unused
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
    oSeed = iSeed;
    oWetness = iWetness;
    oSnowAccum = iSnowAccum;
    oEdgeFade = iEdgeFade;

    float time = push.params.x;
    float disp = push.params.y;

    // Cloud profile: puffy on top, flatter on the bottom, like a cumulus.
    float yProfile = smoothstep(-1.0, 0.4, iPos.y);

    // Domain warping: break spherical symmetry by shearing the sample space
    // with low-frequency noise. Two layers of warp give billowy, asymmetric
    // shapes instead of a uniform ellipsoid.
    vec3 warp1 = vec3(
        noise31(iPos * 0.8 + iSeed * 5.0),
        noise31(iPos * 0.8 + iSeed * 5.0 + 17.3),
        noise31(iPos * 0.8 + iSeed * 5.0 + 33.7)
    ) * 2.0 - 1.0;
    vec3 warp2 = vec3(
        noise31(iPos * 1.6 + warp1 * 0.5 + iSeed * 3.0),
        noise31(iPos * 1.6 + warp1 * 0.5 + iSeed * 3.0 + 12.7),
        noise31(iPos * 1.6 + warp1 * 0.5 + iSeed * 3.0 + 25.4)
    ) * 2.0 - 1.0;
    vec3 npos = iPos + warp1 * 0.35 + warp2 * 0.2 + vec3(iSeed * 10.0);

    float d = noise31(npos * 1.5 + time * 0.03) * 0.5 +
              noise31(npos * 3.0 + time * 0.05) * 0.25 +
              noise31(npos * 5.0) * 0.125 +
              noise31(npos * 9.0) * 0.075;
    // Map noise to 0..1 and bias toward positive displacement for fluffy bumps.
    d = smoothstep(-1.0, 1.0, d);
    d = mix(0.15, 1.0, d) * (0.4 + disp * 0.8) * yProfile;

    // Add horizontal lumps that break the smooth ellipsoid silhouette.
    float lateral = noise31(iPos * 4.0 + warp2 * 0.3 + iSeed * 7.0);
    lateral = smoothstep(-1.0, 1.0, lateral);
    vec3 offset = iNormal * d * disp + normalize(iPos + warp1) * lateral * disp * 0.35 * yProfile;

    vec3 displaced = iPos + offset;
    oHeight = displaced.y;

    vec3 worldPos = iInstancePos + displaced * iScale;

    // Wind drift: consistent direction from push constants, scaled by per-cloud speed.
    vec3 wind = normalize(push.windDir.xyz);
    float windSpeed = push.windDir.w * (0.5 + iSeed);
    worldPos += wind * time * windSpeed;

    oWorldPos = worldPos;
    // Displacement is radial along the original normal, so the normal stays
    // approximately the same; the previous rough approximation created odd
    // lighting/culling artifacts.
    oNormal = normalize(iNormal);
    oLocalPos = displaced;

    gl_Position = push.viewProj * vec4(worldPos, 1.0);
}
