#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_color;
layout(set = 0, binding = 1) uniform sampler2D u_depth;
layout(set = 0, binding = 2) uniform sampler2D u_shadow;

layout(push_constant) uniform GodRaysPush {
    vec4 lightDirIntensity; // xyz = light dir, w = intensity
    vec4 lightColorScattering; // xyz = color, w = scattering
    int numSamples;
    float decay;
    float exposure;
    float weight;
    float screenWidth;
    float screenHeight;
    mat4 invViewProj;
} push;

void main() {
    vec2 texelSize = 1.0 / vec2(push.screenWidth, push.screenHeight);
    // Project light direction to screen space (simplified)
    vec3 lightDir = normalize(push.lightDirIntensity.xyz);
    vec2 lightScreen = vec2(0.5) + lightDir.xy * 0.4;
    vec2 deltaUV = fragUV - lightScreen;
    vec2 delta = deltaUV / float(push.numSamples);

    vec2 rayUV = fragUV;
    float illumination = 0.0;
    float decay = 1.0;

    for (int i = 0; i < push.numSamples; i++) {
        rayUV -= delta;
        float rayDepth = texture(u_depth, rayUV).r;
        // u_shadow is a 2-cascade side-by-side atlas; the legacy full-range
        // content lives in the left half (c0), so remap x into it.
        float shadow = 1.0 - texture(u_shadow, vec2(rayUV.x * 0.5, rayUV.y)).r;
        float depthFade = smoothstep(0.0, 0.1, rayDepth);
        illumination += shadow * decay * depthFade * push.weight;
        decay *= push.decay;
    }

    illumination *= push.lightColorScattering.w;
    vec3 sceneColor = texture(u_color, fragUV).rgb;
    vec3 rays = push.lightColorScattering.rgb * illumination * push.lightDirIntensity.w * push.exposure;
    outColor = vec4(sceneColor + rays, 1.0);
}
