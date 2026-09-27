#version 450

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D shadowAccum;

// Applies the half-res accumulated cloud-shadow occlusion (stored in alpha)
// to the lit scene: outputs black with alpha A and lets the standard
// SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend multiply the scene by (1 - A),
// exactly as the per-cloud fullscreen shadow passes did at full resolution.
void main() {
    float a = texture(shadowAccum, fragUV).a;
    outColor = vec4(0.0, 0.0, 0.0, a);
}
