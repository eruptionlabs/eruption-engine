#include "formats/SpriteParser.hpp"
#include "core/Logger.hpp"
#include <cstring>
#include <fstream>

namespace eruption {

SpriteParser::Result SpriteParser::parse(const uint8_t* data, size_t size) {
    Result result;
    if (size < 28) {
        ERUPTION_LOG_WARN("SpriteParser: file too small (%zu bytes)", size);
        return result;
    }

    size_t pos = 0;
    auto readU32 = [&]() -> uint32_t {
        uint32_t v = 0;
        if (pos + 4 <= size) std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    };
    auto readI32 = [&]() -> int32_t {
        int32_t v = 0;
        if (pos + 4 <= size) std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    };
    auto readF32 = [&]() -> float {
        float v = 0.0f;
        if (pos + 4 <= size) std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    };

    char magic[8];
    if (size < 8) return result;
    std::memcpy(magic, data, 8);
    pos = 8;

    if (std::memcmp(magic, "ERUPTSPR", 8) != 0) {
        ERUPTION_LOG_WARN("SpriteParser: invalid magic (expected ERUPTSPR)");
        return result;
    }

    uint32_t version = readU32();
    if (version != 1) {
        ERUPTION_LOG_WARN("SpriteParser: unsupported version %u", version);
        return result;
    }

    uint32_t texWidth = readU32();
    uint32_t texHeight = readU32();
    uint32_t texChannels = readU32();
    if (texWidth == 0 || texHeight == 0 || texChannels != 4) {
        ERUPTION_LOG_WARN("SpriteParser: invalid texture %ux%ux%u", texWidth, texHeight, texChannels);
        return result;
    }

    size_t pixelBytes = static_cast<size_t>(texWidth) * texHeight * texChannels;
    if (size - pos < pixelBytes) {
        ERUPTION_LOG_WARN("SpriteParser: truncated pixel data (need %zu, have %zu)", pixelBytes, size - pos);
        return result;
    }

    SprImage img;
    img.width = static_cast<uint16_t>(texWidth);
    img.height = static_cast<uint16_t>(texHeight);
    img.isIndexed = false;
    img.pixels.assign(data + pos, data + pos + pixelBytes);
    pos += pixelBytes;

    // Pre-process alpha the same way the engine does for PNGs: hard cut at 128.
    for (size_t i = 0; i < texWidth * texHeight; ++i) {
        uint8_t a = img.pixels[i * 4 + 3];
        if (a < 128) {
            img.pixels[i * 4 + 0] = 0;
            img.pixels[i * 4 + 1] = 0;
            img.pixels[i * 4 + 2] = 0;
            img.pixels[i * 4 + 3] = 0;
        } else {
            img.pixels[i * 4 + 3] = 255;
        }
    }

    result.sprite.images.push_back(std::move(img));

    if (size - pos < 8) {
        ERUPTION_LOG_WARN("SpriteParser: missing animation header");
        return result;
    }

    uint32_t dirCount = readU32();
    uint32_t actionCount = readU32();
    if (dirCount == 0 || dirCount > 64 || actionCount == 0 || actionCount > 64) {
        ERUPTION_LOG_WARN("SpriteParser: invalid dirCount=%u actionCount=%u", dirCount, actionCount);
        return result;
    }

    AnimFile anim;
    anim.version = static_cast<uint16_t>(version);

    for (uint32_t a = 0; a < actionCount; ++a) {
        if (size - pos < 4) {
            ERUPTION_LOG_WARN("SpriteParser: missing framesPerDir for action %u", a);
            return result;
        }
        uint32_t framesPerDir = readU32();
        if (framesPerDir == 0 || framesPerDir > 256) {
            ERUPTION_LOG_WARN("SpriteParser: invalid framesPerDir=%u", framesPerDir);
            return result;
        }

        ActAction action;
        action.delay = 150.0f;
        for (uint32_t d = 0; d < dirCount; ++d) {
            ActFrame frame;
            for (uint32_t fr = 0; fr < framesPerDir; ++fr) {
                if (size - pos < 4) {
                    ERUPTION_LOG_WARN("SpriteParser: missing spriteCount");
                    return result;
                }
                uint32_t spriteCount = readU32();
                if (spriteCount == 0 || spriteCount > 256) {
                    ERUPTION_LOG_WARN("SpriteParser: invalid spriteCount=%u", spriteCount);
                    return result;
                }
                for (uint32_t s = 0; s < spriteCount; ++s) {
                    if (size - pos < 28) {
                        ERUPTION_LOG_WARN("SpriteParser: truncated sprite data");
                        return result;
                    }
                    ActSprite spr{};
                    spr.index = readU32();
                    spr.type = 1; // RGBA texture
                    spr.offsetX = readI32();
                    spr.offsetY = readI32();
                    spr.scaleX = readF32();
                    spr.scaleY = readF32();
                    spr.color = Vec4(readF32(), readF32(), readF32(), readF32());
                    spr.width = result.sprite.images[0].width;
                    spr.height = result.sprite.images[0].height;
                    frame.sprites.push_back(spr);
                }
            }
            action.frames.push_back(frame);
        }
        anim.actions.push_back(action);
    }

    result.anim = std::move(anim);
    result.ok = true;
    return result;
}

SpriteParser::Result SpriteParser::parseFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ERUPTION_LOG_WARN("SpriteParser: failed to open %s", path.c_str());
        return Result{};
    }
    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return parse(data.data(), data.size());
}

} // namespace eruption
