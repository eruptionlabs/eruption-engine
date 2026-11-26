// MODULO DE FRAGMENTO MINIMO dos modelos (preset "model_frag_min_module"). E' o caminho
// MODEL_MIN do model_frag.glsl (preset "model_shader_min") sozinho num modulo: o mesmo
// resultado, pixel por pixel, mas sem o resto do shader no mesmo modulo. A constante de
// especializacao MODEL_MIN ja' tirava o codigo, mas o modulo inteiro era compilado e o driver
// reservava registradores pelo pior caminho. Medido no PowerVR GE8320 (Moto G22, escala 1/2,
// mobile_legacy): G-buffer 145 ms -> 107 ms (-26%), imagem identica.
// So' serve a malha que nao e' chao com splat (ModelRenderer::pickPipeline) e sem tesselacao
// (o ajuste da normal por derivada de tela da banda perto nao esta' aqui).
// Sem velocidade de objeto (o alvo so' existe com FSR) e sem spec constants.
#include "common/noise.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in flat uint inTexIndex;
layout(location = 5) in vec4 inColor;
layout(location = 14) in float inEmissiveStrength;
layout(location = 18) in flat float inSway;
layout(location = 22) in flat uint inGroundVeg;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPBR;
layout(location = 3) out uint outMaterialID;
layout(location = 4) out vec4 outEmissive;

#include "common/slot_textures.glsl"
#include "gbuffer/model_push.glsl"
#include "gbuffer/model_frame_ubo.glsl"
#include "gbuffer/see_through.glsl"
#include "gbuffer/alpha_hash.glsl"

// Matriz de dither 4x4 (a mesma do model_frag.glsl).
const float dither[16] = float[](
    0.0, 0.5, 0.125, 0.625,
    0.75, 0.25, 0.875, 0.375,
    0.1875, 0.6875, 0.0625, 0.5625,
    0.9375, 0.4375, 0.8125, 0.3125
);

void main() {
    // Dither de fade e crossfade de LOD (push.alpha negativo = teste invertido), como no model_frag.glsl.
    if (push.alpha < 0.99) {
        float thr = abs(push.alpha);
        bool invert = push.alpha < 0.0;
        int x = int(gl_FragCoord.x) % 4;
        int y = int(gl_FragCoord.y) % 4;
        bool keep = invert ? (dither[x + y * 4] > thr) : (dither[x + y * 4] <= thr);
        if (!keep) discard;
    }
    // Ver atraves; chao = instancia de chao (balanco -1). Malha com splat nao chega aqui.
    if (seeThroughDiscard(inWorldPos, inNormal, inSway < -0.5)) discard;
    vec3 N = (length(inNormal) > 1e-8) ? normalize(inNormal) : vec3(0.0, 1.0, 0.0);
    // Perfil "flat_normals": normal da face, do lado da normal da malha.
    if ((uint(push._pad + 0.5) & 4u) != 0u) {
        vec3 nf = cross(dFdx(inWorldPos), dFdy(inWorldPos));
        if (dot(nf, nf) > 1e-20) N = normalize(nf) * (dot(nf, N) < 0.0 ? -1.0 : 1.0);
    }
    vec4 texColor = slotTextureGrad(inTexIndex, inTexCoord, dFdx(inTexCoord), dFdy(inTexCoord));
    if (texColor.a < alphaCutoff(groundDense(inGroundVeg))) discard;
    vec3 aMin = texColor.rgb;
    if (inColor.a < 0.999) aMin *= inColor.rgb;
    outAlbedo = vec4(aMin, 1.0);
    outNormal = vec4(N * 0.5 + 0.5, 1.0);
    outPBR = vec4(0.5, 0.85, 0.0, 0.0);
    outMaterialID = uint(clamp((inWorldPos.y + 500.0) / 2000.0, 0.0, 1.0) * 255.0 + 0.5);
    outEmissive = vec4(aMin * inEmissiveStrength, 1.0);
}
