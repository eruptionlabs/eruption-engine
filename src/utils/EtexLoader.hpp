#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

// Baked GPU-compressed texture (tools/bake_assets.py): BC1/BC3 with all mip
// levels precomputed. The engine only reads and uploads - no PNG decode, no
// magenta keying, no dilation, no mip blit chain.
struct EtexData {
    uint32_t vkFormat = 0;   // VK_FORMAT_BC1_RGBA_UNORM_BLOCK / BC3_UNORM_BLOCK
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    std::vector<uint8_t> payload;    // all mips, contiguous
    std::vector<uint32_t> mipSizes;  // byte size per mip, payload order

    bool valid() const { return vkFormat != 0 && mipCount > 0 && !payload.empty(); }
    size_t byteSize() const { return payload.size(); }
};

// Loads "<imagePath>.etex" if present, well-formed and not older than the
// source image. Returns an invalid EtexData otherwise (caller falls back to
// the runtime decode path).
EtexData loadEtexForImage(const std::string& imagePath);

} // namespace eruption
