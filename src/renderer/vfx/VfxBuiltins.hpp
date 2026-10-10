#pragma once

// Texturas de particula que vem com o motor, geradas na hora (sem arquivo):
// "builtin:puff" (fumaca, 4x4 variacoes), "builtin:flame" (chama, 4x4
// quadros), "builtin:spark", "builtin:ring" e "builtin:star". Servem de
// ponto de partida; o jogo usa as proprias folhas de quadros em PNG.

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

struct VfxImage {
    uint32_t width = 0, height = 0;
    uint32_t cols = 1, rows = 1; // folha de quadros
    std::vector<uint8_t> rgba;   // alfa reto (nao premultiplicado)
};

bool makeVfxBuiltin(const std::string& name, VfxImage& out);

// Poligono justo de uma folha de quadros: alcance do conteudo (alfa acima
// do limiar) em 8 direcoes (k*45 graus), em coordenadas do quadro [-1,1],
// juntando todos os quadros. O desenho usa o octogono resultante no lugar do
// quad e nao rasteriza os cantos vazios.
void computeVfxCutout(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t cols, uint32_t rows, float out[8]);
const std::vector<std::string>& vfxBuiltinNames();

// Malhas de particula que vem com o motor (detritos, estilhacos): geradas na
// hora, raio ~1 em volta da origem. Vertice: posicao, normal, uv (32 bytes).
struct VfxMeshVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};
struct VfxMeshData {
    std::vector<VfxMeshVertex> vertices;
    std::vector<uint16_t> indices;
};
bool makeVfxBuiltinMesh(const std::string& name, VfxMeshData& out);
const std::vector<std::string>& vfxBuiltinMeshNames();

} // namespace eruption
