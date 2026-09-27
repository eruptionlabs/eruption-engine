#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SkyParamsUBO {
    vec4 skyTop;            // xyz = topColor, w = timeOfDay
    vec4 skyHorizon;        // xyz = horizonColor, w = starDensity
    vec4 sunDirIntensity;   // xyz = sunDir, w = sunIntensity
    vec4 moonDirIntensity;  // xyz = moonDir, w = moonIntensity
    vec4 sunColor;
    vec4 moonColor;
    vec4 proceduralParams;  // x=cloudCoverage, y=cloudSpeed, z=cloudScale, w=cloudLightness
    vec4 proceduralParams2; // x=cloudShade, y=sunRayCount, z=moonPhaseOffset, w=sunSize
    vec4 proceduralParams3; // x=moonSize, y=enableStars, z=enableClouds, w=starTwinkleSpeed
    vec4 proceduralParams4; // x=cloudSoftness, y=cloudThickness, z=stormTint, w=sunLimbDarkening
    vec4 proceduralParams5; // x=sunHaloIntensity, y=sunHaloRays, z=sunHaloSize, w=moonPhase
    vec4 proceduralParams6; // x=moonPhaseAuto, y=ERUPTION_TEST_SKY_GRADIENT_DEBUG flag
    mat4 invViewProj;
} ubo;

// Reconstruct world-space ray direction from screen UV.
// NOTE: Camera projection already does the Vulkan Y-flip, so no extra flip here.
vec3 getRayDir(vec2 uv) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc, 1.0, 1.0);
    vec4 worldPos = ubo.invViewProj * clipPos;
    worldPos.xyz /= worldPos.w;
    return normalize(worldPos.xyz - vec3(0.0));
}

// Hash functions
float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Value noise for clouds
float noise2D(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

float fbm(vec2 p) {
    float val = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        val += amp * noise2D(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return val;
}

// O(1) Stars with 3x3 neighbor check and twinkling
float getStars(vec2 uv, float threshold, float time, float twinkleSpeed) {
    vec2 id = floor(uv);
    vec2 gv = fract(uv) - 0.5;
    float stars = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 nid = id + neighbor;
            float n = hash21(nid);
            if (n < threshold) continue;
            vec2 offset = vec2(hash21(nid + 1.0), hash21(nid + 2.0)) - 0.5;
            float dist = length(gv - neighbor - offset * 0.8);
            float size = 0.025 * n + 0.008;
            float star = 1.0 - smoothstep(0.0, size, dist);
            // Twinkle
            float twinkle = sin(time * twinkleSpeed + hash21(nid + 3.0) * 6.28318) * 0.5 + 0.5;
            twinkle = mix(0.6, 1.0, twinkle);
            stars += star * twinkle;
        }
    }
    return clamp(stars, 0.0, 1.0);
}

// Camera-style sun disk with limb darkening, corona and diffraction spikes
vec3 getSun(vec3 rayDir, vec3 sunDir, float sunIntensity, vec3 sunColor, float sunSize,
            float rayCount, float limbDarkening, float haloIntensity, float haloRays, float haloSize) {
    float sunDot = dot(rayDir, sunDir);
    // sunSize=1.0 -> angularRadius ~ 0.997 (cos ~4.4 deg). Larger sunSize -> smaller angularRadius.
    float angularRadius = 1.0 - 0.003 * sunSize;
    float discAngle = acos(angularRadius);

    // Early out if we are far from any sun contribution
    float maxHaloAngle = discAngle * (4.0 + haloSize * 8.0);
    if (sunDot < cos(maxHaloAngle)) return vec3(0.0);

    // Normalised radial distance inside the disc (0 = center, 1 = edge)
    float r = clamp((1.0 - sunDot) / (1.0 - angularRadius), 0.0, 1.0);
    float mu = sqrt(1.0 - r * r); // cosine of emission angle

    // Stronger limb darkening: pow law so the edge gets visibly darker.
    // limbDarkening=0 -> uniform, limbDarkening=1 -> very dark edge.
    float ld = pow(max(mu, 0.02), limbDarkening * 3.0 + 0.4);

    // Very sharp disc edge so the dark limb is readable even at Sun Size=1
    float edgeWidth = 0.0003 * sunSize;
    float sunDisc = smoothstep(angularRadius, angularRadius + edgeWidth, sunDot) * ld;

    // Soft glow just outside the disc (corona base)
    float glowRadius = discAngle * (1.0 + 0.5 * haloSize);
    float innerGlow = smoothstep(angularRadius - edgeWidth * 2.0, angularRadius, sunDot) * 0.15;
    float outerGlow = smoothstep(cos(glowRadius), angularRadius, sunDot) * 0.25;

    // Cartoony radial rays (stylised corona)
    float stylisedRays = 0.0;
    if (rayCount > 0.5) {
        vec3 up = vec3(0.0, 1.0, 0.0);
        vec3 right = normalize(cross(up, sunDir));
        vec3 sunUp = cross(sunDir, right);
        float sx = dot(rayDir, right);
        float sy = dot(rayDir, sunUp);
        float angle = atan(sy, sx);
        stylisedRays = pow(abs(sin(angle * rayCount)), 8.0);
        float rayInner = angularRadius;
        float rayOuter = angularRadius - 0.08 * sunSize;
        stylisedRays *= smoothstep(rayOuter, rayInner, sunDot);
        stylisedRays *= 0.6;
    }

    // ---- Camera-style diffraction spikes (4/6/8 pointed star) ----
    float diffraction = 0.0;
    if (haloRays > 0.5) {
        vec3 up = vec3(0.0, 1.0, 0.0);
        vec3 right = normalize(cross(up, sunDir));
        vec3 sunUp = cross(sunDir, right);
        vec2 local = vec2(dot(rayDir, right), dot(rayDir, sunUp));
        float localAngle = atan(local.y, local.x);
        float localDist = length(local);

        // Thin spikes fading with distance from the sun center
        float spikes = pow(abs(sin(localAngle * haloRays * 0.5)), 16.0);
        float spikeFalloff = exp(-localDist * 12.0 / (discAngle * haloSize));
        diffraction = spikes * spikeFalloff * 0.8;
    }

    // ---- Corona rings (Mie + forward scattering approximation) ----
    float corona = 0.0;
    if (haloIntensity > 0.0) {
        float angleFromSun = acos(clamp(sunDot, 0.0, 1.0));
        float normAngle = angleFromSun / (discAngle * (1.0 + haloSize * 4.0));

        // Inner corona (tight)
        float innerCorona = exp(-normAngle * normAngle * 8.0);
        // Outer corona (broad)
        float outerCorona = exp(-normAngle * 2.0) * 0.5;
        // First corona ring
        float ring = exp(-pow(normAngle - 0.5, 2.0) * 32.0) * 0.3;

        corona = (innerCorona + outerCorona + ring) * haloIntensity;
    }

    float sunShape = sunDisc + innerGlow + outerGlow + stylisedRays;
    sunShape = clamp(sunShape, 0.0, 1.0);

    vec3 sunLit = sunColor * sunIntensity * sunShape;
    vec3 haloLit = sunColor * sunIntensity * (corona + diffraction);

    return sunLit + haloLit;
}

// Realistic Moon disk with spherical shading and terminator
// phase: 0.0 = new, 0.5 = full, 1.0 = new (wrapped)
vec3 getMoon(vec3 rayDir, vec3 moonDir, vec3 sunDir, float moonIntensity, vec3 moonColor,
             float moonSize, float phase) {
    float moonDot = dot(rayDir, moonDir);
    // moonSize=1.0 -> angularRadius ~ 0.996 (cos ~5.1 deg). Larger moonSize -> smaller angularRadius.
    float angularRadius = 1.0 - 0.004 * moonSize;
    float glowRadius = max(0.06, 0.08 * moonSize);
    if (moonDot < angularRadius - glowRadius) return vec3(0.0);

    float moonBase = smoothstep(angularRadius, 1.0, moonDot);

    // Build a local frame around the moon
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 moonRight = normalize(cross(up, moonDir));
    vec3 moonUp = cross(moonDir, moonRight);

    // Project ray onto moon disc local coordinates
    vec2 discPos = vec2(dot(rayDir, moonRight), dot(rayDir, moonUp));
    float discLen = length(discPos);
    if (discLen < 0.001) discLen = 0.001;

    // Phase determines the direction of the light source relative to the moon
    // phase=0.5 (full) -> light comes from viewer direction (moonDir)
    // phase=0.0/1.0 (new) -> light comes from behind the moon (-moonDir)
    // We approximate the terminator as a half-plane offset along the light direction
    float phaseAngle = (phase - 0.5) * 3.14159265; // -pi/2 .. +pi/2
    vec2 lightDir = vec2(sin(phaseAngle), 0.0);

    // Terminator position on the disc [-1, 1], scaled by disc radius
    float terminator = dot(normalize(discPos), lightDir);
    float shadow = smoothstep(-0.15, 0.15, terminator);

    // When phase is near 0 or 1 (new moon), the whole disc is in shadow
    float litFraction = 1.0 - 2.0 * abs(phase - 0.5);
    litFraction = clamp(litFraction, 0.0, 1.0);

    // Combine disc, shadow terminator and phase falloff
    float moonLit = moonBase * mix(0.05, shadow, litFraction);

    // Earthshine: faint illumination of the dark side
    float earthshine = moonBase * 0.04 * (1.0 - litFraction);

    // Soft glow around the moon
    float moonGlow = smoothstep(angularRadius - 0.008, angularRadius, moonDot) * 0.25;

    return moonColor * moonIntensity * (moonLit + earthshine + moonGlow);
}

void main() {
    vec3 rayDir = getRayDir(fragUV);

    vec3 sunDir = normalize(ubo.sunDirIntensity.xyz);
    vec3 moonDir = normalize(ubo.moonDirIntensity.xyz);
    float sunIntensity = ubo.sunDirIntensity.w;
    float moonIntensity = ubo.moonDirIntensity.w;
    float sunHeight = sunDir.y;
    float timeOfDay = ubo.skyTop.w;
    float time = timeOfDay * 200.0;

    float cloudCoverage = ubo.proceduralParams.x;
    float cloudSpeed = ubo.proceduralParams.y;
    float cloudScale = ubo.proceduralParams.z;
    float cloudLightness = ubo.proceduralParams.w;
    float cloudShade = ubo.proceduralParams2.x;
    float sunRayCount = ubo.proceduralParams2.y;
    float sunSize = ubo.proceduralParams2.w;
    float moonSize = ubo.proceduralParams3.x;
    bool enableStars = ubo.proceduralParams3.y > 0.5;
    bool enableClouds = ubo.proceduralParams3.z > 0.5;
    float starTwinkleSpeed = ubo.proceduralParams3.w;
    float cloudSoftness = ubo.proceduralParams4.x;
    float cloudThickness = ubo.proceduralParams4.y;
    float stormTint = ubo.proceduralParams4.z;
    float sunLimbDarkening = ubo.proceduralParams4.w;
    float sunHaloIntensity = ubo.proceduralParams5.x;
    float sunHaloRays = ubo.proceduralParams5.y;
    float sunHaloSize = ubo.proceduralParams5.z;
    float moonPhase = ubo.proceduralParams5.w;

    // ---- Atmospheric Gradient (Stylized) ----
    float viewY = rayDir.y;
    float dayBlend = smoothstep(-0.15, 0.25, sunHeight);

    vec3 daySky = mix(ubo.skyHorizon.rgb, ubo.skyTop.rgb, smoothstep(0.0, 0.4, viewY));

    vec3 nightTop = vec3(0.01, 0.01, 0.05);
    vec3 nightHorizon = vec3(0.04, 0.03, 0.08);
    vec3 nightSky = mix(nightHorizon, nightTop, smoothstep(0.0, 0.4, viewY));

    vec3 skyColor = mix(nightSky, daySky, dayBlend);

    // Golden / Twilight hour boost
    float goldenHour = (1.0 - abs(sunHeight)) * smoothstep(0.0, 0.3, sunHeight);
    goldenHour = smoothstep(0.1, 0.35, goldenHour);
    vec3 goldenColor = vec3(1.0, 0.55, 0.2);
    skyColor = mix(skyColor, goldenColor, goldenHour * smoothstep(-0.2, 0.3, viewY) * 0.35);

    // Deep twilight purple near horizon when sun is just below
    float twilight = smoothstep(-0.3, -0.05, sunHeight) * (1.0 - smoothstep(-0.05, 0.1, sunHeight));
    vec3 twilightColor = vec3(0.25, 0.15, 0.35);
    skyColor = mix(skyColor, twilightColor, twilight * smoothstep(-0.1, 0.2, viewY) * 0.25);

    // ---- Sun ----
    // Nada de sol abaixo do horizonte: disco, coroa e raios de difracao somem
    // junto com ele. O arco do astro ja' mergulha (DayNightCycle), mas o ceu
    // continua visivel abaixo da linha do horizonte sempre que a geometria nao
    // cobre - beira de penhasco, mar - e sem este corte o disco reapareceria la'.
    float sunAboveHorizon = smoothstep(-0.06, 0.01, sunHeight);
    skyColor += getSun(rayDir, sunDir, sunIntensity, ubo.sunColor.rgb, sunSize,
                       sunRayCount, sunLimbDarkening, sunHaloIntensity, sunHaloRays,
                       sunHaloSize) * sunAboveHorizon;

    // ---- Moon ----
    float moonAboveHorizon = smoothstep(-0.06, 0.01, moonDir.y);
    skyColor += getMoon(rayDir, moonDir, sunDir, moonIntensity, ubo.moonColor.rgb,
                        moonSize, moonPhase) * moonAboveHorizon;

    // ---- Stars (O(1) Domain Repetition) ----
    if (enableStars) {
        vec2 starUV = vec2(atan(rayDir.z, rayDir.x), asin(clamp(rayDir.y, -1.0, 1.0)));
        starUV *= vec2(6.0, 12.0);

        float starDensity = ubo.skyHorizon.w;
        float starThreshold = 1.0 - starDensity;
        float stars = getStars(starUV, starThreshold, time, starTwinkleSpeed);
        float starFade = 1.0 - smoothstep(-0.2, 0.1, sunHeight);
        skyColor += vec3(0.9, 0.95, 1.0) * stars * starFade;
    }

    // ---- Clouds (Stylized FBM) ----
    if (enableClouds) {
        // Spherical-ish UV that stays stable near horizon
        vec2 cloudUV = vec2(atan(rayDir.z, rayDir.x), asin(clamp(rayDir.y, -1.0, 1.0)));
        cloudUV *= vec2(2.0, 4.0);
        cloudUV += vec2(time * cloudSpeed, time * cloudSpeed * 0.25);

        // Multi-octave FBM
        float cloudNoise = fbm(cloudUV * cloudScale);

        // Coverage remap: higher coverage = lower threshold = more clouds
        // fbm output centers around 0.5; tune threshold so coverage varies 0..1
        float threshold = mix(0.62, 0.40, cloudCoverage);
        float edgeWidth = mix(0.02, 0.12, cloudSoftness);
        float edge0 = threshold - edgeWidth;
        float edge1 = threshold + edgeWidth;

        // Billowy cloud shapes via smoothstep + pow
        float cloudMask = smoothstep(edge0, edge1, cloudNoise);
        cloudMask = pow(cloudMask, 0.7); // fluffier edges

        // Shading based on noise gradient approximation
        float cloudShadeVal = smoothstep(edge0, edge1, cloudNoise);

        // Clouds darken the sky, with bright illuminated tops
        float cloudSun = smoothstep(-0.2, 0.5, sunHeight);
        vec3 cloudBright = mix(vec3(0.75, 0.78, 0.85), vec3(1.0, 1.0, 1.0), cloudSun);
        vec3 cloudBase = skyColor * mix(0.55, 0.75, cloudSun);
        vec3 cloudColor = mix(cloudBase, cloudBright, cloudShadeVal);
        cloudColor = mix(cloudColor, cloudBase, cloudShade * 0.5);

        vec3 stormMood = mix(vec3(0.35, 0.35, 0.40), vec3(0.15, 0.15, 0.20), cloudSun);
        cloudColor = mix(cloudColor, stormMood, stormTint * 0.7);

        cloudColor *= cloudLightness;

        float fadeStart = mix(0.0, -0.18, cloudCoverage);
        float horizonFade = smoothstep(fadeStart, mix(0.05, 0.30, cloudThickness), viewY);

        // Darken the sky behind clouds for readable silhouettes
        skyColor *= 1.0 - cloudMask * 0.35 * horizonFade;
        skyColor = mix(skyColor, cloudColor, cloudMask * 0.95 * horizonFade);
    }

    float groundFactor = smoothstep(-0.02, -0.35, viewY);
    vec3 groundColor = ubo.skyHorizon.rgb * mix(0.12, 0.05, cloudCoverage);
    skyColor = mix(skyColor, groundColor, groundFactor);

    // ERUPTION_TEST_SKY_GRADIENT_DEBUG=1: amplified diff from horizon colour.
    if (ubo.proceduralParams6.y > 0.5) {
        vec3 diff = abs(skyColor - ubo.skyHorizon.rgb) * 8.0;
        outColor = vec4(clamp(diff, 0.0, 1.0), 1.0);
        return;
    }

    // Soft tone-map so clouds don't blow out against bright sky
    skyColor = skyColor / (1.0 + skyColor * 0.5);

    outColor = vec4(skyColor, 1.0);
}
