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

// Full energy-conserving Cook-Torrance BRDF.
// albedo is expected to be in linear space.
// Returns radiance already multiplied by NdotL.
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
    float porosity = clamp(roughness, 0.0, 1.0) * (1.0 - clamp(metallic, 0.0, 1.0));

    // Standing water in the depressions baked by PbrMapGen. Exposed tops only
    // get damp, low areas actually pool.
    float pool = clamp(cavity, 0.0, 1.0);
    float film = clamp(wetAmount * collect * (0.55 + 0.45 * pool), 0.0, 1.0);

    // (1) Darkening. A very porous wet surface loses roughly a third of its
    // diffuse reflectance; a sealed one loses almost none. Multiplicative so
    // hue is preserved and only value drops.
    float darken = mix(1.0, mix(0.95, 0.62, porosity), film);
    w.albedo = albedoLin * darken;

    // (2) Smoothing toward the film's own roughness, scaled by how much pore
    // volume there is for the water to fill. min() so a surface already
    // smoother than the film is never made rougher by getting wet.
    float filmRough = mix(0.30, 0.06, pool); // a puddle is smoother than damp stone
    w.roughness = mix(roughness, min(roughness, filmRough), film * porosity);

    w.film = film;
    return w;
}

#endif // PBR_COMMON_GLSL
