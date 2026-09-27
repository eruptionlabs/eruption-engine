#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_coc;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;

layout(push_constant) uniform PushConstants {
    float focal_distance;
    float aperture;
    float max_blur;
    float zoom_factor;
    float near_plane;
    float far_plane;
    int enable_foreground;
} push;

float linearize_depth(float d) {
    // Vulkan [0, 1] depth linearization
    return push.near_plane * push.far_plane / (push.far_plane + d * (push.near_plane - push.far_plane));
}

void main() {
    float raw_depth = texture(depth_tex, in_uv).r;
    float z = linearize_depth(raw_depth);
    
    float fd = push.focal_distance;
    float apt = push.aperture;
    
    // Artistic CoC: Direct scale by distance from focal plane
    // We scale by apt which now represents a "blur per unit of distance"
    float coc = abs(z - fd) * apt;
    
    // Chromatic aberration: each channel has a slightly different focal plane or scale
    // Red focuses slightly further, Blue slightly closer
    float coc_r = abs(z - (fd * 1.02)) * apt;
    float coc_g = coc;
    float coc_b = abs(z - (fd * 0.98)) * apt;
    
    if (push.enable_foreground == 0) {
        if (z < fd) {
            coc_r = 0.0; coc_g = 0.0; coc_b = 0.0;
        }
    }
    
    coc_r = clamp(coc_r, 0.0, push.max_blur);
    coc_g = clamp(coc_g, 0.0, push.max_blur);
    coc_b = clamp(coc_b, 0.0, push.max_blur);
    
    out_coc = vec4(coc_r, coc_g, coc_b, max(coc_r, max(coc_g, coc_b)));
}
