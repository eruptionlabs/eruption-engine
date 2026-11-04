// Textura por slot (o número que o motor dá a cada textura registrada).
// Os shaders leem sempre por estas funções, nunca pelo descritor direto:
//   slotTexture(s, uv)  slotTextureBias(s, uv, bias)  slotTextureLod(s, uv, lod)
//   slotTextureGrad(s, uv, dx, dy)  slotTextureSize(s, lod)
//   slotQueryLod(s, uv)   (este e os dois primeiros, só no fragmento)
//
// Dois jeitos, escolhidos na compilação (o .nb.spv é a segunda variante):
//  - padrão: um array de texturas sem limite (descriptor indexing);
//  - ERUPTION_NO_BINDLESS: GPU sem indexação e com poucos samplers por estágio
//    (PowerVR/Mali de celular). As texturas de conteúdo são copiadas, num
//    tamanho só, para as camadas de um sampler2DArray; a camada é coordenada,
//    então nenhum array de samplers é indexado. As sRGB guardam o valor
//    codificado e a decodificação é feita aqui. Render targets e formatos que
//    não cabem em 8 bits ficam em poucas ligações diretas. Uma tabela por slot
//    diz onde cada textura está (TextureLayers).
//
// O shader só paga pelo que usa (o compilador de celular não aguenta um
// switch grande em toda leitura):
//   ERUPTION_SLOT_DIRECT   lê render targets por slot (água);
//   ERUPTION_SLOT_NEAREST  lê texturas registradas sem filtro (sprites).
// Sem eles, um slot direto lê cinza e um sem filtro sai filtrado.
// Quem inclui num estágio sem derivadas (vértice, tesselação) define antes
// ERUPTION_SLOT_NO_FRAGMENT_OPS.
#ifndef ERUPTION_SLOT_TEXTURES_GLSL
#define ERUPTION_SLOT_TEXTURES_GLSL

#ifndef ERUPTION_NO_BINDLESS

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];
#define slotTexture(s, uv) texture(u_textures[nonuniformEXT(s)], uv)
#define slotTextureBias(s, uv, bias) texture(u_textures[nonuniformEXT(s)], uv, bias)
#define slotTextureLod(s, uv, lod) textureLod(u_textures[nonuniformEXT(s)], uv, lod)
#define slotTextureGrad(s, uv, dx, dy) textureGrad(u_textures[nonuniformEXT(s)], uv, dx, dy)
#define slotTextureSize(s, lod) textureSize(u_textures[nonuniformEXT(s)], lod)
#define slotQueryLod(s, uv) textureQueryLod(u_textures[nonuniformEXT(s)], uv)

#else

// Mesma numeração de TextureLayers (k*Binding e o formato da tabela).
layout(set = 0, binding = 0) uniform sampler2DArray u_layers;
layout(set = 0, binding = 1) uniform sampler2DArray u_layersNearest;
// Segundo array de camadas (kind >= 16): o que passa do teto de camadas de um
// array. A versao sem filtro so' existe nos shaders que a usam (sprites).
// ERUPTION_NO_LAYER_B (diagnostico de build): o shader nao declara nem le o B.
#ifndef ERUPTION_NO_LAYER_B
layout(set = 0, binding = 11) uniform sampler2DArray u_layersB;
#ifdef ERUPTION_SLOT_NEAREST
layout(set = 0, binding = 12) uniform sampler2DArray u_layersNearestB;
#endif
#endif
// Por slot: camada nos 16 bits de baixo; em cima o tipo: bit 0 = sRGB,
// bit 1 = sem filtro; 4 + k = ligação direta k; 16 + bits = camada do 2º array.
layout(std430, set = 0, binding = 2) readonly buffer SlotTable { uint u_slotTable[]; };
#ifdef ERUPTION_SLOT_DIRECT
layout(set = 0, binding = 3) uniform sampler2D u_direct0;
layout(set = 0, binding = 4) uniform sampler2D u_direct1;
layout(set = 0, binding = 5) uniform sampler2D u_direct2;
layout(set = 0, binding = 6) uniform sampler2D u_direct3;
layout(set = 0, binding = 7) uniform sampler2D u_direct4;
layout(set = 0, binding = 8) uniform sampler2D u_direct5;
layout(set = 0, binding = 9) uniform sampler2D u_direct6;
layout(set = 0, binding = 10) uniform sampler2D u_direct7;
#endif

vec4 slotDecode(vec4 t, uint kind) {
    if ((kind & 1u) != 0u) {
        // Polinomio no lugar do pow por canal (erro < 0,5%): esta decodificacao
        // roda em toda amostra de cor, e o pow e' caro em GPU de celular.
        vec3 c = t.rgb;
        t.rgb = c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);
    }
    return t;
}

#ifndef ERUPTION_SLOT_NO_FRAGMENT_OPS
vec4 slotTexture(uint s, vec2 uv) {
    uint e = u_slotTable[s];
    uint kind = e >> 16u;
#ifdef ERUPTION_SLOT_DIRECT
    switch (kind) {
    case 4u: return texture(u_direct0, uv);
    case 5u: return texture(u_direct1, uv);
    case 6u: return texture(u_direct2, uv);
    case 7u: return texture(u_direct3, uv);
    case 8u: return texture(u_direct4, uv);
    case 9u: return texture(u_direct5, uv);
    case 10u: return texture(u_direct6, uv);
    case 11u: return texture(u_direct7, uv);
    }
#endif
    vec3 c = vec3(uv, float(e & 0xFFFFu));
#ifndef ERUPTION_NO_LAYER_B
    if (kind >= 16u) {
#ifdef ERUPTION_SLOT_NEAREST
        if ((kind & 2u) != 0u) return slotDecode(texture(u_layersNearestB, c), kind);
#endif
        return slotDecode(texture(u_layersB, c), kind);
    }
#endif
    if (kind >= 4u) return vec4(0.5, 0.5, 0.5, 1.0);
#ifdef ERUPTION_SLOT_NEAREST
    if ((kind & 2u) != 0u) return slotDecode(texture(u_layersNearest, c), kind);
#endif
    return slotDecode(texture(u_layers, c), kind);
}

vec4 slotTextureBias(uint s, vec2 uv, float bias) {
    uint e = u_slotTable[s];
    uint kind = e >> 16u;
#ifndef ERUPTION_NO_LAYER_B
    if (kind >= 16u) return slotDecode(texture(u_layersB, vec3(uv, float(e & 0xFFFFu)), bias), kind);
#endif
    if (kind >= 4u) return vec4(0.5, 0.5, 0.5, 1.0);
    return slotDecode(texture(u_layers, vec3(uv, float(e & 0xFFFFu)), bias), kind);
}

// Todas as camadas têm o mesmo tamanho: o LOD não depende do slot.
vec2 slotQueryLod(uint s, vec2 uv) {
    return textureQueryLod(u_layers, uv);
}
#endif

vec4 slotTextureLod(uint s, vec2 uv, float lod) {
    uint e = u_slotTable[s];
    uint kind = e >> 16u;
#ifdef ERUPTION_SLOT_DIRECT
    switch (kind) {
    case 4u: return textureLod(u_direct0, uv, lod);
    case 5u: return textureLod(u_direct1, uv, lod);
    case 6u: return textureLod(u_direct2, uv, lod);
    case 7u: return textureLod(u_direct3, uv, lod);
    case 8u: return textureLod(u_direct4, uv, lod);
    case 9u: return textureLod(u_direct5, uv, lod);
    case 10u: return textureLod(u_direct6, uv, lod);
    case 11u: return textureLod(u_direct7, uv, lod);
    }
#endif
#ifndef ERUPTION_NO_LAYER_B
    if (kind >= 16u) return slotDecode(textureLod(u_layersB, vec3(uv, float(e & 0xFFFFu)), lod), kind);
#endif
    if (kind >= 4u) return vec4(0.5, 0.5, 0.5, 1.0);
    return slotDecode(textureLod(u_layers, vec3(uv, float(e & 0xFFFFu)), lod), kind);
}

vec4 slotTextureGrad(uint s, vec2 uv, vec2 dx, vec2 dy) {
    uint e = u_slotTable[s];
    uint kind = e >> 16u;
#ifndef ERUPTION_NO_LAYER_B
    if (kind >= 16u) return slotDecode(textureGrad(u_layersB, vec3(uv, float(e & 0xFFFFu)), dx, dy), kind);
#endif
    if (kind >= 4u) return vec4(0.5, 0.5, 0.5, 1.0);
    return slotDecode(textureGrad(u_layers, vec3(uv, float(e & 0xFFFFu)), dx, dy), kind);
}

// Todas as camadas têm o mesmo tamanho; um render target direto, o dele.
ivec2 slotTextureSize(uint s, int lod) {
#ifdef ERUPTION_SLOT_DIRECT
    switch (u_slotTable[s] >> 16u) {
    case 4u: return textureSize(u_direct0, lod);
    case 5u: return textureSize(u_direct1, lod);
    case 6u: return textureSize(u_direct2, lod);
    case 7u: return textureSize(u_direct3, lod);
    case 8u: return textureSize(u_direct4, lod);
    case 9u: return textureSize(u_direct5, lod);
    case 10u: return textureSize(u_direct6, lod);
    case 11u: return textureSize(u_direct7, lod);
    }
#endif
    return textureSize(u_layers, lod).xy;
}

#endif // ERUPTION_NO_BINDLESS

#endif // ERUPTION_SLOT_TEXTURES_GLSL
