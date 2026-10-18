#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D original_tex;
layout(set = 0, binding = 1) uniform sampler2D blurred_tex;
layout(set = 0, binding = 2) uniform sampler2D coc_tex;
// Borrao grande em 1/4 da resolucao, cor pre-multiplicada pelo peso no alfa
// (dof_far_*.frag). So' e' lido com teto acima de 3 px.
layout(set = 0, binding = 3) uniform sampler2D far_tex;

layout(push_constant) uniform PushConstants {
    float max_blur;
    float visualize_coc;
} push;

void main() {
    vec3 orig = texture(original_tex, in_uv).rgb;
    vec3 blur = texture(blurred_tex, in_uv).rgb;
    vec4 coc = texture(coc_tex, in_uv);
    
    // Ate' 3 px de CoC o borrao pequeno (resolucao inteira) faz a rampa como
    // antes; com teto maior, de 3 px ao teto ele cede ao borrao grande. Com
    // teto ate' 3 a conta e' a de antes.
    float smallCap = min(push.max_blur, 3.0);
    float blend = clamp(coc.a / smallCap, 0.0, 1.0);
    if (push.max_blur > 3.0) {
        vec4 far = texture(far_tex, in_uv);
        if (far.a > 1e-4) blur = mix(blur, far.rgb / far.a, smoothstep(smallCap, push.max_blur, coc.a));
    }
    
    if (push.visualize_coc > 0.5) {
        // Heatmap: Blue (Focus) -> Cyan -> Green -> Yellow -> Red (Blur)
        vec3 colFocus = vec3(0.0, 0.5, 1.0);
        vec3 colBlur = vec3(1.0, 0.0, 0.0);
        vec3 heatmap = mix(colFocus, colBlur, blend);
        
        // Add a "flash" or pulse to make it obvious it's a visualizer
        out_color = vec4(mix(orig * 0.1, heatmap, 0.9), 1.0);
    } else {
        out_color = vec4(mix(orig, blur, blend), 1.0);
    }
}
