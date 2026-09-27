#version 450

layout(location = 0) in vec2 iUV;
layout(location = 1) in float iAlpha;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 params;
    vec4 params2;
    vec4 params5; // x=rainShadowSoftness, y=cloudBaseHeight, z=windOffsetX, w=windOffsetY
    vec4 screenSize;
    vec4 worldBounds; // rain heightmap bounds
    vec4 cloudWorldBounds; // cloud coverage bounds
    vec4 cloudParams; // x=layer, y=threshold, z=weatherCoverage, w=splashOpacity
} push;

void main() {
    if (iAlpha <= 0.0) discard;

    // Debug viz mode (params2.z = -1): the vertex shader emits ONE big quad
    // per spawn region. The splash pipeline blends ADDITIVELY (src*alpha +
    // dst), so a plain red output would keep the scene's green/blue and read
    // yellowish. Negative green/blue SUBTRACTS the destination's g/b, which
    // clamps to 0 downstream -> literal RGB(255,0,0) on the ground, exactly
    // the "is there pure red where it rains" check the author wants.
    if (push.params2.z < 0.0) {
        vec2 dc = iUV * 2.0 - 1.0;
        if (length(dc) > 1.0) discard;
        outColor = vec4(1.0, -10.0, -10.0, 1.0);
        return;
    }

    vec2 center = iUV * 2.0 - 1.0;
    float r = length(center);
    if (r > 1.0) discard;

    // Ripple ring shape (thicker band so it reads at gameplay zoom)
    float ring = smoothstep(0.85, 0.55, r) * smoothstep(0.10, 0.30, r);
    // Bright core flash early in the ripple's life sells the impact.
    float core = 1.0 - smoothstep(0.0, 0.30, r);
    // Boost: the chain intensity*fade*density*opacity lands ~0.2 — invisible.
    // RESPINGO = ANEL, nao disco (2026-09-05). Antes: nucleo solido (core*0.8)
    // num alpha ja' x2,2 e cor x3,0 -> centro ~5 em HDR = disco branco
    // estourado do tamanho da particula, dezenas sobre a folhagem molhada
    // ("cintilacao saturada na chuva"; some ao dar zoom porque params2.y
    // encolhe a particula). Isolado com ERUPTION_TEST_NO_SPLASH (65 blobs
    // -> 0). Agora anel dominante, nucleo fraco, sem ganho de HDR: fica
    // abaixo do limiar do bloom e le como agua batendo, nao como lampada.
    float alpha = clamp(max(ring, core * 0.30) * iAlpha * 1.2, 0.0, 1.0);

    alpha *= push.cloudParams.w;

    vec3 splashColor = vec3(0.92, 0.95, 1.0);
    outColor = vec4(splashColor * alpha * 1.1, alpha);
}
