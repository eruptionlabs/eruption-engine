#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;
layout(set = 0, binding = 1) uniform sampler2D depthTexture;
layout(set = 0, binding = 2) uniform sampler2D bloomTexture;
layout(set = 0, binding = 3) uniform sampler2D cloudColorTexture;
layout(set = 0, binding = 5) uniform sampler2D cloudDepthTexture;
// Independent cloud field's half-res premultiplied accumulation (rgb, alpha).
// It draws forward into hdrTexture with no depth write, so depthTexture below
// still reads whatever is behind the cloud (usually the ground) - sampling
// its alpha here lets the fog block know a cloud already owns this pixel.
layout(set = 0, binding = 8) uniform sampler2D cloudVolumeCoverageTexture;
// Visao de equipe: R8 da janela no plano XZ (1 = visivel). So' amostrada com
// push.vision.w >= 0.
layout(set = 0, binding = 9) uniform sampler2D visionTexture;
// pbr do G-buffer (resolucao de render, NEAREST): r = 0,75 marca o pixel de
// um sprite (sprite.frag). O borrao de movimento de camera poupa o sprite.
layout(set = 0, binding = 10) uniform sampler2D spriteMarkTexture;

// DoF fundido (spec 52, OE-02): borrao pequeno, CoC (alfa) e borrao grande
// pre-multiplicado. Lidos so' com push.gradingParams.w > 0 (o teto do DoF);
// a mistura e' a mesma conta do coc_composite.frag, feita aqui em vez de
// gravar e reler uma imagem de tela cheia.
layout(set = 0, binding = 11) uniform sampler2D dofBlurTexture;
layout(set = 0, binding = 12) uniform sampler2D dofCocTexture;
layout(set = 0, binding = 13) uniform sampler2D dofFarTexture;

bool spritePixel(vec2 uv) { return abs(texture(spriteMarkTexture, uv).r - 0.75) < 0.06; }

// Bloco no set 1 (PushRing, UBO de offset dinamico): passa de 128 B.
layout(std140, set = 1, binding = 0) uniform PushConstants {
    vec4 exposureBloom; // x=exposure, y=fogOpacity, z=bloomIntensity, w=enableBloom
    vec4 fogParams; // xyz=color, w=fogEnd
    // Era `vec4 screenSize`: declarado aqui e NUNCA lido pelo shader - 16 B de
    // carga morta num push constant que ja' esta' EXATAMENTE no teto de 256 B
    // (8 vec4 + 2 mat4). Reaproveitado sem crescer nada.
    // xyz = direcao PARA o sol (normalizada), w = anisotropia g de
    // Henyey-Greenstein (0 = isotropico, ->1 = espalhamento pra frente).
    vec4 sunFogParams;
    vec4 cameraPos; // xyz=pos, w=fogStart
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 motionBlurParams; // x=enable, y=amount, z=tonemap, w=raio limpo (0 = sem)
    vec4 chromaticAberrationParams; // x=enable, y=amount, z=transicao do raio limpo, w=debug
    vec4 fogHeightParams; // x=fogHeight, y=fogHeightFalloff, z=enableHeightFog, w=unused
    vec4 gradingParams;   // x=saturacao, y=contraste, z=G-buffer cru, w=teto do DoF fundido
    // Do look (LookConfig): vinheta e split-toning. Bloco do PushRing de 320 B.
    vec4 vignetteParams;  // x=forca (0 desliga), y=suavidade
    vec4 shadowTint;      // rgb=matiz (normalizado pela luminancia), a=forca
    vec4 highlightTint;   // idem, nas luzes
    vec4 sceneDim;        // rgb=cor, a=forca: escurece so' o cenario (fx.dim), antes da UI
    vec4 belowHorizon;    // rgb=fundo abaixo do horizonte (antes da exposicao), a=liga
    vec4 vision;          // xy=canto xz da janela, z=1/lado, w=dim (< 0 = desligada)
    // BORDA-VAZIO (world_border do .env, ou world.set_border): fora do
    // retangulo [x0,z0]-[x1,z1] do mapa a cor funde para borderParams.yzw ao
    // longo de borderParams.x unidades. borderParams.x < 0 = desligada.
    vec4 borderRect;      // x0,z0,x1,z1 em unidades do mundo
    vec4 borderParams;    // x=distancia de fusao, yzw=cor (linear)
    // Contorno fino por borda de profundidade (preset `outline`). Bloco do PushRing de 432 B.
    vec4 outlineParams;   // x=modo (0 desliga, 1 lite 4 taps, 2 completo 8 taps), y=espessura em px, z=near, w=far
    vec4 outlineColor;    // rgb=cor (ja' em gamma de tela), a=opacidade maxima
    vec4 outlineParams2;  // x=limiar relativo de profundidade, y=1 inclui sprites
} push;

// Cor da cena em `uv`, com o DoF misturado quando ele vem fundido.
vec3 sceneColor(vec2 uv) {
    vec3 orig = texture(hdrTexture, uv).rgb;
    float maxBlur = push.gradingParams.w;
    if (maxBlur <= 0.0) return orig;
    vec3 blur = texture(dofBlurTexture, uv).rgb;
    float cocA = texture(dofCocTexture, uv).a;
    float smallCap = min(maxBlur, 3.0);
    float blend = clamp(cocA / smallCap, 0.0, 1.0);
    if (maxBlur > 3.0) {
        vec4 far = texture(dofFarTexture, uv);
        if (far.a > 1e-4) blur = mix(blur, far.rgb / far.a, smoothstep(smallCap, maxBlur, cocA));
    }
    return mix(orig, blur, blend);
}

// ---------------------------------------------------------------------------
// Color grading artistico
// ---------------------------------------------------------------------------
// O motor NAO TINHA grading. O que existia aqui era so' o operador de tonemap
// (AgX/ACES/Neutral), que e' outra coisa: tonemap mapeia alcance dinamico de
// cena para o do monitor, grading e' escolha estetica. Sem ele, os campos
// `saturation` e `contrast` do LookConfig eram lidos do disco e jogados fora -
// e o preset "diorama" nao tinha como existir de fato, porque cor saturada e
// contraste duro sao metade do que faz uma cena ler como maquete pintada.
//
// Aplicado DEPOIS do tonemap e ANTES do gamma, que e' a ordem de um LUT de
// filme: o tonemap ja' resolveu o alcance, o grading ajusta o gosto sobre o
// resultado, e o gamma so' codifica para o monitor. Fazer antes do tonemap
// deixaria o operador desfazer parte do ajuste.
// SPLIT-TONING: sombra puxada para um matiz e luz para outro ("sombra
// azulada, luz ambar"), a mistura de tons dos coloristas de cinema (Selan,
// "Cinematic Color: From Your Monitor to the Big Screen", SIGGRAPH 2012,
// cap. de grading). O matiz entra normalizado pela luminancia e o resultado
// volta para a luminancia de entrada: o tom muda a cor, nunca o brilho.
vec3 applySplitToning(vec3 c, vec4 shadowTint, vec4 highlightTint) {
    if (shadowTint.a <= 0.0 && highlightTint.a <= 0.0) return c;
    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
    const float l = dot(c, kLuma);
    const float t = smoothstep(0.0, 1.0, clamp(l, 0.0, 1.0));
    const vec3 sh = shadowTint.rgb / max(dot(shadowTint.rgb, kLuma), 1e-3);
    const vec3 hi = highlightTint.rgb / max(dot(highlightTint.rgb, kLuma), 1e-3);
    vec3 r = c * mix(vec3(1.0), sh, clamp(shadowTint.a, 0.0, 1.0) * (1.0 - t));
    r *= mix(vec3(1.0), hi, clamp(highlightTint.a, 0.0, 1.0) * t);
    return r * (l / max(dot(r, kLuma), 1e-5));
}

// VINHETA: escurece para a borda, com distancia corrigida pelo aspecto (o
// circulo nao vira elipse em 16:9) e normalizada pela meia-diagonal. A queda
// comeca em (1 - suavidade) e chega a `forca` no canto.
float vignetteFactor(vec2 uv, vec2 size, vec4 p) {
    if (p.x <= 0.0) return 1.0;
    const vec2 a = vec2(size.x / max(size.y, 1.0), 1.0);
    const float d = length((uv - 0.5) * a) / length(0.5 * a);
    return 1.0 - clamp(p.x, 0.0, 1.0) * smoothstep(1.0 - clamp(p.y, 0.01, 1.0), 1.0, d);
}

vec3 applyGrading(vec3 c, float saturation, float contrast) {
    // Luminancia Rec.709: satura em torno do CINZA PERCEPTUAL, nao da media dos
    // canais. Usar a media faz cor saturada escurecer ao dessaturar.
    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
    float luma = dot(c, kLuma);
    c = mix(vec3(luma), c, saturation);

    // Contraste em torno do cinza medio 0.18 em espaco linear-ish. Pivotar em
    // 0.5 escureceria a imagem inteira ao aumentar o contraste, porque 0.5 nao
    // e' o meio perceptual de uma cena.
    // Em POTENCIA (espaco log), nao linear: (c - 0,18) * 1,22 + 0,18 zerava
    // tudo abaixo de 0,032 e a noite virava preto chapado (sombra "clipada").
    // pivo * (c / pivo)^contraste mantem o pivo, abre os claros e escurece os
    // escuros sem nunca cortar.
    const float kPivot = 0.18;
    c = max(c, vec3(0.0));
    c = kPivot * pow(c / kPivot, vec3(contrast));

    return c;
}

float fogHeightFactor(float y) {
    if (push.fogHeightParams.z < 0.5) return 1.0;
    float h = max(0.0, y - push.fogHeightParams.x);
    return exp(-h * push.fogHeightParams.y);
}

float fogIntegral(vec3 a, vec3 b) {
    float d = length(b - a);
    float fa = fogHeightFactor(a.y);
    float fb = fogHeightFactor(b.y);
    return (fa + fb) * 0.5 * d;
}


// ============================ TONE MAPPING ============================
// A engine NAO tinha operador nenhum: o pipeline terminava em pow(color,1/2.2)
// cru. Sem curva, tudo acima de 1.0 CORTA por canal - e cortar por canal
// desloca a MATIZ: lava laranja satura o vermelho primeiro e vira branco
// amarelado, ceu estourado vira ciano. E' a assinatura visual classica de
// "parece CG" e nao se resolve com nenhuma quantidade de iluminacao.
//
// motionBlurParams.z seleciona: 0=legado (so' gamma), 1=AgX, 2=ACES, 3=Neutral.

// ---- AgX (Troy Sobotka; a curva que o Blender 4.x adotou) ----
// Preserva matiz em altas luzes dessaturando na direcao do branco em vez de
// cortar por canal. E' a razao de highlight de AgX "parecer foto".
const mat3 kAgxIn = mat3(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
const mat3 kAgxOut = mat3(
     1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417,   -0.0980434501171241,
    -0.0990297440797205,-0.0989611768448433,  1.15107367264116);

vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
         - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 toneAgx(vec3 c) {
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    c = kAgxIn * max(c, vec3(0.0));
    c = clamp(log2(max(c, vec3(1e-10))), minEv, maxEv);
    c = (c - minEv) / (maxEv - minEv);
    c = agxContrast(c);
    c = kAgxOut * c;
    // devolve LINEAR: o pow(1/2.2) no fim do main faz a codificacao.
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

// ---- ACES ajustado (Narkowicz 2015) ----
vec3 toneAces(vec3 c) {
    const float a = 2.51, b = 0.03, cc = 2.43, d = 0.59, e = 0.14;
    return clamp((c * (a * c + b)) / (c * (cc * c + d) + e), 0.0, 1.0);
}

// ---- Khronos PBR Neutral ----
// Nao comprime nada abaixo do joelho, entao a cor do material sai EXATA ate'
// ali - util quando o alvo e' fidelidade de albedo, nao look cinematografico.
vec3 toneNeutral(vec3 c) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d2 = 1.0 - startCompression;
    float newPeak = 1.0 - d2 * d2 / (peak + d2 - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, vec3(newPeak), g);
}

vec3 applyToneMapping(vec3 c, float mode) {
    int m = int(mode + 0.5);
    if (m == 1) return toneAgx(c);
    if (m == 2) return toneAces(c);
    if (m == 3) return toneNeutral(c);
    return c; // legado
}

// Contorno por borda de profundidade, sem geometria extra. O teste e' o laplaciano de 1/z (no espaco da tela
// 1/z e' AFIM num plano, entao um plano visto de raspao da' laplaciano exatamente 0 e nao vira linha), relativo
// a 1/z do centro (limiar proporcional a' distancia: espessura aparente igual perto e longe). Em funcao da
// profundidade d do buffer, 1/z ~ f - d (f - n): o laplaciano sai de (da + db - 2 dc), sem linearizar cada tap
// (so' uma divisao por pixel; medido: tirar as 6 divisoes dos taps poupou parte do custo). So' o pixel MAIS
// PERTO da descontinuidade e' pintado (linha de 1 px dentro da silhueta; o ceu, o mais longe, nunca). A
// cobertura sai de um smoothstep (nao binaria) para a borda nao serrilhar nem rastejar com o jitter do FSR3.
// Laplaciano relativo de um eixo: vizinhos mais longe que o centro => positivo.
float outlineAxis(ivec2 c, ivec2 o, float dc, float k) {
    return k * (texelFetch(depthTexture, c + o, 0).r + texelFetch(depthTexture, c - o, 0).r - 2.0 * dc);
}
bool spriteAt(vec2 uv) { return abs(textureLod(spriteMarkTexture, uv, 0.0).r - 0.75) < 0.06; }
float outlineAt(vec2 uv, float depth) {
    if (push.outlineParams.x < 0.5 || depth >= 0.99999) return 0.0;
    // texelFetch com passo inteiro em px: sem filtragem nem conta de uv por tap (a espessura vira inteira >= 1).
    ivec2 sz = textureSize(depthTexture, 0);
    ivec2 c = clamp(ivec2(uv * vec2(sz)), ivec2(1), sz - ivec2(2));
    int w = max(1, int(push.outlineParams.y + 0.5));
    float n = push.outlineParams.z, f = push.outlineParams.w;
    float k = (f - n) / max(f - depth * (f - n), 1e-6);
    float lap = max(outlineAxis(c, ivec2(w, 0), depth, k), outlineAxis(c, ivec2(0, w), depth, k));
    // Modo completo: as duas diagonais (linha mais cheia nas quinas).
    if (push.outlineParams.x > 1.5) {
        lap = max(lap, outlineAxis(c, ivec2(w, w), depth, k));
        lap = max(lap, outlineAxis(c, ivec2(w, -w), depth, k));
    }
    float t = push.outlineParams2.x;
    float e = smoothstep(t, 2.0 * t, lap);
    // Sprites fora (padrao): so' a marca do PROPRIO pixel, lida SO' onde ha' borda. Um vizinho sprite mais perto deixa
    // o laplaciano negativo (sem linha no chao); um sprite mais longe atras de uma arvore e' borda de cenario, que
    // deve ter linha. Medido na 4060 high 1080p: a marca vem do G-buffer PBR e ler em todo pixel custa +0,46 ms,
    // so' nas bordas +0,28 ms; com sprites incluidos (outline_sprites=1) nao le' e custa ~0.
    if (e > 0.0 && push.outlineParams2.y < 0.5 && spriteAt(uv)) return 0.0;
    return e;
}

void main() {
    float depth = texture(depthTexture, fragUV).r;
    // O contorno so' precisa da profundidade: os taps saem ja' aqui para a latencia da leitura se esconder atras
    // do resto do shader (lido so' no fim, depois do gamma).
    const float outlineEdge = outlineAt(fragUV, depth);
    
    // Reconstruct world position
    vec4 ndc = vec4(fragUV * 2.0 - 1.0, depth, 1.0);
    vec4 worldPosH = push.invViewProj * ndc;
    worldPosH.w = max(worldPosH.w, 1e-6);
    vec3 worldPos = worldPosH.xyz / worldPosH.w;

    vec3 color;

    // VISUALIZACAO CRUA DO G-BUFFER (gradingParams.z, ligado pelo checkbox
    // "Show Normal (RGB)" e irmaos, ou por ERUPTION_TEST_PBR_DEBUG). O passe
    // ambiente ja' escreveu o canal pedido no buffer lit; tudo o que este
    // shader faz depois - exposicao, bloom, FOG (cor e densidade mudam com a
    // hora do dia e tingem o longe), tonemap, grading, vinheta - mudaria a
    // leitura conforme a cena. Sai direto e sem gamma: o pixel na tela e'
    // exatamente o texel do G-buffer.
    if (push.gradingParams.z > 0.5) {
        outColor = vec4(sceneColor(fragUV), 1.0);
        return;
    }

    // RAIO LIMPO: borrao de movimento e aberracao so' FORA de um circulo em
    // volta do centro da tela, onde a camera poe o assunto. Dentro dele a
    // imagem fica nitida; da borda para fora o efeito cresce suave. Raio e
    // transicao em fracao da altura da tela (o circulo e' redondo em
    // qualquer proporcao de tela). Raio 0 = mascara desligada.
    float efeito = 1.0;
    if (push.motionBlurParams.w > 0.0) {
        vec2 tam = vec2(textureSize(hdrTexture, 0));
        vec2 d = (fragUV - 0.5) * vec2(tam.x / tam.y, 1.0);
        efeito = smoothstep(push.motionBlurParams.w,
                            push.motionBlurParams.w + max(push.chromaticAberrationParams.z, 1e-3),
                            length(d));
    }

    // Adaptive Chromatic Aberration
    if (push.chromaticAberrationParams.x > 0.5 && efeito > 0.0) {
        vec2 centerDist = fragUV - 0.5;
        float distSq = dot(centerDist, centerDist); // Mais forte nas bordas
        
        float amount = push.chromaticAberrationParams.y * distSq * 0.1 * efeito;
        
        float r = sceneColor(fragUV + centerDist * amount).r;
        float g = sceneColor(fragUV).g;
        float b = sceneColor(fragUV - centerDist * amount).b;
        color = vec3(r, g, b);
    } else {
        color = sceneColor(fragUV);
    }

    // Camera Motion Blur
    // Sprite fora do borrao: a camera segue o heroi, que fica parado na tela,
    // mas o vetor de camera (do depth) o arrastava junto com o fundo - o
    // boneco andando saia dobrado, com uma copia fantasma atras.
    if (push.motionBlurParams.x > 0.5 && depth < 1.0 && efeito > 0.0 && !spritePixel(fragUV)) {
        vec4 prevNdc = push.prevViewProj * vec4(worldPos, 1.0);
        prevNdc /= prevNdc.w;
        
        vec2 prevUV = prevNdc.xy * 0.5 + 0.5;
        vec2 velocity = (fragUV - prevUV) * push.motionBlurParams.y * efeito;
        
        // Limit velocity to prevent huge streaks
        float maxVel = 0.05;
        float velLen = length(velocity);
        if (velLen > maxVel) {
            velocity = (velocity / velLen) * maxVel;
        }

        const int samples = 8;
        // Amostra que cai num sprite vale a cor do centro: o boneco nao escorre
        // para o fundo em volta dele.
        const vec3 centro = color;
        for (int i = 1; i < samples; ++i) {
            vec2 offsetUV = fragUV + velocity * (float(i) / float(samples - 1) - 0.5);
            color += spritePixel(offsetUV) ? centro : sceneColor(offsetUV);
        }
        color /= float(samples);
    }

    // Exposure
    color *= push.exposureBloom.x;

    // Bloom
    if (push.exposureBloom.w > 0.5) {
        vec3 bloom = texture(bloomTexture, fragUV).rgb;
        color += bloom * push.exposureBloom.z;
    }



    float fogStart = push.cameraPos.w;
    float fogEnd = push.fogParams.w;
    vec3 camPos = push.cameraPos.xyz;
    if (fogEnd > 0.0 && depth < 0.99999) {
        float fogFactor;
        if (push.fogHeightParams.z > 0.5 && push.fogHeightParams.y > 0.0) {
            float opticalDepth = fogIntegral(camPos, worldPos) / max(fogEnd - fogStart, 1.0);
            fogFactor = 1.0 - exp(-opticalDepth * push.exposureBloom.y * 5.0);
        } else {
            fogFactor = clamp((distance(worldPos, camPos) - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
            fogFactor *= push.exposureBloom.y;
        }
        // The independent cloud field already blended into `color` above,
        // with no depth write, so worldPos/distance here is the GROUND far
        // behind it. Where the cloud already covers this pixel, its own
        // shading IS the atmosphere in front of the camera - suppress the
        // ground fog proportionally instead of washing the cloud out toward
        // flat fog colour on top of it.
        float cloudCoverage = texture(cloudVolumeCoverageTexture, fragUV).a;
        fogFactor *= (1.0 - cloudCoverage);

        fogFactor = clamp(fogFactor, 0.0, 1.0);

        // PERSPECTIVA AEREA (dessaturacao com a distancia). O fog classico so'
        // mistura para uma COR - mas atmosfera de verdade primeiro LAVA o
        // croma (espalhamento tira saturacao antes de tirar luminancia; e' o
        // que separa foto de render 2003). Rampa mais LONGA e mais SUAVE que o
        // fog (comeca junto, satura na metade do caminho do fog), entao o
        // longe fica cinzento-azulado ANTES de sumir na cor do fog. Usa a
        // propria cor do fog como tint de ceu - zero uniform novo. Gate pelo
        // mesmo cloudCoverage (nuvem ja' e' atmosfera na frente da camera).
        {
            float aer = clamp((distance(worldPos, camPos) - fogStart * 0.5) /
                              max(fogEnd - fogStart * 0.5, 1.0), 0.0, 1.0);
            aer = aer * aer * (1.0 - cloudCoverage);
            float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
            // 55% de dessaturacao no limite + leve tint do ceu (12%).
            color = mix(color, vec3(lum), aer * 0.55);
            color = mix(color, push.fogParams.xyz * lum * 1.15, aer * 0.12);
        }

        // ERUPTION_TEST_FOG_DEBUG=1: paint pure magenta scaled by fogFactor
        // instead of blending, so a Python analysis of the raw pixel value
        // reads the fog factor directly (no exposure/tonemap contamination).
        if (push.fogHeightParams.w > 0.5) {
            outColor = vec4(fogFactor, 0.0, fogFactor, 1.0);
            return;
        }

        // NEVOA COM DEPENDENCIA ANGULAR (Henyey-Greenstein).
        //
        // Ate' aqui a nevoa olhando PARA o sol era identica a nevoa de COSTAS
        // pra ele: a cor era uma constante do config. E' essa simetria - nao a
        // quantidade de nevoa - que faz atmosfera ler como "fog de videogame".
        // Aerossol real espalha para FRENTE: olhando na direcao do sol, a
        // mesma nevoa fica muito mais clara e puxa a cor da luz.
        //
        // A fase de HG e' normalizada pelo seu proprio valor a 90 graus, entao
        // g=0 devolve exatamente a cor antiga (o caminho sem nevoa angular
        // continua sendo o de antes, bit a bit) e g>0 so' redistribui: clareia
        // contra o sol e escurece de costas, sem inventar energia no total.
        vec3 finalFogColor = push.fogParams.xyz * push.exposureBloom.x;
        float g = clamp(push.sunFogParams.w, 0.0, 0.9);
        if (g > 0.001) {
            vec3 viewDirW = normalize(worldPos - camPos);
            float cosT = dot(viewDirW, normalize(push.sunFogParams.xyz));
            float g2 = g * g;
            float phase  = (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cosT, 1.5);
            float phase90 = (1.0 - g2) / pow(1.0 + g2, 1.5);
            finalFogColor *= phase / max(phase90, 1e-4);
        }
        color = mix(color, finalFogColor, fogFactor);
    }

    // FUNDO ABAIXO DO HORIZONTE ("background" do .env): sem geometria e com o
    // raio descendo, o ceu (claro e HDR) virava um vao na borda do mundo. Sai
    // na cor do fundo, com a mesma exposicao da nevoa para emendar no longe.
    if (push.belowHorizon.a > 0.5 && depth >= 0.99999) {
        float rayY = normalize(worldPos - camPos).y;
        color = mix(color, push.belowHorizon.rgb * push.exposureBloom.x, smoothstep(0.0, -0.02, rayY));
    }

    // Cloud layer composite (half-res ray-marched result).
    // cloudColorTexture stores (luminance.rgb, cloud-opacity) and
    // cloudDepthTexture stores the first-hit distance normalized by 10000.
    vec4 cloudColor = texture(cloudColorTexture, fragUV);
    if (cloudColor.a > 0.001) {
        float cloudDepth = texture(cloudDepthTexture, fragUV).r * 10000.0;
        vec3 camPos = push.cameraPos.xyz;
        float sceneDist = distance(worldPos, camPos);

        // Occlude the cloud ONLY where it sits behind genuinely-near solid
        // geometry. Anything sky-ish (depth ~1) never occludes, so a grazing
        // cloud layer stays visible above the horizon regardless of camera
        // pitch (a strict cloudDepth < sceneDist test made the whole layer pop
        // in and out as the pitch crossed the mountain line). Where solid geo
        // is in front, feather the cloud out over a 200 m band instead of a
        // hard binary cut (which stair-stepped along the half-res depth edge).
        float vis = 1.0;
        if (depth < 0.99999 && cloudDepth > sceneDist + 50.0) {
            vis = clamp((sceneDist + 250.0 - cloudDepth) / 200.0, 0.0, 1.0);
        }
        float cloudFog = 0.0;
        if (vis > 0.0) {
            // Gentle distance haze only via the physically-based height-fog path
            // (and capped), so far clouds settle into the atmosphere instead of
            // being wiped to a flat fog colour.
            float fogStart = push.cameraPos.w;
            float fogEnd = push.fogParams.w;
            if (fogEnd > 0.0 && push.fogHeightParams.z > 0.5 && push.fogHeightParams.y > 0.0) {
                vec3 cloudPos = camPos + normalize(worldPos - camPos) * cloudDepth;
                float opticalDepth = fogIntegral(camPos, cloudPos) / max(fogEnd - fogStart, 1.0);
                cloudFog = clamp((1.0 - exp(-opticalDepth * push.exposureBloom.y * 5.0)) * 0.6, 0.0, 1.0);
            }
            vec3 finalFogColor = push.fogParams.xyz * push.exposureBloom.x;
            // cloudColor is premultiplied; fade the emitted light toward the fog.
            vec3 cloudRgb = mix(cloudColor.rgb, finalFogColor * cloudColor.a, cloudFog);

            float a = cloudColor.a * vis;
            color = color * (1.0 - a) + cloudRgb * vis;
        }

        // ERUPTION_TEST_CLOUD_OCC_DEBUG=1: encode the three independent
        // factors that can make a cloud pixel read as "transparent" so a
        // Python harness can tell them apart instead of guessing from one
        // blended colour. R = raw cloudColor.a (the cloud pass's own
        // opacity, before anything here touches it). G = vis (the
        // occlusion/feather factor computed above). B = cloudFog (how much
        // the cloud's colour - not its alpha - gets washed toward the flat
        // fog colour). Pure magenta (R=B high, G=0) at full brightness with
        // B also high is the "fog washing it out" signature; low R alone is
        // "the raymarch itself produced weak alpha here".
        if (push.chromaticAberrationParams.w > 0.5) {
            outColor = vec4(cloudColor.a, vis, cloudFog, 1.0);
            return;
        }
    } else if (push.chromaticAberrationParams.w > 0.5) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Tone mapping ANTES do gamma (a curva opera em linear).
    color = applyToneMapping(max(color, vec3(0.0)), push.motionBlurParams.z);

    // Grading do LookConfig (saturacao/contraste do preset ativo).
    color = applyGrading(color, push.gradingParams.x, push.gradingParams.y);
    color = applySplitToning(color, push.shadowTint, push.highlightTint);
    color *= vignetteFactor(fragUV, vec2(textureSize(hdrTexture, 0)), push.vignetteParams);

    // Gamma 2.2
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    // Contorno: depois do tonemap/grading/vinheta (cor de tela), antes da visao e do escurecimento.
    color = mix(color, push.outlineColor.rgb, push.outlineColor.a * outlineEdge);

    // Visao de equipe depois do gamma, como o fx.dim: dim 0,35 e' 35% do
    // brilho que se ve'. O ceu nao escurece; fora da janela e' sem visao.
    if (push.vision.w >= 0.0 && depth < 0.99999) {
        vec2 vuv = (worldPos.xz - push.vision.xy) * push.vision.z;
        float vis = all(greaterThanEqual(vuv, vec2(0.0))) && all(lessThanEqual(vuv, vec2(1.0)))
                    ? texture(visionTexture, vuv).r : 0.0;
        color *= mix(push.vision.w, 1.0, vis);
    }
    // BORDA-VAZIO depois do gamma, como a visao: fora do retangulo do mapa a
    // tela funde para a cor do vazio ao longo de borderParams.x unidades. O
    // ceu nao escurece (mesma regra da visao).
    if (push.borderParams.x >= 0.0 && depth < 0.99999) {
        vec2 p = worldPos.xz;
        vec2 d = max(max(push.borderRect.xy - p, p - push.borderRect.zw), vec2(0.0));
        float dist = length(d);
        float t = clamp(dist / max(push.borderParams.x, 1e-3), 0.0, 1.0);
        vec3 borderColor = pow(max(push.borderParams.yzw, vec3(0.0)), vec3(1.0 / 2.2));
        color = mix(color, borderColor, t);
    }
    // fx.dim depois do gamma: a forca e' a que se ve' (0,7 = 70% mais escuro);
    // antes do gamma 0,7 escurecia so' ~42% na tela.
    color = mix(color, pow(max(push.sceneDim.rgb, vec3(0.0)), vec3(1.0 / 2.2)), clamp(push.sceneDim.a, 0.0, 1.0));
    outColor = vec4(color, 1.0);
}
