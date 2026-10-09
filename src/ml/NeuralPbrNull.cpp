// Módulo da rede de PBR desligado (ERUPTION_MODULE_NEURAL_PBR=OFF): o mapa PBR
// que faltar sai da síntese analítica, como sem modelo configurado.
#include "ml/NeuralPbr.hpp"

namespace eruption {

bool neuralPbrAvailable() { return false; }
NeuralPbrStats neuralPbrStats() { return {}; }
uint32_t neuralPbrKey() { return 0; }
bool neuralPbrInfer(const uint8_t*, int, int, NeuralPbrMaps&) { return false; }

} // namespace eruption
