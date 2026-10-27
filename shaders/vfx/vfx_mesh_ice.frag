#version 450
#extension GL_GOOGLE_include_directive : require
// MALHA TRANSLUCIDA (gelo, vidro, cristal): desenhada no lit depois da luz,
// antes das particulas. A cena atras vem da copia do lit em meia resolucao,
// desviada pela normal (refracao) e tingida pela cor da particula conforme a
// espessura atravessada (Beer-Lambert). Por cima: Fresnel de Schlick (ceu na
// borda) e o brilho do sol. Malha convexa: a face de tras some.
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

layout(set = VFX_SET, binding = 17) uniform sampler2D litCopy;

layout(location = 0) in vec3 v_normal;
layout(location = 2) in vec4 v_color;
layout(location = 4) in vec3 v_worldPos;
layout(location = 8) in vec3 v_obj;
layout(location = 9) in vec2 v_shade;
layout(location = 10) flat in vec3 v_ice;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_reactive;

// Distancia da camera ao que a profundidade da cena guarda em `uv`.
float sceneDistance(vec2 uv) {
    float d = textureLod(sceneDepth, uv, 0.0).r;
    vec4 vp = u_inverseProjection * vec4(uv * 2.0 - 1.0, d, 1.0);
    return length(vp.xyz / vp.w);
}

void main() {
    vec3 N = normalize(v_normal);
    vec3 toCam = frame.camPos.xyz - v_worldPos;
    float dist = length(toCam);
    vec3 V = toCam / max(dist, 1e-4);
    if (dot(N, V) <= 0.0) discard;
    // Face plana de gelo nunca e' lisa: ondulacao (soma de senos no espaco
    // da malha, gruda no bloco) espalha o brilho do sol em faiscas e entorta
    // a refracao dentro da face. Senos e nao ruido com hash: 1/10 do custo.
    vec3 q = v_obj * 4.0;
    vec3 g = cos(q.yzx * 1.7 + q) * 0.06 + cos(q.zxy * 3.1 - q.yzx) * 0.035;
    g -= N * dot(g, N);
    N = normalize(N + g);
    float NdotV = max(dot(N, V), 0.02);
    vec2 res = vec2(textureSize(sceneDepth, 0));
    vec2 uv = gl_FragCoord.xy / res;

    // Espessura: a corda pela malha (tamanho / cos), cortada pelo que esta'
    // atras. Encostada no chao a corda vai a zero e o gelo some no contato
    // em vez de cortar reto.
    float size = max(v_ice.z, 1e-3);
    float gap = max(sceneDistance(uv) - dist, 0.0);
    float thick = min(size * 0.9 / max(NdotV, 0.25), gap);
    float contact = smoothstep(0.0, size * 0.25, gap);

    // Refracao: desvio pela normal na tela, maior quanto mais grosso. Se o
    // ponto desviado cai em algo NA FRENTE do gelo, fica sem desvio (a
    // copia nao tem o que esta' atras daquilo).
    vec3 nv = mat3(u_view) * N;
    vec2 off = vec2(nv.x, -nv.y) * v_ice.x * min(thick / size, 1.0);
    vec2 ruv = clamp(uv + off, vec2(0.0), vec2(1.0));
    if (sceneDistance(ruv) < dist) ruv = uv;
    vec3 behind = textureLod(litCopy, ruv, 0.0).rgb;

    // Tinta: cor da particula = o que passa por metro (absorcao em mundo).
    vec3 tint = clamp(v_color.rgb, vec3(0.02), vec3(1.0));
    vec3 trans = exp(log(tint) * v_ice.y * thick);

    // Fresnel (gelo n = 1,31: F0 = 0,018), ceu refletido na borda e sol.
    float fres = 0.018 + 0.982 * pow(1.0 - NdotV, 5.0);
    vec3 L = -normalize(frame.sunDir.xyz);
    vec3 R = reflect(-V, N);
    vec3 sky = frame.ambient.rgb * frame.ambient.w * (0.55 + 0.45 * clamp(R.y, 0.0, 1.0));
    vec3 H = normalize(L + V);
    float NdotL = max(dot(N, L), 0.0);
    // Blinn-Phong normalizado ((n + 8) / 8pi): lobo largo da face e um bem
    // estreito, o disco do sol batendo na ondulacao (faisca que acende o bloom).
    float NdotH = max(dot(N, H), 0.0);
    float lobe = pow(NdotH, 250.0) * (258.0 / 25.13) + pow(NdotH, 4000.0) * (4008.0 / 25.13);
    float spec = lobe * NdotL * (0.018 + 0.982 * pow(1.0 - max(dot(H, V), 0.0), 5.0));
    vec3 sun = frame.sunColor.rgb * frame.sunDir.w;
    // Um pouco de luz espalhada dentro (gelo nao e' vidro limpo).
    vec3 scatter = tint * (sky + sun * NdotL) * 0.06 * v_shade.x;

    vec3 ice = (behind * trans + scatter) * (1.0 - fres) + sky * fres + sun * spec;
    // Fim da vida e contato: volta para a cena sem gelo (premultiplicada:
    // onde a < 1 o lit cheio continua por baixo, sem a copia borrada).
    float a = clamp(v_color.a, 0.0, 1.0) * contact;
    o_color = vec4(ice * a, a);
    o_reactive = vec4(0.5 * a);
}
