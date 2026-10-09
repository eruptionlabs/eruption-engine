// Corpo de model.frag: incluído por model.frag (com velocidade de objeto, FSR)
// e por model_novel.frag (ERUPTION_NO_VELOCITY: sem o alvo de velocidade).
#include "common/noise.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in flat uint inTexIndex;
layout(location = 4) in flat uint inMatId;
layout(location = 5) in vec4 inColor;
layout(location = 6) in flat uint inPbrIndex;
layout(location = 7) in flat uint inNormalIndex;
layout(location = 8) in flat uint inBlendTexIndex;
layout(location = 9) in flat uint inBlendPbrIndex;
layout(location = 10) in flat uint inBlendNormalIndex;
layout(location = 11) in float inBlendWeight;
layout(location = 12) in flat uint inBlendMaskIndex;
layout(location = 13) in vec2 inBlendMaskUV;
// Brilho auto-emissivo por vértice (lava). Pareia com outEmissiveStrength
// do model.vert (location 14); sem esta declaração o frag não compila.
layout(location = 14) in float inEmissiveStrength;
// Splat de 4 camadas: 4 slots bindless de 16 bits empacotados em 2 uint.
// Zero nos dois = mapa sem splat, e o caminho de 2 materiais abaixo roda
// exatamente como antes.
layout(location = 15) in flat uvec2 inSplatTex;
// Segunda mascara/camadas (dobra o splat de 4 pra 8 + base = 9). Zero nos
// dois (inSplatTex2 e inBlendMaskIndex2) = mapa so' com o splat de 4 (ou
// sem splat nenhum), e o bloco abaixo nunca entra no ramo novo.
layout(location = 16) in flat uvec2 inSplatTex2;
layout(location = 17) in flat uint inBlendMaskIndex2;
layout(location = 18) in flat float inSway;
layout(location = 19) in vec4 inDispInfo; // x=alinhamento, y=modulo, z=origem, w=altura crua (0..1) // ver auditoria abaixo // G32: > 0 = vegetacao (flag de folhagem)

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPBR;
layout(location = 3) out uint outMaterialID;
layout(location = 4) out vec4 outEmissive;
// Velocidade de objeto (6o alvo do G-buffer, so' quando ligado): UV do frame
// anterior menos UV atual, sem jitter - mesma convencao do CameraMotion.
#ifndef ERUPTION_NO_VELOCITY
layout(location = 5) out vec2 outVelocity;
layout(location = 20) in vec3 inPrevWorldPos;
#endif

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
    float alpha;
    float metallicScale;  // absolute metallic value from the material profile
    float roughnessScale; // absolute roughness value from the material profile
    float _pad;           // ERUPTION_TEST_MAGENTA_DEBUG (nao e' padding!)
} push;

layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_viewProjection;
    mat4 u_inverseView;
    mat4 u_inverseProjection;
    vec3 u_cameraPos;
    float u_time;
    vec2 u_screenResolution;
    float u_nearPlane;
    float u_farPlane;
    uint u_frameIndex;
    uint u_debugMode;
    float u_spriteExposure;
    float u_giIntensity;
    float u_giAmbientFloor;
    float u_spriteTilt;
    float u_shadowHeightScale;
    float u_spriteNormalYMix;
    float u_normalMapScale;
    float u_normalMapInvertY;
    float u_normalSmoothing;
    float u_defaultRoughness;
    float u_defaultMetallic;
    // Same buffer as sprite_forward.frag - the sun data is already here.
    vec4 u_sunDir;        // xyz=travel dir, w=intensity
    vec4 u_sunColor;      // xyz=color
    vec4 u_ambientSky;
    vec4 u_ambientGround;
    // POM (docs/displacement_design.md): x=heightScale (uv), y=maxSteps,
    // z=minSteps, w=wetness global (o alpha do MRAH-W agora é SÓ altura).
    vec4 u_pomParams;
    // Cauda do UBO: precisa estar declarada aqui para os campos abaixo caiveem
    // nos offsets certos (o C++ escreve nesta ordem - ver FrameUBO).
    vec4 u_windParams;
    // RELEVO POR DISTANCIA: duas curvas editadas no F2 e enviadas como LUT de
    // 8 amostras uniformes em [0, params.x] unidades de mundo.
    // params: x = distancia maxima, y = 1 liga / 0 desliga.
    vec4 u_normalDistParams;
    vec4 u_normalScaleLut0;
    vec4 u_normalScaleLut1;
    vec4 u_normalSmoothLut0;
    vec4 u_normalSmoothLut1;
    // Tesselacao (ver model.tesc/model.tese): x = distancia maxima da curva,
    // y = liga, z = amplitude, w = fator maximo.
    vec4 u_tessParams;
    vec4 u_tessLut0;
    vec4 u_tessLut1;
    // x = reservado, y = espacamento alvo entre vertices da tesselacao (ver model.tesc).
    // z = teto de inclinacao da normal em radianos (0 desliga)
    vec4 u_tessParams2;
    float u_tessHeightBlur;
    // Fim do FrameUBO (FSR): matrizes SEM jitter e o vento do frame anterior,
    // para a velocidade de objeto (vegetacao) - ver SpriteRenderer.hpp.
    mat4 u_viewProjNoJitter;
    mat4 u_prevViewProjNoJitter;
    vec4 u_jitterNdc;
    vec4 u_prevWindParams;
};

// Tabela de 8 valores com interpolacao linear. t em 0..1.
float lut8(vec4 a, vec4 b, float t) {
    float x = clamp(t, 0.0, 1.0) * 7.0;
    int i = int(floor(x));
    float f = x - float(i);
    float v0 = i < 4 ? a[i] : b[i - 4];
    int j = min(i + 1, 7);
    float v1 = j < 4 ? a[j] : b[j - 4];
    return mix(v0, v1, f);
}

// ===================== SO' PRO MODO DE AUDITORIA 3 (height map) ========
// heightUv (model.tese/model.vert) le' no MIP 2 e aplica
// GANHO - e' o que desloca a malha, e so' existe em umas dezenas de vertices
// por patch tesselado (o rasterizador INTERPOLA reto entre eles pra pintar
// os pixels no meio). Isso e' aliasing: um ruido de alta frequencia,
// amostrado esparso e reconstruido por reta, sai mais irregular do que o
// dado original parece quando se olha ele inteiro - e o pedido do autor foi
// exatamente que o height map fosse igual a textura. Estas
// funcoes leem no MIP 0 (arquivo inteiro, sem borrar), SEM ganho, POR
// PIXEL de tela (nao por vertice) - o mesmo dado cru que
// ERUPTION_TEST_DUMP_TEX_DIR grava em BMP. Cobrem os mesmos dois casos que
// o deslocamento real: alfa do MRAH-W (useAlpha) e a mistura ponderada de
// splat do chao sem PBR proprio (heightDebug*Splat*, mesmo criterio de
// heightSplatUv em model.tese).
float heightDebugUv(uint texIdx, vec2 uvIn, bool useAlpha) {
    vec4 s = textureLod(u_textures[nonuniformEXT(texIdx)], uvIn, 0.0);
    if (useAlpha) return s.a > 0.999 ? 0.5 : s.a;
    const vec3 kL = vec3(0.2126, 0.7152, 0.0722);
    return dot(s.rgb, kL);
}

float heightDebugSplatUv(vec2 uvIn, vec2 maskUv, uint baseTex, uvec2 splat, uvec2 splat2,
                         uint maskIdx, uint maskIdx2) {
    float hBase = heightDebugUv(baseTex, uvIn, false);
    if ((splat.x | splat.y) == 0u || maskIdx == 0u) return hBase;
    vec4 w = textureLod(u_textures[nonuniformEXT(maskIdx)], maskUv, 0.0);
    uint s0 =  splat.x        & 0xFFFFu;
    uint s1 = (splat.x >> 16) & 0xFFFFu;
    uint s2 =  splat.y        & 0xFFFFu;
    uint s3 = (splat.y >> 16) & 0xFFFFu;
    float wBaseSum = w.r + w.g + w.b + w.a;
    float acc = 0.0, sum = 0.0;
    if (s0 != 0u) { acc += heightDebugUv(s0, uvIn, false) * w.r; sum += w.r; }
    if (s1 != 0u) { acc += heightDebugUv(s1, uvIn, false) * w.g; sum += w.g; }
    if (s2 != 0u) { acc += heightDebugUv(s2, uvIn, false) * w.b; sum += w.b; }
    if (s3 != 0u) { acc += heightDebugUv(s3, uvIn, false) * w.a; sum += w.a; }
    if (maskIdx2 != 0u) {
        vec4 w2 = textureLod(u_textures[nonuniformEXT(maskIdx2)], maskUv, 0.0);
        uint s4 =  splat2.x        & 0xFFFFu;
        uint s5 = (splat2.x >> 16) & 0xFFFFu;
        uint s6 =  splat2.y        & 0xFFFFu;
        uint s7 = (splat2.y >> 16) & 0xFFFFu;
        wBaseSum += w2.r + w2.g + w2.b + w2.a;
        if (s4 != 0u) { acc += heightDebugUv(s4, uvIn, false) * w2.r; sum += w2.r; }
        if (s5 != 0u) { acc += heightDebugUv(s5, uvIn, false) * w2.g; sum += w2.g; }
        if (s6 != 0u) { acc += heightDebugUv(s6, uvIn, false) * w2.b; sum += w2.b; }
        if (s7 != 0u) { acc += heightDebugUv(s7, uvIn, false) * w2.a; sum += w2.a; }
    }
    float wBase = max(0.0, 1.0 - wBaseSum);
    acc += hBase * wBase;
    sum += wBase;
    return sum > 1e-4 ? acc / sum : hBase;
}


// 4x4 Dither Matrix for transparency in G-Buffer
const float dither[16] = float[](
    0.0, 0.5, 0.125, 0.625,
    0.75, 0.25, 0.875, 0.375,
    0.1875, 0.6875, 0.0625, 0.5625,
    0.9375, 0.4375, 0.8125, 0.3125
);

float srgbToLinear(float c) {
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

vec3 srgbToLinear(vec3 c) {
    return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b));
}

// Tangent frame from the ACTUAL UV gradient (Schüler 2013, "Normal Mapping
// without Precomputed Tangents"). T follows +u and B follows +v of THIS
// pixel's UV mapping, each solved independently from the position/UV screen
// derivatives - so a mirrored (ping-pong) tile gets a mirrored B and the
// relief lands exactly on the texel it was baked from.
//
// HISTORICO. A versao 1 multiplicava T e B por sign(det(dUV)), o que numa
// UV espelhada gira a base 180 graus em vez de espelhar so' o eixo
// espelhado ("a parede respira", relevo alternando por ladrilho). A versao
// 2 trocou por um eixo de MUNDO fixo, que nunca alterna - mas tambem nunca
// e' a UV: medido no vila-A a 06:48 (ERUPTION_TEST_MICRO_SHADOW=8), T
// coincidia com +u em 23% dos pixels e B com +v em 4%; no chao a base
// estava a 90 graus. Normal map girado le como TEXTURA DESLOCADA - o
// sombreado do relevo nao cai onde esta' o detalhe do albedo (relato do
// autor: "a textura desloca alguns pixels" com scale 1 vs 0).
//
// Fallback para a base de eixo de mundo so' quando a UV nao tem gradiente
// (sprite/UV degenerada), para nao produzir NaN.
mat3 computeTBN(vec3 N, vec3 worldPos, vec2 uv) {
    vec3 dp1 = dFdx(worldPos);
    vec3 dp2 = dFdy(worldPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float tl = dot(T, T);
    float bl = dot(B, B);
    if (tl > 1e-16 && bl > 1e-16) {
        T = normalize(T - N * dot(N, T));
        // Mantem o sentido de B (espelhamento) mas garante a ortogonalidade
        // com T e N - transpose(TBN) e' usado como inversa nas marchas.
        vec3 Bo = cross(N, T);
        B = (dot(Bo, B) < 0.0) ? -Bo : Bo;
        return mat3(T, B, N);
    }
    vec3 up = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    T = cross(up, N);
    float l = length(T);
    T = (l > 1e-6) ? (T / l) : vec3(1.0, 0.0, 0.0);
    B = normalize(cross(N, T));
    return mat3(T, B, N);
}

void main() {
#ifndef ERUPTION_NO_VELOCITY
    {
        // Primeiro de tudo: main() tem retornos antecipados (debug/wireframe),
        // e um alvo sem escrita ficaria com lixo em vez do sentinela.
        const vec4 c = u_viewProjNoJitter * vec4(inWorldPos, 1.0);
        const vec4 p = u_prevViewProjNoJitter * vec4(inPrevWorldPos, 1.0);
        outVelocity = (p.xy / p.w - c.xy / c.w) * 0.5;
    }
#endif
    // MODO WIREFRAME (debugMode 3): a pipeline ja' esta' em POLYGON_MODE_LINE,
    // entao so' as ARESTAS chegam aqui. Escreve verde no emissive e zera o
    // resto - o passe de iluminacao passa o emissive direto, entao a linha sai
    // com a cor exata, sem sombra nem PBR por cima. Sai cedo: nada de textura,
    // normal map ou micro-sombra num pixel que e' so' contorno.
    if (u_debugMode == 3u) {
        outAlbedo = vec4(0.0, 0.0, 0.0, 1.0);
        outNormal = vec4(0.5, 0.5, 1.0, 1.0);
        outPBR = vec4(0.0, 1.0, 0.0, 0.0);
        outMaterialID = 0u;
        outEmissive = vec4(0.15, 1.0, 0.25, 1.0);
        return;
    }

    // Transparencia por dither (Bayer 4x4). push.alpha negativo e' o mesmo
    // limiar em MODULO com o teste INVERTIDO - e' como o crossfade de LOD
    // (ModelRenderer::render) desenha os dois niveis vizinhos com padroes
    // COMPLEMENTARES sem precisar de um campo novo no push constant: o par
    // (limiar, +) / (limiar, -) cobre cada pixel exatamente uma vez (SOTA
    // "dithered LOD cross-fade", ver docs/pedidos.md). Uso normal (destaque
    // do editor, fade positivo) nunca usa sinal negativo - comportamento
    // identico ao de antes.
    if (push.alpha < 0.99) {
        float thr = abs(push.alpha);
        bool invert = push.alpha < 0.0;
        int x = int(gl_FragCoord.x) % 4;
        int y = int(gl_FragCoord.y) % 4;
        bool keep = invert ? (dither[x + y * 4] > thr) : (dither[x + y * 4] <= thr);
        if (!keep) {
            discard;
        }
    }

    vec3 N = (length(inNormal) > 1e-8) ? normalize(inNormal) : vec3(0.0, 1.0, 0.0);

    // NORMAL DA GEOMETRIA DESLOCADA. Onde a banda perto deslocou o vertice
    // (tesselacao ou o proprio model.vert), a normal INTERPOLADA continua
    // sendo a da malha plana: o relevo aparecia na silhueta e no recorte, mas
    // nao na luz - "o tesselation nao ta sendo feito sobre a normal" (autor,
    // 2026-09-06). A normal certa da superficie deslocada sai das derivadas da
    // posicao de mundo, que ja' chegam deslocadas aqui. Sinal resolvido pela
    // camera (superficie visivel encara o olho). So' vale dentro da banda -
    // fora dela nada foi deslocado e a normal do vertice continua sendo a boa,
    // inclusive porque e' mais suave que a derivada.
    // NA BANDA PERTO a normal ja' chega correta do model.tese (gradiente
    // analitico da altura) - o TES soma 10 em inDispInfo.z para avisar. Fora
    // da tesselacao, quem deslocou foi o model.vert e a derivada de tela e' a
    // unica fonte da normal deslocada; facetada, mas melhor que sombrear como
    // se a superficie fosse lisa.
    if (u_tessParams.y > 0.5 && inDispInfo.z < 9.0) {
        float dT = distance(inWorldPos, u_cameraPos);
        float fT = lut8(u_tessLut0, u_tessLut1, dT / max(u_tessParams.x, 1.0));
        if (fT > 1.001) {
            vec3 gN = cross(dFdx(inWorldPos), dFdy(inWorldPos));
            float gLen = length(gN);
            if (gLen > 1e-8) {
                gN /= gLen;
                if (dot(gN, u_cameraPos - inWorldPos) < 0.0) gN = -gN;
                // Mistura com a normal do vertice pela mesma rampa do fator:
                // na fronteira da banda o deslocamento e' zero e a normal tem
                // que voltar a ser exatamente a de antes, sem degrau.
                float t = clamp((fT - 1.0) / max(u_tessParams.w - 1.0, 1e-3), 0.0, 1.0);
                N = normalize(mix(N, gN, t));
            }
        }
    }

    // ===================== AUDITORIA DO DESLOCAMENTO =====================
    // ERUPTION_TEST_TESS_AUDIT=1|2|3 (u_normalDistParams.z). Existe porque
    // "acho que nao esta' seguindo a normal" nao e' verificavel a olho: aqui a
    // pergunta vira COR e, no script de bancada, vira porcentagem.
    //   1 = O DESLOCAMENTO SEGUE A NORMAL? verde = |dot(deslocamento, normal)|
    //       igual a 1 (exatamente ao longo da normal), vermelho = desalinhado,
    //       AZUL = pixel sem deslocamento nenhum (fora da banda ou sem altura).
    //   2 = A LUZ SEGUE O RELEVO? angulo entre a normal que vai para o
    //       G-buffer e a normal da geometria deslocada (derivadas da posicao):
    //       verde = coincidem, vermelho = a superficie esta' sendo sombreada
    //       como se fosse lisa.
    //   3 = DISPLACEMENT MAGNITUDE = O HEIGHT MAP EM SI: cinza = h (0..1) lido
    //       do MESMO texIdx/uv usado pra deslocar, POR PIXEL, mip 0, sem
    //       ganho - compare com a textura normal (modo 0) na mesma pose: onde
    //       a pedra/textura escurece, aqui tem que escurecer tambem (vale);
    //       se nao escurecer, a altura nao esta' vindo do canal/UV que a
    //       textura mostra (ex.: MRAH-W com alfa vazio). Painel F2: checkbox
    //       "Debug: height map".
    //   4 = DE ONDE vem a altura (MRAH-W vs. passa-alta do albedo).
    int auditMode = int(u_normalDistParams.z + 0.5);
    if (auditMode > 0) {
        vec3 gAud = cross(dFdx(inWorldPos), dFdy(inWorldPos));
        float gl2 = length(gAud);
        gAud = gl2 > 1e-8 ? gAud / gl2 : N;
        if (dot(gAud, u_cameraPos - inWorldPos) < 0.0) gAud = -gAud;
        // CLASSES, nao rampa: a imagem final passa por exposicao, tonemap e
        // gama, e uma rampa continua sai distorcida do outro lado. Ordem entre
        // canais sobrevive, entao verde/amarelo/vermelho e' legivel e contavel.
        vec3 col;
        if (auditMode == 1) {
            col = inDispInfo.y > 1e-5
                ? (inDispInfo.x > 0.999 ? vec3(0.0, 1.0, 0.0)
                  : inDispInfo.x > 0.99 ? vec3(1.0, 1.0, 0.0)
                                        : vec3(1.0, 0.0, 0.0))
                : vec3(0.0, 0.0, 1.0);
        } else if (auditMode == 2) {
            float al = clamp(dot(normalize(N), gAud), 0.0, 1.0);
            col = inDispInfo.y > 1e-5
                ? (al > 0.90 ? vec3(0.0, 1.0, 0.0)
                  : al > 0.70 ? vec3(1.0, 1.0, 0.0)
                              : vec3(1.0, 0.0, 0.0))
                : vec3(0.0, 0.0, 1.0);
        } else if (auditMode == 3) {
            // DISPLACEMENT MAGNITUDE = O HEIGHT MAP EM SI, cinza, POR PIXEL -
            // nao inDispInfo.y/w (esses sao por VERTICE da tesselacao,
            // interpolados reto entre eles pelo rasterizador; um ruido de
            // textura fica em degrau/faceta assim, nao em grao fino - foi o
            // que o autor viu e reclamou: o height map tem que ser
            // igual a textura). Amostra o mesmo texIdx/uv que o
            // deslocamento usaria, mas em MIP 0 e sem ganho, um sample por
            // FRAGMENTO - ver heightDebugUv acima. E' o
            // mesmo dado que ERUPTION_TEST_DUMP_TEX_DIR grava em BMP, so' que
            // ao vivo, na malha, na pose atual.
            float hFrag = -1.0;
            if (inPbrIndex != 0u) {
                hFrag = heightDebugUv(inPbrIndex, inTexCoord, true);
            } else if (inTexIndex != 0u) {
                hFrag = heightDebugSplatUv(inTexCoord, inBlendMaskUV, inTexIndex, inSplatTex, inSplatTex2,
                                         inBlendMaskIndex, inBlendMaskIndex2);
            }
            col = hFrag >= 0.0 ? vec3(hFrag) : vec3(0.0, 0.0, 1.0);
        } else if (auditMode == 6) {
            // SINAL do deslocamento (pedido do autor: "preto fica fixo, branco
            // sobe - isso ta errado, tinha que afundar tambem"). Mesmo hFrag
            // do modo 3 (por pixel, sem ganho) - o SINAL de (h-0.5) e' IGUAL
            // com ou sem ganho (ganho > 0 nunca troca o sinal), entao serve
            // pra confirmar se a formula real (que usa h-0.5) esta' mesmo
            // deslocando os dois lados. VERDE = h>0.5 (deveria subir),
            // VERMELHO = h<0.5 (deveria afundar), PRETO = h==0.5 exato
            // (neutro/sem dado) - se as partes escuras da textura saem
            // VERMELHAS aqui, o deslocamento ESTA' indo pro lado certo e o
            // que falta ver e' so' visual (afundamento e' sempre mais dificil
            // de perceber que elevacao, luz nao pega do mesmo jeito).
            float hSign = -1.0;
            if (inPbrIndex != 0u) {
                hSign = heightDebugUv(inPbrIndex, inTexCoord, true);
            } else if (inTexIndex != 0u) {
                hSign = heightDebugSplatUv(inTexCoord, inBlendMaskUV, inTexIndex, inSplatTex, inSplatTex2,
                                         inBlendMaskIndex, inBlendMaskIndex2);
            }
            float t = hSign >= 0.0 ? (hSign - 0.5) : 0.0;
            col = t > 0.0 ? vec3(0.0, clamp(t * 2.0, 0.0, 1.0), 0.0)
                          : vec3(clamp(-t * 2.0, 0.0, 1.0), 0.0, 0.0);
        } else {
            // Modo 4: DE ONDE vem a altura. Azul = alfa do MRAH-W (altura
            // cozida no load), laranja = passa-alta da luminancia do albedo
            // (a malha nao tem PBR proprio), preto = nenhuma fonte.
            float src = mod(inDispInfo.z, 10.0);
            col = src > 1.5 ? vec3(1.0, 0.5, 0.0)
                : src > 0.5 ? vec3(0.0, 0.3, 1.0)
                                     : vec3(0.0);
        }
        outAlbedo = vec4(col, 1.0);
        if (auditMode == 3 || auditMode == 6) {
            // MODOS 3 e 6 PRECISAM FICAR CHATOS DE PROPOSITO: os outros
            // modos (1,2,4) usam a normal REAL porque o alinhamento/relevo E'
            // o dado. Aqui o dado e' so' a cor (col) - passar a normal REAL
            // pro G-buffer
            // deixa a luz direcional sombrear o cinza pelo N.L de cada pixel,
            // e se a normal reconstruida estiver ruidosa (facetada), o N.L
            // varia forte de pixel a pixel e a imagem lê como cristal
            // facetado por cima do cinza - testado trocando o ganho de altura
            // de 3.5 pra 0.3 e a imagem NAO mudou, confirmando que quem
            // dominava a imagem nao era a altura, era o sombreamento da
            // normal. Normal FIXA (mesma em toda a tela, igual o wireframe
            // faz em outNormal) zera essa variacao de N.L: sobra so' o cinza,
            // comparavel de olho com a textura (modo 0).
            outNormal = vec4(0.5, 0.5, 1.0, 1.0);
            outPBR = vec4(0.0, 1.0, 0.0, 0.0); // R=source(livre aqui) G=roughness(1=fosco) B=metallic(0) A=wetness
        } else {
            outNormal = vec4(N * 0.5 + 0.5, 1.0);
            outPBR = vec4(0.5, 1.0, 0.0, 0.0);
        }
        outMaterialID = 0u;
        outEmissive = vec4(col, 1.0);   // sai da iluminacao: a cor E' o dado
        return;
    }

    // Normal mapping is only computed when a real normal map is bound.
    vec2 uv = inTexCoord;
    mat3 TBN;
    bool hasPbr = inPbrIndex != 0u;
    bool hasNormal = inNormalIndex != 0u;
    if (hasNormal || hasPbr) {
        TBN = computeTBN(N, inWorldPos, inTexCoord);
    }
    // POM REMOVIDO (decisão de projeto, docs/displacement_design.md): a
    // qualidade do parallax é função do ângulo de vista, e aqui o pitch da
    // câmera é variável livre - o jogador sempre alcança a zona ruim.
    // Relevo vem de: geometria real perto + sombra própria (horizon map,
    // dependente do SOL, não da vista) + normal map.

    uint texIdx = nonuniformEXT(inTexIndex);
    vec4 texColor = texture(u_textures[texIdx], uv);

    // DESCARTE ANTECIPADO POR ALFA. O teste de alfa ficava DEPOIS do splat
    // (ate' 4 texturas + mascara) e do blend (mais 2 buscas), entao um
    // fragmento de folha que ia ser descartado pagava ate' SEIS buscas de
    // textura a toa - e folha e' onde ha' mais sobredesenho, porque o
    // `discard` desabilita o early-Z e cada camada roda o shader inteiro.
    // Subir o teste e' seguro: o alfa NUNCA e' modificado depois (o blend
    // altera so' o .rgb, de proposito - misturar o alfa puxava texColor.a
    // abaixo do limiar e abria buracos). O teste de MAGENTA continua embaixo,
    // porque le' o rgb ja' misturado.
    if (texColor.a < 0.1) discard;
    // Ground crossfade. `inBlendWeight` is a per-vertex weight (0 when the mesh
    // carries no _BLEND_* attributes, so untagged maps are untouched). A plain
    // mix() of that weight interpolates linearly across each mesh triangle, so
    // the seam takes on the shape of the triangulation ("triangular", not a
    // real crossfade) - the two triangles of a quad interpolate differently
    // along their shared diagonal, and that diagonal becomes a visible seam.
    //
    // World-space splat mask (new, optional): when the primitive also carries
    // _BLEND_MASK_UV, `inBlendMaskIndex` points at a texture baked in world
    // space (not per-vertex), so sampling it is continuous and has no
    // per-triangle kink - this is the actual fix for the "triangular" seam.
    // It only REPLACES the weight's source; the gate stays `inBlendWeight >
    // 0.0` exactly as before; a cross-class junction (e.g. ground/cliff) is
    // only tagged near the cliff foot to begin with, and that height-band
    // rule is baked into the per-vertex weight, not representable in a flat
    // XZ texture, so the mask must never turn on blending the vertex weight
    // says is off. `inBlendTexIndex` still names which texture to fade
    // toward either way. Maps baked before this feature carry
    // inBlendMaskIndex == 0 and fall back to the per-vertex inBlendWeight
    // exactly as before - fully backward compatible.
    // ---- SPLAT DE 4 CAMADAS -------------------------------------------
    //
    // Por que isso existe: o blend de 2 materiais logo abaixo tem um teto
    // estrutural. Um triangulo carrega UM material base e UM alvo, entao onde
    // o material base precisa mudar a mudanca cai obrigatoriamente na aresta
    // do quad - grade de 10u alinhada aos eixos - e desenha retangulo e
    // triangulo na tela por mais perfeita que a mascara seja. Nao adianta
    // melhorar a mascara: o que quebra nao e' o peso, e' de onde vem a cor.
    //
    // Aqui TODA primitiva de terreno usa o mesmo material base e as mesmas 4
    // camadas. Quem decide a mistura e' so' a mascara RGBA amostrada em UV de
    // MUNDO, que e' continua por construcao e nao sabe nem que existe malha.
    // Nao sobra nada per-quad, entao nao ha' onde uma aresta aparecer.
    //
    // O peso da camada base e' o que sobra pra fechar 1, e o total e'
    // normalizado - assim a mascara nunca escurece nem estoura a superficie
    // por soma errada.
    bool hasSplat = (inSplatTex.x | inSplatTex.y) != 0u && inBlendMaskIndex != 0u;
    if (hasSplat) {
        vec4 w = texture(u_textures[nonuniformEXT(inBlendMaskIndex)], inBlendMaskUV);
        uint s0 =  inSplatTex.x        & 0xFFFFu;
        uint s1 = (inSplatTex.x >> 16) & 0xFFFFu;
        uint s2 =  inSplatTex.y        & 0xFFFFu;
        uint s3 = (inSplatTex.y >> 16) & 0xFFFFu;
        float wBaseSum = w.r + w.g + w.b + w.a;
        vec3 acc = vec3(0.0);
        float sum = 0.0;
        if (s0 != 0u) { acc += texture(u_textures[nonuniformEXT(s0)], uv).rgb * w.r; sum += w.r; }
        if (s1 != 0u) { acc += texture(u_textures[nonuniformEXT(s1)], uv).rgb * w.g; sum += w.g; }
        if (s2 != 0u) { acc += texture(u_textures[nonuniformEXT(s2)], uv).rgb * w.b; sum += w.b; }
        if (s3 != 0u) { acc += texture(u_textures[nonuniformEXT(s3)], uv).rgb * w.a; sum += w.a; }

        // Segunda mascara (camadas 4..7 - dobra o splat pra 9 no total com a
        // base). Mesma UV da primeira, indice de material proprio. Inerte
        // (peso 0) em qualquer GLB que so' tenha o splat de 4 - inBlendMaskIndex2
        // vem zero e a soma de pesos cai exatamente no caminho de antes.
        if (inBlendMaskIndex2 != 0u) {
            vec4 w2 = texture(u_textures[nonuniformEXT(inBlendMaskIndex2)], inBlendMaskUV);
            uint s4 =  inSplatTex2.x        & 0xFFFFu;
            uint s5 = (inSplatTex2.x >> 16) & 0xFFFFu;
            uint s6 =  inSplatTex2.y        & 0xFFFFu;
            uint s7 = (inSplatTex2.y >> 16) & 0xFFFFu;
            wBaseSum += w2.r + w2.g + w2.b + w2.a;
            if (s4 != 0u) { acc += texture(u_textures[nonuniformEXT(s4)], uv).rgb * w2.r; sum += w2.r; }
            if (s5 != 0u) { acc += texture(u_textures[nonuniformEXT(s5)], uv).rgb * w2.g; sum += w2.g; }
            if (s6 != 0u) { acc += texture(u_textures[nonuniformEXT(s6)], uv).rgb * w2.b; sum += w2.b; }
            if (s7 != 0u) { acc += texture(u_textures[nonuniformEXT(s7)], uv).rgb * w2.a; sum += w2.a; }
        }

        float wBase = max(0.0, 1.0 - wBaseSum);
        acc += texColor.rgb * wBase;
        sum += wBase;
        texColor.rgb = acc / max(sum, 1e-4);

        // VARIACAO MACRO por PIXEL, independente da malha. O COLOR_0 por
        // vertice (Step 6 do pipeline Python) so' varia na resolucao do
        // terreno (~10u entre vertices) - de perto o chao lia
        // "liso"/repetitivo mesmo com a textura certa, porque a mancha de
        // tom nunca era mais fina que o proprio quad. Duas amostras de fbm
        // de frequencia BEM baixa (manchas de dezenas a centenas de
        // unidades, nao granulacao fina - isso e' textura, nao variacao):
        // uma pra brilho, outra (fase/offset diferente) pra empurrar
        // quente/frio. Faixa pequena de proposito (10%/8%) - o objetivo e'
        // quebrar a repeticao, nao mudar qual material se le.
        float macroLum = eruptionMacroFbm(inWorldPos.xz, 1.0 / 180.0);
        float macroWarm = eruptionMacroFbm(inWorldPos.xz + vec2(500.0, 500.0), 1.0 / 260.0);
        texColor.rgb *= (1.0 + macroLum * 0.16);
        texColor.rgb *= vec3(1.0 + max(macroWarm, 0.0) * 0.12,
                              1.0 - abs(macroWarm) * 0.05,
                              1.0 - max(macroWarm, 0.0) * 0.09);
    }

    bool hasBlendMask = inBlendMaskIndex != 0u;
    float blendT = inBlendWeight;
    if (!hasSplat && inBlendWeight > 0.0) {
        uint bTexIdx = nonuniformEXT(inBlendTexIndex);
        vec4 bColor = texture(u_textures[bTexIdx], uv);
        if (hasBlendMask) {
            blendT = texture(u_textures[nonuniformEXT(inBlendMaskIndex)], inBlendMaskUV).r;
        }
        // Height-biased blend (familia "height lerp"): nudge the
        // weight by the two textures' own luminance so the 50% contour follows
        // surface detail instead of the mesh triangles (or the mask's own
        // resolution). Kept gentle - a strong bias or added noise turns the
        // band grainy.
        float hA = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        float hB = dot(bColor.rgb,  vec3(0.299, 0.587, 0.114));
        blendT = clamp(blendT + (hB - hA) * 0.35, 0.0, 1.0);
        // Colour only: mixing alpha too can pull texColor.a under the discard
        // threshold at strong blend weights and punch holes in the surface.
        texColor.rgb = mix(texColor.rgb, bColor.rgb, blendT);
    }

    float magentaness = min(texColor.r, texColor.b) - texColor.g;
    if (texColor.a < 0.1 || magentaness > 0.35) {
        discard;
    }

    // ERUPTION_TEST_MAGENTA_DEBUG=1: green=clean, cyan=residual magenta bleed
    if (push._pad > 0.5) {
        vec3 dbg = (magentaness > 0.05) ? vec3(0.0, 1.0, 1.0) : vec3(0.0, 1.0, 0.0);
        outAlbedo = vec4(dbg, 1.0);
        outNormal = vec4(0.5, 0.5, 1.0, 0.0);
        outPBR = vec4(0.0, 1.0, 0.0, 0.0);
        outMaterialID = inMatId & 0x7FFFu; // bit 15 = marca de costura do deslocamento
        outEmissive = vec4(dbg, 1.0);
        return;
    }

    // Keep albedo in sRGB so the legacy path matches the pre-PBR branch.
    // The PBR lighting pass converts to linear itself.
    vec3 albedo = texColor.rgb;

    // .a carries ambient occlusion (consumed by ambient.frag). Filled from the
    // PBR map's blue channel below; 1.0 = no occlusion for un-cooked meshes.
    outAlbedo = vec4(albedo, 1.0);

    // Ambient-occlusion term written into outAlbedo.a (consumed by ambient.frag).
    // Primary source is a cavity term derived from the normal map itself (see
    // below) - reliable and scaled by u_normalMapScale - plus a light touch of
    // the cooked MRAH-W AO. 1.0 = no occlusion.
    float aoTerm = 1.0;
    // AUDITORIA DE DUPLA OCLUSAO (Tefra->Basalto, "Dupla oclusao - auditoria
    // pedida"). Os dois termos de cavidade sao guardados separados para poder
    // combina-los por min() em vez de produto: bit 16 da mascara de ablacao
    // (ERUPTION_TEST_MICRO_SHADOW=23 = 7|16). Produto conta a MESMA oclusao
    // duas vezes - os dois medem fracao de hemisferio fechada por
    // micro-geometria, um vindo do normal map e outro assado na textura.
    float aoCavityNormal = 1.0;   // termo 1: inclinacao do normal map
    float aoCavityBaked  = 1.0;   // termo 2: AO cozido em mrahw.b

    // Normal mapping. Decode the tangent-space normal, optionally flip the
    // green channel for Y-down normal maps, then blend toward a flat normal
    // based on the global strength so 0 = off, 1 = authored, >1 = exaggerated.
    float materialProps = 0.0;
    // 1.0 = texel vê o sol; < 1 = na sombra do próprio relevo (ver abaixo).
    float sunSelfShadow = 1.0;
    // 1.0 = texel ve a luz de preenchimento; < 1 = na propria sombra do relevo.
    float fillSelfShadow = 1.0;
    // FORCA E SUAVIZACAO DO RELEVO PELA DISTANCIA (ideia do autor 2026-09-06).
    // Perto, o normal map de textura antiga e' agressivo demais - cada texel
    // vira contraste duro e, com a camera andando, cintila; longe ele e' a
    // unica coisa que ainda descreve a superficie, entao vale inteiro. O
    // vies de mip faz o caminho oposto: zero colado na camera (relevo 1:1 com
    // o albedo) e cheio na distancia media, onde o texel ja' nao resolve.
    // Os sliders do F2 continuam valendo como trim: forca multiplica, vies soma.
    float distCam = length(u_cameraPos - inWorldPos);
    float ndScale  = u_normalMapScale;
    float ndSmooth = u_normalSmoothing;
    if (u_normalDistParams.y > 0.5) {
        float t = distCam / max(u_normalDistParams.x, 1.0);
        ndScale  *= lut8(u_normalScaleLut0,  u_normalScaleLut1,  t);
        ndSmooth += lut8(u_normalSmoothLut0, u_normalSmoothLut1, t);
    }
    if (hasNormal) {
        uint nIdx = nonuniformEXT(inNormalIndex);
        vec4 sampledNormal = texture(u_textures[nIdx], uv, ndSmooth);
        if (!hasSplat && inBlendWeight > 0.0 && inBlendNormalIndex != 0u) {
            uint bnIdx = nonuniformEXT(inBlendNormalIndex);
            vec4 bNormal = texture(u_textures[bnIdx], uv, ndSmooth);
            sampledNormal = mix(sampledNormal, bNormal, blendT);
        }
        materialProps = sampledNormal.a;

        // NOTA: nada de reconstruir z de xy aqui - os normal maps do acervo
        // têm blue ~239 para flat (não-unitários) e o look do jogo depende
        // dessa convenção (z baixo amplifica o relevo após o normalize).
        // Por isso os normals cozidos (.etex) ficam em RGBA8 raw, não BC5.
        vec3 tangentNormal = sampledNormal.xyz * 2.0 - 1.0;
        if (u_normalMapInvertY > 0.5) {
            tangentNormal.y = -tangentNormal.y;
        }
        float strength = clamp(ndScale, 0.0, 3.0);
        // RELEVO CONTADO UMA VEZ SO'. Na banda perto o vertice ja' chega
        // DESLOCADO e com a normal do relevo (gradiente da altura, model.tese
        // / derivada em model.vert) - e o normal map vem da MESMA altura. Com
        // strength cheia aqui o sulco era iluminado duas vezes: uma pela
        // geometria, outra pelo normal map, e a luz nao batia com a forma
        // (autor: "a gente usava o normal antes, por algum motivo estava
        // bugado"). Recua o normal map pela MESMA rampa com que o
        // deslocamento entra na geometria: onde o relevo e' todo geometria
        // sobra so' o detalhe fino (35%), na borda da banda volta inteiro.
        if (u_tessParams.y > 0.5) {
            float fN = lut8(u_tessLut0, u_tessLut1, distCam / max(u_tessParams.x, 1.0));
            float inGeom = clamp((fN - 1.0) / max(u_tessParams.w - 1.0, 1e-3), 0.0, 1.0);
            strength *= mix(1.0, 0.35, inGeom);
        }
        vec3 flatNormal = vec3(0.0, 0.0, 1.0);
        tangentNormal = mix(flatNormal, tangentNormal, strength);
        // TETO DE INCLINACAO DA NORMAL (ver u_tessParams2.z; 0 desliga).
        //
        // O mix acima com strength > 1 EXTRAPOLA: com o normal do acervo
        // (azul ~239, ou seja z ~0,87), strength 2,5 leva o z a 0,68 e
        // multiplica xy por 2,5 - a normal deita quase na horizontal. O campo
        // de normais vira alta frequencia saturada, e a superficie le' como
        // papel amassado/aluminio, nao como relevo (autor, 2026-09-06: "fica
        // tudo parecendo papel molhado, impossivel isso ser ruido"). Medido na
        // vista de normal do G-buffer em cidade-D com strength 2,5: 31,5% dos
        // pixels vizinhos com salto de normal > 25/255, contra 1,5% com o
        // normal map desligado.
        //
        // O teto e' um ANGULO, nao um fator: xy fica limitado a
        // tan(anguloMax) * z, entao a inclinacao para de crescer mas o
        // DESENHO do relevo (a direcao de xy) e' preservado.
        if (u_tessParams2.z > 0.001) {
            float maxSlope = tan(clamp(u_tessParams2.z, 0.05, 1.55));
            float z = max(abs(tangentNormal.z), 1e-3);
            float xyLen = length(tangentNormal.xy);
            float lim = maxSlope * z;
            if (xyLen > lim) tangentNormal.xy *= lim / xyLen;
        }
        // Cavity AO from the normal map: where the surface tilts away from flat
        // (bump slopes, groove walls) tangentNormal.z drops below 1, so darken
        // there. Flat areas (z ~ 1) are untouched; strength 0 -> no cavity.
        uint msMask = uint(u_pomParams.y + 0.5); // ERUPTION_TEST_MICRO_SHADOW (ablacao)
        if ((msMask & 4u) != 0u) aoCavityNormal = mix(1.0, clamp(tangentNormal.z, 0.0, 1.0), 0.7);

        // Contact micro-shadow: march a luminance-derived height toward the sun
        // in tangent space. A higher nearby texel occludes this one - this is
        // the groove shadow that dot(N,L) alone can't produce (a normal map has
        // no self-occlusion). Recomputed every frame from the current sun dir,
        // so it swings correctly with the light. Folded into the AO channel.
        // DISTANCIA: a micro-sombra e' detalhe de sulco com marcha de 0,018
        // em UV. Alem de ~150 unidades isso e' SUB-PIXEL - nao aparece na
        // tela, mas custa caro: sao DUAS marchas de 5 passos, cada passo uma
        // busca de textura, ou seja ate' 10 buscas POR FRAGMENTO, e o passe
        // de G-buffer e' 87% limitado por pixel (medido: 0,93 ms fixos +
        // 3,08 ms/Mpx a 1080p) porque a folhagem usa descarte por alfa e nao
        // tem early-Z, entao cada camada de folha roda o shader inteiro.
        // Some suavemente entre 150 e 260 unidades para nao criar costura.
        float msFade = 1.0 - smoothstep(150.0, 260.0, distance(inWorldPos, u_cameraPos));
        if (strength > 0.01 && msFade > 0.01) {
            const vec3 LUMA = vec3(0.299, 0.587, 0.114);
            // Prefer the REAL displacement height packed in MRAH-W alpha
            // (255 = "no height data" -> luminance fallback).
            uint hIdx = hasPbr ? nonuniformEXT(inPbrIndex) : 0u;
            float hProbe = hasPbr ? texture(u_textures[hIdx], uv).a : 1.0;
            bool useDisp = hasPbr && hProbe < 0.995;
            float h0 = useDisp ? hProbe : dot(texColor.rgb, LUMA);
            const int   MS_STEPS = 5;
            const float MS_SCALE = 0.018;      // total march in uv units
            // LOD fixo: no rasante de VISTA o texel estica e a marcha com mip
            // automatico vira aliasing. Um nivel estavel mantem o sulco limpo
            // de qualquer angulo de camera.
            float msLod = textureQueryLod(u_textures[hIdx], uv).y + 0.5;

            // MARCHA 1, direcao do SOL -> luz direta (outEmissive.a). A
            // auto-sombra e' dependente do SOL, nao da vista: e' ela que faz
            // relevo LER numa camera de cima (a fresta entre as pedras
            // escurece e gira conforme o sol anda).
            if ((msMask & 1u) != 0u && u_sunDir.w > 0.01) {
                vec3 Lw  = normalize(-u_sunDir.xyz);        // world dir to the sun
                vec3 Lts = transpose(TBN) * Lw;            // into tangent space
                float ll = length(Lts);
                if (ll > 1e-4) {
                    Lts /= ll;
                    if (Lts.z > 0.06) {
                        // Sol rasante no espaco tangente: Lts.xy/Lts.z explode
                        // e a marcha amostra texels aleatorios longe -> ruido
                        // preto. Limita o passo E apaga a auto-sombra conforme
                        // o sol raspa (onde ela nao e' confiavel).
                        float sunGraze = smoothstep(0.06, 0.35, Lts.z);
                        vec2 marchDir = Lts.xy / max(Lts.z, 0.25);
                        float marchLen = min(length(marchDir), 3.0);
                        if (length(marchDir) > 1e-5) marchDir = normalize(marchDir) * marchLen;
                        vec2 dUV = marchDir * (MS_SCALE / float(MS_STEPS));
                        float occ = 0.0;
                        for (int s = 1; s <= MS_STEPS; ++s) {
                            float hs = useDisp
                                ? textureLod(u_textures[hIdx], uv + dUV * float(s), msLod).a
                                : dot(textureLod(u_textures[texIdx], uv + dUV * float(s), msLod).rgb, LUMA);
                            float rayH = h0 + MS_SCALE * (float(s) / float(MS_STEPS));
                            occ = max(occ, (hs - rayH) * 4.0);
                        }
                        occ *= sunGraze * msFade;
                        sunSelfShadow = clamp(1.0 - occ * strength * 0.45, 0.55, 1.0);
                    }
                }
            }

            // MARCHA 2, direcao FIXA -> ambiente (outNormal.a). O relevo que
            // impressiona na luz vem da auto-sombra, e ela so' existe para luz
            // direta - por isso na sombra tudo achatava. Esta marcha usa uma
            // direcao constante (nao o sol), entao da' o MESMO 3D onde o sol
            // nao bate e nao "anda" com o ciclo do dia.
            //
            // ANTES (ate 2026-09-05) ela ficava DENTRO do gate do sol acima
            // (Lts.z > 0.06): numa superficie virada pra longe do sol -
            // justamente a que esta' na sombra - nunca rodava, e fillSelfShadow
            // ficava 1.0. Resultado: o relevo SOME ao entrar na sombra ("na
            // sombra a luz apaga e a textura se move", relato do autor). E nao
            // multiplicava por msFade: pop seco a 260 unidades. Agora roda
            // independente do sol e some suave com a distancia.
            {
                vec3 Fw = normalize(vec3(0.35, 0.75, 0.55));
                vec3 Fts = transpose(TBN) * Fw;
                float fl = length(Fts);
                if (fl > 1e-4) {
                    Fts /= fl;
                    if (Fts.z > 0.15) {
                        vec2 fDir = Fts.xy / max(Fts.z, 0.3);
                        float fLen = min(length(fDir), 3.0);
                        if (length(fDir) > 1e-5) fDir = normalize(fDir) * fLen;
                        vec2 fUV = fDir * (MS_SCALE / float(MS_STEPS));
                        float fOcc = 0.0;
                        for (int s = 1; s <= MS_STEPS; ++s) {
                            float hs = useDisp
                                ? textureLod(u_textures[hIdx], uv + fUV * float(s), msLod).a
                                : dot(textureLod(u_textures[texIdx], uv + fUV * float(s), msLod).rgb, LUMA);
                            float rayH = h0 + MS_SCALE * (float(s) / float(MS_STEPS));
                            fOcc = max(fOcc, (hs - rayH) * 4.0);
                        }
                        fOcc *= msFade;
                        if ((msMask & 2u) != 0u) fillSelfShadow = clamp(1.0 - fOcc * strength * 0.8, 0.35, 1.0);
                    }
                }
            }
        }

        N = normalize(TBN * normalize(tangentNormal));
    }
    outNormal = vec4(N * 0.5 + 0.5, fillSelfShadow); // .a = auto-sombra constante (ambiente)
    // ERUPTION_TEST_MICRO_SHADOW bit 8 (diagnostico): compara o TBN de eixo de
    // mundo com o tangente REAL da UV (derivadas de tela). Ver com
    // ERUPTION_TEST_PBR_DEBUG=4. R = alinhamento de T, G = de B (1 = igual,
    // 0 = invertido, 0.5 = perpendicular). Amarelo = certo.
    if ((uint(u_pomParams.y + 0.5) & 8u) != 0u && (hasNormal || hasPbr)) {
        vec3 dPx = dFdx(inWorldPos), dPy = dFdy(inWorldPos);
        vec2 dUx = dFdx(inTexCoord), dUy = dFdy(inTexCoord);
        float det = dUx.x * dUy.y - dUy.x * dUx.y;
        vec3 Tt = (dPx * dUy.y - dPy * dUx.y);
        vec3 Bt = (dPy * dUx.x - dPx * dUy.x);
        if (abs(det) > 1e-12) { Tt /= det; Bt /= det; }
        Tt = normalize(Tt - N * dot(N, Tt)); Bt = normalize(Bt - N * dot(N, Bt));
        outNormal.rgb = vec3(0.5 + 0.5 * dot(Tt, TBN[0]), 0.5 + 0.5 * dot(Bt, TBN[1]), 0.5);
    }

    // PBR material.
    // Fallback (no cooked map): uses the global default roughness/metallic.
    // With a cooked map: scale the per-texture profile values by the MRAH-W
    // channels. MRAH-W layout: R=Metallic, G=Roughness, B=AO (cavidade),
    // A=Height (255 = sem dado) - ver PbrMapGen.hpp e o commit 2bea80f.
    float roughness = clamp(u_defaultRoughness, 0.0, 1.0);
    float metallic  = clamp(u_defaultMetallic, 0.0, 1.0);
    float wetness   = 0.0;
    float sourceFlag = 0.5; // fallback heuristic
    if (hasPbr) {
        uint pIdx = nonuniformEXT(inPbrIndex);
        vec4 mrahw = texture(u_textures[pIdx], uv);
        if (!hasSplat && inBlendWeight > 0.0 && inBlendPbrIndex != 0u) {
            uint bpIdx = nonuniformEXT(inBlendPbrIndex);
            vec4 bMrahw = texture(u_textures[bpIdx], uv);
            mrahw = mix(mrahw, bMrahw, blendT);
        }
        roughness = push.roughnessScale * mrahw.g;
        metallic  = push.metallicScale * mrahw.r;
        // MRAH-W alpha IS the cavity/pooling mask baked by PbrMapGen stage 5
        // (curvature * depression); height lives in .b (stage 6). This used to
        // be overwritten with u_pomParams.w -- the global rain intensity --
        // which made the channel a screen-wide constant and threw the baked
        // mask away. The lighting pass now drives wetness from the weather
        // itself, so the G-buffer carries pure MATERIAL data again. (IGNIS G3)
        wetness   = mrahw.a;
        sourceFlag = 1.0; // real PBR map
        // Light touch of the cooked MRAH-W AO on top of the normal-map cavity.
        aoCavityBaked = mix(1.0, mrahw.b, 0.5); // cavidade ESTAVEL no ambiente (nao depende do sol)
    }
    // COMBINACAO DOS DOIS TERMOS DE CAVIDADE: min(), nao produto.
    //
    // Os dois medem a MESMA grandeza - fracao do hemisferio fechada por
    // micro-geometria - so' que um vem da inclinacao do normal map e o outro
    // esta' assado em mrahw.b. Multiplicar conta a mesma oclusao duas vezes.
    // E' a mesma regra que directional.frag ja' aplica entre SSAO e AO.
    //
    // Medido (2026-09-11): trocar produto por min clareia a cena 1,74% em vila-A
    // e 0,75% em parana_field, com 0,02% dos pixels mudando mais que 8/255 -
    // ou seja, imperceptivel HOJE. O motivo de trocar mesmo assim e' que o erro
    // ESCALA COM A FORCA DO AO: com o AO cozido medio em 0,81 o produto perde
    // ~9% no pixel ocluido, e a frente de AO da nota (horizon AO de Timonen)
    // existe justamente para deixar esse canal mais forte. O produto e' uma
    // armadilha que cresce.
    // ERUPTION_TEST_MICRO_SHADOW=23 (7|16) devolve o produto legado.
    {
        uint aoMask = uint(u_pomParams.y + 0.5);
        aoTerm *= ((aoMask & 16u) != 0u) ? (aoCavityNormal * aoCavityBaked)
                                         : min(aoCavityNormal, aoCavityBaked);
    }
    // Floor at 0.02: the lighting passes treat albedo.a < 0.001 as an empty
    // G-buffer pixel and early-out to black.
    outAlbedo.a = max(aoTerm, 0.02);

    // FLAG DE FOLHAGEM (G32) no canal R, sem canal novo no G-buffer: o R so'
    // usava 2 valores em 256. 1.0/0.5 = real/fallback SEM folha; 0.875/0.625
    // = COM folha (instancia com swayAmount > 0 = vegetacao pela classificacao
    // de vento). Decodifica com fract(r*4) ~ 0.5. Os dois valores com folha
    // ficam >= 0.5 porque isPbrFallback (directional.frag) trata r < 0.5 como
    // sprite (Blinn-Phong legado).
    if (inSway > 1e-4) sourceFlag = (sourceFlag > 0.75) ? 0.875 : 0.625;
    // Layout: R=source flag (ver acima), G=roughness, B=metallic, A=wetness.
    outPBR = vec4(sourceFlag, roughness, metallic, wetness);

    // ALTURA DE MUNDO, quantizada em 8 bits sobre [-500, 1500].
    //
    // Este attachment (R8_UINT) carregava `inMatId`, que NINGUEM lia - nenhum
    // shader faz bind dele e o unico uso de materialView() era o proprio
    // getter. Era banda paga por nada todo frame.
    //
    // Agora carrega a altura, porque o depth buffer NAO e' confiavel como
    // fonte de posicao de mundo (ver G14 no PROTOCOLO.cai: a maior parte dos
    // pixels sai com COR escrita e PROFUNDIDADE ainda no valor de limpeza).
    // O minimapa precisa da altura para saber o que esta' submerso e para
    // marchar sombra em campo de altura, e passou a ler daqui.
    //
    // A faixa cobre desde o fundo de caldeira ate' o topo de torre nos mapas
    // atuais; ~7,8 unidades por passo, folgado para decidir "abaixo da agua".
    outMaterialID = uint(clamp((inWorldPos.y + 500.0) / 2000.0, 0.0, 1.0) * 255.0 + 0.5);
    // .a carrega a visibilidade do sol do micro-relevo; a luz direcional
    // multiplica por ela (directional.frag). RGB segue emissivo puro.
    //
    // O RGB era SEMPRE zero - ou seja, nada no jogo emitia luz própria, mesmo
    // com o atributo _EMISSIVE_STRENGTH já chegando até aqui (plumbing pronto,
    // ponta solta). Agora a lava do vulcão ativo acende de verdade: a força vem
    // do atributo por vértice e a cor sai da própria textura, puxada pro
    // laranja-quente, então a crosta escura fica sombria e os veios claros
    // brilham. ambient.frag soma esse RGB como luz. Malha sem o atributo
    // continua com inEmissiveStrength = 0 -> preto, comportamento igual ao de
    // antes.
    vec3 emissiveRgb = vec3(0.0);
    if (inEmissiveStrength > 0.01) {
        float hot = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        hot = pow(clamp(hot, 0.0, 1.0), 1.6);
        emissiveRgb = vec3(1.0, 0.42, 0.12) * hot * inEmissiveStrength * 0.85;
    }
    outEmissive = vec4(emissiveRgb, sunSelfShadow);
}
