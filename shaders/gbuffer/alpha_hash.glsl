// Alfa-teste com limiar por pixel (chave de preset alpha_hash, 0 = desligado = limiar fixo 0,1 de sempre).
// O G-buffer e o pre-passe de profundidade tem de usar o MESMO limiar: o G-buffer testa profundidade IGUAL, e um
// pixel descartado so' num dos dois deixa buraco. Hash de gradiente intercalado: deterministico por pixel, sem
// textura. Os texels de borda (alfa intermediario por filtro/mip) viram pontilhado que o FXAA e o upscale
// bicubico fundem, em vez de degrau duro a baixa resolucao. Precisa de model_frame_ubo.glsl antes.
// `dense` = densidade da copa por estado do chao (neve enche a folhagem): reduz o
// limiar proporcionalmente (0 = igual a antes; negativo = copa rarefeita, limiar maior).
// Pre-passe e G-buffer passam o MESMO valor (vem do mesmo varying flat).
// Estado do chao sobre a copa (inGroundVeg = 16 bits altos do varying inTexIndexPk, do model.vert): densidade (byte 0, com
// sinal, centesimos) e clareamento (byte 1).
float groundDense(uint g) { return float(int(g << 24u) >> 24) * 0.01; }
float groundLift(uint g) { return float((g >> 8u) & 0xFFu) * 0.01; }
float alphaCutoff(float dense) {
    float c = 0.1;
    if (u_alphaHash.x > 0.0) {
        float h = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        c = mix(0.1, clamp(h, 0.05, 0.95), u_alphaHash.x);
    }
    return c * (1.0 - dense);
}
