// Shared noise utilities for the cloud coverage map.
// Both CPU and GPU backends should produce visually similar results.

#ifndef CLOUD_COVERAGE_COMMON_GLSL
#define CLOUD_COVERAGE_COMMON_GLSL

float ccHash(float n) {
    return fract(sin(n + 0.9898) * 43758.5453);
}

float ccHash2(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float ccValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = ccHash2(i);
    float b = ccHash2(i + vec2(1.0, 0.0));
    float c = ccHash2(i + vec2(0.0, 1.0));
    float d = ccHash2(i + vec2(1.0, 1.0));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float ccFbm(vec2 p, int octaves) {
    float total = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    float maxValue = 0.0;
    for (int i = 0; i < octaves; ++i) {
        total += ccValueNoise(p * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return total / maxValue;
}

#endif // CLOUD_COVERAGE_COMMON_GLSL
