// Nascimento de uma particula (comum ao nascimento normal e aos
// sub-emissores). Precisa de vfx_common.glsl e vfx_graph.glsl antes.

const float PI = 3.14159265;

vec3 randomUnit(uint seed, uint salt) {
    float z = rand01(seed, salt) * 2.0 - 1.0;
    float a = rand01(seed, salt + 1u) * 2.0 * PI;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), z, r * sin(a));
}

// Direcao dentro de um cone de meia-abertura `halfAngle` em torno de `axis`.
vec3 coneDir(vec3 axis, float halfAngle, uint seed) {
    float cosMax = cos(halfAngle);
    float z = mix(cosMax, 1.0, rand01(seed, 11u));
    float a = rand01(seed, 12u) * 2.0 * PI;
    float r = sqrt(max(0.0, 1.0 - z * z));
    vec3 local = vec3(r * cos(a), r * sin(a), z);
    vec3 up = abs(axis.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 t = normalize(cross(up, axis));
    vec3 b = cross(axis, t);
    return t * local.x + b * local.y + axis * local.z;
}

vec3 shapePoint(Emitter e, uint seed) {
    uint shape = e.flags.x & VFXF_SHAPE_MASK;
    bool surface = (e.flags.x & VFXF_SURFACE) != 0u;
    vec3 s = e.shape.xyz;
    if (shape == 1u) { // esfera
        float r = surface ? 1.0 : pow(rand01(seed, 21u), 1.0 / 3.0);
        return randomUnit(seed, 22u) * s.x * r;
    }
    if (shape == 2u) { // caixa
        vec3 q = vec3(rand01(seed, 23u), rand01(seed, 24u), rand01(seed, 25u)) * 2.0 - 1.0;
        if (surface) {
            uint axis = pcg(seed ^ 26u) % 3u;
            q[axis] = q[axis] < 0.0 ? -1.0 : 1.0;
        }
        return q * s;
    }
    if (shape == 3u || shape == 4u) { // cone (base) e disco: no plano XZ
        float r = surface ? 1.0 : sqrt(rand01(seed, 27u));
        float a = rand01(seed, 28u) * 2.0 * PI;
        return vec3(cos(a), 0.0, sin(a)) * s.x * r;
    }
    return vec3(0.0);
}

// Tira um indice da lista de mortas; falso = pool cheio (devolve o contador).
bool vfxPopDead(out uint idx) {
    uint before = atomicAdd(counters[VFXC_DEAD], 0xFFFFFFFFu);
    if (before == 0u || before > frame.counts.y) {
        atomicAdd(counters[VFXC_DEAD], 1u);
        idx = 0u;
        return false;
    }
    idx = deadList[before - 1u];
    return true;
}

// Particula nova do emissor `e` (slot `slot`) em `origin`, com `extraVel`
// somada (heranca do efeito ou do pai) e envelhecida `preAge` segundos.
Particle vfxMakeParticle(Emitter e, uint slot, vec3 origin, vec3 extraVel, uint seed, float preAge) {
    mat3 R = emitterRot(e);
    float scale = e.rot0.w;
    vec3 local = shapePoint(e, seed);
    vec3 axis = normalize(e.dirSpread.xyz + vec3(0.0, 1e-6, 0.0));
    uint shape = e.flags.x & VFXF_SHAPE_MASK;
    float halfAngle = shape == 3u ? e.shape.w : e.dirSpread.w * PI;
    vec3 dir = halfAngle > 1e-4 ? coneDir(axis, halfAngle, seed) : axis;
    float speed = mix(e.life.z, e.life.w, rand01(seed, 2u)) * scale;

    Particle p;
    p.vel = R * dir * speed + extraVel;
    p.pos = origin + R * (local * scale);
    p.age = 0.0;
    p.life = mix(e.life.x, e.life.y, rand01(seed, 3u));
    p.slotGen = slot | ((e.flags.y & 0xFFFFu) << 16);
    p.seed = seed;
    float size = mix(e.sizeRot.x, e.sizeRot.y, rand01(seed, 4u)) * scale;
    float rot = mix(e.sizeRot.z, e.sizeRot.w, rand01(seed, 5u));
    p.sizeRot = packHalf2x16(vec2(size, rot));
    // Grafo: velocidade extra no nascimento (em m/s, escalada para o mundo).
    if (e.graphA.y > 0u) p.vel += vfxRunGraph(e.graphA.x, e.graphA.y, p, e).xyz * scale;
    p.pos += p.vel * preAge;
    p.age = preAge;
    float av = mix(e.angVel.x, e.angVel.y, rand01(seed, 6u));
    float startFrame = (e.flags.x & VFXF_FLIP_RANDOM) != 0u ? rand01(seed, 7u) : 0.0;
    p.angVel = packHalf2x16(vec2(av, startFrame));
    return p;
}
