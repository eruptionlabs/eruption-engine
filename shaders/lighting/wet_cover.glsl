// Chuva so' onde ela chega e molhado local (incluido por ambient.frag e
// directional.frag depois do LightingUBO `lights`; as duas passadas tem de
// concordar).
//
// Binding 19: a grade de cima da chuva do WeatherRenderer (R32_UINT, bits de
// float da altura + 10000 do que esta' mais alto em cada ponto, a mesma que
// tira a gota e o respingo de debaixo do telhado em weather_overlay.frag).
// Ponto abaixo do que o cobre fica seco; fora da grade (ou sem ela), molha.
layout(set = 0, binding = 19) uniform usampler2D rainCoverTex;

// Quatro texels vizinhos, cada um comparado, misturados bilinear (como PCF):
// a grade tem ~2 u por texel e um so' texel deixava a borda seca em degraus.
float rainReachAt(vec3 wp) {
    if (lights.rainCover.z <= 0.0) return 1.0;
    vec2 uv = (wp.xz - lights.rainCover.xy) * lights.rainCover.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    ivec2 size = textureSize(rainCoverTex, 0);
    vec2 t = uv * vec2(size) - 0.5;
    ivec2 i0 = ivec2(floor(t));
    vec2 f = t - vec2(i0);
    float r[4];
    for (int k = 0; k < 4; ++k) {
        ivec2 c = clamp(i0 + ivec2(k & 1, k >> 1), ivec2(0), size - 1);
        float topY = uintBitsToFloat(texelFetch(rainCoverTex, c, 0).r) - 10000.0;
        r[k] = smoothstep(topY - lights.rainCover.w, topY, wp.y);
    }
    return mix(mix(r[0], r[1], f.x), mix(r[2], r[3], f.x), f.y);
}

// Molhado das areas do script (world.wet_area): disco de borda macia, o
// maior vence.
float wetAreasAt(vec3 wp) {
    float w = 0.0;
    int n = int(lights.wetAreaParams.x);
    for (int i = 0; i < n; ++i) {
        vec4 a = lights.wetAreas[i];
        w = max(w, a.w * (1.0 - smoothstep(0.6 * a.z, a.z, length(wp.xz - a.xy))));
    }
    return w;
}
