#pragma once

// RELEVO ASSADO NA CARGA: a malha recebe, uma vez, o deslocamento que a
// tesselacao faria a cada quadro. Passos (cada um medido na bancada de
// variantes, not_commit/relevo_fotos/i23_assado_variantes_high_medium_low.png):
//   1. mediana sobre o mapa de altura (raio por preset): preserva crista e
//      paredao, ao contrario do borrao gaussiano, e tira o ponto isolado que a
//      decimacao viraria escada;
//   2. subdivisao da malha ate' nenhuma aresta passar de `spacing` (a grade do
//      preset), uma decisao POR ARESTA: os dois triangulos de uma aresta a
//      partem igual e a malha fica sem fenda;
//   3. cada vertice le' a altura no seu UV (vizinho mais proximo da grade
//      reduzida) e anda (h - 0,5) * amplitude, como a tesselacao; vertices na
//      mesma posicao (costura de UV ou de normal) andam o mesmo tanto pela
//      normal media, sem fenda.
// O detalhe mais fino que a grade fica no normal map.
//
// Sem Vulkan nem glm: o chamador traduz o vertice da malha para ReliefVertex
// e de volta pelo ReliefBakeRecord (de quem cada vertice novo e' o meio e
// quanto cada um andou), que e' tambem o que vai para o cache.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace eruption {

struct ReliefVertex {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float uv[2] = {0.0f, 0.0f};
    uint32_t tex = 0; // textura de altura do vertice (a do chamador)
};

// O que o assado fez, sem os vertices: refaz a malha a partir da original.
struct ReliefBakeRecord {
    // Vertice novo i (a partir do numero de vertices da entrada) = meio de
    // parents[2i] e parents[2i+1], que sao anteriores a ele.
    std::vector<uint32_t> parents;
    // Deslocamento de cada vertice (u), direcao em dir[3i..3i+2] (unitaria).
    std::vector<float> offset, dir;
    // Triangulos de saida por triangulo de entrada, na ordem da entrada: os
    // filhos de um triangulo saem juntos, entao um intervalo de indices da
    // entrada (celula do chao) vira um intervalo da saida.
    std::vector<uint32_t> children;
    std::vector<uint32_t> indices; // malha de saida
};

struct ReliefBakeParams {
    float spacing = 1.0f;         // aresta maxima depois da subdivisao (u)
    float amplitude = 0.0f;       // deslocamento de h = 0 a h = 1 (u)
    size_t maxTriangles = 4u << 20; // teto: a subdivisao para antes de passar
};

struct ReliefBakeStats {
    size_t trianglesIn = 0, trianglesOut = 0, verticesOut = 0;
    int rounds = 0;
    bool capped = false; // parou no teto com aresta maior que `spacing`
    double ms = 0.0;
};

// Altura 0..1 de `tex` no UV (repetido). nullptr ou textura sem altura: 0,5.
using ReliefHeightSampler = std::function<float(uint32_t tex, float u, float v)>;

// Mediana de raio `radius` (janela quadrada 2r+1, bordas repetidas como a
// textura) sobre um canal de 8 bits w x h. radius <= 0 devolve a copia.
std::vector<uint8_t> reliefMedian(const uint8_t* src, int w, int h, int radius);

// Leitura do vizinho mais proximo com UV repetido (o que a grade reduzida le').
float reliefSampleNearest(const std::vector<uint8_t>& map, int w, int h, float u, float v);

// Subdivide e desloca `verts`/`indices` (triangulos) no lugar; `record`
// (opcional) recebe o que foi feito.
ReliefBakeStats bakeRelief(std::vector<ReliefVertex>& verts, std::vector<uint32_t>& indices,
                           const ReliefBakeParams& params, const ReliefHeightSampler& height,
                           ReliefBakeRecord* record = nullptr);

} // namespace eruption
