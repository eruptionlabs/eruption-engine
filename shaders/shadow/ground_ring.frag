#version 450

layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_viewProjection;
    mat4 u_inverseView;
    mat4 u_inverseProjection;
};

// Profundidade da cena (so' leitura). D32 sem filtro: texelFetch.
layout(set = 2, binding = 0) uniform sampler2D u_sceneDepth;
// pbr do G-buffer: r = 0,75 marca o pixel de um sprite (sprite.frag).
layout(set = 2, binding = 1) uniform sampler2D u_gbufferPbr;

layout(push_constant) uniform RingPush {
    vec4 center; // xyz = pe', w = raio externo
    vec4 color;  // rgb, a = alfa
    vec4 shape;  // x = borda, y = folga acima, z = folga abaixo, w = forma (0 coroa, 1 retangulo)
    vec4 dir;    // xy = direcao (x, z), z = meia abertura, w = raio interno / meia largura
    vec4 fill;   // x = progresso, y = alfa do miolo
} ring;

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(u_sceneDepth, px, 0).r;
    if (depth >= 1.0) discard; // ceu
    vec2 uv = (vec2(px) + 0.5) / vec2(textureSize(u_sceneDepth, 0));
    vec4 viewPos = u_inverseProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 P = (u_inverseView * vec4(viewPos.xyz / viewPos.w, 1.0)).xyz;
    vec3 local = P - ring.center.xyz;

    // Distancia com sinal a forma no plano XZ (negativa dentro), borda de um
    // pixel (fwidth) para nao serrilhar nem piscar com a forma andando. Na
    // quina de um objeto a derivada salta: limitada a meia espessura.
    // Coroa: entre o raio interno e o externo; com meia abertura < pi vira
    // setor (cone, arco), cortado pelos dois raios da borda. Retangulo: da
    // origem ate' o comprimento, na direcao `dir`.
    vec2 p = vec2(dot(local.xz, ring.dir.xy), dot(local.xz, vec2(-ring.dir.y, ring.dir.x)));
    float R = ring.center.w, sd, frente, meia;
    float d = length(local.xz);
    float aa = max(fwidth(d), 1e-3);
    if (ring.shape.w > 0.5) {
        sd = max(abs(p.x - 0.5 * R) - 0.5 * R, abs(p.y) - ring.dir.w);
        frente = p.x - ring.fill.x * R;
        meia = min(0.5 * R, ring.dir.w);
    } else {
        float ri = max(ring.dir.w, 0.0);
        sd = max(d - R, ri - d);
        if (ring.dir.z < 3.14159) {
            // Setor (como a fatia de pizza de distancia exata): distancia ao
            // raio da borda, com sinal pelo lado.
            vec2 c = vec2(cos(ring.dir.z), sin(ring.dir.z));
            vec2 q = vec2(p.x, abs(p.y));
            float lado = c.x * q.y - c.y * q.x;
            sd = max(sd, sign(lado) * length(q - c * max(dot(q, c), 0.0)));
        }
        frente = d - (ri + ring.fill.x * (R - ri));
        meia = ri > 0.0 ? 0.5 * (R - ri) : R;
    }
    aa = min(aa, meia);
    float dentro = 1.0 - smoothstep(-aa, aa, sd);
    // Miolo ate' a frente do progresso (alfa fill.y), borda de espessura
    // shape.x e a frente do progresso mais forte enquanto anda.
    float miolo = ring.fill.y * (ring.fill.x >= 1.0 ? 1.0 : 1.0 - smoothstep(-aa, aa, frente));
    float bw = ring.shape.x;
    float borda = bw > 0.0 ? 1.0 - smoothstep(-aa, aa, abs(sd + 0.5 * bw) - 0.5 * bw) : 0.0;
    float onda = ring.fill.x > 0.0 && ring.fill.x < 1.0
                     ? 0.8 * (1.0 - smoothstep(0.0, max(bw, 2.0 * aa), abs(frente))) : 0.0;
    float cobre = dentro * max(max(miolo, borda), onda);
    if (cobre <= 0.0) discard;

    // Pixel de sprite (o proprio boneco ou outro) fica de fora: o anel e' do
    // chao. Pelo plano do billboard, como o disco, a rampa de lado cruzava o
    // plano e abria um dente no anel.
    if (abs(texelFetch(u_gbufferPbr, px, 0).r - 0.75) < 0.06) discard;

    // So' o chao perto do pe': parede ou laje muito acima/abaixo nao.
    float altura = smoothstep(-ring.shape.z, -0.5 * ring.shape.z, local.y) *
                   (1.0 - smoothstep(0.5 * ring.shape.y, ring.shape.y, local.y));
    float a = ring.color.a * cobre * altura;
    if (a <= 0.0) discard;
    outColor = vec4(ring.color.rgb, a);
}
