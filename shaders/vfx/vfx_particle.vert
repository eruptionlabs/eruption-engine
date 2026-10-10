#version 450
#extension GL_GOOGLE_include_directive : require
// Desenho das particulas: 6 vertices por particula (dois triangulos), a
// particula vem da lista de desenho montada pela simulacao (draw indireto).
// Cor, tamanho e quadro da folha saem das curvas da vida aqui, sem gravar
// nada por particula; a luz tambem e' por particula (por vertice), nao por pixel.
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

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    uint idx = renderList[gl_InstanceIndex];
    Particle p = particles[idx];
    Emitter e = emitters[p.slotGen & 0xFFFFu];
    float t = clamp(p.age / p.life, 0.0, 1.0);

    vec2 sr = unpackHalf2x16(p.sizeRot);
    vec2 av = unpackHalf2x16(p.angVel);
    float size = sr.x * sampleSizeLut(e, t);
    float rot = sr.y + av.x * p.age;

    vec3 camRight = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 camUp = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);
    // Profundidade de vista pelo w do clip (independe da convencao da matriz).
    float viewZ = (u_viewProjection * vec4(p.pos, 1.0)).w;
    vec3 toCam = normalize(u_cameraPos - p.pos);

    // Teto de tamanho na tela: a particula que encosta na camera nao vira
    // uma parede de overdraw (o custo de fumaca e' de fill-rate).
    float proj11 = abs(u_projection[1][1]);
    size = min(size, e.render.z * max(viewZ, 1e-3) / max(proj11, 1e-3));

    vec2 c = kCorners[gl_VertexIndex];
    float cs = cos(rot), sn = sin(rot);
    vec2 rc = vec2(c.x * cs - c.y * sn, c.x * sn + c.y * cs);

    uint facing = (e.flags.x >> VFXF_FACING_SHIFT) & 3u;
    vec3 world;
    if (facing == 1u && dot(p.vel, p.vel) > 1e-8) {
        // Alinhada a velocidade e esticada: faisca, chuva, rastro.
        vec3 axis = normalize(p.vel);
        vec3 side = cross(axis, toCam);
        side = dot(side, side) > 1e-8 ? normalize(side) : camRight;
        float len = size + e.angVel.z * length(p.vel); // velocidade ja' em unidades do mundo
        world = p.pos + side * c.x * size + axis * c.y * len;
    } else if (facing == 2u) {
        // Deitada no chao (onda de choque, marca, poca).
        world = p.pos + vec3(rc.x, 0.0, rc.y) * size;
    } else {
        world = p.pos + (camRight * rc.x + camUp * rc.y) * size;
    }
    gl_Position = u_viewProjection * vec4(world, 1.0);

    // Folha de quadros: ao longo da vida ou em quadros/s, com o seguinte
    // misturado (16 quadros parecem muitos mais).
    uint cols = max(e.flags.w & 0xFFu, 1u);
    uint rows = max((e.flags.w >> 8) & 0xFFu, 1u);
    float frames = float(cols * rows);
    // flip.z: 0 = ao longo da vida, 1 = quadros/s, 2 = quadro sorteado fixo.
    float fpos = e.flip.z > 1.5 ? floor(av.y * frames)
               : (e.flip.z > 0.5 ? p.age * e.flip.x : t * (frames - 0.001)) + av.y * frames;
    float f0 = mod(floor(fpos), frames);
    float f1 = mod(f0 + 1.0, frames);
    vec2 cell = vec2(1.0 / float(cols), 1.0 / float(rows));
    vec2 uv = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    v_uv0 = (vec2(mod(f0, float(cols)), floor(f0 / float(cols))) + uv) * cell;
    v_uv1 = (vec2(mod(f1, float(cols)), floor(f1 / float(cols))) + uv) * cell;
    v_frameMix = ((e.flags.x & VFXF_FLIP_BLEND) != 0u && e.flip.z < 1.5) ? fract(fpos) : 0.0;

    // Cor ao longo da vida, emissivo e luz da cena por particula.
    vec4 col = unpackColorLut(e, t);
    col.rgb *= e.render.x;
    if (e.render.y > 0.0) {
        vec3 L = -normalize(u_sunDir.xyz);
        // Normal falsa entre "para a camera" e "para cima": o volume da
        // nuvem de particulas recebe sol por cima, como fumaca de verdade.
        vec3 n = normalize(toCam + vec3(0.0, 1.0, 0.0));
        float wrap = clamp(dot(n, L) * 0.5 + 0.5, 0.0, 1.0);
        vec3 light = u_ambientSky.rgb * u_ambientSky.w + u_sunColor.rgb * u_sunDir.w * wrap;
        col.rgb *= mix(vec3(1.0), light, e.render.y);
    }
    // Some perto da camera em vez de cobrir a tela.
    col.a *= smoothstep(u_nearPlane, u_nearPlane + max(e.render.w * e.rot0.w, 1e-3), viewZ);
    v_color = col;
    v_viewZ = viewZ;
    v_texBlend = uvec2(e.flags.z, (e.flags.x >> VFXF_BLEND_SHIFT) & 3u);
    v_soft = e.angVel.w * e.rot0.w;
    v_corner = c;
}
