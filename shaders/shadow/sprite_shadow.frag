#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 3) in flat uint v_paletteIndex;
layout(location = 4) in flat float v_fade;

// Fade por pontilhado (characters.fade, alfa da tinta): Bayer 4x4 na tela,
// parado no tempo. O sprite e' opaco no G-buffer (deferred), entao some
// pixel a pixel em vez de misturar; alfa 1 nunca corta, 0 corta tudo.
bool fadeCut(float a) {
    const float bayer[16] = float[](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                    3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    return a < (bayer[p.y * 4 + p.x] + 0.5) / 16.0;
}

#define ERUPTION_SLOT_NEAREST // atlas de sprite e paleta sem filtro
#include "common/slot_textures.glsl"

void main() {
    if ((v_flags & 512u) == 0) discard; // SpriteFlags::CastShadow = 1 << 9

    uint texIdx = nonuniformEXT(v_texIndex);
    vec4 albedo = slotTexture(texIdx, v_uv);
    
    // Palettization support for alpha test (bit 8: UsePalette = 256)
    if ((v_flags & 256u) != 0) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        float u = (idx + 0.5) / 256.0;
        albedo = slotTexture(v_paletteIndex, vec2(u, 0.5));
        albedo.a = 1.0; // Indexed palettes often have alpha=0 for all entries
    }
    
    // Mesmo pontilhado do sprite: a sombra do sol some junto com ele.
    if (albedo.a < 0.01 || fadeCut(v_fade)) discard;
}
