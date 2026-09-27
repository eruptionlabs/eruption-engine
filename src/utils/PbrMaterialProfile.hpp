#pragma once

#include <string>
#include <unordered_map>

namespace eruption {

// PBR material profile for a single albedo texture.
// Populated from assets/data/pbr_materials.json (generated externally).
// Values are normalized to [0,1] unless otherwise noted.
struct PbrMaterialProfile {
    float metallic = 0.0f;   // default dielectric (non-metal) to avoid reddish tint
    float roughness = 0.8f;  // default slightly rough, natural look
    float ao = 1.0f;         // ambient occlusion factor (not written to gbuffer yet)
    bool wettable = false;   // whether this surface can become wet from rain
    std::string category;    // semantic category, e.g. "water", "stone", "metal"
};

// Singleton that loads and serves PBR material profiles by texture name.
// The lookup key is case-insensitive, extension-stripped, and normalizes
// both slash directions and common path prefixes (data/, texture/) so that
// "texture/vila/nam1_1.bmp", "vila\\nam1_1.bmp" and "nam1_1" resolve to
// the same profile.
class PbrMaterialProfileManager {
public:
    static PbrMaterialProfileManager& instance();

    // Load profiles from a JSON file. If the file does not exist or is
    // malformed, the manager keeps the default profile set and logs a
    // non-fatal message. Safe to call multiple times; replaces existing data.
    void loadFromFile(const std::string& path);

    // Return the profile for a texture path, or the default profile if none
    // is registered. The path is normalized before lookup.
    const PbrMaterialProfile& getProfile(const std::string& texturePath) const;

    bool hasAnyProfiles() const { return !m_profiles.empty(); }

private:
    PbrMaterialProfileManager() = default;

    static std::string normalizeKey(std::string path);

    std::unordered_map<std::string, PbrMaterialProfile> m_profiles;
    PbrMaterialProfile m_defaultProfile;
};

// Convenience free function.
inline const PbrMaterialProfile& getPbrProfile(const std::string& texturePath) {
    return PbrMaterialProfileManager::instance().getProfile(texturePath);
}

} // namespace eruption
