#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_input;

layout(push_constant) uniform PixelPerfectPush {
    vec2 inputSize;
    vec2 outputSize;
    float pixelScale;
    float gridIntensity;
} push;

float pixelGrid(vec2 uv, vec2 resolution) {
    vec2 pixelCoord = uv * resolution;
    vec2 grid = abs(fract(pixelCoord - 0.5) - 0.5) / fwidth(pixelCoord);
    float line = min(grid.x, grid.y);
    return 1.0 - min(line, 1.0);
}

void main() {
    vec2 screenPos = gl_FragCoord.xy;
    float scaleX = floor(push.outputSize.x / push.inputSize.x);
    float scaleY = floor(push.outputSize.y / push.inputSize.y);
    float integerScale = floor(min(scaleX, scaleY));
    if (integerScale < 1.0) integerScale = 1.0;
    if (push.pixelScale > 0.0) integerScale = push.pixelScale;

    vec2 scaledSize = push.inputSize * integerScale;
    vec2 offset = (push.outputSize - scaledSize) * 0.5;

    if (screenPos.x < offset.x || screenPos.x >= offset.x + scaledSize.x ||
        screenPos.y < offset.y || screenPos.y >= offset.y + scaledSize.y) {
        outColor = vec4(0.05, 0.05, 0.08, 1.0);
        return;
    }

    vec2 inScaled = (screenPos - offset) / integerScale;
    vec2 uv = inScaled / push.inputSize;
    vec3 color = texture(u_input, uv).rgb;

    if (push.gridIntensity > 0.0) {
        float grid = pixelGrid(uv, push.inputSize) * push.gridIntensity;
        color *= 1.0 - grid;
    }

    outColor = vec4(color, 1.0);
}
