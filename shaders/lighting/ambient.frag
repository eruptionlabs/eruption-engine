#version 450
#extension GL_GOOGLE_include_directive : enable

#include "lighting/pbr_common.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outLit;

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 3) uniform sampler2D gbufferPBR;
layout(set = 0, binding = 4) uniform sampler2D gbufferEmissive;
layout(set = 0, binding = 13) uniform sampler2D ssaoTex; // blurred half-res SSAO

// Lighting UBO (SDD 2.4.2) - must stay byte-identical to directional.frag
// and the C++ LightingUBO struct.
layout(set = 0, binding = 7) uniform LightingUBO {
    vec4 dirLightDir;       // xyz=direção, w=intensidade
    vec4 dirLightColor;     // xyz=cor RGB, w=ambientIntensity
    vec4 pointLights[128];  // xyz=posição, w=raio
    vec4 pointColors[128];  // xyz=cor, w=intensidade final (já modulada)
    uint numPointLights;
    float giIntensity;
    float giAmbientFloor;
    uint usePbr;            // 0 = legacy Blinn-Phong, 1 = Cook-Torrance/GGX
    uint pbrDebugMode;      // bitmask: 1=roughness, 2=metallic, 4=normal, 8=wetness, 16=source
    float rainIntensity;    // 0..1 global rain intensity
    float snowIntensity;    // 0..1 global snow intensity
    float temperatureC;     // surface temperature in Celsius
    float pbrLightScale;    // multiplier for PBR direct-light response
    float sunAngularRadius; // era pad0; raio angular da fonte, em radianos
    uint pad1;
    vec4 ambientSky;        // xyz=skyColor
    vec4 ambientGround;     // xyz=groundColor
    vec4 ssaoParams;        // x=enabled, y=strength, z=radius, w=unused
    vec4 indirectParams;    // x=envSpecEnabled, y=envSpecIntensity, z=nightAoPow, w=ambientHemiFloor
    mat4 viewProj;
    vec4 bounceParams;      // x=sunBounceEnabled, y=strength
    vec4 contactParams;     // x=contactShadowEnabled, y=lengthMetres
    vec4 skyHorizon;        // xyz=procedural sky horizon colour
    mat4 invViewProj;
    vec4 skyProbeParams;    // x=sonda de ceu ligada, y=mip max, z=intensidade (G36)
    vec4 probeGridMin;      // xyz=canto minimo da grade de probes, w=ligadas (G38)
    vec4 probeGridInvExtent;// xyz=1/extensao da grade, w=forca
    uvec4 tileLights[128];  // (do passe direcional; so' para chegar aos campos abaixo)
    vec4 rainSpots[64];     // chuva local: xy = centro XZ, z = raio
    vec4 rainSpotParams;    // x = quantos, y = 1 com campo de nuvens, z = nivel da agua, w = tempo
    vec4 shoreMask;         // igual ao directional.frag
    vec4 shoreWaterColor;
    vec4 shoreLiquids[8];   // igual ao directional.frag
} lights;

// PROBES DE IRRADIANCIA (G38): visibilidade do ceu por probe, SH L1 numa
// textura 3D (lighting/probe_bake.comp). E(N) = c4*L0 + 2*c2*(L1.N); /pi da'
// 1 em ceu aberto e cai embaixo de copa/telhado - o que o SSAO de curto
// alcance e o AO de textura nao enxergam.
layout(set = 0, binding = 17) uniform sampler3D probeVis;

// BEIRA D'AGUA (binding 18): 1 sobre agua visivel, cai a 0 longe da margem.
// Sem ela a onda molhava todo chao na altura da agua (piscina "transbordando").
layout(set = 0, binding = 18) uniform sampler2D shoreMaskTex;
vec3 shoreMaskAt(vec3 wp) {
    // x = proximidade (filtrada); y = QUAL liquido e z = altura da superficie
    // dele, do texel mais perto e sem filtro (misturar dois indices daria um
    // terceiro). Grade RGBA16F (Engine::rebuildShoreMask).
    if (lights.shoreMask.z <= 0.0) return vec3(0.0);
    vec2 uv = (wp.xz - lights.shoreMask.xy) * lights.shoreMask.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(0.0);
    ivec2 sz = textureSize(shoreMaskTex, 0);
    ivec2 tc = clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1);
    vec4 t = texelFetch(shoreMaskTex, tc, 0);
    return vec3(textureLod(shoreMaskTex, uv, 0.0).r, clamp(floor(t.g + 0.5), 0.0, 7.0), t.b);
}
// 1 onde a onda pode subir, 0 atras de parede: a = maior inclinacao do chao
// ate' a agua (Engine::rebuildShoreMask). Praia ~0,1; barranco ~0,5; parede
// de piscina (90 graus) dezenas. A agua nao sobe parede: corta de 35 a 55 graus.
// Direcao da agua para a terra (XZ): menos o gradiente da proximidade da
// grade (1 na agua, 0 a 24 u). A espuma da lamina anda ao longo dela.
vec2 shoreOutward(vec3 wp) {
    if (lights.shoreMask.z <= 0.0) return vec2(0.0);
    vec2 uv = (wp.xz - lights.shoreMask.xy) * lights.shoreMask.z;
    float e = 2.0 * lights.shoreMask.z; // 2 u
    vec2 g = vec2(textureLod(shoreMaskTex, uv + vec2(e, 0.0), 0.0).r - textureLod(shoreMaskTex, uv - vec2(e, 0.0), 0.0).r,
                  textureLod(shoreMaskTex, uv + vec2(0.0, e), 0.0).r - textureLod(shoreMaskTex, uv - vec2(0.0, e), 0.0).r);
    float l = length(g);
    return l > 1e-5 ? -g / l : vec2(0.0);
}
float shoreWallPass(vec3 wp) {
    if (lights.shoreMask.z <= 0.0) return 1.0;
    vec2 uv = (wp.xz - lights.shoreMask.xy) * lights.shoreMask.z;
    return 1.0 - smoothstep(0.7, 1.4, textureLod(shoreMaskTex, uv, 0.0).a);
}
// Brilho da lava no chao (beira de lava), somado ao emissivo no fim.
vec3 g_lavaGlow = vec3(0.0);
vec3 worldFromDepth(vec2 uv, float d) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, d, 1.0);
    vec4 wp = lights.invViewProj * ndc;
    if (abs(wp.w) < 1e-6) return vec3(0.0);
    return wp.xyz / wp.w;
}
float probeSkyVisibility(vec3 N) {
    if (lights.probeGridMin.w < 0.5) return 1.0;
    vec3 wp = worldFromDepth(inUV, texture(gbufferDepth, inUV).r);
    vec3 g = (wp - lights.probeGridMin.xyz) * lights.probeGridInvExtent.xyz;
    if (any(lessThan(g, vec3(0.0))) || any(greaterThan(g, vec3(1.0)))) return 1.0;
    vec4 sh = texture(probeVis, g);
    float E = 0.886227 * sh.x + 1.023328 * dot(sh.yzw, N);
    float vis = clamp(E / 3.14159265, 0.0, 1.0);
    return mix(1.0, vis, lights.probeGridInvExtent.w);
}

// SONDA DE CEU (G36): SH9 do cubemap do ceu procedural (skybox/sky_sh.comp).
// Irradiancia em forma fechada (Ramamoorthi & Hanrahan 2001, "An Efficient
// Representation for Irradiance Environment Maps"). Ordem dos coeficientes
// igual a' do compute: 0=L00 1=L1-1(y) 2=L10(z) 3=L11(x) 4=L2-2(xy) 5=L2-1(yz)
// 6=L20 7=L21(xz) 8=L22.
layout(std430, set = 0, binding = 16) readonly buffer SkySH { vec4 sh[9]; } skySH;
vec3 shIrradiance(vec3 n) {
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708;
    vec3 L00 = skySH.sh[0].rgb, L1m1 = skySH.sh[1].rgb, L10 = skySH.sh[2].rgb, L11 = skySH.sh[3].rgb;
    vec3 L2m2 = skySH.sh[4].rgb, L2m1 = skySH.sh[5].rgb, L20 = skySH.sh[6].rgb;
    vec3 L21 = skySH.sh[7].rgb, L22 = skySH.sh[8].rgb;
    float x = n.x, y = n.y, z = n.z;
    return c1 * L22 * (x * x - y * y) + c3 * L20 * z * z + c4 * L00 - c5 * L20
         + 2.0 * c1 * (L2m2 * x * y + L21 * x * z + L2m1 * y * z)
         + 2.0 * c2 * (L11 * x + L1m1 * y + L10 * z);
}

// Bloco por draw no set 1 (PushRing, UBO de offset dinamico): passa de 128 B.
layout(std140, set = 1, binding = 0) uniform PushConstants {
    vec3 ambientColor;
    float ambientIntensity;
    vec3 emissiveColor;
    float emissiveIntensity;
};

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 linearToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = pow(c, vec3(1.0 / 2.4)) * 1.055 - vec3(0.055);
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    vec4 albedo = texture(gbufferAlbedo, inUV);
    if (albedo.a < 0.001 || any(isnan(albedo.rgb)) || any(isinf(albedo.rgb))) { outLit = vec4(0.0, 0.0, 0.0, 0.0); return; }
    // Diagnostico (bit 32 de pbrDebugMode, ERUPTION_DEBUG_FLAT_ALBEDO=1): albedo
    // CIANO chapado em tudo - modelo, terreno, sprite - so' a iluminacao e a
    // sombra sobrevivem. Isola cintilacao de sombra de cintilacao de textura.
    if ((lights.pbrDebugMode & 32u) != 0u) albedo.rgb = vec3(0.0, 1.0, 1.0);

    // Albedo alpha is vertexAO (SDD 2.5)
    float ao = albedo.a;
    
    vec3 normal = texture(gbufferNormal, inUV).rgb;
    vec3 Nraw = normal * 2.0 - 1.0;
    // Guard against NaN/Inf normals that leak from degenerate geometry or bad
    // normal maps and show up as flickering black squares.
    if (any(isnan(Nraw)) || any(isinf(Nraw)) || length(Nraw) < 1e-6) Nraw = vec3(0.0, 1.0, 0.0);
    vec3 N = normalize(Nraw);

    // VISUALIZACAO CRUA DO G-BUFFER (bitmask pbrDebugMode: checkbox "Show
    // Normal (RGB)" e irmaos). Fica AQUI, no passe AMBIENTE, porque ele e' o
    // primeiro e escreve com blend DESLIGADO - o valor sai exato na tela.
    //
    // Antes o overlay vivia no FIM do passe DIRECIONAL, que soma (ONE/ONE)
    // por cima do ambiente e cujo resultado depende do sol e da sombra:
    // "a luz bate ta' ok, quando vai pra sombra parece que o que interage com
    // o PBR some" (relato do autor, 2026-09-05). Uma visualizacao de G-buffer
    // nao pode depender de iluminacao - o dado que ela mostra e' escrito
    // ANTES de qualquer luz. Os passes direcional e pontual devolvem zero
    // quando qualquer destes bits esta' ligado, e o composite pula
    // fog/tonemap/bloom, entao a leitura e' identica a qualquer hora do dia.
    if ((lights.pbrDebugMode & 95u) != 0u) {
        vec4 dpbr = texture(gbufferPBR, inUV);
        vec3 dbg = vec3(0.0);
        if ((lights.pbrDebugMode & 1u)  != 0u) dbg += vec3(dpbr.g, 0.0, 0.0);    // roughness
        if ((lights.pbrDebugMode & 2u)  != 0u) dbg += vec3(0.0, dpbr.b, 0.0);    // metallic
        if ((lights.pbrDebugMode & 4u)  != 0u) dbg += normal;                    // normal, como esta' no G-buffer
        if ((lights.pbrDebugMode & 8u)  != 0u) dbg += vec3(0.0, dpbr.a, dpbr.a); // wetness/cavidade
        if ((lights.pbrDebugMode & 16u) != 0u) dbg += vec3(dpbr.r, 0.0, dpbr.r);
        if ((lights.pbrDebugMode & 64u) != 0u) dbg += vec3(albedo.a); // AO final do G-buffer // flag de origem
        outLit = vec4(clamp(dbg, 0.0, 1.0), 1.0);
        return;
    }
    
    // Hemispheric ambient (SDD 2.4). Slightly steeper than 0.5/0.5 so surfaces
    // facing up vs down differ more - normal-mapped micro-facets then still show
    // relief when the sun is off and this is the only light.
    // Cosine-convolved irradiance of the procedural sky (closed form for a
    // vertical gradient): up-facing surfaces see mostly zenith, side-facing see
    // the horizon band, down-facing see the sun-lit ground albedo.
    vec3 zenith = lights.ambientSky.rgb;
    vec3 horizon = lights.skyHorizon.rgb;
    vec3 groundColor = lights.ambientGround.rgb;
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 skyIrr = mix(horizon, zenith, up * up); // horizon dominates except straight up
    if (lights.skyProbeParams.x > 0.5) {
        // SONDA DE CEU (G36): irradiancia do ceu REAL (nuvens escurecem e
        // achatam, lado do sol mais claro, tempestade, crepusculo) em vez do
        // gradiente de 2 cores. /pi devolve a escala de radiancia que o
        // gradiente usava: ceu uniforme de radiancia L da' E/pi = L.
        skyIrr = max(shIrradiance(N), vec3(0.0)) * (lights.skyProbeParams.z / 3.14159265);
    }
    float hemiFloor = clamp(lights.indirectParams.w, 0.0, 1.0);
    float hemiFactor = hemiFloor + (1.0 - hemiFloor) * up;
    vec3 hemiAmbient = mix(groundColor, skyIrr, hemiFactor);
    // G38: o ceu (e o chao iluminado por ele) so' chega onde a geometria de
    // cima deixa - copa, telhado, patio.
    hemiAmbient *= probeSkyVisibility(N);

    // Global Illumination logic
    // We ignore the map's native ambient intensity and rely on GI Intensity + Floor
    vec3 baseAmbient = hemiAmbient * lights.giIntensity;
    vec3 ambientFloor = vec3(lights.giAmbientFloor);

    // AO contrast rises as the sun drops. At night the direct term (where the
    // normal map's dot(N,L) relief lives) is ~2% intensity, so the bump map
    // "turns off" - the surface is lit only by this flat ambient. Crushing AO
    // harder at night puts the relief back via crevice darkening alone, which is
    // light-independent. Day: ao^1 (unchanged, the sun carries the relief).
    float sunI = clamp(lights.dirLightDir.w, 0.0, 1.0);
    // O expoente do AO era interpolado pela intensidade GLOBAL do sol, entao
    // ao meio-dia caia para 1.0 (contraste minimo). Um pixel NA SOMBRA ao
    // meio-dia perde a luz direta - onde mora todo o relevo - e recebia um
    // ambiente sem contraste: superficie chapada na sombra (relatado pelo
    // usuario). Piso de 1.35 mantem a cavidade legivel sob qualquer sol; o
    // ganho abaixo devolve o brilho medio que o expoente tira.
    float aoShaped = pow(clamp(ao, 0.0, 1.0), mix(max(lights.indirectParams.z, 1.0), 1.35, sunI)) * 1.12;

    // Screen-space AO from the dedicated half-res pass (ssao.frag ->
    // ssao_blur.frag). The pass itself applies strength/radius and writes 1.0
    // when disabled, so a plain read is all the lighting needs.
    float ssao = texture(ssaoTex, inUV).r;

    // Match the linear-space PBR lighting passes. The ambient (== the "GI" term)
    // is now geometrically occluded by ssao, so the GI slider controls a light
    // that actually responds to the scene instead of a flat multiply.
    vec3 albedoLinear = srgbToLinear(albedo.rgb);
    // Wet surfaces (IGNIS G2). The ambient term is the single biggest
    // contributor under an overcast rain sky, so if only the directional pass
    // darkened the wet albedo the ambient would wash the effect straight back
    // out and rain would keep reading as "bright and glossy". Same model, same
    // inputs as directional.frag -- the two passes must agree.
    {
        vec4 pbrA = texture(gbufferPBR, inUV);
        float wetDriveA = lights.rainIntensity * smoothstep(-1.0, 2.0, lights.temperatureC);
        // Chuva local (igual ao directional.frag): so' debaixo das nuvens que chovem.
        if (lights.rainSpotParams.y > 0.5 && wetDriveA > 0.0) {
            vec3 wpA = worldFromDepth(inUV, texture(gbufferDepth, inUV).r);
            float localA = 0.0;
            int nA = int(lights.rainSpotParams.x);
            for (int i = 0; i < nA; ++i) {
                vec4 s = lights.rainSpots[i];
                localA = max(localA, s.w * (1.0 - smoothstep(0.7 * s.z, 1.2 * s.z, length(wpA.xz - s.xy))));
            }
            wetDriveA *= localA;
        }
        // Beira d'agua (espuma/onda): molha e seca sozinha, com ou sem chuva.
        const vec3 wpA = worldFromDepth(inUV, texture(gbufferDepth, inUV).r);
        const vec3 beiraGA = shoreMaskAt(wpA);
        const vec4 liqA = lights.shoreLiquids[int(beiraGA.y)];
        const bool lavaA = liqA.z > 0.5;
        const float beiraA = liqA.x > -1.0e8 ? beiraGA.x : 0.0;
        float molhaBeiraA = 0.0;
        const vec2 foraA = beiraA > 0.0 && !lavaA ? shoreOutward(wpA) : vec2(0.0); // igual ao directional.frag
        if (beiraA > 0.0 && !lavaA) {
            molhaBeiraA = smoothstep(0.0, 0.15, beiraA) * shoreWetness(wpA, liqA.x, lights.rainSpotParams.w, pbrA.a, beiraA, foraA) * shoreWallPass(wpA);
            wetDriveA = max(wetDriveA, molhaBeiraA);
        }
        if ((lights.pbrDebugMode & 256u) != 0u) { wetDriveA = 0.0; molhaBeiraA = 0.0; } // A/B: sem molhado (bit 256)
        if (abs(pbrA.r - 0.75) < 0.06) molhaBeiraA = 0.0; // sprite nao encharca
        PbrWetness wetA = pbrWetSurface(albedoLinear, pbrA.g, pbrA.b,
                                        abs(pbrA.r - 0.75) < 0.06 ? 0.0 : pbrA.a, N.y,
                                        abs(pbrA.r - 0.75) < 0.06 ? 0.0 : wetDriveA); // sprite nao encharca
        albedoLinear = shoreWetAlbedo(wetA.albedo, molhaBeiraA, N.y);
        // Lamina da onda (igual ao directional.frag: as duas passadas concordam).
        if (abs(pbrA.r - 0.75) >= 0.06 && (lights.pbrDebugMode & 256u) == 0u && lavaA && beiraA > 0.0) {
            vec3 lvA = lavaShore(wpA, beiraGA.z, lights.rainSpotParams.w, beiraA, liqA.y > 0.5) * smoothstep(0.0, 0.15, beiraA);
            albedoLinear = lavaShoreAlbedo(albedoLinear, lvA);
            // 0,25: com o brilho cheio do liquido (emissivo ~4) a borda e as
            // fendas estouravam o bloom e lavavam a tela inteira.
            g_lavaGlow = lavaShoreGlow(lvA, wpA, lights.rainSpotParams.w) * max(liqA.w, 0.5) * 0.25;
        }
        if (abs(pbrA.r - 0.75) >= 0.06 && (lights.pbrDebugMode & 256u) == 0u && !lavaA && liqA.y > 0.5 && beiraA > 0.0) {
            vec4 swA = shoreSwash(wpA, liqA.x, lights.rainSpotParams.w, N.y, liqA.y > 1.5, beiraA, foraA) * smoothstep(0.0, 0.15, beiraA) * shoreWallPass(wpA);
            if (swA.x + swA.z > 0.0) albedoLinear = shoreSwashAlbedo(albedoLinear, swA, lights.shoreWaterColor, lights.skyHorizon.rgb);
        }
    }
    // FILL LIGHT (fake 3D do diorama): luz de direção FIXA, sem sombra e sem
    // dependência do sol. Sem ela, um pixel na sombra recebe só ambiente
    // hemisférico - que numa superfície virada pra cima quase não varia com a
    // normal - e o relevo do normal map simplesmente NÃO LÊ na sombra (o
    // problema relatado). Custo: um dot product. Intensidade em
    // bounceParams.z (campo que já existia livre no UBO).
    vec3 fillDir = normalize(vec3(0.35, 0.75, 0.55));
    float fillN = max(dot(N, fillDir), 0.0);
    // Curva suave: realça a inclinação sem estourar a superfície plana.
    fillN = fillN * fillN * (3.0 - 2.0 * fillN);
    // MODULAÇÃO direcional (não soma): somar preenchimento clareia a cena sem
    // criar contraste - o relevo continuava chapado na sombra. Multiplicar a
    // ambiente por um gradiente centrado em 1.0 mantém o brilho médio e faz a
    // face inclinada escurecer/clarear conforme a normal, que é o que o olho
    // lê como 3D. Neutro em fillN=0.5, então superfície plana não muda.
    float fillK = clamp(lights.bounceParams.z, 0.0, 1.5);
    float dirMod = 1.0 + fillK * (2.0 * fillN - 1.0);
    // Auto-sombra do relevo em direcao FIXA (gbufferNormal.a, escrita pelos
    // shaders de gbuffer). E' o mesmo mecanismo que faz o 3D funcionar sob o
    // sol - aqui ele vale mesmo na sombra, e como a direcao e' constante nao
    // "anda" com o ciclo do dia.
    float fillShadow = texture(gbufferNormal, inUV).a;
    // A auto-sombra so' pesa na face que olha para o preenchimento: na face
    // de costas ela somava ao escurecimento do gradiente e a ambiente caia a
    // ~20% - de noite, sem sol, parede virada para o outro lado ficava preta
    // chapada (o mostrador de tras do relogio "virava sombra" ao girar a
    // camera). Piso de 55% no gradiente pelo mesmo motivo.
    dirMod *= mix(1.0, fillShadow, clamp(fillK, 0.0, 1.0) * fillN);
    dirMod = max(dirMod, 0.55);

    vec3 ambientLinear = max(baseAmbient, ambientFloor) * aoShaped * ssao;
    ambientLinear *= dirMod;
    vec3 finalAmbient = albedoLinear * ambientLinear;

    vec4 emissive = texture(gbufferEmissive, inUV);
    vec3 emissiveLit = srgbToLinear(emissive.rgb) * emissiveIntensity + g_lavaGlow;

    // Keep the lit image in linear space; post_composite.frag applies the final gamma.
    vec3 lit = clamp(finalAmbient + emissiveLit, 0.0, 100.0);
    if (any(isnan(lit)) || any(isinf(lit))) lit = vec3(0.05, 0.0, 0.05);
    outLit = vec4(lit, 1.0);
}
