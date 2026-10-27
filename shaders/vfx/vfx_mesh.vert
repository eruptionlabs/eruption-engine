#version 450
#extension GL_GOOGLE_include_directive : require
// PARTICULA DE MALHA: instancia = particula (lista da malha montada pela
// simulacao), vertice = malha embutida. Gira em torno de um eixo sorteado.
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec2 inShade; // x oclusao do relevo, y face de fratura

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_color;
layout(location = 3) flat out uint v_tex;
layout(location = 4) out vec3 v_worldPos;
layout(location = 5) out vec4 v_clip;
layout(location = 6) out vec4 v_prevClip;
layout(location = 7) flat out float v_emissive;
layout(location = 8) out vec3 v_obj;   // posicao na malha (detalhe da superficie)
layout(location = 9) out vec2 v_shade;
layout(location = 10) flat out vec3 v_ice; // translucida: refracao, absorcao, tamanho

// Matrizes sem jitter do frame atual e do anterior (velocidade no G-buffer):
// a malha e' desenhada no passe de geometria, antes da simulacao deste frame.
// Bloco no set 3 (PushRing, UBO de offset dinamico): 144 B passa dos 128 de push constant.
layout(std140, set = 3, binding = 0) uniform Push {
    mat4 viewProjNoJitter;
    mat4 prevViewProjNoJitter;
    uint listOffset;
    uint translucent; // 0: passe opaco (G-buffer), 1: passe translucido
} pc;

mat3 axisAngle(vec3 a, float ang) {
    float c = cos(ang), s = sin(ang), t = 1.0 - c;
    return mat3(t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y,
                t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x,
                t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c);
}

void main() {
    uint idx = renderList[pc.listOffset + gl_InstanceIndex];
    RenderRec r = recs[idx];
    // Cada malha num passe so': fora do dela, o triangulo cai fora da tela.
    if ((r.cutout.x != 0u) != (pc.translucent != 0u)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    uint seed = r.sheet.w;
    mat3 R = axisAngle(unpackOct(r.look.w), unpackHalf2x16(r.look.z).x);
    // Cada particula com a sua forma e o seu tom: escala por eixo e brilho
    // sorteados da semente (a mesma malha vira pedras diferentes).
    vec3 sc = vec3(1.0, mix(0.55, 1.0, rand01(seed, 51u)), mix(0.7, 1.1, rand01(seed, 52u)));
    // Fim da vida: a pedra encolhe ate' sumir (o alfa da cor manda). Mais
    // limpo que pontilhado, que vira xadrez quando o TAA nao resolve.
    float a = unpackHalf2x16(r.look.y).y;
    // Encolhe pelo ponto de apoio (a casca de baixo), nao pelo centro: a
    // pedra no chao afunda sumindo em vez de flutuar encolhendo.
    float k = smoothstep(0.0, 1.0, a);
    vec3 world = r.posSize.xyz - vec3(0.0, (1.0 - k) * r.posSize.w * 0.6, 0.0) + R * (inPos * sc * r.posSize.w * k);
    v_normal = R * normalize(inNormal / sc);
    v_uv = inUv;
    v_color = vec4(unpackHalf2x16(r.look.x), unpackHalf2x16(r.look.y));
    float tone = mix(0.7, 1.2, rand01(seed, 53u));
    float warm = rand01(seed, 54u) * 2.0 - 1.0;
    v_color.rgb *= tone * vec3(1.0 + 0.07 * warm, 1.0, 1.0 - 0.07 * warm);
    v_tex = r.sheet.x;
    v_worldPos = world;
    v_obj = inPos * 2.5 + vec3(rand01(seed, 55u), rand01(seed, 56u), rand01(seed, 57u)) * 37.0;
    v_shade = inShade;
    vec2 dz = unpackHalf2x16(r.sheet.y);
    vec3 disp = vec3(unpackHalf2x16(r.sheet.z), dz.x);
    v_emissive = dz.y;
    v_ice = vec3(unpackHalf2x16(r.cutout.y), r.posSize.w * k);
    gl_Position = u_viewProjection * vec4(world, 1.0);
    v_clip = pc.viewProjNoJitter * vec4(world, 1.0);
    v_prevClip = pc.prevViewProjNoJitter * vec4(world - disp, 1.0);
}
