#include "debug/MipmapDebugMenu.hpp"
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <fstream>

namespace eruption {

using json = nlohmann::json;

void MipmapDebugMenu::loadConfig() {
    std::ifstream file(CONFIG_PATH);
    if (!file.is_open()) {
        ERUPTION_LOG_INFO("Mipmap config not found, using defaults");
        saveConfig();
        return;
    }
    try {
        json j;
        file >> j;
        config.enabled = j.value("enabled", true);
        config.mode = static_cast<MipGenerationMode>(j.value("generation_mode", 0));
        config.forceRegenOnSwitch = j.value("force_regen_on_switch", false);
        config.showWarningNoMipmaps = j.value("show_warning_no_mipmaps", true);
        if (static_cast<uint8_t>(config.mode) >= static_cast<uint8_t>(MipGenerationMode::COUNT)) {
            config.mode = MipGenerationMode::BLIT_LINEAR;
        }
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("Failed to parse mipmap config: %s", e.what());
    }
}

void MipmapDebugMenu::saveConfig() {
    json j;
    j["enabled"] = config.enabled;
    j["generation_mode"] = static_cast<int>(config.mode);
    j["force_regen_on_switch"] = config.forceRegenOnSwitch;
    j["show_warning_no_mipmaps"] = config.showWarningNoMipmaps;
    std::ofstream file(CONFIG_PATH);
    if (file.is_open()) {
        file << j.dump(4);
    }
}

void MipmapDebugMenu::drawUI(Engine* engine) {
    if (!ImGui::CollapsingHeader("Mipmap Settings", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Mipmap Generation");

    bool enabled = config.enabled;
    if (ImGui::Checkbox("Enable Mipmaps", &enabled)) {
        config.enabled = enabled;
        modeChanged = true;
        saveConfig();
    }

    if (!config.enabled) {
        ImGui::TextDisabled("Mipmaps disabled. All textures use single level.");
        return;
    }

    int modeIdx = static_cast<int>(config.mode);
    const char* modeNames[] = {
        "Blit Linear (Runtime)",
        "DirectXTex Offline (Cache KTX2)",
        "Toksvig PBR (Normal+Roughness)",
        "GPU Compute Shared Memory (LDS)"
    };

    if (ImGui::Combo("Generation Mode", &modeIdx, modeNames, IM_ARRAYSIZE(modeNames))) {
        auto newMode = static_cast<MipGenerationMode>(modeIdx);
        if (newMode != config.mode) {
            config.mode = newMode;
            modeChanged = true;
            saveConfig();
        }
    }

    ImGui::TextWrapped("%s", mipModeDescription(config.mode));

    if (config.mode == MipGenerationMode::TOKSVIG_PBR) {
        ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "[Not Implemented]");
        ImGui::SameLine();
        ImGui::TextDisabled("Requires PBR system with normal/roughness maps.");
    } else if (config.mode == MipGenerationMode::DIRECTXTEX_OFFLINE) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "[Fallback]");
        ImGui::SameLine();
        ImGui::TextDisabled("No KTX cache present; falls back to Blit Linear.");
    }

    ImGui::Separator();
    if (ImGui::Checkbox("Force Regen on Switch", &config.forceRegenOnSwitch)) {
        saveConfig();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?);");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("If enabled, changing mode will recreate all textures.\nOtherwise only newly loaded textures use the new mode.");
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0, 0.8f, 1.0f, 1.0f), "Estastisticas de Textura");
    if (engine) {
        auto stats = engine->getTextureStats();
        ImGui::Text("Total Texturas Carregadas: %zu", stats.totalTextures);
        ImGui::Text("  Modelos: %zu | Terreno: %zu | Sprites: %zu", stats.modelTexCount, stats.terrainTexCount, stats.spriteTexCount);
        ImGui::Text("Texturas com Mipmaps: %zu", stats.texturesWithMipmaps);
        
        double totalMB = stats.totalVRAMBytes / (1024.0 * 1024.0);
        double baseMB = stats.baseVRAMBytes / (1024.0 * 1024.0);
        ImGui::Text("Memoria VRAM Estimada: %.2f MB", totalMB);
        ImGui::Text("  Sem Mipmaps: %.2f MB (Overhead: +33%%)", baseMB);
    } else {
        ImGui::TextDisabled("Engine pointer not set, statistics unavailable.");
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Validacao de Shimmering");
    if (engine) {
        bool measure = engine->shimmerMeasureEnabled();
        if (ImGui::Checkbox("Medir Metrica de Shimmering", &measure)) {
            engine->shimmerMeasureEnabled() = measure;
        }
        
        if (measure) {
            ImGui::Text("Flicker/Shimmering Atual: %.3f", engine->getShimmerCurrentValue());
            const auto& history = engine->getShimmerHistory();
            if (!history.empty()) {
                ImGui::PlotLines("Shimmering Over Time", history.data(), static_cast<int>(history.size()), 0, nullptr, 0.0f, 25.0f, ImVec2(0, 80));
            }
        }
    } else {
        ImGui::TextDisabled("Engine pointer not set, validation unavailable.");
    }
}

} // namespace eruption
