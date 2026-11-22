#pragma once

#include "formats/MapLoader.hpp"

namespace eruption {

// (x, z) de mundo -> coordenadas da grade de navegacao do mapa em celulas
// (u, v), a mesma conta de cellOf/cellCenter (WalkGrid.cpp): a celula gx cobre
// [gx, gx+1). Falso sem grade. Nao testa os limites.
bool navGridCoords(const LoadedMap& map, float x, float z, float& u, float& v);

// Altura de mundo (Y) do caminho andavel em (x, z) pelas alturas de canto da
// grade de navegacao. Falso se o mapa nao tem alturas de canto (.nav v1) ou
// (x, z) esta fora da grade.
bool navHeightAt(const LoadedMap& map, float x, float z, float& y);

} // namespace eruption
