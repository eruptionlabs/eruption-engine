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
    // FAIXA EM FOCO (world.set_focus): ate' `focus_band` unidades de cada
    // lado do plano de foco o CoC e' zero, e o borrao so' comeca depois. Sem
    // ela o corpo de um grupo inteiro nunca fica cravado: a frente e as
    // costas dele ja' estao fora do plano. 0 = o CoC de antes.
    float focus_band;
    // coc_model "thin_lens" (PostSettings::cocThinLens): K*|z-zf|/z, com
    // teto separado perto e longe e rampa suave de `edge` u depois da faixa.
    int thin_lens;
    float lens_blur;
    float max_near;
    float max_far;
    float edge;
} push;

float bandDist(float z, float f) {
    return max(abs(z - f) - push.focus_band, 0.0);
}

// Lente fina: CoC = A*f*|z-zf| / (z*(zf-f)) = K*|z-zf|/z com f e zf fixos.
// Depende da distancia real, nao da altura na tela: um chao inclinado sai
// em degrade que satura no horizonte, nao numa faixa.
float thinLensCoc(float z, float f) {
    float d = bandDist(z, f);
    float c = push.lens_blur * d / max(z, 1e-3);
    if (push.edge > 0.0) c *= smoothstep(0.0, push.edge, d);
    return min(c, z < f ? push.max_near : push.max_far);
}

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
    float coc = bandDist(z, fd) * apt;
    
    // Chromatic aberration: each channel has a slightly different focal plane or scale
    // Red focuses slightly further, Blue slightly closer
    float coc_r = bandDist(z, fd * 1.02) * apt;
    float coc_g = coc;
    float coc_b = bandDist(z, fd * 0.98) * apt;
    if (push.thin_lens != 0) {
        coc_r = thinLensCoc(z, fd * 1.02);
        coc_g = thinLensCoc(z, fd);
        coc_b = thinLensCoc(z, fd * 0.98);
    }
    
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
