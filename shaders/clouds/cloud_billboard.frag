#version 450

// Volumetric independent cloud: ONE ray-marched box per layer. The density is
// fully analytic 3D: each baked blob is evaluated as a 3D ellipsoid eroded by
// 3D value-noise fbm (per-blob fixed Z offset) and unioned with max() — the
// exact 3D extension of coverage_gen.comp's localCloudDensity, so composite
// clouds merge into a single organic mass and the silhouette matches the
// cloud shadow / rain occluder. No baked-texture mask (too coarse: it read
// as blocky and popped as the wind drifted the UVs).

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

// Same layout as coverage_plane.frag (used for windOffset + world bounds).
layout(set = 0, binding = 1) uniform CloudUBO {
    vec3 worldMin;
    float cloudBottom;
    vec3 worldMax;
    float cloudTop;
    float layerSpacing;
    uint layerCount;
    float noiseScale;
    float detailStrength;
    vec3 windOffset;
    float extinctionScale;
    float scatteringAlbedo;
    float phaseG1;
    float phaseG2;
    float phaseAlpha;
    float powderStrength;
    float ambientIntensity;
    uint godRayMode;
    uint debugMode;
    float weatherCoverage;
    float rainIntensity;
    float stormTint;
    float cloudCoverageThreshold;
    float cloudScale;
    float cloudLightness;
    float cloudShade;
    float cloudSoftness;
    float pad1;
};

// Scene depth (set 2, already bound for the coverage plane): clamps the march
// so terrain/buildings inside the box occlude the volume correctly.
layout(set = 2, binding = 0) uniform sampler2D sceneDepth;

// The layer's baked blobs (same SSBO coverage_gen.comp reads).
struct LocalCloud {
    vec2 center;
    vec2 radius;
    float rotation;
    float density;
    float falloff;
    float altitude;
    float coverage;
    uint layerMask;
    uint alive;
    uint _pad;
};
layout(set = 3, binding = 0, std430) readonly buffer LocalClouds {
    LocalCloud clouds[];
} localClouds;

// Must match cloud_billboard.vert exactly.
layout(push_constant) uniform PushConstants {
    vec4 boxMin;
    vec4 boxMax;
    float seed;
    float rainIntensity;
    float stormTint;
    float tintAltitude;
    int blobCount;
    float pad0;
    float pad1;
    float pad2;
    // Target scale: 1.0 at full res, 0.5 when accumulating into the half-res
    // volume offscreen — scales gl_FragCoord back into normalized depth UVs.
    float targetScale;
    // Teto de passos da marcha, vindo do preset (cloud_march_steps em
    // data/graphics.json). 0 = usa o maximo do shader (48).
    float marchCap;
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

float valueNoise3D(float x, float y, float z, uint seed, float period) {
    int ix = int(floor(x));
    int iy = int(floor(y));
    int iz = int(floor(z));
    float fx = fract(x);
    float fy = fract(y);
    float fz = fract(z);

    int periodCells = int(ceil(period));
    if (periodCells < 1) periodCells = 1;

    int ix0 = positiveMod(ix, periodCells);
    int iy0 = positiveMod(iy, periodCells);
    int iz0 = positiveMod(iz, periodCells);
    int ix1 = positiveMod(ix + 1, periodCells);
    int iy1 = positiveMod(iy + 1, periodCells);
    int iz1 = positiveMod(iz + 1, periodCells);

    float a = hash3D(ix0, iy0, iz0, seed);
    float b = hash3D(ix1, iy0, iz0, seed);
    float c = hash3D(ix0, iy1, iz0, seed);
    float d = hash3D(ix1, iy1, iz0, seed);
    float e = hash3D(ix0, iy0, iz1, seed);
    float f = hash3D(ix1, iy0, iz1, seed);
    float g = hash3D(ix0, iy1, iz1, seed);
    float h = hash3D(ix1, iy1, iz1, seed);

    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    fz = fz * fz * (3.0 - 2.0 * fz);

    return mix(mix(mix(a, b, fx), mix(c, d, fx), fy),
               mix(mix(e, f, fx), mix(g, h, fx), fy), fz);
}

float fbm3D(float x, float y, float z, uint seed, int octaves, float persistence, float lacunarity, float basePeriod) {
    float total = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    float maxValue = 0.0;
    for (int i = 0; i < octaves; ++i) {
        total += valueNoise3D(x * frequency, y * frequency, z * frequency, seed + uint(i) * 131u, basePeriod * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }
    return total / maxValue;
}

// Per-pixel ray jitter (same IGN the layer ray-marcher uses): breaks the
// coherent march bands into fine noise so grazing rays don't show slices.
// STATIC on purpose: rotating the offset per frame shifted every sample by
// up to a full step (tens of meters at the far LOD) and made the whole cloud
// surface shimmer with everything standing still (author report 2026-08-10).
float interleavedGradientNoise(vec2 fragCoord, uint frame) {
    vec2 offset = vec2(float(frame % 8u) * 0.161803, float(frame % 16u) * 0.314159);
    fragCoord += floor(offset * 256.0);
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(floor(fragCoord), magic.xy)));
}

vec3 hsv2rgb(vec3 c) {
    vec3 rgb = clamp(abs(mod(c.x * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return c.z * mix(vec3(1.0), rgb, c.y);
}

// 3D extension of coverage_gen.comp's localCloudDensity: same ellipsoid mask
// (drifted center) and same noise law (raw-center offset, per-blob fixed Z
// slice), plus a vertical axis sized from the blob's horizontal extent.
float blobDensity(LocalCloud b, vec3 p, vec2 drift, float planeY, int idx) {
    if (b.alive == 0u) return 0.0;
    if (b.radius.x <= 0.0 || b.radius.y <= 0.0) return 0.0;
    float falloff = max(b.falloff, 0.01);

    vec2 q = p.xz - (b.center + drift);
    float cr = cos(-b.rotation);
    float sr = sin(-b.rotation);
    vec2 rp = vec2(q.x * cr - q.y * sr, q.x * sr + q.y * cr);

    // Vertical semi-axis ~0.6x the horizontal one (squat puff), clamped so
    // tiny blobs stay puffy and huge ones stay cloud-like.
    float ry = clamp(max(b.radius.x, b.radius.y) / falloff * 0.6, 6.0, 100.0);
    float cy = planeY + ry;

    vec3 d3 = vec3(rp.x / b.radius.x, (p.y - cy) / ry, rp.y / b.radius.y);
    float d = length(d3);
    float v = 1.0 - smoothstep(0.0, 1.0, d * falloff);
    if (v <= 0.0) return 0.0;

    float noiseZ = float(idx) * 3.97 + 11.13; // localNoiseZ (coverage_gen)
    // LOD band-limiting: coarse marches alias the periodic coverage fbm into
    // horizontal moire stripes (the "zoom-out shimmer"). The band now follows
    // pad2 CONTINUOUSLY so zooming never pops a cloud's noise pattern:
    // base frequency lerps 0.01 -> 0.02 over pad2 0.14..0.28, and the 3rd
    // octave fades in over pad2 0.28..0.9 by scaling its amplitude (the fbm
    // is amplitude-normalized, so weighting the octave and the normalizer by
    // the same factor is exact at both ends and smooth in between — and
    // costs the 3rd noise eval only when the fade is active; pad2 is a push
    // constant, so the branch is coherent for the whole draw).
    float freq = mix(0.01, 0.02, smoothstep(0.14, 0.28, pc.pad2));
    float detail = smoothstep(0.28, 0.9, pc.pad2);
    float fx = q.x * freq + b.center.x;
    float fy = q.y * freq + b.center.y;
    float fz = p.y * freq + noiseZ;
    uint nseed = uint(pc.seed) + 777u;
    float o1 = valueNoise3D(fx, fy, fz, nseed, 4096.0);
    float o2 = valueNoise3D(fx * 2.0, fy * 2.0, fz * 2.0, nseed + 131u, 8192.0);
    float o3 = 0.0;
    if (detail > 0.0)
        o3 = valueNoise3D(fx * 4.0, fy * 4.0, fz * 4.0, nseed + 262u, 16384.0);
    float n = (o1 + 0.5 * o2 + 0.25 * detail * o3) / (1.5 + 0.25 * detail);
    float th = 1.0 - b.coverage;
    n = smoothstep(th, th + 0.35, n);

    return v * b.density * n;
}

float cloudDensity(vec3 p, vec2 drift, float planeY) {
    float d = 0.0;
    int count = min(pc.blobCount, 100); // LocalCloud::MAX_COUNT
    for (int i = 0; i < count; ++i) {
        d = max(d, blobDensity(localClouds.clouds[i], p, drift, planeY, i));
    }
    return d;
}

// Debug (density viz mode 3): ellipsoid mask only, no noise erosion.
float cloudMask(vec3 p, vec2 drift, float planeY) {
    float d = 0.0;
    int count = min(pc.blobCount, 100);
    for (int i = 0; i < count; ++i) {
        LocalCloud b = localClouds.clouds[i];
        if (b.alive == 0u || b.radius.x <= 0.0 || b.radius.y <= 0.0) continue;
        float falloff = max(b.falloff, 0.01);
        vec2 q = p.xz - (b.center + drift);
        float cr = cos(-b.rotation);
        float sr = sin(-b.rotation);
        vec2 rp = vec2(q.x * cr - q.y * sr, q.x * sr + q.y * cr);
        float ry = clamp(max(b.radius.x, b.radius.y) / falloff * 0.6, 6.0, 100.0);
        float cy = planeY + ry;
        vec3 d3 = vec3(rp.x / b.radius.x, (p.y - cy) / ry, rp.y / b.radius.y);
        d = max(d, 1.0 - smoothstep(0.0, 1.0, length(d3) * falloff));
    }
    return d;
}

void main() {
    // pc.pad1 >= 0.5 (debug ERUPTION_TEST_PUFF_SOLID): shade the whole rasterized
    // box solid, bypassing ray-march/density — isolates rasterization
    // (pipeline, scissor, culling) from the volume math.
    // pc.pad1 >= 1.5 (debug ERUPTION_TEST_PUFF_DENSITY): visualize max march density.
    if (pc.pad1 >= 1.5) {
        vec3 ro2 = cameraPos;
        vec3 rd2 = normalize(v_worldPos - cameraPos);
        vec3 invD2 = 1.0 / rd2;
        vec3 tA2 = (pc.boxMin.xyz - ro2) * invD2;
        vec3 tB2 = (pc.boxMax.xyz - ro2) * invD2;
        vec3 tMin2 = min(tA2, tB2);
        vec3 tMax2 = max(tA2, tB2);
        float tEn2 = max(max(tMin2.x, tMin2.y), max(tMin2.z, 0.0));
        float tEx2 = min(min(tMax2.x, tMax2.y), tMax2.z);
        if (tEx2 <= tEn2) { outColor = vec4(1.0, 0.0, 1.0, 1.0); return; } // magenta = ray missed box
        vec2 drift2 = windOffset.xz * (worldMax.xz - worldMin.xz);
        float maxD = 0.0;
        float dbgA = 0.0, dbgB = 0.0;
        for (int i = 0; i < 24; ++i) {
            vec3 p2 = ro2 + rd2 * (tEn2 + (tEx2 - tEn2) * (float(i) + 0.5) / 24.0);
            // pad1 >= 3.5: raw fbm noise + blob fields; >= 2.5: mask only;
            // >= 1.5: full density.
            if (pc.pad1 >= 3.5) {
                float m = cloudMask(p2, drift2, pc.boxMin.y);
                if (m > maxD) {
                    maxD = m;
                    LocalCloud b = localClouds.clouds[0];
                    float n = fbm3D(p2.x * 0.02 + b.center.x, p2.z * 0.02 + b.center.y, p2.y * 0.02 + 11.13,
                                    uint(pc.seed) + 777u, 3, 0.5, 2.0, 4096.0);
                    dbgA = n;          // raw fbm value
                    dbgB = b.coverage; // blob coverage field
                }
            } else {
                maxD = max(maxD, (pc.pad1 >= 2.5) ? cloudMask(p2, drift2, pc.boxMin.y)
                                                  : cloudDensity(p2, drift2, pc.boxMin.y));
            }
        }
        if (pc.pad1 >= 3.5) { outColor = vec4(maxD, dbgA, dbgB, 1.0); return; } // R=mask G=fbm B=coverage
        outColor = vec4(maxD, fract(tEn2 / 100.0), fract(tEx2 / 100.0), 1.0); // R = density, G/B = enter/exit
        return;
    }
    if (pc.pad1 >= 0.5) {
        uint sh = uint(pc.seed);
        sh = (sh ^ (sh >> 13u)) * 1274126177u;
        outColor = vec4(hsv2rgb(vec3(float(sh % 1024u) / 1024.0, 0.85, 1.0)), 0.5);
        return;
    }

    vec3 ro = cameraPos;
    vec3 rd = normalize(v_worldPos - cameraPos);

    // Analytic ray-box intersection (robust with the camera inside the box).
    vec3 invD = 1.0 / rd;
    vec3 tA = (pc.boxMin.xyz - ro) * invD;
    vec3 tB = (pc.boxMax.xyz - ro) * invD;
    vec3 tMin = min(tA, tB);
    vec3 tMax = max(tA, tB);
    float tEnter = max(max(tMin.x, tMin.y), max(tMin.z, 0.0));
    float tExit = min(min(tMax.x, tMax.y), tMax.z);
    if (tExit <= tEnter) discard;

    // Scene depth gives a SOFT occlusion limit: depth precision at 1-5 km is
    // coarse (near=0.1, far=50000), so a hard tExit clamp slices the volume
    // visibly and pops as the zoom changes the projected depth. Instead the
    // density fades out over a band in front of the scene surface.
    // pc.pad0 >= 0.5 (debug ERUPTION_TEST_PUFF_NODEPCLIP): skip the clamp entirely.
    float tScene = 1e30;
    vec2 suv = gl_FragCoord.xy / (screenSize * pc.targetScale);
    float sceneZ = (pc.pad0 < 0.5) ? textureLod(sceneDepth, suv, 0.0).r : 1.0;
    if (sceneZ < 1.0) {
        vec4 wp = invViewProj * vec4(suv * 2.0 - 1.0, sceneZ, 1.0);
        if (abs(wp.w) < 1e-6) wp.w = 1e-6;
        wp.xyz /= wp.w;
        tScene = dot(wp.xyz - ro, rd);
        if (tScene <= tEnter) discard; // cloud fully behind terrain
    }
    tExit = min(tExit, tScene);
    if (tExit <= tEnter) discard;
    float fadeLen = max(8.0, tScene * 0.01);

    const vec3 cloudBright = vec3(0.96, 0.96, 0.95);
    const vec3 cloudMid    = vec3(0.75, 0.77, 0.80);
    const vec3 cloudDark   = vec3(0.52, 0.54, 0.58);

    // Adaptive step count: grazing rays cross hundreds of meters of volume —
    // a fixed 16 steps bands into visible slices (the "punk mohawk" cut).
    // Keep dt around 12 m, capped at 48 steps so distant storm clouds keep
    // enough samples to read as fluffy volume instead of flat streaks.
    // pc.pad2 scales the count down for distant clouds (distance LOD set on
    // the CPU; 1.0 = full quality). Floor of 12: fewer steps let the static
    // IGN jitter freeze into a screen-space halftone grid on far clouds.
    float marchLen = tExit - tEnter;
    // O TETO agora vem do preset. Antes era 48 fixo aqui e o
    // cloud_march_steps do graphics.json nao chegava a este shader - so' ao
    // layers_raymarch. Media do custo: dentro desta marcha, cloudDensity()
    // percorre ate' 100 bolhas POR PASSO (e mais uma vez para a luz do sol
    // quando ha' densidade), entao cada passo cortado vale muito. "Cloud
    // Volumes (field)" e' o passe mais caro dos climas de tempestade:
    // 3,80 ms em stormy contra 0,93 ms de media geral.
    float capSteps = (pc.marchCap > 0.5) ? clamp(pc.marchCap, 12.0, 48.0) : 48.0;
    int steps = int(clamp(marchLen / 2.0, 12.0, capSteps));
    float dt = marchLen / float(steps);
    
    // Convert half-res gl_FragCoord back to full-res screen coordinates using targetScale 
    // to map the 3x3 noise pattern correctly for the TAA resolve phase.
    vec2 fullResCoord = floor(gl_FragCoord.xy * max(1.0, 1.0 / pc.targetScale));
    float jitter = interleavedGradientNoise(fullResCoord, frameIndex);
    float invBoxH = 1.0 / max(pc.boxMax.y - pc.boxMin.y, 1.0);
    vec2 drift = windOffset.xz * (worldMax.xz - worldMin.xz);
    float planeY = pc.boxMin.y;
    float sunStepLen = max(pc.boxMax.y - pc.boxMin.y, 1.0) * 0.3;
    vec3 sunStep = normalize(sunDir) * sunStepLen;

    vec4 accum = vec4(0.0);
    for (int i = 0; i < steps; ++i) {
        float t = tEnter + (float(i) + jitter) * dt;
        vec3 p = ro + rd * t;
        float dens = cloudDensity(p, drift, planeY);
        // Soft occlusion: thin the density out approaching the scene surface.
        dens *= clamp((tScene - t) / fadeLen, 0.0, 1.0);
        if (dens > 0.001) {
            // Cheap directional shading: one density tap toward the sun, plus
            // a vertical gradient so tops read brighter than bottoms.
            // Distance LOD: far clouds shade from the density we already
            // have (no second tap). The switch FADES over pad2 0.7..0.95 so
            // the lighting model never flips on a single frame mid-zoom.
            float sunFade = smoothstep(0.7, 0.95, pc.pad2);
            float sunT = exp(-dens * 2.0);
            if (sunFade > 0.0) {
                float realT = exp(-cloudDensity(p + sunStep, drift, planeY) * 3.0);
                sunT = mix(sunT, realT, sunFade);
            }
            float hFrac = clamp((p.y - pc.boxMin.y) * invBoxH, 0.0, 1.0);
            float lit = clamp(sunT * (0.45 + 0.55 * hFrac) + 0.10, 0.0, 1.0);
            vec3 base = mix(cloudDark, mix(cloudMid, cloudBright, lit), lit);

            float a = 1.0 - exp(-dens * dt * 0.12);
            accum.rgb += (1.0 - accum.a) * base * a;
            accum.a   += (1.0 - accum.a) * a;
            // Early-out once saturated: at 95% opacity the remaining steps add
            // sub-LSB contributions after the final 8-bit rounding.
            if (accum.a > 0.95) break;
        }
    }
    if (accum.a <= 0.003) discard;

    // Heavier darkening during storms to compensate for POM removal
    float darken = 1.0 - 0.75 * pc.rainIntensity - 0.45 * pc.stormTint;
    accum.rgb *= max(darken, 0.15);

    // Debug test tint: opaque, fully saturated color per layer (8-color
    // palette from the layer seed) so each cloud is tracked unambiguously
    // across frames. Off when tintAltitude < 0 (normal rendering).
    if (pc.tintAltitude >= 0.0) {
        if (pc.tintAltitude > 500.0) {
            accum.rgb = vec3(1.0, 0.0, 1.0) * accum.a;
        } else {
            // Debug tint: multiply the shaded ramp by a saturated per-layer hue
            // (palette from the layer seed). The march's gradient and alpha stay
            // exactly as the normal render — only the gray becomes the layer color.
            uint sh = uint(pc.seed);
            sh = (sh ^ (sh >> 13u)) * 1274126177u;
            float hue = float(sh % 8u) / 8.0;
            accum.rgb *= hsv2rgb(vec3(hue, 0.85, 1.0));
        }
    }

    outColor = accum;
}
