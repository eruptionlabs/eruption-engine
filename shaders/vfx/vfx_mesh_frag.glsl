// Corpo do fragmento das particulas de malha. Tres saidas:
//   G-buffer (padrao), sem o alvo de velocidade (ERUPTION_NO_VELOCITY) e
//   forward com luz simples (VFX_MESH_FORWARD, previa do editor).
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

#include "common/slot_textures.glsl"

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in uint v_tex;
layout(location = 4) in vec3 v_worldPos;
layout(location = 5) in vec4 v_clip;
layout(location = 6) in vec4 v_prevClip;
layout(location = 7) flat in float v_emissive;
layout(location = 8) in vec3 v_obj;
layout(location = 9) in vec2 v_shade;

#ifdef VFX_MESH_FORWARD
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_reactive;
#else
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPBR;
layout(location = 3) out uint outMaterialID;
layout(location = 4) out vec4 outEmissive;
#ifndef ERUPTION_NO_VELOCITY
layout(location = 5) out vec2 outVelocity;
#endif
#endif

// Ruido de valor 3D (detalhe da superficie no espaco da malha: gruda na
// pedra enquanto ela gira, sem textura).
float hash3(vec3 p) {
    uvec3 q = uvec3(ivec3(floor(p)) + 1024);
    return float(pcg(q.x ^ pcg(q.y ^ pcg(q.z))) >> 8) * (1.0 / 16777216.0);
}
float noise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x), mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y);
    float b = mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x), mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y);
    return mix(a, b, f.z);
}

void main() {
    vec3 albedo = v_color.rgb;
    if (v_tex != 0xFFFFFFFFu) albedo *= slotTexture(v_tex, v_uv).rgb;
    // Superficie: manchas largas + grao fino; a face de fratura e' rocha
    // limpa (mais clara e menos saturada) e o fundo do relevo escurece.
    float n1 = noise3(v_obj);
    float n2 = noise3(v_obj * 4.3 + 11.0);
    albedo *= mix(0.72, 1.18, n1) * mix(0.88, 1.08, n2);
    float fresh = v_shade.y;
    albedo = mix(albedo, mix(albedo, vec3(dot(albedo, vec3(0.333))), 0.35) * 1.3, fresh * 0.6);
    float ao = v_shade.x;
    // Normal da malha (gerada para fora). Nada de gl_FrontFacing: a projecao
    // do Vulkan inverte o giro na tela e a face da frente virava "de tras",
    // com a normal para dentro (pedra preta sob o sol).
    vec3 N = normalize(v_normal);
#ifdef VFX_MESH_FORWARD
    // Previa sem escrita de profundidade: face de tras some (malhas convexas).
    if (dot(N, frame.camPos.xyz - v_worldPos) < 0.0) discard;
#endif
    // Brilho proprio acima de 1 (brasa, magma): concentra nas fendas e no
    // fundo do relevo, a crosta por cima esfria primeiro.
    float crack = 1.0 - smoothstep(0.0, 0.08, abs(n1 - 0.5));
    vec3 glow = albedo * max(v_emissive - 1.0, 0.0) * 0.5 * (0.7 + 0.8 * crack + 1.2 * (1.0 - ao));
#ifdef VFX_MESH_FORWARD
    vec3 L = -normalize(frame.sunDir.xyz);
    vec3 light = frame.ambient.rgb * frame.ambient.w + frame.sunColor.rgb * frame.sunDir.w * max(dot(N, L), 0.0);
    o_color = vec4(albedo * light * ao + glow, 1.0);
    o_reactive = vec4(0.0);
#else
    // o G-buffer guarda sRGB; .a = oclusao (lida pela luz ambiente)
    outAlbedo = vec4(pow(clamp(albedo, 0.0, 1.0), vec3(1.0 / 2.2)), ao);
    outNormal = vec4(N * 0.5 + 0.5, 1.0);
    outPBR = vec4(1.0, mix(0.7, 0.95, n2) - fresh * 0.15, 0.0, 0.0); // fonte real, rugosa, nao metal, seca
    outMaterialID = uint(clamp((v_worldPos.y + 500.0) / 2000.0, 0.0, 1.0) * 255.0 + 0.5);
    outEmissive = vec4(min(glow, vec3(1.0)), 1.0);
#ifndef ERUPTION_NO_VELOCITY
    outVelocity = (v_prevClip.xy / v_prevClip.w - v_clip.xy / v_clip.w) * 0.5;
#endif
#endif
}
