#version 450

// Anel no chao preso a um personagem (characters.ring) e indicador de area
// (vfx.area: circulo, anel, linha, cone, arco), como DECAL: o mesmo
// truque do disco do pe' (billboard_circle_shadow.vert). O quadro cobre, na
// tela, a caixa do anel com folga em altura; o fragmento le' a profundidade e
// avalia a coroa no ponto visivel, entao o anel deita no relevo (rampa,
// degrau) sem brigar com o chao.

layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_viewProjection;
    mat4 u_inverseView;
    mat4 u_inverseProjection;
};

layout(push_constant) uniform RingPush {
    vec4 center; // xyz = pe', w = raio externo
    vec4 color;  // rgb, a = alfa
    vec4 shape;  // x = borda, y = folga acima, z = folga abaixo, w = forma (0 coroa, 1 retangulo; +2 no plano)
    vec4 dir;    // xy = direcao (x, z), z = meia abertura, w = raio interno / meia largura
    vec4 fill;   // x = progresso, y = alfa do miolo, zw = inclinacao do plano (x, z)
} ring;

void main() {
    const vec2 cantos[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    // Cantos no plano: o quadrado do raio (coroa) ou o retangulo da linha,
    // que sai do centro na direcao `dir` com `w` de meia largura.
    float r = ring.center.w;
    vec2 eixo = ring.dir.xy, lado = vec2(-ring.dir.y, ring.dir.x);
    bool linha = mod(ring.shape.w, 2.0) > 0.5;
    vec2 inclina = ring.shape.w > 1.5 ? ring.fill.zw : vec2(0.0);
    vec2 mn = vec2(1e9), mx = vec2(-1e9);
    bool atras = false;
    for (int k = 0; k < 8; ++k) {
        vec2 q = linha ? eixo * ((k & 1) != 0 ? r : 0.0) + lado * ((k & 4) != 0 ? ring.dir.w : -ring.dir.w)
                       : vec2((k & 1) != 0 ? r : -r, (k & 4) != 0 ? r : -r);
        vec3 c = ring.center.xyz + vec3(q.x, dot(inclina, q) + ((k & 2) != 0 ? ring.shape.y : -ring.shape.z), q.y);
        vec4 clip = u_viewProjection * vec4(c, 1.0);
        if (clip.w <= 1e-3) { atras = true; break; }
        mn = min(mn, clip.xy / clip.w);
        mx = max(mx, clip.xy / clip.w);
    }
    if (atras) { mn = vec2(-1.0); mx = vec2(1.0); }
    gl_Position = vec4(mix(mn, mx, cantos[gl_VertexIndex]), 0.0, 1.0);
}
