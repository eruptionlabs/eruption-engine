// Estruturas e utilitarios comuns do sistema de particulas (VfxSystem).
// O layout em std430 precisa bater com VfxGpu.hpp.
//
// Antes do #include, defina VFX_SET com o numero do set dos buffers do VFX
// (0 nos compute, 2 no desenho, onde 0 e' o bindless e 1 o FrameUBO).

#ifndef VFX_SET
#define VFX_SET 0
#endif
// Desenho le' os buffers sem escrever: sem `readonly` o vertex shader pediria
// vertexPipelineStoresAndAtomics, opcional (e ausente em muito celular).
#ifdef VFX_WRITABLE
#define VFX_RW
#else
#define VFX_RW readonly
#endif

// Uma particula: 48 bytes.
struct Particle {
    vec3 pos;
    float age;
    vec3 vel;
    float life;
    uint slotGen;  // emissor (16 bits baixos) | geracao do emissor (16 altos)
    uint seed;
    uint sizeRot;  // half2(tamanho inicial, rotacao inicial em rad)
    uint angVel;   // half2(velocidade angular rad/s, quadro inicial 0..1)
};

// Parametros de um emissor ativo (um por instancia x emissor), 448 bytes.
struct Emitter {
    vec4 posTime;     // xyz posicao no mundo, w tempo do efeito
    vec4 prevPos;     // xyz posicao no frame anterior (nascimento interpolado)
    vec4 rot0;        // colunas da rotacao do efeito; rot0.w = escala
    vec4 rot1;
    vec4 rot2;
    vec4 velInherit;  // xyz velocidade do efeito, w heranca
    vec4 shape;       // xyz tamanho da forma, w abertura do cone (rad)
    uvec4 flags;      // x bits (ver VFXF_*), y geracao, z textura (bindless), w colunas | linhas<<8
    vec4 life;        // x,y vida min/max; z,w velocidade min/max
    vec4 dirSpread;   // xyz direcao (espaco do efeito), w espalhamento 0..1
    vec4 sizeRot;     // x,y tamanho min/max; z,w rotacao min/max (rad)
    vec4 angVel;      // x,y vel. angular min/max (rad/s); z esticar; w distancia suave
    vec4 gravityDrag; // xyz gravidade, w arrasto
    vec4 noise;       // x forca, y frequencia, z velocidade, w quique
    vec4 collide;     // x altura do plano, y atrito, z perda de vida por batida
    vec4 render;      // x emissivo, y iluminada 0..1, z tamanho max na tela, w some perto da camera
    vec4 flip;        // x quadros/s, y total de quadros, z modo (0 vida, 1 fps, 2 sorteado)
    uint colorLut[16];// RGBA8 (cor * emissivo vai no shader), 16 amostras da vida
    uint sizeLut[8];  // 16 halfs: tamanho ao longo da vida
    uvec4 cutout;     // 8 halfs: poligono justo da textura (alcance em 8 direcoes)
    uvec4 graphA;     // grafo: x,y = velocidade inicial (inicio, tamanho); z,w = forca
    uvec4 graphB;     // grafo: x,y = cor; z,w = tamanho
    uvec4 ribbon;     // fita: x inicio do anel, y tamanho do anel (0 = nao e' fita)
    uvec4 events;     // sub-emissores: x filho na morte (slot+1), y filho na batida,
                      // z quantidades (8 bits cada) e herancas (8 bits, /255),
                      // w probabilidades (8 bits cada, /255)
};

// Bits de Emitter.flags.x
const uint VFXF_SHAPE_MASK   = 0x7u;
const uint VFXF_SURFACE      = 1u << 3;
const uint VFXF_FACING_SHIFT = 4u;     // 2 bits
const uint VFXF_BLEND_SHIFT  = 6u;     // 2 bits
const uint VFXF_FLIP_BLEND   = 1u << 8;
const uint VFXF_FLIP_RANDOM  = 1u << 9;
const uint VFXF_COLLIDE_PLANE= 1u << 10;
const uint VFXF_COLLIDE_DEPTH= 1u << 11;
const uint VFXF_KILLED       = 1u << 12; // emissor removido: particulas morrem ja'
const uint VFXF_SPAWNING     = 1u << 13;
const uint VFXF_LOWRES       = 1u << 14; // desenha em meia resolucao (fumaca)
const uint VFXF_HIDDEN       = 1u << 15; // fora da visao de equipe: simula, nao desenha
const uint VFXF_MESH_SHIFT   = 16u;     // 4 bits: malha da particula + 1 (0 = sprite)
const uint VFXF_MESHLO_SHIFT = 20u;     // 4 bits: versao de longe da malha + 1 (0 = nao tem)
const uint VFXF_DECAL        = 1u << 24; // deitada e projetada na profundidade da cena

// Pedido de nascimento (CPU -> GPU): `count` particulas do emissor `slot`,
// ocupando [first, first+count) da fila de nascimento deste frame.
struct SpawnRequest {
    uint slot;
    uint count;
    uint first;
    uint seed;
    uint seqBase; // fita: numero de sequencia da primeira particula do pedido
    uint pad0, pad1, pad2;
};

// Parametros do frame (CPU -> GPU, um buffer por frame em voo).
struct FrameParams {
    mat4 viewProj;      // sem jitter (colisao com a profundidade)
    mat4 invViewProj;
    vec4 frustum[6];    // planos (xyz normal para dentro, w distancia)
    vec4 camPos;        // xyz camera, w dt
    vec4 timeInfo;      // x tempo, y total de pedidos, z total a nascer, w perto
    vec4 depthInfo;     // x largura, y altura, z longe, w 1 = profundidade valida
    uvec4 counts;       // x lista atual (0/1), y capacidade total, z ordenar (0/1)
    vec4 sunDir;        // xyz direcao em que a luz viaja, w intensidade
    vec4 sunColor;
    vec4 ambient;       // xyz cor do ceu, w intensidade
    vec4 viewInfo;      // x proj[1][1] (teto de tamanho na tela)
    uvec4 meshIndexCount[2]; // indices de cada malha de particula (8)
    vec4 groundInfo;    // xy origem da grade de altura, z 1/celula, w 1 = ha grade
    vec4 groundSize;    // xy texels (vertices da grade)
};

// REGISTRO DE DESENHO (64 bytes, um por particula visivel, no indice dela):
// tudo o que o vertex shader precisa, calculado UMA vez na simulacao - cor
// com curva, grafo, luz e fade; tamanho final; quadro da folha. O vertex
// shader roda 8 vezes por particula e so' le' isto (nada de emissor, curva
// ou grafo por vertice).
struct RenderRec {
    vec4 posSize;  // xyz posicao, w tamanho final (mundo)
    uvec4 look;    // x,y cor half4 (rgb linear com luz, a); z half2(rotacao, comprimento esticado); w eixo (octaedro 2x16)
    uvec4 sheet;   // x textura; y colunas | linhas<<8 | mistura<<16 | direcao<<18; z quadro | mistura do proximo (8 bits)<<16; w borda suave (float)
    uvec4 cutout;  // 8 halfs: poligono justo da textura
};

// Contadores na GPU (uint) - precisa bater com VfxSystem.cpp.
#define VFXC_ALIVE0    0
#define VFXC_ALIVE1    1
#define VFXC_DEAD      2
#define VFXC_RENDER    3  // 3: lista cheia, 4: lista de meia resolucao
#define VFXC_EVENTS    5  // eventos de sub-emissor escritos neste frame
// Desenho indexado (5 uints): 12..16 lista cheia, 17..21 meia resolucao.
#define VFXC_DISPATCH  8  // 8..10: dispatch indireto da simulacao
#define VFXC_DRAW      12
#define VFXC_SORTDISP  24 // 24..26: ordenar lista cheia; 28..30: meia resolucao
#define VFXC_EVENTDISP 32 // 32..34: dispatch indireto das filhas (um grupo por evento)
#define VFXC_MESH      36 // 36..43: particulas de cada malha (8 malhas)
#define VFXC_MESHDRAW  44 // 44..83: draw indexado de cada malha (5 uints cada)
#define VFXC_COUNT     84
#define VFX_MESH_TYPES 8u
// Lista da malha m no buffer de desenho: cap/8 vagas a partir de 4*cap + m*cap/8.
#define VFX_MAX_EVENTS 16384u

// Evento de sub-emissor: onde e com que velocidade herdada nascem as filhas.
struct VfxEvent {
    vec4 posSlot;  // xyz posicao, w slot do emissor filho (bits)
    vec4 velSeed;  // xyz velocidade herdada, w quantidade (8 bits) | semente << 8 (bits)
};

// Listas de desenho no mesmo buffer: [0,cap) cheia, [cap,2cap) meia
// resolucao; ordenadas em [2cap,3cap) e [3cap,4cap).
#define VFX_SORT_BUCKETS 256u

layout(std430, set = VFX_SET, binding = 0) VFX_RW buffer ParticleBuf { Particle particles[]; };
layout(std430, set = VFX_SET, binding = 1) VFX_RW buffer DeadBuf { uint deadList[]; };
layout(std430, set = VFX_SET, binding = 2) VFX_RW buffer AliveBuf { uint aliveList[]; }; // 2 metades
layout(std430, set = VFX_SET, binding = 3) VFX_RW buffer CounterBuf { uint counters[]; };
layout(std430, set = VFX_SET, binding = 4) VFX_RW buffer RenderBuf { uint renderList[]; };
layout(std430, set = VFX_SET, binding = 5) readonly buffer EmitterBuf { Emitter emitters[]; };
layout(std430, set = VFX_SET, binding = 6) readonly buffer SpawnBuf { SpawnRequest spawns[]; };
layout(std140, set = VFX_SET, binding = 7) uniform FrameBuf { FrameParams frame; };
layout(set = VFX_SET, binding = 8) uniform sampler2D sceneDepth;
layout(set = VFX_SET, binding = 9) uniform sampler2D lowResColor;
layout(set = VFX_SET, binding = 16) uniform sampler2D groundHeight; // grade de altura do chao (mundo)
// Ordenacao: por lista, 256 contagens + 256 posicoes de escrita.
layout(std430, set = VFX_SET, binding = 10) VFX_RW buffer SortBuf { uint sortData[]; };
layout(std430, set = VFX_SET, binding = 13) VFX_RW buffer EventBuf { VfxEvent events[]; };
layout(std430, set = VFX_SET, binding = 14) VFX_RW buffer RenderRecBuf { RenderRec recs[]; };
// Aneis das fitas: [inicio + seq % tamanho] = indice da particula com aquele seq.
layout(std430, set = VFX_SET, binding = 15) VFX_RW buffer RibbonBuf { uint ribbonRing[]; };

// Eixo unitario <-> 2 x unorm16 (octaedro): direcao da velocidade no registro.
uint packOct(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-8);
    vec2 o = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return packUnorm2x16(o * 0.5 + 0.5);
}
vec3 unpackOct(uint p) {
    vec2 o = unpackUnorm2x16(p) * 2.0 - 1.0;
    vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// Balde de profundidade (0 = mais longe): log da distancia entre perto e
// longe, de tras para frente (a mistura alfa precisa do fundo primeiro).
uint depthBucket(vec3 pos) {
    float w = max((frame.viewProj * vec4(pos, 1.0)).w, frame.timeInfo.w);
    float t = log2(w / frame.timeInfo.w) / log2(max(frame.depthInfo.z / frame.timeInfo.w, 1.0001));
    return VFX_SORT_BUCKETS - 1u - uint(clamp(t, 0.0, 1.0) * float(VFX_SORT_BUCKETS - 1u));
}

// Aleatorio sem estado (PCG hash): o mesmo resultado em qualquer ordem de
// execucao das threads, entao o efeito e' deterministico dada a semente.
uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rand01(uint seed, uint salt) { return float(pcg(seed ^ (salt * 0x9E3779B9u)) >> 8) * (1.0 / 16777216.0); }

vec4 unpackColorLut(Emitter e, float t) {
    float x = clamp(t, 0.0, 1.0) * 15.0;
    uint i = uint(floor(x));
    uint j = min(i + 1u, 15u);
    return mix(unpackUnorm4x8(e.colorLut[i]), unpackUnorm4x8(e.colorLut[j]), fract(x));
}

float sizeLutAt(Emitter e, uint i) {
    vec2 h = unpackHalf2x16(e.sizeLut[i >> 1]);
    return (i & 1u) == 0u ? h.x : h.y;
}
float sampleSizeLut(Emitter e, float t) {
    float x = clamp(t, 0.0, 1.0) * 15.0;
    uint i = uint(floor(x));
    return mix(sizeLutAt(e, i), sizeLutAt(e, min(i + 1u, 15u)), fract(x));
}

mat3 emitterRot(Emitter e) { return mat3(e.rot0.xyz, e.rot1.xyz, e.rot2.xyz); }
