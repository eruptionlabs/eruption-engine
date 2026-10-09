#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// FRAGMENT DEDICADO DO POPUP DE PREVIEW (src/debug/SpherePreview.*). NAO e'
// model.frag: aquele escreve no G-buffer (5 anexos) pra' passar pelo passe
// de iluminacao diferida da cena principal - reusar ele aqui exigiria um
// G-buffer proprio pequeno MAIS o passe de composicao inteiro so' pra'
// mostrar uma esfera de teste. Este e' direto: amostra o albedo e ilumina
// com luz difusa simples (sol + ambiente) na hora, um so' anexo de saida.
// model.vert/tesc/tese continuam os DE VERDADE, sem copia - o deslocamento
// que aparece aqui e' byte a byte o mesmo que o chao real calcula.

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
layout(location = 12) in flat uint inBlendMaskIndex;
layout(location = 13) in vec2 inBlendMaskUV;
layout(location = 14) in float inEmissiveStrength;
layout(location = 15) in flat uvec2 inSplatTex;
layout(location = 16) in flat uvec2 inSplatTex2;
layout(location = 17) in flat uint inBlendMaskIndex2;
layout(location = 18) in flat float inSway;
layout(location = 19) in vec4 inDispInfo;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
    float alpha;
    float metallicScale;
    float roughnessScale;
    float _pad;
    vec4 uvTranslateRot;
    vec4 uvScale;
} push;

// LUZ PROPRIA DO POPUP, FIXA - nao le' u_sunDir/u_sunColor/u_ambientSky/
// u_ambientGround do FrameUBO da cena principal de proposito (autor: "o
// preview deve ter uma iluminacao propria"). Se lesse o sol de verdade, o
// preview escurecia ou mudava de cor sozinho seguindo o ciclo dia/noite e o
// clima do mapa aberto atras dele - o material pareceria diferente cada vez
// que o popup fosse aberto, sem nenhuma mudanca na textura. Constantes fixas
// tipo "estudio" abaixo: sempre o mesmo resultado pra' mesma textura.
const vec3 kLightDir    = vec3(-0.4082483, -0.8164966, -0.4082483); // normalizado, de cima-frente
// 0.85 (era 1.6): com 1.6, textura clara (chao de pedra branca) estourava
// pra' branco chapado no topo do cubo - ambiente ~0.26 + sol ~1.3 > 1.
const vec3 kLightColor  = vec3(1.05, 1.0, 0.92) * 0.85;
const vec3 kAmbientSky    = vec3(0.42, 0.48, 0.58);
const vec3 kAmbientGround = vec3(0.14, 0.12, 0.10);

// Base tangente pelas derivadas de tela (mesma construcao do model.frag -
// computeTBN), pra' o normal map cair na mesma orientacao que no mundo.
mat3 tbnFromDerivatives(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    if (dot(T, T) < 1e-16 || dot(B, B) < 1e-16) {
        vec3 up = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        T = normalize(cross(up, N));
        return mat3(T, normalize(cross(N, T)), N);
    }
    T = normalize(T - N * dot(N, T));
    vec3 Bo = cross(N, T);
    B = (dot(Bo, B) < 0.0) ? -Bo : Bo;
    return mat3(T, B, N);
}

void main() {
    vec3 N = normalize(inNormal);
    // NORMAL MAP, como no mundo. Sem isto o popup so' mostrava o relevo da
    // GEOMETRIA deslocada - no jogo o normal map tambem entra na luz, entao o
    // preview saia chapado com a MESMA textura que no mundo tinha relevo.
    // Forca 0.35: o mesmo recuo que o model.frag aplica na banda perto (o
    // relevo grosso ja' esta' na geometria, o normal map so' completa).
    if (inNormalIndex != 0u) {
        vec3 tn = texture(u_textures[nonuniformEXT(inNormalIndex)], inTexCoord).xyz * 2.0 - 1.0;
        tn = mix(vec3(0.0, 0.0, 1.0), tn, 0.35);
        N = normalize(tbnFromDerivatives(N, inWorldPos, inTexCoord) * normalize(tn));
    }
    vec3 albedo = vec3(0.8);
    if (inTexIndex != 0u) {
        albedo = texture(u_textures[nonuniformEXT(inTexIndex)], inTexCoord).rgb;
    }
    vec3 L = normalize(-kLightDir);
    float ndl = max(dot(N, L), 0.0);
    vec3 ambient = kAmbientSky * 0.55 + kAmbientGround * 0.20;
    vec3 lit = albedo * (ambient + kLightColor * ndl);
    outColor = vec4(lit, 1.0);
}
