#version 450
#extension GL_GOOGLE_include_directive : require
// Desenho das particulas: o poligono de 8 lados de cada particula (draw
// indexado: 8 vertices, 18 indices), lido do REGISTRO DE DESENHO que a
// simulacao escreveu (cor, tamanho, quadro, luz ja' resolvidos).
#define VFX_SET 2
#include "vfx/vfx_common.glsl"
#include "vfx/vfx_frame_ubo.glsl"

layout(location = 0) out vec2 v_uv0;
layout(location = 1) out vec2 v_uv1;
layout(location = 2) out vec4 v_color;      // rgb linear (ja' com luz e emissivo), a
layout(location = 3) out float v_frameMix;
layout(location = 4) out float v_viewZ;
layout(location = 5) flat out uvec2 v_texBlend; // x textura (0xFFFFFFFF = disco), y mistura
layout(location = 6) out float v_soft;
layout(location = 7) out vec2 v_corner;

layout(push_constant) uniform Push {
    uint listOffset;  // inicio da lista no buffer de desenho
    float depthScale; // 1 = resolucao cheia, 2 = meia resolucao
} pc;

// Poligono de 8 lados recortado pela textura: cada lado k tem normal a k*45
// graus e fica no alcance do conteudo (alfa) naquela direcao, medido na
// importacao. Disco/fumaca viram octogono (~17% a 35% menos pixels que o
// quad); textura cheia ate' o canto continua quadrada.
float cutDist(uvec4 cut, uint k) {
    vec2 h = unpackHalf2x16(cut[k >> 1]);
    return (k & 1u) == 0u ? h.x : h.y;
}
vec2 ringPoint(uvec4 cut, uint k) {
    const float s = 0.70710678;
    vec2 n0 = vec2(cos(float(k) * 0.78539816), sin(float(k) * 0.78539816));
    uint k1 = (k + 1u) & 7u;
    vec2 n1 = vec2(cos(float(k1) * 0.78539816), sin(float(k1) * 0.78539816));
    float d0 = cutDist(cut, k), d1 = cutDist(cut, k1);
    // Intersecao das retas n0.p = d0 e n1.p = d1 (det = sin 45).
    vec2 p = vec2(d0 * n1.y - d1 * n0.y, n0.x * d1 - n1.x * d0) / s;
    return clamp(p, vec2(-1.0), vec2(1.0));
}

void main() {
    uint idx = renderList[pc.listOffset + gl_InstanceIndex];
    RenderRec r = recs[idx];
    vec3 pos = r.posSize.xyz;
    float size = r.posSize.w;
    vec2 rs = unpackHalf2x16(r.look.z); // rotacao, comprimento esticado

    vec3 camRight = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 camUp = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);
    vec3 toCam = normalize(u_cameraPos - pos);

    vec2 c = ringPoint(r.cutout, uint(gl_VertexIndex) & 7u);
    float cs = cos(rs.x), sn = sin(rs.x);
    vec2 rc = vec2(c.x * cs - c.y * sn, c.x * sn + c.y * cs);

    uint facing = (r.sheet.y >> 18) & 3u;
    vec3 world;
    if (facing == 1u) {
        // Alinhada a velocidade e esticada: faisca, chuva, rastro.
        vec3 axis = unpackOct(r.look.w);
        vec3 side = cross(axis, toCam);
        side = dot(side, side) > 1e-8 ? normalize(side) : camRight;
        world = pos + side * c.x * size + axis * c.y * (size + rs.y);
    } else if (facing == 2u) {
        // Deitada no chao (onda de choque, marca, poca).
        world = pos + vec3(rc.x, 0.0, rc.y) * size;
    } else {
        world = pos + (camRight * rc.x + camUp * rc.y) * size;
    }
    gl_Position = u_viewProjection * vec4(world, 1.0);

    // Quadro da folha e o seguinte (misturado no pixel).
    uint cols = max(r.sheet.y & 0xFFu, 1u);
    uint rows = max((r.sheet.y >> 8) & 0xFFu, 1u);
    float frames = float(cols * rows);
    float f0 = float(r.sheet.z & 0xFFFFu);
    float f1 = mod(f0 + 1.0, frames);
    vec2 cell = vec2(1.0 / float(cols), 1.0 / float(rows));
    vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    v_uv0 = (vec2(mod(f0, float(cols)), floor(f0 / float(cols))) + uv) * cell;
    v_uv1 = (vec2(mod(f1, float(cols)), floor(f1 / float(cols))) + uv) * cell;
    v_frameMix = float(r.sheet.z >> 16) / 255.0;

    v_color = vec4(unpackHalf2x16(r.look.x), unpackHalf2x16(r.look.y));
    v_viewZ = (u_viewProjection * vec4(pos, 1.0)).w;
    v_texBlend = uvec2(r.sheet.x, (r.sheet.y >> 16) & 3u);
    v_soft = uintBitsToFloat(r.sheet.w);
    v_corner = c;
}
