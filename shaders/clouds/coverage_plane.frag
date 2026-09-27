#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_worldPos;
layout(location = 2) in vec3 v_viewDir;

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
    float pad2;
};

layout(set = 1, binding = 0) uniform sampler2DArray cloudCoverageArray;

layout(push_constant) uniform PushConstants {
    float layer;
    float cloudCoverageThreshold;
    float edgeSoftness;
    float rayMarchSteps;
    float cloudThickness;
    float pitchThreshold;
    float spikeHeight;
    float baseDepth;
    float spikeWidth;
    float weatherCoverage;
    float rainIntensity;
    float stormTint;
    float noRepeat;     // > 0.5: independent cloud layer, do not tile the UVs
    float tintAltitude; // >= 0: debug test tint, mix(blue, red, tintAltitude)
} pc;

float sampleCloud(vec2 uv) {
    float cov = textureLod(cloudCoverageArray, vec3(uv, pc.layer), 0.0).r;
    return cov;
}

vec4 cloudColor(float cloud) {
    // The coverage texture is already thresholded by the Cloud Amount slider on CPU.
    vec3 cloudBright = vec3(0.96, 0.96, 0.95);
    vec3 cloudMid    = vec3(0.75, 0.77, 0.80);
    vec3 cloudDark   = vec3(0.52, 0.54, 0.58);

    vec3 base = mix(cloudDark, mix(cloudMid, cloudBright, cloud), cloud);

    // Stronger darkening for rain/storm to compensate for missing POM self-shadowing
    float darken = 1.0 - 0.75 * pc.rainIntensity - 0.45 * pc.stormTint;
    base *= max(darken, 0.15);
    return vec4(base, 1.0);
}

float sampleHeight(vec2 uv) {
    return sampleCloud(uv);
}


vec4 rayMarchSample(vec3 worldPos, vec3 viewDir) {
    int steps = max(int(pc.rayMarchSteps), 1);

    // March along the camera view direction through the cloud slab.
    // The length is scaled so we always cover the full vertical height range.
    float verticalRange = pc.baseDepth + pc.spikeHeight;
    float invAbsY = 1.0 / max(abs(viewDir.y), 0.001);
    float marchLength = verticalRange * invAbsY;
    vec3 rayOrigin = worldPos - viewDir * (pc.baseDepth * invAbsY);
    vec3 rayStep = viewDir * (marchLength / float(steps));

    vec2 invWorldSize = 1.0 / (worldMax.xz - worldMin.xz);

    vec4 accum = vec4(0.0);
    for (int i = 0; i < steps; ++i) {
        vec3 p = rayOrigin + rayStep * float(i);
        vec2 uv = (p.xz - worldMin.xz) * invWorldSize - windOffset.xz;
        if (pc.noRepeat < 0.5) uv = fract(uv);

        float cloud = sampleCloud(uv);
        if (cloud <= 0.0) continue;

        // Ellipsoid cloud puff per column: blue stays small/flat, red grows
        // both upward and sideways.
        float h = cloud * pc.spikeHeight;
        float halfH = max(h * 0.5, 0.1);
        float w = max(cloud * pc.spikeWidth, 1.0);
        float baseDrop = pc.baseDepth * 0.3;
        vec3 center = vec3(worldPos.x, worldPos.y - baseDrop + halfH, worldPos.z);
        vec3 local = (p - center) / vec3(w, halfH + baseDrop, w);
        float d = length(local);
        float density = cloud * smoothstep(1.0, 0.35, d);

        if (density > 0.0) {
            vec4 c = cloudColor(cloud);
            float alpha = density * 0.12;
            accum.rgb += (1.0 - accum.a) * c.rgb * alpha;
            accum.a   += (1.0 - accum.a) * alpha;
        }
        if (accum.a > 0.99) break;
    }
    return accum;
}

void main() {
    if (pc.tintAltitude > 500.0) {
        vec2 uv = v_uv;
        if (pc.noRepeat < 0.5) uv = fract(uv);
        float cloud = sampleCloud(uv);
        if (cloud <= 0.01) discard;
        outColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec3 viewDir = normalize(v_viewDir);

    vec4 result = rayMarchSample(v_worldPos, viewDir);

    if (result.a <= 0.0) discard;

    // Debug test tint (independent-cloud altitude tests): blue = low altitude
    // (player level), red = high altitude (global-cloud level). Off when
    // tintAltitude < 0 (normal rendering, default).
    if (pc.tintAltitude >= 0.0) {
        if (pc.tintAltitude > 500.0) {
            result.rgb = vec3(1.0, 0.0, 1.0) * result.a;
        } else {
            float k = clamp(pc.tintAltitude, 0.0, 1.0);
            result.rgb = mix(vec3(0.15, 0.35, 1.0), vec3(1.0, 0.12, 0.05), k);
        }
    }

    outColor = result;
}
