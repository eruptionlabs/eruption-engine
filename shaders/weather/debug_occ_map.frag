#version 450

// Rain-occlusion debug map, DoF-visualizeCoC style: the SCENE is tinted per
// pixel instead of only coloring the rain particles. Two modes (push.misc.z):
//   0 = dim the whole scene (drawn once, first)
//   1 = classify one follower's occlusion field (drawn once per follower):
//       blue  = the ray camera->scene pixel crosses the cloud plane INSIDE the
//               owning cloud's real coverage (rain behind the cloud is hidden)
//       green = the scene pixel is inside the rain box XZ and below the cloud
//               plane (rain reaches here)
// The colored rain particles are drawn on top afterwards and keep their own
// per-particle reason colors.
layout(location = 0) in vec2 iUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2DArray coverageArray;

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 cameraPos; // xyz = camera position
    vec4 box;       // x,z = rain box center XZ, y = cloud plane height, w = box half size X
    vec4 covBounds; // xy = coverage world min XZ, zw = coverage world size XZ
    vec4 misc;      // xy = coverage wind offset, z = mode (0=dim, 1=classify), w = box half size Z
} push;

void main() {
    if (push.misc.z < 0.5) {
        outColor = vec4(0.0, 0.0, 0.0, 0.55);
        return;
    }

    float depth = texture(depthTex, iUV).r;
    vec4 ndc = vec4(iUV * 2.0 - 1.0, depth, 1.0);
    vec4 wp = push.invViewProj * ndc;
    wp /= wp.w;
    vec3 cam = push.cameraPos.xyz;
    vec3 ray = wp.xyz - cam;
    float planeY = push.box.y;

    // Blue: cloud silhouette on screen (real coverage of the owning cloud,
    // same UV math as the visual plane, no tiling). Threshold matches the
    // VISIBLE cloud edge (~0.1): the old 0.01 painted the thin invisible
    // coverage skirt as occluder, way beyond the rendered cloud.
    if (abs(ray.y) > 0.001 && push.covBounds.z > 0.0 && push.covBounds.w > 0.0) {
        float t = (planeY - cam.y) / ray.y;
        if (t > 0.0 && t < 1.0) {
            vec2 crossXZ = cam.xz + t * ray.xz;
            vec2 covUV = (crossXZ - push.covBounds.xy) / push.covBounds.zw - push.misc.xy;
            float cov = textureLod(coverageArray, vec3(covUV, 0.0), 0.0).r;
            if (cov > 0.10) {
                outColor = vec4(0.25, 0.5, 1.0, 0.75);
                return;
            }
        }
    }

    // Red gradient: the ray crosses the cloud BODY (box XZ slab above the
    // plane = the billboard volume the plane test above can't see). Must come
    // BEFORE the green footprint: clouds don't write depth, so cloud pixels
    // fall in the sky path and would otherwise be painted "rain reaches here"
    // — the map is camera-facing, not a fixed top-down view.
    if (push.covBounds.z > 0.0 && push.covBounds.w > 0.0) {
        const float bodyH = 150.0;
        vec2 bmin = push.box.xz - vec2(push.box.w, push.misc.w);
        vec2 bmax = push.box.xz + vec2(push.box.w, push.misc.w);
        vec2 invD2 = 1.0 / ray.xz;
        vec2 tA2 = (bmin - cam.xz) * invD2;
        vec2 tB2 = (bmax - cam.xz) * invD2;
        vec2 tmn = min(tA2, tB2), tmx = max(tA2, tB2);
        float t0 = max(max(tmn.x, tmn.y), 0.0);
        float t1 = min(tmx.x, tmx.y);
        if (abs(ray.y) > 0.001) {
            float ty0 = (planeY - cam.y) / ray.y;
            float ty1 = (planeY + bodyH - cam.y) / ray.y;
            t0 = max(t0, min(ty0, ty1));
            t1 = min(t1, max(ty0, ty1));
        }
        if (t1 > t0) {
            vec2 pXZ = cam.xz + (t0 + t1) * 0.5 * ray.xz;
            vec2 covUV = (pXZ - push.covBounds.xy) / push.covBounds.zw - push.misc.xy;
            if (covUV.x >= 0.0 && covUV.x <= 1.0 && covUV.y >= 0.0 && covUV.y <= 1.0) {
                float cov = textureLod(coverageArray, vec3(covUV, 0.0), 0.0).r;
                if (cov > 0.05) {
                    float d = clamp(cov * 2.0, 0.0, 1.0);
                    outColor = vec4(mix(vec3(0.55, 0.35, 0.25), vec3(1.0, 0.05, 0.05), d), 0.6);
                    return;
                }
            }
        }
    }

    // Green: rain box footprint (rectangle = the owning cloud's real extents)
    // on the scene below the cloud plane.
    if (depth < 0.999999) {
        vec2 d = abs(wp.xz - push.box.xz);
        if (d.x < push.box.w && d.y < push.misc.w && wp.y < planeY) {
            outColor = vec4(0.2, 0.9, 0.3, 0.55);
            return;
        }
    } else {
        // Sky/void pixel: no surface to reconstruct, so the footprint used to
        // be "cut at the horizon line" (visible at high pitch, when the
        // terrain edge is on screen). Classify by ray-vs-rain-box instead:
        // XZ slab intersect, restricted to the part below the cloud plane.
        vec2 bmin = push.box.xz - vec2(push.box.w, push.misc.w);
        vec2 bmax = push.box.xz + vec2(push.box.w, push.misc.w);
        vec2 invD = 1.0 / ray.xz;
        vec2 tA = (bmin - cam.xz) * invD;
        vec2 tB = (bmax - cam.xz) * invD;
        vec2 tmn = min(tA, tB), tmx = max(tA, tB);
        float t0 = max(max(tmn.x, tmn.y), 0.0);
        float t1 = min(tmx.x, tmx.y);
        if (abs(ray.y) > 0.001) {
            float ty = (planeY - cam.y) / ray.y;
            if (ray.y > 0.0) t1 = min(t1, ty); else t0 = max(t0, ty);
        }
        if (t1 > t0) {
            outColor = vec4(0.2, 0.9, 0.3, 0.55);
            return;
        }
    }

    discard;
}
