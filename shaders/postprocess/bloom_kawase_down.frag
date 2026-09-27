#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTexture;

layout(push_constant) uniform Push {
    vec2 srcSize;
    float lod;
    float pad;
} push;

// Dual Kawase downsample
// Samples 13 taps in a cross + corners pattern
void main() {
    vec2 uv = fragUV;
    vec2 texel = 1.0 / push.srcSize;
    
    vec3 sum = vec3(0.0);
    float totalWeight = 0.0;
    
    // Center
    sum += texture(srcTexture, uv).rgb * 4.0;
    totalWeight += 4.0;
    
    // Cross (up, down, left, right at half texel distance)
    sum += texture(srcTexture, uv + vec2(-texel.x,  0.0)).rgb * 2.0;
    sum += texture(srcTexture, uv + vec2( texel.x,  0.0)).rgb * 2.0;
    sum += texture(srcTexture, uv + vec2( 0.0, -texel.y)).rgb * 2.0;
    sum += texture(srcTexture, uv + vec2( 0.0,  texel.y)).rgb * 2.0;
    totalWeight += 8.0;
    
    // Corners (diagonal)
    sum += texture(srcTexture, uv + vec2(-texel.x, -texel.y)).rgb * 1.0;
    sum += texture(srcTexture, uv + vec2( texel.x, -texel.y)).rgb * 1.0;
    sum += texture(srcTexture, uv + vec2(-texel.x,  texel.y)).rgb * 1.0;
    sum += texture(srcTexture, uv + vec2( texel.x,  texel.y)).rgb * 1.0;
    totalWeight += 4.0;
    
    outColor = vec4(sum / totalWeight, 1.0);
}
