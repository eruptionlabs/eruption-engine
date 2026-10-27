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

// Ponto de mundo visivel no pixel `q` (preso a' borda da tela).
vec3 worldAt(ivec2 q) {
    ivec2 tam = textureSize(u_sceneDepth, 0);
    q = clamp(q, ivec2(0), tam - 1);
    float d = texelFetch(u_sceneDepth, q, 0).r;
    vec2 uv = (vec2(q) + 0.5) / vec2(tam);
    vec4 viewPos = u_inverseProjection * vec4(uv * 2.0 - 1.0, d, 1.0);
    return (u_inverseView * vec4(viewPos.xyz / viewPos.w, 1.0)).xyz;
}

// O vizinho mais perto (esquerda/direita ou baixo/cima): na quina de um
// objeto o outro lado e' outra superficie e entortaria a normal.
vec3 tangente(vec3 P, vec3 a, vec3 b) {
    vec3 ta = P - a, tb = b - P;
    return dot(ta, ta) < dot(tb, tb) ? ta : tb;
}

// Cobertura da forma no ponto `xz` (relativo ao centro), com `aa` de borda.
float cobreAt(vec2 xz, float aa0) {
    float aa = aa0;

    // Distancia com sinal a forma no plano XZ (negativa dentro), borda de um
    // pixel (fwidth) para nao serrilhar nem piscar com a forma andando. Na
    // quina de um objeto a derivada salta: limitada a meia espessura.
    // Coroa: entre o raio interno e o externo; com meia abertura < pi vira
    // setor (cone, arco), cortado pelos dois raios da borda. Retangulo: da
    // origem ate' o comprimento, na direcao `dir`.
    vec2 p = vec2(dot(xz, ring.dir.xy), dot(xz, vec2(-ring.dir.y, ring.dir.x)));
    float R = ring.center.w, sd, frente, meia;
    float d = length(xz);
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
    return dentro * max(max(miolo, borda), onda);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(u_sceneDepth, px, 0).r;
    if (depth >= 1.0) discard; // ceu
    vec3 P = worldAt(px);
    vec3 local = P - ring.center.xyz;
    // Chao ACIMA do pe' (crista, calombo do relevo): a forma vale onde o raio
    // da camera cruza o plano do pe', como se o anel estivesse nele e o chao
    // fosse transparente. Pelo ponto visivel, a crista na frente tapava
    // trechos do anel no chao acidentado. As duas leituras coincidem onde o
    // chao cruza a altura do pe', entao o contorno e' um so' (ler as duas e
    // somar dobrava o anel).
    vec2 xz = local.xz;
    bool plano = false;
    if (local.y > 0.0) {
        vec3 cam = u_inverseView[3].xyz;
        vec3 ray = P - cam;
        if (ray.y < -1e-4) {
            xz = (cam + ray * ((ring.center.y - cam.y) / ray.y) - ring.center.xyz).xz;
            plano = true;
        }
    }
    float cobre = cobreAt(xz, max(fwidth(length(xz)), 1e-3));
    if (cobre <= 0.0) discard;

    // Pixel de sprite (o proprio boneco ou outro) fica de fora: o anel e' do
    // chao. Pelo plano do billboard, como o disco, a rampa de lado cruzava o
    // plano e abria um dente no anel.
    if (abs(texelFetch(u_gbufferPbr, px, 0).r - 0.75) < 0.06) discard;

    // So' o chao perto do pe': parede ou laje muito acima/abaixo nao.
    float altura = smoothstep(-ring.shape.z, -0.5 * ring.shape.z, local.y) *
                   (1.0 - smoothstep(0.5 * ring.shape.y, ring.shape.y, local.y));
    // Parede, cerca e paredao de rocha nao: a normal da superficie (do depth,
    // 4 vizinhos a 4 px: a 1 px o degrau miudo do chao virava parede) com y abaixo de 0,5 apaga; 0,62 ja' pinta inteiro, entao a
    // rampa de 45 graus (0,71) fica toda. Lida depois do corte da forma:
    // so' quem a forma cobre paga as 4 leituras.
    vec3 tx = tangente(P, worldAt(px - ivec2(4, 0)), worldAt(px + ivec2(4, 0)));
    vec3 ty = tangente(P, worldAt(px - ivec2(0, 4)), worldAt(px + ivec2(0, 4)));
    vec3 n = cross(tx, ty);
    float ny = abs(n.y) * inversesqrt(max(dot(n, n), 1e-20));
    float a = ring.color.a * cobre * altura * (plano ? 1.0 : smoothstep(0.5, 0.62, ny));
    if (a <= 0.0) discard;
    outColor = vec4(ring.color.rgb, a);
}
