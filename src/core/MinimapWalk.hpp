#pragma once
// Minimapa por grade de andar: celula andavel numa cor, bloqueada em outra.
// Logica pura (sem Vulkan) para ser testada a parte; o motor so' a chama
// uma vez por bake do minimapa e sobe o resultado na textura que ja' existe.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eruption {

struct MinimapWalkConfig {
    bool enabled = false;                 // padrao desligado: desenho de cena de sempre
    uint8_t walk[3] = {0xd9, 0xc7, 0xa0}; // minimap_walk_color
    uint8_t block[3] = {0x6b, 0x4a, 0x2b}; // minimap_block_color
    bool edge = false;                    // minimap_walk_style: "flat" (padrao) ou "edge"

    // Le o JSON (chaves minimap_walk_enabled/_color/minimap_block_color/
    // minimap_walk_style). Arquivo ausente ou chave invalida: mantem o padrao.
    static MinimapWalkConfig load(const std::string& path);
};

// "#rrggbb" ou "rrggbb". Falso (e out intacto) se nao for uma cor valida.
bool parseHexColor(const std::string& text, uint8_t out[3]);

// Pinta a imagem RGBA8 (res x res). `probe(u, v)` recebe a posicao 0..1 da
// imagem e devolve 0 = fora do mapa (transparente), 1 = andavel, 2 = bloqueada.
// Estilo "edge" escurece a celula bloqueada que encosta numa andavel.
void bakeWalkMinimap(const MinimapWalkConfig& cfg, uint32_t res,
                     const std::function<int(float, float)>& probe, std::vector<uint8_t>& rgba);

} // namespace eruption
