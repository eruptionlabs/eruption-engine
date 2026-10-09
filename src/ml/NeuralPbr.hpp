#pragma once

#include <cstdint>
#include <vector>

namespace eruption {

// Sintese de PBR a partir do albedo por uma rede neural qualquer (ver
// NeuralNet.hpp), no bake do load. Plug and play: o codigo nao conhece o
// modelo. A config local (data/launcher_config.json, fora do git) so' aponta
// o arquivo:
//
//   "neural_pbr": "<caminho do .onnx>"
//
// e o .onnx se descreve pelos metadados "eruption.pbr.*" (JSON em cada um):
//   channels      {"normal_x": i, "normal_y": i, "normal_z": i,
//                  "ao": i, "roughness": i, "metallic": i, "height": i}
//   normal_y_up   true/false (convencao do +Y do normal que a rede da')
//   pad_multiple  estende circularmente ate' esse multiplo (ex.: 16)
//   input_scale / input_bias   valor = byte * scale + bias (padrao: /255)
//   input_tensor / output_tensor  (padrao: primeira entrada/saida do grafo)
// Entrada float32 NCHW [1,3,H,W] RGB; saida float32 NCHW [1,C,H,W].
// Qualquer campo pode ser sobrescrito na config, com o bloco em forma de
// objeto: "neural_pbr": {"model": "...", "channels": {...}}.
//
// Canal ausente = o motor usa o valor da sintese classica. Execucao em
// cadeia: GPU (Vulkan) > CPU (ONNX Runtime) > sintese analitica
// (HeightFromAlbedo), cada nivel caindo pro seguinte sozinho. Sem config,
// sem modelo ou sem contrato: available() e' false e vai direto pra
// analitica.
// Config e modelo sao relidos quando mudam no disco (hot reload): trocar o
// arquivo do modelo basta.
// ERUPTION_NEURAL_PBR=0 desliga; ERUPTION_NEURAL_PBR_CONFIG troca o arquivo.
struct NeuralPbrMaps {
    int width = 0, height = 0;
    std::vector<float> normal;    // xyz intercalado, [-1,1], +Y pra cima na imagem
    std::vector<float> ao;        // [0,1]
    std::vector<float> roughness; // [0,1]
    std::vector<float> metallic;  // [0,1]
    std::vector<float> heightMap; // media zero, unidade da rede
};

bool neuralPbrAvailable();

// Inferencias acumuladas por nivel (GPU > CPU), pra log e metrica; o que
// nao passou por nenhum dos dois foi pra sintese analitica.
struct NeuralPbrStats {
    uint64_t gpu = 0, cpu = 0;
};
NeuralPbrStats neuralPbrStats();
// Muda quando o modelo ou o mapeamento muda; 0 sem rede. Entra no cabecalho
// do cache de texturas pra trocar de modelo invalidar os packs.
uint32_t neuralPbrKey();
// Thread-safe. Vetores de canal nao mapeado voltam vazios.
bool neuralPbrInfer(const uint8_t* rgba, int width, int height, NeuralPbrMaps& out);

} // namespace eruption
