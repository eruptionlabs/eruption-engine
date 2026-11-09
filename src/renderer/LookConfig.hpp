#pragma once

#include "math/Types.hpp"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace eruption {

// -----------------------------------------------------------------------------
// LookConfig — o LOOK artistico de uma cena
// -----------------------------------------------------------------------------
// Chamava-se DioramaLookConfig (e antes disso carregava a marca da Square).
// Renomeada porque diorama nao e' o que esta struct E' - diorama e' UM PONTO
// dentro do espaco que ela descreve, e agora e' um preset como os outros.
//
// ESTADO: esta configuracao e' CARREGADA, PREENCHIDA e NUNCA CONSUMIDA. Ela e'
// passada para PostProcessor::render e nao e' dereferenciada no corpo. O
// motivo de nunca ter sido ligada fica claro comparando com data/postprocess.json,
// que e' a configuracao REAL e viva do post: metade dos campos daqui eram
// DUPLICATA do que ja' existe e funciona la' (bloom, tilt-shift/CoC, fog) ou em
// data/water_config.json (ondas). Ligar como estava criaria duas fontes de
// verdade brigando pelo mesmo pixel.
//
// Os campos duplicados foram REMOVIDOS. O que sobrou e' o que nao existe em
// lugar nenhum do motor e e' genuinamente "look de cena":
//
//   grading (saturacao/contraste)  -> NAO IMPLEMENTADO. post_composite.frag so'
//                                     tem o operador de tonemap (AgX); nao ha'
//                                     grading artistico exposto.
//   god rays                       -> LIGADO: raios das luzes pontuais fortes
//                                     na tela (PostProcessor::renderGodRays,
//                                     postprocess/god_rays.frag), com estes
//                                     parametros; quantas luzes vem do preset
//                                     (god_ray_lights).
//   tratamento de camada distante  -> NAO IMPLEMENTADO.
//   grade de pixel / dither        -> dither existe fixo em model.frag; a grade
//                                     nao existe.
//   z-scaling e fade por distancia -> NAO IMPLEMENTADO.
//
// O consumo desses campos cai em post_composite.frag e PostProcessor, que sao
// de outro agente. Ver PROTOCOLO.cai (G15).
struct LookConfig {
    std::string name = "default";

    // --- Color grading (nao existe no motor hoje) ---
    float saturation = 1.10f;
    float contrast = 1.05f;

    // --- Vinheta e split-toning (post_composite, depois do tonemap/grading) ---
    // vignette = forca (0 desliga); softness = largura da queda, a partir da
    // borda para dentro (fracao da meia-diagonal). Tons: cor e forca; a cor e'
    // normalizada pela luminancia (so' o matiz entra, o brilho fica).
    float vignette = 0.0f;
    float vignetteSoftness = 0.45f;
    Vec3  shadowTint = Vec3(1.0f);
    float shadowTintStrength = 0.0f;
    Vec3  highlightTint = Vec3(1.0f);
    float highlightTintStrength = 0.0f;

    // --- LUT 3D de grading (renderer/ColorGrade, post_composite binding 14) ---
    // Assada quando o look muda; com ela o composite troca saturacao,
    // contraste e split-toning pela leitura do cubo. Liga sozinha quando um
    // campo abaixo sai do padrao (ou color_lut = true).
    bool  colorLut = false;          // "color_lut"
    float wbTemperature = 0.0f;      // "wb_temperature" -100..100 (> 0 esquenta)
    float wbTint = 0.0f;             // "wb_tint" -100..100 (> 0 magenta)
    Vec3  lift = Vec3(0.0f);         // "lift"  (somado nas sombras)
    Vec3  gammaRgb = Vec3(1.0f);     // "gamma" (meios-tons, > 1 clareia)
    Vec3  gain = Vec3(1.0f);         // "gain"  (multiplica as luzes)
    float vibrance = 0.0f;           // "vibrance" (satura o pouco saturado)
    std::vector<float> curvePoints;  // "curve": x0,y0,x1,y1... em gamma de tela
    std::string lutPath;             // "lut": arquivo .cube (caminho do projeto)
    float lutStrength = 1.0f;        // "lut_strength"

    // --- Luz volumetrica do sol (renderer/VolumetricLight) ---
    // Forca (0 desliga; o preset escolhe resolucao e passos), densidade do ar
    // na altura da nevoa (1/u; cai com a nevoa de altura do PostSettings),
    // tinta e anisotropia g de Henyey-Greenstein.
    float volumetricIntensity = 0.0f;   // "volumetric_intensity"
    float volumetricDensity = 0.002f;   // "volumetric_density"
    Vec3  volumetricTint = Vec3(1.0f);  // "volumetric_tint"
    float volumetricAnisotropy = 0.6f;  // "volumetric_anisotropy"

    // --- God rays das luzes pontuais (PostProcessor::renderGodRays) ---
    // intensity x exposure = forca; scattering x 2 = alcance na tela (fracao da
    // altura); samples e decay = marcha no nucleo da luz; weight = peso final.
    float godRayIntensity = 0.30f;
    float godRayScattering = 0.20f;
    int   godRaySamples = 32;
    float godRayDecay = 0.95f;
    float godRayWeight = 0.25f;
    float godRayExposure = 0.50f;

    // --- Lens flare do sol (no mesmo passe; o preset "lens_flare" liga) ---
    // forca = preset x lensFlare; linhas da estrela: 0 = as do preset.
    float lensFlare = 1.0f;
    int   lensFlareRays = 0;

    // --- Camada distante (parallax / profundidade aerea) ---
    // Distancia dessatura e lava a cor em direcao a atmosfera. E' o que separa
    // um plano de fundo "colado" de um que parece longe.
    float farLayerSaturation = 0.60f;
    float farLayerFog = 0.70f;

    // --- Grade de pixel ---
    bool  pixelGrid = false;
    float pixelGridIntensity = 0.03f;

    // --- Escala em profundidade / fade por distancia ---
    bool  zScaling = true;
    float zScaleReference = 20.0f;
    float fadeFarDistance = 40.0f;
    float fadeMaxDistance = 60.0f;

    // --- Presets ---
    static LookConfig cityLook();
    static LookConfig dungeonLook();
    static LookConfig fieldLook();
    static LookConfig forestLook();

    // DIORAMA: a cena tem que ler como maquete fisica em escala pequena.
    // O que produz essa leitura, em ordem de peso perceptual:
    //   1. GRADIENTE DE DESFOQUE em relacao ao plano do chao. Este e' o sinal
    //      dominante, e nao esta' aqui: mora em data/postprocess.json, que hoje
    //      tem "enable_tilt_shift": false. O motor ja' faz a parte dificil -
    //      coc_gen.comp usa um plano focal quase horizontal, que e' exatamente
    //      a orientacao certa - mas o efeito esta' DESLIGADO nos presets.
    //      Sem ligar la', este preset entrega so' metade do efeito.
    //   2. Cor saturada e contraste alto, como miniatura pintada sob luz forte.
    //   3. Profundidade aerea CURTA: numa maquete o "longe" fica a um metro,
    //      entao a camada distante lava pouco - o oposto de paisagem real.
    static LookConfig dioramaLook();

    void loadFromJson(const nlohmann::json& j);
};

} // namespace eruption
