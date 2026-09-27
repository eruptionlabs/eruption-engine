#include "utils/PbrMapGen.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace eruption;

static int g_failures = 0;

static void check(bool condition, const char* expr, const char* file, int line) {
    if (!condition) {
        std::cerr << "FAIL: " << expr << " at " << file << ":" << line << "\n";
        ++g_failures;
    }
}
#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

static bool approx(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) <= eps;
}

static std::vector<uint8_t> makeCheckerboard(int size) {
    std::vector<uint8_t> rgba(size * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            bool dark = ((x / 4) + (y / 4)) % 2 == 0;
            uint8_t c = dark ? 32 : 220;
            int i = (y * size + x) * 4;
            rgba[i + 0] = c;
            rgba[i + 1] = c;
            rgba[i + 2] = c;
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}

static float decodeNormal(uint8_t c) { return (c / 255.0f) * 2.0f - 1.0f; }

static int testNormalMapLength() {
    int size = 16;
    auto rgba = makeCheckerboard(size);
    PbrMapSet maps = generatePbrMaps(rgba.data(), size, size, "brick", 1.0f);
    CHECK(!maps.normal.empty());
    CHECK(maps.width == size);
    CHECK(maps.height == size);

    int center = (size / 2 * size + size / 2) * 4;
    float nx = decodeNormal(maps.normal[center + 0]);
    float ny = decodeNormal(maps.normal[center + 1]);
    float nz = decodeNormal(maps.normal[center + 2]);
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    CHECK(approx(len, 1.0f, 0.02f));
    CHECK(maps.normal[center + 3] == 255);
    return 0;
}

static int testNeutralMetallicFull() {
    int size = 8;
    auto rgba = makeCheckerboard(size);
    PbrMapSet maps = generatePbrMaps(rgba.data(), size, size, "weapon_blade", 1.0f);

    int count = size * size;
    for (int i = 0; i < count; ++i) {
        CHECK(maps.mrahw[i * 4 + 0] == 255); // metallic baked at 100%
    }
    return 0;
}

static int testRoughnessAndHeightRanges() {
    int size = 16;
    auto rgba = makeCheckerboard(size);
    PbrMapSet maps = generatePbrMaps(rgba.data(), size, size, "stone", 1.0f);

    int count = size * size;
    for (int i = 0; i < count; ++i) {
        float r = maps.mrahw[i * 4 + 1] / 255.0f;
        float h = maps.mrahw[i * 4 + 2] / 255.0f;
        CHECK(r >= 0.0f && r <= 1.0f);
        CHECK(h >= 0.0f && h <= 1.0f);
    }
    return 0;
}

static int testWetnessRange() {
    int size = 16;
    auto rgba = makeCheckerboard(size);
    PbrMapSet maps = generatePbrMaps(rgba.data(), size, size, "ground", 1.0f);

    int count = size * size;
    for (int i = 0; i < count; ++i) {
        float w = maps.mrahw[i * 4 + 3] / 255.0f;
        CHECK(w >= 0.0f && w <= 1.0f);
    }
    return 0;
}

int main() {
    std::cout << "Running PBR map generation tests...\n";
    testNormalMapLength();
    testNeutralMetallicFull();
    testRoughnessAndHeightRanges();
    testWetnessRange();

    if (g_failures == 0) {
        std::cout << "All PBR tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " PBR test(s) failed.\n";
    return 1;
}
