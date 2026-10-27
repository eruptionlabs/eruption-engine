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
// Precisa de u_seeThrough/u_seeThrough2/u_seeThrough3, u_seeThroughBand,
// u_seeThroughExtra, u_cutaway, u_viewProjNoJitter e u_cameraPos.

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

// FACE INGREME (normal.y < 0,6, fora do chao): a beirada de pedra na frente
// do grupo. Ela fica abaixo de meio sprite e perto do foco, onde o resto e'
// preservado (chao na altura dos pes, parede em que o boneco encosta), e
// cobria as pernas da montaria do lider (17% do grupo coberto a 25 graus).
// Face ingreme sai a partir de kSteepFront u na frente do foco e logo acima
// dos pes. Abaixo dos pes nao: com a camera acima do grupo, a linha ate' ele
// desce, e o que a cruza esta' acima dos pes; cortar ali so' abriria buraco.
const float kSteepFront = 4.0;
bool seeThroughSteep(vec3 n, bool ground) {
    return !ground && length(n) > 1e-6 && normalize(n).y < 0.6;
}

// Quanto corta uma elipse: ell = centro e raios em NDC, par = distancia
// camera-meio do boneco, altura dos pes, altura do sprite.
float seeThroughEllipse(vec2 p, float d, float y, bool steep, vec4 ell, vec4 par) {
    float e = length((p - ell.xy) / ell.zw);
    if (e >= 1.0) return 0.0;
    float h = par.z;
    float front = steep ? 1.0 - smoothstep(par.x - kSteepFront - 2.0, par.x - kSteepFront, d)
                        : 1.0 - smoothstep(par.x - 0.5 * h, par.x - 0.25 * h, d);
    float above = steep ? smoothstep(par.y + 0.02 * h, par.y + 0.08 * h, y)
                        : smoothstep(par.y + 0.05 * h, par.y + 0.25 * h, y);
    return (1.0 - smoothstep(0.6, 1.0, e)) * front * above;
}

float seeThroughKeep(vec3 worldPos, bool steep, bool ground) {
    if (u_seeThrough.z <= 0.0) return 1.0;
    vec4 c = u_viewProjNoJitter * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec2 p = c.xy / c.w;
    float d = distance(worldPos, u_cameraPos);
    // u_seeThroughBand.w = 1: centro no foco com personagens perto; so' as
    // elipses deles (abaixo) cortam, nunca a do foco (Render.cpp).
    float cut = u_seeThroughBand.w > 0.5 ? 0.0
                                         : seeThroughEllipse(p, d, worldPos.y, steep, u_seeThrough, u_seeThrough2);
    // Personagens perto do foco (centro no foco): cada um com a sua elipse,
    // e nelas toda face fora do chao a mais de kSteepFront u na frente dele
    // sai (a beirada e o topo da pedra logo na frente da montaria; o "1/4 de
    // sprite" da elipse do foco, num boneco grande, guardava 10 u de pedra).
    // "Na frente" no plano do chao (distancia XZ ate' a camera, contra a dos
    // pes): com a camera no alto, a pedra baixa na frente fica tao longe da
    // lente quanto o meio do boneco, e a distancia reta a guardava.
    float dxz = distance(worldPos.xz, u_cameraPos.xz);
    for (int k = 0; k < 6 && !ground; ++k) {
        vec4 ell = u_seeThroughExtra[2 * k];
        if (ell.z <= 0.0) break;
        cut = max(cut, seeThroughEllipse(p, dxz, worldPos.y, true, ell, u_seeThroughExtra[2 * k + 1]));
    }
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
float seeThroughBandKeep(vec3 worldPos, vec3 n, bool ground, bool steep) {
    if (u_seeThroughBand.x <= 0.0 || u_seeThrough.z <= 0.0 || u_seeThroughBand.w > 0.5) return 1.0;
    float h = u_seeThrough2.z;
    if (ground) return 1.0;
    if (worldPos.y < u_seeThrough2.y + (steep ? 0.02 : 0.5) * h) return 1.0;
    if (length(n) > 1e-6 && normalize(n).y > 0.6 && worldPos.y < u_seeThrough2.y + h) return 1.0;
    vec4 c = u_viewProjNoJitter * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec2 p = c.xy / c.w;
    float down = (p.y - u_seeThrough.y) * u_seeThroughBand.y;
    float hw = u_seeThroughBand.z;
    float across = 1.0 - smoothstep(0.8 * hw, hw, abs(p.x - u_seeThrough.x));
    float below = smoothstep(0.0, 0.25 * u_seeThrough.w, down);
    float d = distance(worldPos, u_cameraPos);
    // Face ingreme baixa: a partir de kSteepFront u na frente do foco (a
    // margem da faixa vale para o resto).
    float near = 1.0 - smoothstep(0.9 * u_seeThroughBand.x, u_seeThroughBand.x, d);
    if (steep && worldPos.y < u_seeThrough2.y + 0.5 * h)
        near = max(near, 1.0 - smoothstep(u_seeThrough2.x - kSteepFront - 2.0, u_seeThrough2.x - kSteepFront, d));
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
    bool steep = seeThroughSteep(n, ground);
    float keep = min(min(seeThroughKeep(worldPos, steep, ground), nearFadeKeep(worldPos)), cutawayKeep(worldPos));
    keep = min(keep, seeThroughBandKeep(worldPos, n, ground, steep));
    if (keep >= 1.0) return false;
    // Indice do Bayer relativo a um ponto fixo do mundo (u_seeThrough3.xy):
    // preso a gl_FragCoord, o pontilhado ficava parado na tela e o chao e a
    // parede escorregavam por baixo quando a camera andava ("rasteja" -
    // medido na bancada do jogo: sem ver-atraves o residuo do chao caia 35%).
    // Com a camera quase ortografica (fov 20) o padrao fica preso a superficie.
    ivec2 p = ivec2(floor(gl_FragCoord.xy - u_seeThrough3.xy)) & 3;
    return kSeeThroughBayer[p.x + p.y * 4] >= keep;
}
