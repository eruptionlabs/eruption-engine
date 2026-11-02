// Cena do navegador em WebGL2 (spec 10, marco 1): chao com splat de cor,
// malhas do mapa e sprites dos personagens. Desenha com os dados que o motor
// ja' tem: malhas e texturas chegam pelo espelho do dispositivo Vulkan nulo
// (platform/web/WebMirror.hpp), pelos mesmos handles que o renderer Vulkan
// guarda; instancias, sprites, camera e sol vem do motor como no desktop.
// Marco 2: normal map e luz PBR (sol + ambiente hemisferico). Sem sombra,
// pos e ceu procedural por ora.
#if defined(__EMSCRIPTEN__)

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "renderer/SkyProbe.hpp"
#include "platform/web/WebMirror.hpp"
#include "platform/web/WebVfxSim.hpp"
#include "WebGles.hpp" // shaders do desktop em GLSL ES (gerado no build, tools/web/embed_gles.py)

#include <GLES3/gl3.h>
#include <webgl/webgl2.h> // glGetBufferSubData (leitura da sonda sem parar o quadro)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace eruption {

namespace {

const char* kModelVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in vec2 aMaskUv;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform vec4 uUvXform; // escala.xy, deslocamento.zw (animacao de UV da instancia)
out vec2 vUv;
out vec2 vMaskUv;
out vec3 vNormal;
out vec3 vWorld;
void main() {
    vUv = aUv * uUvXform.xy + uUvXform.zw;
    vMaskUv = aMaskUv;
    vNormal = mat3(uModel) * aNormal;
    vec4 w = uModel * vec4(aPos, 1.0);
    vWorld = w.xyz;
    gl_Position = uViewProj * w;
}
)";

// Splat como no model_frag.glsl: as duas camadas que mais pesam na mascara,
// a base fecha o peso que sobra, media renormalizada. Cor, normal e MRAH-W
// das camadas vem de tres texture arrays (WebSceneGl::Splat): 5 camadas com
// os tres mapas sao 15 texturas, e o WebGL2 garante so' 16 unidades.
// Luz como o directional.frag com pbrLighting: difusa sem 1/pi (a escala do
// sol do motor), especular GGX/Smith/Schlick e ambiente hemisferico
// (chao -> horizonte -> zenite), com a cavidade do MRAH-W.
const char* kModelFs = R"(#version 300 es
precision highp float;
precision highp sampler2DArray;
in vec2 vUv;
in vec2 vMaskUv;
in vec3 vNormal;
in vec3 vWorld;
uniform sampler2D uBase;
uniform sampler2D uBaseN;  // normal map do material (unidade 4)
uniform sampler2D uBaseM;  // MRAH-W do material (unidade 5)
uniform sampler2D uMask0;
uniform sampler2D uMask1;
uniform sampler2DArray uLayC, uLayN, uLayM;
uniform vec4 uIdx0, uIdx1;   // camada do array de cada camada do splat (-1 = sem)
uniform vec4 uHasN0, uHasN1; // a camada tem normal
uniform vec4 uHasM0, uHasM1; // a camada tem MRAH-W
uniform int uSplat;          // 0 = sem splat, 1 = uma mascara, 2 = duas
uniform vec2 uHasBase;       // x = normal do material, y = MRAH-W do material
uniform vec4 uMat;           // metallicScale, roughnessScale, rugosidade padrao, inverte Y da normal
uniform vec3 uCam;
uniform vec3 uSunL;  // para onde esta' o sol
uniform vec3 uSun;   // cor x intensidade
uniform vec3 uSky, uHorizon, uGround; // ambiente hemisferico, ja' com a intensidade
uniform float uHemiFloor;
uniform vec3 uSH[9];      // sonda de ceu (SH9 de radiancia, como o SkySH do desktop)
uniform vec4 uProbe;      // x = sonda pronta, z = intensidade x GI
// pbrMultiScatterCompensation do desktop (Karis analitico, Kulla-Conty).
vec3 multiScatter(vec3 F0, float rough, float NdV) {
    vec4 r = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * NdV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return vec3(1.0) + F0 * (1.0 / clamp(ab.x + ab.y, 1e-3, 1.0) - 1.0);
}
vec3 shIrradiance(vec3 n) {
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708;
    float x = n.x, y = n.y, z = n.z;
    return c1 * uSH[8] * (x * x - y * y) + c3 * uSH[6] * z * z + c4 * uSH[0] - c5 * uSH[6]
         + 2.0 * c1 * (uSH[4] * x * y + uSH[7] * x * z + uSH[5] * y * z)
         + 2.0 * c2 * (uSH[3] * x + uSH[1] * y + uSH[2] * z);
}
uniform float uLinear; // 1 = sai linear (HDR) para o pos; 0 = sRGB direto na tela
// Relevo pela distancia (model_frag.glsl): forca e vies de mip do normal map
// por tabela de 8 amostras, e a atenuacao na banda tesselada.
uniform vec4 uNd;        // x = distancia maxima, y = curva ligada, z = forca base, w = vies base
uniform vec4 uNdScale0, uNdScale1, uNdSmooth0, uNdSmooth1;
uniform vec4 uTess;      // x = distancia maxima, y = ligada, w = nivel maximo
uniform vec4 uTessLut0, uTessLut1;
uniform float uQuebra;   // 1 = amostra sem ladrilho no splat (0 no shader_lite, como o LITE do desktop)
// Luzes pontuais do mapa e dos efeitos, as mais perto da camera
// (point_light.frag: atenuacao "cuspid", Cook-Torrance x pbrLightScale).
const int kMaxLuzes = 8;
uniform int uNumLuzes;
uniform vec4 uLuzPos[kMaxLuzes]; // xyz, raio
uniform vec4 uLuzCor[kMaxLuzes]; // cor x intensidade x escala
out vec4 fragColor;
void topDuas(int i, float p, inout int s0, inout float p0, inout int s1, inout float p1) {
    if (p <= 0.0) return;
    if (p > p0) { s1 = s0; p1 = p0; s0 = i; p0 = p; }
    else if (p > p1) { s1 = i; p1 = p; }
}
float pega(vec4 a, vec4 b, int i) { return i < 4 ? a[i] : b[i - 4]; }
// Amostra sem ladrilho do chao com splat (model_frag.glsl: gradeTriangular,
// sorteioDoVertice, amostraSemGrade): tres amostras deslocadas por vertice de
// uma grade triangular. Na cor e no MRAH-W o desvio volta ao da imagem; na
// normal nao, e a media das tres achata o relevo como no desktop.
float lut8(vec4 a, vec4 b, float t) {
    float x = clamp(t, 0.0, 1.0) * 7.0;
    int i = int(floor(x));
    float f = x - float(i);
    float v0 = i < 4 ? a[i] : b[i - 4];
    int j = min(i + 1, 7);
    float v1 = j < 4 ? a[j] : b[j - 4];
    return mix(v0, v1, f);
}
void gradeTriangular(vec2 uv, out vec3 w, out vec2 v1, out vec2 v2, out vec2 v3) {
    const mat2 paraObliquo = mat2(1.0, 0.0, -0.57735027, 1.15470054);
    vec2 obl = paraObliquo * (uv * 3.4641016);
    vec2 base = floor(obl);
    vec3 t = vec3(fract(obl), 0.0);
    t.z = 1.0 - t.x - t.y;
    if (t.z > 0.0) {
        w = vec3(t.z, t.y, t.x);
        v1 = base; v2 = base + vec2(0.0, 1.0); v3 = base + vec2(1.0, 0.0);
    } else {
        w = vec3(-t.z, 1.0 - t.y, 1.0 - t.x);
        v1 = base + vec2(1.0, 1.0); v2 = base + vec2(1.0, 0.0); v3 = base + vec2(0.0, 1.0);
    }
}
vec2 sorteioDoVertice(vec2 p) {
    return fract(sin(vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)))) * 43758.5453);
}
vec4 semGrade(sampler2DArray t, float l, vec2 uv, vec2 dx, vec2 dy, bool restaura) {
    if (uQuebra < 0.5) return textureGrad(t, vec3(uv, l), dx, dy);
    vec3 w; vec2 v1, v2, v3;
    gradeTriangular(uv, w, v1, v2, v3);
    vec4 c = w.x * textureGrad(t, vec3(uv + sorteioDoVertice(v1), l), dx, dy)
           + w.y * textureGrad(t, vec3(uv + sorteioDoVertice(v2), l), dx, dy)
           + w.z * textureGrad(t, vec3(uv + sorteioDoVertice(v3), l), dx, dy);
    if (!restaura) return c;
    vec4 media = textureLod(t, vec3(uv, l), 14.0);
    return (c - media) * inversesqrt(max(dot(w, w), 1e-4)) + media;
}
// Schuler 2013, como computeTBN do model_frag.glsl.
mat3 tbn(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    if (dot(T, T) > 1e-16 && dot(B, B) > 1e-16) {
        T = normalize(T - N * dot(N, T));
        vec3 Bo = cross(N, T);
        return mat3(T, dot(Bo, B) < 0.0 ? -Bo : Bo, N);
    }
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    T = normalize(cross(up, N));
    return mat3(T, cross(N, T), N);
}
void main() {
    float distCam = length(uCam - vWorld);
    float ndScale = uNd.z, ndSmooth = uNd.w;
    if (uNd.y > 0.5) {
        float tn = distCam / max(uNd.x, 1.0);
        ndScale *= lut8(uNdScale0, uNdScale1, tn);
        ndSmooth += lut8(uNdSmooth0, uNdSmooth1, tn);
    }
    float nBlur = exp2(ndSmooth);
    vec4 base = texture(uBase, vUv);
    if (base.a < 0.5) discard;
    vec3 c = base.rgb;
    vec4 nrm = uHasBase.x > 0.5 ? textureGrad(uBaseN, vUv, dFdx(vUv) * nBlur, dFdy(vUv) * nBlur) : vec4(0.5, 0.5, 1.0, 1.0);
    vec4 mrh = uHasBase.y > 0.5 ? texture(uBaseM, vUv) : vec4(0.0);
    bool temN = uHasBase.x > 0.5, temM = uHasBase.y > 0.5;
    float fracN = temN ? 1.0 : 0.0, fracM = temM ? 1.0 : 0.0;
    if (uSplat > 0) {
        vec4 w = texture(uMask0, vMaskUv) * step(0.0, uIdx0);
        vec4 w2 = uSplat > 1 ? texture(uMask1, vMaskUv) * step(0.0, uIdx1) : vec4(0.0);
        int s0 = -1, s1 = -1;
        float p0 = 0.0, p1 = 0.0;
        for (int i = 0; i < 8; ++i) topDuas(i, pega(w, w2, i), s0, p0, s1, p1);
        vec2 dxUv = dFdx(vUv), dyUv = dFdy(vUv);
        vec3 acc = vec3(0.0);
        vec4 accN = vec4(0.0), accM = vec4(0.0);
        float sum = 0.0, sumN = 0.0, sumM = 0.0;
        for (int k = 0; k < 2; ++k) {
            int s = k == 0 ? s0 : s1;
            float p = k == 0 ? p0 : p1;
            if (s < 0) continue;
            float l = pega(uIdx0, uIdx1, s);
            acc += semGrade(uLayC, l, vUv, dxUv, dyUv, true).rgb * p;
            sum += p;
            if (pega(uHasN0, uHasN1, s) > 0.5) { accN += semGrade(uLayN, l, vUv, dxUv * nBlur, dyUv * nBlur, false) * p; sumN += p; }
            if (pega(uHasM0, uHasM1, s) > 0.5) { accM += semGrade(uLayM, l, vUv, dxUv, dyUv, true) * p; sumM += p; }
        }
        float wBase = max(0.0, 1.0 - dot(w, vec4(1.0)) - dot(w2, vec4(1.0)));
        float tot = max(sum + wBase, 1e-4);
        c = (acc + c * wBase) / tot;
        // Fracao do pixel coberta por camada com o mapa (model_frag.glsl:
        // splatNormalW/splatMrahwW); o resto fica com o do material.
        if (sumN > 0.0) { nrm = mix(nrm, accN / sumN, sumN / tot); temN = true; }
        if (sumM > 0.0) {
            mrh = temM ? mix(mrh, accM / sumM, sumM / tot) : accM / sumM;
            fracM = temM ? 1.0 : sumM / tot;
            temM = true;
        }
    }
    vec3 N = length(vNormal) > 1e-8 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    if (temN) {
        // Sem reconstruir z: o acervo tem azul ~239 no plano (model_frag.glsl).
        vec3 t = nrm.xyz * 2.0 - 1.0;
        if (uMat.w > 0.5) t.y = -t.y;
        float strength = clamp(ndScale, 0.0, 3.0);
        if (uTess.y > 0.5) {
            float fN = lut8(uTessLut0, uTessLut1, distCam / max(uTess.x, 1.0));
            strength *= mix(1.0, 0.35, clamp((fN - 1.0) / max(uTess.w - 1.0, 1e-3), 0.0, 1.0));
        }
        t = mix(vec3(0.0, 0.0, 1.0), t, strength);
        N = normalize(tbn(N, vWorld, vUv) * t);
    }
    float rough = uMat.z, metal = 0.0, cav = 1.0;
    if (temM) {
        rough = mix(uMat.z, uMat.y * mrh.g, fracM);
        metal = mix(0.0, uMat.x * mrh.r, fracM);
        cav = mix(1.0, mrh.b, 0.5 * fracM);
    }
    rough = clamp(rough, 0.04, 1.0);
    // Luz direta como o pbrCookTorrance do desktop (pbr_common.glsl): specular
    // AA pela variancia da normal no pixel (soma em alpha^2, teto 0,18) e a
    // compensacao de multi-scattering. O especular do ambiente segue com a
    // rugosidade do material, como no directional.frag.
    float roughD;
    {
        vec3 dndu = dFdx(N), dndv = dFdy(N);
        float variance = 0.25 * (dot(dndu, dndu) + dot(dndv, dndv));
        float aa = rough * rough;
        roughD = variance > 0.0 ? sqrt(sqrt(clamp(aa * aa + min(2.0 * variance, 0.18), 0.0, 1.0))) : rough;
        roughD = clamp(roughD, 0.012, 1.0);
    }
    // As texturas sobem em UNORM com valores sRGB (como no desktop): a luz
    // trabalha no linear e a saida volta para sRGB.
    vec3 albedo = pow(c, vec3(2.2));
    vec3 V = normalize(uCam - vWorld), L = uSunL, H = normalize(L + V);
    float NdL = max(dot(N, L), 0.0), NdV = max(dot(N, V), 1e-3), NdH = max(dot(N, H), 0.0);
    float a = roughD * roughD, a2 = a * a;
    float d = NdH * NdH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d);
    float kG = (roughD + 1.0) * (roughD + 1.0) / 8.0;
    float G = NdL / (NdL * (1.0 - kG) + kG) * NdV / (NdV * (1.0 - kG) + kG);
    vec3 F0 = mix(vec3(0.04), albedo, metal);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    vec3 ms = multiScatter(F0, roughD, NdV);
    vec3 spec = D * G * F / max(4.0 * NdL * NdV, 1e-4) * ms * 3.14159265;
    vec3 direto = ((1.0 - F) * (1.0 - metal) * albedo + spec) * uSun * NdL;
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ceu = uProbe.x > 0.5 ? max(shIrradiance(N), vec3(0.0)) * (uProbe.z / 3.14159265)
                              : mix(uHorizon, uSky, up * up);
    vec3 hemi = mix(uGround, ceu, uHemiFloor + (1.0 - uHemiFloor) * up);
    vec3 Fa = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdV, 5.0);
    vec3 amb = hemi * cav * ((1.0 - Fa) * (1.0 - metal) * albedo + Fa * (1.0 - rough) * 0.5);
    vec3 kD = (1.0 - metal) * albedo / 3.14159265;
    for (int i = 0; i < kMaxLuzes; ++i) {
        if (i >= uNumLuzes) break;
        vec3 toL = uLuzPos[i].xyz - vWorld;
        float dist = length(toL);
        if (dist >= uLuzPos[i].w) continue;
        vec3 Lp = toL / max(dist, 1e-4);
        float nl = dot(N, Lp);
        if (nl <= 0.0) continue;
        float q = dist / uLuzPos[i].w;
        float att = pow(1.0 - q * q, 2.0) / (1.0 + q * q);
        vec3 Hp = normalize(V + Lp);
        float nh = max(dot(N, Hp), 0.0);
        float dp = nh * nh * (a2 - 1.0) + 1.0;
        float Gp = nl / (nl * (1.0 - kG) + kG) * NdV / (NdV * (1.0 - kG) + kG);
        vec3 Fp = F0 + (1.0 - F0) * pow(1.0 - max(dot(Hp, V), 0.0), 5.0);
        vec3 specP = min(a2 / (3.14159265 * dp * dp) * Gp * Fp / (4.0 * NdV * nl + 0.001) * ms, vec3(64.0));
        direto += ((1.0 - Fp) * kD + specP) * uLuzCor[i].rgb * att * nl;
    }
    vec3 cor = direto + amb;
    // NaN/inf vira preto aqui (expoente todo 1, pelos bits: o isnan pode
    // sumir na otimizacao do ANGLE): sem isto um pixel ruim entra no bloom.
    uvec3 eb = floatBitsToUint(cor) & 0x7f800000u;
    if (any(equal(eb, uvec3(0x7f800000u)))) cor = vec3(0.0);
    fragColor = vec4(uLinear > 0.5 ? cor : pow(cor, vec3(1.0 / 2.2)), 1.0);
}
)";

// Quad do sprite como no sprite_vert.glsl: ancora na vista, deslocamento no
// plano da camera inclinado por uTilt.
const char* kSpriteVs = R"(#version 300 es
layout(location = 0) in vec2 aCorner; // [-0.5, 0.5]^2
uniform mat4 uView;
uniform mat4 uProj;
uniform vec3 uAnchor;
uniform vec2 uSize;
uniform vec4 uRect;  // xOff, yOff, -, centerY
uniform float uTilt;
uniform float uFlip;
out vec2 vUv;
void main() {
    vUv = vec2(aCorner.x + 0.5, 0.5 - aCorner.y);
    if (uFlip > 0.5) vUv.x = 1.0 - vUv.x;
    vec3 anchor = (uView * vec4(uAnchor, 1.0)).xyz;
    vec3 up = vec3(0.0, cos(uTilt), -sin(uTilt));
    vec3 local = vec3(-1.0, 0.0, 0.0) * (uRect.x + aCorner.x * uSize.x) +
                 up * (-uRect.y + aCorner.y * uSize.y - uRect.w);
    gl_Position = uProj * vec4(anchor + local, 1.0);
}
)";

const char* kSpriteFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform vec4 uTint;
uniform vec3 uLight;
uniform float uLinear;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUv) * uTint;
    if (c.a < 0.5) discard;
    vec3 cor = pow(c.rgb, vec3(2.2)) * uLight;
    fragColor = vec4(uLinear > 0.5 ? cor : pow(cor, vec3(1.0 / 2.2)), 1.0);
}
)";

// Particulas (vfx_particle.vert/frag): o quad ja' vem pronto da CPU
// (WebVfxSim); aqui so' a folha de quadros, a borda suave e a saida
// premultiplicada - aditiva e' alfa 0, entao todas cabem numa mistura so'.
// Sem bindless: ate' 8 texturas por desenho, escolhidas pela unidade.
const char* kVfxVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aMisc;
layout(location = 4) in vec3 aCorner;
uniform mat4 uViewProj;
out vec4 vUv;
out vec4 vColor;
out vec4 vMisc;
out vec3 vCorner;
void main() {
    vUv = aUv;
    vColor = aColor;
    vMisc = aMisc;
    vCorner = aCorner;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

const char* kVfxFs = R"(#version 300 es
precision highp float;
in vec4 vUv;
in vec4 vColor;
in vec4 vMisc;   // x mistura do quadro seguinte, y unidade (-1 = disco), z borda suave, w profundidade
in vec3 vCorner; // xy canto, z mistura (0 aditiva, 1 premultiplicada, 2 alfa)
uniform sampler2D uTex0, uTex1, uTex2, uTex3, uTex4, uTex5, uTex6, uTex7;
uniform sampler2D uDepth;   // copia da profundidade da cena (o alvo esta' preso ao FBO)
uniform vec3 uNearFar;      // perto, longe, 1 = ha copia da profundidade
uniform vec2 uLow;          // x pixel da camada -> pixel da profundidade, y 1 = meia resolucao
out vec4 fragColor;
// GLSL ES 3.00 so' indexa sampler com constante: cadeia de ifs, com as
// derivadas tiradas fora dela (o fluxo nao e' uniforme).
vec4 pick(int u, vec2 uv, vec2 dx, vec2 dy) {
    if (u == 0) return textureGrad(uTex0, uv, dx, dy);
    if (u == 1) return textureGrad(uTex1, uv, dx, dy);
    if (u == 2) return textureGrad(uTex2, uv, dx, dy);
    if (u == 3) return textureGrad(uTex3, uv, dx, dy);
    if (u == 4) return textureGrad(uTex4, uv, dx, dy);
    if (u == 5) return textureGrad(uTex5, uv, dx, dy);
    if (u == 6) return textureGrad(uTex6, uv, dx, dy);
    return textureGrad(uTex7, uv, dx, dy);
}
void main() {
    vec2 dx = dFdx(vUv.xy), dy = dFdy(vUv.xy);
    vec4 tex;
    if (vMisc.y < -0.5) {
        float r2 = dot(vCorner.xy, vCorner.xy);
        float a = clamp(1.0 - r2, 0.0, 1.0);
        tex = vec4(1.0, 1.0, 1.0, a * a);
    } else {
        int u = int(vMisc.y + 0.5);
        tex = pick(u, vUv.xy, dx, dy);
        if (vMisc.x > 0.0) tex = mix(tex, pick(u, vUv.zw, dx, dy), vMisc.x);
    }
    vec4 c = vColor * tex;
    // Meia resolucao nao tem teste de profundidade por hardware: compara com
    // a profundidade cheia no centro do pixel.
    bool low = uLow.y > 0.5;
    if ((vMisc.z > 0.0 || low) && uNearFar.z > 0.5) {
        float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy * uLow.x), 0).r;
        if (d < 1.0) {
            float n = uNearFar.x, f = uNearFar.y;
            float sceneZ = n * f / (f - d * (f - n));
            if (vMisc.z > 0.0) c.a *= clamp((sceneZ - vMisc.w) / vMisc.z, 0.0, 1.0);
            else if (sceneZ < vMisc.w) discard;
        }
    }
    if (c.a <= 0.002) discard;
    int blend = int(vCorner.z + 0.5);
    if (blend == 1) fragColor = vec4(c.rgb * vColor.a, c.a);
    else fragColor = vec4(c.rgb * c.a, blend == 0 ? 0.0 : c.a);
}
)";

// Camada de meia resolucao das particulas sobre a cena (ja' premultiplicada).
const char* kVfxCompositeFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uLayer;
out vec4 fragColor;
void main() { fragColor = texture(uLayer, vUv); }
)";

// Pos do navegador: o que o post_composite.frag faz depois da luz, na mesma
// ordem - ceu onde nao ha' geometria, exposicao, nevoa de distancia ou de
// altura com a perspectiva aerea, a fase angular contra o sol
// (Henyey-Greenstein), tonemap (AgX/ACES/Neutral) e gamma. O ceu e' o
// gradiente horizonte -> zenite do ciclo, acinzentado pela cobertura de
// nuvem do clima (o ceu procedural do desktop fica para depois).
const char* kPostVs = R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* kPostFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uHdr;
uniform sampler2D uDepth;
uniform mat4 uInvViewProj; // GL: NDC -1..1 -> mundo
uniform vec3 uCam;
uniform vec4 uFog;       // rgb = cor, a = fim (0 = sem nevoa)
uniform vec4 uFog2;      // x = inicio, y = opacidade, z = altura, w = queda
uniform vec4 uSunFog;    // xyz = para onde esta' o sol, w = g da fase
uniform vec3 uSkyTop, uSkyHorizon, uSunColor;
uniform vec4 uTone;      // x = exposicao, y = operador, z = nuvem (0..1), w = escurecer (fx.dim)
uniform vec3 uDim;
uniform sampler2D uBloom;
uniform float uBloomK;   // intensidade do bloom (0 = desligado)
uniform float uSkyHdr;   // 1 = o ceu ja' esta' no alvo HDR (skybox.frag); 0 = gradiente daqui
uniform vec4 uGrade;     // x = saturacao, y = contraste, z = forca da vinheta, w = suavidade dela
uniform vec4 uTintSh, uTintHi; // split-toning: rgb = matiz, a = forca
// Grading, split-toning e vinheta do look, na ordem do post_composite.frag
// (depois do tonemap, antes do gamma).
vec3 gradeLook(vec3 c, vec2 uv) {
    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
    c = mix(vec3(dot(c, kLuma)), c, uGrade.x);
    c = 0.18 * pow(max(c, vec3(0.0)) / 0.18, vec3(uGrade.y));
    if (uTintSh.a > 0.0 || uTintHi.a > 0.0) {
        float l = dot(c, kLuma);
        float t = smoothstep(0.0, 1.0, clamp(l, 0.0, 1.0));
        vec3 sh = uTintSh.rgb / max(dot(uTintSh.rgb, kLuma), 1e-3);
        vec3 hi = uTintHi.rgb / max(dot(uTintHi.rgb, kLuma), 1e-3);
        vec3 r = c * mix(vec3(1.0), sh, clamp(uTintSh.a, 0.0, 1.0) * (1.0 - t));
        r *= mix(vec3(1.0), hi, clamp(uTintHi.a, 0.0, 1.0) * t);
        c = r * (l / max(dot(r, kLuma), 1e-5));
    }
    if (uGrade.z > 0.0) {
        vec2 sz = vec2(textureSize(uHdr, 0));
        vec2 asp = vec2(sz.x / max(sz.y, 1.0), 1.0);
        float d = length((uv - 0.5) * asp) / length(0.5 * asp);
        c *= 1.0 - clamp(uGrade.z, 0.0, 1.0) * smoothstep(1.0 - clamp(uGrade.w, 0.01, 1.0), 1.0, d);
    }
    return c;
}
out vec4 fragColor;
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
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 toneAgx(vec3 c) {
    const float minEv = -12.47393, maxEv = 4.026069;
    c = kAgxIn * max(c, vec3(0.0));
    c = clamp(log2(max(c, vec3(1e-10))), minEv, maxEv);
    c = agxContrast((c - minEv) / (maxEv - minEv));
    return pow(max(kAgxOut * c, vec3(0.0)), vec3(2.2));
}
vec3 toneAces(vec3 c) {
    return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
}
vec3 toneNeutral(vec3 c) {
    const float start = 0.76, desat = 0.15;
    float x = min(c.r, min(c.g, c.b));
    c -= x < 0.08 ? x - 6.25 * x * x : 0.04;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    const float d2 = 1.0 - start;
    float np = 1.0 - d2 * d2 / (peak + d2 - start);
    c *= np / peak;
    return mix(c, vec3(np), 1.0 - 1.0 / (desat * (peak - np) + 1.0));
}
float alturaNevoa(float y) {
    return exp(-max(0.0, y - uFog2.z) * uFog2.w);
}
void main() {
    float depth = texture(uDepth, vUv).r;
    vec4 w = uInvViewProj * vec4(vUv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec3 pos = w.xyz / w.w;
    vec3 dir = normalize(pos - uCam);
    vec3 color;
    if (depth >= 0.99999 && uSkyHdr > 0.5) {
        // Como o post_composite: o fundo so' recebe exposicao e bloom.
        color = texture(uHdr, vUv).rgb * uTone.x + texture(uBloom, vUv).rgb * uBloomK;
    } else if (depth >= 0.99999) {
        float y = clamp(dir.y, 0.0, 1.0);
        vec3 ceu = mix(uSkyHorizon, uSkyTop, sqrt(y));
        ceu += uSunColor * pow(max(dot(dir, uSunFog.xyz), 0.0), 64.0) * 0.5;
        float lum = dot(ceu, vec3(0.2126, 0.7152, 0.0722));
        color = mix(ceu, vec3(lum), uTone.z * 0.8) * uTone.x;
        // Particula na frente do ceu: o alvo HDR guarda a cor premultiplicada
        // e a cobertura (alfa) dela, limpo com alfa 0 onde nao ha' geometria.
        vec4 sobre = texture(uHdr, vUv);
        color = color * (1.0 - clamp(sobre.a, 0.0, 1.0)) + sobre.rgb * uTone.x;
        color += texture(uBloom, vUv).rgb * uBloomK;
    } else {
        // Como no post_composite: exposicao, bloom somado, depois a nevoa.
        color = texture(uHdr, vUv).rgb * uTone.x + texture(uBloom, vUv).rgb * uBloomK;
        if (uFog.a > 0.0) {
            float d = distance(pos, uCam);
            float f;
            if (uFog2.w > 0.0) {
                float od = (alturaNevoa(uCam.y) + alturaNevoa(pos.y)) * 0.5 * d / max(uFog.a - uFog2.x, 1.0);
                f = 1.0 - exp(-od * uFog2.y * 5.0);
            } else {
                f = clamp((d - uFog2.x) / (uFog.a - uFog2.x), 0.0, 1.0) * uFog2.y;
            }
            f = clamp(f, 0.0, 1.0);
            float aer = clamp((d - uFog2.x * 0.5) / max(uFog.a - uFog2.x * 0.5, 1.0), 0.0, 1.0);
            aer *= aer;
            float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
            color = mix(color, vec3(lum), aer * 0.55);
            color = mix(color, uFog.rgb * lum * 1.15, aer * 0.12);
            vec3 fogCor = uFog.rgb * uTone.x;
            float g = clamp(uSunFog.w, 0.0, 0.9);
            if (g > 0.001) {
                float g2 = g * g, ct = dot(dir, uSunFog.xyz);
                fogCor *= ((1.0 - g2) / pow(1.0 + g2 - 2.0 * g * ct, 1.5)) / max((1.0 - g2) / pow(1.0 + g2, 1.5), 1e-4);
            }
            color = mix(color, fogCor, f);
        }
    }
    int m = int(uTone.y + 0.5);
    color = max(color, vec3(0.0));
    if (m == 1) color = toneAgx(color);
    else if (m == 2) color = toneAces(color);
    else if (m == 3) color = toneNeutral(color);
    color = gradeLook(color, vUv);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    color = mix(color, pow(max(uDim, vec3(0.0)), vec3(1.0 / 2.2)), clamp(uTone.w, 0.0, 1.0));
    fragColor = vec4(color, 1.0);
}
)";

// Bloom do desktop (PostProcessor::render): bright pass com media ponderada
// contra vagalume em meia resolucao, piramide de 5 niveis com o downsample e
// o upsample de Kawase (bloom_kawase_down/up.frag), cada nivel de subida
// somando o de descida do mesmo tamanho.
const char* kBloomBrightFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSrc;
uniform float uThreshold;
out vec4 fragColor;
// Amostra finita e ate' 64: um inf daria peso 0 e inf * 0 = NaN, que a
// piramide de Kawase espalha em quadrados pretos piscando. Teste pelos bits
// (o isnan pode sumir na otimizacao do ANGLE).
vec3 finita(vec3 c) {
    uvec3 eb = floatBitsToUint(c) & 0x7f800000u;
    return any(equal(eb, uvec3(0x7f800000u))) ? vec3(0.0) : min(c, vec3(64.0));
}
void main() {
    vec2 t = 0.5 / vec2(textureSize(uSrc, 0));
    vec3 c0 = finita(texture(uSrc, vUv + vec2(-t.x, -t.y)).rgb), c1 = finita(texture(uSrc, vUv + vec2(t.x, -t.y)).rgb);
    vec3 c2 = finita(texture(uSrc, vUv + vec2(-t.x, t.y)).rgb), c3 = finita(texture(uSrc, vUv + vec2(t.x, t.y)).rgb);
    const vec3 L = vec3(0.2126, 0.7152, 0.0722);
    float w0 = 1.0 / (1.0 + dot(c0, L)), w1 = 1.0 / (1.0 + dot(c1, L));
    float w2 = 1.0 / (1.0 + dot(c2, L)), w3 = 1.0 / (1.0 + dot(c3, L));
    vec3 c = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / (w0 + w1 + w2 + w3);
    float b = max(dot(c, L) - uThreshold, 0.0);
    fragColor = vec4(c * (b / (1.0 + b * 0.25)), 1.0);
}
)";

const char* kBloomDownFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSrc;
out vec4 fragColor;
void main() {
    vec2 t = 1.0 / vec2(textureSize(uSrc, 0));
    vec3 s = texture(uSrc, vUv).rgb * 4.0;
    s += (texture(uSrc, vUv + vec2(-t.x, 0.0)).rgb + texture(uSrc, vUv + vec2(t.x, 0.0)).rgb +
          texture(uSrc, vUv + vec2(0.0, -t.y)).rgb + texture(uSrc, vUv + vec2(0.0, t.y)).rgb) * 2.0;
    s += texture(uSrc, vUv + vec2(-t.x, -t.y)).rgb + texture(uSrc, vUv + vec2(t.x, -t.y)).rgb +
         texture(uSrc, vUv + vec2(-t.x, t.y)).rgb + texture(uSrc, vUv + vec2(t.x, t.y)).rgb;
    fragColor = vec4(s / 16.0, 1.0);
}
)";

const char* kBloomUpFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSrc;
uniform sampler2D uAdd;
out vec4 fragColor;
void main() {
    vec2 t = 1.0 / vec2(textureSize(uSrc, 0));
    vec4 d = t.xyxy * vec4(1.0, 1.0, -1.0, 0.0);
    vec3 s = texture(uSrc, vUv - d.xy).rgb + texture(uSrc, vUv - d.wy).rgb +
             texture(uSrc, vUv + d.wy).rgb + texture(uSrc, vUv + d.xy).rgb;
    s += (texture(uSrc, vUv + d.zy).rgb + texture(uSrc, vUv + d.xw).rgb +
          texture(uSrc, vUv - d.xw).rgb + texture(uSrc, vUv - d.zy).rgb) * 2.0;
    s += texture(uSrc, vUv).rgb * 4.0;
    fragColor = vec4(s / 16.0 + texture(uAdd, vUv).rgb, 1.0);
}
)";

// Lava (liquid.frag, liquidKind 1): placas de crosta por Voronoi com as
// rachaduras incandescentes, emissiva e sem luz da cena. O caminho completo
// (rachadura fina incluida, o desktop so' a tira no LITE) e a mesma subida
// de nivel do liquid.vert (waveParams.w).
const char* kLavaVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uViewProj;
uniform float uLift;
out vec3 vWorld;
out vec2 vUv;
void main() {
    vec3 p = aPos + vec3(0.0, uLift, 0.0);
    vWorld = p;
    vUv = aUv;
    gl_Position = uViewProj * vec4(p, 1.0);
}
)";

const char* kLavaFs = R"(#version 300 es
precision highp float;
in vec3 vWorld;
in vec2 vUv;
uniform float uT;
uniform vec3 uLava; // brilho (emissivo do .env), calor da margem, calor do centro
uniform float uLinear;
out vec4 fragColor;
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x * 34.0) + 1.0) * x); }
float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    vec2 i = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289(i);
    vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
    vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m;
    m = m * m;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
vec2 lavaCells(vec2 p, float t) {
    vec2 ip = floor(p), fp = fract(p);
    float f1 = 8.0, f2 = 8.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = 0.5 + 0.38 * sin(t * 0.06 + 6.2831 * hash22(ip + g));
            vec2 r = g + o - fp;
            float d = dot(r, r);
            if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) { f2 = d; }
        }
    }
    return sqrt(vec2(f1, f2));
}
void main() {
    float t = uT, radial = vUv.x;
    float calor = uLava.x > 0.0 ? uLava.x / 3.0 : 1.0;
    vec2 fw = vWorld.xz * 0.010;
    vec2 warp = vec2(snoise(fw + vec2(t * 0.004, 0.0)), snoise(fw.yx - vec2(0.0, t * 0.003)));
    vec2 platesUv = vWorld.xz * 0.028 + warp * 0.45;
    vec2 f = lavaCells(platesUv, t);
    float width = 0.07 * calor * (0.55 + 0.9 * (snoise(fw * 3.1 + 5.0) * 0.5 + 0.5));
    float borda = uLava.y * smoothstep(0.65, 1.0, radial) + uLava.z * (1.0 - smoothstep(0.0, 0.35, radial));
    float meio = (uLava.y + uLava.z) > 0.0 ? smoothstep(0.25, 0.45, radial) * (1.0 - smoothstep(0.55, 0.75, radial)) : 0.0;
    width *= (1.0 + 1.5 * borda) * (1.0 - 0.35 * meio);
    float crack = 1.0 - smoothstep(0.0, width, f.y - f.x);
    vec2 fFine = lavaCells(platesUv * 2.6 + 17.0, t * 1.3);
    float crackFine = (1.0 - smoothstep(0.0, width * 0.45, fFine.y - fFine.x)) * 0.55;
    float rim = exp(-(f.y - f.x) * 9.0) * 0.22;
    float heat = clamp(max(max(crack, crackFine), rim) * min(calor, 1.6) + 0.18 * borda, 0.0, 1.0);
    vec3 hot = vec3(1.0, 0.24, 0.03);
    vec3 lava = mix(vec3(0.010, 0.005, 0.005), hot, heat);
    lava = mix(lava, vec3(1.0, 0.80, 0.32), pow(heat, 4.0)) + hot * heat * 0.8;
    fragColor = vec4(uLinear > 0.5 ? lava : pow(lava, vec3(1.0 / 2.2)), 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        ERUPTION_LOG_ERROR("[WEB] shader nao compilou: %s", log);
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        ERUPTION_LOG_ERROR("[WEB] programa nao ligou: %s", log);
    }
    return p;
}

// Projecao do motor (Vulkan: Y para baixo, profundidade 0..1) para o GL
// (Y para cima, -1..1): y' = -y, z' = 2z - w.
Mat4 vulkanToGl(const Mat4& proj) {
    Mat4 m(1.0f);
    m[1][1] = -1.0f;
    m[2][2] = 2.0f;
    m[3][2] = -1.0f;
    return m * proj;
}

// Slots de textura de um desenho, lidos do primeiro vertice: no chao o splat
// e' o mesmo em toda a malha. Numa peca com varios materiais a base muda por
// trecho (WebSceneGl::BaseRun).
// texIndex leva o slot nos 16 bits de baixo e o id do material em cima
// (model_vert.glsl: inMaterial.y & 0xFFFF).
constexpr uint32_t kTexSlotMask = 0xFFFFu;

// Bits altos do slot de 16 bits da camada do splat: a camada traz normal em
// slot+1 e/ou MRAH-W em slot+2 (TerrainPaint.cpp, model_frag.glsl).
constexpr uint32_t kSplatSlotMask = 0x3FFFu, kSplatHasNormal = 0x8000u, kSplatHasMrahw = 0x4000u;
// Lado das camadas nos arrays do splat: o das texturas de chao do acervo.
constexpr GLsizei kSplatLayerSize = 512;

struct DrawSlots {
    uint32_t base = 0, normal = 0, pbr = 0, mask0 = 0, mask1 = 0;
    uint32_t layer[8] = {}; // slot com os bits de normal/MRAH-W
};

} // namespace

struct WebSceneGl {
    GLuint model = 0, sprite = 0, post = 0, lava = 0;
    // Alvo HDR da cena (RGBA16F + profundidade em textura) para o pos; sem
    // EXT_color_buffer_float a cena desenha direto na tela, como antes.
    GLuint hdrFbo = 0, hdrColor = 0, hdrDepth = 0;
    // Copia da profundidade para a borda suave das particulas (ler o alvo
    // preso ao FBO seria laco de realimentacao no WebGL2).
    GLuint depthCopy = 0, depthCopyFbo = 0;
    // Camada de meia resolucao das particulas lowRes (RGBA16F premultiplicada).
    GLuint lowFbo = 0, lowColor = 0;
    int lowW = 1, lowH = 1;
    int hdrW = 0, hdrH = 0;
    bool hdrOk = false;
    // Piramide do bloom (RGBA16F), refeita junto com o alvo HDR.
    static constexpr int kBloomMips = 5;
    GLuint bloomProg[3] = {0, 0, 0}; // bright, down, up
    GLuint bloomFbo = 0;
    GLuint bloomDown[kBloomMips] = {}, bloomUp[kBloomMips] = {};
    int bloomW = 0, bloomH = 0;
    GLuint vao = 0, quad = 0;
    GLuint white = 0, whiteArray = 0; // fallbacks: textura e array de 1 texel branco
    GLuint sky = 0, skyUbo = 0; // skybox.frag do desktop e o bloco SkyParamsUBO dele
    // Sonda de ceu (SkyProbe no desktop): as 6 faces lado a lado num alvo
    // float, lidas de volta e projetadas em SH9 na CPU (sem compute no WebGL2).
    // A leitura vai para um buffer de pixel com fence e so' e' copiada quando
    // a GPU ja' terminou: o glReadPixels direto parava o quadro.
    GLuint probeFbo = 0, probeTex = 0, probePbo = 0;
    GLsync probeFence = nullptr;
    bool probeOk = false;
    uint32_t probeFrame = 0;
    Vec3 sh[9] = {};
    bool shReady = false;
    struct GlBuffer {
        GLuint name = 0;
        uint64_t version = ~0ull;
    };
    std::unordered_map<VkBuffer, GlBuffer> buffers;
    // Trechos de triangulos com a mesma textura base, por malha (buffer de
    // indices + primeiro indice): sem bindless, cada material e' um desenho.
    struct BaseRun {
        uint32_t first, count, tex, normal, pbr;
    };
    struct BaseRuns {
        uint64_t version = ~0ull, vertexVersion = ~0ull; // o motor remenda texIndex nos vertices depois
        std::vector<BaseRun> runs;
    };
    std::map<std::pair<VkBuffer, uint32_t>, BaseRuns> baseRuns;
    // Camadas de um splat em tres texture arrays (cor, normal, MRAH-W), pelas
    // 8 camadas empacotadas: sobe uma vez, quando todas as texturas chegaram.
    struct Splat {
        GLuint arr[3] = {0, 0, 0};
        float idx[8], hasN[8], hasM[8];
    };
    std::map<std::array<uint32_t, 8>, Splat> splats;
    GLuint fboRead = 0, fboDraw = 0;
    // Uniformes por nome, uma vez.
    GLint mViewProj, mModel, mUvXform, mIdx0, mIdx1, mHasN0, mHasN1, mHasM0, mHasM1, mSplat, mHasBase, mMat, mCam,
        mSunL, mSun, mSky, mHorizon, mGround, mHemiFloor;
    GLint sView, sProj, sAnchor, sSize, sRect, sTilt, sFlip, sTint, sLight;
    // Particulas: simulacao na CPU e um buffer de vertices por frame.
    WebVfxSim vfx;
    std::vector<WebVfxVertex> vfxVerts;
    std::vector<uint32_t> vfxTex;
    GLuint vfxProg = 0, vfxComposite = 0, vfxVbo = 0, vfxIbo = 0;
    GLint vfxViewProj = -1, vfxNearFar = -1, vfxLow = -1;
};

namespace {

// Buffer GL espelhando um VkBuffer; sobe de novo quando o motor copia para ele.
GLuint glBufferFor(WebSceneGl& s, VkBuffer vk, GLenum target) {
    size_t size = 0;
    uint64_t version = 0;
    const uint8_t* bytes = web::bufferBytes(vk, &size, &version);
    if (!bytes) return 0;
    WebSceneGl::GlBuffer& b = s.buffers[vk];
    if (!b.name) glGenBuffers(1, &b.name);
    glBindBuffer(target, b.name);
    if (b.version != version) {
        glBufferData(target, GLsizeiptr(size), bytes, GL_STATIC_DRAW);
        b.version = version;
    }
    return b.name;
}

const std::vector<WebSceneGl::BaseRun>& baseRunsFor(WebSceneGl& s, const ModelMeshGPU& mesh, const uint8_t* vb,
                                                    const uint8_t* ib) {
    uint64_t version = 0, vertexVersion = 0;
    web::bufferBytes(mesh.indexBuffer, nullptr, &version);
    web::bufferBytes(mesh.vertexBuffer, nullptr, &vertexVersion);
    WebSceneGl::BaseRuns& entry = s.baseRuns[{mesh.indexBuffer, mesh.firstIndex}];
    if (entry.version == version && entry.vertexVersion == vertexVersion) return entry.runs;
    entry.version = version;
    entry.vertexVersion = vertexVersion;
    entry.runs.clear();
    for (uint32_t t = 0; t + 3 <= mesh.indexCount; t += 3) {
        uint32_t index = 0;
        std::memcpy(&index, ib + (size_t(mesh.firstIndex) + t) * 4, 4);
        uint32_t tex = 0;
        std::memcpy(&tex, vb + (size_t(index) + size_t(mesh.vertexOffset)) * sizeof(TerrainVertex) +
                              offsetof(TerrainVertex, texIndex), 4);
        tex &= kTexSlotMask;
        if (!entry.runs.empty() && entry.runs.back().tex == tex) {
            entry.runs.back().count += 3;
            continue;
        }
        TerrainVertex v;
        std::memcpy(&v, vb + (size_t(index) + size_t(mesh.vertexOffset)) * sizeof(TerrainVertex), sizeof(v));
        entry.runs.push_back({mesh.firstIndex + t, 3, tex, v.normalIndex, v.pbrIndex});
    }
    return entry.runs;
}

// Copia a textura do slot para a camada `layer` do array (blit com filtro:
// o tamanho da fonte pode ser outro). Falso se a textura ainda nao chegou.
bool copyToLayer(WebSceneGl& s, uint32_t slot, GLuint arr, GLint layer) {
    unsigned target = 0;
    const GLuint src = web::slotTexture(slot, &target);
    uint32_t w = 0, h = 0;
    if (!src || target != GL_TEXTURE_2D || !web::slotExtent(slot, &w, &h)) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fboRead);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fboDraw);
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr, 0, layer);
    glBlitFramebuffer(0, 0, GLint(w), GLint(h), 0, 0, kSplatLayerSize, kSplatLayerSize, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
    return true;
}

// Arrays do splat de um desenho, ou nulo enquanto falta alguma textura (o
// desenho sai so' com a base ate' la').
const WebSceneGl::Splat* splatFor(WebSceneGl& s, const uint32_t (&layer)[8]) {
    std::array<uint32_t, 8> key;
    std::copy(std::begin(layer), std::end(layer), key.begin());
    const auto it = s.splats.find(key);
    if (it != s.splats.end()) return &it->second;
    WebSceneGl::Splat sp;
    GLint count = 0;
    for (int l = 0; l < 8; ++l) {
        const uint32_t slot = layer[l] & kSplatSlotMask;
        sp.idx[l] = slot ? float(count++) : -1.0f;
        sp.hasN[l] = slot && (layer[l] & kSplatHasNormal) ? 1.0f : 0.0f;
        sp.hasM[l] = slot && (layer[l] & kSplatHasMrahw) ? 1.0f : 0.0f;
        if (slot && (!web::slotTexture(slot) || (sp.hasN[l] > 0.0f && !web::slotTexture(slot + 1)) ||
                     (sp.hasM[l] > 0.0f && !web::slotTexture(slot + 2))))
            return nullptr;
    }
    if (!count) return nullptr;
    if (!s.fboRead) {
        glGenFramebuffers(1, &s.fboRead);
        glGenFramebuffers(1, &s.fboDraw);
    }
    GLint drawFb = 0, readFb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
    GLsizei mips = 1;
    while ((kSplatLayerSize >> mips) > 0) ++mips;
    glGenTextures(3, sp.arr);
    for (int m = 0; m < 3; ++m) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, sp.arr[m]);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, mips, GL_RGBA8, kSplatLayerSize, kSplatLayerSize, count);
        for (int l = 0; l < 8; ++l) {
            const uint32_t slot = layer[l] & kSplatSlotMask;
            if (!slot || (m == 1 && sp.hasN[l] == 0.0f) || (m == 2 && sp.hasM[l] == 0.0f)) continue;
            copyToLayer(s, slot + uint32_t(m), sp.arr[m], GLint(sp.idx[l]));
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, sp.arr[m]);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFb));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFb));
    ERUPTION_LOG_INFO("[WEB] splat: %d camadas em arrays de %dx%d", int(count), int(kSplatLayerSize),
                      int(kSplatLayerSize));
    return &s.splats.emplace(key, sp).first->second;
}

void bindArray(GLuint program, const char* name, int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glUniform1i(glGetUniformLocation(program, name), unit);
}

// (Re)cria o alvo HDR no tamanho da tela. Falso se o navegador nao renderiza
// em float (sem EXT_color_buffer_float).
bool ensureHdr(WebSceneGl& s, int w, int h) {
    if (s.hdrFbo && s.hdrW == w && s.hdrH == h) return s.hdrOk;
    if (!s.hdrFbo) {
        glGenFramebuffers(1, &s.hdrFbo);
        glGenTextures(1, &s.hdrColor);
        glGenTextures(1, &s.hdrDepth);
    }
    s.hdrW = w;
    s.hdrH = h;
    glBindTexture(GL_TEXTURE_2D, s.hdrColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, s.hdrDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLint prev = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
    if (!s.depthCopy) {
        glGenTextures(1, &s.depthCopy);
        glGenFramebuffers(1, &s.depthCopyFbo);
    }
    glBindTexture(GL_TEXTURE_2D, s.depthCopy);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, s.depthCopyFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s.depthCopy, 0);
    if (!s.lowFbo) {
        glGenTextures(1, &s.lowColor);
        glGenFramebuffers(1, &s.lowFbo);
    }
    s.lowW = std::max(1, w / 2);
    s.lowH = std::max(1, h / 2);
    glBindTexture(GL_TEXTURE_2D, s.lowColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, s.lowW, s.lowH, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, s.lowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.lowColor, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s.hdrFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.hdrColor, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s.hdrDepth, 0);
    s.hdrOk = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev));
    ERUPTION_LOG_WARN("[WEB] alvo HDR %dx%d: %s", w, h, s.hdrOk ? "RGBA16F" : "sem float, desenho direto");
    return s.hdrOk;
}

// Bloom do quadro sobre o alvo HDR; devolve a textura do nivel 0 de subida
// (meia resolucao) ou 0 sem bloom.
GLuint renderBloom(WebSceneGl& s, float threshold) {
    const int w0 = std::max(1, s.hdrW / 2), h0 = std::max(1, s.hdrH / 2);
    auto texSize = [&](int i, int& w, int& h) {
        w = std::max(1, w0 >> i);
        h = std::max(1, h0 >> i);
    };
    if (s.bloomW != w0 || s.bloomH != h0) {
        if (!s.bloomFbo) {
            glGenFramebuffers(1, &s.bloomFbo);
            glGenTextures(WebSceneGl::kBloomMips, s.bloomDown);
            glGenTextures(WebSceneGl::kBloomMips, s.bloomUp);
        }
        for (int i = 0; i < WebSceneGl::kBloomMips; ++i) {
            int w, h;
            texSize(i, w, h);
            for (GLuint t : {s.bloomDown[i], s.bloomUp[i]}) {
                glBindTexture(GL_TEXTURE_2D, t);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }
        s.bloomW = w0;
        s.bloomH = h0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s.bloomFbo);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    auto pass = [&](GLuint prog, GLuint dst, int level, GLuint src, GLuint add) {
        int w, h;
        texSize(level, w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        glViewport(0, 0, w, h);
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glUniform1i(glGetUniformLocation(prog, "uSrc"), 0);
        if (add) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, add);
            glUniform1i(glGetUniformLocation(prog, "uAdd"), 1);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    glUseProgram(s.bloomProg[0]);
    glUniform1f(glGetUniformLocation(s.bloomProg[0], "uThreshold"), threshold);
    pass(s.bloomProg[0], s.bloomDown[0], 0, s.hdrColor, 0);
    for (int i = 1; i < WebSceneGl::kBloomMips; ++i) pass(s.bloomProg[1], s.bloomDown[i], i, s.bloomDown[i - 1], 0);
    GLuint last = s.bloomDown[WebSceneGl::kBloomMips - 1];
    for (int i = WebSceneGl::kBloomMips - 2; i >= 0; --i) {
        pass(s.bloomProg[2], s.bloomUp[i], i, last, s.bloomDown[i]);
        last = s.bloomUp[i];
    }
    glActiveTexture(GL_TEXTURE0);
    return last;
}

void bindSlot(GLuint program, const char* name, int unit, uint32_t slot, GLuint fallback) {
    glActiveTexture(GL_TEXTURE0 + unit);
    const GLuint tex = slot ? web::slotTexture(slot) : 0;
    glBindTexture(GL_TEXTURE_2D, tex ? tex : fallback);
    glUniform1i(glGetUniformLocation(program, name), unit);
}

// Teto do pool na CPU: o preset do desktop chega a 131072, o que no wasm
// custaria varios ms por frame so' de simulacao.
constexpr uint32_t kWebVfxMaxParticles = 32768;
constexpr int kVfxUnits = 8;              // texturas por desenho (uTex0..7)
constexpr uint32_t kVfxQuadsPerDraw = 16384; // indices de 16 bits: 65536 vertices

// Um trecho de quads seguidos (na ordem de tras para frente) que cabe nas
// 8 unidades de textura.
struct VfxBatch {
    uint32_t first = 0, count = 0;
    GLuint tex[kVfxUnits] = {};
    int used = 0;
};

// Trechos dos quads [begin, end): a unidade de cada quad vai no vertice
// (misc.y) antes de subir; um trecho novo quando as 8 unidades enchem.
void buildVfxBatches(WebSceneGl& s, uint32_t begin, uint32_t end, std::vector<VfxBatch>& batches) {
    batches.clear();
    batches.emplace_back();
    batches.back().first = begin;
    for (uint32_t q = begin; q < end; ++q) {
        unsigned target = 0;
        const uint32_t slot = s.vfxTex[q];
        const GLuint tex = slot != 0xFFFFFFFFu ? web::slotTexture(slot, &target) : 0;
        if (batches.back().count == kVfxQuadsPerDraw) {
            const VfxBatch prev = batches.back();
            batches.emplace_back();
            VfxBatch& b = batches.back();
            b.first = q;
            std::copy(std::begin(prev.tex), std::end(prev.tex), std::begin(b.tex));
            b.used = prev.used;
        }
        int unit = -1;
        if (tex && target == GL_TEXTURE_2D) {
            VfxBatch* b = &batches.back();
            for (int u = 0; u < b->used; ++u)
                if (b->tex[u] == tex) unit = u;
            if (unit < 0 && b->used == kVfxUnits) {
                batches.emplace_back();
                batches.back().first = q;
                b = &batches.back();
            }
            if (unit < 0) {
                unit = b->used++;
                b->tex[unit] = tex;
            }
        }
        ++batches.back().count;
        for (int v = 0; v < 4; ++v) s.vfxVerts[size_t(q) * 4 + size_t(v)].misc[1] = float(unit);
    }
}

void drawVfxBatches(WebSceneGl& s, const std::vector<VfxBatch>& batches) {
    for (const VfxBatch& b : batches) {
        if (!b.count) continue;
        for (int u = 0; u < kVfxUnits; ++u) {
            glActiveTexture(GL_TEXTURE0 + GLenum(u));
            glBindTexture(GL_TEXTURE_2D, u < b.used ? b.tex[u] : s.white);
        }
        // Sem base vertex no GLES 3.0: o primeiro quad entra no ponteiro.
        const size_t base = size_t(b.first) * 4 * sizeof(WebVfxVertex);
        auto attr = [base](GLuint loc, GLint n, size_t off) {
            glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, sizeof(WebVfxVertex),
                                  reinterpret_cast<const void*>(base + off));
        };
        attr(0, 3, offsetof(WebVfxVertex, pos));
        attr(1, 4, offsetof(WebVfxVertex, uv));
        attr(2, 4, offsetof(WebVfxVertex, color));
        attr(3, 4, offsetof(WebVfxVertex, misc));
        attr(4, 3, offsetof(WebVfxVertex, corner));
        glDrawElements(GL_TRIANGLES, GLsizei(b.count * 6), GL_UNSIGNED_SHORT, nullptr);
    }
}

// Particulas (WebVfxSim::buildQuads) no alvo HDR, na ordem de tras para
// frente, mistura premultiplicada (aditiva sai com alfa 0). As de meia
// resolucao (fumaca, poeira) vao antes numa camada de 1/4 dos pixels, como o
// drawLowRes do desktop: no navegador o custo delas e' de preenchimento
// (medido no quadro 200 da corrida: 117 s com tudo em resolucao cheia contra
// 75 s sem desenhar particula).
void drawWebParticles(WebSceneGl& s, const Mat4& viewProj, float nearPlane, float farPlane, int width, int height,
                      uint32_t lowQuads) {
    const uint32_t quads = uint32_t(s.vfxTex.size());
    if (!quads) return;
    if (!s.vfxProg) {
        s.vfxProg = link(kVfxVs, kVfxFs);
        s.vfxComposite = link(kPostVs, kVfxCompositeFs);
        s.vfxViewProj = glGetUniformLocation(s.vfxProg, "uViewProj");
        s.vfxNearFar = glGetUniformLocation(s.vfxProg, "uNearFar");
        s.vfxLow = glGetUniformLocation(s.vfxProg, "uLow");
        glGenBuffers(1, &s.vfxVbo);
        glGenBuffers(1, &s.vfxIbo);
        std::vector<uint16_t> idx(size_t(kVfxQuadsPerDraw) * 6);
        for (uint32_t q = 0; q < kVfxQuadsPerDraw; ++q) {
            const uint16_t v = uint16_t(q * 4);
            const uint16_t tri[6] = {v, uint16_t(v + 1), uint16_t(v + 2), v, uint16_t(v + 2), uint16_t(v + 3)};
            std::copy(std::begin(tri), std::end(tri), idx.begin() + long(q) * 6);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.vfxIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(uint16_t)), idx.data(), GL_STATIC_DRAW);
    }
    lowQuads = std::min(lowQuads, quads);
    static std::vector<VfxBatch> lowBatches, fullBatches;
    buildVfxBatches(s, 0, lowQuads, lowBatches);
    buildVfxBatches(s, lowQuads, quads, fullBatches);

    // Copia da profundidade para a borda suave (mesmo formato: blit direto).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.hdrFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.depthCopyFbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glUseProgram(s.vfxProg);
    glUniformMatrix4fv(s.vfxViewProj, 1, GL_FALSE, &viewProj[0][0]);
    glUniform3f(s.vfxNearFar, nearPlane, farPlane, 1.0f);
    static const char* kNames[kVfxUnits] = {"uTex0", "uTex1", "uTex2", "uTex3", "uTex4", "uTex5", "uTex6", "uTex7"};
    for (int u = 0; u < kVfxUnits; ++u) glUniform1i(glGetUniformLocation(s.vfxProg, kNames[u]), u);
    glUniform1i(glGetUniformLocation(s.vfxProg, "uDepth"), kVfxUnits);
    glActiveTexture(GL_TEXTURE0 + kVfxUnits);
    glBindTexture(GL_TEXTURE_2D, s.depthCopy);

    glBindBuffer(GL_ARRAY_BUFFER, s.vfxVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(s.vfxVerts.size() * sizeof(WebVfxVertex)), s.vfxVerts.data(),
                 GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.vfxIbo);
    for (GLuint loc = 0; loc < 5; ++loc) glEnableVertexAttribArray(loc);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (lowQuads) {
        // Camada de meia resolucao: sem profundidade presa, o pixel compara
        // com a profundidade cheia no centro dele (vfx_particle_frag.glsl).
        glBindFramebuffer(GL_FRAMEBUFFER, s.lowFbo);
        glViewport(0, 0, s.lowW, s.lowH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);
        glUniform2f(s.vfxLow, float(width) / float(s.lowW), 1.0f);
        drawVfxBatches(s, lowBatches);
        // Compoe sobre a cena (premultiplicada, filtro linear).
        glBindFramebuffer(GL_FRAMEBUFFER, s.hdrFbo);
        glViewport(0, 0, width, height);
        for (GLuint loc = 0; loc < 5; ++loc) glDisableVertexAttribArray(loc);
        glUseProgram(s.vfxComposite);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s.lowColor);
        glUniform1i(glGetUniformLocation(s.vfxComposite, "uLayer"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(s.vfxProg);
        for (GLuint loc = 0; loc < 5; ++loc) glEnableVertexAttribArray(loc);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s.hdrFbo);
    glEnable(GL_DEPTH_TEST);
    glUniform2f(s.vfxLow, 1.0f, 0.0f);
    drawVfxBatches(s, fullBatches);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    for (GLuint loc = 0; loc < 5; ++loc) glDisableVertexAttribArray(loc);
    glActiveTexture(GL_TEXTURE0);
}

} // namespace

// As kMaxLuzes (8) luzes que mais pesam perto da camera: as do mapa (ligadas) e
// as dos efeitos (VfxSystem::collectLights, o mesmo orcamento do desktop).
void Engine::uploadWebLights(unsigned program, const Vec3& eye) {
    constexpr int kMax = 8; // igual a kMaxLuzes do shader: 16 deixou o quadro ~45% mais lento no Chrome sem GPU
    static std::vector<VfxSystem::Light> vfxLights;
    static std::vector<PointLight> todas;
    m_vfx.collectLights(eye, m_vfx.lightBudget(), vfxLights);
    todas.clear();
    for (const PointLight& l : m_deferredLighting.getPointLights())
        if (l.enabled && l.intensity > 0.0f && l.radius > 0.0f) todas.push_back(l);
    for (const auto& v : vfxLights) {
        PointLight p;
        p.position = v.position;
        p.radius = v.radius;
        p.color = v.color;
        p.intensity = v.intensity;
        todas.push_back(p);
    }
    auto longe = [&](const PointLight& l) { return glm::distance(l.position, eye) - l.radius; };
    const size_t n = std::min<size_t>(kMax, todas.size());
    std::partial_sort(todas.begin(), todas.begin() + long(n), todas.end(),
                      [&](const PointLight& a, const PointLight& b) { return longe(a) < longe(b); });
    Vec4 pos[kMax], cor[kMax];
    const float escala = m_deferredLighting.pbrLightScale();
    for (size_t i = 0; i < n; ++i) {
        pos[i] = Vec4(todas[i].position, todas[i].radius);
        cor[i] = Vec4(todas[i].color * todas[i].intensity * escala, 0.0f);
    }
    glUniform1i(glGetUniformLocation(program, "uNumLuzes"), int(n));
    if (n) {
        glUniform4fv(glGetUniformLocation(program, "uLuzPos"), GLsizei(n), &pos[0][0]);
        glUniform4fv(glGetUniformLocation(program, "uLuzCor"), GLsizei(n), &cor[0][0]);
    }
}

void Engine::renderWebScene() {
    if (!m_webScene) {
        WebSceneGl* s = new WebSceneGl();
        s->model = link(kModelVs, kModelFs);
        s->sprite = link(kSpriteVs, kSpriteFs);
        s->post = link(kPostVs, kPostFs);
        s->lava = link(kLavaVs, kLavaFs);
        // Ceu: o skybox.frag do desktop, convertido do SPIR-V no build.
        s->sky = link(kGles_postprocess_fullscreen_vert, kGles_skybox_skybox_frag);
        GLint skyOk = 0;
        glGetProgramiv(s->sky, GL_LINK_STATUS, &skyOk);
        if (skyOk) {
            glUniformBlockBinding(s->sky, glGetUniformBlockIndex(s->sky, "SkyParamsUBO"), 0);
            glGenBuffers(1, &s->skyUbo);
        } else {
            s->sky = 0;
        }
        s->bloomProg[0] = link(kPostVs, kBloomBrightFs);
        s->bloomProg[1] = link(kPostVs, kBloomDownFs);
        s->bloomProg[2] = link(kPostVs, kBloomUpFs);
        glGenVertexArrays(1, &s->vao);
        const float quad[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
        glGenBuffers(1, &s->quad);
        glBindBuffer(GL_ARRAY_BUFFER, s->quad);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        const uint8_t white[4] = {255, 255, 255, 255};
        glGenTextures(1, &s->white);
        glBindTexture(GL_TEXTURE_2D, s->white);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glGenTextures(1, &s->whiteArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, s->whiteArray);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        auto u = [](GLuint p, const char* n) { return glGetUniformLocation(p, n); };
        s->mViewProj = u(s->model, "uViewProj");
        s->mModel = u(s->model, "uModel");
        s->mUvXform = u(s->model, "uUvXform");
        s->mIdx0 = u(s->model, "uIdx0");
        s->mIdx1 = u(s->model, "uIdx1");
        s->mHasN0 = u(s->model, "uHasN0");
        s->mHasN1 = u(s->model, "uHasN1");
        s->mHasM0 = u(s->model, "uHasM0");
        s->mHasM1 = u(s->model, "uHasM1");
        s->mSplat = u(s->model, "uSplat");
        s->mHasBase = u(s->model, "uHasBase");
        s->mMat = u(s->model, "uMat");
        s->mCam = u(s->model, "uCam");
        s->mSunL = u(s->model, "uSunL");
        s->mSun = u(s->model, "uSun");
        s->mSky = u(s->model, "uSky");
        s->mHorizon = u(s->model, "uHorizon");
        s->mGround = u(s->model, "uGround");
        s->mHemiFloor = u(s->model, "uHemiFloor");
        s->sView = u(s->sprite, "uView");
        s->sProj = u(s->sprite, "uProj");
        s->sAnchor = u(s->sprite, "uAnchor");
        s->sSize = u(s->sprite, "uSize");
        s->sRect = u(s->sprite, "uRect");
        s->sTilt = u(s->sprite, "uTilt");
        s->sFlip = u(s->sprite, "uFlip");
        s->sTint = u(s->sprite, "uTint");
        s->sLight = u(s->sprite, "uLight");
        m_webScene = s;
    }
    WebSceneGl& s = *m_webScene;
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLint screenFb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &screenFb);
    const bool hdr = viewport[2] > 0 && viewport[3] > 0 && ensureHdr(s, viewport[2], viewport[3]);
    if (hdr) {
        glBindFramebuffer(GL_FRAMEBUFFER, s.hdrFbo);
        glViewport(0, 0, viewport[2], viewport[3]);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // alfa = cobertura de particula sobre o ceu (pos)
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    const Mat4 view = m_camera.viewMatrix();
    const Mat4 proj = vulkanToGl(m_camera.projNoJitter());
    const Mat4 viewProj = proj * view;
    // Sol e ambiente como o Render.cpp monta o LightingEnvironment: ambiente
    // constante do painel, lua de noite, e o que o .env do mapa escreveu.
    LightingEnvironment env;
    env.sun.direction = m_dayNightCycle.getSunDirection();
    env.sun.color = m_dayNightCycle.getSunColor();
    env.sun.intensity = m_dayNightCycle.getSunIntensity();
    env.ambientColor = m_dayNightCycle.getAmbientColor();
    env.ambientIntensity = m_ambientIntensity;
    env.ambientSkyColor = m_dayNightCycle.getSkyTopColor();
    env.ambientGroundColor = m_mapGroundAlbedo;
    env.skyHorizonColor = m_dayNightCycle.getSkyHorizonColor();
    if (m_moonFillEnabled && env.sun.intensity < 0.25f) {
        const float t = 1.0f - glm::clamp(env.sun.intensity / 0.25f, 0.0f, 1.0f);
        float moonI = m_dayNightCycle.getMoonIntensity();
        if (moonI <= 0.0f) moonI = 0.18f;
        env.sun.direction =
            glm::normalize(glm::mix(env.sun.direction, -glm::normalize(m_dayNightCycle.getMoonVisualDirection()), t));
        env.sun.color = glm::mix(env.sun.color, m_dayNightCycle.getMoonColor(), t);
        env.sun.intensity = glm::mix(env.sun.intensity, moonI * m_moonFillStrength, t);
    }
    if (m_mapAmbientAuthored) {
        env.ambientColor = m_mapAmbientColor;
        env.ambientIntensity = m_mapAmbientIntensity;
    }
    if (m_mapLightAuthored) {
        env.sun.direction = m_mapSunDirection;
        env.sun.color = m_mapSunColor;
        env.sun.intensity = m_mapSunIntensity;
    }
    if (m_classic.on) applyClassicLight(env);
    const float gi = m_classic.on ? 1.0f
                                  : (m_renderEffects.size() > 1 && m_renderEffects[1]->isEnabled()
                                         ? m_renderEffects[1]->getParam(0)
                                         : 0.0f);
    const float ambI = env.ambientIntensity * gi;
    const Vec3 sunL = -glm::normalize(env.sun.direction);
    const Vec3 sun = env.sun.color * env.sun.intensity;
    const Vec3 sky = env.ambientSkyColor * ambI, horizon = env.skyHorizonColor * ambI,
               ground = env.ambientGroundColor * ambI;
    const Vec3 ambient = (env.ambientSkyColor + env.skyHorizonColor) * 0.5f * ambI;
    const Vec3 camPos = m_camera.position();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE); // a troca de Y da projecao inverte o sentido dos triangulos
    glDisable(GL_BLEND);
    glBindVertexArray(s.vao);

    if (hdr) updateWebSkyProbe(s);
    // ---- malhas do mapa (chao e pecas)
    glUseProgram(s.model);
    glUniformMatrix4fv(s.mViewProj, 1, GL_FALSE, &viewProj[0][0]);
    glUniform3fv(s.mSunL, 1, &sunL[0]);
    glUniform3fv(s.mSun, 1, &sun[0]);
    glUniform3fv(s.mSky, 1, &sky[0]);
    glUniform3fv(s.mHorizon, 1, &horizon[0]);
    glUniform3fv(s.mGround, 1, &ground[0]);
    glUniform1f(s.mHemiFloor, glm::clamp(m_ambientHemiFloor, 0.0f, 1.0f));
    glUniform3fv(glGetUniformLocation(s.model, "uSH"), 9, &s.sh[0][0]);
    glUniform4f(glGetUniformLocation(s.model, "uProbe"), s.shReady ? 1.0f : 0.0f, 0.0f, m_skyProbeIntensity * ambI, 0.0f);
    glUniform3fv(s.mCam, 1, &camPos[0]);
    glUniform1f(glGetUniformLocation(s.model, "uLinear"), hdr ? 1.0f : 0.0f);
    uploadWebLights(s.model, camPos);
    {
        // Tabelas como o Render.cpp monta o FrameUBO (sampleLut).
        auto lut = [](const SplineCurve& c, Vec4& lo, Vec4& hi, float minV, float maxV) {
            for (int i = 0; i < 8; ++i)
                (i < 4 ? lo : hi)[i & 3] = glm::clamp(c.evaluate(float(i) / 7.0f), minV, maxV);
        };
        Vec4 sc0, sc1, sm0, sm1, ts0, ts1;
        lut(m_normalScaleCurve, sc0, sc1, -1e9f, 1e9f);
        lut(m_normalSmoothCurve, sm0, sm1, -1e9f, 1e9f);
        lut(m_tessCurve, ts0, ts1, 1.0f, std::min(64.0f, m_tessLevelCap));
        float tessMax = 1.0f;
        for (int i = 0; i < 4; ++i) tessMax = std::max({tessMax, ts0[i], ts1[i]});
        const float distMax = std::max(m_normalDistMax, 1.0f);
        const bool tessOn = m_tessEnabled && m_tessPresetAllowed && tessMax > 1.001f;
        const GLuint m = s.model;
        glUniform4f(glGetUniformLocation(m, "uNd"), distMax, m_normalDistCurve ? 1.0f : 0.0f, m_normalMapScale,
                    m_normalSmoothing);
        glUniform4fv(glGetUniformLocation(m, "uNdScale0"), 1, &sc0[0]);
        glUniform4fv(glGetUniformLocation(m, "uNdScale1"), 1, &sc1[0]);
        glUniform4fv(glGetUniformLocation(m, "uNdSmooth0"), 1, &sm0[0]);
        glUniform4fv(glGetUniformLocation(m, "uNdSmooth1"), 1, &sm1[0]);
        glUniform4f(glGetUniformLocation(m, "uTess"), distMax, tessOn ? 1.0f : 0.0f, 0.0f, tessMax);
        glUniform4fv(glGetUniformLocation(m, "uTessLut0"), 1, &ts0[0]);
        glUniform4fv(glGetUniformLocation(m, "uTessLut1"), 1, &ts1[0]);
        glUniform1f(glGetUniformLocation(m, "uQuebra"), m_vulkan.shaderLite() ? 0.0f : 1.0f);
    }
    const auto& meshes = m_modelRenderer.getMeshes();
    for (const ModelInstance& inst : m_modelRenderer.getInstances()) {
        if (!inst.enabled || inst.viewHidden || inst.meshIndex >= meshes.size()) continue;
        const ModelMeshGPU& mesh = meshes[inst.meshIndex];
        if (!mesh.indexCount) continue;
        const uint8_t* vb = web::bufferBytes(mesh.vertexBuffer);
        const uint8_t* ib = web::bufferBytes(mesh.indexBuffer);
        if (!vb || !ib) continue;
        const GLuint vbo = glBufferFor(s, mesh.vertexBuffer, GL_ARRAY_BUFFER);
        const GLuint ibo = glBufferFor(s, mesh.indexBuffer, GL_ELEMENT_ARRAY_BUFFER);
        if (!vbo || !ibo) continue;

        uint32_t first = 0;
        std::memcpy(&first, ib + size_t(mesh.firstIndex) * 4, 4);
        TerrainVertex v0;
        std::memcpy(&v0, vb + (size_t(first) + size_t(mesh.vertexOffset)) * sizeof(TerrainVertex), sizeof(v0));
        DrawSlots d;
        d.base = v0.texIndex & kTexSlotMask;
        d.normal = v0.normalIndex;
        d.pbr = v0.pbrIndex;
        d.mask0 = v0.blendMaskIndex;
        d.mask1 = v0.blendMaskIndex2;
        const uint32_t packed[4] = {v0.splatTex01, v0.splatTex23, v0.splatTex45, v0.splatTex67};
        for (int l = 0; l < 8; ++l) d.layer[l] = (packed[l / 2] >> (16 * (l % 2))) & 0xFFFFu;
        const WebSceneGl::Splat* sp = d.mask0 ? splatFor(s, d.layer) : nullptr;
        const int splat = sp ? (d.mask1 ? 2 : 1) : 0;

        bindSlot(s.model, "uBase", 0, d.base, s.white);
        bindSlot(s.model, "uMask0", 1, d.mask0, s.white);
        bindSlot(s.model, "uMask1", 2, d.mask1, s.white);
        bindSlot(s.model, "uBaseN", 4, d.normal, s.white);
        bindSlot(s.model, "uBaseM", 5, d.pbr, s.white);
        static const float kSem[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, kZero[8] = {};
        bindArray(s.model, "uLayC", 6, sp ? sp->arr[0] : s.whiteArray);
        bindArray(s.model, "uLayN", 7, sp ? sp->arr[1] : s.whiteArray);
        bindArray(s.model, "uLayM", 8, sp ? sp->arr[2] : s.whiteArray);
        glUniform4fv(s.mIdx0, 1, sp ? sp->idx : kSem);
        glUniform4fv(s.mIdx1, 1, (sp ? sp->idx : kSem) + 4);
        glUniform4fv(s.mHasN0, 1, sp ? sp->hasN : kZero);
        glUniform4fv(s.mHasN1, 1, (sp ? sp->hasN : kZero) + 4);
        glUniform4fv(s.mHasM0, 1, sp ? sp->hasM : kZero);
        glUniform4fv(s.mHasM1, 1, (sp ? sp->hasM : kZero) + 4);
        glUniform1i(s.mSplat, splat);
        glUniform4f(s.mMat, mesh.metallicScale, mesh.roughnessScale, m_defaultRoughness,
                    m_normalMapInvertY ? 1.0f : 0.0f);
        auto hasBase = [](uint32_t normal, uint32_t pbr) {
            return Vec2(normal && web::slotTexture(normal) ? 1.0f : 0.0f, pbr && web::slotTexture(pbr) ? 1.0f : 0.0f);
        };
        const Vec2 hb = hasBase(d.normal, d.pbr);
        glUniform2fv(s.mHasBase, 1, &hb[0]);
        glUniformMatrix4fv(s.mModel, 1, GL_FALSE, &inst.transform[0][0]);
        glUniform4f(s.mUvXform, inst.uvScale.x, inst.uvScale.y, inst.uvTranslate.x, inst.uvTranslate.y);

        // Sem base vertex no GLES 3.0: o deslocamento entra no ponteiro do atributo.
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const size_t base = size_t(mesh.vertexOffset) * sizeof(TerrainVertex);
        auto attr = [base](GLuint loc, GLint n, size_t off) {
            glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex),
                                  reinterpret_cast<const void*>(base + off));
        };
        attr(0, 3, offsetof(TerrainVertex, position));
        attr(1, 2, offsetof(TerrainVertex, texCoord));
        attr(2, 3, offsetof(TerrainVertex, normal));
        attr(3, 2, offsetof(TerrainVertex, blendMaskUV));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        if (splat) {
            glDrawElements(GL_TRIANGLES, GLsizei(mesh.indexCount), GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(size_t(mesh.firstIndex) * 4));
            continue;
        }
        for (const WebSceneGl::BaseRun& run : baseRunsFor(s, mesh, vb, ib)) {
            bindSlot(s.model, "uBase", 0, run.tex, s.white);
            bindSlot(s.model, "uBaseN", 4, run.normal, s.white);
            bindSlot(s.model, "uBaseM", 5, run.pbr, s.white);
            const Vec2 rb = hasBase(run.normal, run.pbr);
            glUniform2fv(s.mHasBase, 1, &rb[0]);
            glDrawElements(GL_TRIANGLES, GLsizei(run.count), GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(size_t(run.first) * 4));
        }
    }
    for (GLuint loc = 1; loc < 4; ++loc) glDisableVertexAttribArray(loc);

    // ---- lava (um desenho por lago, como o laco do Render.cpp)
    if (m_water.m_lavaIndexCount > 0) {
        const GLuint lvb = glBufferFor(s, m_water.m_lavaVertexBuffer, GL_ARRAY_BUFFER);
        const GLuint lib = glBufferFor(s, m_water.m_lavaIndexBuffer, GL_ELEMENT_ARRAY_BUFFER);
        if (lvb && lib) {
            glUseProgram(s.lava);
            glUniformMatrix4fv(glGetUniformLocation(s.lava, "uViewProj"), 1, GL_FALSE, &viewProj[0][0]);
            glUniform1f(glGetUniformLocation(s.lava, "uT"), float(m_timer.elapsed()));
            glUniform1f(glGetUniformLocation(s.lava, "uLinear"), hdr ? 1.0f : 0.0f);
            glBindBuffer(GL_ARRAY_BUFFER, lvb);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WaterVertex),
                                  reinterpret_cast<const void*>(offsetof(WaterVertex, position)));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(WaterVertex),
                                  reinterpret_cast<const void*>(offsetof(WaterVertex, texCoord)));
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lib);
            for (uint32_t li = 0; li < m_lavaLiquids.size() && li < m_water.lavaSurfaceCount(); ++li) {
                const LoadedMap::MapLiquid& lava = m_lavaLiquids[li];
                float lift = 0.0f;
                if (li < m_lavaSources.size() && m_lavaSources[li].map &&
                    m_lavaSources[li].index < m_lavaSources[li].map->liquids.size())
                    lift = m_lavaSources[li].map->liquids[m_lavaSources[li].index].level - m_lavaSources[li].builtLevel;
                glUniform1f(glGetUniformLocation(s.lava, "uLift"), lift);
                glUniform3f(glGetUniformLocation(s.lava, "uLava"), lava.emissive, lava.shoreHeat, lava.centerHeat);
                const auto& surf = m_water.m_lavaSurfaces[li];
                glDrawElements(GL_TRIANGLES, GLsizei(surf.indexCount), GL_UNSIGNED_INT,
                               reinterpret_cast<const void*>(size_t(surf.firstIndex) * 4));
            }
            glDisableVertexAttribArray(1);
        }
    }

    // ---- sprites (personagens), ja' cortados e ordenados pelo SpriteSystem
    const auto& sprites = m_spriteSystem.getSortedSprites();
    const uint32_t n = std::min<uint32_t>(m_spriteSystem.visibleCount(), uint32_t(sprites.size()));
    if (n) {
        glUseProgram(s.sprite);
        glUniformMatrix4fv(s.sView, 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(s.sProj, 1, GL_FALSE, &proj[0][0]);
        const float pitchNorm = glm::degrees(m_camera.orbitPitch()) / 89.0f;
        // A spline guarda graus (sprite_vert.glsl faz radians(u_spriteTilt)).
        glUniform1f(s.sTilt, glm::radians(m_charConfig.billboardTiltSpline.evaluate(pitchNorm)));
        // O sprite olha para a camera: luz de frente, metade do sol.
        const Vec3 light = ambient + sun * 0.5f;
        glUniform3fv(s.sLight, 1, &light[0]);
        glUniform1f(glGetUniformLocation(s.sprite, "uLinear"), hdr ? 1.0f : 0.0f);
        glUniform1i(glGetUniformLocation(s.sprite, "uTex"), 0);
        glBindBuffer(GL_ARRAY_BUFFER, s.quad);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glActiveTexture(GL_TEXTURE0);
        for (uint32_t i = 0; i < n; ++i) {
            const Sprite& sp = sprites[i];
            const GLuint tex = web::slotTexture(sp.texIndex);
            if (!tex) continue;
            glBindTexture(GL_TEXTURE_2D, tex);
            const uint32_t f = static_cast<uint32_t>(sp.flags);
            const bool flip = ((f & static_cast<uint32_t>(SpriteFlags::FlipX)) != 0) !=
                              ((f & static_cast<uint32_t>(SpriteFlags::PngSprite)) != 0);
            glUniform3fv(s.sAnchor, 1, &sp.anchorPoint[0]);
            glUniform2fv(s.sSize, 1, &sp.size[0]);
            glUniform4fv(s.sRect, 1, &sp.texRect[0]);
            glUniform1f(s.sFlip, flip ? 1.0f : 0.0f);
            glUniform4fv(s.sTint, 1, &sp.tintColor[0]);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }
    glDisableVertexAttribArray(0);

    // ---- ceu no alvo HDR onde nao ha' geometria (antes das particulas e do
    // bloom, como no desktop): o skybox.frag real com o bloco que o
    // ProceduralSkyBackend monta, so' com a rotacao da camera.
    bool skyInHdr = false;
    if (hdr && s.sky) {
        alignas(16) unsigned char params[256];
        const Mat4 invSky = glm::inverse(proj * Mat4(glm::mat3(view)));
        const size_t n = m_skybox.skyParams(m_dayNightCycle, invSky, params);
        if (n) {
            glBindBuffer(GL_UNIFORM_BUFFER, s.skyUbo);
            glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(n), params, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, s.skyUbo);
            glUseProgram(s.sky);
            GLint depthFunc = GL_LESS;
            glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL); // o fullscreen.vert escreve z = 1: so' onde o fundo ficou vazio
            glDepthMask(GL_FALSE);
            glDisable(GL_BLEND);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDepthMask(GL_TRUE);
            glDepthFunc(GLenum(depthFunc));
            skyInHdr = true;
        }
    }

    // ---- particulas: o VfxSystem conta quem nasce (como no desktop), a CPU
    // simula e desenha por cima da cena, antes do pos (que expoe e faz nevoa).
    m_vfx.setLighting(Vec4(env.sun.direction, env.sun.intensity), Vec4(env.sun.color, 1.0f),
                      Vec4(env.ambientSkyColor, env.ambientIntensity));
    m_vfx.update(m_timer.deltaTime(), m_camera, VkExtent2D{uint32_t(viewport[2]), uint32_t(viewport[3])});
    if (m_vfx.active()) {
        s.vfx.step(m_vfx.slotData(), m_vfx.slotCount(), m_vfx.spawnRequests(), m_timer.deltaTime(), m_vfx.simTime(),
                   std::min<uint32_t>(m_vfx.capacity(), kWebVfxMaxParticles));
        if (hdr) {
            WebVfxView vv;
            vv.viewProj = m_camera.projNoJitter() * view;
            vv.camPos = camPos;
            vv.camRight = Vec3(view[0][0], view[1][0], view[2][0]);
            vv.camUp = Vec3(view[0][1], view[1][1], view[2][1]);
            vv.nearPlane = m_camera.nearPlane();
            vv.proj11 = std::abs(m_camera.projNoJitter()[1][1]);
            vv.sunDir = Vec4(env.sun.direction, env.sun.intensity);
            vv.sunColor = env.sun.color;
            vv.ambient = Vec4(env.ambientSkyColor, env.ambientIntensity);
            const uint32_t lowQuads = s.vfx.buildQuads(m_vfx.slotData(), m_vfx.slotCount(), vv, s.vfxVerts, s.vfxTex);
            drawWebParticles(s, viewProj, m_camera.nearPlane(), m_camera.farPlane(), viewport[2], viewport[3],
                             lowQuads);
        }
    } else {
        s.vfx.clear();
    }

    glDisable(GL_DEPTH_TEST);
    if (hdr) renderWebPost(s, viewProj, env, GLuint(screenFb), skyInHdr);
    glBindVertexArray(0);
}

// Parametros do pos como o Render.cpp monta o PostSettings: nevoa do clima,
// vencida pela do script (world.set_fog) ou a do .env; exposicao automatica
// pela estimativa do orcamento de luz (o histograma da GPU nao existe aqui).
// Sonda de ceu como a do desktop (Render.cpp: 6 faces de 32x32 a cada 8
// quadros, SkyProbe): o mesmo skybox.frag desenha cada face com a matriz do
// SkyProbe, a CPU le as faces e projeta em SH9 pela conta do sky_sh.comp.
void Engine::updateWebSkyProbe(WebSceneGl& s) {
    constexpr int kFace = 32;
    if (!s.sky || !m_skyProbeEnabled) return;
    if (!s.probeFbo) {
        glGenTextures(1, &s.probeTex);
        glBindTexture(GL_TEXTURE_2D, s.probeTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kFace * 6, kFace, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &s.probeFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s.probeFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.probeTex, 0);
        s.probeOk = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        ERUPTION_LOG_WARN("[WEB] sonda de ceu: %s", s.probeOk ? "RGBA32F" : "sem alvo float, gradiente");
    }
    if (!s.probeOk) return;
    const size_t floats = size_t(kFace) * 6 * kFace * 4;
    if (s.probeFence) {
        // Leitura anterior: copia so' quando pronta, sem esperar.
        const GLenum st = glClientWaitSync(s.probeFence, 0, 0);
        if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) return;
        glDeleteSync(s.probeFence);
        s.probeFence = nullptr;
        // Lido em linhas de 6 faces; o projectSH quer face por face.
        std::vector<float> row(floats), faces(floats);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s.probePbo);
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(floats * sizeof(float)), row.data());
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < kFace; ++y)
                std::memcpy(&faces[(size_t(f) * kFace * kFace + size_t(y) * kFace) * 4],
                            &row[(size_t(y) * kFace * 6 + size_t(f) * kFace) * 4], sizeof(float) * 4 * kFace);
        SkyProbe::projectSH(faces.data(), kFace, SkyProbe::kMaxRadiance, s.sh);
        s.shReady = true;
        return;
    }
    if (s.shReady && (s.probeFrame++ % 8u) != 0u) return;
    if (!s.probePbo) {
        glGenBuffers(1, &s.probePbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s.probePbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(floats * sizeof(float)), nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    GLint fb = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glBindFramebuffer(GL_FRAMEBUFFER, s.probeFbo);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s.sky);
    bool drew = true;
    for (uint32_t f = 0; f < 6 && drew; ++f) {
        alignas(16) unsigned char params[256];
        const size_t n = m_skybox.skyParams(m_dayNightCycle, SkyProbe::faceInvViewProj(f), params);
        drew = n > 0;
        if (!drew) break;
        glBindBuffer(GL_UNIFORM_BUFFER, s.skyUbo);
        glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(n), params, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, s.skyUbo);
        glViewport(GLint(f) * kFace, 0, kFace, kFace);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (drew) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s.probePbo);
        glReadPixels(0, 0, kFace * 6, kFace, GL_RGBA, GL_FLOAT, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        s.probeFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fb));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glEnable(GL_DEPTH_TEST);
}

void Engine::renderWebPost(WebSceneGl& s, const Mat4& viewProj, const LightingEnvironment& env, unsigned screenFb,
                           bool skyInHdr) {
    // A caixa "Bloom" (efeito 2) manda, como no Render.cpp a cada quadro.
    if (m_renderEffects.size() > 2) m_postSettings.enableBloom = m_renderEffects[2]->isEnabled();
    static bool avisado = false;
    if (!avisado) {
        avisado = true;
        ERUPTION_LOG_WARN("[WEB] pos: bloom %s (limiar %.2f, forca %.2f), tonemap %d", m_postSettings.enableBloom ? "ligado" : "desligado",
                          m_postSettings.bloomThreshold, m_postSettings.bloomIntensity, m_postSettings.toneMapMode);
    }
    const GLuint bloom = m_postSettings.enableBloom ? renderBloom(s, m_postSettings.bloomThreshold) : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, screenFb);
    glViewport(0, 0, s.hdrW, s.hdrH);
    const WeatherParams& w = m_weatherSystem.current();
    Vec3 fogColor = m_dayNightCycle.getFogColor();
    float fogStart = w.fogStart, fogEnd = w.fogEnd, fogOpacity = w.fogDensity;
    float fogHeight = w.fogHeight, fogFalloff = w.fogHeightFalloff;
    bool fogOn = w.fogDensity > 0.001f;
    const MapFog* fog = m_scriptFog.set ? &m_scriptFog
                      : (m_currentMap && m_currentMap->env.fog.set) ? &m_currentMap->env.fog : nullptr;
    if (fog) {
        fogOn = fog->opacity > 0.001f;
        fogColor = fog->color;
        fogStart = fog->start;
        fogEnd = fog->end;
        fogOpacity = fog->opacity;
        fogHeight = fog->height;
        fogFalloff = fog->heightFalloff;
    }
    float exposure = m_postSettings.exposure;
    // Exposicao travada (world.set_exposure ou "exposure" do .env), como no Render.cpp.
    const float lockedExposure = m_scriptExposure >= 0.0f ? m_scriptExposure
                               : (m_currentMap && m_currentMap->env.exposureLocked) ? m_currentMap->env.exposureValue
                                                                                     : -1.0f;
    if (lockedExposure >= 0.0f) {
        m_autoExposureCurrent = lockedExposure;
        exposure *= lockedExposure;
    } else if (m_autoExposureEnabled) {
        const float level = std::max(env.sun.intensity * 0.7f + m_ambientIntensity * 0.9f +
                                         (m_moonFillEnabled ? 0.05f : 0.0f), 0.05f);
        const float target = glm::clamp(m_autoExposureKey / level, m_autoExposureMin, m_autoExposureMax);
        const float rate = 1.0f - std::exp(-m_timer.deltaTime() * m_autoExposureSpeed);
        m_autoExposureCurrent = glm::mix(m_autoExposureCurrent, target, glm::clamp(rate, 0.0f, 1.0f));
        exposure *= m_autoExposureCurrent;
    }
    const Mat4 inv = glm::inverse(viewProj);
    const Vec3 cam = m_camera.position();
    const Vec3 toSun = -glm::normalize(env.sun.direction);
    const Vec3 sunColor = env.sun.color * env.sun.intensity;
    const Vec3 top = m_dayNightCycle.getSkyTopColor(), horizon = m_dayNightCycle.getSkyHorizonColor();
    const Vec4 dim = m_uiCanvas.sceneDimNow();
    const float g = glm::mix(0.65f, 0.30f, glm::clamp(w.rainIntensity, 0.0f, 1.0f));

    glUseProgram(s.post);
    auto u = [&](const char* n) { return glGetUniformLocation(s.post, n); };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.hdrColor);
    glUniform1i(u("uHdr"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s.hdrDepth);
    glUniform1i(u("uDepth"), 1);
    glUniformMatrix4fv(u("uInvViewProj"), 1, GL_FALSE, &inv[0][0]);
    glUniform3fv(u("uCam"), 1, &cam[0]);
    glUniform4f(u("uFog"), fogColor.r, fogColor.g, fogColor.b, fogOn ? fogEnd : 0.0f);
    glUniform4f(u("uFog2"), fogStart, fogOpacity, fogHeight, fogFalloff);
    glUniform4f(u("uSunFog"), toSun.x, toSun.y, toSun.z, g);
    glUniform3fv(u("uSkyTop"), 1, &top[0]);
    glUniform3fv(u("uSkyHorizon"), 1, &horizon[0]);
    glUniform3fv(u("uSunColor"), 1, &sunColor[0]);
    glUniform4f(u("uTone"), exposure, float(m_postSettings.toneMapMode), glm::clamp(w.cloudCoverage, 0.0f, 1.0f),
                dim.a);
    glUniform3f(u("uDim"), dim.r, dim.g, dim.b);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, bloom ? bloom : s.white);
    glUniform1i(u("uBloom"), 2);
    glUniform1f(u("uBloomK"), bloom ? m_postSettings.bloomIntensity : 0.0f);
    glUniform1f(u("uSkyHdr"), skyInHdr ? 1.0f : 0.0f);
    const LookConfig& look = m_lookConfig;
    glUniform4f(u("uGrade"), look.saturation, look.contrast, look.vignette, look.vignetteSoftness);
    glUniform4f(u("uTintSh"), look.shadowTint.r, look.shadowTint.g, look.shadowTint.b, look.shadowTintStrength);
    glUniform4f(u("uTintHi"), look.highlightTint.r, look.highlightTint.g, look.highlightTint.b, look.highlightTintStrength);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
}

} // namespace eruption

#endif
