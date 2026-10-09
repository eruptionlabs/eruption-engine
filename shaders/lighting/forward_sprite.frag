#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inWorldPos;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in flat uint inTexIndex;
layout(location = 4) in vec4 inTint;
layout(location = 5) in flat uint inFlags;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_textures[ERUPTION_TEX_SLOTS];

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
    vec4 sunDir;          // xyz=dir, w=intensity
    vec4 sunColor;        // xyz=color, w=unused
    vec4 ambientSky;      // xyz=skyColor, w=ambientIntensity
    vec4 ambientGround;   // xyz=groundColor, w=unused
} frame;

void main() {
    vec4 texColor = texture(nonuniformEXT(u_textures[inTexIndex]), inUV);
    if (texColor.a < 0.01) discard;

    vec3 albedo = texColor.rgb * inTint.rgb;
    
    // Normal is already billboarded or passed from vertex shader
    vec3 N = normalize(inNormal);
    
    // 1. Directional Light
    vec3 L = normalize(-frame.sunDir.xyz);
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = frame.sunColor.rgb * NdotL * frame.sunColor.w;
    
    // 2. Hemispheric Ambient
    vec3 skyColor = frame.ambientSky.rgb;
    vec3 groundColor = frame.ambientGround.rgb;
    float hemiFactor = 0.5 + 0.5 * N.y;
    vec3 hemiAmbient = mix(groundColor, skyColor, hemiFactor) * frame.ambientSky.w;
    
    // 3. Final Lighting with emissive floor
    vec3 final = albedo * (diffuse + hemiAmbient + 0.3); // Add 0.3 minimum light
    
    outColor = vec4(final, texColor.a * inTint.a);
}
