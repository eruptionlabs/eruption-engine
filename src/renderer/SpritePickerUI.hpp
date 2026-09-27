#pragma once
#include <string>
#include <vector>
#include "renderer/PaletteManager.hpp"
#include "utils/SplineCurve.hpp"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <iconv.h>
#include "formats/PackManager.hpp"

namespace eruption {

inline bool isValidUtf8(const std::string& str) {
    size_t i = 0;
    while (i < str.size()) {
        unsigned char c = str[i];
        int len = 0;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;
        if (i + len > str.size()) return false;
        for (int j = 1; j < len; j++) {
            if ((str[i + j] & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

inline std::string eucKrToUtf8(const std::string& input) {
    if (input.empty()) return "";
    iconv_t cd = iconv_open("UTF-8", "EUC-KR");
    if (cd == (iconv_t)-1) return input;
    size_t inLen = input.size();
    size_t outLen = inLen * 4 + 4;
    std::string output(outLen, '\0');
    char* inBuf = const_cast<char*>(input.data());
    char* outBuf = output.data();
    if (iconv(cd, &inBuf, &inLen, &outBuf, &outLen) == (size_t)-1) {
        iconv_close(cd);
        return input;
    }
    iconv_close(cd);
    output.resize(output.size() - outLen);
    return output;
}

inline std::string utf8ToEucKr(const std::string& input) {
    if (input.empty()) return "";
    iconv_t cd = iconv_open("EUC-KR", "UTF-8");
    if (cd == (iconv_t)-1) return input;
    size_t inLen = input.size();
    size_t outLen = inLen * 2 + 4;
    std::string output(outLen, '\0');
    char* inBuf = const_cast<char*>(input.data());
    char* outBuf = output.data();
    if (iconv(cd, &inBuf, &inLen, &outBuf, &outLen) == (size_t)-1) {
        iconv_close(cd);
        return input;
    }
    iconv_close(cd);
    output.resize(output.size() - outLen);
    return output;
}

inline std::string encodeForJson(const std::string& raw) {
    if (isValidUtf8(raw)) return raw;
    return eucKrToUtf8(raw);
}

inline std::string decodeFromJson(const std::string& stored) {
    if (!isValidUtf8(stored)) return stored;
    bool hasMultibyte = false;
    for (unsigned char c : stored) { if (c > 0x7F) { hasMultibyte = true; break; } }
    if (!hasMultibyte) return stored;
    return utf8ToEucKr(stored);
}

inline std::string toDisplayUtf8(const std::string& raw) {
    bool hasHigh = false;
    for (unsigned char c : raw) { if (c > 0x7F) { hasHigh = true; break; } }
    if (!hasHigh) return raw;
    if (isValidUtf8(raw)) return raw;
    return eucKrToUtf8(raw);
}

struct CharConfig {
    std::string job;
    std::string hair;
    int gender = 0; // 0=M, 1=F
    Palette bodyPalette;
    Palette hairPalette;

    // Character rendering & animation tuning
    float billboardTilt = 45.0f;
    float walkSpeed = 125.0f; // This will be calculated dynamically now
    float moveSpeed = 50.0f;   // Default movement speed set to 50 as requested
    float planarShadowSoftness = 0.0f;
    float planarShadowDilation = 1.0f;
    int shadowType = 1;        // 0 = Planar, 1 = Circle
    float circleShadowSoftness = 0.5f;
    float circleShadowDilation = 1.0f;

    // Adaptive billboard tilt spline: curve mapping camera pitch in degrees (X, stored normalized 0..1)
    // to billboard tilt in degrees (Y). Domain is pitch 0..89; tilt can be -90..90.
    SplineCurve billboardTiltSpline = SplineCurve(
        {10.0f / 89.0f, 50.0f / 89.0f, 70.0f / 89.0f}, // pitch knots (normalized)
        {-13.5f, 5.0f, 13.5f},                          // tilt knots
        InterpolationMode::Linear
    );

    CharConfig() {
        for(int i=0; i<1024; i++) { bodyPalette.colors[i] = 255; hairPalette.colors[i] = 255; }
    }

    void save(const std::string& path) {
        nlohmann::json j;
        j["job"] = encodeForJson(job);
        j["hair"] = encodeForJson(hair);
        j["gender"] = gender;
        std::vector<uint8_t> bpal(bodyPalette.colors, bodyPalette.colors + 1024);
        std::vector<uint8_t> hpal(hairPalette.colors, hairPalette.colors + 1024);
        j["bodyPalette"] = bpal; j["hairPalette"] = hpal;
        j["billboardTilt"] = billboardTilt;
        j["walkSpeed"] = walkSpeed;
        j["moveSpeed"] = moveSpeed;
        j["planarShadowSoftness"] = planarShadowSoftness;
        j["planarShadowDilation"] = planarShadowDilation;
        j["shadowType"] = shadowType;
        j["circleShadowSoftness"] = circleShadowSoftness;
        j["circleShadowDilation"] = circleShadowDilation;
        j["billboardTiltSpline"] = billboardTiltSpline.toJson();
        std::ofstream f(path); if (f.is_open()) f << j.dump(4);
    }

    void load(const std::string& path) {
        std::ifstream f(path); if (!f.is_open()) return;
        try {
            nlohmann::json j = nlohmann::json::parse(f);
            job = decodeFromJson(j.value("job", ""));
            hair = decodeFromJson(j.value("hair", ""));
            gender = j.value("gender", 0);
            if (j.contains("bodyPalette")) {
                auto bpal = j["bodyPalette"].get<std::vector<uint8_t>>();
                if (bpal.size() == 1024) std::copy(bpal.begin(), bpal.end(), bodyPalette.colors);
            }
            billboardTilt = j.value("billboardTilt", 45.0f);
            walkSpeed = j.value("walkSpeed", 65.0f);
            moveSpeed = j.value("moveSpeed", 100.0f);
            planarShadowSoftness = j.value("planarShadowSoftness", 0.0f);
            planarShadowDilation = j.value("planarShadowDilation", 1.0f);
            shadowType = j.value("shadowType", 1);
            circleShadowSoftness = j.value("circleShadowSoftness", 0.5f);
            circleShadowDilation = j.value("circleShadowDilation", 1.0f);
            if (j.contains("billboardTiltSpline") && j["billboardTiltSpline"].is_array()) {
                billboardTiltSpline = SplineCurve::fromJson(j["billboardTiltSpline"]);
            }
            if (billboardTiltSpline.pointsX.size() < 2) {
                billboardTiltSpline = SplineCurve(
                    {45.0f, 50.0f, 70.0f},
                    {0.0f, 5.0f, 25.0f},
                    InterpolationMode::Linear
                );
            }
            if (j.contains("bodyPalette")) {
                auto hpal = j["hairPalette"].get<std::vector<uint8_t>>();
                if (hpal.size() == 1024) std::copy(hpal.begin(), hpal.end(), hairPalette.colors);
            }
        } catch(...) {}
    }
};

// SpritePickerUI is now a no-op placeholder. Legacy sprite formats are no longer
// supported; characters use PNG/ERUPTSPR files or the bundled default sprite.
class SpritePickerUI {
public:
    static void draw(bool* open, CharConfig& config, const PackManager& /*packManager*/) {
        if (!ImGui::Begin("Sprite Picker (F6)", open)) { ImGui::End(); return; }

        ImGui::Text("Sprite picker is disabled.");
        ImGui::Text("Legacy sprite formats are no longer supported.");
        ImGui::Text("Set a PNG path in char_config.json or use the default sprite.");

        if (ImGui::Button("Reset to default sprite")) {
            config.job = "assets/sprites/default.png";
            config.save("data/char_config.json");
        }

        ImGui::End();
    }
};

} // namespace eruption
