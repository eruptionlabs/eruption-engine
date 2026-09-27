#version 450

layout(location = 0) in vec2 iUV;
layout(location = 1) flat in float iType;
layout(location = 2) in float iAlpha;
layout(location = 3) in float iDepth;
layout(location = 4) in vec3 iWorldPos;
layout(location = 5) in float iReason; // debug occlusion reason from the vertex shader

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2DArray cloudCoverageArray; // owning cloud's coverage (followers)
layout(set = 0, binding = 4) uniform sampler2D cloudAltitudeMap;

// Cross-cloud rain occluders: every rainy cloud's blob + coverage mapping,
// so a drop is hidden by ANY cloud between it and the camera, tested against
// that cloud's EXACT coverage silhouette (same cov > 0.10 gate the owning
// cloud uses), so rain still shows through a cloud's transparent regions.
layout(set = 0, binding = 5) uniform RainOccluders {
    vec4 geom[100];   // xy = blob center XZ, z = cloud planeY, w = blob radius
    vec4 bounds[100]; // xy = coverage world min XZ, zw = coverage world size XZ
    vec4 wind[100];   // xy = coverage wind offset (UV)
} occ;
layout(set = 0, binding = 6) uniform sampler2DArray covArrays[100];

// Rain streak shape tuning (same UBO as the vertex stage).
layout(set = 0, binding = 7) uniform RainTuning {
    vec4 shape; // x=widthScale, y=lengthScale, z=edgeSharp, w=tipFade
    vec4 look;  // x=alphaGain, y=brightness, z=pixelFade (px, 0=off), w=unused
} tune;

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 params;  // x=time, y=rainIntensity, z=snowIntensity, w=screenAspect
    vec4 params2; // x=rainWind, y=rainSplashRadius, z=farPlane, w=fogDensity
    vec4 params5; // x=rainShadowSoftness, y=cloudBaseHeight, z=windOffsetX, w=windOffsetY
    vec4 screenSize;
    vec4 worldBounds; // rain heightmap bounds
    vec4 cloudWorldBounds; // cloud coverage bounds
    vec4 cloudParams; // x=layer, y=threshold, z=weatherCoverage, w=unused
    vec4 sunDir;
    vec4 cloudRange; // x=cloudBottom (follower: cloud plane height); particle vert reads yw as follower box XZ extents
    vec4 rainBoxCenter; // xyz = fixed world-space center of the rain box, w = 0 (global) / 1 (per-cloud follower box)
    vec4 occluder; // xy = cloud blob center XZ (followers), z = blob radius (<=0 = infinite plane), w = debug occlusion flag
} push;

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p;
    return fract(p);
}

void main() {
    int ptype = int(iType + 0.5);
    bool isRain = ptype == 0;
    bool isSnow = ptype == 1;
    bool isHail = ptype == 2;
    bool isFreezingRain = ptype == 3;
    bool isSand = ptype == 4;
    bool isDust = ptype == 5;
    bool isPrecip = isRain || isHail || isFreezingRain;
    // Sand/dust are NOT cloud-bound: the lateral wind storm exists with or
    // without clouds, so every cloud-based cull below (plane cull, cross-cloud
    // occlusion, ceiling, from-above depth, follower rain mask) is skipped for
    // them. They keep only the scene-depth occlusion and the vertex-side
    // terrain/wind-shadow tests (author feedback 2026-08-10: debug showed the
    // whole dust field painted blue/orange = killed by cloud tests).
    bool isParticle = isSand || isDust;

    vec2 uv = iUV;
    float alpha = iAlpha;

    bool debugOcclusion = false;
    bool debugRainRegions = false;
    float occVal = push.occluder.w;
    if (occVal >= 9.5) {
        debugRainRegions = true;
        occVal -= 10.0;
    }
    debugOcclusion = occVal > 0.5 && occVal < 2.0;
    float reason = iReason; // 0 = alive (vertex-side reasons 1..3 already set)
    float hitCov = 0.0; // max cloud coverage this drop overlaps (debug tint)

    // Soft depth occlusion: hide precipitation behind scene geometry
    // (Camera > Wall > Rain). One depth fetch per fragment; scene depth is
    // linearized and compared against the particle's view-space depth, with a
    // short softness band so intersections don't clip harshly.
    {
        vec2 screenUV = gl_FragCoord.xy * push.screenSize.zw;
        float rawDepth = texture(depthTex, screenUV).r;
        if (rawDepth < 0.999999) { // 1.0 = sky/far: nothing to occlude
            float near = push.cloudParams.w;
            float sceneZ = near * push.params2.z / (push.params2.z - rawDepth * (push.params2.z - near));
            float dz = iDepth - sceneZ;
            if (dz > 0.0) {
                alpha *= 1.0 - smoothstep(0.3, 2.0, dz); // softness band in meters
                if (alpha <= 0.001) {
                    if (debugOcclusion) { if (reason < 0.5) reason = 4.0; alpha = iAlpha; }
                    else discard;
                }
            }
        }
    }

    // Treat the cloud as an opaque horizontal plane at cloudBaseHeight. This
    // opaque-plane cull applies ONLY to the legacy global draw (rainBoxCenter.w
    // == 0, the 2D-era infinite cloud plane): a camera above that plane sees no
    // rain. Per-cloud rain followers (rainBoxCenter.w == 1) are EXEMPT — a rainy
    // cloud must visibly rain ALWAYS, even when the camera is above its deck:
    // the cloud volume is translucent fluff, and hiding the rain falling under
    // it made the whole rainy weather read as "no rain" from the gameplay
    // (top-down-ish) camera. Rain hidden behind OTHER clouds is still handled
    // by the cross-cloud loop below.
    float cloudBase = push.params5.y;
    vec2 occCenter = vec2(0.0);
    float occRadius = 0.0;
    bool followerBox = push.rainBoxCenter.w > 0.5;
    if (followerBox) {
        cloudBase = push.cloudRange.x;
        occCenter = push.occluder.xy;
        occRadius = push.occluder.z;
    }
    float rayDy = iWorldPos.y - push.cameraPos.y;
    if (!isParticle && abs(rayDy) > 0.001) {
        float t = (cloudBase - push.cameraPos.y) / rayDy;
        // Debug overlap viz: a crossing BEYOND the drop (t >= 1, cloud behind
        // the drop on screen — the dominant top-down/pitch-45 case) still
        // counts for the red density tint. Culling stays t < 1.
        bool beyondDrop = t >= 1.0;
        if (t > 0.0 && (!beyondDrop || debugOcclusion)) {
            vec2 crossXZ = push.cameraPos.xz + t * (iWorldPos.xz - push.cameraPos.xz);
            bool blocked = false;
            if (followerBox) {
                // Followers never discard on their own deck (see the comment
                // above). The coverage fetch remains only to feed the debug
                // overlap tint (hitCov), so skip it in normal rendering.
                if (debugOcclusion && occRadius > 0.0 &&
                    push.cloudWorldBounds.z > 0.0 && push.cloudWorldBounds.w > 0.0 &&
                    distance(crossXZ, occCenter) <= occRadius) {
                    vec2 covUV = (crossXZ - push.cloudWorldBounds.xy) / push.cloudWorldBounds.zw
                                 - push.params5.zw;
                    float cov = textureLod(cloudCoverageArray, vec3(covUV, 0.0), 0.0).r;
                    hitCov = max(hitCov, cov);
                }
            } else {
                blocked = true; // global weather: infinite cloud plane (legacy)
            }
            if (!beyondDrop && blocked) {
                if (debugOcclusion) { if (reason < 0.5) reason = 5.0; }
                else discard;
            }
        }
    }
    // Cross-cloud occlusion: this drop is also hidden by any OTHER rainy cloud
    // whose plane the camera->drop ray crosses inside that cloud's OPAQUE
    // coverage (exact silhouette, same 0.10 gate as the owning cloud — thin
    // transparent regions still let the rain through).
    if (!isParticle) {
        int ownIndex = int(floor(push.cloudParams.y + 0.5));
        int occCount = int(floor(push.cloudParams.z + 0.5));
        if (occCount > 0 && abs(rayDy) > 0.001) {
            for (int j = 0; j < occCount; ++j) {
                if (j == ownIndex) continue;
                vec4 o = occ.geom[j];
                if (o.w <= 0.0) continue;
                float t = (o.z - push.cameraPos.y) / rayDy;
                if (t <= 0.0) continue;
                bool beyondDrop = t >= 1.0; // cloud behind the drop: overlap viz only
                if (beyondDrop && !debugOcclusion) continue; // not between camera and drop
                vec2 crossXZ = push.cameraPos.xz + t * (iWorldPos.xz - push.cameraPos.xz);
                if (distance(crossXZ, o.xy) > o.w) continue; // outside the blob footprint
                vec4 b = occ.bounds[j];
                if (b.z > 0.0 && b.w > 0.0) {
                    vec2 covUV = (crossXZ - b.xy) / b.zw - occ.wind[j].xy;
                    if (covUV.x < 0.0 || covUV.x > 1.0 || covUV.y < 0.0 || covUV.y > 1.0)
                        continue; // outside the cloud's texture: no cloud there
                    float cov = textureLod(covArrays[j], vec3(covUV, 0.0), 0.0).r;
                    hitCov = max(hitCov, cov);
                    if (beyondDrop) continue;
                    // Attenuate (not discard) by the occluding cloud's coverage:
                    // a wispy translucent region only dims the rain, exactly
                    // like the cloud's own volume would; only a near-opaque
                    // core hides the drop. Gain 1.75 mirrors the cloud shadow.
                    alpha *= 1.0 - clamp(cov * 1.75, 0.0, 1.0);
                    if (alpha <= 0.003) {
                        if (debugOcclusion) { if (reason < 0.5) reason = 5.0; }
                        else discard;
                    }
                } else {
                    // No coverage available: fall back to the blob disc.
                    if (debugOcclusion) { if (reason < 0.5) reason = 5.0; }
                    else discard;
                }
            }
        }
    }

    // Debug overlap viz vs the cloud BODY: the plane tests above only see the
    // deck, and their crossing extrapolation is wind/view dependent (looking
    // downwind at slanted rain throws the crossing point far outside the
    // cloud texture — the yaw-180° asymmetry). The billboard volume hangs in
    // a slab above the plane, so test the ray against the blob's vertical
    // cylinder and sample coverage where the ray is inside it. Yaw-neutral:
    // the cylinder surrounds the cloud instead of extrapolating past it.
    if (!isParticle && debugOcclusion) {
        vec3 O = push.cameraPos.xyz;
        vec3 D = iWorldPos - O;
        int ownIndex2 = int(floor(push.cloudParams.y + 0.5));
        int occCount2 = int(floor(push.cloudParams.z + 0.5));
        for (int j = -1; j < occCount2; ++j) {
            // j == -1: the owning cloud (push constants); else UBO occluders.
            vec2 C; float planeY; float radius; vec4 b; vec2 windOff;
            if (j < 0) {
                if (!followerBox) break;
                C = occCenter; planeY = cloudBase; radius = occRadius;
                b = push.cloudWorldBounds; windOff = push.params5.zw;
            } else {
                if (j == ownIndex2) continue;
                vec4 o = occ.geom[j];
                if (o.w <= 0.0) continue;
                C = o.xy; planeY = o.z; radius = o.w;
                b = occ.bounds[j]; windOff = occ.wind[j].xy;
            }
            if (radius <= 0.0 || b.z <= 0.0 || b.w <= 0.0) continue;
            // 2D ray-circle in XZ (param t along D: point = O + t*D).
            vec2 d2 = D.xz, o2 = O.xz - C;
            float qa = dot(d2, d2);
            if (qa < 1e-6) continue;
            float qb = dot(o2, d2);
            float qc = dot(o2, o2) - radius * radius;
            float disc = qb * qb - qa * qc;
            if (disc <= 0.0) continue;
            float sq = sqrt(disc);
            float t0 = (-qb - sq) / qa, t1 = (-qb + sq) / qa;
            // Slab the body occupies above the plane (approximation of the
            // billboard box height; the exact puffTop lives on the CPU).
            const float bodyH = 150.0;
            if (abs(D.y) > 1e-6) {
                float ty0 = (planeY - O.y) / D.y, ty1 = (planeY + bodyH - O.y) / D.y;
                t0 = max(t0, min(ty0, ty1)); t1 = min(t1, max(ty0, ty1));
            }
            if (t1 <= max(t0, 0.0)) continue;
            float tm = clamp((max(t0, 0.0) + t1) * 0.5, 0.0, 2.0);
            vec2 pXZ = O.xz + tm * D.xz;
            vec2 covUV = (pXZ - b.xy) / b.zw - windOff;
            if (covUV.x < 0.0 || covUV.x > 1.0 || covUV.y < 0.0 || covUV.y > 1.0) continue;
            float cov = (j < 0) ? textureLod(cloudCoverageArray, vec3(covUV, 0.0), 0.0).r
                                : textureLod(covArrays[j], vec3(covUV, 0.0), 0.0).r;
            hitCov = max(hitCov, cov);
        }
    }

    // Also enforce the physical ceiling: precipitation never exists above clouds.
    if (!isParticle && iWorldPos.y > cloudBase &&
        (occRadius <= 0.0 || distance(iWorldPos.xz, occCenter) <= occRadius)) {
        if (debugOcclusion) { if (reason < 0.5) reason = 6.0; }
        else discard;
    }

    // Cloud depth occlusion (from above): when the camera is above the cloud plane,
    // any rain drops underneath the cloud footprint (where the Magenta billboard is rendered)
    // are occluded by the cloud and must be discarded. Legacy global draw ONLY:
    // per-cloud followers are exempt — their rain must stay visible under the
    // cloud even when the camera looks down from above the deck (same rule as
    // the own-deck cull above), otherwise the whole rainy field reads as dry
    // from the gameplay camera.
    if (!isParticle && !followerBox && push.cameraPos.y > cloudBase && iWorldPos.y < cloudBase) {
        float t = (cloudBase - push.cameraPos.y) / (iWorldPos.y - push.cameraPos.y);
        vec3 pIntersect = push.cameraPos.xyz + t * (iWorldPos - push.cameraPos.xyz);
        if (push.cloudWorldBounds.z > 0.0 && push.cloudWorldBounds.w > 0.0) {
            vec2 covUV = (pIntersect.xz - push.cloudWorldBounds.xy) / push.cloudWorldBounds.zw
                         - push.params5.zw;
            if (covUV.x >= 0.0 && covUV.x <= 1.0 && covUV.y >= 0.0 && covUV.y <= 1.0) {
                float cov = textureLod(cloudCoverageArray, vec3(covUV, 0.0), 0.0).r;
                if (cov > 0.01) {
                    if (debugOcclusion) { if (reason < 0.5) reason = 7.0; }
                    else discard;
                }
            }
        }
    }

    // Rain only falls UNDER the owning cloud (its shadow footprint). Sample the
    // cloud's real coverage at the drop's own XZ (vertical column, no sun shift
    // — rain falls straight down). In the lit region (1 - shadow, coverage below
    // the cloud threshold) the rain is invisible: same fate as the debug-blue
    // occluded case (reason 5). Feathered edge so the boundary doesn't pop.
    if (!isParticle && followerBox && push.cloudWorldBounds.z > 0.0 && push.cloudWorldBounds.w > 0.0) {
        vec2 rainCovUV = (iWorldPos.xz - push.cloudWorldBounds.xy) / push.cloudWorldBounds.zw
                         - push.params5.zw;
        float cov = 0.0;
        if (rainCovUV.x >= 0.0 && rainCovUV.x <= 1.0 && rainCovUV.y >= 0.0 && rainCovUV.y <= 1.0) {
            // 5-tap cross blur (~4 m radius; the coverage texture has no
            // mips): full-res coverage has meter-scale holes and drops
            // drifting through them popped in/out every few meters, which
            // read as shimmering rain inside the footprint. The blurred
            // mask keeps the silhouette but kills the high-frequency popping.
            vec2 uvR = 4.0 / push.cloudWorldBounds.zw;
            cov = textureLod(cloudCoverageArray, vec3(rainCovUV, 0.0), 0.0).r * 0.4
                + textureLod(cloudCoverageArray, vec3(rainCovUV + vec2(uvR.x, 0.0), 0.0), 0.0).r * 0.15
                + textureLod(cloudCoverageArray, vec3(rainCovUV - vec2(uvR.x, 0.0), 0.0), 0.0).r * 0.15
                + textureLod(cloudCoverageArray, vec3(rainCovUV + vec2(0.0, uvR.y), 0.0), 0.0).r * 0.15
                + textureLod(cloudCoverageArray, vec3(rainCovUV - vec2(0.0, uvR.y), 0.0), 0.0).r * 0.15;
            hitCov = max(hitCov, cov);
        }
        // Low threshold aligned with the cloud shadow: small/dilute clouds have
        // low peak coverage (maxCov ~0.1-0.7 = v*density*noise), so the old
        // 0.25 gate killed the rain of 3 of the 4 test clouds entirely. Gate at
        // the shadow's visible edge so rain covers the cloud's full footprint.
        float rainMask = smoothstep(0.0, 0.10, cov); // 0 = lit (1-shadow), 1 = under cloud
        // Small clouds (drizzle: ~1% of the layer's coverage texture) bake so
        // few texels that cov stays under the gate EVERYWHERE and the cloud
        // never rains (author feedback 2026-08-09: "drizzle sem chuva vinda
        // das nuvens"). Union with the rain-box footprint — the same region
        // the Debug Rain Regions button shows and the crown splash gates on —
        // so every rainy cloud rains under its whole box; dense-cloud pixels
        // keep the exact silhouette via the max().
        if (followerBox) {
            vec2 boxD = abs(iWorldPos.xz - push.rainBoxCenter.xz) / max(push.cloudRange.yw * 0.5, vec2(1.0));
            float boxMask = 1.0 - smoothstep(0.85, 1.0, max(boxD.x, boxD.y));
            rainMask = max(rainMask, boxMask * 0.85);
        }
        if (rainMask <= 0.001) {
            if (debugOcclusion) { if (reason < 0.5) reason = 5.0; }
            else discard;
        }
        alpha *= rainMask;
    }

    // Debug occlusion map (estilo DoF visualizeCoC): color every particle by
    // its culling fate instead of discarding it.
    if (debugOcclusion) {
        vec3 dc = reason < 0.5 ? vec3(0.15, 0.9, 0.2)   // alive / visible
                : reason < 1.5 ? vec3(0.2, 0.85, 1.0)   // camera above cloud base (vertex)
                : reason < 2.5 ? vec3(1.0, 0.9, 0.1)    // below surface heightmap (vertex)
                : reason < 3.5 ? vec3(1.0, 0.15, 0.9)   // outside cloud shadow mask (vertex)
                : reason < 4.5 ? vec3(1.0, 0.15, 0.1)   // behind scene depth (fragment)
                : reason < 5.5 ? vec3(0.15, 0.25, 1.0)  // cloud plane blocks ray (fragment)
                : reason < 6.5 ? vec3(1.0, 0.55, 0.05)  // above cloud base (fragment)
                :                vec3(0.6, 0.1, 0.8);   // occluded by cloud from above (fragment)
        // Alive drops overlapping cloud: blend green -> red by the MAX coverage
        // the drop touches (own column, own/cross ray-plane crossing) — the
        // "conta de vezes": dense = red, thin = barely tinted, no cloud = the
        // same green as before.
        if (reason < 0.5 && hitCov > 0.0)
            dc = mix(dc, vec3(1.0, 0.05, 0.05), clamp(hitCov * 2.0, 0.0, 1.0));
        // Additive pipeline (SRC_ALPHA, ONE): boost enough for the tint to
        // read over bright backgrounds, but cap the alpha so dense overlap
        // accumulates to the tint color instead of blowing out to white.
        outColor = vec4(dc * 1.2, debugRainRegions ? 0.15 : 0.35);
        return;
    }

    // Fade distant particles with fog. Precipitation/snow get a floor so
    // far-away drops/flakes stay faintly visible instead of dying completely
    // in the haze (author request 2026-08-10: particles must remain visible
    // from a high top-down camera, zoom 0 ≈ 1000 m). Sand/dust also get a
    // floor (0.30): the storm's own fogDensity would otherwise eat the far
    // half of the box and expose its edge (author feedback 2026-08-11).
    float fogFactor = 1.0 - exp(-iDepth * push.params2.w * 0.01);
    float fogFade = 1.0 - fogFactor;
    if (isPrecip || isSnow) fogFade = max(fogFade, 0.35);
    else if (isSand || isDust) fogFade = max(fogFade, 0.30);
    alpha *= fogFade;

    if (isPrecip) {
        vec3 color;
        if (isHail) {
            color = vec3(0.88, 0.91, 0.95);
        } else if (isFreezingRain) {
            color = vec3(0.70, 0.80, 0.92);
        } else {
            color = vec3(0.78, 0.81, 0.85);
        }
        if (debugRainRegions) {
            outColor = vec4(color * 0.5, 0.15);
        } else {
            // Streak: bright center, fade at edges. Edge/tip falloffs come
            // straight from the per-type preset (no extra multipliers).
            float edge = tune.shape.z;
            float tip = tune.shape.w;
            float streak = 1.0 - abs(uv.x - 0.5) * edge;
            streak *= 1.0 - abs(uv.y - 0.5) * tip;
            streak = max(streak, 0.0);
            alpha *= streak * push.params.y * tune.look.x;
            if (isHail) {
                alpha *= 1.6;
            } else if (isFreezingRain) {
                alpha *= 1.2;
            }
            outColor = vec4(color * alpha * tune.look.y, alpha);
        }
    } else if (isSand || isDust) {
        // Sand/dust grain with depth layering. Larger, softer grains in the
        // distance fade out to create a volumetric dust wall instead of a
        // screen-space PNG overlay.
        vec2 center = uv - 0.5;
        float r = length(center);
        // Irregular fluffy shape via noise-like radial falloff.
        float grain = 1.0 - smoothstep(0.0, 0.5, r + (sin(atan(center.y, center.x) * 5.0 + iDepth) * 0.03));
        grain *= 0.85 + 0.15 * sin(push.params.x * 2.0 + iDepth);
        alpha *= grain;

        // Depth fade: near grains are opaque, far grains dissolve into haze.
        float depthFade = 1.0 - smoothstep(30.0, 800.0, iDepth);
        alpha *= depthFade;

        // (The old extra fog fade was removed: in a real sandstorm the global
        // fogDensity is huge BECAUSE of the dust, so alpha *= 1-fogFactor
        // killed the grains with their own storm's haze — depthFade above
        // already dissolves the distance, author report 2026-08-10.)

        // Sunlit dust motes: pale desaturated tan reads as dust, not embers
        // (author feedback 2026-08-11: "a areia tá brilhando" — the old
        // saturated gold + high gain blew out to sparks; the first matte
        // attempt went too dark and vanished against the haze).
        vec3 sandColor = isSand ? vec3(0.75, 0.68, 0.55) : vec3(0.68, 0.62, 0.52);
        // Darken distant dust so it silhouettes against the sky.
        sandColor *= mix(0.55, 1.0, depthFade);
        outColor = vec4(sandColor * alpha * 2.2, alpha);
    } else {
        // Snow: hail-style bright pellet (author request 2026-08-10 — the old
        // big soft flake read as a blurry dust smudge on screen). Hard round
        // core + the streak tune look (x = alpha gain, y = brightness), so
        // each snow weather presets its own intensity: blizzard glows harder
        // than hail, light snow stays a gentle sprinkle.
        vec2 center = uv - 0.5;
        float r = length(center);
        float flake = 1.0 - smoothstep(0.10, 0.42, r);
        flake *= 1.0 + 0.3 * sin(push.params.x * 3.0 + iDepth);
        alpha *= flake * push.params.z * tune.look.x;
        // (Snow's old second fog fade was removed: the shared fade above
        // already fogs it once, with the precipitation floor.)

        vec3 snowColor = vec3(0.93, 0.95, 1.0);
        outColor = vec4(snowColor * alpha * tune.look.y, alpha);
    }

    // Debug (ERUPTION_TEST_RAIN_TINT): occluder.w >= 2.0 carries the follower index;
    // paint each cloud's rain a distinct saturated color so overlaps between
    // different clouds' rain are visible.
    if (occVal >= 2.0) {
        float fi = occVal - 2.0;
        float hue = fract(fi * 0.61803398875);
        vec3 rgb = clamp(abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
        outColor.rgb = rgb * max(outColor.a, 0.15) * 6.0;
    }
}
