// VER ATRAVES (camera.see_through). Screen-door: o fragmento entre a camera e
// o jogador, dentro da elipse em volta dele na tela, sai num padrao Bayer 4x4
// que deixa u_seeThrough2.w dele no centro e volta ao cheio na borda. O chao
// na altura dos pes fica inteiro (o piso na frente do boneco furava junto) e
// o que esta' a menos de 1/4 de sprite na frente dele tambem (a parede em que
// ele encosta). O pre-passe (model_depth.frag) e o G-buffer fazem a mesma
// conta: o pixel que um descarta o outro tambem descarta. A sombra nao passa
// por aqui: a parede continua sombreando.
// Transparencia por mascara de pixel em vez de mistura (Mulder, Groen e van
// Wijk, "Pixel Masks for Screen-Door Transparency", IEEE Visualization 1998):
// cabe no G-buffer, que nao mistura, sem ordenar nada; o limiar ordenado e' o
// de Bayer (1973), o mesmo do crossfade de LOD.
// Precisa de u_seeThrough/u_seeThrough2, u_viewProjNoJitter e u_cameraPos.

const float kSeeThroughBayer[16] = float[](
    0.0, 0.5, 0.125, 0.625,
    0.75, 0.25, 0.875, 0.375,
    0.1875, 0.6875, 0.0625, 0.5625,
    0.9375, 0.4375, 0.8125, 0.3125
);

bool seeThroughDiscard(vec3 worldPos) {
    if (u_seeThrough.z <= 0.0) return false;
    vec4 c = u_viewProjNoJitter * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return false;
    float e = length((c.xy / c.w - u_seeThrough.xy) / u_seeThrough.zw);
    if (e >= 1.0) return false;
    float h = u_seeThrough2.z;
    float front = 1.0 - smoothstep(u_seeThrough2.x - 0.5 * h, u_seeThrough2.x - 0.25 * h, distance(worldPos, u_cameraPos));
    float above = smoothstep(u_seeThrough2.y + 0.05 * h, u_seeThrough2.y + 0.25 * h, worldPos.y);
    float cut = (1.0 - smoothstep(0.6, 1.0, e)) * front * above;
    float keep = mix(1.0, u_seeThrough2.w, cut);
    // Indice do Bayer relativo a um ponto fixo do mundo (u_seeThrough3.xy):
    // preso a gl_FragCoord, o pontilhado ficava parado na tela e o chao e a
    // parede escorregavam por baixo quando a camera andava ("rasteja" -
    // medido na bancada do jogo: sem ver-atraves o residuo do chao caia 35%).
    // Com a camera quase ortografica (fov 20) o padrao fica preso a superficie.
    ivec2 p = ivec2(floor(gl_FragCoord.xy - u_seeThrough3.xy)) & 3;
    return kSeeThroughBayer[p.x + p.y * 4] >= keep;
}
