// Alfa-teste com limiar por pixel (chave de preset alpha_hash, 0 = desligado = limiar fixo 0,1 de sempre).
// O G-buffer e o pre-passe de profundidade tem de usar o MESMO limiar: o G-buffer testa profundidade IGUAL, e um
// pixel descartado so' num dos dois deixa buraco. Hash de gradiente intercalado: deterministico por pixel, sem
// textura. Os texels de borda (alfa intermediario por filtro/mip) viram pontilhado que o FXAA e o upscale
// bicubico fundem, em vez de degrau duro a baixa resolucao. Precisa de model_frame_ubo.glsl antes.
float alphaCutoff() {
    if (u_alphaHash.x <= 0.0) return 0.1;
    float h = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return mix(0.1, clamp(h, 0.05, 0.95), u_alphaHash.x);
}
