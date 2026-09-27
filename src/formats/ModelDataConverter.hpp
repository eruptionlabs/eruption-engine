#pragma once

#include "renderer/ModelData.hpp"
#include "formats/ModelFile.hpp"

#include <vector>

namespace eruption {

// Convert a ModelFile (populated by the GLB parser) to the generic ModelAsset format.
// This adapter lets the renderer consume the source-agnostic ModelFile structure.
ModelAsset convertModelToAsset(const ModelFile& model);

} // namespace eruption
