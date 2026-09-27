#include "formats/PackManager.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace eruption {

static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string PackManager::normalizePath(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '/', '\\');
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool PackManager::pathContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void PackManager::scanDirectory(const std::string& dir, DirCache& cache) {
    cache.rootPath = dir;
    cache.files.clear();
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        ERUPTION_LOG_WARN("PackManager: directory not found: %s", dir.c_str());
        return;
    }

    try {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string rel = fs::relative(entry.path(), dir).string();
            cache.files.insert(normalizePath(rel));
        }
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("PackManager: error scanning %s: %s", dir.c_str(), e.what());
    }
}

void PackManager::addDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    DirCache cache;
    scanDirectory(dir, cache);
    m_dirs.push_back(std::move(cache));

    ResourceSourceInfo info;
    info.path = dir;
    info.isDirectory = true;
    info.fileCount = static_cast<uint32_t>(m_dirs.back().files.size());
    m_sources.push_back(info);
}

void PackManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dirs.clear();
    m_sources.clear();
}

std::vector<uint8_t> PackManager::extract(const std::string& filename) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string norm = normalizePath(filename);

    for (const auto& dir : m_dirs) {
        // Exact relative match
        fs::path candidate = fs::path(dir.rootPath) / fs::path(norm);
        if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
            std::ifstream f(candidate, std::ios::binary);
            if (!f) continue;
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
        }

        // Case-insensitive basename scan
        std::string baseName;
        size_t lastSlash = norm.find_last_of('\\');
        if (lastSlash != std::string::npos) {
            baseName = norm.substr(lastSlash + 1);
        } else {
            baseName = norm;
        }
        std::string lowerBase = toLower(baseName);

        try {
            for (const auto& entry : fs::recursive_directory_iterator(dir.rootPath)) {
                if (!entry.is_regular_file()) continue;
                std::string entryName = toLower(entry.path().filename().string());
                if (entryName == lowerBase) {
                    std::ifstream f(entry.path(), std::ios::binary);
                    if (!f) continue;
                    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                                 std::istreambuf_iterator<char>());
                }
            }
        } catch (const std::exception& e) {
            ERUPTION_LOG_WARN("PackManager: extract fallback scan failed for %s: %s", filename.c_str(), e.what());
        }
    }

    return {};
}

std::vector<uint8_t> PackManager::extractSafe(const std::string& filename) const {
    return extract(filename);
}

bool PackManager::exists(const std::string& filename) const {
    return !extract(filename).empty();
}

std::vector<std::string> PackManager::getAllFilenames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& dir : m_dirs) {
        for (const auto& f : dir.files) {
            result.push_back(f);
        }
    }
    return result;
}

std::vector<std::string> PackManager::getFilenamesByPrefix(const std::string& prefix) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> result;
    std::string normPrefix = normalizePath(prefix);

    for (const auto& dir : m_dirs) {
        for (const auto& f : dir.files) {
            if (f.size() >= normPrefix.size() &&
                std::mismatch(normPrefix.begin(), normPrefix.end(), f.begin()).first == normPrefix.end()) {
                result.push_back(f);
            }
        }
    }
    return result;
}

} // namespace eruption
