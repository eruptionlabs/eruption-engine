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

// Fator de deslocamento por categoria (push.uvScale.w / ModelMeshGPU::dispScale).
// UNICA fonte de verdade - usado tanto pelo chao real (ModelRenderer.cpp)
// quanto pelo popup de preview (SpherePreview.cpp), pra' nao ter dois lugares
// que podem desalinhar (era exatamente o bug: o preview tinha 1.0 fixo,
// ignorando a categoria de verdade do material clicado). Chao, terra e neve
// tem relevo de verdade e levam a amplitude cheia. Pedra e material sem
// categoria (todo modelo de cena cujo nome nao esta' no pbr_materials.json)
// tambem: eram 0,45 e 0,35 pelo medo do triangulo GRANDE esticado em espeto,
// que a grade por distancia (aresta em pixels, model.tesc) e a solda da
// costura ja' resolvem. Com 0,35 o relevo das rochas e paredoes na camera de
// orbita a 250 u nao se lia (autor, 2026-10-27: "so' o chao ganha"); com 1,0
// a borda dos terracos vira pedra recortada. Madeira fica em 0,30 (tabua
// lisa). Telhado, metal e agua ficam em zero: superficie dura, lisa ou com
// shader proprio.
inline float dispScaleForCategory(const std::string& category) {
    if (category == "ground" || category == "dirt" || category == "snow") return 1.0f;
    if (category == "grass") return 0.6f;
    if (category == "stone") return 1.0f;
    if (category == "wood") return 0.30f;
    if (category == "roof" || category == "metal" || category == "water" ||
        category == "vegetation") return 0.0f;
    return 1.0f;
}

} // namespace eruption
