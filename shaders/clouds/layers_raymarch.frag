#version 450

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outDepth;

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

layout(push_constant) uniform PushConstants {
    vec4 data;
} pc;

layout(set = 0, binding = 1) uniform CloudLayerUBO {
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
    float marchStepBudget;
    float pad2;
};

layout(set = 1, binding = 0) uniform sampler2DArray cloudCoverageArray;
layout(set = 1, binding = 1) uniform sampler2D cloudAltitudeMap;

const int MAX_STEPS = 32;
const float STEP_SIZE = 80.0;

float hash(uint n) {
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7fffffffu) / float(0x7fffffffu);
}

float interleavedGradientNoise(vec2 uv, uint frame) {
    vec2 coord = floor(uv * screenSize);
    coord += vec2(float(frame % 8u) * 0.5, float(frame % 4u) * 0.5);
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(floor(coord), magic.xy)));
}

float sampleCoverage(vec3 P) {
    vec2 range = worldMax.xz - worldMin.xz;
    vec2 uv = fract((P.xz - worldMin.xz) / range);

    // Horizontal coverage from the array.
    float cov;
    if (layerCount == 1u) {
        cov = textureLod(cloudCoverageArray, vec3(uv, 0.0), 0.0).r;
    } else {
        float layerF = (P.y - cloudBottom) / layerSpacing;
        int layer0 = int(clamp(floor(layerF), 0.0, float(layerCount) - 1.0));
        int layer1 = int(clamp(ceil(layerF), 0.0, float(layerCount) - 1.0));
        float t = fract(layerF);
        t = t * t * (3.0 - 2.0 * t);

        float c0 = textureLod(cloudCoverageArray, vec3(uv, float(layer0)), 0.0).r;
        float c1 = textureLod(cloudCoverageArray, vec3(uv, float(layer1)), 0.0).r;
        cov = mix(c0, c1, t);
    }

    // Per-pixel altitude: dense clouds sit lower, light clouds float higher.
    float altitude = textureLod(cloudAltitudeMap, uv, 0.0).r;
    float cloudRange = cloudTop - cloudBottom;
    float centerY = cloudBottom + altitude * cloudRange;
    // cloudScale thickens/thins the vertical puff; smaller scale = puffier, larger = wispier.
    float halfThick = cloudRange * 0.12 / max(cloudScale * cloudScale, 0.1);
    float envelope = exp(-pow((P.y - centerY) / halfThick, 2.0));

    // Soften / harden coverage edges.
    float softEdge = smoothstep(0.0, 1.0, cov);
    cov = mix(cov, softEdge, cloudSoftness);

    // Only apply the 2D altitude envelope if using a flat 1-layer coverage texture.
    // When using multiple layers (3D field), the vertical profile is already baked into the slices.
    if (layerCount == 1u) {
        cov *= envelope;
    }

    return cov;
}

float fbmDetail(vec3 P) {
    // Two octaves of pseudo-random value noise for cheap, fluffy detail.
    float v = 0.0;
    float amp = 0.5;
    float freq = noiseScale * cloudScale * cloudScale;
    for (int i = 0; i < 2; ++i) {
        vec3 c = floor(P * freq);
        vec3 f = fract(P * freq);
        f = f * f * (3.0 - 2.0 * f);
        uint seed = uint(c.x + c.y * 57u + c.z * 113u + uint(i) * 737u);
        float n = mix(mix(hash(seed), hash(seed + 1u), f.x),
                      mix(hash(seed + 57u), hash(seed + 58u), f.x), f.y);
        n = mix(n, hash(seed + 113u), f.z);
        v += n * amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return v;
}

float sampleDensity(vec3 P) {
    float cov = sampleCoverage(P);
    // The coverage texture is already thresholded by the Cloud Amount slider on CPU.
    // weatherCoverage only adds extra density for stormy weather.
    cov = pow(cov, 1.0 - weatherCoverage * 0.5);

    float density = cov;
    if (density < 0.02) return 0.0;

    // Erode coverage with high-frequency procedural detail.
    float detail = fbmDetail(P + windOffset);
    detail = mix(0.65, 1.0, detail);

    // Clamp density so clouds stay translucent and cheap to integrate.
    return clamp(density * detail * 1.5, 0.0, 1.0);
}

float hg(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
}

float phaseFunction(float cosTheta) {
    return hg(cosTheta, phaseG1) * phaseAlpha + hg(cosTheta, phaseG2) * (1.0 - phaseAlpha);
}

float powder(float density, float cosTheta) {
    float p = 1.0 - exp(-density * 2.0);
    float w = smoothstep(0.5, -0.5, cosTheta);
    return mix(1.0, p, w * powderStrength);
}

vec3 sampleSunTransmittance(vec3 P, vec3 L) {
    // Cheap 3-step transmittance lookup toward the sun. Fewer steps than the
    // old 6-step loop keeps the inner ray-march from exploding in cost.
    float T = 1.0;
    float step = (cloudTop - cloudBottom) * 0.333;
    for (int i = 1; i <= 3; ++i) {
        vec3 Pi = P + L * float(i) * step;
        float d = sampleDensity(Pi);
        T *= exp(-d * extinctionScale * step);
        if (T < 0.05) break;
    }
    return vec3(T);
}

vec3 computeLighting(vec3 P, float density, vec3 V) {
    vec3 L = normalize(sunDir);
    float cosTheta = dot(V, L);
    vec3 sunColor = vec3(1.0, 0.95, 0.85);
    // Scale direct sun to avoid HDR blow-out on bright clouds.
    vec3 sunLight = sunIntensity * 0.25 * sunColor * phaseFunction(cosTheta) * powder(density, cosTheta);

    // Sun transmittance through the cloud volume toward the light source.
    // 3 steps is cheap and already gives good self-shadowing until a cached
    // cloud shadow map is implemented.
    vec3 T = sampleSunTransmittance(P, L);
    sunLight *= T;

    // Keep cloud lighting in a sane HDR range so they don't blow out the composite.
    sunLight = clamp(sunLight, vec3(0.0), vec3(6.0));

    vec3 ambient = vec3(0.65, 0.70, 0.90) * ambientIntensity;

    // Darken clouds heavily when raining or stormy.
    float darken = 1.0 - 0.75 * rainIntensity - 0.45 * stormTint;
    darken = max(darken, 0.15);

    // Shade/lightness tuning from the skybox UI.
    vec3 lit = sunLight + ambient;
    vec3 shaded = lit * (1.0 - cloudShade);
    vec3 cloudColor = mix(shaded, lit * cloudLightness, darken);

    return cloudColor * density * scatteringAlbedo;
}

bool intersectLayer(vec3 ro, vec3 rd, out float tEnter, out float tExit) {
    float tBottom = (cloudBottom - ro.y) / rd.y;
    float tTop = (cloudTop - ro.y) / rd.y;
    tEnter = min(tBottom, tTop);
    tExit = max(tBottom, tTop);
    return tExit > max(tEnter, 0.0);
}

vec3 reconstructWorldDir(vec2 uv) {
    vec4 clip = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec4 eye = invViewProj * clip;
    if (abs(eye.w) > 1e-6) eye.xyz /= eye.w;
    return normalize(eye.xyz - cameraPos);
}

void main() {
    vec3 ro = cameraPos;
    vec3 rd = reconstructWorldDir(v_uv);

    float tEnter, tExit;
    if (!intersectLayer(ro, rd, tEnter, tExit)) {
        // DEBUG: ray misses the cloud layer box — show as dark blue when enabled.
        if (pc.data.x > 0.5) {
            outColor = vec4(0.0, 0.0, 0.5, 0.5);
        } else {
            outColor = vec4(0.0);
        }
        outDepth = 1.0;
        return;
    }



    float marchLen = max(tExit - tEnter, 0.0);
    // When the camera is inside or very close to the cloud layer, marchLen is small.
    // Clamping to a large STEP_SIZE (e.g., 20m) causes huge dithering jumps that TAA
    // cannot resolve, producing harsh halftone patterns/jittering. Lower clamp to 1.0m.
    // Orçamento de passos do preset. Menos passos com passo MAIOR percorre a
    // mesma distância: perde detalhe de borda, não perde a nuvem. É a alavanca
    // direta para hardware fraco neste passe, que é o mais caro do frame.
    int marchSteps = MAX_STEPS;
    if (marchStepBudget > 0.5) marchSteps = clamp(int(marchStepBudget), 4, MAX_STEPS);
    float currentStepSize = max(1.0, marchLen / float(marchSteps));
    
    // Scale gl_FragCoord back to full screen resolution to match the 3x3 TAA kernel.
    // (Without this, non-power-of-two viewports stretch the IGN grid and create static halftones).
    vec2 fullResCoord = floor(v_uv * screenSize);
    float jitter = interleavedGradientNoise(fullResCoord, frameIndex);
    float t = max(tEnter, 0.0) + jitter * currentStepSize;

    vec3 luminance = vec3(0.0);
    float transmittance = 1.0;
    float firstHitT = tExit;

    for (int i = 0; i < marchSteps && t < tExit; ++i) {
        vec3 P = ro + rd * t;
        float density = sampleDensity(P);

        if (density > 0.001) {
            vec3 light = computeLighting(P, density, rd);
            float ext = density * extinctionScale;
            float stepT = exp(-ext * currentStepSize);
            vec3 stepL = light * (1.0 - stepT) / max(ext, 0.0001);

            luminance += transmittance * stepL;
            transmittance *= stepT;

            if (firstHitT > tExit - 0.1) firstHitT = t;

            if (transmittance < 0.01) break;
        }

        t += currentStepSize;
    }

    // Clamp cloud opacity so the scene never disappears completely behind clouds.
    outColor = vec4(clamp(luminance, 0.0, 1.5), clamp(1.0 - transmittance, 0.0, 0.85));

    // Normalize depth to [0,1] roughly; engine uses reversed-Z likely, so keep linear for now.
    outDepth = firstHitT / 10000.0;
}
