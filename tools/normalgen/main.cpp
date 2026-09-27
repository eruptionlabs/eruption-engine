#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

struct Vec3 {
    float x, y, z;
    Vec3(float x = 0, float y = 0, float z = 0) : x(x), y(y), z(z) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};

inline float length(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline Vec3 normalize(const Vec3& v) {
    float len = length(v);
    if (len < 0.0001f) return Vec3(0, 0, 1);
    return Vec3(v.x / len, v.y / len, v.z / len);
}

inline float sampleHeight(const std::vector<float>& h, int w, int h2, int x, int y) {
    x = std::max(0, std::min(x, w - 1));
    y = std::max(0, std::min(y, h2 - 1));
    return h[y * w + x];
}

Vec3 computeNormal(const std::vector<float>& heightmap, int w, int h, int x, int y, float strength) {
    float sx = sampleHeight(heightmap, w, h, x + 1, y) - sampleHeight(heightmap, w, h, x - 1, y);
    float sy = sampleHeight(heightmap, w, h, x, y + 1) - sampleHeight(heightmap, w, h, x, y - 1);
    Vec3 n(-sx * strength * 2.0f, -sy * strength * 2.0f, 1.0f);
    return normalize(n);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.png> <output.png> [strength]\n";
        std::cerr << "  strength: normal map strength (default 1.0)\n";
        return 1;
    }

    const char* inputPath = argv[1];
    const char* outputPath = argv[2];
    float strength = 1.0f;
    if (argc >= 4) strength = std::stof(argv[3]);

    int w, h, channels;
    unsigned char* pixels = stbi_load(inputPath, &w, &h, &channels, 1);
    if (!pixels) {
        std::cerr << "Failed to load image: " << inputPath << "\n";
        return 1;
    }

    std::vector<float> heightmap(w * h);
    for (int i = 0; i < w * h; i++) {
        heightmap[i] = pixels[i] / 255.0f;
    }
    stbi_image_free(pixels);

    std::vector<unsigned char> normalMap(w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            Vec3 n = computeNormal(heightmap, w, h, x, y, strength);
            int idx = (y * w + x) * 4;
            normalMap[idx + 0] = static_cast<unsigned char>((n.x * 0.5f + 0.5f) * 255.0f);
            normalMap[idx + 1] = static_cast<unsigned char>((n.y * 0.5f + 0.5f) * 255.0f);
            normalMap[idx + 2] = static_cast<unsigned char>((n.z * 0.5f + 0.5f) * 255.0f);
            normalMap[idx + 3] = 255;
        }
    }

    if (!stbi_write_png(outputPath, w, h, 4, normalMap.data(), w * 4)) {
        std::cerr << "Failed to write image: " << outputPath << "\n";
        return 1;
    }

    std::cout << "Normal map generated: " << outputPath << " (" << w << "x" << h << ")\n";
    return 0;
}
