#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <filesystem>
#include <fstream>
#include <chrono>

namespace eruption {

using json = nlohmann::json;

class ConfigManager {
public:
    ConfigManager() = default;

    // Load config from file. Returns false if file not found or invalid.
    bool load(const std::string& path);

    // Check if file was modified since last load. Call this periodically.
    bool wasModified() const;

    // Reload if modified. Returns true if reloaded.
    bool reloadIfModified();

    // Get the root JSON object
    const json& root() const { return m_data; }
    json& root() { return m_data; }

    // Convenience: get value with fallback
    template<typename T>
    T get(const std::string& key, const T& defaultValue) const {
        try {
            if (m_data.contains(key)) {
                return m_data[key].get<T>();
            }
        } catch (...) {}
        return defaultValue;
    }

    // Convenience: nested get with dot-separated path (e.g. "shadows.enabled")
    template<typename T>
    T getPath(const std::string& path, const T& defaultValue) const {
        try {
            const json* node = &m_data;
            size_t start = 0;
            while (true) {
                size_t dot = path.find('.', start);
                std::string key = (dot == std::string::npos) ? path.substr(start) : path.substr(start, dot - start);
                if (!node->contains(key)) return defaultValue;
                node = &(*node)[key];
                if (dot == std::string::npos) break;
                start = dot + 1;
            }
            return node->get<T>();
        } catch (...) {
            return defaultValue;
        }
    }

    // Access raw json node by path (returns nullptr if not found)
    const json* getNode(const std::string& path) const;

    const std::string& path() const { return m_path; }

private:
    std::string m_path;
    json m_data;
    std::filesystem::file_time_type m_lastWriteTime;
};

} // namespace eruption
