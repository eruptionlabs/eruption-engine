#pragma once

#include "math/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Desenho de um .glb na CPU (prévia girável e miniaturas do painel Projeto):
// cor média de cada textura, luz de lado e buffer de profundidade. Não usa a
// GPU, então roda numa thread sem tocar no renderizador.
namespace eruption {

struct RasterMesh {
    std::vector<Vec3> pos;         // já no espaço do modelo inteiro
    std::vector<uint32_t> color;   // RGBA por vértice
    std::vector<uint32_t> indices; // triângulos
    Vec3 lo{0.0f}, hi{0.0f};
    size_t sourceTriangles = 0;    // antes de reduzir (modelos enormes)
};

bool loadRasterMesh(const std::string& path, RasterMesh& out, std::string& error, size_t maxTriangles = 400000);
// RGBA w x h, fundo transparente. yaw/pitch em radianos.
void rasterizeMesh(const RasterMesh& mesh, float yaw, float pitch, int w, int h, std::vector<uint8_t>& rgba);

} // namespace eruption
