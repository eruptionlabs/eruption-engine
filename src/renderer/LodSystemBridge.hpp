#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eruption {

struct TerrainVertex;

// Ponte entre o LOD das malhas de modelo e o LodSystem (utils/LodSystem.hpp):
// monta a entrada a partir dos vertices do motor, guarda o resultado em cache
// no disco e devolve cada nivel ja' no formato de vertice do motor.

// Pixels RGBA8 de uma textura pelo slot bindless (as malhas guardam o slot por vertice).
using LodPixelSource = std::function<bool(uint32_t slot, std::vector<uint8_t>& rgba, int& w, int& h)>;
void lodBridgeSetPixelSource(LodPixelSource source);
// Mapa auxiliar do material (o MRAH-W que o shader le na mesma UV da cor), pelo
// slot da COR: o bake o amostra na UV de cada folha e o nivel ganha o seu.
void lodBridgeSetSurfaceSource(LodPixelSource source);
// Identidade barata da textura de um slot (cor, ou o MRAH-W dela com surface =
// true): hash dos bytes como estao no arquivo, sem decodificar. Alimenta a chave
// rapida do cache (lodBridgeBuild). false = sem a textura (o MRAH-W que nao
// existe) - o mesmo caso em que a fonte de pixels devolve false.
using LodSlotIdentity = std::function<bool(uint32_t slot, bool surface, uint64_t& id)>;
void lodBridgeSetSlotIdentity(LodSlotIdentity identity);
// Hash de 64 bits por palavra (8 bytes por passo): o da chave rapida.
uint64_t lodBridgeFastHash(const void* data, size_t bytes, uint64_t seed = 0x9E3779B97F4A7C15ULL);

struct LodBridgeLevel {
    float px = 0;                          // diametro na tela para o qual o nivel foi feito
    float distortion = 0;                  // erro do nivel nesse tamanho (area x erro^2, pixels)
    // Indices abaixo do numero de vertices da malha apontam para ela; os demais, para
    // vertices (TerrainVertex, so' os criados pelo nivel), a partir de extraBegin.
    std::vector<unsigned char> vertices;
    std::vector<uint32_t> indices;
    uint32_t extraBegin = 0, extraCount = 0; // extraBegin = vertices da malha; extraCount = os de vertices
    std::vector<uint8_t> texture;          // RGBA8; vazio = nivel sem textura propria
    std::vector<uint8_t> normalMap;        // RGBA8, normal no espaco tangente do quad (meia resolucao, mesma UV)
    int normalW = 0, normalH = 0;
    // As mesmas texturas ja' comprimidas em bloco (.etex serializado); vazio = sem suporte, vale o RGBA8
    std::vector<uint8_t> textureEtex, normalEtex;
    int textureW = 0, textureH = 0;
    std::vector<uint8_t> surfaceEtex;      // MRAH-W do nivel (mesma UV da cor), comprimido; vazio = o da folha
    // Impostor de vistas (views x views ladrilhos no atlas): o nivel e' um quadrado
    // canonico (-1..1 em xy, UV do ladrilho 0) e quem desenha poe a matriz e o
    // deslocamento de UV do ladrilho. impostor = centro do objeto (3) e, por
    // ladrilho, centro (3), eixo x (3), eixo y (3) e direcao (3), no espaco do objeto.
    int views = 0;
    std::vector<float> impostor;
};

// Niveis do mais detalhado ao mais grosseiro. false = sem implementacao, malha
// sem ganho ou textura indisponivel: fica o LOD comum.
bool lodBridgeBuild(const std::vector<TerrainVertex>& vertices, const std::vector<uint32_t>& indices,
                    std::vector<LodBridgeLevel>& levels);

} // namespace eruption
