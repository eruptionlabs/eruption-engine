// JUNCAO MALHA x TERRENO (src/core/GroundBlend.cpp). Peca (malha sem splat)
// numa faixa de altura acima do chao mistura com o material DO CHAO naquele
// XZ: cor base + as duas camadas que mais pesam na mascara (a mesma conta do
// bloco de splat do main), a variacao macro, a normal (relevo da grade de
// altura + normal map das camadas) e o MRAH-W. O peso desce com a altura acima
// do chao e a borda segue o grao das duas texturas: mistura por altura com
// vies pela luminancia (Mishkinis, "Advanced Terrain Texture Splatting", 2013).
// Material do terreno amostrado pela malha perto do chao, normal puxada para a
// dele: DICE, "Photogrammetry and Star Wars Battlefront", GDC 2016.
//
// Incluido no model_frag.glsl DEPOIS de camadaSplat/ProjLado/amostraChao.
// A UV do chao e a da mascara sao afins no XZ (ver captureGroundBlend), entao
// as derivadas saem das derivadas do proprio XZ, calculadas no fluxo uniforme.

struct GroundSample {
    float w;        // peso da mistura neste pixel (0 = fora da faixa)
    vec3 color;     // sRGB, como o texColor
    vec3 normal;    // normal do chao no mundo
    vec4 mrahw;
    float mrahwW;   // 0 = chao sem MRAH-W
    float nPeso;    // quanto da normal do chao a peca puxa (menos na parede)
};

float gbAltura(vec2 xz) {
    const vec2 huv = vec2(xz.x * u_gbHeight.x + u_gbHeight.y, xz.y * u_gbHeight.z + u_gbHeight.w);
    return u_gbUvU.w + texture(u_textures[nonuniformEXT(u_gbSlots.z)], huv).r * u_gbUvV.w;
}

// Normal do relevo pela grade de altura (diferenca central, uma celula).
vec3 gbNormalDoChao(vec2 xz) {
    const float e = u_gbMaskV.w;
    const float hl = gbAltura(xz - vec2(e, 0.0)), hr = gbAltura(xz + vec2(e, 0.0));
    const float hd = gbAltura(xz - vec2(0.0, e)), hu = gbAltura(xz + vec2(0.0, e));
    return normalize(vec3(hl - hr, 2.0 * e, hd - hu));
}

// Espaco tangente do chao: T e B sao para onde a UV cresce no XZ (inversa da
// parte linear da funcao afim), deitados no plano da normal.
mat3 gbTBN(vec3 n) {
    const mat2 m = mat2(u_gbUvU.x, u_gbUvV.x, u_gbUvU.y, u_gbUvV.y); // colunas: d(uv)/dx, d(uv)/dz
    const mat2 inv = inverse(m);                                       // colunas: d(xz)/du, d(xz)/dv
    vec3 t = vec3(inv[0].x, 0.0, inv[0].y);
    vec3 b = vec3(inv[1].x, 0.0, inv[1].y);
    t.y = -(n.x * t.x + n.z * t.z) / max(n.y, 0.05);
    b.y = -(n.x * b.x + n.z * b.z) / max(n.y, 0.05);
    return mat3(normalize(t), normalize(b), n);
}

// `faixa`: a altura da mistura para ESTA peca (ver o main: no maximo 30% da
// altura dela, senao uma pedra baixa inteira virava chao).
// `ng`: normal de GEOMETRIA da peca (sem normal map), no mundo.
GroundSample gbAmostra(vec3 wp, vec3 ng, vec3 dpdx, vec3 dpdy, float lumPeca, float faixa) {
    GroundSample g;
    g.w = 0.0; g.color = vec3(0.0); g.normal = vec3(0.0, 1.0, 0.0); g.mrahw = vec4(0.0); g.mrahwW = 0.0; g.nPeso = 0.0;
    const float h = wp.y - gbAltura(wp.xz);
    // Fora: acima da faixa, face voltada para baixo, ou bem abaixo do chao (o
    // degrau de dentro de uma escada que desce virava terra).
    if (h >= faixa || h < -faixa || ng.y < -0.2) return g;
    const vec3 p = vec3(wp.xz, 1.0);
    const vec2 uv = vec2(dot(u_gbUvU.xyz, p), dot(u_gbUvV.xyz, p));
    const vec2 ddx = vec2(dot(u_gbUvU.xy, dpdx.xz), dot(u_gbUvV.xy, dpdx.xz));
    const vec2 ddy = vec2(dot(u_gbUvU.xy, dpdy.xz), dot(u_gbUvV.xy, dpdy.xz));
    const vec2 muv = vec2(dot(u_gbMaskU.xyz, p), dot(u_gbMaskV.xyz, p));
    const vec3 n = gbNormalDoChao(wp.xz);
    const mat3 tbn = gbTBN(n);
    const bool quebrar = !LITE;
    // FACE INGREME: o chao lido de cima (XZ) virava riscos verticais na
    // parede da peca (caixas de escada, blocos). Ali as camadas sao lidas
    // pelas projecoes laterais, as mesmas do triplanar do chao (T2; pesos
    // |N|^4, Golus 2017), na mesma escala da UV do chao.
    const float k = length(vec2(u_gbUvU.x, u_gbUvV.x)) + length(vec2(u_gbUvU.y, u_gbUvV.y));
    const float ke = 0.5 * k;
    const float sx = ng.x >= 0.0 ? 1.0 : -1.0, sz = ng.z >= 0.0 ? 1.0 : -1.0;
    const float lado = smoothstep(0.80, 0.55, abs(ng.y));
    const float ax = pow(abs(ng.x), 4.0), az = pow(abs(ng.z), 4.0);
    const float somaL = max(ax + az, 1e-6);
    ProjLado lx = ProjLado(vec2(-wp.z * sx, -wp.y) * ke, vec2(-dpdx.z * sx, -dpdx.y) * ke, vec2(-dpdy.z * sx, -dpdy.y) * ke,
                           lado * ax / somaL, vec3(0.0, 0.0, -sx), vec3(0.0, -1.0, 0.0), vec3(sx, 0.0, 0.0));
    ProjLado lz = ProjLado(vec2(wp.x * sz, -wp.y) * ke, vec2(dpdx.x * sz, -dpdx.y) * ke, vec2(dpdy.x * sz, -dpdy.y) * ke,
                           lado * az / somaL, vec3(sz, 0.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, sz));
    float topo = 1.0 - lado;
    if (lx.w < 0.02) { topo += lx.w; lx.w = 0.0; }
    if (lz.w < 0.02) { topo += lz.w; lz.w = 0.0; }

    // As duas camadas que mais pesam (mesma regra do splat do chao).
    const vec4 w1 = texture(u_textures[nonuniformEXT(u_gbSlots.x)], muv);
    const vec4 w2 = u_gbSlots.y != 0u ? texture(u_textures[nonuniformEXT(u_gbSlots.y)], muv) : vec4(0.0);
    uint slots[8];
    float pesos[8];
    slots[0] =  u_gbSplat.x        & 0xFFFFu; pesos[0] = w1.r;
    slots[1] = (u_gbSplat.x >> 16) & 0xFFFFu; pesos[1] = w1.g;
    slots[2] =  u_gbSplat.y        & 0xFFFFu; pesos[2] = w1.b;
    slots[3] = (u_gbSplat.y >> 16) & 0xFFFFu; pesos[3] = w1.a;
    slots[4] =  u_gbSplat.z        & 0xFFFFu; pesos[4] = w2.r;
    slots[5] = (u_gbSplat.z >> 16) & 0xFFFFu; pesos[5] = w2.g;
    slots[6] =  u_gbSplat.w        & 0xFFFFu; pesos[6] = w2.b;
    slots[7] = (u_gbSplat.w >> 16) & 0xFFFFu; pesos[7] = w2.a;
    const float wSoma = w1.r + w1.g + w1.b + w1.a + w2.r + w2.g + w2.b + w2.a;
    int i0 = -1, i1 = -1;
    float p0 = 0.0, p1 = 0.0;
    for (int k = 0; k < 8; ++k) {
        if ((slots[k] & 0x3FFFu) == 0u || pesos[k] <= 0.0) continue;
        if (pesos[k] > p0) { i1 = i0; p1 = p0; i0 = k; p0 = pesos[k]; }
        else if (pesos[k] > p1) { i1 = k; p1 = pesos[k]; }
    }
    vec3 acc = vec3(0.0);
    float sum = 0.0;
    vec4 accN = vec4(0.0), accM = vec4(0.0);
    float sumN = 0.0, sumM = 0.0;
    if (i0 >= 0) camadaSplat(slots[i0], p0, uv, ddx, ddy, quebrar, topo, lx, lz, tbn, acc, sum, accN, sumN, accM, sumM);
    if (i1 >= 0) camadaSplat(slots[i1], p1, uv, ddx, ddy, quebrar, topo, lx, lz, tbn, acc, sum, accN, sumN, accM, sumM);
    const float wBase = max(0.0, 1.0 - wSoma);
    if (wBase > 0.0) {
        vec3 base = topo > 0.0 ? amostraChao(u_gbSlots.w, uv, ddx, ddy, quebrar, true).rgb * topo : vec3(0.0);
        if (lx.w > 0.0) base += amostraChao(u_gbSlots.w, lx.uv, lx.ddx, lx.ddy, quebrar, true).rgb * lx.w;
        if (lz.w > 0.0) base += amostraChao(u_gbSlots.w, lz.uv, lz.ddx, lz.ddy, quebrar, true).rgb * lz.w;
        acc += base * wBase;
    }
    sum += wBase;
    g.color = acc / max(sum, 1e-4);
    if (!LITE) { // a mesma variacao macro do chao (por pixel, em XZ)
        const float macroLum = eruptionMacroFbm(wp.xz, 1.0 / 180.0);
        const float macroWarm = eruptionMacroFbm(wp.xz + vec2(500.0, 500.0), 1.0 / 260.0);
        g.color *= (1.0 + macroLum * 0.16);
        g.color *= vec3(1.0 + max(macroWarm, 0.0) * 0.12, 1.0 - abs(macroWarm) * 0.05, 1.0 - max(macroWarm, 0.0) * 0.09);
    }

    // Normal: mapa base do chao, depois as camadas pela fracao delas.
    vec3 tn = vec3(0.0, 0.0, 1.0);
    if (u_gbSlots2.x != 0u) tn = amostraChao(u_gbSlots2.x, uv, ddx, ddy, quebrar, false).xyz * 2.0 - 1.0;
    if (sumN > 0.0) tn = mix(tn, (accN.xyz / sumN) * 2.0 - 1.0, sumN / max(sum, 1e-4));
    if (u_normalMapInvertY > 0.5) tn.y = -tn.y;
    g.normal = normalize(tbn * normalize(tn));

    if (u_gbSlots2.y != 0u) { g.mrahw = amostraChao(u_gbSlots2.y, uv, ddx, ddy, quebrar, true); g.mrahwW = 1.0; }
    if (sumM > 0.0) {
        const float f = sumM / max(sum, 1e-4);
        g.mrahw = g.mrahwW > 0.0 ? mix(g.mrahw, accM / sumM, f) : accM / sumM;
        g.mrahwW = max(g.mrahwW, f);
    }

    // Altura com vies pela luminancia: onde o chao e' mais claro que a peca ele
    // sobe um pouco mais, e a borda acompanha o grao em vez de sair reta.
    const float vies = (dot(g.color, vec3(0.299, 0.587, 0.114)) - lumPeca) * 0.5 * faixa;
    g.w = (1.0 - smoothstep(0.0, faixa, h - vies)) * smoothstep(-faixa, -0.25 * faixa, h);
    // Normal do chao: inteira no plano, menos na parede (puxada toda, a parede
    // acendia como chao).
    g.nPeso = mix(0.4, 1.0, smoothstep(0.3, 0.8, ng.y));
    // ERUPTION_TEST_GROUND_BLEND_DEBUG (u_gbSlots2.z): 1 = pesos da mascara,
    // 2 = so' a cor base, 3 = altura acima do chao (verde em cima, vermelho embaixo).
    if (u_gbSlots2.z == 1u) { g.color = w1.rgb; g.w = 1.0; }
    else if (u_gbSlots2.z == 2u) { g.color = amostraChao(u_gbSlots.w, uv, ddx, ddy, quebrar, true).rgb; g.w = 1.0; }
    else if (u_gbSlots2.z == 3u) { g.color = vec3(clamp(-h / faixa, 0.0, 1.0), clamp(h / faixa, 0.0, 1.0), 0.0); g.w = 1.0; }
    return g;
}
