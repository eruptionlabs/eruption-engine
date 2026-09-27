#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

/**
 * Simple on-disk cache for decoded embedded textures.
 * Stores raw RGBA8 pixels keyed by texture name + encoded data hash.
 * Safe to call from multiple threads (each key maps to a unique file).
 */
class TextureCache {
public:
    static std::string getCacheDir();

    /**
     * Try to load cached decoded pixels for an embedded texture.
     * Returns true on success and fills width/height/channels/pixels.
     */
    static bool load(const std::string& name,
                     const std::vector<uint8_t>& encodedData,
                     int& width,
                     int& height,
                     int& channels,
                     std::vector<uint8_t>& pixels);

    /**
     * Save decoded pixels to disk cache.
     */
    static bool save(const std::string& name,
                     const std::vector<uint8_t>& encodedData,
                     int width,
                     int height,
                     int channels,
                     const std::vector<uint8_t>& pixels);

    /**
     * Compute a filesystem-safe cache key from name + encoded bytes.
     */
    static std::string computeKey(const std::string& name,
                                  const uint8_t* data,
                                  size_t size);

private:
    static void ensureCacheDir();
};

} // namespace eruption
