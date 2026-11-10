#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : enable
// BUFFER DE DISTORCAO: os emissores com "distortion" escrevem aqui, em vez da
// cor, um deslocamento de tela (rg, fracao da altura da tela; mistura somada).
// O pos (distortion.frag) entorta a cena por ele. Mesmo vertex shader das
// particulas (vfx_particle.vert), no passe com distortPass = 1.
//   - com textura: rg da textura = direcao (0,5 parado), alfa = mascara
//     (normal map de onda de choque, de gota, de vidro);
//   - sem textura: ondulacao de calor (dois senos em fase com o tempo, presos
//     ao pixel para nao "andar" com a particula) e/ou empurrao radial do
//     centro (anel de onda de choque), pela mistura `distortionRadial`.
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

#include "common/slot_textures.glsl"

layout(location = 0) in vec2 v_uv0;
layout(location = 1) in vec2 v_uv1;
layout(location = 2) in vec4 v_color;      // r = forca, g = radial, a = alfa da vida
layout(location = 3) in float v_frameMix;
layout(location = 4) in float v_viewZ;
layout(location = 5) flat in uvec2 v_texBlend;
layout(location = 6) in float v_soft;
layout(location = 7) in vec2 v_corner;
layout(location = 8) flat in vec4 v_decal;
layout(location = 9) flat in vec4 v_decalRot; // cos, sin do giro da particula

layout(location = 0) out vec4 o_offset;

layout(push_constant) uniform Push {
    uint listOffset;
    float depthScale;
} pc;

void main() {
    vec2 c = v_corner;
    float r2 = dot(c, c);
    // Giro da particula, para a direcao sair na tela (v cresce para baixo).
    vec2 rc = vec2(c.x * v_decalRot.x - c.y * v_decalRot.y, c.x * v_decalRot.y + c.y * v_decalRot.x);
    vec2 screenDir = vec2(rc.x, -rc.y);

    vec2 dir;
    float mask;
    if (v_texBlend.x == 0xFFFFFFFFu) {
        float r = sqrt(r2);
        mask = clamp(1.0 - r2, 0.0, 1.0);
        mask *= mask;
        // Calor: ondulacao presa a' tela, subindo com o tempo.
        vec2 p = gl_FragCoord.xy * (pc.depthScale / 18.0);
        float t = u_time * 3.0;
        vec2 heat = vec2(sin(p.y * 1.7 + t + sin(p.x * 0.9 + t * 0.7)),
                         cos(p.x * 1.3 - t * 1.1 + sin(p.y * 0.8)));
        // Choque: anel (pico no meio do raio), para fora.
        vec2 radial = r > 1e-4 ? screenDir / r * (sin(clamp(r, 0.0, 1.0) * 3.14159265) * 2.0) : vec2(0.0);
        dir = mix(heat, radial, clamp(v_color.g, 0.0, 1.0));
        if (v_color.g > 0.0) mask = mix(mask, step(r, 1.0), clamp(v_color.g, 0.0, 1.0));
    } else {
        vec4 tex = slotTexture(v_texBlend.x, v_uv0);
        if (v_frameMix > 0.0) tex = mix(tex, slotTexture(v_texBlend.x, v_uv1), v_frameMix);
        vec2 n = tex.rg * 2.0 - 1.0;
        // A direcao da textura gira com a particula.
        dir = vec2(n.x * v_decalRot.x - n.y * v_decalRot.y, -(n.x * v_decalRot.y + n.y * v_decalRot.x));
        mask = tex.a;
    }
    float a = mask * clamp(v_color.a, 0.0, 1.0);

    // Some ao encostar na geometria, como a particula suave.
    if (v_soft > 0.0) {
        float d = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy * pc.depthScale), 0).r;
        if (d < 1.0) {
            float n = u_nearPlane, f = u_farPlane;
            float sceneZ = n * f / (f - d * (f - n));
            a *= clamp((sceneZ - v_viewZ) / v_soft, 0.0, 1.0);
        }
    }
    if (a <= 0.002) discard;
    o_offset = vec4(dir * (v_color.r * a), 0.0, 0.0);
}
