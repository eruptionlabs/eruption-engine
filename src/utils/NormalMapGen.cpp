#include "utils/NormalMapGen.hpp"
#include <glm/glm.hpp>

namespace eruption {

std::vector<uint8_t> NormalMapGen::generateNormalMap(const std::vector<float>& heightmap,
                                                      uint32_t width, uint32_t height,
                                                      float strength) {
    std::vector<uint8_t> result(width * height * 4);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            Vec3 n = sampleNormal(heightmap, width, height, (int)x, (int)y, strength);
            uint32_t idx = (y * width + x) * 4;
            result[idx + 0] = static_cast<uint8_t>((n.x * 0.5f + 0.5f) * 255.0f);
            result[idx + 1] = static_cast<uint8_t>((n.y * 0.5f + 0.5f) * 255.0f);
            result[idx + 2] = static_cast<uint8_t>((n.z * 0.5f + 0.5f) * 255.0f);
            result[idx + 3] = 255;
        }
    }
    return result;
}

Vec3 NormalMapGen::sampleNormal(const std::vector<float>& h, uint32_t w, uint32_t h2,
                                 int x, int y, float strength) {
    int x1 = glm::max(x - 1, 0);
    int x2 = glm::min(x + 1, (int)w - 1);
    int y1 = glm::max(y - 1, 0);
    int y2 = glm::min(y + 1, (int)h2 - 1);

    float sx = h[y * w + x2] - h[y * w + x1];
    float sy = h[y2 * w + x] - h[y1 * w + x];

    Vec3 n(-sx * strength * 2.0f, -sy * strength * 2.0f, 1.0f);
    return glm::normalize(n);
}

std::vector<uint8_t> NormalMapGen::generateWaterNormalMap(uint32_t width, uint32_t height,
                                                           float time, float strength) {
    std::vector<float> heightmap(width * height);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            float fx = (float)x / width * 4.0f;
            float fy = (float)y / height * 4.0f;
            heightmap[y * width + x] = 
                sinf(fx * 3.14159f + time) * 0.3f +
                cosf(fy * 3.14159f * 0.7f + time * 0.8f) * 0.3f +
                sinf((fx + fy) * 2.0f + time * 1.2f) * 0.2f;
        }
    }
    return generateNormalMap(heightmap, width, height, strength);
}

} // namespace eruption
