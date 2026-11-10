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
//
// REALCES (bokeh_intensity > 0): a amostra que cobre este pixel pesa
// 1 + intensidade x (luminancia exposta - limiar) na media. O ponto claro
// fora de foco (ceu entre folhas, janela acesa, lampiao) domina o disco que
// ele cobre e aparece como um disco claro de borda firme - o bokeh de lente -
// em vez de diluir na media. Com intensidade 0 o peso e' 1 e a coleta e' a
// de sempre. bokeh_shape mistura o disco com um hexagono (6 laminas): a
// amostra cobre este pixel se a distancia cabe no raio do hexagono naquele
// angulo.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_tex; // HDR, resolucao cheia
layout(set = 0, binding = 1) uniform sampler2D coc_tex;   // CoC em px de tela cheia (alfa)
layout(set = 0, binding = 2) uniform sampler2D depth_tex; // profundidade (sem filtro)

layout(push_constant) uniform PushConstants {
    float max_coc; // teto do CoC neste quadro (px de tela cheia)
    float taps;    // amostras do disco
    float hl_threshold; // limiar dos realces (luminancia exposta)
    float hl_intensity; // 0 = sem realce
    float exposure;
    float shape;        // 0 disco .. 1 hexagono
} pc;

// Peso de realce de uma amostra (teto: o sol nao vira um disco branco chapado).
float highlightWeight(vec3 c) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722)) * pc.exposure;
    return 1.0 + pc.hl_intensity * min(max(l - pc.hl_threshold, 0.0), 8.0);
}

// Raio relativo do hexagono no angulo a (1 nos vertices, cos 30 nas arestas),
// misturado com o disco por `shape`. Hexagono girado 15 graus (nada de
// aresta reta na horizontal, que le' como "UI").
float apertureScale(float a) {
    if (pc.shape <= 0.0) return 1.0;
    const float kSeg = 1.04719755; // 60 graus
    float t = mod(a + 0.2617994, kSeg) - kSeg * 0.5;
    return mix(1.0, 0.8660254 / cos(t), pc.shape);
}

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
    float w0 = highlightWeight(col0);
    vec3 acc = col0 * w0;
    float tot = w0;
    float nearCover = 0.0;
    for (int i = 0; i < n; ++i) {
        float r = reach * sqrt((float(i) + 0.5) / float(n));
        float a = float(i) * kGolden;
        vec2 uv = in_uv + vec2(cos(a), sin(a)) * r * texel;
        float s = texture(coc_tex, uv).a;
        bool behind = texture(depth_tex, uv).r > d0;
        float sEff = behind ? min(s, c0 * 2.0) : s;
        float m = smoothstep(r - 1.0, r + 1.0, sEff * apertureScale(a));
        vec3 c = texture(scene_tex, uv).rgb;
        // Amostra que cobre entra com o peso de realce; a que nao cobre, com
        // a media acumulada e peso 1 (como antes).
        float hw = m * highlightWeight(c);
        acc += (acc / tot) * (1.0 - m) + c * hw;
        tot += (1.0 - m) + hw;
        if (!behind) nearCover += m * smoothstep(0.5, 1.5, s - c0);
    }
    out_color = vec4(acc / tot, clamp(nearCover / float(n) * 3.0, 0.0, 1.0));
}
