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
    std::string configPath = "resources.ini";

    bool load(const std::string& path = "resources.ini") {
        configPath = path;
        entries.clear();
        std::ifstream file(path);
        if (!file.is_open()) {
            ERUPTION_LOG_WARN("Resource config not found: %s", path.c_str());
            return false;
        }

        std::string line;
        bool inDataSection = false;
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
