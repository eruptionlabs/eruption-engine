#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

// Alvo reduzido do funil (premultiplicado) e profundidade da cena em resolucao cheia.
layout(set = 0, binding = 0) uniform sampler2D accum;
layout(set = 1, binding = 0) uniform sampler2D sceneDepth;

layout(push_constant) uniform Push {
    float nearZ;
    float farZ;
    vec2 invLow; // 1 / tamanho do alvo reduzido
} pc;

float linZ(float d) {
    return pc.nearZ * pc.farZ / (pc.farZ - d * (pc.farZ - pc.nearZ));
}

// Upsample ciente da profundidade. O funil marchou cada texel baixo contra a profundidade lida no
// centro dele (tornado.frag); na borda de um objeto o pixel cheio pertence a outra superficie que a
// do texel vizinho, e a bilinear simples levava a nuvem do fundo para cima da copa (linha escura).
// Cada um dos 4 texels pesa pela bilinear e pela semelhanca da sua profundidade com a do pixel cheio.
void main() {
    vec2 lowSize = 1.0 / pc.invLow;
    vec2 lc = fragUV * lowSize - 0.5;
    ivec2 i0 = ivec2(floor(lc));
    vec2 f = lc - vec2(i0);
    ivec2 lim = ivec2(lowSize) - 1;
    float zF = linZ(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 d = ivec2(k & 1, k >> 1);
        ivec2 ij = clamp(i0 + d, ivec2(0), lim);
        vec2 uv = (vec2(ij) + 0.5) * pc.invLow;
        float zT = linZ(textureLod(sceneDepth, uv, 0.0).r);
        float wb = (d.x == 1 ? f.x : 1.0 - f.x) * (d.y == 1 ? f.y : 1.0 - f.y);
        float wd = exp(-40.0 * abs(log(zT / zF))) + 1e-3;
        float w = wb * wd + 1e-6;
        sum += w * texelFetch(accum, ij, 0);
        wsum += w;
    }
    outColor = sum / wsum;
}
