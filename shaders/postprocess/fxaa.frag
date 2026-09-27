#version 450
// FXAA (Fast Approximate Anti-Aliasing), formula publica de Timothy Lottes
// (Nvidia, "FXAA 3.11", dominio publico via Nvidia GameWorks). Roda em UM
// passe, sem historico de frame nenhum - por isso e' a escolha certa aqui:
// os sujeitos do quadro sao SPRITES COM ALPHA-TEST + dither de LOD (ver
// docs/plano_iluminacao_p0p2.md secao 7), e qualquer solucao com acumulacao
// temporal (TAA, SMAA T2x) borra ou fantasma nesses dois casos.
//
// NAO e' SMAA. SMAA precisa de duas texturas de lookup pre-computadas
// (Area/Search, ~340 KB) que nao existem neste repositorio e exigiriam
// buscar um asset externo; FXAA resolve o MESMO problema (serrilha de
// silhueta que sobra depois do specular AA) com uma formula auto-contida.
//
// Roda ANTES do upscale (upscale_sharpen.frag), na resolucao de RENDER: e'
// mais barato (menos pixels) e da' ao upsample uma entrada ja' suavizada.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_input;

layout(push_constant) uniform PushConstants {
    vec2 invResolution; // 1/largura, 1/altura da imagem de ENTRADA
} push;

// Luma perceptual (BT.601, a mesma constante do FXAA original).
float fxaaLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 texel = push.invResolution;
    vec3 colorCenter = texture(u_input, inUV).rgb;

    // Luma dos 4 vizinhos ortogonais + centro.
    float lumaN = fxaaLuma(texture(u_input, inUV + vec2(0.0, -texel.y)).rgb);
    float lumaS = fxaaLuma(texture(u_input, inUV + vec2(0.0,  texel.y)).rgb);
    float lumaE = fxaaLuma(texture(u_input, inUV + vec2( texel.x, 0.0)).rgb);
    float lumaW = fxaaLuma(texture(u_input, inUV + vec2(-texel.x, 0.0)).rgb);
    float lumaM = fxaaLuma(colorCenter);

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range = lumaMax - lumaMin;

    // Limiar padrao do FXAA 3.11: pula pixel de baixo contraste local (nao
    // e' borda) e pixel muito escuro (ruido de sombra nao deveria suavizar).
    const float EDGE_THRESHOLD_MIN = 0.0312;
    const float EDGE_THRESHOLD = 0.125;
    if (range < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD)) {
        outColor = vec4(colorCenter, 1.0);
        return;
    }

    // Luma dos 4 diagonais, para achar a ORIENTACAO da borda (horizontal x
    // vertical) antes de decidir a direcao de blend.
    float lumaNW = fxaaLuma(texture(u_input, inUV + vec2(-texel.x, -texel.y)).rgb);
    float lumaNE = fxaaLuma(texture(u_input, inUV + vec2( texel.x, -texel.y)).rgb);
    float lumaSW = fxaaLuma(texture(u_input, inUV + vec2(-texel.x,  texel.y)).rgb);
    float lumaSE = fxaaLuma(texture(u_input, inUV + vec2( texel.x,  texel.y)).rgb);

    float edgeHorz = abs((lumaNW + lumaNE) - (lumaSW + lumaSE)) +
                      abs((lumaW  * 2.0)   - (lumaNW + lumaSW)) * 0.5 +
                      abs((lumaE  * 2.0)   - (lumaNE + lumaSE)) * 0.5;
    float edgeVert = abs((lumaNW + lumaSW) - (lumaNE + lumaSE)) +
                      abs((lumaN  * 2.0)   - (lumaNW + lumaNE)) * 0.5 +
                      abs((lumaS  * 2.0)   - (lumaSW + lumaSE)) * 0.5;
    bool isHorizontal = edgeHorz >= edgeVert;

    // Gradiente ao longo do eixo PERPENDICULAR a borda, pra saber pra qual
    // lado o blend deve puxar.
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float grad1 = abs(luma1 - lumaM);
    float grad2 = abs(luma2 - lumaM);
    bool steepest1 = grad1 >= grad2;
    float gradientScaled = 0.25 * max(grad1, grad2);

    vec2 stepDir = isHorizontal ? vec2(0.0, texel.y) : vec2(texel.x, 0.0);
    if (!steepest1) stepDir = -stepDir;

    // Media entre o centro e o vizinho do lado com maior gradiente: e' o
    // "primeiro passo" do FXAA original, suficiente pra remover a escada de
    // 1 texel que sobra depois do specular AA (que ja' resolve cintilacao,
    // nao serrilha de silhueta).
    vec3 colorNeighbor = texture(u_input, inUV + stepDir * 0.5).rgb;
    vec3 blended = mix(colorCenter, colorNeighbor, 0.5);

    // Nao deixa o blend passar do range [lumaMin, lumaMax] local - trava
    // contra halo em borda de alto contraste (texto do HUD, se algum dia
    // passar por aqui).
    float lumaBlended = fxaaLuma(blended);
    if (lumaBlended < lumaMin - gradientScaled || lumaBlended > lumaMax + gradientScaled) {
        blended = mix(colorCenter, colorNeighbor, 0.25);
    }

    outColor = vec4(blended, 1.0);
}
