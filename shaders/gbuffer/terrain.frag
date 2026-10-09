#version 450
#extension GL_EXT_nonuniform_qualifier : enable

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

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPBR;
layout(location = 3) out uint outMaterialID;
layout(location = 4) out vec4 outEmissive;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    float metallicScale;  // absolute metallic value from the material profile
    float roughnessScale; // absolute roughness value from the material profile
};

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
    vec4 u_sunColor;
    vec4 u_ambientSky;
    vec4 u_ambientGround;
    // POM: x=heightScale, y=maxSteps, z=minSteps, w=wetness global.
    vec4 u_pomParams;
};

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

    vec3 N = (length(inNormal) > 1e-8) ? normalize(inNormal) : vec3(0.0, 1.0, 0.0);

    // Normal mapping is only computed when a real normal map is bound.
    vec2 uv = inTexCoord;
    mat3 TBN;
    bool hasPbr = inPbrIndex != 0u;
    bool hasNormal = inNormalIndex != 0u;
    if (hasNormal) {
        TBN = computeTBN(N, inWorldPos, inTexCoord);
    }

    uint texIdx = nonuniformEXT(inTexIndex);
    vec4 texColor = texture(u_textures[texIdx], uv);
    // Ground crossfade. `inBlendWeight` is a per-vertex weight (0 when the mesh
    // carries no _BLEND_* attributes, so untagged maps are untouched). A plain
    // mix() of that weight interpolates linearly across each mesh triangle, so
    // the seam takes on the shape of the triangulation ("triangular", not a
    // real crossfade). Height-based blending (Bloom, "Advanced Terrain Texture
    // Splatting") fixes that: bias the blend by the two textures' own detail so
    // the transition contour follows the surface, not the mesh. Luminance is a
    // free height proxy - no extra textures baked.
    float blendT = inBlendWeight;
    if (inBlendWeight > 0.0) {
        uint bTexIdx = nonuniformEXT(inBlendTexIndex);
        vec4 bColor = texture(u_textures[bTexIdx], uv);
        // Height-biased blend (familia "height lerp"): nudge the
        // linear per-vertex weight by the two textures' own luminance so the
        // 50% contour follows surface detail instead of the mesh triangles.
        // Kept gentle - a strong bias or added noise turns the band grainy.
        float hA = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        float hB = dot(bColor.rgb,  vec3(0.299, 0.587, 0.114));
        blendT = clamp(inBlendWeight + (hB - hA) * 0.35, 0.0, 1.0);
        texColor.rgb = mix(texColor.rgb, bColor.rgb, blendT); // colour only; keep alpha
    }

    if (texColor.a < 0.1) discard;

    // Keep albedo in sRGB for both legacy and PBR paths so the base brightness
    // matches the pre-PBR branch. Only F0/specular benefits from linear-space
    // conversion; converting the whole albedo made the scene look unnaturally
    // dark compared to the legacy renderer.
    vec3 albedo = texColor.rgb * inColor.rgb;

    // Alpha from terrain used as static occlusion (vertexAO)
    float vertexAO = inColor.a;
    outAlbedo = vec4(albedo, vertexAO);

    // AO term: vertex AO * normal-map cavity (* light MRAH-W AO). 1.0 = none.
    // vertexAO e' oclusao MACRO (assada na geometria do mapa) e continua
    // multiplicando: escala diferente da cavidade de textura, entao compor os
    // dois nao e' contar a mesma coisa duas vezes. Os dois termos de CAVIDADE,
    // esses sim, sao a mesma grandeza e vao por min() - ver model.frag.
    float aoTerm = vertexAO;
    float aoCavityNormal = 1.0;
    float aoCavityBaked  = 1.0;

    // Normal mapping. Decode the tangent-space normal, optionally flip the
    // green channel for Y-down normal maps, then blend toward a flat normal
    // based on the global strength so 0 = off, 1 = authored, >1 = exaggerated.
    float materialProps = 0.0;
    // 1.0 = texel vê o sol; < 1 = na sombra do próprio relevo (ver model.frag).
    float sunSelfShadow = 1.0;
    // 1.0 = texel ve a luz de preenchimento; < 1 = na propria sombra do relevo.
    float fillSelfShadow = 1.0;
    if (hasNormal) {
        uint nIdx = nonuniformEXT(inNormalIndex);
        vec4 sampledNormal = texture(u_textures[nIdx], uv, u_normalSmoothing);
        if (inBlendWeight > 0.0 && inBlendNormalIndex != 0u) {
            uint bnIdx = nonuniformEXT(inBlendNormalIndex);
            vec4 bNormal = texture(u_textures[bnIdx], uv, u_normalSmoothing);
            sampledNormal = mix(sampledNormal, bNormal, blendT);
        }
        materialProps = sampledNormal.a;

        // Sem reconstrução de z - ver a nota em model.frag (normals do acervo
        // são não-unitários de fábrica e o look depende disso).
        vec3 tangentNormal = sampledNormal.xyz * 2.0 - 1.0;
        if (u_normalMapInvertY > 0.5) {
            tangentNormal.y = -tangentNormal.y;
        }
        float strength = clamp(u_normalMapScale, 0.0, 3.0);
        vec3 flatNormal = vec3(0.0, 0.0, 1.0);
        tangentNormal = mix(flatNormal, tangentNormal, strength);
        // Cavity AO from the normal map (see model.frag).
        uint msMask = uint(u_pomParams.y + 0.5); // ERUPTION_TEST_MICRO_SHADOW (ablacao)
        if ((msMask & 4u) != 0u) aoCavityNormal = mix(1.0, clamp(tangentNormal.z, 0.0, 1.0), 0.7);

        // Contact micro-shadow: march a luma-height toward the sun in tangent
        // space (see model.frag). Dynamic with the sun, folded into AO.
        // DISTANCIA: mesma razao de model.frag - a marcha e' detalhe de sulco
        // de 0,018 em UV, sub-pixel alem de ~150 unidades, e sao DUAS marchas
        // de 5 passos por fragmento num passe 87% limitado por pixel. O
        // terreno e' a maior area de fill da cena, entao aqui o fade paga
        // ainda mais que no modelo.
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
            const float MS_SCALE = 0.018;
            // LOD fixo: no rasante de VISTA o texel estica e a marcha com mip
            // automatico vira aliasing (ver model.frag).
            float msLod = textureQueryLod(u_textures[hIdx], uv).y + 0.5;

            // MARCHA 1, direcao do SOL -> luz direta (outEmissive.a).
            if ((msMask & 1u) != 0u && u_sunDir.w > 0.01) {
                vec3 Lts = transpose(TBN) * normalize(-u_sunDir.xyz);
                float ll = length(Lts);
                if (ll > 1e-4) {
                    Lts /= ll;
                    if (Lts.z > 0.06) {
                        // Ver model.frag: passo limitado + fade no sol rasante
                        // (senao Lts.xy/Lts.z explode e vira ruido preto).
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
                        // Auto-sombra do relevo -> LUZ DIRETA (ver model.frag).
                        sunSelfShadow = clamp(1.0 - occ * strength * 0.45, 0.55, 1.0);
                    }
                }
            }

            // MARCHA 2, direcao FIXA -> ambiente (outNormal.a). O relevo que
            // impressiona na luz vem da auto-sombra, e ela so existe para luz
            // direta - por isso na sombra tudo achatava. Esta marcha usa uma
            // direcao constante (nao o sol), entao da o MESMO 3D onde o sol
            // nao bate e nao "anda" com o ciclo do dia.
            //
            // ANTES (ate 2026-09-05) ela ficava DENTRO do gate do sol acima
            // (u_sunDir.w e Lts.z > 0.06): no terreno virado pra longe do sol -
            // justamente o que esta' na sombra - nunca rodava, e fillSelfShadow
            // ficava 1.0. O relevo do chao SUMIA ao entrar na sombra. model.frag
            // foi corrigido em 2026-09-05 e o terreno ficou pra tras; agora roda
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
        if (inBlendWeight > 0.0 && inBlendPbrIndex != 0u) {
            uint bpIdx = nonuniformEXT(inBlendPbrIndex);
            vec4 bMrahw = texture(u_textures[bpIdx], uv);
            mrahw = mix(mrahw, bMrahw, blendT);
        }
        roughness = roughnessScale * mrahw.g;
        metallic  = metallicScale * mrahw.r;
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
    // Floor at 0.02: the lighting passes treat albedo.a < 0.001 as an empty
    // G-buffer pixel and early-out to black.
    {   // min() entre os dois termos de cavidade - ver model.frag.
        uint aoMask = uint(u_pomParams.y + 0.5);
        aoTerm *= ((aoMask & 16u) != 0u) ? (aoCavityNormal * aoCavityBaked)
                                         : min(aoCavityNormal, aoCavityBaked);
    }
    outAlbedo.a = max(aoTerm, 0.02);

    // Layout: R=source flag (1.0 real, 0.5 fallback), G=roughness, B=metallic, A=wetness.
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
    outEmissive = vec4(0.0, 0.0, 0.0, sunSelfShadow);
}
