#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// TESSELACAO DE HARDWARE, BANDA PERTO (pedido do autor 2026-09-06: "deforma a
// malha em tudo que ta com distancia de 10% da tela... zoom de 90~100%").
//
// Por que hardware e nao malha pre-subdividida: a malha subdividida e' guardada
// e custa VRAM - medido em docs/tessellation_viabilidade.md, o chao do
// parana_field (250 mil vertices, 498 mil triangulos, 32 MB) iria a 128 MB com
// UM nivel. A tesselacao de hardware amplifica entre o vertex shader e o
// rasterizador e nao guarda nada: 0 byte. O mesmo vertex buffer e o mesmo index
// buffer sao lidos como patch list de 3 pontos.
//
// O FATOR vem de uma SPLINE por distancia (a mesma familia das curvas de
// relevo, editavel no F2): fora da banda ela vale 1 e o patch sai identico ao
// triangulo original - transicao sem costura com a malha que ja' existe.

layout(vertices = 3) out;

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
layout(location = 19) in vec3 inDispInfo[];

layout(location = 0) out vec3 outWorldPos[];
layout(location = 1) out vec2 outTexCoord[];
layout(location = 2) out vec3 outNormal[];
layout(location = 3) out flat uint outTexIndex[];
layout(location = 4) out flat uint outMatId[];
layout(location = 5) out vec4 outColor[];
layout(location = 6) out flat uint outPbrIndex[];
layout(location = 7) out flat uint outNormalIndex[];
layout(location = 8) out flat uint outBlendTexIndex[];
layout(location = 9) out flat uint outBlendPbrIndex[];
layout(location = 10) out flat uint outBlendNormalIndex[];
layout(location = 11) out float outBlendWeight[];
layout(location = 12) out flat uint outBlendMaskIndex[];
layout(location = 13) out vec2 outBlendMaskUV[];
layout(location = 14) out float outEmissiveStrength[];
layout(location = 15) out flat uvec2 outSplatTex[];
layout(location = 16) out flat uvec2 outSplatTex2[];
layout(location = 17) out flat uint outBlendMaskIndex2[];
layout(location = 18) out flat float outSway[];
layout(location = 20) in vec3 inPrevWorldPos[];
layout(location = 20) out vec3 outPrevWorldPos[];
// A location 19 (outDispInfo) PARA aqui: o TES escreve a dele do zero, entao
// repassar era um varying morto - e estava declarado vec3 aqui contra vec2 la',
// que e' VUID-RuntimeSpirv-OpEntryPoint-07754. O glslang compila cada estagio
// isolado e nao pega; quem reclama e' o driver, e driver velho e' justamente o
// alvo (930M). Tefra 2026-09-06, G1.

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
    // z = amplitude do deslocamento (u), w = fator maximo da curva.
    vec4 u_tessParams;
    vec4 u_tessLut0;
    vec4 u_tessLut1;
    // x = 0 UV / 1 mundo, y = escala do mundo (ver model.tese),
    // z = inclinacao maxima da normal, w = TETO DE FATOR PARA FOLHAGEM.
    vec4 u_tessParams2;
};

float lut8(vec4 a, vec4 b, float t) {
    float x = clamp(t, 0.0, 1.0) * 7.0;
    int i = int(floor(x));
    float f = x - float(i);
    float v0 = i < 4 ? a[i] : b[i - 4];
    int j = min(i + 1, 7);
    float v1 = j < 4 ? a[j] : b[j - 4];
    return mix(v0, v1, f);
}

// Fator do VERTICE pela sua propria distancia. Usar o vertice (e nao o centro
// do patch) e' o que garante que dois patches vizinhos calculem o MESMO fator
// para a aresta que compartilham - se cada um usasse o proprio centro, os
// fatores divergiriam e abriria fenda entre eles.
float tessAt(int v) {
    float d = distance(inWorldPos[v], u_cameraPos);
    return max(lut8(u_tessLut0, u_tessLut1, d / max(u_tessParams.x, 1.0)), 1.0);
}

// PISO EM PIXELS (quad overshading). A GPU sombreia em blocos 2x2: um triangulo
// menor que um quad paga um quad INTEIRO. Subdividir abaixo de ~8 px de aresta
// multiplica o custo de fragment por ate' 4x sem entregar imagem - e' o defeito
// que fez a industria abandonar tesselacao ingenua (Brainerd, CoD: Ghosts,
// SIGGRAPH 2014; Hable 2025). A curva por distancia sozinha nao ve' isso: ela
// da' o mesmo fator para um triangulo de 8 u do chao e para um de 0,5 u de um
// prop, no mesmo raio de camera - errado nos dois sentidos.
//
// Comprimento da aresta em pixels: u_projection[1][1] = 1/tan(fovY/2), entao a
// escala de pixel por unidade de mundo a uma distancia d e'
// 0.5 * alturaDaTela * P11 / d.
//
// Depende SO' dos dois vertices da aresta - nunca do interior do patch - entao
// os dois patches vizinhos chegam ao mesmo numero e a aresta continua sem
// racha, que e' a mesma regra que ja' valia para tessAt().
const float kTargetEdgePixels = 8.0;

float edgePixels(int a, int b) {
    vec3 pa = inWorldPos[a];
    vec3 pb = inWorldPos[b];
    float d = max(distance(0.5 * (pa + pb), u_cameraPos), 1e-3);
    // abs(): a engine faz o Y-flip do Vulkan invertendo P11 (Camera.cpp:234),
    // entao o termo e' NEGATIVO. Sem o abs o piso dava numero negativo, o
    // max(.., 1.0) grampeava tudo em 1 e a tesselacao inteira sumia - medido,
    // 28% da tela mudando, que e' a assinatura de "tess desligada", nao de piso.
    return distance(pa, pb) * (0.5 * u_screenResolution.y * abs(u_projection[1][1]) / d);
}

// TETO DE FATOR PARA FOLHAGEM (u_tessParams2.w).
//
// A folha ganha, sim, com tesselacao - a subdivisao CURVA a lamina, e sem ela
// o cartao volta a ser reto e facetado, com aresta de poligono visivel de
// perto. Mas ela precisa de POUCO: 2 ou 3 segmentos ao longo da lamina bastam
// para a silhueta parar de ser reta. A curva por distancia dava a ela o mesmo
// fator de uma pedra, e e' de la' que vinha 5,90x de triangulo rasterizado
// para 1,08x de fragmento (medido, parana_demo colado).
//
// Cortar a folhagem INTEIRA foi tentado e e' longe demais: 72% dos pixels
// iluminados mudam a 75 u. Encurtar a BANDA dela tambem nao resolve, porque a
// folhagem que custa e' justamente a mais perto, que e' a que se ve' - medido,
// banda a 25% economiza 1,0 ms de 8,9. O eixo certo e' o FATOR.
//
// Depende so' de inSway, que e' por malha e igual nos dois vertices da aresta,
// entao a regra anti-crack continua valendo: dois patches vizinhos chegam ao
// mesmo numero para a aresta compartilhada.
float foliageCap(int a, int b) {
    float cap = u_tessParams2.w;
    if (cap <= 0.0) return 64.0;                     // teto desligado
    float sway = max(inSway[a], inSway[b]);
    return (sway > 0.0) ? cap : 64.0;
}

// Fator da ARESTA: a curva manda, o piso em pixels corta. min() e nao mix()
// porque o piso e' um limite fisico do rasterizador, nao uma preferencia.
float edgeFactor(int a, int b) {
    float fromCurve = max(tessAt(a), tessAt(b));
    float fromPixels = max(edgePixels(a, b) / kTargetEdgePixels, 1.0);
    return max(min(min(fromCurve, fromPixels), foliageCap(a, b)), 1.0);
}

void main() {
    outWorldPos[gl_InvocationID] = inWorldPos[gl_InvocationID];
    outTexCoord[gl_InvocationID] = inTexCoord[gl_InvocationID];
    outNormal[gl_InvocationID] = inNormal[gl_InvocationID];
    outTexIndex[gl_InvocationID] = inTexIndex[gl_InvocationID];
    outMatId[gl_InvocationID] = inMatId[gl_InvocationID];
    outColor[gl_InvocationID] = inColor[gl_InvocationID];
    outPbrIndex[gl_InvocationID] = inPbrIndex[gl_InvocationID];
    outNormalIndex[gl_InvocationID] = inNormalIndex[gl_InvocationID];
    outBlendTexIndex[gl_InvocationID] = inBlendTexIndex[gl_InvocationID];
    outBlendPbrIndex[gl_InvocationID] = inBlendPbrIndex[gl_InvocationID];
    outBlendNormalIndex[gl_InvocationID] = inBlendNormalIndex[gl_InvocationID];
    outBlendWeight[gl_InvocationID] = inBlendWeight[gl_InvocationID];
    outBlendMaskIndex[gl_InvocationID] = inBlendMaskIndex[gl_InvocationID];
    outBlendMaskUV[gl_InvocationID] = inBlendMaskUV[gl_InvocationID];
    outEmissiveStrength[gl_InvocationID] = inEmissiveStrength[gl_InvocationID];
    outSplatTex[gl_InvocationID] = inSplatTex[gl_InvocationID];
    outSplatTex2[gl_InvocationID] = inSplatTex2[gl_InvocationID];
    outBlendMaskIndex2[gl_InvocationID] = inBlendMaskIndex2[gl_InvocationID];
    outSway[gl_InvocationID] = inSway[gl_InvocationID];
    outPrevWorldPos[gl_InvocationID] = inPrevWorldPos[gl_InvocationID];

    if (gl_InvocationID == 0) {
        // Aresta i fica OPOSTA ao vertice i: a aresta 0 liga os vertices 1 e 2.
        float e0 = edgeFactor(1, 2);
        float e1 = edgeFactor(2, 0);
        float e2 = edgeFactor(0, 1);
        // PATCH FORA DA BANDA PASSA DIRETO (nivel 1), NAO E' DESCARTADO.
        //
        // Aqui era nivel 0 - descarte - e isso ABRIA BURACO NO CHAO. O corte
        // grosso e' por CELULA (ModelRenderer::flushRun): a celula perto vai
        // INTEIRA pelo pipeline de patch e nao e' desenhada por mais ninguem.
        // Mas a celula tem 96 u e a banda da curva acaba em ~100 u, entao os
        // triangulos da MESMA celula que caem alem da banda saiam com nivel 0
        // e simplesmente nao eram desenhados: um anel de chao faltando de
        // ~100 u ate' a borda da celula (~250 u). E' o "o chao sumiu, cinza e'
        // o fundo" que o autor reportou - reproduzido com ERUPTION_TEST_TESS
        // 0 x 1 na mesma pose: 3,60% da tela virava fundo, 0,00% depois.
        //
        // Nivel 1 devolve o triangulo original, sem subdividir, e a amplitude
        // ja' e' zero na borda da banda (model.tese), entao o vertice sai
        // identico ao do caminho normal - mesma imagem, sem costura.
        //
        // O medo registrado no comentario antigo (G-buffer a 303 ms) era de
        // quando o CHAO INTEIRO ia pro patch, antes da particao por celula.
        // Com a particao, o que passa por aqui e' so' a vizinhanca da camera.
        if (max(max(e0, e1), e2) <= 1.001) {
            gl_TessLevelOuter[0] = 1.0;
            gl_TessLevelOuter[1] = 1.0;
            gl_TessLevelOuter[2] = 1.0;
            gl_TessLevelInner[0] = 1.0;
            return;
        }
        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        // O interior nao e' compartilhado com vizinho: pode ser o maior dos tres.
        gl_TessLevelInner[0] = max(max(e0, e1), e2);
    }
}
