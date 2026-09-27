#include "utils/PbrMaterialProfile.hpp"
#include "core/Logger.hpp"
#include "renderer/SpritePickerUI.hpp" // for eucKrToUtf8

#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace eruption {

PbrMaterialProfileManager& PbrMaterialProfileManager::instance() {
    static PbrMaterialProfileManager s_instance;
    return s_instance;
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string PbrMaterialProfileManager::normalizeKey(std::string path) {
    // pack/source world texture names are often stored in EUC-KR bytes. Convert them to
    // UTF-8 before lookup so legacy folder names match the pbr_materials.json
    // entries (which are stored as UTF-8).
    if (!isValidUtf8(path)) {
        path = eucKrToUtf8(path);
    }

    // Normalize backslashes to forward slashes.
    std::replace(path.begin(), path.end(), '\\', '/');

    // Strip a leading separator.
    if (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }

    // Strip common prefixes so the same texture can be referenced with or
    // without the data/texture container.
    if (path.rfind("data/", 0) == 0) {
        path = path.substr(5);
    }
    if (path.rfind("texture/", 0) == 0) {
        path = path.substr(8);
    }

    // Strip extension while keeping the relative folder, then lower-case.
    // pbr_materials.json keys are relative to assets/data/texture/ and include
    // subdirectories (e.g. "town/al-hou-r1"), so dropping the folder broke
    // lookups for textures outside the root texture folder.
    fs::path p(path);
    path = p.replace_extension().string();
    return toLower(path);
}

void PbrMaterialProfileManager::loadFromFile(const std::string& path) {
    m_profiles.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        ERUPTION_LOG_INFO("PbrMaterialProfile: no profile file at '%s', using defaults.", path.c_str());
        return;
    }

    try {
        nlohmann::json j;
        file >> j;

        if (!j.is_object()) {
            ERUPTION_LOG_WARN("PbrMaterialProfile: root is not an object in '%s'", path.c_str());
            return;
        }

        for (auto& [key, value] : j.items()) {
            PbrMaterialProfile profile;
            if (value.is_object()) {
                if (value.contains("metallic") && value["metallic"].is_number()) {
                    profile.metallic = value["metallic"].get<float>();
                }
                if (value.contains("roughness") && value["roughness"].is_number()) {
                    profile.roughness = value["roughness"].get<float>();
                }
                if (value.contains("ao") && value["ao"].is_number()) {
                    profile.ao = value["ao"].get<float>();
                }
                if (value.contains("wettable") && value["wettable"].is_boolean()) {
                    profile.wettable = value["wettable"].get<bool>();
                }
                if (value.contains("category") && value["category"].is_string()) {
                    profile.category = value["category"].get<std::string>();
                }
            }
            m_profiles[normalizeKey(key)] = profile;
        }

        ERUPTION_LOG_INFO("PbrMaterialProfile: loaded %zu profiles from '%s'", m_profiles.size(), path.c_str());
    } catch (const std::exception& e) {
        ERUPTION_LOG_ERROR("PbrMaterialProfile: failed to parse '%s': %s", path.c_str(), e.what());
    }
}

const PbrMaterialProfile& PbrMaterialProfileManager::getProfile(const std::string& texturePath) const {
    auto it = m_profiles.find(normalizeKey(texturePath));
    if (it != m_profiles.end()) {
        return it->second;
    }
    return m_defaultProfile;
}

} // namespace eruption
