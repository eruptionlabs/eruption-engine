#version 450
// DoF DE QUALIDADE (preset dof_quality 3): bokeh por coleta em disco, a meia
// resolucao. Cada pixel junta as amostras de um disco de Vogel (espiral de
// angulo de ouro, densidade uniforme) e aceita a amostra que, espalhada pelo
// CoC DELA, cobriria este pixel ("scatter as gather"): o desfoque sai em
// disco de borda firme, como o de uma lente, e nao na gaussiana do caminho
// separavel.
//
// Separacao perto/longe: amostra ATRAS deste pixel tem o CoC limitado a 2x o
// deste pixel. Assim o fundo (nitido ou desfocado) nao vaza por cima de um
// primeiro plano, e o primeiro plano desfocado - que esta' na frente - se
// espalha por cima do fundo nitido, que e' o que a lente faz. Amostra que nao
// cobre entra com a media acumulada (Gustafsson): nao escurece nem clareia.
//
// Raio do disco: o maior entre o CoC deste pixel e o do primeiro plano em
// volta (8 leituras num anel no teto do CoC). Pixel nitido sem frente
// desfocada por perto sai no primeiro teste, sem disco.
//
// Saida: rgb = cor desfocada; a = cobertura pelo primeiro plano desfocado
// (bokeh_composite.frag mistura por max(CoC proprio, a)).

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_tex; // HDR, resolucao cheia
layout(set = 0, binding = 1) uniform sampler2D coc_tex;   // CoC em px de tela cheia (alfa)
layout(set = 0, binding = 2) uniform sampler2D depth_tex; // profundidade (sem filtro)

layout(push_constant) uniform PushConstants {
    float max_coc; // teto do CoC neste quadro (px de tela cheia)
    float taps;    // amostras do disco
} pc;

const float kGolden = 2.39996323;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(scene_tex, 0));
    float c0 = texture(coc_tex, in_uv).a;
    float d0 = texture(depth_tex, in_uv).r;
    vec3 col0 = texture(scene_tex, in_uv).rgb;

    // Primeiro plano desfocado em volta que alcanca este pixel.
    float reach = c0;
    for (int k = 0; k < 8; ++k) {
        float a = float(k) * 0.78539816 + 0.3927;
        for (int ring = 1; ring <= 2; ++ring) {
            float r = pc.max_coc * float(ring) * 0.5;
            vec2 uv = in_uv + vec2(cos(a), sin(a)) * r * texel;
            float s = texture(coc_tex, uv).a;
            if (texture(depth_tex, uv).r < d0 && s > r * 0.5) reach = max(reach, min(s, pc.max_coc));
        }
    }
    if (reach < 0.5) {
        out_color = vec4(col0, 0.0);
        return;
    }

    int n = int(pc.taps);
    vec3 acc = col0;
    float tot = 1.0;
    float nearCover = 0.0;
    for (int i = 0; i < n; ++i) {
        float r = reach * sqrt((float(i) + 0.5) / float(n));
        float a = float(i) * kGolden;
        vec2 uv = in_uv + vec2(cos(a), sin(a)) * r * texel;
        float s = texture(coc_tex, uv).a;
        bool behind = texture(depth_tex, uv).r > d0;
        float sEff = behind ? min(s, c0 * 2.0) : s;
        float m = smoothstep(r - 1.0, r + 1.0, sEff);
        vec3 c = texture(scene_tex, uv).rgb;
        acc += mix(acc / tot, c, m);
        tot += 1.0;
        if (!behind) nearCover += m * smoothstep(0.5, 1.5, s - c0);
    }
    out_color = vec4(acc / tot, clamp(nearCover / float(n) * 3.0, 0.0, 1.0));
}
