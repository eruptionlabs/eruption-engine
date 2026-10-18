#version 450
// BORRAO GRANDE DO DoF, passo 1: a cena em 1/4 da resolucao, pesada pelo CoC.
// Desfocar numa imagem 16x menor e' o que torna barato um raio de dezenas de
// pixels (o borrao de "mini-maquete"); o kernel cheio na resolucao inteira
// custaria o mesmo por pixel com 16x mais pixels. Ideia da cadeia em
// resolucao reduzida: Abadie, "A Life of a Bokeh" (SIGGRAPH 2018, Advances in
// Real-Time Rendering). O peso pelo CoC da propria amostra (cor
// pre-multiplicada, peso no alfa) segura o halo: o que esta' em foco quase
// nao entra na media, e o boneco nitido nao vaza para o fundo borrado.
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 0, binding = 1) uniform sampler2D coc_tex;

layout(push_constant) uniform PushConstants {
    float max_blur;   // teto do CoC (pixels de resolucao inteira)
    float small_cap;  // ate' aqui o borrao pequeno (bokeh_blur_*) da' conta
} push;

void main() {
    // 4 amostras bilineares no meio de cada quadra 2x2 do bloco 4x4.
    vec2 texel = 1.0 / vec2(textureSize(scene_tex, 0));
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0) * texel;
        vec2 uv = in_uv + o;
        vec3 c = texture(scene_tex, uv).rgb;
        float coc = texture(coc_tex, uv).a;
        // Brilho comprimido (mesmo 1/(1+max) do bokeh): uma faisca nao vira
        // um disco estourado no borrao grande.
        float w = smoothstep(push.small_cap * 0.5, push.max_blur, coc) / (1.0 + max(c.r, max(c.g, c.b)));
        acc += vec4(c * w, w);
    }
    out_color = acc * 0.25;
}
