#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform BrightPush {
    float threshold;
} push;

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;

void main() {
    vec2 uv = fragUV;
    vec3 color = texture(hdrTexture, uv).rgb;
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float brightness = max(luminance - push.threshold, 0.0);
    outColor = vec4(color * brightness, 1.0);
}
