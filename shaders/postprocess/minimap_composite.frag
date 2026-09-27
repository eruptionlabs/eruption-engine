#version 450

// Minimap composite (IGNIS, G10/G11).
//
// The minimap used to be produced by copying the G-buffer ALBEDO image
// straight into the minimap texture (Engine::renderMinimap). Albedo is the
// raw surface colour BEFORE any lighting, so the minimap showed:
//   - no shadow at all (shadow is born in the deferred lighting pass, which
//     never runs for the minimap camera), and
//   - no water (water is a forward pass drawn after lighting, into the lit
//     colour target -- it never touches the G-buffer the minimap copies).
//
// This pass replaces that blind copy. It relights the minimap G-buffer with
// the sun, casts a height-field shadow, and paints water from terrain height.
// It runs ON DEMAND only (the minimap is regenerated when the map or the
// instance/chunk count changes, per commit 1f8c4a5), so the cost is paid once
// per map load, not per frame.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gAlbedo;  // rgb = albedo, a = AO
layout(set = 0, binding = 1) uniform sampler2D gNormal;  // rgb = N*0.5+0.5
// O DEPTH NAO E' MAIS AMOSTRADO AQUI.
//
// Ele saiu por dois motivos que se somam. Primeiro, deixou de ser necessario:
// presenca de geometria vem do alpha do albedo e posicao vertical vem do canal
// de altura, os dois confiaveis. Segundo, ele era ATIVAMENTE NOCIVO: amostrar
// o depth exigia transiciona-lo de layout, e essa transicao gerava 40 erros de
// validacao por execucao (VkDescriptorImageInfo-imageLayout-00344, medido por
// A/B) porque o passe de fumaça deixa a imagem em DEPTH_READ_ONLY_OPTIMAL e a
// minha barreira assumia DEPTH_ATTACHMENT_OPTIMAL. Transicao com oldLayout
// errado deixa o conteudo INDEFINIDO - o que explica por que o depth lido aqui
// vinha no valor de limpeza. Sem amostrar, o problema deixa de existir.
layout(set = 0, binding = 3) uniform usampler2D gHeight;

layout(push_constant) uniform Push {
    mat4 invViewProj;   // minimap ortho VP, inverted
    vec4 sunDir;        // xyz = direction TOWARD the sun (normalized), w = sun intensity
    vec4 sunColor;      // rgb = sun colour, a = ambient intensity
    vec4 params;        // x = water level (world Y), y = shadow strength,
                        // z = water opacity, w = shadow ray length (world units)
    vec4 waterColor;    // rgb = deep water tint, a = shoreline fade depth (world units)
    vec4 uvScale;       // xy = minimap sub-rect / G-buffer size, zw = unused
} push;

// The minimap is drawn into the TOP-LEFT MINIMAP_RES x MINIMAP_RES corner of the
// full-size scene G-buffer (renderMinimap sets a 256x256 viewport on a e.g.
// 1280x1024 attachment). So there are two different UV spaces here and mixing
// them up samples the wrong pixels entirely:
//   inUV          0..1 across the minimap OUTPUT image, and the ortho viewport,
//                 so this is what NDC / world reconstruction must use;
//   inUV*uvScale  where that same point actually lives in the G-buffer.
vec2 gbufUV(vec2 uv) { return uv * push.uvScale.xy; }

// uv here is minimap-space (0..1 over the ortho viewport), NOT G-buffer space.
// A projecao do minimapa e' ORTOGRAFICA, logo o mapeamento de NDC para mundo e'
// AFIM: o XZ resultante nao depende de z. Por isso a profundidade passada aqui
// e' uma constante arbitraria - so' o plano XZ e' consumido.
vec3 worldFromNdc(vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    vec4 wp = push.invViewProj * ndc;
    if (abs(wp.w) < 1e-6) return vec3(0.0);
    return wp.xyz / wp.w;
}

void main() {
    ivec2 dsz = textureSize(gHeight, 0);
    ivec2 dxy = clamp(ivec2(gbufUV(inUV) * vec2(dsz)), ivec2(0), dsz - 1);
    vec4 albedo = texture(gAlbedo, gbufUV(inUV));

    // Nothing was rasterized here: outside the map, or a hole in the terrain.
    // Transparent so the UI underneath shows through instead of a black square.
    // PRESENCA DE GEOMETRIA vem do ALBEDO, nao do depth.
    //
    // Medido neste mapa: 50.912 pixels tem albedo escrito e profundidade ainda
    // no valor de limpeza (1.0), contra 9.566 com profundidade real. Ou seja o
    // passe do minimapa escreve COR sem escrever PROFUNDIDADE na maior parte da
    // area, apesar de o pipeline declarar depthWrite=true. Usar depth como
    // teste de "tem geometria aqui" apagava ~55% do mapa - era a causa da
    // faixa preta. O alpha do albedo (termo de AO, com piso 0.02 quando
    // escrito e 0 na limpeza) e' o sinal confiavel.
    //
    // A causa raiz da profundidade nao escrita esta' registrada como meta
    // aberta no PROTOCOLO.cai; nao e' do minimapa em si.
    if (albedo.a < 0.001) { outColor = vec4(0.0); return; }

    // Onde a profundidade NAO foi escrita nao ha' posicao de mundo confiavel,
    // entao tudo que depende dela (agua por altura, sombra por campo de
    // altura) precisa se abster ali em vez de chutar. Sem esta porta, o Y
    // reconstruido a partir de depth=1.0 cai para milhares de unidades abaixo
    // do nivel de agua e o mapa inteiro era pintado de azul.

    // Altura vinda do G-buffer: definida em TODO pixel que tem geometria, ao
    // contrario do depth. Desquantiza a faixa [-500, 1500].
    float worldY = float(texelFetch(gHeight, dxy, 0).r) / 255.0 * 2000.0 - 500.0;


    vec3 N = normalize(texture(gNormal, gbufUV(inUV)).rgb * 2.0 - 1.0);
    if (any(isnan(N)) || any(isinf(N))) N = vec3(0.0, 1.0, 0.0);

    // A minimap has to stay LEGIBLE at every hour. Relighting it with the live
    // sun means it goes black at night and near-black under a storm, which is a
    // usability regression against the old raw-albedo blit -- ugly, but always
    // readable. So the sun sets the DIRECTION of the relief and the tint, while
    // the exposure is held stable by the floors below.
    //
    // When the sun is at or below the horizon its direction is useless for
    // relief (everything faces away from it), so fall back to a fixed synthetic
    // key from the north-west, the cartographic convention for hill shading.
    vec3 L = normalize(push.sunDir.xyz);
    if (L.y < 0.15) {
        L = normalize(vec3(-0.5, 0.7, -0.5));
    }
    float ao = albedo.a;

    // --- Height-field cast shadow -----------------------------------------
    // The minimap is a fixed orthographic top-down render, so the depth buffer
    // IS a height field of the whole map and a screen-space march along the sun
    // direction gives real cast shadows -- buildings and cliffs drop shadows
    // onto the terrain. This is only sound BECAUSE the projection is ortho and
    // axis-aligned; do not copy it to the perspective camera.
    //
    // The sun-facing world offset has to become a UV offset. Rather than pass
    // the forward matrix too (push constants are already at the 128-byte
    // limit), recover the linear world->UV basis from invViewProj itself: for
    // an orthographic projection the mapping is affine, so two finite
    // differences at the centre describe it exactly.
    float shadow = 0.0;
    if (push.params.y > 0.001 && L.y > 0.05) {
        vec3 originW = worldFromNdc(vec2(0.5, 0.5));
        vec3 duW = worldFromNdc(vec2(0.5 + 0.25, 0.5)) - originW; // mundo por +0.25 u
        vec3 dvW = worldFromNdc(vec2(0.5, 0.5 + 0.25)) - originW; // mundo por +0.25 v

        // Invert the 2x2 world-XZ <- UV matrix to get UV per world XZ.
        float a = duW.x, b = dvW.x, c = duW.z, d = dvW.z;
        float det = a * d - b * c;
        if (abs(det) > 1e-9) {
            float rayLen = max(push.params.w, 1.0);
            const int STEPS = 24;
            float stepLen = rayLen / float(STEPS);

            // Dither the ray start so the finite step count reads as soft
            // gradient instead of 24 hard terraces.
            float jitter = fract(sin(dot(inUV, vec2(12.9898, 78.233))) * 43758.5453);

            float occ = 0.0;
            for (int i = 1; i <= STEPS; ++i) {
                float t = (float(i) - 1.0 + jitter) * stepLen;
                vec2 offW = L.xz * t;
                // Solve [a b; c d] * (du,dv) = offW, then scale back from the
                // 0.25-sized finite differences to real UV units.
                vec2 duv = vec2(( d * offW.x - b * offW.y),
                                (-c * offW.x + a * offW.y)) / det * 0.25;
                vec2 sampleUV = inUV + duv;
                if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) break;

                ivec2 sxy = clamp(ivec2(gbufUV(sampleUV) * vec2(dsz)), ivec2(0), dsz - 1);
                float terrainY = float(texelFetch(gHeight, sxy, 0).r) / 255.0 * 2000.0 - 500.0;
                float rayY = worldY + L.y * t;

                // Occluded when terrain rises above the ray. The thickness
                // window keeps a tall spike from shadowing the whole map.
                float above = terrainY - rayY;
                if (above > 0.0 && above < rayLen) {
                    occ = max(occ, smoothstep(0.0, 2.0, above));
                }
            }
            shadow = occ * push.params.y;
        }
    }

    // --- Relight ----------------------------------------------------------
    // Deliberately simple: a lambert sun term plus a hemispheric ambient. The
    // minimap wants readable relief, not a second full PBR evaluation.
    // Key light: relief and cast shadow, on a fixed budget. The sun's colour
    // still tints it (a sunset minimap reads warm) but its INTENSITY does not
    // set the exposure, so the map never fades out.
    float NdotL = max(dot(N, L), 0.0);
    vec3 sunTint = mix(vec3(1.0), push.sunColor.rgb, 0.6);
    vec3 key = sunTint * (0.55 * NdotL * (1.0 - shadow));

    // Fill: hemispheric, floored. This is what guarantees a readable map at
    // midnight; the floor is what the old blit effectively had for free.
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 fill = mix(vec3(0.42, 0.44, 0.50), vec3(0.66, 0.70, 0.78), up) * 0.75;

    vec3 color = albedo.rgb * (key + fill * mix(1.0, ao, 0.7));

    // --- Water ------------------------------------------------------------
    // Terrain below the map's water level is underwater. Reconstructing it from
    // height needs no water geometry and no WaterRenderer involvement, and it
    // covers the map's full extent, which is exactly what a minimap wants.
    float waterDepth = push.params.x - worldY;
    if (waterDepth > 0.0 && push.params.z > 0.001) {
        // Shallow edges stay readable so the shoreline shape survives; deep
        // water saturates toward the tint.
        float fade = clamp(waterDepth / max(push.waterColor.a, 0.1), 0.0, 1.0);
        float blend = push.params.z * mix(0.35, 1.0, fade);
        color = mix(color, push.waterColor.rgb, blend);
        // A touch of the sky on the surface keeps deep water from reading as a
        // flat dead hole.
        color += push.waterColor.rgb * 0.12 * (1.0 - fade);
    }

    outColor = vec4(color, 1.0);
}
