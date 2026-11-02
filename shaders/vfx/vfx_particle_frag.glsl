// Corpo do fragmento das particulas (vfx_particle.frag e a variante de meia
// resolucao, que nao tem o alvo da mascara reativa: VFX_NO_REACTIVE).
// Pixel da particula: folha de quadros (com mistura), particula suave
// (some ao encostar na geometria, sem a linha de corte) e saida
// premultiplicada - aditiva e so' alfa 0, entao tudo cabe num draw.
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

#include "common/slot_textures.glsl"

layout(location = 0) in vec2 v_uv0;
layout(location = 1) in vec2 v_uv1;
layout(location = 2) in vec4 v_color;
layout(location = 3) in float v_frameMix;
layout(location = 4) in float v_viewZ;
layout(location = 5) flat in uvec2 v_texBlend;
layout(location = 6) in float v_soft;
layout(location = 7) in vec2 v_corner;
layout(location = 8) flat in vec4 v_decal;
layout(location = 9) flat in vec4 v_decalRot;

layout(location = 0) out vec4 o_color;
// Mascara reativa do FSR (R8, mistura MAX): onde a particula cobre, o
// upscaler confia menos no historico - sem isso fogo e faisca deixam rastro.
#ifndef VFX_NO_REACTIVE
layout(location = 1) out vec4 o_reactive;
#endif

layout(push_constant) uniform Push {
    uint listOffset;
    float depthScale;
} pc;

void main() {
    vec2 uv0 = v_uv0, uv1 = v_uv1, corner = v_corner;
    bool decal = (v_texBlend.y & 16u) != 0u;
    // Sem discard antes da textura (a derivada do quad sairia indefinida):
    // fora do decal vira cobertura 0, e o alfa zero descarta no fim.
    float cover = 1.0;
    if (decal) {
        // Ponto visivel da cena no espaco da particula deitada.
        ivec2 px = ivec2(gl_FragCoord.xy * pc.depthScale);
        float d = texelFetch(sceneDepth, px, 0).r;
        if (d >= 1.0) cover = 0.0; // ceu
        vec2 suv = (vec2(px) + 0.5) / vec2(textureSize(sceneDepth, 0));
        vec4 vp = u_inverseProjection * vec4(suv * 2.0 - 1.0, d, 1.0);
        vec3 L = (u_inverseView * vec4(vp.xyz / vp.w, 1.0)).xyz - v_decal.xyz;
        float s = max(v_decal.w, 1e-4);
        vec2 rc = L.xz / s;
        corner = vec2(rc.x * v_decalRot.x + rc.y * v_decalRot.y, rc.y * v_decalRot.x - rc.x * v_decalRot.y);
        if (any(greaterThan(abs(corner), vec2(1.0)))) cover = 0.0;
        // So' o chao: o ponto fica dentro de um cone de ~35 graus a partir do
        // centro. Parede, tronco e o corpo de quem esta' em cima ficam de fora.
        if (abs(L.y) > 0.7 * length(L.xz) + 0.1 * s) cover = 0.0;
        corner = clamp(corner, vec2(-1.0), vec2(1.0));
        vec2 uv = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5) * v_decalRot.zw;
        uv0 += uv;
        uv1 += uv;
    }
    vec4 tex;
    if (v_texBlend.x == 0xFFFFFFFFu) {
        // Sem textura: disco suave (queda quadratica ate' a borda).
        float r2 = dot(corner, corner);
        float a = clamp(1.0 - r2, 0.0, 1.0);
        tex = vec4(1.0, 1.0, 1.0, a * a);
    } else {
        uint slot = v_texBlend.x;
        tex = slotTexture(slot, uv0);
        if (v_frameMix > 0.0) tex = mix(tex, slotTexture(slot, uv1), v_frameMix);
    }
    vec4 c = v_color * tex;
    c.a *= cover;

    // Meia resolucao nao tem teste de profundidade por hardware: o pixel
    // compara com a profundidade cheia no centro dele (fumaca e' suave).
    bool lowRes = pc.depthScale > 1.5;
    if (!decal && (v_soft > 0.0 || lowRes)) {
        float d = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy * pc.depthScale), 0).r;
        if (d < 1.0) {
            float n = u_nearPlane, f = u_farPlane;
            float sceneZ = n * f / (f - d * (f - n));
            // v_soft ja' em unidades do mundo
            if (v_soft > 0.0) c.a *= clamp((sceneZ - v_viewZ) / v_soft, 0.0, 1.0);
            else if (sceneZ < v_viewZ) discard;
        }
    }
    if (c.a <= 0.002) discard;
    // Saida premultiplicada. Aditiva: alfa 0 (soma sem escurecer o fundo).
    // "premultiplied": a textura ja' vem multiplicada pelo proprio alfa.
    uint blend = v_texBlend.y & 3u;
    if (blend == 1u) o_color = vec4(c.rgb * v_color.a, c.a);
    else o_color = vec4(c.rgb * c.a, blend == 0u ? 0.0 : c.a);
    // Alfa para a mistura alfa; luminancia somada para a aditiva (que nao
    // tem alfa). Teto 0,9: o FSR ainda usa um pouco do historico (menos ruido).
#ifndef VFX_NO_REACTIVE
    float lum = dot(o_color.rgb, vec3(0.2126, 0.7152, 0.0722));
    // Aditiva (alfa 0) pesa a luminancia x4: faisca fraca e rapida e' a que
    // mais deixa rastro quando o historico a arrasta.
    o_reactive = vec4(min(max(o_color.a, lum * 4.0), 0.9));
#endif
}
