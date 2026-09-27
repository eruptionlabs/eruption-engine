#pragma once

#include "utils/BcCompressor.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace eruption {

struct ModelFile;

// One embedded GLB image, block-compressed and ready to upload: albedo plus the
// PBR maps derived from it. Any of the three may be invalid, in which case the
// caller falls back to the RGBA8 path for that map only.
struct BakedEmbeddedTexture {
    EtexData albedo;
    EtexData normal;
    EtexData mrahw;
};

struct EmbeddedBakeResult {
    std::unordered_map<std::string, BakedEmbeddedTexture> textures;
    // Average albedo of the map (bounce/ambient ground tint). Computed while the
    // pixels are decoded and stored in the cache, so a cached load produces the
    // exact same lighting as a cold one instead of falling back to a constant.
    float groundAlbedo[3] = {0.0f, 0.0f, 0.0f};
    bool hasGroundAlbedo = false;
    bool fromCache = false;
    size_t bytes = 0; // total compressed payload
};

// Block-compress every texture embedded in `model` (GLB images), synthesizing
// the PBR pair from each albedo, and cache the whole thing next to the GLB as
// "<glb>.etexpack".
//
// The cache is a DERIVED artifact: it is keyed by the GLB's size+mtime, the
// resolution cap and the format set, so editing the GLB, changing the preset or
// running on a GPU without BC support all invalidate it automatically. Deleting
// it only costs one slower load. When no BC format is supported at all the
// function returns an empty result and the engine keeps its RGBA8 path.
EmbeddedBakeResult bakeEmbeddedTextures(const ModelFile& model,
                                        uint32_t maxTextureSize,
                                        bool synthesizePbr);

// Teto de resolucao ativo (preset de data/graphics.json). O loader de fundo
// (warp) nao le' o preset, entao o Engine publica o valor aqui.
void setEmbeddedTextureMaxSize(uint32_t maxSize);
uint32_t embeddedTextureMaxSize();

} // namespace eruption
