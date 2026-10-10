#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// VISUALIZADOR DE MODELO do editor (src/editor/ModelPreview.cpp). O vértice
// é o model_novel.vert de verdade (mesma malha, mesmo LOD, mesmas texturas);
// este fragment ilumina com luz de estúdio própria, que não depende do mapa,
// da hora nem do clima - o mesmo modelo sai sempre igual.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in flat uint inTexIndex;
layout(location = 4) in flat uint inMatId;
layout(location = 5) in vec4 inColor;
layout(location = 6) in flat uint inPbrIndex;
layout(location = 7) in flat uint inNormalIndex;
// O resto do que o model_novel.vert entrega (declarado para casar as saídas).
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

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
    float alpha;
    float metallicScale;
    float roughnessScale;
    float _pad;
    vec4 uvTranslateRot; // o vértice não lê: aqui leva a posição do olho (xyz)
    vec4 uvScale;
    vec4 lightMode;  // xyz: direção da luz (para onde ela vai), w: modo
    vec4 wireCut;    // rgb: cor do aramado, a: corte de alfa (0 = sem corte)
} push;

const vec3 kKeyColor = vec3(1.05, 1.0, 0.94) * 0.95;
const vec3 kFillColor = vec3(0.55, 0.62, 0.75) * 0.25;
const vec3 kSky = vec3(0.42, 0.48, 0.58);
const vec3 kGround = vec3(0.16, 0.14, 0.12);

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
    const int mode = int(push.lightMode.w + 0.5);
    vec4 tex = vec4(0.8, 0.8, 0.8, 1.0);
    if (inTexIndex != 0u) tex = texture(u_textures[nonuniformEXT(inTexIndex)], inTexCoord);
    if (mode == 4) { // aramado: cor sólida
        outColor = vec4(push.wireCut.rgb, 1.0);
        return;
    }
    if (push.wireCut.a > 0.0 && tex.a < push.wireCut.a) discard; // cartões de folha
    vec3 N = normalize(inNormal);
    if (!gl_FrontFacing) N = -N; // os dois lados (folhas)
    if (inNormalIndex != 0u) {
        vec3 tn = texture(u_textures[nonuniformEXT(inNormalIndex)], inTexCoord).xyz * 2.0 - 1.0;
        N = normalize(tbnFromDerivatives(N, inWorldPos, inTexCoord) * normalize(tn));
    }
    if (mode == 2) { // normais
        outColor = vec4(N * 0.5 + 0.5, 1.0);
        return;
    }
    if (mode == 3) { // UV em xadrez
        vec2 c = floor(fract(inTexCoord) * 8.0);
        float k = mod(c.x + c.y, 2.0);
        outColor = vec4(mix(vec3(0.25), vec3(0.85), k) * vec3(fract(inTexCoord), 1.0) * 1.2, 1.0);
        return;
    }
    vec3 albedo = tex.rgb * inColor.rgb;
    if (mode == 1) { // só a cor (as texturas já estão na curva de exibição)
        outColor = vec4(albedo, 1.0);
        return;
    }
    float metallic = 0.0, rough = 0.8, ao = 1.0;
    if (inPbrIndex != 0u) {
        vec4 m = texture(u_textures[nonuniformEXT(inPbrIndex)], inTexCoord);
        metallic = clamp(m.r * push.metallicScale, 0.0, 1.0);
        rough = clamp(m.g * push.roughnessScale, 0.04, 1.0);
        ao = m.b;
    }
    const vec3 L = normalize(-push.lightMode.xyz);
    const vec3 Lf = normalize(vec3(-L.x, 0.35, -L.z)); // preenchimento do lado oposto
    const vec3 V = normalize(push.uvTranslateRot.xyz - inWorldPos);
    const float ndl = max(dot(N, L), 0.0);
    const float ndf = max(dot(N, Lf), 0.0);
    const vec3 H = normalize(L + V);
    const float shininess = mix(128.0, 6.0, rough);
    const vec3 f0 = mix(vec3(0.04), albedo, metallic);
    const vec3 spec = f0 * pow(max(dot(N, H), 0.0), shininess) * (shininess + 8.0) / 25.0 * ndl;
    const vec3 hemi = mix(kGround, kSky, N.y * 0.5 + 0.5);
    const vec3 diffuse = albedo * (1.0 - metallic);
    vec3 lit = diffuse * (hemi * ao + kKeyColor * ndl + kFillColor * ndf) + spec * kKeyColor;
    // A imagem vai direto para a interface: curva suave (Reinhard) e gama, como
    // a cena faz no pós - sem isso a luz linear aparece escura.
    lit = lit * 1.6 / (1.0 + lit * 1.6);
    outColor = vec4(pow(lit, vec3(1.0 / 2.2)), 1.0);
}
