#version 450
// LUZ VOLUMETRICA DO SOL (renderer/VolumetricLight): marcha da camera ate' o
// pixel (no maximo max_distance) testando o mapa de sombra do sol a cada passo.
// O ar segue a nevoa de altura (densidade cai com exp acima de fogHeight);
// fase Henyey-Greenstein; dither IGN fixo (a captura repete igual) que o
// borrao bilateral (volumetric_blur.frag) apaga.
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;  // D32, sem filtro
layout(set = 0, binding = 1) uniform sampler2D sunShadowDepth;
// Comeco do ShadowUBO de directional.frag (DeferredLighting::ShadowUBO).
layout(set = 0, binding = 2) uniform SunShadowUBO {
    mat4 lightSpaceMats[2];
    mat4 nextLightSpaceMats[2];
    vec4 cascadeSplits;
    vec4 cascadeScales;  // x, y = texel em mundo da c0 e da c1
    vec4 shadowParams;
    vec4 shadowRanges;   // y = profundidade real da caixa, z = vies
};

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 camPos;      // xyz, w = distancia maxima da marcha
    vec4 toSun;       // xyz = direcao PARA o sol, w = anisotropia g
    vec4 radiance;    // rgb = sol x intensidade x tinta, w = passos
    vec4 medium;      // x = densidade, y = altura da nevoa, z = queda, w = livre
} pc;

// Sol (1) ou sombra (0) num ponto: um teste na cascata que o contem; fora das
// duas, sol (o atlas nao cobre: o ar longe fica aceso, como o chao).
float sunVisible(vec3 P) {
    for (int idx = 0; idx < 2; ++idx) {
        vec4 ls = lightSpaceMats[idx] * vec4(P, 1.0);
        vec3 c = ls.xyz / ls.w;
        if (c.z > 1.0 || any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0)))) continue;
        vec2 uv = clamp(vec2(c.x * 0.5 + 0.5 * float(idx), c.y), vec2(0.5 * float(idx), 0.0),
                        vec2(0.5 * float(idx) + 0.5, 1.0));
        float texelWorld = idx == 0 ? cascadeScales.x : cascadeScales.y;
        float bias = shadowRanges.z + texelWorld / max(shadowRanges.y, 100.0);
        return texture(sunShadowDepth, uv).r < c.z - bias ? 0.0 : 1.0;
    }
    return 1.0;
}

float phaseHG(float cosT, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
}

// Interleaved gradient noise (Jimenez 2014), parado no tempo.
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

void main() {
    float d = texture(sceneDepth, fragUV).r;
    vec4 h = pc.invViewProj * vec4(fragUV * 2.0 - 1.0, min(d, 0.9999999), 1.0);
    vec3 P = h.xyz / max(h.w, 1e-6);
    vec3 ray = P - pc.camPos.xyz;
    float dist = length(ray);
    vec3 dir = ray / max(dist, 1e-4);
    float L = d >= 1.0 ? pc.camPos.w : min(dist, pc.camPos.w);
    int steps = max(int(pc.radiance.w), 1);
    float dt = L / float(steps);
    float jitter = ign(gl_FragCoord.xy);
    float T = 1.0;
    float scat = 0.0;
    for (int i = 0; i < steps; ++i) {
        vec3 X = pc.camPos.xyz + dir * ((float(i) + jitter) * dt);
        float sigma = pc.medium.x * exp(-max(X.y - pc.medium.y, 0.0) * pc.medium.z);
        scat += T * sigma * sunVisible(X) * dt;
        T *= exp(-sigma * dt);
    }
    vec3 L_in = pc.radiance.rgb * phaseHG(dot(dir, pc.toSun.xyz), pc.toSun.w) * scat;
    outColor = vec4(L_in, 1.0 - T);
}
