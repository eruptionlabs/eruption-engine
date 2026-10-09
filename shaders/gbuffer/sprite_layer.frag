#version 450
#extension GL_EXT_nonuniform_qualifier : enable
// CAMADA DE SPRITES (FSR): o sprite redesenhado na resolucao de DISPLAY, por
// cima da saida do FSR (HDR linear, antes do pos).
//
// Cor = textura nitida do sprite x luz. A luz NAO e' recalculada aqui: o
// sprite continua no G-buffer e o deferred o ilumina como sempre (sol com
// sombra, luzes pontuais, ambiente). Este shader so' le' essa iluminacao na
// resolucao de render como razao  iluminado / albedo  e a aplica sobre o
// albedo da resolucao de display. Luz varia devagar sobre o sprite; o detalhe
// de pixel art vem da textura - e e' isso que o FSR borraria.
//
// Oclusao: o depth do G-buffer (render) ja' contem o sprite e tudo na frente
// dele. Fragmento cujo depth linear passa do depth do G-buffer naquele ponto
// esta' atras de algo (arvore, casa, outro sprite) e e' descartado.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 3) in vec4 v_tint;
layout(location = 6) in flat uint v_paletteIndex;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];
layout(set = 2, binding = 0) uniform sampler2D u_depth;   // depth do G-buffer (render)
layout(set = 2, binding = 1) uniform sampler2D u_lit;     // imagem iluminada (render, HDR linear)
layout(set = 2, binding = 2) uniform sampler2D u_albedo;  // albedo do G-buffer (render)

layout(push_constant) uniform LayerPush {
    mat4 p_view;
    mat4 p_proj;
    vec4 p_sizes;   // xy = resolucao de render, zw = resolucao de display
    vec4 p_planes;  // x = near, y = far
};

layout(location = 0) out vec4 o_color;

// O G-buffer guarda o albedo em sRGB e o deferred o converte para linear antes
// de iluminar (directional/ambient/point_light). A razao de luz tem que ser
// feita no MESMO espaco, senao ela passa a depender do texel amostrado - e o
// texel amostrado em render muda com o jitter (o sprite piscava).
vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

float linearDepth(float d) {
    const float n = p_planes.x, f = p_planes.y;
    return n * f / (f - d * (f - n));
}

void main() {
    vec4 albedo = texture(nonuniformEXT(u_textures[v_texIndex]), v_uv);
    if ((v_flags & 256u) != 0) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        float u = (idx + 0.5) / 256.0;
        albedo = texture(nonuniformEXT(u_textures[v_paletteIndex]), vec2(u, 0.5));
        albedo.a = 1.0;
    }
    if (albedo.a < 0.01) discard;
    albedo.rgb *= v_tint.rgb;

    const vec2 scale = p_sizes.xy / p_sizes.zw;
    const ivec2 renderSize = ivec2(p_sizes.xy);
    const ivec2 center = clamp(ivec2(gl_FragCoord.xy * scale), ivec2(0), renderSize - 1);
    const float zFrag = linearDepth(gl_FragCoord.z);

    // Tolerancia relativa: cobre a diferenca de 1 pixel de render entre o
    // depth com jitter (G-buffer) e este, sem jitter.
    const float tol = 0.01 * zFrag + 0.05;
    if (zFrag > linearDepth(texelFetch(u_depth, center, 0).r) + tol) discard;

    // Luz como funcao do albedo LINEAR na vizinhanca 3x3 que E' este sprite
    // (depth igual): iluminado = albedo * D + S. D e' a parte que escala com o
    // albedo (difuso de sol, ambiente, luzes pontuais); S e' o que se soma sem
    // depender dele (especular dieletrico, ceu refletido). Ajuste por minimos
    // quadrados ponderados (peso em tenda pela distancia ao ponto exato), com
    // regularizacao para a razao pura quando o albedo da vizinhanca e' quase
    // uniforme. Tudo continuo: a amostra de render anda com o jitter a cada
    // frame, e qualquer escolha discreta de vizinho fazia o sprite piscar.
    const vec2 pos = gl_FragCoord.xy * scale - 0.5;   // em pixels de render
    float W = 0.0;
    vec3 sa = vec3(0.0), sl = vec3(0.0), saa = vec3(0.0), sal = vec3(0.0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const ivec2 p = clamp(center + ivec2(dx, dy), ivec2(0), renderSize - 1);
            if (abs(linearDepth(texelFetch(u_depth, p, 0).r) - zFrag) > tol) continue;
            const vec2 d = abs(vec2(p) - pos);
            const float w = max(0.0, 1.5 - d.x) * max(0.0, 1.5 - d.y);
            const vec3 a = srgbToLinear(texelFetch(u_albedo, p, 0).rgb);
            const vec3 l = texelFetch(u_lit, p, 0).rgb;
            W += w; sa += w * a; sl += w * l; saa += w * a * a; sal += w * a * l;
        }
    }
    vec3 D, S;
    if (W <= 1e-4) {
        // Nenhum pixel de render deste sprite por perto (detalhe mais fino que
        // um pixel de render): razao do ponto como esta'.
        D = texelFetch(u_lit, center, 0).rgb /
            max(srgbToLinear(texelFetch(u_albedo, center, 0).rgb), vec3(0.002));
        S = vec3(0.0);
    } else {
        const vec3 ma = sa / W, ml = sl / W;
        const vec3 varA = max(saa / W - ma * ma, vec3(0.0));
        const vec3 cov = sal / W - ma * ml;
        const vec3 ratio = ml / max(ma, vec3(0.002));
        const float mu = 2e-3;   // variancia de albedo abaixo disso = "uniforme"
        D = max((cov + mu * ratio) / (varA + mu), vec3(0.0));
        S = max(ml - D * ma, vec3(0.0));
    }
    o_color = vec4(srgbToLinear(albedo.rgb) * D + S, 1.0);
}
