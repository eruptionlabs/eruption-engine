#pragma once
// BVH simples (split pela mediana do maior eixo) sobre os triangulos de um
// unico nó de ModelAsset, em espaço LOCAL do nó. Existia para
// ModelRenderer::raycastInstance varrer triangulo por triangulo mesmo em
// malhas de dezenas de milhares de triangulos (uma montanha de
// terreno grande): world.floor_height/floor_walkable chamam isso por candidato
// solido, e world.path chama floor_* centenas de vezes por busca - o custo
// medido era ~1,2 ms por chamada de piso e 340 ms por busca de caminho numa
// malha grande (pedido E-PISO-GRANDE, spec 12 do jogo).
#include "renderer/ModelData.hpp"
#include <cstdint>
#include <vector>

namespace eruption {

class RaycastBvh {
public:
    // Reconstroi a arvore a partir dos vertices/indices do no' (copia as
    // posicoes dos triangulos; não guarda ponteiro para o node).
    void build(const std::vector<ModelVertex>& vertices, const std::vector<uint32_t>& indices);

    bool empty() const { return m_tris.empty(); }

    // t positivo mais proximo entre os dois lados de qualquer triangulo (o
    // mesmo criterio do raycastInstance original: Moller-Trumbore sem culling
    // de face), ou -1 se nenhum.
    float raycastClosest(const Vec3& origin, const Vec3& dir) const;

private:
    struct Tri {
        Vec3 a, b, c;
    };
    struct Node {
        Vec3 bmin{0.0f}, bmax{0.0f};
        int32_t left = -1, right = -1;  // -1 em folha
        uint32_t start = 0, count = 0;  // intervalo em m_order; folha se left < 0
    };

    std::vector<Tri> m_tris;     // triangulos na ordem original (indexados por m_order)
    std::vector<Vec3> m_centroid;
    std::vector<uint32_t> m_order;  // indices de m_tris, reordenados pelo build
    std::vector<Node> m_nodes;

    int32_t buildRange(uint32_t begin, uint32_t end);
};

}  // namespace eruption
