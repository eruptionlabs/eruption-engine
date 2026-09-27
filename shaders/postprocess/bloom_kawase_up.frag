#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTexture;
layout(set = 0, binding = 1) uniform sampler2D addTexture;

layout(push_constant) uniform Push {
    vec2 srcSize;
    float lod;
    float pad;
} push;

// Dual Kawase upsample with addition of original pyramid level
// 9-tap tent filter + additive combine
void main() {
    vec2 uv = fragUV;
    vec2 texel = 1.0 / push.srcSize;
    
    vec4 d = texel.xyxy * vec4(1.0, 1.0, -1.0, 0.0);
    
    vec3 sum = vec3(0.0);
    
    // 4 large corners
    sum += texture(srcTexture, uv - d.xy).rgb;
    sum += texture(srcTexture, uv - d.wy).rgb;
    sum += texture(srcTexture, uv + d.wy).rgb;
    sum += texture(srcTexture, uv + d.xy).rgb;
    
    // 4 small corners / edges
    sum += texture(srcTexture, uv + d.zy).rgb * 2.0;
    sum += texture(srcTexture, uv + d.xw).rgb * 2.0;
    sum += texture(srcTexture, uv - d.xw).rgb * 2.0;
    sum += texture(srcTexture, uv - d.zy).rgb * 2.0;
    
    // Center
    sum += texture(srcTexture, uv).rgb * 4.0;
    
    vec3 upsampled = sum / 16.0;
    
    // Add original pyramid detail at this level
    vec3 add = texture(addTexture, uv).rgb;
    
    outColor = vec4(upsampled + add, 1.0);
}
