#pragma once

#include "math/Types.hpp"
#include <vector>
#include <cstdint>
#include <string>

namespace eruption {

// Generic, engine-side sprite data structures.
// These are intentionally format-agnostic: the native ERUPTSPR format and
// simple PNG billboards all get normalized into these types before rendering.

struct SprImage {
    uint16_t width = 0, height = 0;
    std::vector<uint8_t> pixels; // RGBA8 (or palette indices when isIndexed=true)
    bool isIndexed = false;
};

struct SpriteFile {
    std::vector<SprImage> images;        // RGBA images
    std::vector<SprImage> indexedImages; // Palette indices
    uint8_t palette[256 * 4] = {};       // RGBA palette
};

struct ActAttachPoint {
    int32_t x = 0;
    int32_t y = 0;
    int32_t attr = 0;
};

struct ActSprite {
    int32_t offsetX = 0, offsetY = 0;
    int32_t index = 0;
    int32_t flags = 0;
    Vec4 color = Vec4(1.0f);
    float scaleX = 1.0f, scaleY = 1.0f;
    int32_t angle = 0;
    int32_t type = 0;   // 0 = indexed, 1 = RGBA
    int32_t width = 0, height = 0;
};

struct ActFrame {
    uint32_t range1[4] = {};
    uint32_t range2[4] = {};
    std::vector<ActSprite> sprites;
    int32_t eventId = -1;
    std::vector<ActAttachPoint> attachPoints;
};

struct ActAction {
    std::vector<ActFrame> frames;
    float delay = 150.0f;
};

struct ActEvent {
    char name[40] = {};
};

struct AnimFile {
    uint16_t version = 0;
    std::vector<ActAction> actions;
    std::vector<ActEvent> events;
    std::vector<float> delays;
};

} // namespace eruption
