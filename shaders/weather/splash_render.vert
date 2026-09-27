#version 450

layout(location = 0) in vec4 iOrigin;   // xyz = world position, w = seed
layout(location = 1) in vec4 iData;     // x = lifetime scale, y = phase, z = size, w = intensity

layout(location = 0) out vec2 oUV;
layout(location = 1) out float oAlpha;

layout(set = 0, binding = 2) uniform usampler2D heightmap;
layout(set = 0, binding = 3) uniform sampler2DArray cloudCoverageArray;
layout(set = 0, binding = 4) uniform sampler2D cloudAltitudeMap;

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 params; // x=time, y=rainIntensity, z=unused, w=screenAspect
    vec4 params2; // x=rainWind, y=rainSplashRadius, z=farPlane, w=fogDensity
    vec4 params5; // x=rainShadowSoftness, y=cloudBaseHeight, z=windOffsetX, w=windOffsetY
    vec4 screenSize;
    vec4 worldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeZ (rain heightmap)
    vec4 cloudWorldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeZ (cloud coverage)
    vec4 cloudParams; // x=layer, y=threshold, z=weatherCoverage, w=unused
    vec4 cloudRange; // x=cloudBottom, y=cloudTop, z=cloudTop-cloudBottom, w=unused
    vec4 cloudRangeCpp;  // C++ slot 10 (real cloudRange) — kept for layout, unused
    vec4 rainBoxCenter;  // C++ slot 11: per-cloud group center XZ
    vec4 occluder;       // C++ slot 12: z = cloud blob radius
} push;

const vec2 QUAD[4] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2(-1.0,  1.0),
    vec2( 1.0,  1.0)
);

void main() {
    vec2 quadUV = QUAD[gl_VertexIndex];
    oUV = quadUV * 0.5 + 0.5;

    // Debug viz mode (params2.z = -1): draw EVERY splash as a pure-red quad,
    // but through the EXACT same gates as the normal path (density mask,
    // heightmap, cloud altitude, camera-under-cloud, exposure) — the red on
    // the ground is precisely where splashes really appear, not the raw
    // spawn region. Only the look changes: bigger quad, alpha forced to 1.
    const bool viz = push.params2.z < 0.0;

    float t = push.params.x;
    float lifeScale = iData.x;
    float phase = iData.y;
    float size = iData.z;
    float intensity = iData.w;

    float localT = fract(t * lifeScale + phase);
    float age = localT;

    // Splash expands then fades (faster growth: the ring reaches readable
    // size while the alpha is still high)
    float currentSize = size * (0.25 + age * 1.9) * push.params2.y;
    float alpha = intensity * (1.0 - smoothstep(0.3, 1.0, age));
    if (viz) currentSize = 1.5; // debug: fixed readable size, gates unchanged

    // Sample the top-down heightmap to place the splash on the highest visible surface
    // Origin space (params.z): 0 = absolute world (legacy global group),
    // 1 = anchor-relative (suppress-mode global group; resolved against the
    // rain anchor carried in cameraPos), 2 = follower-relative (per-cloud
    // groups; resolved against the cloud's CURRENT rain-box center in
    // rainBoxCenter, refreshed per draw — the pattern drifts with its cloud).
    vec2 originBase = (push.params.z > 1.5) ? push.rainBoxCenter.xz
                    : (push.params.z > 0.5) ? push.cameraPos.xz
                    : vec2(0.0);
    vec2 worldXZ = iOrigin.xz + originBase;
    vec2 hmUV = (worldXZ - push.worldBounds.xy) / push.worldBounds.zw;
    float surfaceY = iOrigin.y;
    float maxY = surfaceY;
    bool valid = all(greaterThanEqual(hmUV, vec2(0.0))) && all(lessThanEqual(hmUV, vec2(1.0)));
    if (valid) {
        uint h = texture(heightmap, hmUV).r;
        maxY = uintBitsToFloat(h) - 10000.0;
        surfaceY = maxY;
    }

    // Cloud-altitude occlusion: splashes only exist below the local cloud top.
    if (push.cloudRange.z > 0.0) {
        vec2 altUV = (worldXZ - push.cloudWorldBounds.xy) / push.cloudWorldBounds.zw
                     - push.params5.zw;
        altUV = fract(altUV);
        float altitude = textureLod(cloudAltitudeMap, altUV, 0.0).r;
        float centerY = push.cloudRange.x + altitude * push.cloudRange.z;
        float halfThick = push.cloudRange.z * 0.12 / max(push.cloudRange.w * push.cloudRange.w, 0.1);
        float cloudTopY = centerY + halfThick;
        if (surfaceY > cloudTopY) {
            oAlpha = 0.0;
            gl_Position = push.viewProj * vec4(worldXZ.x, push.cameraPos.y, worldXZ.y, 1.0);
            return;
        }
    }

    // Kill splashes that fell outside the heightmap or over empty space
    if (!valid || surfaceY < -9000.0) {
        oAlpha = 0.0;
        gl_Position = push.viewProj * vec4(worldXZ.x, push.cameraPos.y, worldXZ.y, 1.0);
        return;
    }

    // Splashes only exist when the camera is beneath the cloud layer. Apply a
    // smooth vertical transition so the effect does not pop when flying through
    // the cloud base.
    float cloudBase = push.params5.y;
    float cameraUnderCloud = 1.0 - smoothstep(cloudBase - 15.0, cloudBase + 15.0, push.cameraPos.y);
    if (cameraUnderCloud < 0.001) {
        oAlpha = 0.0;
        gl_Position = push.viewProj * vec4(worldXZ.x, push.cameraPos.y, worldXZ.y, 1.0);
        return;
    }
    alpha *= cameraUnderCloud;

    // Splash density: when the camera is under the cloud layer the effect is
    // almost uniform across the screen (like being underwater); when above it
    // follows the dense-cloud mask.
    vec2 cloudWorldSize = push.cloudWorldBounds.zw;
    vec2 cloudUV = fract((worldXZ - push.cloudWorldBounds.xy) / cloudWorldSize - push.params5.zw);
    // The coverage texture is already thresholded by the Cloud Amount slider on CPU.
    float cov = textureLod(cloudCoverageArray, vec3(cloudUV, push.cloudParams.x), 0.0).r;
    cov = pow(cov, 3.0);
    float density = mix(cov, 0.85, cameraUnderCloud) * push.cloudParams.z;
    if (density < 0.15) {
        oAlpha = 0.0;
        gl_Position = push.viewProj * vec4(worldXZ.x, push.cameraPos.y, worldXZ.y, 1.0);
        return;
    }
    alpha *= density;

    // Smoothly fade splashes based on how exposed this surface is.
    // surfaceY was snapped to maxY, so exposure is ~1.0 for valid top surfaces.
    float softness = push.params5.x;
    float exposure = smoothstep(maxY - softness, maxY, surfaceY);
    if (exposure < 0.01) {
        oAlpha = 0.0;
        gl_Position = push.viewProj * vec4(worldXZ.x, push.cameraPos.y, worldXZ.y, 1.0);
        return;
    }
    alpha *= exposure;

    // Wind direction is along world X (rainWind parameter).
    vec2 windDir = vec2(push.params2.x, 0.0);

    // Billboard facing up (rain splash on ground/object top), leaned by wind.
    vec3 right = normalize(vec3(push.viewProj[0][0], push.viewProj[1][0], push.viewProj[2][0]));
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 pos = vec3(worldXZ.x, surfaceY + 0.02, worldXZ.y)
               + right * quadUV.x * currentSize
               + up * quadUV.y * currentSize * 0.3
               + vec3(windDir * currentSize * 0.3, 0.0);

    // Slight UV shear so the ripple looks angled in the wind.
    oUV.x += quadUV.y * windDir.x * 0.25;

    gl_Position = push.viewProj * vec4(pos, 1.0);
    oAlpha = viz ? 1.0 : alpha; // debug: binary — passed every gate = visible
}
