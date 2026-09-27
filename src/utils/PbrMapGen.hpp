#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

// Output channels of the generated PBR maps, matching docs/18_TextureRemasterPBR_Eruption.md.
// MRAH-W: R=Metallic, G=Roughness, B=AO (cavidade), A=Height (255 = sem dado).
// Mesmo layout de loadIndustryStandardPbr/synthesizePbrFromPixels e dos shaders.
// Normal: RGB tangent-space normal packed to [0,1], A=1.
//
// Maps are baked neutral ("100%"): metallic and roughness are full-strength and
// the engine scales them at runtime using the per-material profile table
// (data/pbr_materials.json). Only image-derived spatial detail is baked in.
struct PbrMapSet {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> mrahw;   // 4 channels
    std::vector<uint8_t> normal;  // 4 channels
};

// Generate neutral PBR maps from an RGBA8 albedo buffer.
// nameHint is unused (material values are resolved at runtime, not at cook time).
// normalStrength scales the normal map relief.
PbrMapSet generatePbrMaps(const uint8_t* rgba, int width, int height,
                          const std::string& nameHint = {},
                          float normalStrength = 1.0f);

} // namespace eruption
