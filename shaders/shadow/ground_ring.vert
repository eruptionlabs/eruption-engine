#version 450

// Anel no chao preso a um personagem (characters.ring), como DECAL: o mesmo
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
    vec4 shape;  // x = espessura, y = folga acima, z = folga abaixo
} ring;

void main() {
    const vec2 cantos[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    float r = ring.center.w;
    vec2 mn = vec2(1e9), mx = vec2(-1e9);
    bool atras = false;
    for (int k = 0; k < 8; ++k) {
        vec3 c = ring.center.xyz + vec3((k & 1) != 0 ? r : -r, (k & 2) != 0 ? ring.shape.y : -ring.shape.z,
                                        (k & 4) != 0 ? r : -r);
        vec4 clip = u_viewProjection * vec4(c, 1.0);
        if (clip.w <= 1e-3) { atras = true; break; }
        mn = min(mn, clip.xy / clip.w);
        mx = max(mx, clip.xy / clip.w);
    }
    if (atras) { mn = vec2(-1.0); mx = vec2(1.0); }
    gl_Position = vec4(mix(mn, mx, cantos[gl_VertexIndex]), 0.0, 1.0);
}
