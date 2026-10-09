#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 v_uv;
layout(location = 1) in flat uint v_texIndex;
layout(location = 2) in flat uint v_flags;
layout(location = 3) in vec4 v_tint;
layout(location = 4) in vec3 v_worldPos;
layout(location = 5) in vec3 v_normal;
layout(location = 6) in flat uint v_paletteIndex;
layout(location = 7) in flat uint v_normalTexIndex;
layout(location = 8) in flat uint v_mrahwTexIndex;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_pbr;
layout(location = 3) out uint o_materialId;
layout(location = 4) out vec4 o_emissive;

layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 u_view;
    mat4 u_projection;
    mat4 u_viewProjection;
    mat4 u_inverseView;
    mat4 u_inverseProjection;
    vec3 u_cameraPos;
    float u_time;
    vec2 u_screenResolution;
    float u_nearPlane;
    float u_farPlane;
    uint u_frameIndex;
    uint u_debugMode;
    float u_spriteExposure;
    float u_giIntensity;
    float u_giAmbientFloor;
    float u_spriteTilt;
    float u_shadowHeightScale;
    float u_spriteNormalYMix;
    float u_normalMapScale;
    float u_normalMapInvertY;
    float u_normalSmoothing;
    float u_defaultRoughness;
    float u_defaultMetallic;
};

float srgbToLinear(float c) {
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

vec3 srgbToLinear(vec3 c) {
    return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b));
}

mat3 computeTBN(vec3 N, vec3 worldPos, vec2 uv) {
    vec3 dPosX = dFdx(worldPos);
    vec3 dPosY = dFdy(worldPos);
    vec2 dUVX  = dFdx(uv);
    vec2 dUVY  = dFdy(uv);

    vec3 T = dPosX * dUVY.t - dPosY * dUVX.t;
    vec3 B = -dPosX * dUVY.s + dPosY * dUVX.s;

    T = normalize(T - N * dot(T, N));
    B = normalize(B - N * dot(B, N) - T * dot(B, T));

    return mat3(T, B, N);
}

void main() {
    vec4 albedo = texture(nonuniformEXT(u_textures[v_texIndex]), v_uv);

    // bit 8: usePalette
    if ((v_flags & 256u) != 0) {
        float idx = albedo.r * 255.0;
        if (idx < 0.5) discard;
        float u = (idx + 0.5) / 256.0;
        albedo = texture(nonuniformEXT(u_textures[v_paletteIndex]), vec2(u, 0.5));
        albedo.a = 1.0;
    }

    if (albedo.a < 0.01) discard;

    // Keep albedo in sRGB for both legacy and PBR paths so the base brightness
    // matches the pre-PBR branch. Only F0/specular benefits from linear-space
    // conversion; converting the whole albedo made the scene look unnaturally
    // dark compared to the legacy renderer.
    albedo.rgb *= v_tint.rgb;

    uint matId = (v_flags >> 3) & 0x1F;

    vec3 N = normalize(v_normal);
    if (v_normalTexIndex != 0u) {
        // Generated normal maps are kept out of sprite lighting to preserve the
        // original look; alpha is still sampled by the deferred pass elsewhere.
    }

    float roughness = 0.5;
    // Era 0.5: METADE METAL pra' TODO sprite sem mapa MRAH-W - ou seja, pra'
    // todo personagem, monstro e NPC do jogo, que sao pele, pano, couro e
    // papel. Meio-metal escurece o difuso pela metade e joga essa metade num
    // reflexo colorido pelo albedo - o personagem vira uma "banana metalica".
    // Sprite sem mapa e' dieletrico ate' prova em contrario: 0.
    float metallic = 0.0;
    if (v_mrahwTexIndex != 0u) {
        vec4 mrahw = texture(nonuniformEXT(u_textures[v_mrahwTexIndex]), v_uv);
        metallic  = mrahw.r;
        roughness = mrahw.g;
    }

    vec3 encodedNormal = N * 0.5 + 0.5;

    // Use Albedo.a to carry the Exposure multiplier for Ambient Pass (Deferred)
    o_albedo = vec4(albedo.rgb, u_spriteExposure);
    o_normal = vec4(encodedNormal, roughness);
    // Sprites use the same fallback source flag so they participate in the PBR
    // pipeline unless a real MRAH-W map is bound.
    o_pbr = vec4(0.5, roughness, metallic, 0.0);
    o_materialId = matId;
    o_emissive = vec4(0.0, 0.0, 0.0, 1.0);
}
