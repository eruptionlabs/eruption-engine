#version 450

layout(location = 0) in vec2 fragUV;

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
layout(set = 2, binding = 0) uniform sampler2D sceneDepth;

layout(push_constant) uniform PushConstants {
    float layer;
    float shadowIntensity;
    float coveragePlaneHeight;
    float baseShadowStrength; // scene base shadow darkening: sun/(sun+ambient), baked on CPU
    float weatherCoverage;
    float noRepeat; // > 0.5: independent cloud layer, do not tile the UVs
} pc;

float sampleShadow(vec2 uv) {
    // Local clouds can drift outside the procedural world bounds; REPEAT sampler
    // plus fract() keeps the shadow valid everywhere. Independent cloud layers
    // (noRepeat) use a CLAMP_TO_BORDER sampler and must not tile.
    if (pc.noRepeat < 0.5) uv = fract(uv);
    // Clouds (including local clouds) may live on any layer, so accumulate the
    // maximum coverage across the whole array instead of sampling a single layer.
    float cov = 0.0;
    for (uint i = 0u; i < layerCount; ++i) {
        cov = max(cov, textureLod(cloudCoverageArray, vec3(uv, float(i)), 0.0).r);
    }
    // The coverage texture already contains the final global + local coverage.
    // Do not multiply by weatherCoverage here, or local clouds cast no shadow
    // when global cloud amount is zero.
    return cov;
}

float gaussianWeight(float r, float sigma) {
    return exp(-(r * r) / (2.0 * sigma * sigma));
}

void main() {
    float depth = texture(sceneDepth, fragUV).r;

    // Ignore sky pixels (far plane) so the shadow is not applied to the background.
    // With near=0.1 and far=50000, depth=0.9999 corresponds to only ~1000 units,
    // which clipped distant ground shadows when zooming out. Use a threshold much
    // closer to 1.0 so the shadow covers the full visible range.
    if (depth >= 0.999999) {
        outColor = vec4(0.0);
        return;
    }

    // Reconstruct world position from depth.
    vec2 ndcUV = fragUV * 2.0 - 1.0;
    vec4 clipPos = vec4(ndcUV, depth, 1.0);
    vec4 worldPosH = invViewProj * clipPos;
    if (abs(worldPosH.w) < 1e-6) worldPosH.w = 1e-6;
    vec3 worldPos = worldPosH.xyz / worldPosH.w;

    vec2 worldSize = worldMax.xz - worldMin.xz;

    // Height from the ground point up to the cloud plane.
    float heightFactor = max(pc.coveragePlaneHeight - worldPos.y, 0.0);

    // Project the shadow along the light direction: shadows shift away from the sun
    // and stretch more when the sun is low. Clamp the tangent to avoid runaway
    // offsets when the sun is near the horizon.
    // NOTE: sunDir in this UBO is the direction the light TRAVELS (points DOWN,
    // L.y < 0), so the L.y > 0 gate is never true and the shadow falls straight
    // down under the cloud. That is DELIBERATE for the low gameplay field
    // clouds (300-600 m): it keeps the shadow glued under its cloud — the look
    // the author validated. A real sun projection would displace the shadow
    // ~planeY * tan(el) (hundreds of meters), usually off the visible ground.
    vec3 L = normalize(sunDir);
    vec2 lightOffset = vec2(0.0);
    if (L.y > 0.01) {
        float tanAngle = min(length(L.xz) / L.y, 4.0);
        lightOffset = -normalize(L.xz + vec2(0.0001)) * heightFactor * tanAngle;
    }

    // UV on the cloud layer that casts the shadow over this ground point.
    // windOffset keeps the shadow locked to the drifting cloud layer.
    // The sampler is REPEAT, so the shadow wraps naturally with the drifting
    // clouds without creating seams or disappearing at the world bounds.
    vec2 baseUV = (worldPos.xz + lightOffset - worldMin.xz) / worldSize - windOffset.xz;

    // Penumbra radius grows with cloud altitude, but is capped so very high
    // clouds do not blur the shadow away completely.
    float sampleRadius = min(heightFactor * 0.0004, 0.0015);
    const int SAMPLE_COUNT = 16;

    // Gaussian-weighted sampling: closer samples contribute more.
    float gaussianSigma = max(sampleRadius, 0.0001);

    float shadow = 0.0;
    float weightSum = 0.0;

    float centerW = gaussianWeight(0.0, gaussianSigma);
    shadow += sampleShadow(baseUV) * centerW;
    weightSum += centerW;

    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        float angle = float(i) / float(SAMPLE_COUNT) * 6.283185307;
        vec2 dir = vec2(cos(angle), sin(angle));
        // Non-uniform radius so samples cluster near the center.
        float r = sampleRadius * (0.25 + 0.75 * fract(float(i) * 0.6180339887));
        vec2 offsetUV = dir * r / worldSize;
        float s = sampleShadow(baseUV + offsetUV);
        float w = gaussianWeight(r, gaussianSigma);
        shadow += s * w;
        weightSum += w;
    }
    shadow /= weightSum;

    // Shadow gain (hardcoded): the baked coverage is envelope * density *
    // noise, so the raw value rarely approaches 1.0 even at the cloud core
    // (thin clouds bake ~0.2-0.35, cloudy-weather wisps ~0.05-0.15) and
    // shadows read too faint. Boost before the data-driven opacity multiplier
    // (clamped to 1.0) so even thin clouds cast clearly visible shadows;
    // dense storm cores clamp and stay about as dark as before.
    shadow = min(shadow * 6.0, 1.0);

    // Half-strength floor (author feedback 2026-08-10): light/thin clouds
    // (pollen, light wind, partly cloudy) must still cast a readable shadow —
    // about half of the strongest (storm core = 1.0). Gated by the frame's
    // weather coverage: storm scenes (coverage >= 0.9) keep their raw
    // density variation, only light-coverage weather lifts thin clouds.
    float floorW = 1.0 - smoothstep(0.5, 0.9, pc.weatherCoverage);
    float presence = smoothstep(0.02, 0.10, shadow);
    shadow = max(shadow, presence * 0.5 * floorW);

    // Higher clouds cast softer shadows, but keep them visible even at 5 km.
    // Use a gentler curve so the shadow does not vanish under high clouds.
    float altitudeSoftening = 1.0 / (1.0 + heightFactor * 0.00005);

    // Shadow intensity = scene base shadow strength x cloud darkness (author
    // feedback 2026-08-10): a fully dense cloud darkens the ground like map
    // geometry shadows (base = sun/(sun+ambient), baked on CPU); thinner
    // clouds scale down linearly with their density. pc.shadowIntensity
    // (Cloud Shadow Opacity slider) stays as a global trim, default 1.0.
    float shadowAlpha = shadow * pc.baseShadowStrength * pc.shadowIntensity * altitudeSoftening;

    // Black shadow, alpha driven by coverage.
    outColor = vec4(0.0, 0.0, 0.0, shadowAlpha);
}
