#include "core/ConfigManager.hpp"
#include "core/Logger.hpp"
#include "utils/AssetVfs.hpp"
#include <fstream>

namespace eruption {

bool ConfigManager::load(const std::string& path) {
    m_path = path;
    // Sem arquivo (ou com erro), a raiz fica um objeto vazio: quem le usa
    // value()/contains() e cai nos padroes; com json nulo, value() lanca.
    m_data = json::object();
    std::string text; // disco ou pacote .ecp do projeto
    if (!vfs::readText(path, text)) {
        ERUPTION_LOG_WARN("Config file not found: %s", path.c_str());
        return false;
    }

    try {
        m_data = json::parse(text);
        std::error_code tec; // do pacote: sem data de arquivo
        m_lastWriteTime = std::filesystem::last_write_time(path, tec);
        ERUPTION_LOG_INFO("Config loaded: %s", path.c_str());
        return true;
    } catch (const std::exception& e) {
        ERUPTION_LOG_ERROR("Failed to parse config %s: %s", path.c_str(), e.what());
        m_data = json::object();
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
