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

layout(set = 0, binding = 0) uniform sampler2D u_textures[];

layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 inverseView;
    mat4 inverseProjection;
    vec3 cameraPos;
    float time;
    vec2 screenResolution;
    float nearPlane;
    float farPlane;
    uint frameIndex;
    uint debugMode;
    float spriteExposure;
    float giIntensity;
    float giAmbientFloor;
    float spriteTilt;
    float shadowHeightScale;
    float spriteNormalYMix;
    float normalMapScale;
    float normalMapInvertY;
    float normalSmoothing;
    float defaultRoughness;
    float defaultMetallic;
    
    // Lighting for Forward Sprites
    vec4 sunDir;          // xyz=dir, w=intensity
    vec4 sunColor;        // xyz=color, w=unused
    vec4 ambientSky;      // xyz=skyColor, w=ambientIntensity
    vec4 ambientGround;   // xyz=groundColor, w=unused
} frame;

layout(location = 0) out vec4 o_color;

void main() {
    vec4 albedo = texture(nonuniformEXT(u_textures[v_texIndex]), v_uv);
    
    if ((v_flags & 256u) != 0) {
        float idx = albedo.r;
        vec4 pal = texture(nonuniformEXT(u_textures[v_paletteIndex]), vec2(idx, 0.5));
        albedo.rgb = pal.rgb;
        albedo.a = 1.0; 
        if (idx < 0.001) discard; 
    }

    if (albedo.a < 0.01) discard;
    albedo.rgb *= v_tint.rgb;

    // --- Exact Map Lighting Rules ---
    
    // 1. Directional (Sun)
    // For NdotL we need L pointing TOWARDS the light source
    vec3 L = normalize(-frame.sunDir.xyz); 
    
    // NdotL calculation (matching directional.frag)
    float NdotL = max(dot(v_normal, L), 0.0);
    vec3 sunLit = frame.sunColor.rgb * NdotL * frame.sunDir.w;
    
    // 2. Hemispheric Ambient (matching ambient.frag)
    float hemiFactor = 0.5 + 0.5 * max(v_normal.y, 0.0);
    vec3 hemiAmbient = mix(frame.ambientGround.rgb, frame.ambientSky.rgb, hemiFactor);
    
    // Global Illumination logic from map
    // Applying Sprite Exposure to the environment/GI parameters as requested
    float giIntensity = frame.giIntensity * frame.spriteExposure;
    float giAmbientFloor = frame.giAmbientFloor * frame.spriteExposure;

    vec3 baseAmbient = hemiAmbient * giIntensity;
    vec3 ambientFloor = vec3(giAmbientFloor);
    vec3 finalAmbient = max(baseAmbient, ambientFloor);

    // 3. Final Result
    // Combine Sun + Ambient following the map's additive rules
    vec3 lightTotal = sunLit + finalAmbient;
    
    vec3 finalColor = albedo.rgb * lightTotal;
    
    o_color = vec4(finalColor, albedo.a * v_tint.a);
}
