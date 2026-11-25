// Parte comum das variantes de STREAM da sombra instanciada (spec 52, OE-03):
// o mesmo push, a mesma matriz por instancia e o mesmo balanco ao vento do
// shadow_inst.vert, que segue sendo o caminho do vertice inteiro (88 B).
// Os streams so' mudam DE ONDE vem a posicao/UV, nunca a conta.

layout(push_constant) uniform PushConstants {
    layout(offset = 0)  mat4 cascadeVP;
    layout(offset = 64) vec4 windParams;
    layout(offset = 80) float swayAmount;
    // Rajadas locais (ModelRenderer::setShadowGusts): as duas mais fortes, ver
    // shadow_gust.glsl. Escalares de proposito (sem alinhamento de vec).
    layout(offset = 84)  uint gA0;
    layout(offset = 88)  uint gB0;
    layout(offset = 92)  uint gC0;
    layout(offset = 96)  float gX0;
    layout(offset = 100) float gZ0;
    layout(offset = 104) float gX1;
    layout(offset = 108) float gZ1;
    layout(offset = 112) uint gA1;
    layout(offset = 116) uint gB1;
    layout(offset = 120) uint gC1;
};

layout(location = 6) in vec4 inModel0;
layout(location = 7) in vec4 inModel1;
layout(location = 8) in vec4 inModel2;
layout(location = 9) in vec4 inModel3;

// Copia de applyWindSway (shadow_inst.vert / gbuffer/model.vert): MESMOS
// coeficientes, senao a sombra descola da copa ao vento.
vec3 applyWindSway(vec3 worldPos, float localHeight, float sway) {
    if (sway <= 1e-4) return worldPos;

    vec3 wind = windParams.xyz;
    float swayScale = max(windParams.y, 0.0);
    wind.y = 0.0;
    float strength = length(wind.xz);
    if (strength <= 1e-4 || swayScale <= 0.0) return worldPos;
    vec3 windDir = wind / strength;

    float t = windParams.w;
    float phase = dot(worldPos.xz, vec2(0.0731, 0.0917));
    float slow = sin(t * 1.9 + phase);
    float fast = sin(t * 5.7 + phase * 2.3);
    float osc = slow * 0.65 + fast * 0.35;

    float response = 0.30 + 0.70 * clamp(strength, 0.0, 1.0);

    float h = max(localHeight, 0.0);
    float bend = min(h * h * sway * swayScale, h * sway * swayScale * 0.65) * response;

    float gust = 0.55 + 0.45 * osc;
    worldPos.xz += windDir.xz * bend * gust;
    worldPos.y -= bend * 0.12 * gust;
    return worldPos;
}

#include "shadow/shadow_gust.glsl"

vec4 shadowClipPos(vec3 position) {
    mat4 model = mat4(inModel0, inModel1, inModel2, inModel3);
    vec4 worldPos = model * vec4(position, 1.0);
    float instBaseY = (model * vec4(0.0, 0.0, 0.0, 1.0)).y;
    worldPos.xyz = applyWindSway(worldPos.xyz, worldPos.y - instBaseY, swayAmount);
    if (swayAmount > 1e-4) {
        const float hG = max(worldPos.y - instBaseY, 0.0);
        worldPos.xyz = shadowGustOne(worldPos.xyz, hG, inModel3.xz, gA0, gB0, gC0, gX0, gZ0);
        worldPos.xyz = shadowGustOne(worldPos.xyz, hG, inModel3.xz, gA1, gB1, gC1, gX1, gZ1);
    }
    return cascadeVP * worldPos;
}
