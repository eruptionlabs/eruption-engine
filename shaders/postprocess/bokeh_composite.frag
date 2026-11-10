#version 450
// Mistura do bokeh de meia resolucao (bokeh_gather.frag) com a cena nitida:
// pelo CoC deste pixel (resolucao cheia: a borda de um assunto nitido nao
// pega o desfoque do fundo vizinho) ou pela cobertura do primeiro plano
// desfocado que passa por cima dele.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 0, binding = 1) uniform sampler2D bokeh_tex;
layout(set = 0, binding = 2) uniform sampler2D coc_tex;

void main() {
    vec3 orig = texture(scene_tex, in_uv).rgb;
    vec4 b = texture(bokeh_tex, in_uv);
    float c = texture(coc_tex, in_uv).a;
    float w = max(smoothstep(0.3, 1.2, c), b.a);
    out_color = vec4(mix(orig, b.rgb, w), 1.0);
}
