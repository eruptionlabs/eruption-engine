// Corpo de model.tese: incluído por model.tese (com velocidade de objeto, FSR)
// e por model_novel.tese (ERUPTION_NO_VELOCITY: sem o alvo de velocidade).

// TESSELACAO - AVALIACAO. Interpola os atributos do triangulo e DESLOCA o
// vertice novo pela altura do MRAH-W (canal alfa; 255 = sem dado), a mesma
// height field que a banda de vertice do model.vert ja' usa - entao as duas
// tecnicas descrevem o MESMO relevo e a transicao entre elas nao tem degrau.
//
// A amplitude e' modulada pelo mesmo fator da spline, normalizado: onde a
// curva chega a 1 (fim da banda) o deslocamento e' ZERO e o patch coincide com
// o triangulo original. E' o que evita costura na fronteira.

layout(triangles, fractional_odd_spacing, ccw) in;

layout(location = 0) in vec3 inWorldPos[];
layout(location = 1) in vec2 inTexCoord[];
layout(location = 2) in vec3 inNormal[];
layout(location = 3) in flat uint inTexIndex[];
layout(location = 4) in flat uint inMatId[];
layout(location = 5) in vec4 inColor[];
layout(location = 6) in flat uint inPbrIndex[];
layout(location = 7) in flat uint inNormalIndex[];
layout(location = 8) in flat uint inBlendTexIndex[];
layout(location = 9) in flat uint inBlendPbrIndex[];
layout(location = 10) in flat uint inBlendNormalIndex[];
layout(location = 11) in float inBlendWeight[];
layout(location = 12) in flat uint inBlendMaskIndex[];
layout(location = 13) in vec2 inBlendMaskUV[];
layout(location = 14) in float inEmissiveStrength[];
layout(location = 15) in flat uvec2 inSplatTex[];
layout(location = 16) in flat uvec2 inSplatTex2[];
layout(location = 17) in flat uint inBlendMaskIndex2[];
layout(location = 18) in flat float inSway[];
// location 19: o TCS nao repassa mais (varying morto, tipo divergente - G1).

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out flat uint outTexIndex;
layout(location = 4) out flat uint outMatId;
layout(location = 5) out vec4 outColor;
layout(location = 6) out flat uint outPbrIndex;
layout(location = 7) out flat uint outNormalIndex;
layout(location = 8) out flat uint outBlendTexIndex;
layout(location = 9) out flat uint outBlendPbrIndex;
layout(location = 10) out flat uint outBlendNormalIndex;
layout(location = 11) out float outBlendWeight;
layout(location = 12) out flat uint outBlendMaskIndex;
layout(location = 13) out vec2 outBlendMaskUV;
layout(location = 14) out float outEmissiveStrength;
layout(location = 15) out flat uvec2 outSplatTex;
layout(location = 16) out flat uvec2 outSplatTex2;
layout(location = 17) out flat uint outBlendMaskIndex2;
layout(location = 18) out flat float outSway;
layout(location = 19) out vec4 outDispInfo;
#ifndef ERUPTION_NO_VELOCITY
layout(location = 20) in vec3 inPrevWorldPos[];
#endif
// Frame anterior: mesma interpolacao + o MESMO deslocamento do vertice final
// (o balanco de vento ja' veio dos vertices de controle).
#ifndef ERUPTION_NO_VELOCITY
layout(location = 20) out vec3 outPrevWorldPos;
#endif

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
    float alpha;
    float metallicScale;
    float roughnessScale;
    float _pad;           // ERUPTION_TEST_MAGENTA_DEBUG - nao e' padding
    vec4 uvTranslateRot;
    vec4 uvScale;
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
    vec4 u_sunDir;
    vec4 u_sunColor;
    vec4 u_ambientSky;
    vec4 u_ambientGround;
    vec4 u_pomParams;
    vec4 u_windParams;
    vec4 u_normalDistParams;
    vec4 u_normalScaleLut0;
    vec4 u_normalScaleLut1;
    vec4 u_normalSmoothLut0;
    vec4 u_normalSmoothLut1;
    // Tesselacao: x = distancia maxima da curva (u), y = 1 liga / 0 desliga,
    // z = amplitude do deslocamento (u), w = nao usado.
    vec4 u_tessParams;
    vec4 u_tessLut0;
    vec4 u_tessLut1;
    // x = reservado, y = espacamento alvo entre vertices da tesselacao (ver model.tesc).
    // z = teto de inclinacao da normal (radianos), w = teto de tesselacao da
    // folhagem. Ver model.frag / model.tesc.
    vec4 u_tessParams2;
    // Filtro passa-baixa (blur) sobre o height map, em niveis de mip
    // somados ao mip "perto" (2.0) que heightUv() usa - 0 = sem blur extra
    // (comportamento de antes). Slider "Height Blur".
    float u_tessHeightBlur;
};

// ALTURA PELA UV DA MALHA - o relevo coincide com a textura que se ve, onde o
// desenho e' escuro/fundo a geometria afunda (o que o olho espera). Existiu
// tambem um modo triplanar/mundo (removido a pedido do autor - a combo nao
// fazia sentido no menu, o modo UV e' o unico usado): amostrava pela POSICAO
// DE MUNDO em vez da UV pra fechar juntas onde a UV quebra (coluna de varios
// paineis, ilha de UV do chao), ao custo do relevo virar uma projecao no
// espaco que nao acompanha mais a textura ("papel molhado"). Se essa costura
// voltar a incomodar, e' o primeiro lugar pra olhar de novo (ver model.vert).
//
// Passa-alta (mip 2 menos mip 5) pelos mesmos motivos de sempre: a altura
// cozida vem da luminancia do albedo e a banda baixa dela e' sombra pintada.
// useAlpha=true: alfa do MRAH-W JA' E' altura normalizada (PbrMapGen faz
// min/max por imagem no bake) - ler direto. useAlpha=false: luminancia CRUA
// do albedo (sem bake nenhum), essa sim precisa da passa-alta (mip2 menos
// mip5) pra jogar fora a sombra pintada - ver o comentario grande la' embaixo
// (main()). Repetir a passa-alta TAMBEM em cima da altura ja' cozida virava
// DERIVADA da altura (tipo detector de borda), nao a altura em si: testado
// numa pedra de rua com grout escuro - baixar o ganho em 10x nao mudou o
// relevo nada (a derivada satura igual pra qualquer ganho pequeno o
// bastante pra nao zerar sozinho) e SSAO desligado tambem nao mudou, so'
// sobrou like ruido de cristal sem relacao nenhuma com a forma da pedra
// (autor: "nao ta com cara de paralelepipedo, ta tudo
// distorcido"). 255/255 exato (so' possivel num UNORM de 8 bits) e' o
// sentinela "sem dado" do bake (PbrMapGen) - vira neutro (0.5, sem vies),
// nao "ponto mais alto".
// MIP MINIMO PELA GRADE DE VERTICES (Nyquist): a altura e' amostrada por
// VERTICE, entao detalhe menor que 2 espacamentos da grade vira serrote na
// borda da pedra ("borda serrilhada"). texels por u = tamanho da textura x
// UV por u da malha (push.uvScale.z, mediana calculada no load). IDENTICO
// em model.vert e model.tese - o vertice de canto do patch tem que ler a
// mesma altura que o vertice original, senao abre fenda na borda da celula.
float dispMipFloor(uint texIdx) {
    float spacing = u_tessParams2.y;
    float uvPerU = push.uvScale.z;
    if (spacing <= 0.0 || uvPerU <= 0.0) return 0.0;
    float texels = float(textureSize(u_textures[nonuniformEXT(texIdx)], 0).x);
    return log2(max(texels * uvPerU * spacing * 2.0, 1.0));
}

float heightUv(uint texIdx, vec2 uvIn, float gain, bool useAlpha) {
    // Blur (slider "Height Blur") sobe os mips de leitura da altura - mip
    // maior = media de mais texels = ruido de alta frequencia sumindo antes
    // do ganho/contraste amplificar ele. 0 = mip 2/5 originais.
    float nearMip = max(2.0 + u_tessHeightBlur, dispMipFloor(texIdx));
    float farMip = nearMip + 3.0;
    if (useAlpha) {
        float a = textureLod(u_textures[nonuniformEXT(texIdx)], uvIn, nearMip).a;
        if (a > 0.999) return 0.5;
        // GANHO como CONTRASTE em torno de uma LINHA DE BASE DINAMICA - nao
        // mais 0.5 fixo, e nao mais diferenca entre dois mips proximos (isso
        // era a passa-alta dupla que virava derivada - ja' tirado). A
        // linha de base e' a MEDIA da textura inteira: mip 8.0 e' pedido
        // alto de proposito, o sampler clampa sozinho no ultimo nivel que
        // existir (pra uma textura de 256, isso e' ~1 texel = a cor media de
        // tudo). Sem isto, textura de material fotografado real - que NAO
        // distribui simetrico em volta de 0.5, pedra clara bate bem acima da
        // media mas o vao/grout escuro fica so' um pouco abaixo dela - saia
        // com o lado escuro quase sem deslocamento nenhum (autor, testado com
        // um modo de auditoria a parte que colore vermelho/verde pelo sinal:
        // "preto fica fixo (nem move), branca sobe" - o preto nao tava preso,
        // ele so' tava perto da MEDIA real da textura, que 0.5 fixo tratava
        // como se fosse alta). Ganho continua sendo o slider "Height
        // contrast", agora esticando/comprimindo em torno da media de
        // verdade em vez de um meio arbitrario.
        float base = textureLod(u_textures[nonuniformEXT(texIdx)], uvIn, 8.0).a;
        return clamp(0.5 + (a - base) * gain, 0.0, 1.0);
    }
    vec4 n = textureLod(u_textures[nonuniformEXT(texIdx)], uvIn, nearMip);
    vec4 f = textureLod(u_textures[nonuniformEXT(texIdx)], uvIn, farMip);
    const vec3 kL = vec3(0.2126, 0.7152, 0.0722);
    float hn = dot(n.rgb, kL);
    float hf = dot(f.rgb, kL);
    return clamp(0.5 + (hn - hf) * gain, 0.0, 1.0);
}

float distToSegment(vec3 p, vec3 a, vec3 b) {
    vec3 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
    return length(p - (a + ab * t));
}

float seamEdgeMask(bool sa, bool sb, vec3 p, vec3 a, vec3 b, float r) {
    if (sa && sb) return 0.0;
    float m = 1.0;
    if (sa) m *= smoothstep(0.0, r, distance(p, a));
    if (sb) m *= smoothstep(0.0, r, distance(p, b));
    return m;
}

float lut8(vec4 a, vec4 b, float t) {
    float x = clamp(t, 0.0, 1.0) * 7.0;
    int i = int(floor(x));
    float f = x - float(i);
    float v0 = i < 4 ? a[i] : b[i - 4];
    int j = min(i + 1, 7);
    float v1 = j < 4 ? a[j] : b[j - 4];
    return mix(v0, v1, f);
}

// MISTURA DE ALTURA POR CAMADA DE SPLAT (mesma mascara, mesmo peso que o
// model.frag usa pra misturar a COR - ver o bloco "hasSplat" la'). Sem isto a
// altura ficava sempre presa na camada base (indice 0, gainH em cima da
// textura errada) enquanto a cor via a mascara e trocava de camada em cada
// texel - o relevo simplesmente parava de bater com o que a tela mostra toda
// vez que uma camada de cima vencia a base (autor: "ele nao ta sendo feito
// sobre o albedo"). Cada camada usa PASSA-ALTA da sua propria luminancia
// (useAlpha=false: camada de splat e' so' albedo, nao tem PBR/MRAH-W propria)
// e a mistura e' a MESMA formula do frag: soma pesada, base fecha o que sobra
// de 1, normaliza no fim. Custa ate' 8 leituras de height a mais (1 par de
// mip por camada ativa) so' dentro da banda perto tesselada - caro se o autor
// usar as 8 camadas nesse alcance, mas o alcance ja' e' pequeno de proposito.
float heightSplatUv(vec2 uvIn, vec2 maskUv, uint baseTex, uvec2 splat, uvec2 splat2,
                     uint maskIdx, uint maskIdx2, float gain) {
    float hBase = heightUv(baseTex, uvIn, gain, false);
    if ((splat.x | splat.y) == 0u || maskIdx == 0u) return hBase;
    vec4 w = textureLod(u_textures[nonuniformEXT(maskIdx)], maskUv, 0.0);
    uint s0 =  splat.x        & 0xFFFFu;
    uint s1 = (splat.x >> 16) & 0xFFFFu;
    uint s2 =  splat.y        & 0xFFFFu;
    uint s3 = (splat.y >> 16) & 0xFFFFu;
    float wBaseSum = w.r + w.g + w.b + w.a;
    float acc = 0.0, sum = 0.0;
    if (s0 != 0u) { acc += heightUv(s0, uvIn, gain, false) * w.r; sum += w.r; }
    if (s1 != 0u) { acc += heightUv(s1, uvIn, gain, false) * w.g; sum += w.g; }
    if (s2 != 0u) { acc += heightUv(s2, uvIn, gain, false) * w.b; sum += w.b; }
    if (s3 != 0u) { acc += heightUv(s3, uvIn, gain, false) * w.a; sum += w.a; }
    if (maskIdx2 != 0u) {
        vec4 w2 = textureLod(u_textures[nonuniformEXT(maskIdx2)], maskUv, 0.0);
        uint s4 =  splat2.x        & 0xFFFFu;
        uint s5 = (splat2.x >> 16) & 0xFFFFu;
        uint s6 =  splat2.y        & 0xFFFFu;
        uint s7 = (splat2.y >> 16) & 0xFFFFu;
        wBaseSum += w2.r + w2.g + w2.b + w2.a;
        if (s4 != 0u) { acc += heightUv(s4, uvIn, gain, false) * w2.r; sum += w2.r; }
        if (s5 != 0u) { acc += heightUv(s5, uvIn, gain, false) * w2.g; sum += w2.g; }
        if (s6 != 0u) { acc += heightUv(s6, uvIn, gain, false) * w2.b; sum += w2.b; }
        if (s7 != 0u) { acc += heightUv(s7, uvIn, gain, false) * w2.a; sum += w2.a; }
    }
    float wBase = max(0.0, 1.0 - wBaseSum);
    acc += hBase * wBase;
    sum += wBase;
    return sum > 1e-4 ? acc / sum : hBase;
}


// NAO nomear o parametro 'x': o preprocessador do GLSL substitui tokens, e o
// 'x' de gl_TessCoord.x tambem seria trocado ("unknown swizzle selection").
#define BARY(attr_) (gl_TessCoord.x * attr_[0] + gl_TessCoord.y * attr_[1] + gl_TessCoord.z * attr_[2])

void main() {
    vec3 wp   = BARY(inWorldPos);
    const vec3 wpBase = wp;
    vec2 uv   = BARY(inTexCoord);
    vec3 nrm  = normalize(BARY(inNormal));

    outTexCoord = uv;
    outNormal = nrm;
    outColor = BARY(inColor);
    outBlendWeight = BARY(inBlendWeight);
    outBlendMaskUV = BARY(inBlendMaskUV);
    outEmissiveStrength = BARY(inEmissiveStrength);
    outSway = BARY(inSway);
    outDispInfo = vec4(1.0, 0.0, 0.0, 0.0);
    // Atributos 'flat' vem do primeiro vertice do patch: interpolar indice de
    // textura nao significa nada, e o triangulo inteiro compartilha o material.
    outTexIndex = inTexIndex[0];
    outMatId = inMatId[0];
    outPbrIndex = inPbrIndex[0];
    outNormalIndex = inNormalIndex[0];
    outBlendTexIndex = inBlendTexIndex[0];
    outBlendPbrIndex = inBlendPbrIndex[0];
    outBlendNormalIndex = inBlendNormalIndex[0];
    outBlendMaskIndex = inBlendMaskIndex[0];
    outSplatTex = inSplatTex[0];
    outSplatTex2 = inSplatTex2[0];
    outBlendMaskIndex2 = inBlendMaskIndex2[0];

    // DESLOCAMENTO. Amplitude = u_tessParams.z, atenuada pela posicao na curva:
    // 1 no pico da banda, 0 onde o fator volta a 1. Mip 2 na leitura de altura:
    // altura suave o bastante para nao serrilhar a silhueta e barata como VTF.
    if (u_tessParams.y > 0.5 && u_tessParams.z > 1e-5) {
        float d = distance(wp, u_cameraPos);
        float f = lut8(u_tessLut0, u_tessLut1, d / max(u_tessParams.x, 1.0));
        float amp = clamp((f - 1.0) / max(u_tessParams.w - 1.0, 1e-3), 0.0, 1.0);
        if (amp > 0.0) {
            // FONTE DE ALTURA.
            // 1) alfa do MRAH-W, que e' a altura cozida no load (PbrMapGen
            //    escreve height em .a; .b e' a cavidade/AO).
            // 2) sem PBR proprio - o caso do CHAO COM SPLAT, que resolve
            //    textura por camada - usa a luminancia do albedo em
            //    PASSA-ALTA: mip 2 menos mip 5, MISTURADA pela mesma mascara
            //    que decide a cor (heightSplatUv acima),
            //    nao so' a camada base. A luminancia crua NAO serve como altura
            //    porque estas texturas
            //    de acervo tem SOMBRA PINTADA: o vale escuro do desenho virava
            //    vale de geometria e o relevo seguia o AO da textura em vez do
            //    relevo real (autor, 2026-09-06: "o que ta fazendo o AO pra
            //    fazer esse tesselation?"). A passa-alta joga fora essa banda
            //    baixa e deixa so' o grao - pedra, sulco, reboco.
            float h = -1.0;
            float hSrc = 0.0; // 1 = MRAH-W (altura cozida), 2 = albedo
            float gainH = max(u_normalDistParams.w, 0.1);
            if (inPbrIndex[0] != 0u) {
                h = heightUv(inPbrIndex[0], uv, gainH, true);
                hSrc = 1.0;
            } else if (inTexIndex[0] != 0u) {
                // CHAO COM SPLAT: la' embaixo pode ter ate' 8 camadas por cima
                // da base - ver heightSplatUv acima.
                h = heightSplatUv(uv, outBlendMaskUV, inTexIndex[0], inSplatTex[0], inSplatTex2[0],
                                    inBlendMaskIndex[0], inBlendMaskIndex2[0], gainH);
                hSrc = 2.0;
            }
            if (h >= 0.0) {
                // MASCARA DE COSTURA, ESTANQUE. Canto marcado (bit 15 do matId -
                // ver ModelRenderer: posicao dividida com vertice de
                // textura/UV/normal diferente, ou borda de malha) = 0.
                //
                // Regra que garante que a junta fecha: num ponto EM CIMA de uma
                // aresta, a mascara depende SO' dos dois vertices dessa aresta
                // (e' o unico dado que o triangulo vizinho tambem tem). A versao
                // anterior usava distancia ate' as OUTRAS arestas do triangulo -
                // o vizinho nao ve' essas, calculava outra mascara e abria uma
                // fileira de fendinhas ao longo de cada aresta interna.
                //   seamEdgeMask: aresta com os 2 cantos marcados = 0 inteira;
                //     1 canto marcado = sobe em kSeamR a partir dele; 0 = 1.
                //   Interior: media das 3 arestas com peso b_j*b_k (zera fora da
                //     propria aresta, entao EM CIMA dela sobra so' ela), e o
                //     max() com a distancia ate' a borda devolve o miolo do tile
                //     - sem ele, triangulo com as 3 arestas marcadas ficava
                //     chapado inteiro (metade do chao de mapa legado).
                const float kSeamR = 0.6;
                bool sm0 = (inMatId[0] & 0x8000u) != 0u;
                bool sm1 = (inMatId[1] & 0x8000u) != 0u;
                bool sm2 = (inMatId[2] & 0x8000u) != 0u;
                vec3 bc = gl_TessCoord;
                float we0 = bc.y * bc.z, we1 = bc.z * bc.x, we2 = bc.x * bc.y;
                float wsum = we0 + we1 + we2;
                float edgeBlend;
                if (wsum < 1e-7) {
                    bool pinned = bc.x > 0.5 ? sm0 : (bc.y > 0.5 ? sm1 : sm2);
                    edgeBlend = pinned ? 0.0 : 1.0;
                } else {
                    edgeBlend = (we0 * seamEdgeMask(sm1, sm2, wp, inWorldPos[1], inWorldPos[2], kSeamR)
                               + we1 * seamEdgeMask(sm2, sm0, wp, inWorldPos[2], inWorldPos[0], kSeamR)
                               + we2 * seamEdgeMask(sm0, sm1, wp, inWorldPos[0], inWorldPos[1], kSeamR)) / wsum;
                }
                float dEdge = min(min(distToSegment(wp, inWorldPos[1], inWorldPos[2]),
                                      distToSegment(wp, inWorldPos[2], inWorldPos[0])),
                                  distToSegment(wp, inWorldPos[0], inWorldPos[1]));
                float seamMask = max(edgeBlend, smoothstep(0.0, kSeamR, dEdge));
                float ampW = u_tessParams.z * amp * push.uvScale.w * seamMask;
                vec3 dsp = nrm * ((h - 0.5) * ampW);
                wp += dsp;

                // NORMAL ANALITICA DO RELEVO, no lugar da derivada de tela.
                // A derivada e' plana por quad de pixel: com deslocamento
                // forte perto da camera, o chao aparecia FACETADO, em placas
                // triangulares. Aqui a normal sai do GRADIENTE da mesma altura
                // que deslocou o vertice - duas amostras a mais, ao longo de
                // dois eixos tangentes - entao a luz descreve a superficie
                // real e o resultado e' liso. So' roda na banda perto: e' a
                // "qualidade maxima onde a camera esta'", que e' o pedido.
                vec3 tA = normalize(abs(nrm.y) < 0.99 ? cross(nrm, vec3(0.0, 1.0, 0.0))
                                                      : vec3(1.0, 0.0, 0.0));
                vec3 tB = normalize(cross(nrm, tA));
                float eps = max(0.35, ampW * 0.5);
                float hA, hB;
                // O passo tangente vira passo de UV pela razao entre a
                // extensao do triangulo em mundo e em UV.
                vec2 duv = (inTexCoord[1] - inTexCoord[0]);
                vec3 dwp = (inWorldPos[1] - inWorldPos[0]);
                float sc = length(duv) / max(length(dwp), 1e-4);
                hA = heightUv(hSrc > 1.5 ? inTexIndex[0] : inPbrIndex[0],
                              uv + vec2(eps * sc, 0.0), gainH, hSrc < 1.5);
                hB = heightUv(hSrc > 1.5 ? inTexIndex[0] : inPbrIndex[0],
                              uv + vec2(0.0, eps * sc), gainH, hSrc < 1.5);
                vec3 pA = tA * eps + nrm * ((hA - 0.5) * ampW);
                vec3 pB = tB * eps + nrm * ((hB - 0.5) * ampW);
                vec3 nD = cross(pA, pB);
                float nl = length(nD);
                if (nl > 1e-6) {
                    nD /= nl;
                    if (dot(nD, nrm) < 0.0) nD = -nD;
                    outNormal = normalize(mix(nrm, nD, clamp(amp, 0.0, 1.0)));
                    outDispInfo.z += 10.0; // marca: normal ja' e' a do relevo
                }
                float dl = length(dsp);
                // AUDITORIA: alinhamento do deslocamento com a normal do
                // ponto e o modulo. Fica exposto no modo de auditoria do
                // model.frag para responder "isto esta' indo pela normal?"
                // com numero, nao com impressao.
                outDispInfo = vec4(dl > 1e-6 ? abs(dot(dsp / dl, nrm)) : 1.0, dl, hSrc, h);
            }

        }
    }

    outWorldPos = wp;
#ifndef ERUPTION_NO_VELOCITY
    outPrevWorldPos = BARY(inPrevWorldPos) + (wp - wpBase);
#endif
    gl_Position = push.viewProjection * vec4(wp, 1.0);
}
