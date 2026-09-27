#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTexture;

void main() {
    vec2 uv = fragUV;
    vec2 texel = 1.0 / textureSize(srcTexture, 0);
    
    vec3 result = vec3(0.0);
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    
    for (int i = -4; i <= 4; i++) {
        float w = weights[abs(i)];
        result += texture(srcTexture, uv + vec2(texel.x * i, 0.0)).rgb * w;
    }
    outColor = vec4(result, 1.0);
}
