#include "renderer/LookConfig.hpp"

namespace eruption {

LookConfig LookConfig::cityLook() {
    LookConfig cfg;
    cfg.name = "city";
    cfg.saturation = 1.15f;
    cfg.contrast = 1.05f;
    cfg.godRayIntensity = 0.20f;
    cfg.godRayScattering = 0.15f;
    cfg.farLayerSaturation = 0.60f;
    cfg.farLayerFog = 0.70f;
    cfg.zScaling = true;
    return cfg;
}

LookConfig LookConfig::dungeonLook() {
    LookConfig cfg;
    cfg.name = "dungeon";
    // Interior fechado: cor lavada e contraste alto entre a tocha e o breu.
    cfg.saturation = 0.85f;
    cfg.contrast = 1.20f;
    // Poeira em suspensao num espaco fechado espalha muito a luz que entra.
    cfg.godRayIntensity = 0.45f;
    cfg.godRayScattering = 0.35f;
    // Sem horizonte: nao ha' profundidade aerea para simular.
    cfg.farLayerSaturation = 0.85f;
    cfg.farLayerFog = 0.30f;
    cfg.zScaling = true;
    return cfg;
}

LookConfig LookConfig::fieldLook() {
    LookConfig cfg;
    cfg.name = "field";
    cfg.saturation = 1.10f;
    cfg.contrast = 1.05f;
    cfg.godRayIntensity = 0.30f;
    cfg.godRayScattering = 0.20f;
    // Campo aberto tem o horizonte mais longe do jogo: profundidade aerea forte.
    cfg.farLayerSaturation = 0.55f;
    cfg.farLayerFog = 0.80f;
    cfg.zScaling = true;
    return cfg;
}

LookConfig LookConfig::forestLook() {
    LookConfig cfg;
    cfg.name = "forest";
    // Verde denso: satura sozinho, entao o grading segura em vez de empurrar.
    cfg.saturation = 1.05f;
    cfg.contrast = 1.10f;
    // Luz coada por copa e' o caso classico de raio visivel.
    cfg.godRayIntensity = 0.50f;
    cfg.godRayScattering = 0.30f;
    cfg.farLayerSaturation = 0.65f;
    cfg.farLayerFog = 0.75f;
    cfg.zScaling = true;
    return cfg;
}

LookConfig LookConfig::dioramaLook() {
    LookConfig cfg;
    cfg.name = "diorama";

    // Miniatura pintada sob luz forte: cor mais saturada e contraste mais duro
    // do que qualquer cena real. Tinta em modelo de escala nao tem a
    // dessaturacao que a atmosfera impoe a uma paisagem de verdade.
    cfg.saturation = 1.35f;
    cfg.contrast = 1.22f;

    // Raio de luz discreto: numa maquete a fonte esta' a centimetros do
    // objeto, entao praticamente nao ha' coluna de ar para espalhar. God ray
    // forte e' justamente uma pista de ESCALA GRANDE e trabalha contra a
    // leitura de miniatura.
    cfg.godRayIntensity = 0.12f;
    cfg.godRayScattering = 0.08f;

    // PROFUNDIDADE AEREA CURTA: e' a pista mais mal-compreendida do efeito.
    // Numa maquete o "longe" esta' a um metro de distancia, entao quase nao ha'
    // ar entre a camera e o fundo - a camada distante mantem cor e contraste.
    // Lavar o fundo, que e' o certo para uma paisagem real, faz a cena voltar a
    // ler como paisagem real.
    cfg.farLayerSaturation = 0.92f;
    cfg.farLayerFog = 0.20f;

    // A grade de pixel e' uma pista de VIDEOGAME, nao de maquete: puxa a
    // leitura para "arte 2D" em vez de "objeto fisico pequeno".
    cfg.pixelGrid = false;

    // Escala em profundidade exagerada: objeto proximo cresce mais rapido que o
    // natural, imitando a lente macro curta com que se fotografa maquete.
    cfg.zScaling = true;
    cfg.zScaleReference = 14.0f;
    cfg.fadeFarDistance = 55.0f;
    cfg.fadeMaxDistance = 80.0f;
    return cfg;
}

void LookConfig::loadFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    // Chaves em snake_case, como no data/diorama.json existente: preset antigo
    // continua carregando. Campo ausente mantem o valor do preset, entao um
    // preset so' precisa declarar o que o torna diferente.
    name = j.value("name", name);

    saturation = j.value("saturation", saturation);
    contrast   = j.value("contrast", contrast);

    godRayIntensity  = j.value("god_ray_intensity", godRayIntensity);
    godRayScattering = j.value("god_ray_scattering", godRayScattering);
    godRaySamples    = j.value("god_ray_samples", godRaySamples);
    godRayDecay      = j.value("god_ray_decay", godRayDecay);
    godRayWeight     = j.value("god_ray_weight", godRayWeight);
    godRayExposure   = j.value("god_ray_exposure", godRayExposure);

    farLayerSaturation = j.value("far_layer_saturation", farLayerSaturation);
    farLayerFog        = j.value("far_layer_fog", farLayerFog);

    pixelGrid          = j.value("pixel_grid", pixelGrid);
    pixelGridIntensity = j.value("pixel_grid_intensity", pixelGridIntensity);

    zScaling        = j.value("z_scaling", zScaling);
    zScaleReference = j.value("z_scale_reference", zScaleReference);
    fadeFarDistance = j.value("fade_far_distance", fadeFarDistance);
    fadeMaxDistance = j.value("fade_max_distance", fadeMaxDistance);

    // NAO LIDOS DE PROPOSITO: tilt_*, bloom_*, fog_* e as ondas d'agua. Esses
    // parametros pertencem a data/postprocess.json e data/water_config.json,
    // que sao as configuracoes VIVAS desses sistemas. Um preset antigo que
    // ainda os declare e' simplesmente ignorado aqui, em vez de criar uma
    // segunda fonte de verdade competindo pelo mesmo pixel - que e' o motivo
    // desta struct nunca ter sido ligada.
}

} // namespace eruption
