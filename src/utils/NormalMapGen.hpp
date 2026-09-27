#pragma once

#include "math/Types.hpp"
#include <vector>

namespace eruption {

class NormalMapGen {
public:
    // Generate a tangent-space normal map from a heightmap (grayscale)
    // Input: width x height grayscale values (0.0-1.0)
    // Output: RGB normal map with alpha = roughness (optional)
    static std::vector<uint8_t> generateNormalMap(const std::vector<float>& heightmap,
                                                   uint32_t width, uint32_t height,
                                                   float strength = 1.0f);

    // Generate water normal map with animated waves (procedural)
    static std::vector<uint8_t> generateWaterNormalMap(uint32_t width, uint32_t height,
                                                        float time, float strength = 1.0f);

private:
    static Vec3 sampleNormal(const std::vector<float>& h, uint32_t w, uint32_t h2,
                              int x, int y, float strength);
};

} // namespace eruption
