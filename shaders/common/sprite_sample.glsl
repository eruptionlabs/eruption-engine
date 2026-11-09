// Leitura de sprite com filtro "bilinear nitido" (chave de preset sprite_filter 1).
//
// Pixel art ampliado em nearest tem serrilhado que anda (o texel cai inteiro
// num pixel de tela ou no vizinho conforme a camera se move); bilinear puro
// borra a arte. O meio-termo: 4 leituras nearest dos texels em volta e peso
// que so' mistura na faixa de ~1 pixel de tela em volta da costura entre
// texels (largura dada por fwidth). Ampliado, a arte fica nitida e a borda
// sai anti-serrilhada; reduzido (fwidth > 1 texel) nao ha' costura a
// suavizar e volta o nearest de sempre.
//
// Precisa de slot_textures.glsl (com ERUPTION_SLOT_NEAREST) antes. Com
// paleta (indice no canal r, 0 = transparente) a mistura e' feita DEPOIS da
// consulta de cada texel. A cor sai pre-multiplicada misturada e dividida
// pelo alfa: o texel transparente nao puxa a borda para a cor dele.
// As leituras ficam presas ao quadro [0,5 texel, tamanho - 0,5 texel]: a
// costura da borda nao le' o texel do outro lado (repeat) da folha.
#ifndef ERUPTION_SPRITE_SAMPLE_GLSL
#define ERUPTION_SPRITE_SAMPLE_GLSL

vec4 spriteTexel(uint tex, uint palette, bool usePalette, vec2 uv) {
    vec4 c = slotTextureLod(tex, uv, 0.0);
    if (usePalette) {
        float idx = c.r * 255.0;
        if (idx < 0.5) return vec4(0.0);
        c = slotTextureLod(palette, vec2((idx + 0.5) / 256.0, 0.5), 0.0);
        c.a = 1.0;
    }
    return c;
}

// uvDx/uvDy: derivadas de uv tiradas FORA de fluxo divergente (antes de
// qualquer discard). Devolve false em `filtered` quando caiu no nearest.
vec4 spriteSample(uint tex, uint palette, bool usePalette, vec2 uv, vec2 uvDx, vec2 uvDy) {
    vec2 size = vec2(slotTextureSize(tex, 0));
    vec2 fw = (abs(uvDx) + abs(uvDy)) * size; // texels por pixel de tela
    if (max(fw.x, fw.y) > 1.0) return spriteTexel(tex, palette, usePalette, uv);
    vec2 p = uv * size - 0.5;
    vec2 i0 = floor(p);
    vec2 f = p - i0;
    // Rampa de 1 pixel de tela centrada na costura (f = 0,5).
    vec2 w = clamp((f - 0.5) / max(fw, vec2(1e-4)) + 0.5, 0.0, 1.0);
    vec2 lo = clamp((i0 + 0.5) / size, 0.5 / size, 1.0 - 0.5 / size);
    vec2 hi = clamp((i0 + 1.5) / size, 0.5 / size, 1.0 - 0.5 / size);
    vec4 c00 = spriteTexel(tex, palette, usePalette, vec2(lo.x, lo.y));
    vec4 c10 = spriteTexel(tex, palette, usePalette, vec2(hi.x, lo.y));
    vec4 c01 = spriteTexel(tex, palette, usePalette, vec2(lo.x, hi.y));
    vec4 c11 = spriteTexel(tex, palette, usePalette, vec2(hi.x, hi.y));
    vec4 w4 = vec4((1.0 - w.x) * (1.0 - w.y), w.x * (1.0 - w.y), (1.0 - w.x) * w.y, w.x * w.y);
    float a = dot(w4, vec4(c00.a, c10.a, c01.a, c11.a));
    vec3 rgb = (c00.rgb * c00.a * w4.x + c10.rgb * c10.a * w4.y + c01.rgb * c01.a * w4.z + c11.rgb * c11.a * w4.w);
    return vec4(rgb / max(a, 1e-4), a);
}

#endif
