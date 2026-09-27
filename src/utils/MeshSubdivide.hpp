#pragma once

#include "renderer/TerrainRenderer.hpp" // TerrainVertex

#include <cstdint>
#include <vector>

namespace eruption {

// LOD geométrico no estilo mipmap (pedido do usuário): perto usa uma malha
// subdividida com passa-baixa leve, longe usa a malha original. Tudo é gerado
// UMA VEZ no load - trocar de nível em runtime é só desenhar outro buffer, o
// que evita o hitch de zoom (todos os objetos cruzam o limiar ao mesmo tempo
// quando a câmera afasta/aproxima).
//
// Por que subdividir: a geometria de mapa legado tem quads gigantes (aresta
// mediana de 10u no chão de uma cidade, 26-37u numa ponte). Sem vértices não há
// onde o relevo acontecer. Medido: a ponte inteira a 8u custa 2,3 ms e
// 2,6 MB; a 2u custa 37 ms e 42 MB (cada divisão pela metade multiplica por
// 4 - o custo é MEMÓRIA, não tempo).
// AVISO (bug conhecido, por isso desligado por padrão em data/graphics.json):
// os vértices do meio são compartilhados entre triângulos vizinhos pela
// aresta, mas herdam UV/índice de textura de UM lado. Em malha legada, triângulos
// vizinhos frequentemente estão em ILHAS DE UV diferentes, então o vizinho
// passa a apontar para a UV errada e a textura escorre esticada - geometria
// visivelmente rasgada após um warp (o boot usa outra rota de malha e não
// mostrava o problema). Correção necessária antes de religar: chavear o mapa
// de arestas também por (texIndex, uv) ou simplesmente duplicar o midpoint por
// triângulo quando as UVs não coincidem.
struct SubdivideResult {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    bool valid = false;
};

struct SubdivideParams {
    float targetEdge = 8.0f;    // aresta alvo em unidades de mundo
    int maxLevels = 3;          // teto de segurança (4^3 = 64x)
    size_t maxOutTriangles = 200000; // aborta se explodir
    float lowPassStrength = 0.5f;    // 0 = sem suavização, 1 = máxima
};

// Subdivide por midpoint até a maior aresta ficar <= targetEdge, depois
// aplica passa-baixa na POSIÇÃO dos vértices novos (a forma grande fica, o
// ruído de alta frequência - que é o que vira moiré na sombra - sai).
// Retorna valid=false quando não vale a pena (malha já densa) ou estouraria
// o teto de triângulos.
SubdivideResult subdivideMesh(const std::vector<TerrainVertex>& vertices,
                              const std::vector<uint32_t>& indices,
                              const SubdivideParams& params);

// Aresta mediana da malha: usada para decidir se ela merece subdivisão.
float medianEdgeLength(const std::vector<TerrainVertex>& vertices,
                       const std::vector<uint32_t>& indices);

} // namespace eruption
