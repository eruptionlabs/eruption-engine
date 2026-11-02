#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "core/Logger.hpp"

namespace eruption {

struct ResourceEntry {
    std::string path;
    bool isDirectory = false;
};

class ResourceConfig {
public:
    std::vector<ResourceEntry> entries;
    // Trechos de caminho (minusculos) dos sprites que ganham recorte do contorno
    // por abertura morfologica; vem da secao [sprites], chave clean_bounds
    // (lista separada por virgula). Vazio = nenhum sprite usa o recorte.
    std::vector<std::string> cleanBoundsPatterns;
    std::string configPath = "resources.ini";

    bool load(const std::string& path = "resources.ini") {
        configPath = path;
        entries.clear();
        cleanBoundsPatterns.clear();
        std::ifstream file(path);
        if (!file.is_open()) {
            ERUPTION_LOG_WARN("Resource config not found: %s", path.c_str());
            return false;
        }

        std::string line;
        bool inDataSection = false;
        bool inSpritesSection = false;
        while (std::getline(file, line)) {
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(start, end - start + 1);

            if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
            if (trimmed[0] == '[' && trimmed.back() == ']') {
                std::string section = trimmed.substr(1, trimmed.size() - 2);
                std::transform(section.begin(), section.end(), section.begin(), ::tolower);
                inDataSection = (section == "data");
                inSpritesSection = (section == "sprites");
                continue;
            }

            if (inSpritesSection) {
                size_t sEq = trimmed.find('=');
                if (sEq == std::string::npos) continue;
                std::string key = trimmed.substr(0, sEq);
                key.erase(key.find_last_not_of(" \t") + 1);
                if (key != "clean_bounds") continue;
                std::stringstream list(trimmed.substr(sEq + 1));
                std::string item;
                while (std::getline(list, item, ',')) {
                    size_t a = item.find_first_not_of(" \t");
                    if (a == std::string::npos) continue;
                    item = item.substr(a, item.find_last_not_of(" \t") - a + 1);
                    std::transform(item.begin(), item.end(), item.begin(), ::tolower);
                    cleanBoundsPatterns.push_back(item);
                }
                continue;
            }

            if (!inDataSection) continue;

            // Parse key=value (e.g., 0=/path/to/assets)
            size_t eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string value = trimmed.substr(eq + 1);

            // Trim value
            size_t vstart = value.find_first_not_of(" \t\r\n");
            size_t vend = value.find_last_not_of(" \t\r\n");
            if (vstart == std::string::npos) continue;
            std::string pathVal = value.substr(vstart, vend - vstart + 1);

            // Remove quotes if present
            if (pathVal.size() >= 2 && pathVal.front() == '"' && pathVal.back() == '"') {
                pathVal = pathVal.substr(1, pathVal.size() - 2);
            }

            ResourceEntry entry;
            entry.path = pathVal;
            entry.isDirectory = true;
            entries.push_back(entry);
        }
        return true;
    }

    bool save(const std::string& path = "resources.ini") const {
        std::ofstream file(path);
        if (!file.is_open()) {
            ERUPTION_LOG_ERROR("Failed to save resource config: %s", path.c_str());
            return false;
        }
        file << "[Data]\n";
        for (size_t i = 0; i < entries.size(); i++) {
            file << i << "=" << entries[i].path << "\n";
        }
        if (!cleanBoundsPatterns.empty()) {
            file << "\n[sprites]\nclean_bounds=";
            for (size_t i = 0; i < cleanBoundsPatterns.size(); i++)
                file << (i ? "," : "") << cleanBoundsPatterns[i];
            file << "\n";
        }
        return true;
    }

    void createDefault(const std::string& path = "resources.ini") {
        configPath = path;
        entries.clear();
        // Default source: the texture folder shipped with the repo. Extra
        // sources can be appended in resources.ini.
        entries.push_back({"assets/data/texture", true});
        save(path);
    }
};

} // namespace eruption
