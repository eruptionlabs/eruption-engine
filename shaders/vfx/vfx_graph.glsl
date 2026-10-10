// Interpretador do GRAFO DE EFEITO (VfxGraph.cpp compila; aqui roda).
// Programa = lista de instrucoes de 32 bits sobre 16 registradores vec4:
//   op(6) | dst(4) | a(4) | b(4) | c(4) | imm(10); Const usa uma 2a palavra
//   com o indice da constante. Emissor sem grafo nao entra aqui (len 0).
// Precisa de vfx_common.glsl incluido antes.

layout(std430, set = VFX_SET, binding = 11) readonly buffer GraphCodeBuf { uint graphCode[]; };
layout(std430, set = VFX_SET, binding = 12) readonly buffer GraphConstBuf { vec4 graphConst[]; };

// Turbulencia sem divergencia (a mesma da simulacao).
vec3 vfxCurl(vec3 p, float t) {
    vec3 a = p + vec3(0.0, t, 0.0);
    vec3 b = p * 1.7 + vec3(t * 0.7, 0.0, 3.1);
    return vec3(1.7 * cos(b.y) - cos(a.z), 1.7 * cos(b.z) - cos(a.x), 1.7 * cos(b.x) - cos(a.y)) * 0.5;
}

vec4 vfxRunGraph(uint off, uint len, Particle p, Emitter e) {
    vec4 r[16];
    uint i = off;
    uint end = off + len;
    float t = clamp(p.age / max(p.life, 1e-4), 0.0, 1.0);
    while (i < end) {
        uint w = graphCode[i++];
        uint op = w & 63u;
        uint d = (w >> 6) & 15u, a = (w >> 10) & 15u, b = (w >> 14) & 15u, c = (w >> 18) & 15u;
        switch (op) {
        case 0u: return r[a];                                         // End
        case 1u: r[d] = graphConst[graphCode[i++]]; break;            // Const
        case 2u: r[d] = vec4(p.age); break;                           // InAge
        case 3u: r[d] = vec4(t); break;                               // InLife
        case 4u: r[d] = vec4(p.pos, 1.0); break;                      // InPos
        case 5u: r[d] = vec4(p.vel, 0.0); break;                      // InVel
        case 6u: r[d] = vec4(rand01(p.seed, 101u + (w >> 22))); break; // InRandom
        case 7u: r[d] = vec4(e.posTime.w); break;                     // InTime
        case 8u: r[d] = vec4(e.posTime.xyz, 1.0); break;              // InEmitterPos
        case 9u: r[d] = vec4(unpackHalf2x16(p.sizeRot).x); break;     // InSize
        case 10u: r[d] = r[a] + r[b]; break;
        case 11u: r[d] = r[a] - r[b]; break;
        case 12u: r[d] = r[a] * r[b]; break;
        case 13u: r[d] = r[a] / (abs(r[b]) + 1e-6) * sign(r[b] + 1e-12); break;
        case 14u: r[d] = min(r[a], r[b]); break;
        case 15u: r[d] = max(r[a], r[b]); break;
        case 16u: r[d] = pow(abs(r[a]), r[b]); break;
        case 17u: r[d] = sin(r[a]); break;
        case 18u: r[d] = cos(r[a]); break;
        case 19u: r[d] = abs(r[a]); break;
        case 20u: r[d] = fract(r[a]); break;
        case 21u: r[d] = floor(r[a]); break;
        case 22u: r[d] = sqrt(max(r[a], vec4(0.0))); break;
        case 23u: r[d] = -r[a]; break;
        case 24u: r[d] = 1.0 - r[a]; break;
        case 25u: r[d] = clamp(r[a], 0.0, 1.0); break;
        case 26u: r[d] = mix(r[a], r[b], r[c]); break;
        case 27u: r[d] = clamp(r[a], r[b], r[c]); break;
        case 28u: r[d] = smoothstep(r[a], r[b], r[c]); break;
        case 29u: r[d] = step(r[a], r[b]); break;
        case 30u: r[d] = vec4(length(r[a].xyz)); break;
        case 31u: { float l = length(r[a].xyz); r[d] = vec4(l > 1e-6 ? r[a].xyz / l : vec3(0.0), 0.0); } break;
        case 32u: r[d] = vec4(dot(r[a].xyz, r[b].xyz)); break;
        case 33u: r[d] = vec4(cross(r[a].xyz, r[b].xyz), 0.0); break;
        case 34u: r[d] = vec4(r[a].x); break;
        case 35u: r[d] = vec4(r[a].y); break;
        case 36u: r[d] = vec4(r[a].z); break;
        case 37u: r[d] = vec4(r[a].w); break;
        case 38u: r[d] = vec4(r[a].x, r[b].x, r[c].x, 0.0); break;    // Combine3
        case 39u: r[d] = vec4(vfxCurl(r[a].xyz, r[b].x), 0.0); break; // Noise
        case 40u: r[d] = mod(r[a], r[b] + 1e-6); break;
        default: break;
        }
    }
    return vec4(0.0);
}
