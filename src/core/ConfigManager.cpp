#include "core/ConfigManager.hpp"
#include "core/Logger.hpp"
#include <fstream>

namespace eruption {

bool ConfigManager::load(const std::string& path) {
    m_path = path;
    std::ifstream file(path);
    if (!file.is_open()) {
        ERUPTION_LOG_WARN("Config file not found: %s", path.c_str());
        return false;
    }

    try {
        file >> m_data;
        m_lastWriteTime = std::filesystem::last_write_time(path);
        ERUPTION_LOG_INFO("Config loaded: %s", path.c_str());
        return true;
    } catch (const std::exception& e) {
        ERUPTION_LOG_ERROR("Failed to parse config %s: %s", path.c_str(), e.what());
        return false;
    }
}

bool ConfigManager::wasModified() const {
    if (m_path.empty()) return false;
    try {
        auto currentTime = std::filesystem::last_write_time(m_path);
        return currentTime != m_lastWriteTime;
    } catch (...) {
        return false;
    }
}

bool ConfigManager::reloadIfModified() {
    if (wasModified()) {
        return load(m_path);
    }
    return false;
}

const json* ConfigManager::getNode(const std::string& path) const {
    try {
        const json* node = &m_data;
        size_t start = 0;
        while (true) {
            size_t dot = path.find('.', start);
            std::string key = (dot == std::string::npos) ? path.substr(start) : path.substr(start, dot - start);
            if (!node->contains(key)) return nullptr;
            node = &(*node)[key];
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return node;
    } catch (...) {
        return nullptr;
    }
}

} // namespace eruption
