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
    // 0 linear, 1 thin_lens, 2 tilt_plane, 3 spotlight (PostSettings).
    int coc_model;
    float lens_blur;
    float max_near;
    float max_far;
    float edge;
    // tilt_plane: rampas (u de mundo), mistura com a faixa de tela, altura
    // do assunto na tela, plano focal em espaco de vista (normal, d) e
    // 1/P00, 1/P11 para reconstruir a posicao de vista da profundidade.
    float ramp_near;
    float ramp_far;
    float screen_mix;
    float focus_uv_y;
    float plane_x;
    float plane_y;
    float plane_z;
    float plane_d;
    float inv_p00;
    float inv_p11;
    // spotlight: ponto de foco em espaco de vista (o "para cima" do
    // mundo vai em plane_x/y/z).
    float focus_vx;
    float focus_vy;
    float focus_vz;
    // Camera ortografica (1) ou perspectiva (0) e o deslocamento P[3][0],
    // P[3][1] da projecao ortografica, para reconstruir a posicao de vista.
    float ortho;
    float p30;
    float p31;
    // Sinal do eixo "para frente" no espaco de vista (+1 ou -1): a view do
    // motor olha para +Z; uma lookAt classica, para -Z.
    float fwd_z;
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

// TILT-SHIFT DE PLANO: distancia com sinal s ao plano focal (positivo =
// atras do assunto). Nitido dentro da faixa; depois rampas separadas para
// longe e perto, com teto proprio cada. A faixa de tela opcional desfoca
// pela altura na tela em volta do assunto (o tilt-shift "de foto").
float tiltPlaneCoc(float z) {
    vec2 ndc = in_uv * 2.0 - 1.0;
    vec3 pv = vec3(ndc.x * z * push.inv_p00, ndc.y * z * push.inv_p11, z * push.fwd_z);
    float s = dot(vec3(push.plane_x, push.plane_y, push.plane_z), pv) + push.plane_d;
    float far = push.max_far * smoothstep(push.focus_band, push.focus_band + push.ramp_far, s);
    float near = push.max_near * smoothstep(push.focus_band, push.focus_band + push.ramp_near, -s);
    if (push.enable_foreground == 0) near = 0.0;
    float c = far + near;
    if (push.screen_mix > 0.0) {
        // Acima do assunto na tela = longe (camera de cima), abaixo = perto.
        float t = push.focus_uv_y - in_uv.y;
        float farS = push.max_far * smoothstep(0.08, 0.43, t);
        float nearS = push.enable_foreground != 0 ? push.max_near * smoothstep(0.08, 0.30, -t) : 0.0;
        c = mix(c, farS + nearS, push.screen_mix);
    }
    return c;
}

// SPOTLIGHT DE FOCO (circulo no chao): o borrao cresce com a distancia HORIZONTAL ate'
// o assunto, nao com a profundidade da camera. Na tela vira uma elipse
// nitida em volta do grupo que acompanha o chao em perspectiva; o fundo e
// os lados desfocam igual. O que sobe acima do assunto (copa, telhado perto
// da lente) desfoca pela altura, com teto proprio.
//   raio = focus_band, rampa = ramp_far, teto longe = max_far
//   altura inicial = screen_mix, rampa da altura = ramp_near, teto = max_near
float spotlightCoc(float z) {
    vec2 ndc = in_uv * 2.0 - 1.0;
    vec3 pv = push.ortho > 0.5
        ? vec3((ndc.x - push.p30) * push.inv_p00, (ndc.y - push.p31) * push.inv_p11, z * push.fwd_z)
        : vec3(ndc.x * z * push.inv_p00, ndc.y * z * push.inv_p11, z * push.fwd_z);
    vec3 up = vec3(push.plane_x, push.plane_y, push.plane_z);
    vec3 d = pv - vec3(push.focus_vx, push.focus_vy, push.focus_vz);
    float h = dot(d, up);
    float r = length(d - up * h);
    float far = push.max_far * smoothstep(push.focus_band, push.focus_band + push.ramp_far, r);
    float near = push.enable_foreground != 0
        ? push.max_near * smoothstep(push.screen_mix, push.screen_mix + push.ramp_near, h) : 0.0;
    return max(far, near);
}

float linearize_depth(float d) {
    // Ortografica: a profundidade ja' e' linear entre near e far.
    if (push.ortho > 0.5) return push.near_plane + d * (push.far_plane - push.near_plane);
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
    if (push.coc_model == 1) {
        coc_r = thinLensCoc(z, fd * 1.02);
        coc_g = thinLensCoc(z, fd);
        coc_b = thinLensCoc(z, fd * 0.98);
    } else if (push.coc_model == 3) {
        float c = clamp(spotlightCoc(z), 0.0, push.max_blur);
        out_coc = vec4(c, c, c, c);
        return;
    } else if (push.coc_model == 2) {
        // Um CoC so' para os tres canais: o plano nao tem a "lente" que
        // separava o foco por cor, e o teto ja' vem em max_blur.
        float c = clamp(tiltPlaneCoc(z), 0.0, push.max_blur);
        out_coc = vec4(c, c, c, c);
        return;
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
