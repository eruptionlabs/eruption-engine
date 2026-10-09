#pragma once

#include "OnnxModel.hpp"

#include <string>
#include <vector>

namespace eruption {

// Avaliacao na CPU de um no cujas entradas sao todas conhecidas (constantes
// do modelo ou valores derivados so' da FORMA da entrada). E' assim que o
// planejador resolve aritmetica de forma, indices e tabelas que a rede monta
// em tempo de execucao (ex.: matrizes de transformada do tamanho da imagem):
// tudo isso vira constante antes da GPU entrar.
//
// inputs[i] == nullptr = entrada opcional ausente. false = op nao suportada
// ou entrada invalida (mensagem em 'error').
bool evalConstNode(const OnnxNode& node, const std::vector<const OnnxTensor*>& inputs,
                   std::vector<OnnxTensor>& outputs, std::string& error);

// Utilitarios de forma compartilhados com o planejador.
std::vector<int64_t> contiguousStrides(const std::vector<int64_t>& shape);
bool broadcastShapes(const std::vector<int64_t>& a, const std::vector<int64_t>& b, std::vector<int64_t>& out);
int64_t normalizeAxis(int64_t axis, size_t rank);
size_t shapeCount(const std::vector<int64_t>& shape);

} // namespace eruption
