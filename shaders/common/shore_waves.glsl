#ifndef ERUPTION_SHORE_WAVES_GLSL
#define ERUPTION_SHORE_WAVES_GLSL

// ONDAS DA PRAIA em LOBOS - a mesma conta no chao (lighting/pbr_common.glsl:
// lamina, espuma, areia molhada) e na agua (water/liquid.frag: repuxo,
// espuma da linha d'agua, cristas chegando). Um so' arquivo para os dois
// lados: com a onda copiada em dois shaders, a agua e a lamina na areia
// saiam de sincronia (faixa escura / linha entre a espuma e o mar).
//
// A frente e' o MAIOR de varios LOBOS, um por celula de uma grade jitterizada
// de 48 u no plano XZ. Cada lobo e' uma ELIPSE: semi-eixo ao longo da costa
// 25-35 u (3 a 5 vezes o alcance da subida na praia rasa), semi-eixo costa
// adentro 35 u; o perfil (1 - q)^0,6 e' quase uma meia-elipse, entao cada subida
// e' uma lamina larga abrindo na areia e nao uma cupula redonda. A costa
// vem do `fora` (direcao agua -> terra): na areia, do gradiente da grade da
// beira; na agua, do gradiente da profundidade na tela (liquid.frag).
// Cada celula tem fase propria (a fase grande da costa, kShoreFase, mais
// ate' +-1 s) e cada onda sorteia amplitude, largura do lobo e um atraso
// (0-2 s) antes de subir: periodo medio de 10 s com chegadas irregulares,
// sem que o tempo deixe de dar a volta em 10000 s (Render.cpp). Os
// parametros so' trocam com a onda dessa celula no fundo do recuo (lobo
// zerado): nada pula.
//
// UM EVENTO SO': a crista que chega pela agua (shoreCristaEm) e' a MESMA
// subida do lobo na areia, vista com a antecedencia do tempo de viagem. Em
// vez de calcular a crista por conta propria (cada celula com a sua, e a
// crista de um lobo que a areia ja' cobria com o vizinho chegava depois da
// subida), a agua avalia os lobos no instante em que a crista chega 'a linha
// d'agua (t + viagem) e mostra a crista do lobo que ESTA' na frente nesse
// instante - o mesmo criterio (o maior) que a areia usa. Crista na linha
// d'agua == espuma estourando == inicio da subida. Nao ha' espuma parada
// esperando a onda: a espuma da linha d'agua so' existe com a crista.
//
// O lobo e' avaliado no PE do pixel (o ponto da linha d'agua sob ele: wp -
// fora * d). Assim todo ponto de chao ao longo da direcao agua->terra ve' a
// mesma cupula, e a frente contra a distancia d desenha o arco do lobo. No
// plano d'agua e no fundo debaixo dele d = 0: o pe' e' o proprio ponto, e a
// agua (sem a grade da beira) chega na mesma conta que o chao.
//
// Subida rapida (32% do ciclo, desacelerando ate' parar no alto, como um
// corpo lancado rampa acima) e descida lenta (68%, comecando parada e
// acelerando: a agua ainda escorre quando a proxima chega). Medicoes de
// campo dao a subida mais curta que a descida (ver not_commit/onda_praia).

const float kShoreOmega = 0.6283185;            // rad/s (2 pi / 10 s), periodo medio
const float kShorePeriodo = 10.0;               // s
const vec2 kShoreFase = vec2(0.0103, 0.0077);   // fase da costa: 1 volta a cada ~490 u
const float kShoreCelula = 48.0;                // u, lado da celula de lobos
// Semi-eixo maximo do lobo: 0,74 celula. Com o jitter em 0,25-0,75 o centro
// de uma celula fora do bloco 2x2 fica a >= 0,75 celula do pixel: so' 4
// celulas por pixel e a frente continua sem degrau ao trocar de bloco.
const float kShoreLobo = 0.74 * kShoreCelula;   // u
const float kShoreSubida = 0.32;                // fracao do ciclo ativo subindo
const float kShoreRecuo = 0.3;                  // u, repuxo da borda do mar no fundo do recuo
const float kShoreFace = 0.07;                  // ciclo: janela da face da crista, antes de quebrar
// Ciclo entre o lobo comecar a subir e a espuma estourar: a face da crista
// aparece enquanto a frente comeca a andar (os primeiros 0,7 s) e a espuma
// branca vem depois dela, nunca antes. Nos quadros a cada 0,1 s a frente so'
// se destaca na areia ~1 s depois do inicio onde o novo lobo e' fraco
// (a descida do vizinho ainda e' maior); 0,7 s fica no meio.
const float kShoreChega = 0.07;

// Hash inteiro (PCG de um passo): sem sin(), igual em toda GPU.
uint shorePcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
vec4 shoreHash4(ivec2 c, int onda) {
    uint h = shorePcg(uint(c.x) ^ shorePcg(uint(c.y) ^ shorePcg(uint(onda) + 0x9e3779b9u)));
    return vec4(uvec4(h, h >> 8u, h >> 16u, h >> 24u) & 255u) * (1.0 / 255.0);
}

// Quanto da subida (0..1) com s = fracao do ciclo ativo (0..1).
float shoreExcursao(float s) {
    if (s < kShoreSubida) { float v = 1.0 - s / kShoreSubida; return 1.0 - v * v; }
    float v = (s - kShoreSubida) / (1.0 - kShoreSubida);
    return 1.0 - v * sqrt(v);
}

struct ShoreOnda {
    float e1;       // frente aqui (0..1 do alcance): maior lobo
    float e2;       // segundo maior lobo (encontro de lobos = linha de espuma)
    float sobe1;    // 1 se o lobo maior esta' subindo
    float ex1;      // excursao (0..1, sem amplitude nem forma) do lobo maior
    float sobe2;
    float cuspide;  // u (no pe') ate' a linha onde os dois maiores lobos se igualam
    vec2 semente;   // desenho de espuma da onda do lobo maior (troca a cada onda)
    float seca;     // s desde que a agua saiu deste ponto (0 = coberto)
    float crista;   // agua: altura relativa da crista que chega (0..1)
    float face;     // agua: face da crista virada para a praia, antes de quebrar
    float quebra;   // agua: espuma da onda quebrando / rolando para a areia
};

// Uma celula no instante tc (ciclos): onda n da celula, sua elipse e a
// excursao. costa = tangente da costa (unitaria) ou 0 se desconhecida (a
// elipse vira circulo de semi-eixo maior).
struct ShoreCel {
    vec4 hc, hw;    // sorteio da celula e da onda
    vec2 dv;        // pe - centro
    float al;       // dv ao longo da costa
    float A;        // semi-eixo ao longo da costa (u)
    float a;        // amplitude da onda (0,45-1)
    float u, u0, s; // fase no ciclo, atraso, fracao do ciclo ativo (<= 0: parada)
    float forma;    // meia-elipse (0..1)
    float ex;       // excursao (0..1)
    float v;        // a * ex * forma
};
ShoreCel shoreCelula(ivec2 c, float tc, vec2 pe, vec2 costa) {
    ShoreCel k;
    k.hc = shoreHash4(c, 0);
    vec2 centro = (vec2(c) + 0.25 + 0.5 * k.hc.xy) * kShoreCelula;
    float fase = dot(centro, kShoreFase) * 0.15915494 + 0.2 * (k.hc.z - 0.5);
    float ciclo = tc + fase;
    float n = floor(ciclo);
    k.u = ciclo - n;
    k.hw = shoreHash4(c, int(n));
    k.A = kShoreLobo * (0.72 + 0.28 * k.hw.z);
    k.dv = pe - centro;
    float B = kShoreLobo * 0.98;
    float q;
    if (costa != vec2(0.0)) {
        k.al = dot(k.dv, costa);
        float cr = dot(k.dv, vec2(-costa.y, costa.x));
        q = (k.al * k.al) / (k.A * k.A) + (cr * cr) / (B * B);
    } else {
        k.al = 0.0;
        q = dot(k.dv, k.dv) / (k.A * k.A);
    }
    // Meia-elipse com as pontas um pouco mais afuniladas (expoente 0,6, nao
    // 0,5): com a tangente vertical da elipse pura a lamina acabava em
    // degrau reto ao lado do lobo, visto de cima.
    k.forma = pow(clamp(1.0 - q, 0.0, 1.0), 0.6);
    // Amplitude por onda: series de ondas maiores e menores (0,45-1).
    k.a = 0.45 + 0.55 * k.hw.x * (2.0 - k.hw.x);
    k.u0 = 0.2 * k.hw.y;                          // atraso antes de subir (0-2 s)
    k.s = (k.u - k.u0) / (1.0 - k.u0);
    k.ex = k.s > 0.0 ? shoreExcursao(k.s) : 0.0;
    k.v = k.a * k.ex * k.forma;
    return k;
}

// pe = ponto da linha d'agua (XZ), t = tempo (s, o mesmo dos dois lados),
// rel = distancia ate' a agua / alcance (para `seca`; < 0 dispensa),
// costa = direcao ao longo da costa (unitaria) ou 0 se desconhecida. No chao
// o pe' so' anda ao longo da linha d'agua, e a cuspide entre dois lobos e'
// onde eles se cruzam NESSA direcao (um lobo mar adentro e outro terra
// adentro com o mesmo centro na costa nao se cruzam).
ShoreOnda shoreOndaEm(vec2 pe, float t, float rel, vec2 costa) {
    ShoreOnda o;
    o.e1 = 0.0; o.e2 = 0.0; o.sobe1 = 0.0; o.sobe2 = 0.0; o.ex1 = 0.0; o.cuspide = 1.0e4;
    vec2 g1 = vec2(0.0), g2 = vec2(0.0);
    o.semente = vec2(0.0); o.seca = 1.0e4;
    o.crista = 0.0; o.face = 0.0; o.quebra = 0.0;
    vec2 g = pe / kShoreCelula - 0.5;
    ivec2 c0 = ivec2(floor(g));
    float tc = t / kShorePeriodo;
    for (int i = 0; i < 4; ++i) {
        ShoreCel k = shoreCelula(c0 + ivec2(i & 1, i >> 1), tc, pe, costa);
        float sobe = (k.s > 0.0 && k.s < kShoreSubida) ? 1.0 : 0.0;
        // Gradiente do lobo no pe' (1/u): a distancia ate' a cuspide sai da
        // diferenca dos dois maiores dividida pela diferenca dos gradientes.
        // So' a diferenca (limiar fixo) pintava lobo inteiro quando dois
        // vizinhos subiam quase juntos.
        float gk = -(k.a * k.ex / max(k.forma, 0.15)) / (k.A * k.A);
        vec2 gv = costa != vec2(0.0) ? gk * k.al * costa : gk * k.dv;
        if (k.v > o.e1) {
            o.e2 = o.e1; o.sobe2 = o.sobe1; g2 = g1;
            o.e1 = k.v; o.sobe1 = sobe; o.ex1 = k.ex; g1 = gv; o.semente = k.hw.wz * 53.0 + k.hc.xy * 17.0;
        } else if (k.v > o.e2) {
            o.e2 = k.v; o.sobe2 = sobe; g2 = gv;
        }
        // Ha' quanto tempo a agua deste lobo saiu do ponto (rel do proprio lobo).
        if (rel >= 0.0) {
            float alto = k.a * k.forma;
            if (alto > rel) {
                float r = rel / alto;
                float sSai = kShoreSubida + (1.0 - kShoreSubida) * exp2(log2(max(1.0 - r, 1e-4)) * 0.6666667);
                float sChega = 1.0 - sqrt(max(1.0 - r, 0.0));
                sChega *= kShoreSubida;
                float dt;
                if (k.s >= sChega && k.s <= sSai) dt = 0.0;
                else if (k.s > sSai) dt = (k.s - sSai) * (1.0 - k.u0);
                else dt = k.u + 1.0 - (k.u0 + sSai * (1.0 - k.u0)); // saiu na onda anterior
                o.seca = min(o.seca, dt * kShorePeriodo);
            }
        }
    }
    // Dois lobos quase no mesmo lugar e na mesma fase (gradientes iguais)
    // nao se cruzam: sobem como uma onda so'. Sem isto a razao 0/0 pintava o
    // lobo inteiro de espuma.
    float dg = length(g1 - g2);
    float lados = smoothstep(0.25, 0.6, dg / (length(g1) + length(g2) + 1e-4));
    o.cuspide = (o.e1 - o.e2) / max(dg, 1e-3) / max(lados, 1e-3);
    return o;
}

// Crista que chega pela agua (so' no lado da agua), preenche crista, face e
// quebra de `o`. pe = ponto da linha d'agua sob o pixel (wp + fora * d), prof
// = profundidade (u). A onda leva ~5 sqrt(h) s ate' a linha d'agua (c =
// sqrt(g h), rampa ~1:8): por isso as cristas se apertam e empinam perto da
// areia. A raiz pura (5 sqrt(h)) tem inclinacao infinita em h = 0: os ultimos
// 0,3 u de fundo levavam 2,7 s, e a espuma estourava na borda visivel da agua
// ~2 s ANTES de o lobo subir. Com h + 2 (T = 5 (sqrt(h + 2) - sqrt(2)): 0,4 s
// a 0,2 u, 1,6 s a 1 u, 4 s a 2,6 u) a crista na borda visivel e' o inicio da
// subida: ali a crista ja' e' a propria frente da lamina (4-6 u/s). A conta avalia os lobos no instante da chegada (t + viagem) e segue
// o lobo que esta' na frente nele; chi = tempo (ciclos) desde o inicio da
// subida desse lobo, 0 = crista na linha d'agua = lobo comecando a subir.
void shoreCristaEm(inout ShoreOnda o, vec2 pe, float t, float prof, vec2 costa) {
    float viagem = 5.0 * (sqrt(prof + 2.0) - 1.4142136) / kShorePeriodo;
    // + kShoreFace: o lobo so' existe (v > 0) depois de comecar a subir; a
    // face aparece antes da espuma, entao olha-se 0,7 s adiante.
    float tv = t / kShorePeriodo + viagem + kShoreFace;
    vec2 g = pe / kShoreCelula - 0.5;
    ivec2 c0 = ivec2(floor(g));
    float vk[4]; vec3 ck[4]; // x = crista, y = face, z = quebra
    float vmax = 0.0;
    for (int i = 0; i < 4; ++i) {
        ShoreCel k = shoreCelula(c0 + ivec2(i & 1, i >> 1), tv, pe, costa);
        float chi = (k.u - k.u0) - kShoreFace - kShoreChega; // 0 = espuma estoura
        // Quebra onde a altura passa de ~0,8 da profundidade: altura de
        // ~0,25 u a 1 u de fundo crescendo com h^-1/4 (empinamento),
        // quebra a ~0,4 u (menos nas ondas pequenas).
        float hq = 0.42 * k.a;
        float frenteC = smoothstep(-0.07, 0.0, chi);
        float costas = 1.0 - smoothstep(0.0, 0.3, chi);
        float perfil = frenteC * costas * costas;
        float chega = 1.0 - smoothstep(1.6, 2.6, prof);   // nasce longe, apagada
        float antes = smoothstep(hq * 0.85, hq * 1.15, prof);
        float face = smoothstep(-0.07, -0.006, chi) - smoothstep(-0.006, 0.006, chi);
        // Rolando: logo antes de quebrar o labio da crista fica branco.
        float labio = (1.0 - smoothstep(0.005, 0.035, abs(chi + 0.006))) * (1.0 - smoothstep(hq * 1.1, hq * 1.9, prof));
        // Quebrada: frente branca (vagalhao) e espuma rala atras.
        float bore = step(-0.02, chi) * (1.0 - smoothstep(0.02, 0.09, chi)) + 0.45 * (1.0 - smoothstep(0.0, 0.28, chi)) * step(0.0, chi);
        float env = smoothstep(0.03, 0.35, k.forma) * chega * float(k.s > 0.0);
        vk[i] = k.v;
        ck[i] = env * k.a * vec3(perfil, face * antes, max(labio * antes, bore * frenteC * (1.0 - antes)));
        vmax = max(vmax, k.v);
    }
    // So' conta o lobo que esta' na frente (como a areia, que mostra o maior);
    // lobos quase iguais contam juntos (o encontro).
    vec3 soma = vec3(0.0);
    float peso = 0.0;
    for (int i = 0; i < 4; ++i) {
        float w = smoothstep(0.7, 0.95, vk[i] / (vmax + 1e-4)) * step(1e-5, vk[i]);
        soma += w * ck[i];
        peso += w;
    }
    soma /= max(peso, 1.0);
    o.crista = soma.x; o.face = soma.y; o.quebra = soma.z;
}

// Repuxo (u abaixo do nivel) com a frente em e1: 0 com a onda no alto,
// kShoreRecuo no fundo do recuo e nas baias entre lobos.
float shoreRecuoDe(float e1) {
    float r = 1.0 - clamp(e1, 0.0, 1.0);
    return kShoreRecuo * r * r;
}

// Linha de espuma onde dois lobos se encontram (cuspide): a descida de um
// batendo na subida do outro, ou duas subidas se cruzando.
float shoreEncontro(ShoreOnda o) {
    float ativo = smoothstep(0.03, 0.12, o.e2);
    float perto = 1.0 - smoothstep(0.3, 1.3, o.cuspide);
    float bate = o.sobe1 != o.sobe2 ? 1.0 : 0.6;
    return perto * ativo * bate;
}

#endif
