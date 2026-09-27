#include "TextureCache.hpp"

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace eruption {

namespace fs = std::filesystem;

static std::string toHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

static size_t hashBytes(const uint8_t* data, size_t size) {
    // FNV-1a 64-bit
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return static_cast<size_t>(h);
}

std::string TextureCache::getCacheDir() {
    return ".cache/eruption/textures/";
}

void TextureCache::ensureCacheDir() {
    fs::create_directories(getCacheDir());
}

std::string TextureCache::computeKey(const std::string& name,
                                     const uint8_t* data,
                                     size_t size) {
    size_t nameHash = std::hash<std::string>{}(name);
    size_t dataHash = hashBytes(data, size);
    size_t combined = nameHash ^ (dataHash + 0x9e3779b97f4a7c15ULL + (nameHash << 6) + (nameHash >> 2));

    std::array<uint8_t, 8> digest{};
    for (size_t i = 0; i < 8; ++i) {
        digest[i] = static_cast<uint8_t>(combined >> (i * 8));
    }

    std::string safeName = name;
    for (char& c : safeName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
            c = '_';
        }
    }
    return safeName + "_" + toHex(digest.data(), digest.size()) + ".bin";
}

bool TextureCache::load(const std::string& name,
                        const std::vector<uint8_t>& encodedData,
                        int& width,
                        int& height,
                        int& channels,
                        std::vector<uint8_t>& pixels) {
    ensureCacheDir();
    std::string key = computeKey(name, encodedData.data(), encodedData.size());
    fs::path path = fs::path(getCacheDir()) / key;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    int32_t w = 0, h = 0, c = 0;
    file.read(reinterpret_cast<char*>(&w), sizeof(w));
    file.read(reinterpret_cast<char*>(&h), sizeof(h));
    file.read(reinterpret_cast<char*>(&c), sizeof(c));
    if (!file || w <= 0 || h <= 0 || c <= 0) {
        return false;
    }

    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(c);
    pixels.resize(expected);
    file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(expected));
    if (!file || pixels.size() != expected) {
        pixels.clear();
        return false;
    }

    width = w;
    height = h;
    channels = c;
    return true;
}

bool TextureCache::save(const std::string& name,
                        const std::vector<uint8_t>& encodedData,
                        int width,
                        int height,
                        int channels,
                        const std::vector<uint8_t>& pixels) {
    if (width <= 0 || height <= 0 || channels <= 0 || pixels.empty()) {
        return false;
    }
    ensureCacheDir();
    std::string key = computeKey(name, encodedData.data(), encodedData.size());
    fs::path path = fs::path(getCacheDir()) / key;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    int32_t w = width, h = height, c = channels;
    file.write(reinterpret_cast<const char*>(&w), sizeof(w));
    file.write(reinterpret_cast<const char*>(&h), sizeof(h));
    file.write(reinterpret_cast<const char*>(&c), sizeof(c));
    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return file.good();
}

} // namespace eruption
