// PROFUNDIDADE DO SPRITE ACIMA DO RELEVO. Incluido pelos vertex shaders de
// sprite em billboard (sprite_vert.glsl, sprite_forward.vert,
// sprite_layer.vert); os parametros sao membros da FrameUBO de cada um.
//
// A tesselacao (model.tese) desloca o chao em (h - 0,5) * amplitude: a crista
// sobe ate' meia amplitude acima da altura em que o personagem esta' em pe' e,
// na frente dele, cobria os pes (12 u nos presets de PC: terra por cima do
// sapato). Deslocar so' para baixo resolvia os pes mas deixava todo objeto
// pousado no chao flutuando e o chao inteiro dentro da sombra do mapa de
// sombra, que nao e' deslocado (medido no mapa base de teste: brilho medio
// do chao perto da camera 90 -> 63). Aqui so' a PROFUNDIDADE do quadro
// anda para a camera, pelo tanto que a crista mais alta pode subir naquela
// distancia: o relevo continua igual e nao esconde mais o pe'. Sem
// tesselacao o resultado e' zero e nada muda.

// Curva do fator em 16 amostras de meia precisao (2 por float, nos mesmos 8
// floats da LUT de antes: o UBO nao muda). Nos em t = (i/15)^1.5: mais
// densos perto da camera, onde a curva importa (8 amostras uniformes davam um
// no a cada 470 u e achatavam a curva do editor). Linear entre nos: continua.
float spriteReliefLut8(vec4 a, vec4 b, float t) {
    float x = pow(clamp(t, 0.0, 1.0), 0.6666667) * 15.0;
    int i = int(floor(x));
    float f = x - float(i);
    float v[8] = float[8](a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w);
    int i0 = min(i, 15), i1 = min(i + 1, 15);
    vec2 p0 = unpackHalf2x16(floatBitsToUint(v[i0 >> 1]));
    vec2 p1 = unpackHalf2x16(floatBitsToUint(v[i1 >> 1]));
    return mix(((i0 & 1) == 0) ? p0.x : p0.y, ((i1 & 1) == 0) ? p1.x : p1.y, f);
}

// Distancia, ao longo do raio da camera, que a crista do relevo pode tomar
// na frente de um pe' em `anchor` (mundo). Mesma curva e normalizacao da
// amplitude que model.tese usa; a crista do chao (fator 1) e' 0,5 * amplitude
// de altura, e o raio que desce a theta graus a atravessa em altura / sin.
float spriteReliefLift(vec3 anchor, vec3 cameraPos, vec4 tessParams, vec4 lut0, vec4 lut1) {
    if (tessParams.y < 0.5 || tessParams.z <= 1e-5) return 0.0;
    vec3 ray = anchor - cameraPos;
    float d = length(ray);
    float f = spriteReliefLut8(lut0, lut1, d / max(tessParams.x, 1.0));
    float amp = clamp((f - 1.0) / max(tessParams.w - 1.0, 1e-3), 0.0, 1.0);
    float crest = 0.5 * tessParams.z * amp;
    float sinDown = max(-ray.y / max(d, 1e-3), 0.2);
    return crest / sinDown;
}

// Fator que leva um ponto de vista (view space) `lift` unidades para a camera
// ao longo do proprio raio, sem passar da metade do caminho.
float spriteReliefPull(float viewDist, float lift) {
    return lift > 0.0 ? max(1.0 - lift / max(viewDist, 1e-3), 0.5) : 1.0;
}
