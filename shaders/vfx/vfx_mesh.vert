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

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_color;
layout(location = 3) flat out uint v_tex;
layout(location = 4) out vec3 v_worldPos;
layout(location = 5) out vec4 v_clip;
layout(location = 6) out vec4 v_prevClip;
layout(location = 7) flat out float v_emissive;

// Matrizes sem jitter do frame atual e do anterior (velocidade no G-buffer):
// a malha e' desenhada no passe de geometria, antes da simulacao deste frame.
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
    mat3 R = axisAngle(unpackOct(r.look.w), unpackHalf2x16(r.look.z).x);
    vec3 world = r.posSize.xyz + R * (inPos * r.posSize.w);
    v_normal = R * inNormal;
    v_uv = inUv;
    v_color = vec4(unpackHalf2x16(r.look.x), unpackHalf2x16(r.look.y));
    v_tex = r.sheet.x;
    v_worldPos = world;
    vec2 dz = unpackHalf2x16(r.sheet.y);
    vec3 disp = vec3(unpackHalf2x16(r.sheet.z), dz.x);
    v_emissive = dz.y;
    gl_Position = u_viewProjection * vec4(world, 1.0);
    v_clip = pc.viewProjNoJitter * vec4(world, 1.0);
    v_prevClip = pc.prevViewProjNoJitter * vec4(world - disp, 1.0);
}
