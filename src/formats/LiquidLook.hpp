#pragma once

#include "math/Types.hpp"
#include <nlohmann/json_fwd.hpp>

namespace eruption {

// Aparencia propria de uma agua de contorno (entrada "liquids" do .env com
// "kind": "water" e polygon), por cima do menu de agua do mapa. Negativo (ou
// false) = usa o do menu. Cores em linear (as mesmas de data/water_config.json).
// Formato e faixas: docs/agua_contorno.md.
struct WaterLook {
    bool hasDeep = false, hasShallow = false, hasScatter = false;
    Vec3 deep = Vec3(0.0f), shallow = Vec3(0.0f);
    Vec3 scatter = Vec3(0.0f);       // cor funda do estilo diorama (espalhamento)
    float transparency = -1.0f;      // 0 opaco .. 1 transparente
    float refraction = -1.0f;
    float reflectivity = -1.0f;
    float roughness = -1.0f;
    float waveAmplitude = -1.0f;
    float waveFrequency = -1.0f;
    float waveSpeed = -1.0f;
    float clarity = -1.0f;           // diorama: unidades ate' o vermelho cair a 1/e
    float shoreFoam = -1.0f;         // diorama: largura da espuma de margem
    float caustics = -1.0f;          // intensidade das causticas (0 desliga)
    int foam = -1;                   // espuma de contato: -1 menu, 0 off, 1 on
};

// Le' os campos de aparencia de uma entrada de liquido. Campo ausente ou do
// tipo errado fica como o padrao (usa o menu); numeros fora da faixa sao
// presos a ela.
WaterLook parseWaterLook(const nlohmann::json& entry);

} // namespace eruption
