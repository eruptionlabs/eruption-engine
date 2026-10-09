#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 3) in flat uint v_paletteIndex;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

void main() {
    if ((v_flags & 512u) == 0) discard; // SpriteFlags::CastShadow = 1 << 9

    uint texIdx = nonuniformEXT(v_texIndex);
    vec4 albedo = texture(u_textures[texIdx], v_uv);
    
    // Palettization support for alpha test (bit 8: UsePalette = 256)
    if ((v_flags & 256u) != 0) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        float u = (idx + 0.5) / 256.0;
        albedo = texture(u_textures[nonuniformEXT(v_paletteIndex)], vec2(u, 0.5));
        albedo.a = 1.0; // Indexed palettes often have alpha=0 for all entries
    }
    
    if (albedo.a < 0.01) discard;
}
