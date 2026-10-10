#version 450
#extension GL_GOOGLE_include_directive : require
// SOMBRA DA PARTICULA DE MALHA: mesma forma do vfx_mesh.vert, so' que pela
// matriz da cascata (viewProjNoJitter do push carrega a matriz da luz).
// Set 0 proprio (so' lista e registros): o set de desenho pode mudar
// depois do passe de sombra no mesmo frame.
#define VFX_SET 0
#include "vfx/vfx_common.glsl"

layout(location = 0) in vec3 inPos;

layout(push_constant) uniform Push {
    mat4 viewProjNoJitter;
    mat4 prevViewProjNoJitter;
    uint listOffset;
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
    uint seed = r.sheet.w;
    mat3 R = axisAngle(unpackOct(r.look.w), unpackHalf2x16(r.look.z).x);
    vec3 sc = vec3(1.0, mix(0.55, 1.0, rand01(seed, 51u)), mix(0.7, 1.1, rand01(seed, 52u)));
    // Encolhe junto com a malha (fim da vida).
    float a = unpackHalf2x16(r.look.y).y;
    // Encolhe pelo ponto de apoio (a casca de baixo), nao pelo centro: a
    // pedra no chao afunda sumindo em vez de flutuar encolhendo.
    float k = smoothstep(0.0, 1.0, a);
    vec3 world = r.posSize.xyz - vec3(0.0, (1.0 - k) * r.posSize.w * 0.6, 0.0) + R * (inPos * sc * r.posSize.w * k);
    gl_Position = pc.viewProjNoJitter * vec4(world, 1.0);
}
