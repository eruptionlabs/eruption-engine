#version 450

layout(location = 0) in vec4 iOrigin;   // xyz = spawn pos, w = seed
layout(location = 1) in vec4 iVelocity; // xyz = velocity, w = size
layout(location = 2) in vec4 iData;     // x = type (0=rain,1=snow,2=hail,3=freezingRain), y = turbSeed, z = lifeScale, w = fadeIn

layout(location = 0) out vec2 oUV;
layout(location = 1) out float oType;
layout(location = 2) out float oAlpha;
layout(location = 3) out float oDepth;
layout(location = 4) out vec3 oWorldPos;
layout(location = 5) out float oCloudCenterY;

layout(set = 0, binding = 2) uniform usampler2D heightmap;
layout(set = 0, binding = 3) uniform sampler2DArray cloudCoverageArray;
layout(set = 0, binding = 4) uniform sampler2D cloudAltitudeMap;

// Rain streak shape tuning (WeatherParams rainStreak*; hail/freezing rain
// scale proportionally from these so the sliders move all precip together).
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
    vec4 worldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeY (rain heightmap)
    vec4 cloudWorldBounds; // x=worldMinX, y=worldMinZ, z=worldSizeX, w=worldSizeZ (cloud coverage)
    vec4 cloudParams; // x=layer, y=threshold, z=weatherCoverage, w=unused
    vec4 sunDir; // xyz = sun direction, w = ground level under the player (dust box anchor)
    vec4 cloudRange; // x=cloudBottom (follower: cloud plane height), y,z=cloudTop/range (follower: yw = rain box XZ extents)
    vec4 rainBoxCenter; // xyz = fixed world-space center of the rain box, w = 0 (global) / 1 (per-cloud follower box)
    vec4 occluder; // xy = cloud blob center XZ (followers), z = blob radius (<=0 = infinite plane), w = debug occlusion flag
} push;

// Debug occlusion (w flag): the vertex shader stops clipping culled particles
// and forwards a reason code through oCloudCenterY so the fragment shader can
// color the "occlusion map": 0=alive, 1=camera above cloud base,
// 2=below surface heightmap, 3=outside cloud shadow mask.

const vec2 QUAD[4] = vec2[](
    vec2(-1.0, -1.0),
    vec2(-1.0,  1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0)
);

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p;
    return fract(p);
}

float noise21(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash11(i.x + i.y * 57.0);
    float b = hash11(i.x + 1.0 + i.y * 57.0);
    float c = hash11(i.x + (i.y + 1.0) * 57.0);
    float d = hash11(i.x + 1.0 + (i.y + 1.0) * 57.0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    int ptype = int(iData.x + 0.5);
    bool isRain = ptype == 0;
    bool isSnow = ptype == 1;
    bool isHail = ptype == 2;
    bool isFreezingRain = ptype == 3;
    bool isSand = ptype == 4;
    bool isDust = ptype == 5;
    bool isPrecip = isRain || isHail || isFreezingRain;
    bool isParticle = isSand || isDust;

    // When the camera is above the cloud layer no precipitation should be
    // visible — LEGACY GLOBAL DRAW ONLY (rainBoxCenter.w == 0, the 2D-era
    // infinite cloud plane). Per-cloud followers are EXEMPT: their flakes are
    // occluded per-cloud in the fragment shader (the ray tests each cloud's
    // real coverage silhouette), so a camera above the deck still sees the
    // precipitation wherever no cloud actually blocks the view, instead of
    // the whole field vanishing behind one global plane (author feedback
    // 2026-08-10: "zoom 0 n tem neve nenhuma, zoom 10 DO NADA aparece").
    // Sand/dust are not cloud-bound, so they keep falling.
    float cloudBase = push.params5.y;
    float occVal = push.occluder.w;
    bool debugRainRegions = false;
    if (occVal >= 9.5) {
        debugRainRegions = true;
        occVal -= 10.0;
    }
    float cameraUnderCloud = debugRainRegions ? 1.0 : (1.0 - smoothstep(cloudBase - 15.0, cloudBase + 15.0, push.cameraPos.y));
    bool debugOcclusion = occVal > 0.5;
    oCloudCenterY = 0.0; // debug reason: 0 = alive
    if (cameraUnderCloud < 0.001 && (isPrecip || isSnow) && push.rainBoxCenter.w < 0.5) {
        if (debugOcclusion) {
            oCloudCenterY = 1.0;
        } else {
            oAlpha = 0.0;
            oType = iData.x;
            gl_Position = vec4(0.0, 0.0, -1000.0, 1.0);
            return;
        }
    }

    oType = iData.x;
    float seed = iOrigin.w;
    float time = push.params.x;

    // Periodic time for this particle
    float lifeScale = iData.z;
    float t = fract(time * 0.2 * lifeScale + seed * 10.0);
    oAlpha = 1.0 - abs(t - 0.5) * 2.0;
    oAlpha = smoothstep(0.0, 0.2, oAlpha) * (0.5 + 0.5 * iData.w);

    // Sand/dust: stratified depth layers (near/mid/far) for volumetric feel.
    int layer = isSand || isDust ? int(seed * 3.0) : 0;
    float boxSizes[3] = float[](90.0, 180.0, 360.0);

    // Box wrapping. Vertical size reaches 95% of the cloud base so rain spans
    // from the ground up to just below the clouds. Global precipitation uses
    // the fixed 80x80 m camera box; per-cloud followers (rainBoxCenter.w > 0.5)
    // carry the owning cloud's footprint extents in cloudRange.yw, so the rain
    // spreads under the whole cloud instead of clustering (and additively
    // blowing out into a glowing blob) in one 80x80 m patch.
    bool followerBox = push.rainBoxCenter.w > 0.5;
    vec2 boxXZ = followerBox ? push.cloudRange.yw : vec2(80.0);
    float boxHeight = max(push.cloudRange.x * 0.95, 60.0);
    vec3 boxCenter = push.rainBoxCenter.xyz + vec3(0.0, boxHeight * 0.5, 0.0);
    if (isSand || isDust) {
        // Sand/dust: low ground-hugging box (40 m) anchored at the REAL ground
        // level (sunDir.w = orbit target Y) and centered on the player
        // (worldBounds center), not the camera — at pitched zooms the camera
        // sits hundreds of meters behind the viewed ground, so a camera-anchored
        // near layer misses the street the player is looking at. The legacy
        // cloud-spanning column diluted grains over ~450 m of altitude and made
        // the storm invisible at street level (author feedback 2026-08-10).
        boxHeight = 40.0;
        boxCenter = vec3(push.worldBounds.x + push.worldBounds.z * 0.5,
                         push.sunDir.w + boxHeight * 0.5,
                         push.worldBounds.y + push.worldBounds.w * 0.5);
    }
    vec3 boxSize = vec3(boxXZ.x, boxHeight, boxXZ.y);
    if (isSand || isDust) {
        // Scale the box with the camera distance so the storm covers the whole
        // frustum at any zoom (was: fixed 90/180/360 m layers — the box edge
        // was clearly visible when zooming out, author feedback 2026-08-11).
        float zoomScale = max(1.0, length(push.cameraPos.xz - boxCenter.xz) / 200.0);
        boxSize = vec3(boxSizes[layer], 40.0, boxSizes[layer]) * zoomScale;
    }
    vec3 spawnOffset = iOrigin.xyz - boxCenter;

    // Current position: spawn + velocity * time
    vec3 vel = iVelocity.xyz;
    vec3 pos = boxCenter + spawnOffset + vel * t * 5.0;

    // Wrap around box relative to fixed center
    vec3 rel = pos - boxCenter;
    rel.x = mod(rel.x + boxSize.x * 0.5, boxSize.x) - boxSize.x * 0.5;
    rel.z = mod(rel.z + boxSize.z * 0.5, boxSize.z) - boxSize.z * 0.5;
    rel.y = mod(rel.y + boxSize.y * 0.5, boxSize.y) - boxSize.y * 0.5;
    pos = boxCenter + rel;

    // Seamless box edge: fade grains out near the XZ boundary so the wrap
    // border never reads as a hard cut (author feedback 2026-08-11).
    if (isSand || isDust) {
        vec2 edgeD = abs(rel.xz) / max(boxSize.xz * 0.5, vec2(1.0));
        oAlpha *= 1.0 - smoothstep(0.75, 0.98, max(edgeD.x, edgeD.y));
    }

    // The cloud base (already fetched above) defines the ceiling for precipitation.
    // Instead of killing whole particles in the vertex shader (which hid rain
    // completely when the camera was above the clouds), we pass the world position
    // to the fragment shader and clip each fragment there.  This lets rain be
    // visible below the cloud layer from any camera angle.

    // Rain/hail/freezing-rain occlusion: kill drops that have fallen below the
    // highest visible surface in their XZ column (i.e. under a roof/bridge).
    // Sand/dust get the same below-surface kill (grains under terrain).
    if (isPrecip || isSnow || isSand || isDust) {
        vec2 hmUV = (pos.xz - push.worldBounds.xy) / push.worldBounds.zw;
        if (all(greaterThanEqual(hmUV, vec2(0.0))) && all(lessThanEqual(hmUV, vec2(1.0)))) {
            uint h = texture(heightmap, hmUV).r;
            float maxY = uintBitsToFloat(h) - 10000.0;
            if (pos.y <= maxY + 0.1) {
                if (debugOcclusion) {
                    oCloudCenterY = 2.0;
                } else {
                    oAlpha = 0.0;
                    gl_Position = vec4(0.0, 0.0, -1000.0, 1.0);
                    return;
                }
            }
        }
    }

    // Wind-shadow occlusion for sand/dust (author request 2026-08-10): grains
    // travel SIDEWAYS with the wind, so the vertical column test above is not
    // enough — a building or hill UPWIND blocks the grains that come from
    // behind it. March a few samples against the horizontal wind direction; if
    // any terrain column along that path is taller than the grain, the grain
    // is in the lee wind shadow and dies.
    if (isSand || isDust) {
        vec2 windDir = normalize(vel.xz + vec2(1.0e-4, 0.0));
        bool windBlocked = false;
        for (int k = 1; k <= 6; ++k) {
            vec2 sp = pos.xz - windDir * (float(k) * 6.0);
            vec2 hmUV = (sp - push.worldBounds.xy) / push.worldBounds.zw;
            if (all(greaterThanEqual(hmUV, vec2(0.0))) && all(lessThanEqual(hmUV, vec2(1.0)))) {
                uint h = textureLod(heightmap, hmUV, 0.0).r;
                float maxY = uintBitsToFloat(h) - 10000.0;
                if (maxY > pos.y + 0.3) { windBlocked = true; break; }
            }
        }
        if (windBlocked) {
            if (debugOcclusion) {
                oCloudCenterY = 3.0;
            } else {
                oAlpha = 0.0;
                gl_Position = vec4(0.0, 0.0, -1000.0, 1.0);
                return;
            }
        }
    }

    // Cloud-altitude occlusion: precipitation/snow only exists below the local
    // cloud top. The altitude map stores a normalized [0,1] cloud-center
    // height; map it to [cloudBottom, cloudTop].
    float cloudCenterY = -1.0e9;
    if ((isPrecip || isSnow) && push.cloudRange.z > 0.0) {
        vec2 altUV = (pos.xz - push.cloudWorldBounds.xy) / push.cloudWorldBounds.zw
                     - push.params5.zw;
        altUV = fract(altUV);
        float altitude = textureLod(cloudAltitudeMap, altUV, 0.0).r;
        cloudCenterY = push.cloudRange.x + altitude * push.cloudRange.z;
    }

    // Precipitation/snow only fall where the cloud shadow reaches this point.
    // Sample the cloud coverage at the sun-projected cloud column, matching
    // the shadow projection direction so rain/snow lands inside the shadow mask.
    // Use the cloud coverage world bounds (not the rain heightmap bounds) so the
    // UV mapping matches the actual cloud coverage texture.
    if (isPrecip || isSnow) {
        vec3 L = normalize(push.sunDir.xyz);
        float heightFactor = max(push.params5.y - pos.y, 0.0);
        vec2 lightOffset = vec2(0.0);
        if (L.y > 0.01) {
            // Clamp the horizontal stretch so low sun does not wrap UVs out of bounds.
            float tanAngle = min(length(L.xz) / L.y, 4.0);
            lightOffset = -normalize(L.xz + vec2(0.0001)) * heightFactor * tanAngle;
        }
        // Dilate the sample area with height so the shadow penumbra is soft,
        // but keep the radius in UV space so it scales with the texture.
        float sampleRadius = min(heightFactor * 0.002, 0.015);
        float maxCov = 0.0;
        vec2 cloudWorldSize = push.cloudWorldBounds.zw;
        // Apply the same wind offset as the visual clouds and overlay so the
        // precipitation mask stays aligned with the actual cloud silhouettes.
        vec2 baseUV = (pos.xz + lightOffset - push.cloudWorldBounds.xy) / cloudWorldSize
                      - push.params5.zw;
        // More samples for sharper holes in areas with no cloud shadow.
        for (int i = 0; i < 8; ++i) {
            float angle = float(i) * 0.78539816339; // 2*pi/8
            vec2 offset = vec2(cos(angle), sin(angle)) * sampleRadius;
            vec2 uv = fract(baseUV + offset);
            // The coverage texture is already thresholded by the Cloud Amount slider on CPU.
            float cov = textureLod(cloudCoverageArray, vec3(uv, push.cloudParams.x), 0.0).r;
            cov = pow(cov, 4.0);
            maxCov = max(maxCov, cov);
        }
        float density = maxCov * push.cloudParams.z;
        // Per-cloud rain boxes (rainBoxCenter.w > 0.5) carry their own rain:
        // they bypass the global coverage mask, which is empty in Clear weather.
        float forceRain = step(0.5, push.rainBoxCenter.w);
        density = max(density, forceRain);
        // Lower threshold: precipitation only survives under dense cloud shadows.
        if (density < 0.20) {
            if (debugOcclusion) {
                oCloudCenterY = 3.0;
            } else {
                oAlpha = 0.0;
                gl_Position = vec4(0.0, 0.0, -1000.0, 1.0);
                return;
            }
        }
        // Follower boxes bypass the camera-height fade too: their flakes are
        // culled per-cloud in the fragment shader, not by one global plane.
        float camFade = mix(cameraUnderCloud, 1.0, forceRain);
        oAlpha *= density * camFade;
    }

    // Turbulence for snow / sand / dust
    if (isSnow || isSand || isDust) {
        float turb = noise21(vec2(pos.x, pos.z) * 0.3 + time * 0.5 + iData.y);
        pos.x += (turb - 0.5) * 2.0;
        pos.z += (noise21(vec2(pos.z, pos.x) * 0.3 + time * 0.5) - 0.5) * 2.0;
    }

    // Billboard expansion
    vec2 quad = QUAD[gl_VertexIndex];
    float size = iVelocity.w;
    // Debug occlusion view: fatten the particles a bit so per-drop colors stay
    // readable from far / top-down cameras. Modest on purpose: the debug path
    // is additive, so oversized quads overlap and saturate to white.
    if (debugOcclusion) size *= 1.5;

    // Minification compensation (precipitation + snow): past the camera
    // distance where a drop/flake shrinks below ~1px (zoom 40% ≈ 604 m;
    // zoomPercent = 1-(dist-10)/990), scale the quad up so distant particles
    // keep the apparent size they have at zoom 40% instead of vanishing
    // (author request 2026-08-10: "a 0% quero enxergar a partícula como a
    // 40%"). Capped at 1000/604 ≈ 1.66 (zoom 0%) so they never grow past
    // that look.
    // Teto do travamento: o quad cresce em LARGURA e COMPRIMENTO, entao o
    // custo de preenchimento cresce com o QUADRADO - 4x de tamanho e' 16x de
    // fill com 393 mil gotas (medido: passe de particula 0,22 -> 1,6 ms).
    // 2,5x pega quase toda a leitura por ~6x de fill.
    const float kFarSizeCap = 2.5;
    if ((isPrecip || isSnow) && !debugRainRegions) {
        // TAMANHO APARENTE TRAVADO A PARTIR DE 150 u, nao de 604. A gota tem
        // tamanho de MUNDO afinado para a camera de jogo (orbita 60-150 u);
        // dobrar a distancia corta o rastro pela metade na tela. O trecho
        // anterior so' compensava alem de 604 u (e no maximo 1,66x), entao a
        // faixa 150-600 u - que e' onde a camera de diorama trabalha - ficava
        // SEM compensacao nenhuma: "voce liga tempestade e eu nao to vendo
        // pingo" (autor, 2026-09-06). Com a referencia em 150 u a gota mantem
        // na tela o tamanho que tem no zoom de jogo, ate' o teto de 4x. Perto
        // (abaixo de 150 u) nada muda - o look que o autor ajustou fica
        // intacto. Medido a 420 u: pixels de gota 0,49% -> 1,4% do quadro.
        float camDist = length(pos - push.cameraPos.xyz);
        size *= clamp(camDist / 150.0, 1.0, kFarSizeCap);
    }
    // Sand/dust apparent-size lock (author feedback 2026-08-11): below zoom
    // ~86% (dist < 150 m) grains keep their true (small) size — no more
    // fish-school close-up. From 150 m to ~350 m the quad grows with distance
    // so the look at zoom 77% holds down to zoom 65%; past 350 m the cap
    // freezes growth and grains fade to near-invisible (realistic).
    if ((isSand || isDust) && !debugRainRegions) {
        float camDist = length(pos - push.cameraPos.xyz);
        size *= clamp(camDist / 150.0, 1.0, 2.33);
    }

    if ((isPrecip || isSnow) && !debugRainRegions) {
        // Rain/hail/freezing rain + SNOW: stretch in velocity direction. Snow
        // uses the same streak path (author request 2026-08-10): flakes must
        // lie along the wind angle ("cair deitada"), not fall as upright
        // camera-facing dots. The flake frag shape is radial, so the stretched
        // quad reads as an ellipse tilted by the wind.
        // The streak shape comes straight from the per-weather-type preset
        // (WeatherSystem setStreakShape) — hail/freezing rain/snow carry their
        // own preset values, no extra per-type multipliers here.
        vec3 velN = normalize(vel);
        vec3 right = normalize(cross(velN, vec3(0.0, 1.0, 0.0)));
        float lengthScale = tune.shape.y;
        float widthScale = tune.shape.x;
        vec3 up = velN * size * lengthScale;
        right *= size * widthScale;

        // Pixel-coverage fade (anti-shimmer): when the projected streak width
        // drops below pixelFade pixels, dim the drop instead of letting the
        // additive blend blow a sub-pixel quad out into a fat bright blob.
        if (tune.look.z > 0.0) {
            vec4 clipC = push.viewProj * vec4(pos, 1.0);
            vec4 clipR = push.viewProj * vec4(pos + right, 1.0);
            float pixHalfW = length((clipR.xy / clipR.w - clipC.xy / clipC.w) * 0.5 * push.screenSize.xy);
            oAlpha *= clamp(2.0 * pixHalfW / tune.look.z, 0.0, 1.0);
        }
        pos += right * quad.x + up * quad.y * 0.5;
        oUV = quad * 0.5 + 0.5;
    } else {
        // Snow / sand / dust, or precip under debugRainRegions: camera-facing square
        vec3 right = vec3(push.viewProj[0][0], push.viewProj[1][0], push.viewProj[2][0]);
        vec3 up = vec3(push.viewProj[0][1], push.viewProj[1][1], push.viewProj[2][1]);
        float finalSize = size;
        if (isPrecip && debugRainRegions) {
            finalSize = size * 4.0; // fat solid blocks
        }
        float aspect = push.params.w;
        // Sand/dust: stretch the billboard along the SCREEN PROJECTION of the
        // wind velocity (was: fixed 2.5x along screen-right regardless of the
        // wind) so every grain reads as a lateral streak in the wind direction
        // (author request 2026-08-10: "vai pros lados, seguindo o vento").
        if (isSand || isDust) {
            vec4 vClip = push.viewProj * vec4(pos + vel, 1.0);
            vec4 pClip = push.viewProj * vec4(pos, 1.0);
            vec2 vdir = vClip.xy / vClip.w - pClip.xy / pClip.w;
            float vlen = length(vdir);
            vec2 stretchDir = vlen > 1.0e-5 ? vdir / vlen : vec2(1.0, 0.0);
            vec2 perpDir = vec2(-stretchDir.y, stretchDir.x);
            vec3 sr = normalize(right);
            vec3 su = normalize(up);
            vec3 worldStretch = normalize(sr * stretchDir.x + su * stretchDir.y * aspect);
            vec3 worldPerp = normalize(sr * perpDir.x + su * perpDir.y * aspect);
            pos += worldStretch * quad.x * finalSize * 2.2
                 + worldPerp * quad.y * finalSize;
        } else {
            pos += normalize(right) * quad.x * finalSize
                 + normalize(up) * quad.y * finalSize;
        }
        oUV = quad * 0.5 + 0.5;
    }

    oWorldPos = pos;

    vec4 clip = push.viewProj * vec4(pos, 1.0);
    oDepth = clip.w;
    gl_Position = clip;
}
