#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_set>
#include <memory>
#include <mutex>

namespace eruption {

struct ResourceSourceInfo {
    std::string path;
    bool isDirectory;
    uint32_t fileCount = 0;
};

// Simple directory-based asset manager.
// Assets are expected to live under one or more root directories on disk
// (e.g. assets/, assets/external/). Packed archive formats are not supported.
class PackManager {
public:
    void addDirectory(const std::string& dir);
    void clear();

    std::vector<uint8_t> extract(const std::string& filename) const;
    bool exists(const std::string& filename) const;
    std::vector<std::string> getAllFilenames() const;
    std::vector<std::string> getFilenamesByPrefix(const std::string& prefix) const;

    size_t dirCount() const { return m_dirs.size(); }
    size_t sourceCount() const { return m_dirs.size(); }

    const std::vector<ResourceSourceInfo>& getSources() const { return m_sources; }

    // Thread-safe wrapper
    std::vector<uint8_t> extractSafe(const std::string& filename) const;

private:
    struct DirCache {
        std::string rootPath;
        std::unordered_set<std::string> files;
    };

    mutable std::mutex m_mutex;
    std::vector<DirCache> m_dirs;
    std::vector<ResourceSourceInfo> m_sources;

    static std::string normalizePath(const std::string& path);
    static bool pathContains(const std::string& haystack, const std::string& needle);
    void scanDirectory(const std::string& dir, DirCache& cache);
};

} // namespace eruption
