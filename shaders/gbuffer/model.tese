#version 450
#extension GL_EXT_nonuniform_qualifier : enable

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
layout(location = 19) out vec3 outDispInfo;
layout(location = 20) in vec3 inPrevWorldPos[];
// Frame anterior: mesma interpolacao + o MESMO deslocamento do vertice final
// (o balanco de vento ja' veio dos vertices de controle).
layout(location = 20) out vec3 outPrevWorldPos;

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

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
    // x = 0 UV / 1 mundo, y = escala do mundo. Ver heightAt().
    vec4 u_tessParams2;
};

// ALTURA AMARRADA NO MUNDO (triplanar), nao na UV.
//
// O problema que isto resolve: uma coluna feita de 8 painteis, ou dois pedacos
// de chao, encostam no mesmo ponto do MUNDO mas com UVs DIFERENTES. Amostrando
// a altura pela UV, cada lado recebe um valor diferente, desloca para lugares
// diferentes e ABRE UM VAO na junta (autor, 2026-09-06: "na hora que aplica o
// tesselation elas nao tao se juntando, fica um vao aberto"). Amostrando pela
// POSICAO DE MUNDO, dois vertices coincidentes leem exatamente o mesmo texel -
// mesmo deslocamento, junta fechada por construcao.
//
// Projecao triplanar (peso pelo quadrado da normal) em vez de um eixo so':
// escolher "o eixo dominante" reabriria a costura justamente onde a superficie
// passa de 45 graus e o eixo troca.
//
// Passa-alta (mip 2 menos mip 5) pelos mesmos motivos de sempre: a altura
// cozida vem da luminancia do albedo e a banda baixa dela e' sombra pintada.
// AS DUAS FONTES DE ALTURA, e por que existe escolha:
//
// UV DA MALHA: o relevo coincide com a textura que se ve - onde o desenho e'
//   escuro/fundo, a geometria afunda. E' o que o olho espera. O preco: onde a
//   UV quebra (dois paineis de uma coluna, ilha de UV do chao), os dois lados
//   leem alturas diferentes e a junta ABRE.
// MUNDO (triplanar): dois pontos coincidentes leem o mesmo texel, entao junta
//   nunca abre. O preco: o padrao de relevo NAO acompanha mais a textura -
//   ele passa a ser uma projecao no espaco, com o periodo do ladrilho de mundo,
//   e le' como ondulacao mole ("papel molhado") em vez de grao da pedra.
//
// Nao ha' terceira opcao barata: ou a altura e' funcao da UV (e herda as
// costuras da UV), ou e' funcao da posicao (e ignora a UV).
// Altura pela UV da malha, em passa-alta (mip 2 menos mip 5) pelo mesmo motivo
// de sempre: a altura cozida vem da luminancia do albedo e a banda baixa dela
// e' a sombra pintada.
float heightUv(uint texIdx, vec2 uvIn, float gain, bool useAlpha) {
    vec4 n = textureLod(u_textures[nonuniformEXT(texIdx)], uvIn, 2.0);
    vec4 f = textureLod(u_textures[nonuniformEXT(texIdx)], uvIn, 5.0);
    const vec3 kL = vec3(0.2126, 0.7152, 0.0722);
    float hn = useAlpha ? n.a : dot(n.rgb, kL);
    float hf = useAlpha ? f.a : dot(f.rgb, kL);
    return clamp(0.5 + (hn - hf) * gain, 0.0, 1.0);
}

float heightTriplanar(uint texIdx, vec3 wp, vec3 nrm, float worldScale, float gain,
                      bool useAlpha) {
    vec3 w = abs(nrm);
    w = w / max(w.x + w.y + w.z, 1e-4);
    vec2 uvX = wp.zy * worldScale;
    vec2 uvY = wp.xz * worldScale;
    vec2 uvZ = wp.xy * worldScale;
    vec4 nX = textureLod(u_textures[nonuniformEXT(texIdx)], uvX, 2.0);
    vec4 nY = textureLod(u_textures[nonuniformEXT(texIdx)], uvY, 2.0);
    vec4 nZ = textureLod(u_textures[nonuniformEXT(texIdx)], uvZ, 2.0);
    vec4 fX = textureLod(u_textures[nonuniformEXT(texIdx)], uvX, 5.0);
    vec4 fY = textureLod(u_textures[nonuniformEXT(texIdx)], uvY, 5.0);
    vec4 fZ = textureLod(u_textures[nonuniformEXT(texIdx)], uvZ, 5.0);
    const vec3 kLum = vec3(0.2126, 0.7152, 0.0722);
    float hNear = useAlpha ? (nX.a * w.x + nY.a * w.y + nZ.a * w.z)
                           : (dot(nX.rgb, kLum) * w.x + dot(nY.rgb, kLum) * w.y + dot(nZ.rgb, kLum) * w.z);
    float hFar  = useAlpha ? (fX.a * w.x + fY.a * w.y + fZ.a * w.z)
                           : (dot(fX.rgb, kLum) * w.x + dot(fY.rgb, kLum) * w.y + dot(fZ.rgb, kLum) * w.z);
    return clamp(0.5 + (hNear - hFar) * gain, 0.0, 1.0);
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
    outDispInfo = vec3(1.0, 0.0, 0.0);
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
            //    textura por camada e deixa o indice base em 0 - usa a
            //    luminancia do albedo em PASSA-ALTA: mip 2 menos mip 5. A
            //    luminancia crua NAO serve como altura porque estas texturas
            //    de acervo tem SOMBRA PINTADA: o vale escuro do desenho virava
            //    vale de geometria e o relevo seguia o AO da textura em vez do
            //    relevo real (autor, 2026-09-06: "o que ta fazendo o AO pra
            //    fazer esse tesselation?"). A passa-alta joga fora essa banda
            //    baixa e deixa so' o grao - pedra, sulco, reboco.
            float h = -1.0;
            float hSrc = 0.0; // 1 = MRAH-W (altura cozida), 2 = albedo
            float gainH = max(u_normalDistParams.w, 0.1);
            bool worldSpace = u_tessParams2.x > 0.5;
            float wsc = max(u_tessParams2.y, 1e-4);
            if (inPbrIndex[0] != 0u) {
                h = worldSpace ? heightTriplanar(inPbrIndex[0], wp, nrm, wsc, gainH, true)
                                   : heightUv(inPbrIndex[0], uv, gainH, true);
                hSrc = 1.0;
            } else if (inTexIndex[0] != 0u) {
                h = worldSpace ? heightTriplanar(inTexIndex[0], wp, nrm, wsc, gainH, false)
                                   : heightUv(inTexIndex[0], uv, gainH, false);
                hSrc = 2.0;
            }
            if (h >= 0.0) {
                float ampW = u_tessParams.z * amp * push.uvScale.w;
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
                if (worldSpace) {
                    hA = heightTriplanar(hSrc > 1.5 ? inTexIndex[0] : inPbrIndex[0],
                                         wp + tA * eps, nrm, wsc, gainH, hSrc < 1.5);
                    hB = heightTriplanar(hSrc > 1.5 ? inTexIndex[0] : inPbrIndex[0],
                                         wp + tB * eps, nrm, wsc, gainH, hSrc < 1.5);
                } else {
                    // Em UV, o passo tangente vira passo de UV pela razao entre
                    // a extensao do triangulo em mundo e em UV.
                    vec2 duv = (inTexCoord[1] - inTexCoord[0]);
                    vec3 dwp = (inWorldPos[1] - inWorldPos[0]);
                    float sc = length(duv) / max(length(dwp), 1e-4);
                    hA = heightUv(hSrc > 1.5 ? inTexIndex[0] : inPbrIndex[0],
                                  uv + vec2(eps * sc, 0.0), gainH, hSrc < 1.5);
                    hB = heightUv(hSrc > 1.5 ? inTexIndex[0] : inPbrIndex[0],
                                  uv + vec2(0.0, eps * sc), gainH, hSrc < 1.5);
                }
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
                outDispInfo = vec3(dl > 1e-6 ? abs(dot(dsp / dl, nrm)) : 1.0, dl, hSrc);
            }

        }
    }

    outWorldPos = wp;
    outPrevWorldPos = BARY(inPrevWorldPos) + (wp - wpBase);
    gl_Position = push.viewProjection * vec4(wp, 1.0);
}
