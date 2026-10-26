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
// Precisa de u_seeThrough/u_seeThrough2/u_seeThrough3, u_seeThroughBand, u_cutaway, u_viewProjNoJitter
// e u_cameraPos.

const float kSeeThroughBayer[16] = float[](
    0.0, 0.5, 0.125, 0.625,
    0.75, 0.25, 0.875, 0.375,
    0.1875, 0.6875, 0.0625, 0.5625,
    0.9375, 0.4375, 0.8125, 0.3125
);

// PERTO DA CAMERA (camera.near_fade): o que fica a menos de u_seeThrough3.z
// da camera some, e ate' u_seeThrough3.w volta aos poucos, no mesmo padrao.
// A cerca colada na lente deixa de cobrir a tela inteira, borrada pelo DoF.
// Pela distancia, nao por tempo: a camera chegando perto ja' e' a transicao.
float nearFadeKeep(vec3 worldPos) {
    if (u_seeThrough3.w <= 0.0) return 1.0;
    return smoothstep(u_seeThrough3.z, u_seeThrough3.w, distance(worldPos, u_cameraPos));
}

float seeThroughKeep(vec3 worldPos) {
    if (u_seeThrough.z <= 0.0) return 1.0;
    vec4 c = u_viewProjNoJitter * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    float e = length((c.xy / c.w - u_seeThrough.xy) / u_seeThrough.zw);
    if (e >= 1.0) return 1.0;
    float h = u_seeThrough2.z;
    float front = 1.0 - smoothstep(u_seeThrough2.x - 0.5 * h, u_seeThrough2.x - 0.25 * h, distance(worldPos, u_cameraPos));
    float above = smoothstep(u_seeThrough2.y + 0.05 * h, u_seeThrough2.y + 0.25 * h, worldPos.y);
    float cut = (1.0 - smoothstep(0.6, 1.0, e)) * front * above;
    return mix(1.0, u_seeThrough2.w, cut);
}

// FAIXA ABAIXO DO FOCO (camera.see_through(.., "focus", {band = m})). Com a
// camera baixa (25 graus) o que tampa o grupo nao esta' so' em volta dele: e'
// a rocha entre a lente e o chao diante dele, que na tela fica ABAIXO do foco.
// O retangulo vai do centro da elipse ate' a borda da tela do lado da camera,
// com a largura da elipse x 1,2, e corta o que esta' mais perto da camera que
// o foco menos a margem. Chao fica: malha pintada (splat), instancia de chao,
// tudo ate' meio sprite acima dos pes e piso com normal para cima ate' um
// sprite (cortar um morro baixo abre buraco onde o mapa nao tem nada atras).
// Bordas em rampa (sem estalo).
// u_seeThroughBand: x = distancia de corte (0 desliga), y = sinal do "para
// baixo" em NDC, z = meia largura em NDC.
float seeThroughBandKeep(vec3 worldPos, vec3 n, bool ground) {
    if (u_seeThroughBand.x <= 0.0 || u_seeThrough.z <= 0.0) return 1.0;
    float h = u_seeThrough2.z;
    if (ground) return 1.0;
    if (worldPos.y < u_seeThrough2.y + 0.5 * h) return 1.0;
    if (length(n) > 1e-6 && normalize(n).y > 0.6 && worldPos.y < u_seeThrough2.y + h) return 1.0;
    vec4 c = u_viewProjNoJitter * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec2 p = c.xy / c.w;
    float down = (p.y - u_seeThrough.y) * u_seeThroughBand.y;
    float hw = u_seeThroughBand.z;
    float across = 1.0 - smoothstep(0.8 * hw, hw, abs(p.x - u_seeThrough.x));
    float below = smoothstep(0.0, 0.25 * u_seeThrough.w, down);
    float d = distance(worldPos, u_cameraPos);
    float near = 1.0 - smoothstep(0.9 * u_seeThroughBand.x, u_seeThroughBand.x, d);
    return mix(1.0, u_seeThrough2.w, across * below * near);
}

// RECORTE POR VOLUME (world.set_cutaway): dentro da caixa ou do cilindro a
// malha sai no mesmo padrao, ate' a opacidade pedida; a borda suave vai da
// superficie do volume ate' `soft` para dentro. Distancia com sinal (negativa
// dentro), no espaco do volume girado em Y.
float cutawayKeep(vec3 worldPos) {
    float keep = 1.0;
    for (int k = 0; k < 4; ++k) {
        vec4 a = u_cutaway[3 * k];
        if (a.w < 0.5) continue;
        vec4 b = u_cutaway[3 * k + 1], c = u_cutaway[3 * k + 2];
        vec3 d = worldPos - a.xyz;
        vec2 xz = vec2(c.x * d.x + c.y * d.z, -c.y * d.x + c.x * d.z);
        float sd;
        if (a.w < 1.5) {
            vec3 q = abs(vec3(xz.x, d.y, xz.y)) - b.xyz;
            sd = length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
        } else {
            vec2 q = vec2(length(xz) - b.x, abs(d.y) - b.y);
            sd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
        }
        float inside = 1.0 - smoothstep(-max(b.w, 1e-3), 0.0, sd);
        keep = min(keep, mix(1.0, c.z, inside));
    }
    return keep;
}

// n = normal da malha, ground = malha de chao pintada (splat): a faixa nao
// corta chao.
bool seeThroughDiscard(vec3 worldPos, vec3 n, bool ground) {
    float keep = min(min(seeThroughKeep(worldPos), nearFadeKeep(worldPos)), cutawayKeep(worldPos));
    keep = min(keep, seeThroughBandKeep(worldPos, n, ground));
    if (keep >= 1.0) return false;
    // Indice do Bayer relativo a um ponto fixo do mundo (u_seeThrough3.xy):
    // preso a gl_FragCoord, o pontilhado ficava parado na tela e o chao e a
    // parede escorregavam por baixo quando a camera andava ("rasteja" -
    // medido na bancada do jogo: sem ver-atraves o residuo do chao caia 35%).
    // Com a camera quase ortografica (fov 20) o padrao fica preso a superficie.
    ivec2 p = ivec2(floor(gl_FragCoord.xy - u_seeThrough3.xy)) & 3;
    return kSeeThroughBayer[p.x + p.y * 4] >= keep;
}
