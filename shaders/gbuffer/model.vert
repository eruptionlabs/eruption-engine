#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in uint inTexIndex;
layout(location = 4) in uint inMatId;
layout(location = 5) in uint inColor;
layout(location = 6) in uint inPbrIndex;
layout(location = 7) in uint inNormalIndex;
layout(location = 8) in uint inBlendTexIndex;
layout(location = 9) in uint inBlendPbrIndex;
layout(location = 10) in uint inBlendNormalIndex;
layout(location = 11) in float inBlendWeight;
layout(location = 12) in uint inBlendMaskIndex;
layout(location = 13) in vec2 inBlendMaskUV;
layout(location = 14) in float inEmissiveStrength;
layout(location = 15) in uvec2 inSplatTex;
layout(location = 16) in uvec2 inSplatTex2;
layout(location = 17) in uint inBlendMaskIndex2;

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
// G32: balanco da instancia (uvScaleDisp.w) - o fragment usa como FLAG DE
// FOLHAGEM (vegetacao pela classificacao de vento) no canal R do PBR.
layout(location = 18) out flat float outSway;
// AUDITORIA DO DESLOCAMENTO (ver model.frag, u_normalDistParams.z):
// x = |dot(direcao do deslocamento, normal)| - 1 significa exatamente ao
// longo da normal; y = modulo do deslocamento em unidades de mundo.
layout(location = 19) out vec3 outDispInfo;

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

// Prefixo do FrameUBO compartilhado (ver model.frag) - só o que o vertex
// stage precisa; um bloco menor que o buffer é válido com offsets idênticos.
layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_viewProjection;
    mat4 u_inverseView;
    mat4 u_inverseProjection;
    vec3 u_cameraPos;
    float u_time;
    // O bloco declarava so' ate' aqui. O resto do prefixo entrou porque o
    // vento mora no FIM do UBO e, para alcancar um campo anexado, todos os
    // anteriores precisam existir com os mesmos offsets.
    vec2 u_screenResolution;
    float u_nearPlane;
    float u_farPlane;
    uint  u_frameIndex;
    uint  u_debugMode;
    float u_spriteExposure;
    float u_giIntensity;
    float u_giAmbientFloor;
    float u_spriteTilt;
    float u_shadowHeightScale;
    float u_spriteNormalYMix;
    float u_normalMapScale;
    float u_normalMapInvertY;
    float u_normalSmoothing;
    float u_defaultRoughnessUbo;
    float u_defaultMetallicUbo;
    vec4 u_sunDir;
    vec4 u_sunColor;
    vec4 u_ambientSky;
    vec4 u_ambientGround;
    vec4 u_pomParamsUbo;
    // xyz = vetor de vento em mundo (direcao * forca, ja' com rajada),
    // w = tempo proprio do vento em segundos.
    vec4 u_windParams;
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
    // x = 0 UV / 1 mundo, y = escala do mundo. Ver heightAt().
    vec4 u_tessParams2;
};

// -----------------------------------------------------------------------------
// Balanco de vegetacao ao vento
// -----------------------------------------------------------------------------
// Ate' aqui o vento nao movia UMA folha: existia como direcao de nuvem e como
// inclinacao de chuva, e so'. Uma cena com vegetacao completamente imovel
// enquanto a nuvem corre e a chuva vem torta le como cenario congelado.
//
// O modelo e' o de uma haste engastada: o deslocamento cresce com a ALTURA
// acima da base (topo anda, base nao), a favor do vento, com uma oscilacao em
// torno dessa posicao inclinada. Nao e' senoide pura em volta do zero - planta
// sob vento constante fica INCLINADA e treme em torno da inclinacao.
//
// A fase vem da posicao no mundo, entao cada moita anda no seu tempo. Sem isso
// um campo inteiro balanca em unissono, que le como animacao e nao como vento.
vec3 applyWindSway(vec3 worldPos, float localHeight, float sway) {
    if (sway <= 1e-4) return worldPos;

    vec3 wind = u_windParams.xyz;
    // windParams.y carrega a ESCALA de balanco (graphics.json wind_sway_scale).
    // O vento e' sempre horizontal, entao o y do vetor era sempre 0 e a vaga
    // estava livre - nao custa slot novo de UBO.
    float swayScale = max(u_windParams.y, 0.0);
    wind.y = 0.0;
    float strength = length(wind.xz);
    if (strength <= 1e-4 || swayScale <= 0.0) return worldPos;
    vec3 windDir = wind / strength;   // SO' a direcao daqui pra frente

    float t = u_windParams.w;

    // Fase por posicao: numeros irracionais evitam que a moita caia em
    // ressonancia com a grade do terreno e apareca um padrao regular.
    float phase = dot(worldPos.xz, vec2(0.0731, 0.0917));

    // Duas frequencias: o bamboleio lento da haste e o tremor rapido da folha.
    float slow = sin(t * 1.9 + phase);
    float fast = sin(t * 5.7 + phase * 2.3);
    float osc = slow * 0.65 + fast * 0.35;

    // RESPOSTA AO VENTO. Antes a forca entrava DUAS vezes (o vetor `wind` ja'
    // tem magnitude = forca, e ela era multiplicada de novo), entao o
    // deslocamento ia com o QUADRADO: em tempo limpo a forca e' 0,082 e o
    // resultado era 0,0067 - movimento sub-pixel, vegetacao parada na tela.
    // Medido antes da correcao com camera imovel: so' 0,82% dos pixels
    // mudavam em clear e 4,64% em stormy. Agora a forca entra UMA vez, com
    // piso: mato de verdade nunca esta' totalmente imovel, e sem o piso o
    // tempo limpo (forca 0,08) continuaria congelado.
    float response = 0.30 + 0.70 * clamp(strength, 0.0, 1.0);

    // Engaste: cresce com o QUADRADO da altura, como viga engastada - a base
    // nao se mexe e o topo anda. TETO de 20% da altura da planta: sem ele o
    // h*h faz uma arvore de 20 unidades deslocar 60, o que a arranca do chao.
    float h = max(localHeight, 0.0);
    // TETO como fracao da altura, e o teto tambem obedece ao knob: com
    // wind_sway_scale = 0.15 uma planta nunca inclina mais que 1,5% da propria
    // altura por unidade de escala. Sem teto o h*h faz uma arvore de 20
    // unidades deslocar 60 e ela sai do chao. Medido no parana com o teto em
    // 0.20: 76% dos pixels mudavam e a copa DESLIZAVA em vez de balancar.
    float bend = min(h * h * sway * swayScale, h * sway * swayScale * 0.65) * response;

    // A parcela que OSCILA passa a ser a maior parte (0,45 contra 0,25): a
    // inclinacao fixa nao le' como vento, e' so' um objeto numa posicao
    // ligeiramente diferente. O que o olho ve' como vento e' a variacao.
    float gust = 0.55 + 0.45 * osc;
    worldPos.xz += windDir.xz * bend * gust;
    // Uma pitada vertical: a ponta desce um pouco quando inclina, porque a
    // haste tem comprimento fixo. Sem isso a planta parece esticar.
    worldPos.y -= bend * 0.12 * gust;
    return worldPos;
}

// Dados POR INSTANCIA (SIMT). Antes, cada instancia visivel era um
// vkCmdPushConstants + vkCmdDrawIndexed proprios: ~1,5 us de CPU por draw,
// 1940 draws em parana_field = o passe inteiro serializado na CPU enquanto a
// GPU esperava. Agora a CPU escreve este buffer uma vez e emite UM draw
// instanciado por malha; a GPU indexa por gl_InstanceIndex em paralelo.
struct EngInstance {
    mat4 model;
    vec4 uvTranslateRot; // xy = translate, z = rotation (rad), w = geoDispDistance
    vec4 uvScaleDisp;    // xy = scale, z = geoDispAmplitude, w = livre
};
layout(set = 2, binding = 0, std430) readonly buffer InstanceBuf {
    EngInstance u_inst[];
};

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
    float alpha;
    float metallicScale;  // profile metallic factor, scales the MRAH-W red channel
    float roughnessScale; // profile roughness factor, scales the MRAH-W green channel
    float _pad;           // ERUPTION_TEST_MAGENTA_DEBUG (nao e' padding!)
    vec4 uvTranslateRot;  // xy = translate, z = rotation (radians), w = unused
    // uvScale.w = FATOR DE DESLOCAMENTO POR MATERIAL (chao 1,0, pedra 0,45,
    // telhado/metal/agua 0) - ver o bloco de categoria em ModelRenderer.
    vec4 uvScale;         // xy = scale, z = geo displacement (unidades de
                          // mundo, já com fade de banda; 0 = off), w = unused
} push;

vec2 applyUvAnimation(vec2 uv) {
    // Legacy editor UV transform order (column-major):
    //   T(+center) * S * R * T(translate) * T(-center)
    // In 2D that becomes:
    //   1. uv -= center
    //   2. uv += translate
    //   3. uv = rotate(uv)
    //   4. uv *= scale
    //   5. uv += center
    const vec2 center = vec2(0.5, 0.5);
    uv -= center;
    uv += u_inst[gl_InstanceIndex].uvTranslateRot.xy;
    float rot = u_inst[gl_InstanceIndex].uvTranslateRot.z;
    if (abs(rot) > 0.0001) {
        float c = cos(rot);
        float s = sin(rot);
        uv = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
    }
    uv *= u_inst[gl_InstanceIndex].uvScaleDisp.xy;
    uv += center;
    return uv;
}

// Tabela de 8 valores com interpolacao linear (ver model.tesc).
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

void main() {
    vec4 worldPos = u_inst[gl_InstanceIndex].model * vec4(inPosition, 1.0);

    // Balanco ao vento. A altura vem do espaco de MODELO (inPosition.y), onde a
    // base da planta e' y=0 - e' o unico lugar onde "altura acima da base" e'
    // conhecida sem carregar a AABB da instancia para o shader.
    // ALTURA EM ESPACO DE MUNDO, nao a local. `bend` e' somado a worldPos,
    // entao usar inPosition.y (espaco do modelo) so' acerta se a matriz nao
    // escalar. Estas malhas SAO escaladas: com a altura local o deslocamento
    // saia' na escala errada (minusculo) e a vegetacao continuava parada
    // mesmo com o vento chegando ao shader. A altura acima da ORIGEM da
    // instancia e' o que interessa - a base do objeto e' o engaste.
    float instBaseY = (u_inst[gl_InstanceIndex].model * vec4(0.0, 0.0, 0.0, 1.0)).y;
    worldPos.xyz = applyWindSway(worldPos.xyz, worldPos.y - instBaseY,
                                 u_inst[gl_InstanceIndex].uvScaleDisp.w);
    outSway = u_inst[gl_InstanceIndex].uvScaleDisp.w;
    outDispInfo = vec3(0.0);

    vec2 animUv = applyUvAnimation(inTexCoord);

    mat3 normalMatrix = transpose(inverse(mat3(u_inst[gl_InstanceIndex].model)));
    vec3 worldNormal = normalize(normalMatrix * inNormal);

    // Banda perto do displacement híbrido (docs/displacement_design.md):
    // desloca o vértice ao longo da normal pela altura do MRAH-W (alpha;
    // 255 = sem dado). Fade POR VÉRTICE (uvTranslateRot.w = alcance da
    // banda): em malhas gigantes (o chão inteiro) só os vértices perto da
    // câmera deslocam, e o deslocamento zera na fronteira - a malha passa a
    // coincidir com o caminho POM sem costura. Mip 3 fixo: altura suave
    // (anti-aliasing do relevo) e VTF barato.
    float geoDist = u_inst[gl_InstanceIndex].uvTranslateRot.w;
    if (u_inst[gl_InstanceIndex].uvScaleDisp.z > 1e-5 && geoDist > 1e-3 && inPbrIndex != 0u) {
        float dCam = distance(worldPos.xyz, u_cameraPos);
        float fadeStart = geoDist * 0.75;
        float t = 1.0 - clamp((dCam - fadeStart) / (geoDist - fadeStart), 0.0, 1.0);
        if (t > 0.0) {
            float h = textureLod(u_textures[nonuniformEXT(inPbrIndex)], animUv, 3.0).a;
            if (h < 0.995) {
                worldPos.xyz += worldNormal * ((h - 0.5) * u_inst[gl_InstanceIndex].uvScaleDisp.z * t);
            }
        }
    }

    // NAO REINTRODUZIR o scroll de UV por inEmissiveStrength (a "lava
    // escorrendo"): o autor removeu deliberadamente porque fazia a textura do
    // vulcao DESLIZAR. Eu ja' restaurei isso uma vez por engano, achando que
    // era perda acidental de working tree - nao era. Se voltar, some de novo.

    // DESLOCAMENTO IGUAL AO DA TESSELACAO, nos vertices originais.
    // Sem isto, o vertice compartilhado entre um triangulo tesselado e um
    // vizinho que nao foi tesselado ia parar em DOIS lugares diferentes - e a
    // fenda entre eles deixava ver o fundo (a "regiao toda branca" que o autor
    // viu, 2026-09-06; por isso sumia quando ele punha a curva no maximo, que
    // e' quando TODO mundo passa a ser tesselado). Mesma curva, mesma fonte de
    // altura, mesma conta: as pontas coincidem e a tesselacao so' acrescenta
    // detalhe ENTRE elas.
    if (u_tessParams.y > 0.5 && u_tessParams.z > 1e-5 && push.uvScale.w > 1e-4) {
        float dTess = distance(worldPos.xyz, u_cameraPos);
        float fTess = lut8(u_tessLut0, u_tessLut1, dTess / max(u_tessParams.x, 1.0));
        float ampTess = clamp((fTess - 1.0) / max(u_tessParams.w - 1.0, 1e-3), 0.0, 1.0);
        if (ampTess > 0.0) {
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
            float hT = -1.0;
            float hSrc = 0.0; // 1 = MRAH-W (altura cozida), 2 = albedo
            float gainH = max(u_normalDistParams.w, 0.1);
            bool worldSpace = u_tessParams2.x > 0.5;
            float wsc = max(u_tessParams2.y, 1e-4);
            if (inPbrIndex != 0u) {
                hT = worldSpace ? heightTriplanar(inPbrIndex, worldPos.xyz, worldNormal, wsc, gainH, true)
                                   : heightUv(inPbrIndex, animUv, gainH, true);
                hSrc = 1.0;
            } else if (inTexIndex != 0u) {
                hT = worldSpace ? heightTriplanar(inTexIndex, worldPos.xyz, worldNormal, wsc, gainH, false)
                                   : heightUv(inTexIndex, animUv, gainH, false);
                hSrc = 2.0;
            }
            if (hT >= 0.0) {
                vec3 dsp = worldNormal * ((hT - 0.5) * u_tessParams.z * ampTess * push.uvScale.w);
                worldPos.xyz += dsp;
                float dl = length(dsp);
                outDispInfo = vec3(dl > 1e-6 ? abs(dot(dsp / dl, worldNormal)) : 1.0, dl, hSrc);
            }
        }
    }

    gl_Position = push.viewProjection * worldPos;
    outWorldPos = worldPos.xyz;
    // Sem esta atribuição o varying vai para o fragment shader com valor
    // INDEFINIDO: model.frag faz `if (inEmissiveStrength > 0.01)` e acendia
    // lava laranja em vértice aleatório de qualquer malha. O atributo é 0 em
    // toda malha que não declara _EMISSIVE_STRENGTH, então repassar é inerte
    // para todo asset antigo.
    outEmissiveStrength = inEmissiveStrength;
    outSplatTex = inSplatTex;
    outSplatTex2 = inSplatTex2;
    outBlendMaskIndex2 = inBlendMaskIndex2;
    outTexCoord = animUv;

    outNormal = worldNormal;
    
    outTexIndex = inTexIndex;
    outMatId = inMatId;
    outPbrIndex = inPbrIndex;
    outNormalIndex = inNormalIndex;
    outBlendTexIndex = inBlendTexIndex;
    outBlendPbrIndex = inBlendPbrIndex;
    outBlendNormalIndex = inBlendNormalIndex;
    outBlendWeight = inBlendWeight;
    outBlendMaskIndex = inBlendMaskIndex;
    outBlendMaskUV = inBlendMaskUV;
    outColor = vec4(
        float(inColor & 0xFF) / 255.0,
        float((inColor >> 8) & 0xFF) / 255.0,
        float((inColor >> 16) & 0xFF) / 255.0,
        float((inColor >> 24) & 0xFF) / 255.0
    );
}
