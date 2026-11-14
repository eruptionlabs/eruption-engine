#include "EtexLoader.hpp"

#include "core/Logger.hpp"
#include "utils/AssetVfs.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace eruption {

namespace fs = std::filesystem;

static constexpr uint32_t kEtexMagic = 0x31585445; // 'ETX1'

EtexData loadEtexForImage(const std::string& imagePath, const std::string& ext) {
    EtexData out;
    if (imagePath.empty()) return out;
    const std::string etexPath = imagePath + ext;

    // Pacote do mapa (.emap) primeiro: tamanho, data e bytes do build.
    uint64_t etexSize = 0, srcSize = 0;
    int64_t bakT = 0, srcT = 0;
    if (!vfs::stat(etexPath, etexSize, bakT)) return out;
    // Stale bake (source edited after baking): ignore, runtime path decodes.
    if (vfs::stat(imagePath, srcSize, srcT) && bakT < srcT) return out;

    std::vector<uint8_t> bytes;
    if (!vfs::read(etexPath, bytes) || bytes.size() < 20) return out;
    size_t c = 0;
    auto take = [&](void* dst, size_t n) {
        if (c + n > bytes.size()) return false;
        std::memcpy(dst, bytes.data() + c, n);
        c += n;
        return true;
    };

    uint32_t header[5] = {};
    if (!take(header, sizeof(header)) || header[0] != kEtexMagic) return out;
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
        if (!take(&sz, sizeof(sz)) || sz == 0 || sz > (256u << 20)) return out;
        const size_t base = data.payload.size();
        data.payload.resize(base + sz);
        if (!take(data.payload.data() + base, sz)) return out;
        data.mipSizes.push_back(sz);
    }
    return data;
}

} // namespace eruption
