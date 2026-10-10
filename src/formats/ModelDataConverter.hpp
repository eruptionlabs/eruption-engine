#pragma once

#include "renderer/ModelData.hpp"
#include "formats/ModelFile.hpp"

#include <vector>

namespace eruption {

// Convert a ModelFile (populated by the GLB parser) to the generic ModelAsset format.
// This adapter lets the renderer consume the source-agnostic ModelFile structure.
ModelAsset convertModelToAsset(const ModelFile& model);

// Igual, mas libera a geometria de `model` nó a nó enquanto converte e deixa
// `model.nodes` vazio (texturas embutidas e caminho ficam). Na carga do mapa a
// malha lida do GLB não é usada depois da conversão; sem isso as duas cópias
// conviviam (+1,3 GB no parana_field).
ModelAsset convertModelToAssetReleasing(ModelFile& model);

} // namespace eruption
