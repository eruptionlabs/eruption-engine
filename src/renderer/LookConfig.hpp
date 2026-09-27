#pragma once

#include "math/Types.hpp"
#include <string>
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
//   god rays                       -> shader EXISTE (postprocess/god_rays.frag);
//                                     falta amarrar estes parametros nele.
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

    // --- God rays (shader existe, parametros soltos) ---
    float godRayIntensity = 0.30f;
    float godRayScattering = 0.20f;
    int   godRaySamples = 32;
    float godRayDecay = 0.95f;
    float godRayWeight = 0.25f;
    float godRayExposure = 0.50f;

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
