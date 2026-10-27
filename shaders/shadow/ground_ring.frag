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
    vec4 shape;  // x = espessura, y = folga acima, z = folga abaixo
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

    // Coroa no plano XZ: distancia ao meio da faixa, borda de um pixel
    // (fwidth) para nao serrilhar nem piscar com o anel andando. Na quina de
    // um objeto a derivada salta: limitada a meia espessura.
    float d = length(local.xz);
    float meia = 0.5 * ring.shape.x;
    float faixa = abs(d - (ring.center.w - meia)) - meia;
    float aa = min(max(fwidth(d), 1e-3), meia);
    float cobre = 1.0 - smoothstep(-aa, aa, faixa);
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
