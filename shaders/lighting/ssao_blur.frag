#version 450

// Bilateral 3x3 blur over the half-res SSAO target. Weighted by world-space
// distance so occlusion never bleeds across depth discontinuities (a prop's
// AO staying off the wall behind it).

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outAO;

layout(set = 0, binding = 2)  uniform sampler2D gbufferDepth;
layout(set = 0, binding = 12) uniform sampler2D ssaoRaw;

void main() {
    // Peso bilateral por PROFUNDIDADE em vez de distancia de mundo: o blur so'
    // precisa saber se dois pixels estao na mesma superficie, e o depth
    // responde isso sem exigir um attachment RGBA16F inteiro no G-buffer.
    float dC = texture(gbufferDepth, inUV).r;
    vec2 texel = 1.0 / vec2(textureSize(ssaoRaw, 0));

    float total = 0.0;
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 suv = inUV + vec2(float(x), float(y)) * texel;
            float ao = texture(ssaoRaw, suv).r;
            float dS = texture(gbufferDepth, suv).r;
            // Tolerancia escala com a profundidade porque o depth e' nao linear.
            float tol = max(dC * 0.0015, 2e-5);
            float dd = (dS - dC) / tol;
            float dw = exp(-dd * dd);
            total += ao * dw;
            wsum += dw;
        }
    }
    outAO = (wsum > 1e-4) ? (total / wsum) : 1.0;
}
