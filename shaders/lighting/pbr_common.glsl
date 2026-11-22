#ifndef PBR_COMMON_GLSL
#define PBR_COMMON_GLSL

const float PBR_PI = 3.14159265359;

// Standard Fresnel-Schlick with roughness modification used by IBL.
vec3 pbrFresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Plain Fresnel-Schlick for direct lighting.
vec3 pbrFresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (vec3(1.0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float pbrDistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PBR_PI * denom * denom);
}

float pbrGeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float pbrGeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return pbrGeometrySchlickGGX(NdotV, roughness) * pbrGeometrySchlickGGX(NdotL, roughness);
}

// ---------------------------------------------------------------------------
// Specular anti-aliasing (Tokuyoshi & Kaplanyan)
// ---------------------------------------------------------------------------
// O problema: quando a normal varia MUITO dentro de um unico pixel - malha
// densa ao longe, normal map forte, superficie curva no horizonte - o lobo
// especular fica menor que o pixel. Ai o pixel ora acerta ora erra o brilho
// conforme a camera anda, e isso e' o "blob branco que nada" que os comentarios
// abaixo descrevem.
//
// A solucao correta nao e' proibir superficie lisa: e' ALARGAR o lobo o
// suficiente para cobrir a variacao de normal que cabe no pixel. Mede-se essa
// variacao pela derivada de tela da normal e converte-se em rugosidade extra,
// somada em VARIANCIA. Superficie lisa e bem resolvida continua lisa; so'
// engorda onde de fato ha' detalhe sub-pixel.
//
// Substitui dois remendos que existiam so' para esconder o sintoma:
//   - piso de rugosidade em 0.045, que proibia qualquer superficie de ser
//     realmente polida;
//   - min(specular, 1.0), que cortava o pico do lobo e escurecia reflexo
//     legitimo de metal e agua.
float pbrGeometricRoughness(vec3 N, float roughness) {
    // Variancia da normal dentro do pixel, pelas derivadas de tela.
    vec3 dndu = dFdx(N);
    vec3 dndv = dFdy(N);
    float variance = 0.25 * (dot(dndu, dndu) + dot(dndv, dndv));
    if (variance <= 0.0) return roughness;

    // Soma em variancia (alpha = roughness^2), nao em rugosidade: e' assim que
    // dois lobos gaussianos se combinam. Somar rugosidade direto engordaria
    // demais e deixaria tudo fosco.
    float a = roughness * roughness;
    // Teto para nao explodir na silhueta, onde a derivada e' enorme por causa
    // da descontinuidade de profundidade e nao por micro-relevo real.
    float kernelRoughness = min(2.0 * variance, 0.18);
    float a2 = clamp(a * a + kernelRoughness, 0.0, 1.0);
    return sqrt(sqrt(a2));
}

// ---------------------------------------------------------------------------
// Compensacao de multi-scattering (Fdez-Aguera)
// ---------------------------------------------------------------------------
// O Cook-Torrance classico modela UMA reflexao no microrelevo. Numa superficie
// rugosa a luz bate varias vezes entre as micro-facetas antes de sair, e essa
// energia simplesmente sumia: metal escovado e pedra rugosa saiam escuros
// demais, e o erro cresce com a rugosidade.
//
// A compensacao devolve essa energia sem LUT nenhuma: usa o mesmo split-sum
// analitico ja' empregado no especular do ambiente para estimar o quanto se
// perdeu, e reinjeta.
vec3 pbrMultiScatterCompensation(vec3 F0, float roughness, float NdotV) {
    // Aproximacao analitica da integral do BRDF (Karis), a mesma ja' usada no
    // especular do ambiente em directional.frag - sem LUT, sem textura.
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572,  0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.04,  -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;

    // Ess = fracao da energia que o modelo de UM salto entrega. O que falta
    // para 1 e' a energia perdida nos saltos seguintes. Devolve-se como
    // MULTIPLICADOR (Kulla-Conty), nao como lobo somado: a energia perdida sai
    // na mesma direcao do lobo original, entao escalar e' a operacao certa e
    // nao ha' risco de introduzir um segundo pico.
    float Ess = clamp(ab.x + ab.y, 1e-3, 1.0);
    return vec3(1.0) + F0 * (1.0 / Ess - 1.0);
}

// MICROSOMBRA ("Micro Shadows", Brinck & Maximov, "The Technical Art of
// Uncharted 4", SIGGRAPH 2016 Advances). O AO da textura vira um cone de
// visibilidade e a luz passa se N.L sai pela boca dele; forma fechada:
//   micro = saturate(|N.L| + 2 ao^2 - 1)
// ao = 1 nao muda nada; ao = 0,5 com N.L = 0,7 cai a 0,2 - a fresta escurece
// SOB O SOL, que o AO (so' na luz ambiente, ~14% do brilho ao meio-dia) nao
// fazia. A versao do Chan (CoD WWII, SIGGRAPH 2018, eq. 58: cos = sqrt(1 - ao),
// vis = saturate(N.L / cos)^2) e' a mesma ideia mais branda: com o sol alto o
// disco cabe no cone e quase nada escurece - por isso esta.
// strength (0..1) mistura com "sem efeito"; contrast eleva o AO a esse expoente
// antes do cone (1 = AO como vem); floor e' a fracao da luz direta que a fresta
// mais fechada ainda recebe. O clamp seco da formula zera a luz (preto puro de
// borda dura: a junta vira linha); a curva suave + o piso mantem gradacao.
// Presets micro_shadow / micro_shadow_contrast / micro_shadow_floor.
float pbrMicroShadow(float NdotL, float ao, float strength, float contrast, float floorV) {
    float a = pow(clamp(ao, 0.0, 1.0), max(contrast, 1.0));
    float m = smoothstep(0.0, 1.0, clamp(abs(NdotL) + 2.0 * a * a - 1.0, 0.0, 1.0));
    m = mix(clamp(floorV, 0.0, 1.0), 1.0, m);
    return mix(1.0, m, clamp(strength, 0.0, 1.0));
}

// Full energy-conserving Cook-Torrance BRDF.
// albedo is expected to be in linear space.
// Returns radiance already multiplied by NdotL.
// Sprite sem mapa MRAH-W (pbr.r = 0,75 e rugosidade 1, sprite.frag): so' o
// difuso. A arte ja' traz o brilho pintado; o lobo especular de dieletrico
// (F0 0,04) somava por igual sobre a arte escura e a deixava cinza-lavada.
// Bloco de arte preta (albedo 18/255) sob o sol, quadro 120: ver o commit.
bool pbrSpriteDiffuseOnly(vec4 pbr) {
    return abs(pbr.r - 0.75) < 0.06 && pbr.g > 0.99;
}

vec3 pbrCookTorrance(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness, vec3 lightColor, float lightIntensity) {
    vec3 H = normalize(V + L);

    // Specular AA no lugar do piso fixo de rugosidade. O piso de 0.045 existia
    // para impedir que um texel de rugosidade ~0 virasse espelho perfeito e o
    // sol estourasse num blob branco que nadava pela superficie. Mas a causa
    // nao era a rugosidade baixa, era o lobo ficar menor que o pixel - e' isso
    // que pbrGeometricRoughness mede e corrige na origem. Com ele, agua parada
    // e metal polido voltam a poder ser realmente lisos.
    roughness = pbrGeometricRoughness(N, clamp(roughness, 0.0, 1.0));
    roughness = clamp(roughness, 0.012, 1.0); // so' contra divisao por zero

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    if (NdotL <= 0.0 || NdotV <= 0.0) return vec3(0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = pbrFresnelSchlick(max(dot(H, V), 0.0), F0);

    float D = pbrDistributionGGX(N, H, roughness);
    float G = pbrGeometrySmith(N, V, L, roughness);

    float denominator = 4.0 * NdotV * NdotL + 0.001;
    vec3 specular = (D * G * F) / denominator;

    // Devolve a energia dos saltos multiplos entre micro-facetas, que o modelo
    // de um salto perde. Sem isto, pedra rugosa e metal escovado saem escuros
    // demais, e o erro cresce com a rugosidade. E' multiplicador: a energia
    // perdida reaparece na mesma direcao do lobo.
    specular *= pbrMultiScatterCompensation(F0, roughness, NdotV);

    // Teto MUITO mais alto que o antigo min(spec, 1.0). Aquele cortava o pico
    // do lobo para conter o blob e, no caminho, escurecia reflexo legitimo de
    // metal e agua. O blob agora e' resolvido na origem pelo specular AA, entao
    // isto e' so' rede contra NaN/overflow.
    specular = min(specular, vec3(64.0));

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PBR_PI;

    return (diffuse + specular) * lightColor * lightIntensity * NdotL;
}

// ---------------------------------------------------------------------------
// Wet surface model (IGNIS, G2/G3)
// ---------------------------------------------------------------------------
// A water film on a surface does four things, in descending order of how much
// they matter to the eye:
//
//   1. It DARKENS the albedo. Light enters the film, scatters inside the
//      substrate, and a large part of it is trapped on the way out by total
//      internal reflection at the water/air boundary. Less light comes back,
//      so the surface reads darker and more saturated. This is the dominant
//      effect, and it is the one the engine was missing entirely -- which is
//      exactly why rain used to read as "shiny" instead of "wet".
//   2. It SMOOTHS the microsurface. The film fills the pores, so roughness
//      falls toward the roughness of the film itself -- proportionally to how
//      porous the material is, not to a fixed constant.
//   3. It adds its OWN specular layer. Water is a dielectric with F0 ~ 0.02,
//      slightly LOWER than the 0.04 default, so the grazing-angle flare comes
//      from the smoothness in (2) concentrating the lobe, not from extra F0.
//   4. It POOLS in cavities. Depressions go nearly mirror-smooth, exposed tops
//      stay merely damp.
//
// `metallic` is deliberately NOT touched. Water is a dielectric, not a
// conductor. The old (dead) applyClimatePbr pushed metallic toward 0.05, which
// steals energy from the diffuse albedo and tints the specular with the base
// colour -- lacquered plastic, not wet stone.
struct PbrWetness {
    vec3  albedo;     // linear albedo, darkened by the film
    float roughness;  // microsurface smoothed by the film
    float film;       // 0..1 how much film this pixel actually carries
};

// BEIRA D'AGUA: a onda avanca e recua pela praia; a faixa que ela cobre
// fica encharcada e seca em ~tau s depois que a agua recua. Sem estado: pela
// fase da onda sabe-se ha' quanto tempo a agua saiu de cada ponto. MASCARA: a
// frente e' recortada por ruido, e fora d'agua so' molham as manchas (ruido +
// cavidade do material): o ponto alto seca primeiro, a depressao segura a
// agua. waterY < -1e8 = sem agua.
//
// ALCANCE PELA INCLINACAO: a onda era medida em ALTURA (ate' 5 u acima do
// nivel). Na praia rasa 5 u de altura sao dezenas de u de chao - a agua ia
// ate' o fim da grade da beira e era cortada ali; no barranco subia 5 u e
// andava quase nada. Agora o alcance e' HORIZONTAL e depende da inclinacao
// media ate' a agua (altura / distancia, a distancia vem da grade da beira:
// beira = 1 - d / 24): praia rasa ~16 u, barranco ~3 u, gradiente no meio.
float shoreHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float shoreNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(shoreHash(i), shoreHash(i + vec2(1, 0)), f.x),
               mix(shoreHash(i + vec2(0, 1)), shoreHash(i + vec2(1, 1)), f.x), f.y);
}
const float kShoreGridReach = 24.0; // u em que a grade da beira chega a 0 (Engine::buildShoreMask)
// Onda: periodo medio de 10 s, fase da costa andando 1 volta a cada ~490 u
// (kShoreOmega, kShoreFase) e a frente em lobos por celula - tudo em
// common/shore_waves.glsl, o mesmo arquivo que o liquid.frag inclui. O tempo
// da lamina da' a volta em 10000 s (Render.cpp, multiplo do periodo).
#include "common/shore_waves.glsl"
// x = distancia horizontal ate' a agua com ruido na frente, y = alcance da
// onda nesse ponto (u), z = distancia sem o ruido (para achar o pe' do
// lobo, shoreOndaChao). beira = valor da grade (1 na agua visivel).
vec3 shoreReach(vec3 wp, float y, float beira) {
    float dGrade = (1.0 - clamp(beira, 0.0, 1.0)) * kShoreGridReach;
    // Perto da agua a distancia sai da altura do proprio pixel (praia de
    // ~1:8): a grade tem celulas de 2 u marcadas como agua ou nao, e a frente
    // da lamina seguia o contorno das celulas, em escada. A 6 u da agua volta
    // 'a grade, que sabe da distancia de verdade.
    float d = mix(max(y, 0.0) * 8.0, dGrade, smoothstep(2.0, 6.0, dGrade));
    float inclin = (max(y, 0.0) + 0.2) / (d + 2.0);           // tangente media ate' a agua
    float alcance = mix(16.0, 3.0, smoothstep(0.05, 0.7, inclin));
    float n1 = shoreNoise(wp.xz * 0.09) * 0.65 + shoreNoise(wp.xz * 0.31) * 0.35;
    return vec3(d + (n1 - 0.5) * 0.4 * alcance, alcance, d);
}
// A onda deste ponto de chao, uma vez por pixel: shoreWetness e shoreSwash
// pedem a mesma (mesmo wp, mesmo tempo) e os 4 lobos custam o dobro se
// contados duas vezes.
ShoreOnda g_shoreOnda;
vec4 g_shoreOndaChave = vec4(1.0e30);
ShoreOnda shoreOndaChao(vec3 wp, float t, vec3 r, vec2 fora) {
    vec4 chave = vec4(wp, t);
    if (chave != g_shoreOndaChave) {
        g_shoreOndaChave = chave;
        g_shoreOnda = shoreOndaEm(wp.xz - fora * r.z, t, max(r.x, 0.0) / r.y, vec2(-fora.y, fora.x));
    }
    return g_shoreOnda;
}
float shoreWetness(vec3 wp, float waterY, float t, float cavity, float beira, vec2 fora) {
    if (waterY < -1.0e8) return 0.0;
    // s para secar. Era 4,5 (1,5 a 6 s com a mascara): com periodo de ~7 s a
    // areia ja' estava seca quando a lamina recuava e nao se via a areia
    // molhada/brilhante atras dela (A/B com ERUPTION_TEST_PBR_DEBUG=256 igual).
    const float tau = 9.0;
    float y = wp.y - waterY;
    if (y < -0.5 || y > 14.0) return 0.0;
    vec3 r = shoreReach(wp, y, beira);
    float rel = clamp(r.x / r.y, 0.0, 1.0);
    // Tempo desde que a agua saiu: o menor entre os lobos que chegam aqui
    // (shoreOndaEm). Coberto = 0.
    float since = shoreOndaChao(wp, t, r, fora).seca;
    // Mascara de retencao: depressao (cavidade) e manchas finas seguram a agua.
    float n2 = shoreNoise(wp.xz * 0.6 + 17.0);
    float hold = clamp(0.55 * clamp(cavity, 0.0, 1.0) + 0.6 * n2 - 0.1, 0.0, 1.0);
    // Seca mais rapido onde segura menos (topo exposto) - 1,5 a 6 s.
    float tauM = tau * mix(0.33, 1.35, hold);
    float wet = exp(-since / tauM);
    wet = max(wet, 0.4 * hold * (1.0 - smoothstep(0.0, r.y, r.x)));
    // Fora do alcance da onda a mascara corta (sem faixa uniforme).
    wet *= mix(hold, 1.0, 1.0 - smoothstep(0.2, 0.8, rel));
    // A faixa que a onda varre nunca seca de todo entre uma onda e outra
    // (areia encharcada, escura e lisa). Depois da mascara: antes dela o piso
    // era multiplicado pelo hold e a areia molhada lia ~0,3 (quase seca).
    wet = max(wet, 0.8 * (1.0 - smoothstep(0.6, 1.0, rel)));
    return wet * (1.0 - smoothstep(r.y + 1.0, r.y + 5.0, r.x));
}

// LAVA NO CHAO (beira de um plano de lava, mesma grade da agua): x = coberto
// pela lava que sobe (crosta escura com fendas), y = borda incandescente na
// linha de contato, z = chao chamuscado acima dela.
// Medido na VERTICAL (altura acima do nivel), nao na horizontal como a onda:
// a lava fica em crateras de rocha ingreme, e a distancia horizontal da grade
// espalhava a borda por pedras inteiras (manchas laranja chapadas). A grade so'
// diz que ha' lava perto. Lava e' viscosa: sobe ate' ~1,2 u, devagar (~25 s),
// e nunca recua abaixo do nivel. Sem `invade` fica so' o contato.
vec3 lavaShore(vec3 wp, float lavaY, float t, float beira, bool invade) {
    if (lavaY < -1.0e8) return vec3(0.0);
    float y = wp.y - lavaY;
    if (y < -2.0 || y > 8.0) return vec3(0.0);
    float ph = 0.25 * t + dot(wp.xz, vec2(0.017, 0.013)) + shoreNoise(wp.xz * 0.05) * 3.0;
    float sobe = invade ? 1.2 * (0.5 + 0.5 * sin(ph)) * (0.6 + 0.4 * shoreNoise(wp.xz * 0.21)) : 0.0;
    float prof = sobe - y;                              // > 0: debaixo da lava
    float cobre = smoothstep(-0.12, 0.12, prof);
    float borda = exp(-abs(prof) * 5.0);
    float queimado = 1.0 - smoothstep(0.0, 4.5 + 2.0 * shoreNoise(wp.xz * 0.3), -prof);
    return vec3(cobre, borda, queimado) * smoothstep(0.0, 0.2, beira);
}
vec3 lavaShoreAlbedo(vec3 albedo, vec3 lv) {
    vec3 a = mix(albedo, albedo * vec3(0.30, 0.26, 0.24), 0.85 * lv.z); // chamuscado
    return mix(a, vec3(0.040, 0.032, 0.028), lv.x);                     // crosta
}
// Brilho proprio (linear, x brilho do liquido): fendas na crosta que sobe,
// a borda de contato e um calor fraco no chamuscado.
vec3 lavaShoreGlow(vec3 lv, vec3 wp, float t) {
    float n = shoreNoise(wp.xz * 0.8 + vec2(t * 0.04, -t * 0.03)) * 0.6 + shoreNoise(wp.xz * 2.3) * 0.4;
    float fenda = smoothstep(0.62, 0.78, n);
    float e = lv.y * 1.5 + lv.x * (0.03 + 0.9 * fenda) + lv.z * lv.z * 0.04;
    return vec3(1.0, 0.30, 0.05) * e;
}

// Areia molhada pela onda escurece bem mais que o pbrWetSurface da chuva
// (~15% na areia do mapa, que some no ruido da textura): areia encharcada
// reflete cerca de metade da luz difusa da seca. So' no chao virado pra cima.
vec3 shoreWetAlbedo(vec3 albedo, float molha, float ny) {
    return albedo * mix(1.0, 0.55, clamp(molha, 0.0, 1.0) * smoothstep(0.35, 0.8, ny));
}

// LAMINA DA ONDA no chao (mesma onda de shoreWetness). A superficie da agua
// e' um plano no nivel do mar, entao a onda subindo a praia nao aparecia.
// x = cobertura (borda nitida: o degrau da agua entrando na areia), y =
// altura de agua equivalente (cor/absorcao), z = espuma da frente (so' com
// comEspuma): renda que sobe na frente da lamina e, no recuo, fica em fios
// que somem.
// REPUXO: quanto (u de profundidade) a borda do mar desce abaixo do nivel
// com a onda recuada - 0 com a onda no alto, 0,3 u no fundo do recuo. O
// plano d'agua e' fixo; sem isso so' a lamina na areia ia e voltava e o mar
// nunca recuava de verdade. O liquid.frag da' o mesmo valor (shoreRecuoDe
// com a mesma onda, common/shore_waves.glsl): a agua fica transparente nessa
// faixa e mostra a areia molhada.

// fora = direcao (XZ) da agua para a terra ali (gradiente da grade da beira).
vec4 shoreSwash(vec3 wp, float waterY, float t, float ny, bool comEspuma, float beira, vec2 fora) {
    if (waterY < -1.0e8 || ny < 0.35) return vec4(0.0);
    float y = wp.y - waterY;
    // Abaixo do nivel a lamina continua: o fundo visto pela agua parada
    // recebe a mesma agua a mais (a onda acima do plano). Sem isso o chao
    // sob a agua voltava sem tom e a linha d'agua virava uma faixa.
    if (y < -6.0 || y > 14.0) return vec4(0.0);
    vec3 r = shoreReach(wp, y, beira);
    ShoreOnda ond = shoreOndaChao(wp, t, r, fora);
    float frente = r.y * ond.e1;                  // u de chao que a onda cobre agora (lobo maior)
    float prof = frente - r.x;                    // > 0: debaixo da lamina (u horizontais)
    float plano = smoothstep(0.35, 0.8, ny);
    // Debaixo do plano a cor do fundo e' do shader da agua (absorcao pela
    // profundidade). A lamina so' continua 1,2 u para baixo, apagando em
    // cor, e so' onde a onda esta' agora: antes havia um piso fixo de agua
    // sob o plano e o fundo inteiro ficava claro ate' o fim da grade da beira
    // (em blocos), parado, mesmo com a espuma recuada.
    // Inteira enquanto a agua ainda e' transparente (0..0,35 u, liquid.frag
    // g_dioramaAlpha), e so' some depois, ja' coberta: apagando junto com a
    // agua sobrava no meio areia molhada sem cor (~105 de 255 entre o mar a
    // ~140 e a lamina, medido num mapa de praia).
    // Com o repuxo a agua so' fica opaca a recuo + 0,35 u; a lamina segue
    // inteira ate' la' e some logo depois.
    float recuo = shoreRecuoDe(ond.e1);
    float raso = mix(1.0, 1.0 - smoothstep(recuo + 0.3, recuo + 0.45, -y), step(y, 0.0));
    // Debaixo do plano: coberto onde ainda ha' mar (alem do repuxo) ou onde
    // a lamina passa; entre os dois, a areia molhada exposta pelo recuo.
    float cobre = max(smoothstep(0.0, 0.4, prof), smoothstep(recuo, recuo + 0.08, -y)) * plano * raso;
    // Agua aerada logo atras da frente (o claro da lamina): anda com a
    // espuma e some em cor para dentro, em direcao 'a agua (2 u ou 30% do
    // alcance). Antes o claro era fixo em toda a area coberta - e debaixo
    // do plano ela esta' sempre coberta: ficava uma faixa clara parada sob a
    // agua que nao acompanhava a espuma no recuo.
    // Subindo a lamina e' um vagalhao cheio de bolha (claro); descendo as
    // bolhas estouram e ela vira agua lisa e transparente, com o claro so'
    // perto do alto. Claro o tempo todo, os lobos grandes liam como placas.
    float bolha = ond.sobe1 > 0.5 ? 1.0 : 0.3 + 0.7 * smoothstep(0.6, 1.0, ond.ex1);
    float aerada = step(0.0, prof) * exp(-prof / max(1.5, 0.2 * r.y)) * bolha * plano * raso;
    // Altura equivalente: cresce para dentro da lamina (0,3 por u, ate' 3) e
    // afina praia acima (a lamina perde agua subindo; 35% a 1 u do nivel).
    // Na linha d'agua fica a mesma (o tom tem de bater com o do mar); com os
    // lobos grandes a lamina inteira saia no tom cheio, uma placa clara.
    float altura = clamp(prof * 0.3, 0.0, 3.0) * mix(1.0, 0.35, smoothstep(0.1, 1.0, y));
    float espuma = 0.0;
    if (comEspuma) {
        // Renda PRESA NA FRENTE: o desenho anda com a agua, so' para a terra
        // e de volta (desloca-se `frente` u ao longo de `fora`). Antes a torcao
        // do dominio girava num circulo (renda se retorcendo no lugar, 18 s por
        // volta) e, antes disso, andava de lado pela praia - as duas sem
        // relacao com a onda. Cada onda tem um desenho novo: a semente e' a da
        // onda do lobo maior e troca com ele no fundo do recuo (espuma ja'
        // debaixo d'agua, oculta) ou na cuspide, debaixo da linha de encontro.
        vec2 q = (wp.xz - fora * frente) * 0.5 + ond.semente;
        q += 1.6 * vec2(shoreNoise(q * 0.7), shoreNoise(q * 0.7 + 9.0));
        float renda = shoreNoise(q) * 0.6 + shoreNoise(q * 2.3) * 0.4;
        float fios = smoothstep(0.12, 0.0, abs(renda - 0.5));
        float massa = smoothstep(0.45, 0.7, renda);
        bool sobe = ond.sobe1 > 0.5;
        // A espuma anda SEMPRE com a frente: subindo, faixa cheia de 1,5 u;
        // descendo, mais fina (0,9 u), so' fios e alguma massa, e vai
        // rareando conforme a agua escoa. Antes, no recuo, ela ficava largada
        // na areia (fios parados acima da frente) e nao acompanhava a agua.
        float o = ond.e1;
        float faixa = step(0.0, prof) * (1.0 - smoothstep(0.0, sobe ? 1.5 : 0.9, prof));
        float corpo = sobe ? max(fios, massa * 0.8) : max(fios * 0.85, massa * 0.35);
        espuma = corpo * faixa * (sobe ? 1.0 : mix(0.2, 0.75, o)) * plano;
        // Encontro de dois lobos (cuspide): a descida de um bate na subida
        // do vizinho e sobe uma linha de espuma, da frente para o mar, mais
        // forte perto da frente. Mesma renda (so' vai e volta com a agua).
        float encontro = shoreEncontro(ond) * step(0.0, prof) * exp(-prof / 5.0);
        espuma = max(espuma, encontro * smoothstep(0.25, 0.6, renda + 0.15) * plano);
        // So' acima do plano: com a onda recuada a frente fica na linha d'agua
        // e a faixa de espuma logo atras dela caia no fundo, debaixo d'agua
        // (manchas brancas paradas no raso, vistas com ERUPTION_TEST_PBR_DEBUG=512).
        espuma *= smoothstep(-0.3, 0.0, y);
    }
    return vec4(cobre, cobre * altura, clamp(espuma, 0.0, 1.0), aerada);
}

// Chao debaixo da lamina: a mesma absorcao do corpo d'agua (liquid.frag,
// shadeDiorama: o vermelho some primeiro e a cor funda entra no lugar), com a
// altura da lamina como caminho. Era um escurecimento de 28% com um pouco da
// cor rasa: a areia ficava so' transparente, sem a cor da agua, e o fundo
// visto pelo plano d'agua tinha outro tom (a divisa aparecia). O caminho
// conta o raio inclinado (1,6x a altura). water: rgb = cor funda, w = clareza.
// Por cima, o ceu que a agua reflete (o azul claro que se ve na agua vem do
// reflexo, nao da absorcao: a cor funda e' quase preta): forte perto da agua,
// fraco na frente fina. sky = cor do horizonte do ceu.
vec3 shoreSwashAlbedo(vec3 albedo, vec4 sw, vec4 water, vec3 sky) {
    vec3 T = exp(-vec3(1.0, 0.36, 0.22) / max(water.w, 0.1) * sw.y * 1.6);
    vec3 c = albedo * T + water.rgb * (1.0 - T);
    vec3 ceu = sky / max(max(sky.r, max(sky.g, sky.b)), 1e-3) * 0.6;
    // Tom do mar (o ceu refletido) na lamina toda, crescendo com a altura de
    // agua, e mais na faixa aerada atras da frente: na linha d'agua a lamina
    // tem de ter o mesmo tom da superficie, senao a divisa vira uma linha.
    c = mix(c, ceu, max(mix(0.3, 0.55, smoothstep(0.0, 3.0, sw.y)) * step(1e-4, sw.y), 0.4 * sw.w));
    c = mix(albedo, c, sw.x);
    return mix(c, vec3(0.85, 0.88, 0.9), 0.85 * sw.z);
}

// albedoLin : linear-space albedo
// roughness : dry microsurface roughness
// metallic  : used ONLY as a porosity hint; never modified
// cavity    : MRAH-W alpha from PbrMapGen (curvature * depression) = pooling mask
// normalY   : world-space N.y, so water pools on up-facing surfaces and runs
//             off walls and ceilings instead of coating everything equally
// wetAmount : 0..1 driver (rain intensity, later the per-region climate field)
PbrWetness pbrWetSurface(vec3 albedoLin, float roughness, float metallic,
                         float cavity, float normalY, float wetAmount) {
    PbrWetness w;
    w.albedo = albedoLin;
    w.roughness = roughness;
    w.film = 0.0;
    if (wetAmount <= 0.001) return w;

    // Gravity gate: rain falls down. Up-facing ground collects and holds water,
    // vertical walls keep only a running sheet, undersides and ceilings stay
    // dry. Without this every surface in the scene lights up at once, which is
    // a large part of the "the whole map glows in the rain" look.
    float facing = clamp(normalY, 0.0, 1.0);
    float collect = mix(0.25, 1.0, facing);

    // Porosity proxy. A rough dielectric is porous (stone, dirt, cloth, wood);
    // a smooth or metallic one is sealed (glass, painted metal, polished tile).
    // Porous surfaces darken and smooth hard; sealed ones barely change.
    // Piso 0,35: pedra, madeira e terra com mapa de roughness baixo (cozido
    // liso) ainda escurecem na chuva; so' o metal fica de fora.
    float porosity = max(clamp(roughness, 0.0, 1.0), 0.35) * (1.0 - clamp(metallic, 0.0, 1.0));

    // Standing water in the depressions baked by PbrMapGen. Exposed tops only
    // get damp, low areas actually pool.
    float pool = clamp(cavity, 0.0, 1.0);
    // Chao encharcado (wetAmount ~1 depois de alguns segundos de chuva) fica
    // com pelicula quase cheia; antes 0,55 x chuva 0,55 dava ~0,3 e mal se via.
    float film = clamp(wetAmount * collect * (0.8 + 0.2 * pool), 0.0, 1.0);

    // (1) Darkening. A very porous wet surface loses roughly a third of its
    // diffuse reflectance; a sealed one loses almost none. Multiplicative so
    // hue is preserved and only value drops.
    float darken = mix(1.0, mix(0.85, 0.45, porosity), film);
    w.albedo = albedoLin * darken;

    // (2) Smoothing toward the film's own roughness, scaled by how much pore
    // volume there is for the water to fill. min() so a surface already
    // smoother than the film is never made rougher by getting wet.
    // A pelicula e' agua: lisa em qualquer material. Antes o alisamento era
    // multiplicado pela porosidade, e com roughness ~0,5 (chao comum) a chuva
    // quase nao mudava o brilho - "nao ativa o roughness quando chove".
    float filmRough = mix(0.18, 0.05, pool); // a puddle is smoother than damp stone
    w.roughness = mix(roughness, min(roughness, filmRough), film);

    w.film = film;
    return w;
}

#endif // PBR_COMMON_GLSL
