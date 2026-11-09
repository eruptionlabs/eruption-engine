#version 450
// Borrao bilateral separavel da luz volumetrica: 7 amostras gaussianas,
// peso cortado pela diferenca de profundidade (o raio de um poste nao vaza
// para o ceu atras dele). Apaga o dither de volumetric_sun.frag.
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth; // D32, sem filtro

layout(push_constant) uniform Push {
    vec2 dir;    // (1,0) ou (0,1) em texels do alvo
    float nearPlane;
    float farPlane;
} pc;

float linearDepth(float d) {
    return pc.nearPlane * pc.farPlane / (pc.farPlane + d * (pc.nearPlane - pc.farPlane));
}

void main() {
    const float w[4] = float[](0.2270, 0.1945, 0.1216, 0.0541);
    vec2 texel = 1.0 / vec2(textureSize(src, 0));
    float z0 = linearDepth(texture(sceneDepth, fragUV).r);
    vec4 acc = texture(src, fragUV) * w[0];
    float wsum = w[0];
    for (int i = 1; i < 4; ++i) {
        for (int s = -1; s <= 1; s += 2) {
            vec2 uv = fragUV + pc.dir * texel * float(i * s) * 1.5;
            float z = linearDepth(texture(sceneDepth, uv).r);
            float wz = w[i] * exp(-abs(z - z0) / max(0.05 * z0, 1.0));
            acc += texture(src, uv) * wz;
            wsum += wz;
        }
    }
    outColor = acc / max(wsum, 1e-4);
}
