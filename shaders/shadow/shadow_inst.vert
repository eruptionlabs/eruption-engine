#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// Variante INSTANCIADA do shadow.vert, usada SO pelo ModelRenderer. O terreno e
// a chuva continuam no shadow.vert com mvp por push: mudar o shader deles para
// ler SSBO quebraria quem desenha com instanceCount=1 sem bindar o set 1.
//
// Por que existe: a sombra pagava um vkCmdPushConstants + vkCmdDrawIndexed por
// CASTER por CASCATA (~4,9 ms de CPU em parana_field). Agora a matriz de cada
// caster vai num SSBO e cada malha vira um draw instanciado por cascata.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in uint inTexIndex;
layout(location = 4) in uint inMatId;
layout(location = 5) in uint inColor;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out flat uint outTexIndex;

// windParams e swayAmount: adicionados pra sombra de vegetacao acompanhar o
// balanco que model.vert aplica no G-buffer (docs/pedidos.md item 4). Antes
// so' cascadeVP existia aqui - a arvore balancava, a sombra dela ficava
// cravada. Offsets EXPLICITOS porque o lado C++ (ModelRenderer::renderShadow)
// escreve nesses mesmos bytes com tres vkCmdPushConstants separados.
layout(push_constant) uniform PushConstants {
    layout(offset = 0)  mat4 cascadeVP;
    // Mesma convencao do FrameUBO.windParams (ver model.vert): xyz = vetor de
    // vento em mundo (y reaproveitado pra carregar a escala de balanco, o
    // vento e' sempre horizontal), w = tempo proprio do vento.
    layout(offset = 64) vec4 windParams;
    // mesh.swayAmount do draw ATUAL. Constante por run: um run instanciado
    // agrupa sempre a MESMA malha (ver ModelRenderer::renderShadow), entao um
    // push por inicio de run basta - nao precisou ir pro SSBO por instancia.
    layout(offset = 80) float swayAmount;
};

layout(set = 1, binding = 0, std430) readonly buffer ShadowInstances {
    mat4 u_model[];
};

// Copia de applyWindSway (shaders/gbuffer/model.vert). MESMOS coeficientes de
// proposito: a sombra tem que descrever a mesma curva que a malha, senao
// descola visivelmente do tronco/copa ao vento forte. Ver model.vert para o
// raciocinio por tras de cada termo (engaste em h^2, teto de 65% linear,
// resposta a forca, duas frequencias de oscilacao etc).
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

void main() {
    mat4 model = u_model[gl_InstanceIndex];
    vec4 worldPos = model * vec4(inPosition, 1.0);

    // Mesma logica de model.vert: altura acima da BASE da instancia (nao a
    // local do modelo), porque as malhas sao escaladas e a base e' o engaste.
    float instBaseY = (model * vec4(0.0, 0.0, 0.0, 1.0)).y;
    worldPos.xyz = applyWindSway(worldPos.xyz, worldPos.y - instBaseY, swayAmount);

    gl_Position = cascadeVP * worldPos;
    outTexCoord = inTexCoord;
    outTexIndex = inTexIndex;
}
