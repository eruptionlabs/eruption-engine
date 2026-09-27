#include "EtexLoader.hpp"

#include "core/Logger.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace eruption {

namespace fs = std::filesystem;

static constexpr uint32_t kEtexMagic = 0x31585445; // 'ETX1'

EtexData loadEtexForImage(const std::string& imagePath) {
    EtexData out;
    if (imagePath.empty()) return out;
    const std::string etexPath = imagePath + ".etex";

    std::error_code ec;
    if (!fs::exists(etexPath, ec)) return out;
    // Stale bake (source edited after baking): ignore, runtime path decodes.
    if (fs::exists(imagePath, ec)) {
        const auto srcT = fs::last_write_time(imagePath, ec);
        const auto bakT = fs::last_write_time(etexPath, ec);
        if (!ec && bakT < srcT) return out;
    }

    std::ifstream f(etexPath, std::ios::binary);
    if (!f.is_open()) return out;

    uint32_t header[5] = {};
    f.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!f.good() || header[0] != kEtexMagic) return out;
    const uint32_t mipCount = header[4];
    if (mipCount == 0 || mipCount > 16 || header[2] == 0 || header[3] == 0) return out;

    EtexData data;
    data.vkFormat = header[1];
    data.width = header[2];
    data.height = header[3];
    data.mipCount = mipCount;
    data.mipSizes.reserve(mipCount);
    for (uint32_t i = 0; i < mipCount; ++i) {
        uint32_t sz = 0;
        f.read(reinterpret_cast<char*>(&sz), sizeof(sz));
        if (!f.good() || sz == 0 || sz > (256u << 20)) return out;
        const size_t base = data.payload.size();
        data.payload.resize(base + sz);
        f.read(reinterpret_cast<char*>(data.payload.data() + base),
               static_cast<std::streamsize>(sz));
        if (!f.good()) return out;
        data.mipSizes.push_back(sz);
    }
    return data;
}

} // namespace eruption
