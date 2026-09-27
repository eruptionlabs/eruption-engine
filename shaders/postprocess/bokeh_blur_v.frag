#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_blur;

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 0, binding = 1) uniform sampler2D coc_tex;

const int TAPS = 15;
const float offsets[TAPS] = float[](-7.0, -6.0, -5.0, -4.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0);
const float weights[TAPS] = float[](0.00874, 0.01799, 0.03316, 0.05467, 0.08065, 0.10648, 0.12579, 0.13298, 0.12579, 0.10648, 0.08065, 0.05467, 0.03316, 0.01799, 0.00874);

void main() {
    vec2 texel = 1.0 / vec2(textureSize(scene_tex, 0));
    vec4 coc_center = texture(coc_tex, in_uv);
    float coc_max = coc_center.a;
    
    if (coc_max < 0.1) {
        out_blur = texture(scene_tex, in_uv);
        return;
    }
    
    float color_r = 0.0, color_g = 0.0, color_b = 0.0;
    float weight_r = 0.0, weight_g = 0.0, weight_b = 0.0;
    
    for (int i = 0; i < TAPS; i++) {
        vec2 sample_uv = in_uv + vec2(0.0, offsets[i] * coc_max * texel.y);
        vec3 samp = texture(scene_tex, sample_uv).rgb;
        
        float dist = abs(offsets[i]);
        
        float contrib_r = clamp(coc_center.r - dist + 0.5, 0.0, 1.0);
        float contrib_g = clamp(coc_center.g - dist + 0.5, 0.0, 1.0);
        float contrib_b = clamp(coc_center.b - dist + 0.5, 0.0, 1.0);

        // PESO ANTI-FIREFLY (Karis, "Real Shading in UE4", 2013). Um pixel
        // especular de HDR alto (filme molhado da chuva: roughness ~0,06 sob
        // o sol, radiancia ate 64) entrava aqui com o mesmo peso dos vizinhos
        // e, desfocado, virava um DISCO branco do tamanho do CoC - a "cintilacao
        // saturada na chuva" relatada em 2026-09-05 (some ao dar zoom porque o
        // CoC vai a zero e o ponto volta a ter 1 pixel). Ponderar cada tap por
        // 1/(1+luma) deixa um pixel isolado contribuir no maximo com sua
        // vizinhanca, em vez de dominar o kernel. Mesma receita do downsample
        // de bloom de referencia.
        float kw = 1.0 / (1.0 + max(samp.r, max(samp.g, samp.b)));
        contrib_r *= kw; contrib_g *= kw; contrib_b *= kw;

        color_r += samp.r * weights[i] * contrib_r;
        weight_r += weights[i] * contrib_r;
        
        color_g += samp.g * weights[i] * contrib_g;
        weight_g += weights[i] * contrib_g;
        
        color_b += samp.b * weights[i] * contrib_b;
        weight_b += weights[i] * contrib_b;
    }
    
    vec3 final_color;
    final_color.r = (weight_r > 0.0) ? color_r / weight_r : texture(scene_tex, in_uv).r;
    final_color.g = (weight_g > 0.0) ? color_g / weight_g : texture(scene_tex, in_uv).g;
    final_color.b = (weight_b > 0.0) ? color_b / weight_b : texture(scene_tex, in_uv).b;
    
    out_blur = vec4(final_color, 1.0);
}
