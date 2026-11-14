#version 450
// Reflexo espelhado da agua HD-2D (WaterReflectionPass): acende o G-buffer
// desenhado pela camera espelhada no plano d'agua. Luz simples - sol N.L sem
// sombra, ambiente hemisferico com o AO do G-buffer, emissivo -, na mesma
// escala linear da imagem acesa da cena, que e' de onde o SSPR tira a cor.
// Saida alinhada a' tela da camera principal: o G-buffer foi desenhado com o
// x do recorte negado (para o espelho nao inverter a ordem dos vertices), entao
// a coluna c da saida le' a coluna W-1-c. Alfa = cobertura (0 onde nada foi
// desenhado: la' a agua cai no ceu).
// NUVENS: o passe da agua roda antes do passe das nuvens, entao nem o SSPR
// nem este G-buffer tinham nuvem - o lago refletia so' o ceu limpo. As
// nuvens de verdade (as mesmas bolhas, o mesmo ray-march do cloud_billboard)
// sao desenhadas pela camera espelhada num acumulo pre-multiplicado de baixa
// resolucao (CloudLayerRenderer::beginMirrorView) e entram aqui por cima da
// cena espelhada; onde so' ha' nuvem, a cobertura e' o alfa dela.

layout(location = 0) in vec2 inUV; // do fullscreen.vert; a leitura e' por gl_FragCoord
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gAlbedo;   // rgb sRGB, a = AO (0 = vazio)
layout(set = 0, binding = 1) uniform sampler2D gNormal;   // rgb = N*0,5+0,5
layout(set = 0, binding = 2) uniform sampler2D gEmissive; // rgb sRGB
layout(set = 0, binding = 3) uniform sampler2D cloudAccum; // rgb*a, a (tambem espelhado em x)

layout(push_constant) uniform Push {
    vec4 toSun;         // xyz = direcao ate' o sol, w = intensidade
    vec4 sunColor;      // rgb, w = escala do difuso direto
    vec4 ambientSky;    // rgb = zenite, w = intensidade do GI
    vec4 ambientGround; // rgb, w = piso do hemisferio
    vec4 skyHorizon;    // rgb, w = escala do emissivo
    vec4 clouds;        // x = 1 com nuvens no binding 3 (0 = fallback, nao le')
} pc;

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    ivec2 size = textureSize(gAlbedo, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 src = ivec2(size.x - 1 - p.x, p.y);
    vec4 cloud = vec4(0.0);
    if (pc.clouds.x > 0.5) {
        cloud = textureLod(cloudAccum, (vec2(src) + 0.5) / vec2(size), 0.0);
        cloud = clamp(cloud, vec4(0.0), vec4(100.0, 100.0, 100.0, 1.0));
    }
    vec4 albedo = texelFetch(gAlbedo, src, 0);
    if (albedo.a <= 0.0) {
        // So' nuvem (ou nada): cor sem a pre-multiplicacao, alfa = cobertura.
        outColor = cloud.a > 0.003 ? vec4(cloud.rgb / cloud.a, cloud.a) : vec4(0.0);
        return;
    }

    vec3 N = texelFetch(gNormal, src, 0).rgb * 2.0 - 1.0;
    float nl = length(N);
    N = nl > 1e-4 ? N / nl : vec3(0.0, 1.0, 0.0);
    vec3 base = srgbToLinear(albedo.rgb);

    // Ambiente: o mesmo hemisferio do ambient.frag (sem sonda de ceu e sem SSAO).
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 skyIrr = mix(pc.skyHorizon.rgb, pc.ambientSky.rgb, up * up);
    float hemi = pc.ambientGround.w + (1.0 - pc.ambientGround.w) * up;
    vec3 amb = mix(pc.ambientGround.rgb, skyIrr, hemi) * pc.ambientSky.w;
    float ao = pow(clamp(albedo.a, 0.0, 1.0), 1.35) * 1.12;

    // Sol: difuso de Lambert, sem sombra.
    float NoL = max(dot(N, normalize(pc.toSun.xyz)), 0.0);
    vec3 sun = pc.sunColor.rgb * pc.toSun.w * NoL * pc.sunColor.w;

    vec3 emi = srgbToLinear(texelFetch(gEmissive, src, 0).rgb) * pc.skyHorizon.w;
    vec3 c = base * (amb * ao + sun) + emi;
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    // A nuvem na frente do que o espelho desenhou (o ray-march ja' cortou a
    // que fica atras, pela profundidade deste G-buffer).
    c = cloud.rgb + clamp(c, 0.0, 100.0) * (1.0 - cloud.a);
    outColor = vec4(c, 1.0);
}
