#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable

#include "lighting/pbr_common.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outLit;

// G-Buffer samplers
layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 3) uniform sampler2D gbufferPBR;
layout(set = 0, binding = 4) uniform sampler2D gbufferEmissive;
layout(set = 0, binding = 5) uniform sampler2DShadow shadowAtlas;
layout(set = 0, binding = 8) uniform sampler2DShadow nextShadowAtlas;
layout(set = 0, binding = 10) uniform sampler2DShadow cloudShadowMap;
layout(set = 0, binding = 13) uniform sampler2D ssaoTex; // blurred half-res SSAO
// MESMA imagem do binding 5, com sampler NAO comparativo. O binding 5 e'
// sampler2DShadow: devolve o resultado do teste (0/1 filtrado), nunca a
// profundidade. O blocker search do PCSS precisa da profundidade CRUA.
layout(set = 0, binding = 14) uniform sampler2D shadowAtlasDepth;

// Shadow data in UBO
layout(set = 0, binding = 6) uniform ShadowUBO {
    mat4 lightSpaceMats[2];      // [0] = legacy near box, [1] = extended coverage box
    mat4 nextLightSpaceMats[2];
    vec4 cascadeSplits;
    vec4 cascadeScales;
    vec4 shadowParams;   // x=pcfKernelSize, y=poissonTaps, z=usePoisson, w=time
    vec4 shadowRanges;   // x=blendRange, y=depthRange (real ortho near..far), z=bias, w=blendFactor
    // Opacidade da sombra do sol por distancia, como LUT de 8 amostras
    // uniformes em [0, params.x]. params.y = 1 liga / 0 desliga.
    vec4 shadowOpacityParams;
    vec4 shadowOpacityLut0;
    vec4 shadowOpacityLut1;
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

// Lighting UBO
layout(set = 0, binding = 7) uniform LightingUBO {
    vec4 dirLightDir;       // xyz=direção, w=intensidade
    vec4 dirLightColor;     // xyz=cor RGB, w=ambientIntensity
    vec4 pointLights[128];  
    vec4 pointColors[128];  
    uint numPointLights;
    float giIntensity;
    float giAmbientFloor;
    uint usePbr;        // 0 = legacy Blinn-Phong, 1 = Cook-Torrance/GGX
    uint pbrDebugMode;  // bitmask: 1=roughness, 2=metallic, 4=normal, 8=wetness, 16=source
    float rainIntensity; // 0..1 global rain intensity (wettable/water surfaces get wet)
    float snowIntensity; // 0..1 global snow intensity (frost/snow surfaces freeze/accumulate)
    float temperatureC;  // surface temperature in Celsius
    float pbrLightScale; // multiplier for PBR direct-light response (legacy unchanged)
    float sunAngularRadius; // era pad0; raio angular da fonte, em radianos
    uint pad1;
    vec4 ambientSky;
    vec4 ambientGround;
    vec4 ssaoParams;        // x=enabled, y=strength, z=radius, w=unused
    vec4 indirectParams;    // x=envSpecEnabled, y=envSpecIntensity, z=nightAoPow, w=ambientHemiFloor
    mat4 viewProj;
    vec4 bounceParams;      // x=sunBounceEnabled, y=strength
    vec4 contactParams;     // x=contactShadowEnabled, y=lengthMetres
    vec4 skyHorizon;        // xyz=procedural sky horizon colour
    mat4 invViewProj;
    vec4 skyProbeParams;    // x=sonda de ceu ligada, y=mip max, z=intensidade (G36)
} lights;
// SONDA DE CEU (G36): o proprio skybox.frag em cubemap 32x32 com mips -
// especular ambiental pre-filtrado (roughness -> lod).
layout(set = 0, binding = 15) uniform samplerCube skyProbe;

layout(push_constant) uniform PushConstants {
    vec3 lightDir;
    float intensity;
    vec3 lightColor;
    uint shadowMapIndex;
    vec3 cameraPos;
    float cloudShadowIntensity;
    vec4 cloudWorldMin;     // xyz = world min, w = inv size x
    vec4 cloudWorldMax;     // xyz = world max, w = inv size z
    mat4 invViewProj;
    mat4 cloudShadowMatrix;
};

// Posicao de mundo a partir do DEPTH, no lugar do attachment de posicao do
// G-buffer (RGBA16F, 8 B/px) - o unico alvo cujo conteudo ja' estava
// inteiramente contido no depth. Medido: o GBuffer nao e' limitado por
// triangulo (cortar os triangulos pela metade nao mexeu no gpu_total) e sim por
// BANDA: eram 36 B/px = 74,6 MB por passada a 1080p. Sao 22% a menos.
vec3 worldFromDepth(vec2 uv, float d) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, d, 1.0);
    vec4 wp = invViewProj * ndc;   // invViewProj vem do push deste passe
    if (abs(wp.w) < 1e-6) return vec3(0.0);
    return wp.xyz / wp.w;
}


// Poisson disk samples (16 taps)
const vec2 poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2(0.94558609, -0.76890725),
    vec2(-0.094184101, -0.92938870),
    vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845),
    vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507),
    vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790)
);

// Sample one cascade quadrant. Cascades are side by side in the atlas:
// c0 at x in [0, 0.5], c1 at x in [0.5, 1]. texelSize is in full-atlas UV,
// so kernel offsets land on physical texels in either quadrant.
float sampleCascade(sampler2DShadow atlas, vec3 projCoord, int idx, float currentDepth, vec2 texelSize, vec2 dz_duv) {
    vec2 baseUV = vec2(projCoord.x * 0.5 + 0.5 * float(idx), projCoord.y);
    float shadow = 0.0;

    int kernelSize = int(shadowParams.x);
    if (kernelSize < 3) kernelSize = 7; // SAFEGUARD: Prevent uninitialized 0 value at startup
    bool usePoisson = shadowParams.z > 0.5;

    // Guard against NaN/Inf depth or UVs that would corrupt the entire shadow.
    if (isnan(currentDepth) || isinf(currentDepth) ||
        isnan(baseUV.x) || isnan(baseUV.y) ||
        isinf(baseUV.x) || isinf(baseUV.y)) {
        return 0.0;
    }

    // Keep PCF/Poisson taps inside this cascade quadrant to prevent bleeding
    // from the neighbour cascade, which manifests as flickering black rectangles.
    vec2 cascadeMinUV = vec2(0.5 * float(idx), 0.0);
    vec2 cascadeMaxUV = vec2(0.5 * float(idx) + 0.5, 1.0);

    // PCSS — LARGURA DE PENUMBRA POR DISTANCIA DO BLOQUEADOR.
    //
    // O raio do filtro era FIXO (`texelSize * kernelSize`), o que equivale a
    // afirmar "a fonte e' pontual e esta' no infinito" em toda a cena: a sombra
    // tem a mesma moleza encostada no objeto e a 50 metros dele. Essa e' a
    // assinatura de exterior em escala 1:1 - o oposto de foto de maquete, onde
    // a fonte e' grande e proxima e a penumbra ABRE com a distancia.
    //
    // Fisica: para uma fonte de raio angular `a`, a penumbra projetada tem
    // largura ~ 2*tan(a) * (dReceptor - dBloqueador), em unidades de
    // profundidade do mapa. O sol subtende 0,53 grau; um softbox de 60 cm a
    // 40 cm de uma maquete subtende ~37 graus - dai a diferenca de leitura.
    //
    // Passo 1: procurar o bloqueador. Media das amostras MAIS PROXIMAS da luz
    // que o receptor. Sem bloqueador -> totalmente iluminado, sai cedo.
    float penumbraScale = 1.0;
    {
        const int BLOCKER_TAPS = 8;
        float searchRadius = max(tan(lights.sunAngularRadius) * 6.0, texelSize.x * 2.0);
        float blockerSum = 0.0;
        float blockerCount = 0.0;
        for (int i = 0; i < BLOCKER_TAPS; ++i) {
            vec2 sUV = clamp(baseUV + poissonDisk[i] * searchRadius, cascadeMinUV, cascadeMaxUV);
            float d = texture(shadowAtlasDepth, sUV).r;
            if (d < currentDepth) { blockerSum += d; blockerCount += 1.0; }
        }
        if (blockerCount > 0.0) {
            float avgBlocker = blockerSum / blockerCount;
            // Distancia receptor->bloqueador em profundidade normalizada.
            float dz = max(currentDepth - avgBlocker, 0.0);
            // Fator de abertura. O `* 40` traz a escala de profundidade do mapa
            // pra ordem de grandeza do kernel em texels; o teto impede que um
            // bloqueador muito distante vire um borrao que vaza pra fora da
            // cascata (e que custaria taps fora do quadrante).
            penumbraScale = clamp(1.0 + tan(lights.sunAngularRadius) * dz * 40.0, 1.0, 4.0);
        } else {
            // Nenhum bloqueador entre este ponto e a luz: sem sombra pra filtrar.
            return 0.0;
        }
    }

    if (usePoisson) {
        int numTaps = clamp(int(shadowParams.y), 1, 16);
        for (int i = 0; i < numTaps; i++) {
            vec2 offsetUV = poissonDisk[i] * texelSize * float(kernelSize) * penumbraScale;
            vec2 sampleUV = clamp(baseUV + offsetUV, cascadeMinUV, cascadeMaxUV);
            
            // Map full-atlas UV offset back to projCoord offset to apply dz_duv gradient
            vec2 projOffset = vec2((sampleUV.x - baseUV.x) * 2.0, sampleUV.y - baseUV.y);
            float sampleDepth = currentDepth + dot(dz_duv, projOffset);
            
            shadow += 1.0 - texture(atlas, vec3(sampleUV, sampleDepth));
        }
        shadow /= float(numTaps);
    } else {
        int halfKernel = kernelSize / 2;
        float sampleCount = 0.0;
        for (int x = -halfKernel; x <= halfKernel; ++x) {
            for (int y = -halfKernel; y <= halfKernel; ++y) {
                vec2 offsetUV = vec2(float(x), float(y)) * texelSize * penumbraScale;
                vec2 sampleUV = clamp(baseUV + offsetUV, cascadeMinUV, cascadeMaxUV);
                
                vec2 projOffset = vec2((sampleUV.x - baseUV.x) * 2.0, sampleUV.y - baseUV.y);
                float sampleDepth = currentDepth + dot(dz_duv, projOffset);
                
                shadow += 1.0 - texture(atlas, vec3(sampleUV, sampleDepth));
                sampleCount += 1.0;
            }
        }
        shadow /= sampleCount;
    }

    return shadow;
}

// Cascade selection: wherever the legacy c0 box contains the fragment, its
// map is used (near shadows pixel-identical to the single-map behavior).
// Outside it, fall back to the extended c1 box; outside both, fully lit.
float calcSingleShadow(sampler2DShadow atlas, mat4 mats[2], vec3 worldPos, vec3 normal) {
    // Shadow bias must follow the GEOMETRY, not the normal map. Feeding the
    // bump-mapped normal into the normal-offset and the dz_duv compare-plane
    // tilt makes both swim as the normal-map strength or light angle changes -
    // the wall's edges appear to creep. Reconstruct the flat surface normal
    // from the world-position screen derivatives instead.
    vec3 shadeN = normalize(normal * 2.0 - 1.0);
    vec3 geoN = cross(dFdx(worldPos), dFdy(worldPos));
    float geoLen = length(geoN);
    vec3 N = (geoLen > 1e-8) ? geoN / geoLen : shadeN;
    // ORIENTACAO PELA CAMERA, nao pelo normal map. O sinal de cross(dFdx,dFdy)
    // e' arbitrario e precisa ser resolvido; resolver com dot(N, shadeN) fazia
    // a normal GEOMETRICA virar de cabeca pra baixo em toda parede de sulco do
    // normal map (chao "terra roxa": z do mapa baixo, relevo forte) - o
    // offset ia pra DEBAIXO do chao e o pixel se auto-sombreava. Parado nao
    // se ve (cada pixel e' deterministico); com a camera girando o pixel
    // troca de texel do normal map e o sulco liga/desliga: medido com a
    // saida "so' sombra", 26% dos pixels mudavam >0,25 por frame a 0,05
    // grau/frame, contra 5,5% com normal map plano. Uma superficie visivel
    // sempre encara o olho, entao o olho decide o sinal e o normal map sai
    // da conta da sombra de vez.
    if (dot(N, cameraPos - worldPos) < 0.0) N = -N;
    vec3 L = normalize(-lights.dirLightDir.xyz);
    float NdotL = max(dot(N, L), 0.0);

    // BIAS ESCALADO PELA INCLINACAO (slope-scaled depth bias). Antes o fator
    // era 0.0 - so' o bias constante - e o chao inclinado ficava com o teste
    // de profundidade no LIMIAR: qualquer deslocamento sub-texel da camera
    // virava o resultado ("acne" oscilando). Medido no parana, zoom 0, camera
    // quase parada (0,01 grau/frame): ruido extra da sombra sobre chao aberto
    // 2,42 -> 0,41 com bias 20x constante. Constante 20x gera peter-panning
    // em superficie virada pro sol; por isso escala com tan(angulo): 1x onde
    // N.L = 1 e cresce so' nas encostas rasantes. Fator vem de
    // shadows.json ("slope_bias"), teto de 6 em tan pra nao explodir no
    // horizonte.
    float baseBias = shadowRanges.z; // Constant depth bias
    float slopeBiasFactor = shadowParams.w;
    float tanTheta = clamp(sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.1), 0.0, 6.0);
    float bias = baseBias * (1.0 + slopeBiasFactor * tanTheta);

    // Normal-offset ESCALADO PELO TEXEL (Holbert/'normal offset shadows').
    // O valor fixo shadowRanges.x (0.001u) era invisivel: com texel de ~1u
    // sobre terreno com micro-relevo, a altura dentro de um texel varia mais
    // que o offset e o depth-test falha em area aberta ("tudo sombreado" em
    // parana). O offset certo acompanha o texel do mapa (1.5 texel), com o
    // valor de config como piso p/ compatibilidade.
    float texelW0 = max(cascadeScales.x, cascadeScales.y);
    // G40 (autor: "a sombra mexe quando movo o pitch, parece que o sol se
    // moveu"). O texel do mapa e' ADAPTATIVO ao pitch (ShadowRenderer:
    // lowPitchSizeBoost 3x, teto 6 u) e o offset seguia o texel: a consulta
    // subia ate' ~9 u acima do chao e a sombra no chao deslizava para o lado
    // do sol por offset/tan(elevacao) - e o deslize MUDAVA com o pitch. Dois
    // freios, os dois independentes da camera: (1) teto em unidades de mundo;
    // (2) fator de Holbert (normal offset shadows, 2011): o offset e' pra acne
    // rasante, entao escala com sin(angulo N.L) - superficie virada ao sol
    // (chao ao meio-dia) quase nao desloca, o bias de profundidade ja' cobre.
    const float kNormalOffsetMaxWorld = 2.0;
    float nOff = max(shadowRanges.x, min(texelW0 * 1.5, kNormalOffsetMaxWorld));
    nOff *= clamp(sqrt(max(1.0 - NdotL * NdotL, 0.0)), 0.25, 1.0);
    vec3 offsetPos = worldPos + N * nOff;

    vec4 lightSpace = mats[0] * vec4(offsetPos, 1.0);
    vec3 projCoord = lightSpace.xyz / lightSpace.w;
    vec3 projC0 = projCoord;   // guardado para a mistura de borda (ver abaixo)
    int idx = 0;

    if (projCoord.z > 1.0 || projCoord.x < 0.0 || projCoord.x > 1.0 || projCoord.y < 0.0 || projCoord.y > 1.0) {
        lightSpace = mats[1] * vec4(offsetPos, 1.0);
        projCoord = lightSpace.xyz / lightSpace.w;
        idx = 1;
        if (projCoord.z > 1.0 || projCoord.x < 0.0 || projCoord.x > 1.0 || projCoord.y < 0.0 || projCoord.y > 1.0)
            return 0.0;
    }

    vec2 texelSize = 1.0 / vec2(textureSize(atlas, 0));

    // Texel-scaled depth bias: on a flat horizontal receiver, the depth change
    // per texel in light space is texelWorld * (cosElev / sinElev) / depthRange.
    // cosElev / sinElev is the tangent of the angle of incidence.
    float lightY  = max(abs(normalize(lights.dirLightDir.xyz).y), 0.001); // sinElev
    float cosElev = sqrt(max(1.0 - lightY * lightY, 0.0));
    float slope   = cosElev / lightY;
    float texelWorld = (idx == 0) ? cascadeScales.x : cascadeScales.y;
    float realDepthRange = max(shadowRanges.y, 100.0);
    
    // We apply a very small constant bias (0.5 texels) just for numerical stability
    slope = min(slope, 5.0);
    bias += (texelWorld * slope * 0.5) / realDepthRange;

    // Calculate analytical depth gradient (dz/du, dz/dv) in light space
    // to tilt the depth comparison plane exactly parallel to the geometry.
    mat4 M = mats[idx];
    // Robust inverse: if the matrix is singular (e.g., near-degenerate
    // projections), inverse() can produce NaN/Inf and create black pixels.
    mat4 invM = inverse(M);
    vec3 N_L = (transpose(invM) * vec4(N, 0.0)).xyz;
    if (any(isinf(N_L)) || any(isnan(N_L))) {
        // Fallback to a small constant bias when the light-space matrix is bad.
        return sampleCascade(atlas, projCoord, idx, projCoord.z - bias, texelSize, vec2(0.0));
    }
    float N_L_z = sign(N_L.z) * max(abs(N_L.z), 1e-8);
    vec2 dz_duv = -N_L.xy / N_L_z;
    // Clamp de seguranca ESCALADO PELA CAIXA. O antigo +-0.1 fixo assumia a
    // caixa pequena do c0 (~500u): com o mapa unico de milhares de unidades,
    // um declive de 45 graus precisa de dz/duv ~ box/depthRange >> 0.1, e o
    // truncamento desligava a compensacao de declive - acne em bandas nas
    // encostas (as "linhas retas pretas" do autor). Limite = declive maximo
    // fisico (tan=5, o mesmo cap do slope-bias) sobre a caixa real.
    float boxWorld = texelWorld * float(textureSize(atlas, 0).y); // slot quadrado
    float dzMax = max(boxWorld * 5.0 / realDepthRange, 0.1);
    dz_duv = clamp(dz_duv, vec2(-dzMax), vec2(dzMax));
    // Final NaN guard before the shadow kernel.
    if (any(isnan(dz_duv)) || any(isinf(dz_duv))) dz_duv = vec2(0.0);

    float sh = sampleCascade(atlas, projCoord, idx, projCoord.z - bias, texelSize, dz_duv);

    // MISTURA ENTRE CASCATAS na borda da c0. A escolha de cascata acima e' um
    // DEGRAU SECO: o fragmento que sai da caixa da c0 cai na c1, que tem
    // texel varias vezes maior. Na pratica isso desenha uma COSTURA em volta
    // do jogador - de um lado sombra fina, do outro sombra grossa - e a
    // costura varre a tela conforme a camera anda. Foi o que o autor relatou
    // em 2026-09-03 ("a sombra ta com um culling fodido quando eu movo a
    // camera, ta bem visivel"): nao e' culling, e' a troca de cascata sem
    // transicao.
    // Agora as duas cascatas sao amostradas numa faixa estreita perto da
    // borda e misturadas, entao a mudanca de resolucao acontece ao longo de
    // uma banda em vez de numa linha. So' os pixels DENTRO da faixa pagam a
    // segunda amostragem (a esmagadora maioria da tela nao entra no if).
    if (idx == 0) {
        float borda = max(abs(projC0.x - 0.5), abs(projC0.y - 0.5)) * 2.0;
        float t = smoothstep(0.86, 0.99, borda);
        if (t > 0.001) {
            vec4 ls1 = mats[1] * vec4(offsetPos, 1.0);
            vec3 pc1 = ls1.xyz / ls1.w;
            if (pc1.z <= 1.0 && pc1.x >= 0.0 && pc1.x <= 1.0 && pc1.y >= 0.0 && pc1.y <= 1.0) {
                float sh1 = sampleCascade(atlas, pc1, 1, pc1.z - bias, texelSize, dz_duv);
                sh = mix(sh, sh1, t);
            }
        }
    }
    // Fade de borda SO' no modo single (cascadeSplits.z = 1, setado pela
    // CPU): la' a caixa e' capada por orcamento de texel e alem dela nao ha'
    // NADA - sem fade a fronteira vira linha reta acompanhando o jogador.
    // No csm/cascade o fade seria um ERRO: fora da c0 o caminho existente
    // cai na c1 (handoff limpo), e o fade abriria uma banda clara na costura.
    if (idx == 0 && cascadeSplits.z > 0.5) {
        float edge = max(abs(projCoord.x - 0.5), abs(projCoord.y - 0.5)) * 2.0;
        sh *= 1.0 - smoothstep(0.88, 1.0, edge);
    }
    return sh;
}

float calcShadow(vec3 worldPos, vec3 normal) {
    if (cascadeScales.w > 0.5) return 0.0; // ERUPTION_TEST_NO_SHADOW=1 (debug)
    float shadow1 = calcSingleShadow(shadowAtlas, lightSpaceMats, worldPos, normal);
    
    // Blend factor is used only for legacy temporal blend (now deprecated).
    // With Positional Snap, blend is always 0.0 and only shadowAtlas is sampled.
    if (shadowRanges.w <= 0.001) return shadow1;
    if (shadowRanges.w >= 0.999) {
        return calcSingleShadow(nextShadowAtlas, nextLightSpaceMats, worldPos, normal);
    }
    
    float shadow2 = calcSingleShadow(nextShadowAtlas, nextLightSpaceMats, worldPos, normal);
    return mix(shadow1, shadow2, shadowRanges.w);
}

float calcCloudShadow(vec3 worldPos) {
    if (cloudShadowIntensity <= 0.001) return 0.0;

    vec4 lightSpace = cloudShadowMatrix * vec4(worldPos, 1.0);
    vec3 projCoord = lightSpace.xyz / lightSpace.w;
    if (projCoord.z > 1.0 || projCoord.x < 0.0 || projCoord.x > 1.0 ||
        projCoord.y < 0.0 || projCoord.y > 1.0)
        return 0.0;

    vec2 texelSize = 1.0 / vec2(textureSize(cloudShadowMap, 0));
    float shadow = 0.0;
    int kernel = 3;
    int halfK = kernel / 2;
    for (int x = -halfK; x <= halfK; ++x) {
        for (int y = -halfK; y <= halfK; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            shadow += 1.0 - texture(cloudShadowMap, vec3(projCoord.xy + offset, projCoord.z));
        }
    }
    shadow /= float(kernel * kernel);
    return shadow * cloudShadowIntensity;
}

vec3 computePointLight(vec3 worldPos, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, uint idx) {
    vec3 toLight = lights.pointLights[idx].xyz - worldPos;
    float dist = length(toLight);
    float radius = lights.pointLights[idx].w;
    if (dist > radius) return vec3(0.0);

    vec3 L = normalize(toLight);

    float s = dist / radius;
    float s2 = s * s;
    float atten = pow(1.0 - s2, 2.0) / (1.0 + 1.0 * s2);

    float intensity = lights.pointColors[idx].w;
    vec3 color = lights.pointColors[idx].rgb;

    return pbrCookTorrance(N, V, L, albedo, metallic, roughness, color, intensity * atten);
}

vec3 linearToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = pow(c, vec3(1.0 / 2.4)) * 1.055 - vec3(0.055);
    return mix(lo, hi, step(vec3(0.0031308), c));
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 legacyDirectionalLight(vec3 N, vec3 albedo, vec3 viewDir, vec4 pbr) {
    vec3 L = normalize(-lights.dirLightDir.xyz);
    float NdotL = max(dot(N, L), 0.0);
    vec3 lColor = lights.dirLightColor.rgb;
    float lIntensity = lights.dirLightDir.w;

    // The g-buffer stores albedo in sRGB to match legacy brightness expectations.
    // For a consistent linear pipeline through post-process tone-mapping we must
    // convert before lighting, otherwise gamma is applied twice and the scene
    // looks over-exposed/blows out.
    vec3 albedoLinear = srgbToLinear(albedo.rgb);
    vec3 diffuse = albedoLinear * lColor * NdotL * lIntensity;

    vec3 H = normalize(L + viewDir);
    float roughness = pbr.g;
    float metallic = pbr.b;
    float specPower = mix(8.0, 256.0, 1.0 - roughness);
    // DEBUG: guard roughness/metallic to avoid pow(0, huge) or specular blowout.
    float NdotH = max(dot(N, H), 0.0);
    float specular = (NdotH > 0.0 && specPower < 1000.0) ? pow(NdotH, specPower) * lIntensity : 0.0;
    vec3 specColor = mix(vec3(0.04), albedoLinear, metallic);

    return diffuse + specColor * specular;
}

vec3 legacyComputePointLight(vec3 worldPos, vec3 N, vec3 albedo, uint idx) {
    vec3 toLight = lights.pointLights[idx].xyz - worldPos;
    float dist = length(toLight);
    float radius = lights.pointLights[idx].w;
    if (dist > radius) return vec3(0.0);

    vec3 L = normalize(toLight);
    float NdotL = max(dot(N, L), 0.0);

    float s = dist / radius;
    float s2 = s * s;
    float atten = pow(1.0 - s2, 2.0) / (1.0 + 1.0 * s2);

    float intensity = lights.pointColors[idx].w;
    vec3 color = lights.pointColors[idx].rgb;

    // Keep the legacy path in linear space so post-process gamma is applied once.
    vec3 albedoLinear = srgbToLinear(albedo);
    return albedoLinear * NdotL * color * intensity * atten;
}

// Decode semantic material properties encoded in the normal-map alpha channel
// by the PBR map generator. Bit layout:
//   bit 0 (0x01): wettable (stone, wood, grass, dirt)
//   bit 1 (0x02): snow/ice surface
//   bit 2 (0x04): water surface
//   bit 3 (0x08): frost-capable
//   bit 4 (0x10): metal
struct MaterialProps {
    bool wettable;
    bool snow;
    bool water;
    bool frost;
    bool metal;
};
MaterialProps decodeMaterialProps(float a) {
    uint mask = uint(a * 255.0 + 0.5);
    MaterialProps p;
    p.wettable = (mask & 0x01u) != 0u;
    p.snow     = (mask & 0x02u) != 0u;
    p.water    = (mask & 0x04u) != 0u;
    p.frost    = (mask & 0x08u) != 0u;
    p.metal    = (mask & 0x10u) != 0u;
    return p;
}

// NOTE (IGNIS 2026-09-01): applyClimatePbr() used to live here and was DEAD
// CODE -- defined, never called (the call site survives only in
// directional.frag.bak). It gated everything on decodeMaterialProps(), but the
// semantic bitmask it decodes was never actually written to the G-buffer, so
// every surface arrived with alpha=255, decoded as water+metal, and cidade-A's
// pavement turned into a blue mirror. It was disabled instead of fixed, and
// that is why roughness/metallic stopped reacting to weather at all.
//
// The replacement is pbrWetSurface() in lighting/pbr_common.glsl, called from
// main() below. It needs no authored mask: it derives porosity from the
// roughness/metallic already in the G-buffer and pooling from the MRAH-W
// cavity channel PbrMapGen already bakes. decodeMaterialProps() is kept above
// for G1, when the real semantic mask does get written.

// The red channel encodes the material source: 1.0 = real map, 0.5 = fallback
// heuristic, 0.0 = sprite. Fallbacks should still use the PBR BRDF with the
// global default roughness/metallic; only sprites stay on legacy Blinn-Phong.
bool isPbrFallback(vec4 pbr) {
    return pbr.r < 0.5;
}

void main() {
    vec4 albedo = texture(gbufferAlbedo, inUV);
    if (albedo.a < 0.001 || any(isnan(albedo.rgb)) || any(isinf(albedo.rgb))) { outLit = vec4(0.0); return; }
    // Visualizacao crua do G-buffer: quem desenha e' o passe AMBIENTE (ver
    // ambient.frag). Este passe SOMA por cima, entao aqui so' pode sair zero -
    // senao a leitura mudaria com o sol e com a sombra, que era o bug.
    if ((lights.pbrDebugMode & 95u) != 0u) { outLit = vec4(0.0); return; }
    // Diagnostico (bit 32 de pbrDebugMode, ERUPTION_DEBUG_FLAT_ALBEDO=1): albedo
    // CIANO chapado em tudo - modelo, terreno, sprite - so' a iluminacao e a
    // sombra sobrevivem. Isola cintilacao de sombra de cintilacao de textura.
    if ((lights.pbrDebugMode & 32u) != 0u) albedo.rgb = vec3(0.0, 1.0, 1.0);

    vec3 normal = texture(gbufferNormal, inUV).rgb;
    vec3 worldPos = worldFromDepth(inUV, texture(gbufferDepth, inUV).r);
    if (any(isnan(worldPos)) || any(isinf(worldPos))) worldPos = cameraPos + vec3(0.0, 0.0, 1.0);

    vec3 Nraw = normal * 2.0 - 1.0;
    // Guard against zero/NaN/Inf normals (degenerate geometry or bad G-Buffer)
    // which manifest as flickering black squares / seams at grazing angles.
    if (any(isnan(Nraw)) || any(isinf(Nraw)) || length(Nraw) < 1e-6) {
        Nraw = vec3(0.0, 1.0, 0.0);
    }
    vec3 N = normalize(Nraw);

    vec3 L = normalize(-lights.dirLightDir.xyz);
    vec3 lColor = lights.dirLightColor.rgb;
    float lIntensity = lights.dirLightDir.w;

    vec3 viewDir = cameraPos - worldPos;
    float viewDist = length(viewDir);
    if (any(isnan(viewDir)) || any(isinf(viewDir)) || viewDist < 1e-6) viewDir = vec3(0.0, 0.0, 1.0);
    else viewDir /= viewDist;

    vec4 pbr = texture(gbufferPBR, inUV);
    float roughness = pbr.g;
    float metallic = pbr.b;

    // --- Wet surfaces (IGNIS G2/G3) --------------------------------------
    // pbr.a carries the MRAH-W cavity/pooling mask baked by PbrMapGen
    // (curvature * depression). Below freezing there is no liquid film: the
    // water is ice or snow, handled separately, so the drive fades out.
    // PBR expects linear albedo; the G-buffer stores sRGB to match legacy
    // brightness, so convert once here and reuse everywhere below.
    vec3 albedoLinear = srgbToLinear(albedo.rgb);
    float wetDrive = lights.rainIntensity * smoothstep(-1.0, 2.0, lights.temperatureC);
    PbrWetness wet = pbrWetSurface(albedoLinear, roughness, metallic,
                                   pbr.a, N.y, wetDrive);
    albedoLinear = wet.albedo;
    roughness    = wet.roughness;

    // Full PBR only: when usePbr is enabled every surface uses Cook-Torrance.
    // Fallback values (metallic/roughness from the material profile) are used
    // whenever a real PBR map is not available.
    bool useLegacy = (lights.usePbr == 0u) || isPbrFallback(pbr);
    vec3 lit;
    if (lights.usePbr == 0u) {
        lit = legacyDirectionalLight(N, albedo.rgb, viewDir, pbr);
    } else {
        // albedoLinear/roughness already carry the wet-surface modulation.
        vec3 radiance = pbrCookTorrance(N, viewDir, L, albedoLinear, metallic, roughness, lColor, lIntensity) * lights.pbrLightScale;
        // Keep the lit image in linear space; post_composite.frag applies the final gamma.
        lit = max(radiance, vec3(0.0));
    }

    float shadow = calcShadow(worldPos, normal);
    float cloudShadow = calcCloudShadow(worldPos);

    // Fade sun shadows out as the sun approaches / drops below the horizon.
    // At grazing elevation a shadow-map texel covers many metres of ground, so
    // the shadow edge snaps across the surface in visible chunks as the sun
    // rotates ("the wall edge jumps down a few pixels" at dawn/dusk). Physically
    // the sun's disc is also reddened and scattered near the horizon, so hard
    // shadows there are wrong anyway. L.y is sin(sun elevation).
    float sunElev = normalize(-lights.dirLightDir.xyz).y;
    // Antes: smoothstep(0, 0.12) zerava a sombra com o sol baixo - por isso
    // "as 18h e as 5h ficam sem sombra no mapa". O motivo original (texel
    // gigante no rasante faz a borda pular) é real, então em vez de apagar,
    // mantém-se uma fração: sombra longa de entardecer existe, só mais suave.
    shadow *= mix(0.45, 1.0, smoothstep(0.0, 0.12, sunElev));

    // Screen-space contact shadows: short ray march from the surface toward the
    // sun, testing the world-position buffer. Catches the near-field occluders
    // (crate on the ground, ledge over a wall) whose shadows fall between the
    // shadow map's texels. 8 taps, ~1m default reach.
    //
    // PLANO DE REJEICAO = NORMAL GEOMETRICA, nao a sombreada. O teste "a
    // amostra sobe acima do plano do receptor?" com a normal do normal map
    // inclina o plano por texel (chao terra roxa: 15-20 graus) e qualquer
    // vizinho no MESMO chao plano parece subir dele -> auto-sombra em
    // chuvisco pelo talude inteiro. Parado nao se ve (deterministico por
    // pixel); com a camera girando o pixel troca de texel do normal map e o
    // chuvisco liga/desliga: medido com a saida "so' sombra" (SHADOW_TINT=1)
    // a 0,05 grau/frame, 26% dos pixels mudavam >0,25 por frame com a marcha
    // ligada e 3,4% com ela desligada - era a "sombra flickando quando a
    // camera gira" do autor (2026-09-05). A normal geometrica sai das
    // derivadas do worldPos (FORA do if: dFdx em fluxo nao-uniforme e'
    // indefinido), sinal resolvido pela camera (superficie visivel encara o
    // olho), e o chao plano volta a ser plano para o teste.
    vec3 csGeoN = cross(dFdx(worldPos), dFdy(worldPos));
    float csGeoLen = length(csGeoN);
    csGeoN = (csGeoLen > 1e-8) ? csGeoN / csGeoLen : N;
    if (dot(csGeoN, cameraPos - worldPos) < 0.0) csGeoN = -csGeoN;
    // TAMANHO DE UM PIXEL NO MUNDO, neste ponto. A marcha tem alcance fixo em
    // metros (1,2 u); com a camera a 300+ u um pixel ja' mede 0,4-0,7 u no
    // chao e a marcha inteira cabe em 2-3 pixels - os 8 passos releem o
    // proprio pixel e os vizinhos, e o ruido de reconstrucao do depth vira
    // "bloqueador". Abaixo de ~3 pixels de alcance a sombra de contato nao e'
    // resolvivel: pula. E o bias de rejeicao cresce com o pixel, porque o
    // erro de posicao reconstruida e' proporcional a ele.
    float csPxWorld = max(length(dFdx(worldPos)), length(dFdy(worldPos)));
    if (lights.contactParams.x > 0.5 && shadow < 0.98 && sunElev > 0.02 &&
        lights.contactParams.y > 3.0 * csPxWorld) {
        const int CS_STEPS = 8;
        float csLen = max(lights.contactParams.y, 0.05);
        float stepLen = csLen / float(CS_STEPS);
        // Dither ao longo do raio esconde o banding dos passos. ANCORADO NO
        // MUNDO, nao em gl_FragCoord: com a camera andando, ruido por pixel de
        // tela re-sorteia a fase da marcha em todo pixel a cada frame e o
        // acerto/erro da janela `along` cintila (medido: 12% -> ver abaixo).
        // Preso ao mundo, o padrao anda com o chao e fica coerente entre frames.
        vec2 wq = floor(worldPos.xz * 8.0);
        float ign2 = fract(52.9829189 * fract(dot(wq, vec2(0.06711056, 0.00583715))));
        float csOcc = 0.0;
        // REJEICAO DE AUTO-PLANO — o termo que decide se isto vira sombra de
        // contato ou chuvisco de auto-sombra num talude inteiro.
        //
        // O teste abaixo pergunta "o que amostrei sobe acima do plano do
        // receptor?" usando `N`, que e' a normal SOMBREADA (perturbada pelo
        // normal map), nao a geometrica. Entao o plano de referencia esta'
        // inclinado por texel, e o erro do teste e' proporcional a QUANTO SE
        // MARCHOU: erro ~ distanciaMarchada * sin(angulo entre N sombreada e
        // N geometrica). Com normal map agressivo esse angulo passa de 15 graus
        // facil, o que da' ~0,26 de erro por unidade marchada.
        //
        // A formula anterior escalava com a DISTANCIA DA CAMERA, que nao tem
        // relacao nenhuma com esse erro - e ainda por cima lia a camera de
        // `lights.viewProj[3].xyz`, que e' a quarta COLUNA de uma matriz
        // view-projection e nao a posicao da camera. Ou seja: variavel errada,
        // lida errado. Trocar so' a leitura por `cameraPos` (que ja' chega no
        // push deste shader) DESMASCAROU o artefato em vez de corrigi-lo - o
        // numero sem sentido era grande o bastante pra suprimir a marcha quase
        // inteira, e era isso, nao a rejeicao, que "segurava" o moire.
        //
        // O bias correto cresce com a marcha e por isso fica DENTRO do laco.
        const float CS_NORMAL_ERR = 0.30; // sin(~17 graus) de erro de normal
        for (int i = 1; i <= CS_STEPS; ++i) {
            float marchDist = stepLen * (float(i) - 0.5 + ign2);
            float rejectBias = 0.08 + marchDist * CS_NORMAL_ERR + csPxWorld * 0.5;
            vec3 rp = worldPos + L * marchDist;
            vec4 clip = lights.viewProj * vec4(rp, 1.0);
            if (clip.w <= 0.0) break;
            vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
            if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
            vec3 sp = worldFromDepth(suv, texture(gbufferDepth, suv).r);
            if (any(isnan(sp))) continue;
            // Superficie amostrada so' conta como bloqueador se SOBE claramente
            // acima do plano do receptor (ver o bloco de rejeicao acima).
            if (dot(sp - worldPos, csGeoN) < rejectBias) continue;
            vec3 toRay = rp - sp;
            float along = dot(toRay, L);
            if (along > 0.04 && along < stepLen * 2.0) {
                csOcc = max(csOcc, 1.0 - float(i) / float(CS_STEPS) * 0.5);
            }
        }
        shadow = max(shadow, csOcc * 0.85);
    }

    // OPACIDADE DA SOMBRA. O autor (2026-09-06): "ta feio, ta dark, as sombras
    // tao muito pretas". A sombra sai preta porque o unico preenchimento e' o
    // ambiente; em vez de levantar o ambiente inteiro (que lava a cena), a
    // sombra do SOL passa por uma curva de opacidade por distancia. Perto ela
    // e' a mais dura da tela e e' onde o preto chapado incomoda; longe ela ja'
    // se dilui na nevoa. A sombra de nuvem nao entra na curva - ela ja' e'
    // suave por natureza.
    float sunShadow = shadow;
    if (shadowOpacityParams.y > 0.5) {
        sunShadow *= lut8(shadowOpacityLut0, shadowOpacityLut1,
                          length(cameraPos - worldPos) / max(shadowOpacityParams.x, 1.0));
    }
    lit *= (1.0 - max(sunShadow, cloudShadow));

    // TRANSLUCENCIA DE FOLHAGEM (Barre-Brisebois & Bouchard, GDC 2011:
    // "Approximating Translucency for a Fast, Cheap and Convincing
    // Subsurface Scattering Look"). Folha contra o sol nao fica preta: a luz
    // atravessa e sai esverdeada do outro lado. G32: o gate agora e' o FLAG
    // DE FOLHAGEM do G-buffer (canal R do PBR: 0.875/0.625 = vegetacao, escrito
    // pelo model.frag a partir do swayAmount da instancia). A dominancia de
    // verde, que era o gate, vira so' o PESO: folha verde 100%, folha seca ou
    // tronco da mesma malha 35%. Um telhado verde nao ganha mais wrap.
    // Aditivo e fora do pbr_common (arquivo de outro agente).
    {
        vec3 aLin = srgbToLinear(albedo.rgb);
        float greenness = clamp((aLin.g - max(aLin.r, aLin.b)) * 5.0, 0.0, 1.0);
        const bool foliage = abs(fract(pbr.r * 4.0) - 0.5) < 0.15;
        float leafGate = foliage ? mix(0.35, 1.0, greenness) : 0.0;
        if (leafGate > 0.01) {
            vec3 Lsun = normalize(-lights.dirLightDir.xyz);
            // Luz "distorcida" pela normal: quanto mais o olhar encara a luz
            // atraves da superficie, mais transmissao.
            vec3 vLT = -normalize(Lsun + N * 0.4);
            float ltDot = pow(clamp(dot(viewDir, vLT), 0.0, 1.0), 3.0) * 0.65;
            // Wrap ambiente da folha: um pouco de luz vaza mesmo de lado.
            float ltAmb = 0.18;
            // Sombra do sol atenua a transmissao (folha sombreada nao brilha),
            // mas nao zera - a copa espalha luz internamente.
            float sunVis = 1.0 - max(shadow, cloudShadow) * 0.75;
            vec3 leafTint = aLin * mix(vec3(1.0), vec3(0.85, 1.1, 0.55), greenness); // clorofila
            lit += leafTint * lights.dirLightColor.rgb * lights.dirLightDir.w *
                   (ltDot + ltAmb) * leafGate * sunVis * 0.6;
        }
    }

    // Auto-sombra do micro-relevo (gbuffer emissive.a, escrito por
    // model.frag/terrain.frag). Depende do SOL, não da vista: é o que faz a
    // fresta entre pedras/tijolos escurecer e girar conforme o sol anda -
    // relevo que lê mesmo com a câmera de cima. Só a luz DIRETA é afetada;
    // o ambiente segue com a cavidade (albedo.a).
    float microSunVis = texture(gbufferEmissive, inUV).a;
    lit *= microSunVis;

    // OCLUSAO DE CAVIDADE NA LUZ DIRETA.
    //
    // Antes eram DOIS termos multiplicando `lit` com pesos magicos diferentes:
    //   lit *= mix(1.0, ssaoTex.r, 0.35);   // 65% do SSAO jogado fora
    //   lit *= mix(1.0, albedo.a,  0.6);    // AO do G-buffer, outro peso
    // Os dois medem a MESMA grandeza (fracao do hemisferio fechada por
    // micro-geometria) em escalas diferentes - um em espaco de tela, outro
    // assado na textura. Multiplicar os dois conta a mesma oclusao duas vezes,
    // e os pesos 0,35/0,6 nao vinham de lugar nenhum.
    //
    // Fisicamente, oclusao de curto alcance nao atenua a luz direta de forma
    // uniforme: ela modula a VISIBILIDADE DO DISCO SOLAR. Numa face voltada
    // para o sol o disco esta' quase todo a vista mesmo dentro de uma cavidade;
    // numa face rasante a mesma cavidade fecha quase tudo. Por isso o peso
    // agora e' (1 - NdotL): a fenda escurece onde a fenda de fato bloqueia.
    //
    // Efeito colateral desejado: e' este termo que da' o "contact darkening"
    // de objeto pousado (a base do prop encosta em superficie rasante), que a
    // pesquisa de percepcao de miniatura aponta como pista forte de escala.
    // Custo zero - uma busca de textura a menos que antes.
    // `ao` segue nomeado porque o especular AMBIENTAL (mais abaixo) tambem o
    // usa - e la' a atenuacao cheia esta' certa: aquilo e' luz de hemisferio,
    // nao disco solar.
    float ao = albedo.a;
    float NdotLsun = max(dot(N, L), 0.0);
    float cavity = min(texture(ssaoTex, inUV).r, ao);
    lit *= mix(1.0, cavity, clamp(1.0 - NdotLsun, 0.0, 1.0) * 0.8);

    // NOTE: the hemispheric ambient DIFFUSE is handled once by ambient.frag.
    // This pass only ADDS the directional (sun/moon) contribution + the ambient
    // SPECULAR below.

    // Analytic environment specular (cheap IBL approximation, no cubemap/probe).
    // Reflect the view around N, sample the procedural sky gradient in that
    // direction, weight by a roughness-aware Fresnel. Glossy / wet / metal
    // surfaces get an environment highlight instead of reading dead-flat under a
    // pure-diffuse ambient. Scaled by giIntensity so the GI slider governs
    // indirect specular too. Fades to nothing on matte (roughness -> 1).
    if (lights.usePbr != 0u && lights.indirectParams.x > 0.5) {
        vec3 R = reflect(-viewDir, N);
        // Widen the reflection lobe toward the normal as roughness grows, so a
        // rough surface samples a broad chunk of sky instead of a mirror line.
        // For a smooth analytic sky this closed-form widening IS the prefiltered
        // environment: the GGX prefilter integral of a vertical gradient
        // collapses to evaluating the gradient at the mean lobe direction.
        vec3 Rr = normalize(mix(R, N, roughness * roughness));
        // Evaluate the ACTUAL procedural sky in the reflected direction: the
        // same horizon->top gradient the skybox draws, with the sun-lit ground
        // colour below the horizon line.
        float skyT = smoothstep(0.0, 0.4, Rr.y);
        vec3 skyCol = mix(lights.skyHorizon.rgb, lights.ambientSky.rgb, skyT);
        if (lights.skyProbeParams.x > 0.5) {
            // SONDA DE CEU (G36): o ceu de verdade (nuvens, lado do sol,
            // crepusculo, tempestade) pre-filtrado por mip. O alargamento do
            // lobulo agora e' o mip (roughness -> lod); Rr continua puxando a
            // direcao dominante para N como antes.
            skyCol = textureLod(skyProbe, Rr, roughness * lights.skyProbeParams.y).rgb
                   * lights.skyProbeParams.z;
        }
        vec3 gndCol = lights.ambientGround.rgb * (0.25 + lights.dirLightColor.rgb * clamp(lights.dirLightDir.w, 0.0, 1.5));
        vec3 envCol = mix(gndCol, skyCol, smoothstep(-0.15, 0.05, Rr.y));
        // Split-sum environment BRDF (Karis 2014, analytic - no LUT texture).
        // Returns the (scale, bias) that pre-integrate the GGX BRDF over the
        // hemisphere: specular = envColor * (F0 * scale + bias).
        vec3 F0e = mix(vec3(0.04), albedoLinear, metallic);
        float NoV = clamp(dot(N, viewDir), 0.0, 1.0);
        const vec4 c0 = vec4(-1.0, -0.0275, -0.572,  0.022);
        const vec4 c1 = vec4( 1.0,  0.0425,  1.04,  -0.04);
        vec4 r = roughness * c0 + c1;
        float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
        vec2 envBRDF = vec2(-1.04, 1.04) * a004 + r.zw;
        vec3 specEnv = F0e * envBRDF.x + envBRDF.y;
        lit += envCol * specEnv * ao
               * max(lights.giIntensity, lights.giAmbientFloor)
               * lights.indirectParams.y;
    }

    // One-bounce GI: sunlight hits the ground, picks up the map's average
    // ground colour (computed at load - real per-map colour bleed), and lights
    // walls and undersides from below. Strongest on surfaces facing DOWN toward
    // the lit ground plane, scaled by the sun and occluded by AO/SSAO.
    if (lights.bounceParams.x > 0.5 && lights.usePbr != 0u) {
        float sunI2 = clamp(lights.dirLightDir.w, 0.0, 2.0);
        if (sunI2 > 0.02) {
            // How much of the bounced (upward) light this surface catches.
            float catchUp = clamp(-N.y * 0.5 + 0.5, 0.0, 1.0); // 1 facing down, 0.5 vertical walls
            vec3 groundLit = lights.ambientGround.rgb * lights.dirLightColor.rgb * sunI2;
            vec3 albedoLin2 = albedoLinear; // wet ground bounces less
            // `ao * ssaoD` contava a mesma oclusao duas vezes (os dois medem
            // fracao de hemisferio fechada, um assado e um em espaco de tela);
            // o produto de dois termos <1 fechava a luz de rebote bem mais do
            // que qualquer um dos dois sozinho. `cavity` e' o min dos dois.
            lit += albedoLin2 * groundLit * catchUp * lights.bounceParams.y
                   * cavity * (1.0 - shadow * 0.5);
        }
    }

    // Debug (cascadeScales.z = ERUPTION_TEST_SHADOW_TINT / F2 checkbox): paint
    // sun-shadowed fragments magenta instead of only darkening, to isolate
    // the sun shadow from cloud shadows and shading.
    if (cascadeScales.z > 0.999) {
        // Opacidade 1 = SO' o fator de sombra, sem albedo nem N.L por baixo.
        // E' o unico jeito de MEDIR a sombra isolada numa sequencia de frames:
        // com o mix, textura e normal map por baixo contaminam o valor lido.
        lit = vec3(shadow);
    } else if (cascadeScales.z > 0.001) {
        lit = mix(lit, vec3(1.0, 0.0, 1.0), shadow * cascadeScales.z);
    }

    // Final NaN/Inf safety net: replace any corrupted output with a dark but
    // visible magenta so a single bad pixel does not bloom into a black square.
    if (any(isnan(lit)) || any(isinf(lit))) {
        lit = vec3(0.05, 0.0, 0.05);
    }

    outLit = vec4(lit, 1.0);
}
