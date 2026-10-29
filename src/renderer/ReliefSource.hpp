#pragma once

// RELEVO ASSADO, lado do motor: de onde vem a altura e onde fica o cache.
// O assado em si (mediana, subdivisao, deslocamento) e' renderer/ReliefBake.
//
// Altura = alfa do MRAH-W da textura de cor do vertice (o mesmo que a
// tesselacao le' em heightUv), com o contraste dela: h = 0,5 + (alfa - media
// da textura) x ganho; alfa 255 = textura sem altura (0,5). Antes, uma mediana
// do tamanho da grade (raio = metade dos texels entre dois vertices): pega a
// crista e o paredao, nao o grao, que fica no normal map.
//
// Cache por conteudo (vertices, indices, parametros e os bytes do MRAH-W de
// cada textura): le' do pacote do jogo (assets/lodcache/<mobile|desktop>/
// relevo_<chave>.bin, a pasta que a exportacao ja' leva) e depois de
// .cache/eruption/relevo/; grava no segundo e, com
// ERUPTION_RELIEF_PACK_OUT=<pasta>, tambem la' (montar o pacote do jogo).

#include "renderer/ReliefBake.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eruption {

struct EtexData;
struct TerrainVertex;

// Alfa (mip 0) de uma textura em bloco (BC1/BC3/BC7) ou RGBA8 cru.
bool reliefDecodeAlpha(const EtexData& e, std::vector<uint8_t>& alpha);
// Identidade de conteudo (hash por palavra de 8 bytes) para a fonte de altura.
uint64_t reliefContentHash(const void* data, size_t bytes);

// Altura crua de uma textura de cor (pelo slot): identidade do conteudo (hash
// dos bytes como estao no arquivo) e, com pixels = true, o alfa w x h. false =
// sem altura. A identidade vem antes: com o cache certo nada e' decodificado.
using ReliefAlphaSource = std::function<bool(uint32_t slot, bool pixels, std::vector<uint8_t>& alpha, int& w,
                                             int& h, uint64_t& id)>;

struct ReliefParams {
    float spacing = 2.0f;   // aresta maxima (u, espaco do modelo)
    float amplitude = 0.0f; // h = 0 -> -amplitude/2, h = 1 -> +amplitude/2
    float gain = 1.5f;      // contraste da altura (o da tesselacao)
    // Alvo de triangulos por malha: malha grande (paredao de 2000 u) abre a
    // grade ate' caber (ReliefResult::spacing); o teto do assado e' 2x isto.
    size_t maxTriangles = 600000;
    // false: nem le' nem grava o cache em disco (previa de material no cubo,
    // que muda a cada arrasto do controle).
    bool store = true;
    const std::atomic<bool>* stop = nullptr; // ligado: larga o assado, sem gravar cache
};

struct ReliefResult {
    ReliefBakeRecord record;
    bool fromCache = false;
    float spacing = 0.0f; // grade usada (>= a pedida)
    double ms = 0.0;
};

// false = sem altura nenhuma na malha (nada a assar) ou entrada vazia.
// Seguro em varias threads ao mesmo tempo (a fonte tambem precisa ser).
bool reliefBakeCached(const std::vector<TerrainVertex>& vertices, const std::vector<uint32_t>& indices,
                      const ReliefParams& params, const ReliefAlphaSource& source, ReliefResult& out);

// Esquece os mapas de altura medianos guardados (troca de mapa).
void reliefClearMaps();

} // namespace eruption
