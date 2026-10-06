// Painel de debug do Engine (F1-F12, atalhos, HUD do minimapa, abas de clima
// e iluminacao). Saiu do Engine.cpp em 2026-09-04: o arquivo tinha 10.443
// linhas e SO' este bloco eram ~2.220 delas. E' a mesma classe Engine - o
// unico que muda e' em qual unidade de traducao os corpos moram, entao o
// acesso a membro privado continua valendo via Engine.hpp.
//
// Nada de logica mudou nessa passagem: e' recorta-e-cola verificado por
// screenshot deterministico (RMS 0.0 contra o binario anterior).

#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "renderer/PostFormat.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "core/Input.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TelemetryExporter.hpp"
#include "utils/EtexLoader.hpp"
#include "utils/MeshSubdivide.hpp"
#include "utils/ImGuiEx.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "utils/PbrTextureLoader.hpp"
#include <stb_image.h>
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/skymap/SkySystem.hpp"
#include "renderer/skymap/SkyConfig.hpp"
#include "renderer/WeatherTypes.hpp"
#include <cfloat>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numeric>
#include "renderer/SplineEditorUI.hpp"
#include "renderer/SpritePickerUI.hpp"
#include "formats/MapLoader.hpp"
#include "formats/ModelDataConverter.hpp"
#include "formats/SpriteTypes.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <filesystem>
#include <cctype>

#include "utils/ImageUtils.hpp"
#include "utils/TextureCache.hpp"
#include <vk_mem_alloc.h>

namespace eruption {

// Separador de milhar no painel de estatisticas (1.234.567). Veio junto do
// bloco: era `static` no Engine.cpp e so' o painel usava.
static std::string formatThousands(uint64_t v) {
    std::string d = std::to_string(v), out;
    int c = 0;
    for (int i = static_cast<int>(d.size()) - 1; i >= 0; --i) {
        out.push_back(d[i]);
        if (++c % 3 == 0 && i > 0) out.push_back('.');
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void Engine::drawWeatherTab() {
    auto& editor = m_weatherSystem.editor();
    auto syncEditor = [&]() {
        m_weatherSystem.target() = editor;
        m_weatherSystem.snapToTarget();
        // Sync weather-editor cloud tuning into the active CloudLayerRenderer
        // so these sliders have immediate visual effect. Manual edits override
        // any automatic cloud-amount transition from weather-type changes.
        m_cloudAmountInTransition = false;
        auto cfg = m_cloudLayerRenderer.config();
        cfg.cloudAmount = editor.cloudCoverage;
        cfg.cloudThickness = 400.0f + editor.cloudThickness * 1200.0f;
        cfg.windSpeed = editor.cloudSpeed * 10.0f;
        m_cloudLayerRenderer.setConfig(cfg);
        m_cloudLayerRenderer.coverageArray().setCoverage(cfg.cloudAmount);
        m_cloudLayerRenderer.coverageArray().setLayerSpacing(cfg.cloudThickness / glm::max(1u, cfg.layerCount - 1));
    };

    auto applyWeatherType = [&](WeatherType type) {
        applyWeatherTypeFull(type, 1.0f);
    };

    auto isDay = [&]() -> bool {
        float tod = m_dayNightCycle.timeOfDay();
        return tod >= 0.20f && tod <= 0.80f && m_dayNightCycle.getSunIntensity() > 0.1f;
    };

    // --- Categories (8 buttons) ---
    ImGui::Text("Weather Categories");
    WeatherCategory currentCategory = weatherTypeCategory(m_weatherSystem.currentType());
    for (int i = 0; i < static_cast<int>(WeatherCategory::Count); ++i) {
        auto cat = static_cast<WeatherCategory>(i);
        bool active = (currentCategory == cat);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button(weatherCategoryName(cat), ImVec2(90, 0))) {
            // Pick the first valid type in this category (respecting day/night).
            bool day = isDay();
            for (uint32_t t = 0; t < WeatherTypeCount; ++t) {
                WeatherType wt = static_cast<WeatherType>(t);
                if (weatherTypeCategory(wt) != cat) continue;
                if (day && isWeatherTypeNightOnly(wt)) continue;
                if (!day && isWeatherTypeDayOnly(wt)) continue;
                applyWeatherType(wt);
                break;
            }
        }
        if (active) ImGui::PopStyleColor();
        if (i % 4 != 3) ImGui::SameLine();
    }

    // --- Weather Type combo for the selected category ---
    WeatherCategory selectedCategory = weatherTypeCategory(editor.weatherType);
    std::vector<std::string> typeNameStorage;
    std::vector<WeatherType> typeValues;
    typeNameStorage.reserve(WeatherTypeCount);
    typeValues.reserve(WeatherTypeCount);
    bool day = isDay();

    auto addType = [&](WeatherType wt, bool forceInclude) {
        const auto& info = getWeatherTypeInfo(wt);
        bool allowed = true;
        if (day && isWeatherTypeNightOnly(wt)) allowed = false;
        if (!day && isWeatherTypeDayOnly(wt)) allowed = false;
        if (!allowed && !forceInclude) return;

        std::string label = info.displayName;
        if (!allowed) {
            label += day ? " (night only)" : " (day only)";
        }
        typeNameStorage.push_back(std::move(label));
        typeValues.push_back(wt);
    };

    // Ensure the currently active type is always visible in the combo.
    addType(editor.weatherType, true);
    for (uint32_t t = 0; t < WeatherTypeCount; ++t) {
        WeatherType wt = static_cast<WeatherType>(t);
        if (weatherTypeCategory(wt) != selectedCategory) continue;
        if (wt == editor.weatherType) continue;
        addType(wt, false);
    }

    // Build const char* list only after all strings are stored, so c_str()
    // pointers remain stable while ImGui renders the combo.
    std::vector<const char*> typeNames;
    typeNames.reserve(typeNameStorage.size());
    for (const auto& s : typeNameStorage) typeNames.push_back(s.c_str());

    int typeIdx = 0;
    for (size_t i = 0; i < typeValues.size(); ++i) {
        if (typeValues[i] == editor.weatherType) { typeIdx = static_cast<int>(i); break; }
    }
    if (ImGui::Combo("Weather Type", &typeIdx, typeNames.data(), static_cast<int>(typeNames.size()))) {
        applyWeatherType(typeValues[typeIdx]);
    }

    // --- Global intensity ---
    if (ImGui::SliderFloat("Weather Intensity", &editor.weatherIntensity, 0.0f, 1.0f)) {
        // Preserve manually-tweaked cloud parameters while re-applying the weather template.
        float savedCloudCoverage = editor.cloudCoverage;
        float savedCloudThickness = editor.cloudThickness;
        float savedCloudSpeed = editor.cloudSpeed;
        m_weatherSystem.applyType(editor.weatherType, editor.weatherIntensity);
        editor.cloudCoverage = savedCloudCoverage;
        editor.cloudThickness = savedCloudThickness;
        editor.cloudSpeed = savedCloudSpeed;
        m_weatherSystem.target() = editor;
        m_weatherSystem.snapToTarget();
    }

    float duration = m_weatherSystem.transitionDuration();
    if (ImGui::SliderFloat("Transition Duration", &duration, 0.1f, 10.0f)) {
        m_weatherSystem.setTransitionDuration(duration);
    }
    if (m_weatherSystem.inTransition()) {
        ImGui::ProgressBar(m_weatherSystem.transitionProgress(), ImVec2(-1, 0), "transitioning");
    }

    ImGui::Separator();
    const char* tiers[] = {"Mobile (screen-space)", "Medium (GPU particles)", "High (SOTA)"};
    int tierIdx = static_cast<int>(m_postSettings.weatherTier);
    if (ImGui::Combo("Weather Tier", &tierIdx, tiers, IM_ARRAYSIZE(tiers))) {
        m_postSettings.weatherTier = static_cast<WeatherTier>(tierIdx);
    }
    ImGui::Checkbox("Affect Water", &m_weatherAffectsWater);
    ImGui::SetItemTooltip("Rain/storm agitates water normals and waves.");
    ImGui::Text("Sun occlusion: %.2f  Moon occlusion: %.2f", m_weatherSystem.sunOcclusion(), m_weatherSystem.moonOcclusion());
    ImGui::Text("Effective rain: %.2f  snow: %.2f  heat: %.2f", m_weatherSystem.effectiveRainIntensity(), m_weatherSystem.effectiveSnowIntensity(), m_weatherSystem.effectiveHeatShimmer());

    ImGui::Separator();
    ImGui::Text("Rain");
    if (ImGui::SliderFloat("Rain Intensity", &editor.rainIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Rain Speed", &editor.rainSpeed, 0.1f, 3.0f)) syncEditor();
    if (ImGui::SliderFloat("Rain Wind", &editor.rainWind, -1.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Rain Turbulence", &editor.rainTurbulence, 0.0f, 1.0f)) syncEditor();
    if (ImGui::TreeNode("Drop Shape (anti-shimmer)")) {
        if (ImGui::SliderFloat("Drop Width", &editor.rainStreakWidth, 0.02f, 0.50f)) syncEditor();
        ImGui::SetItemTooltip("Streak width in world units (Rainy default 0.15).");
        if (ImGui::SliderFloat("Drop Length", &editor.rainStreakLength, 1.0f, 12.0f)) syncEditor();
        ImGui::SetItemTooltip("Streak length in world units (Rainy default 6.0).");
        if (ImGui::SliderFloat("Drop Edge Sharpness", &editor.rainStreakEdge, 1.0f, 10.0f)) syncEditor();
        ImGui::SetItemTooltip("Lateral falloff of the streak; higher = thinner core (Rainy default 5.0).");
        if (ImGui::SliderFloat("Drop Tip Fade", &editor.rainStreakTipFade, 0.5f, 4.0f)) syncEditor();
        ImGui::SetItemTooltip("Fade along the streak tips (Rainy default 1.3).");
        if (ImGui::SliderFloat("Drop Alpha Gain", &editor.rainStreakAlpha, 0.5f, 6.0f)) syncEditor();
        ImGui::SetItemTooltip("Alpha multiplier of the streak (default 2.0).");
        if (ImGui::SliderFloat("Drop Brightness", &editor.rainStreakBrightness, 0.5f, 5.0f)) syncEditor();
        ImGui::SetItemTooltip("Color multiplier of the streak (legacy default 2.5).");
        if (ImGui::SliderFloat("Drop Pixel Fade (px)", &editor.rainPixelFade, 0.0f, 4.0f)) syncEditor();
        ImGui::SetItemTooltip("Anti-shimmer: drops projected thinner than this many pixels fade out instead of\n"
                              "blowing up into fat bright blobs. 0 = off. Try 1.5-2.5.");
        ImGui::TreePop();
    }
    if (ImGui::Checkbox("Enable Rain Splash", &editor.rainSplashEnabled)) syncEditor();
    ImGui::SetItemTooltip("Contact splash on wet surfaces; scales automatically with rain intensity.");
    if (ImGui::SliderFloat("Rain Splash Amount", &editor.rainSplashAmount, 0.0f, 1.0f)) syncEditor();
    ImGui::SetItemTooltip("Global multiplier for the number of ground splash particles.");
    if (ImGui::SliderFloat("Rain Splash Opacity", &editor.rainSplashOpacity, 0.0f, 1.0f)) syncEditor();
    ImGui::SetItemTooltip("Transparency of each splash ripple.");
    if (ImGui::SliderFloat("Rain Splash Radius", &editor.rainSplashRadius, 0.25f, 4.0f)) syncEditor();
    ImGui::Checkbox("Debug Splash Area", &m_postSettings.debugSplashArea);
    ImGui::SetItemTooltip("Paints every visible rain splash as a pure RGB(255,0,0) quad, through the exact same occlusion gates as normal rendering - red on the ground = splashes really appear there. Works on any tier.");
    if (ImGui::SliderFloat("Rain Shadow Blur", &editor.heightmapBlur, 0.0f, 1.0f)) syncEditor();
    ImGui::SetItemTooltip("Smooths the rain shadow heightmap; white = more splash, black = none.");
    if (ImGui::SliderFloat("Lens Drops", &editor.dropLensAmount, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Lens Drop Radius", &editor.lensDropRadius, 0.25f, 4.0f)) syncEditor();

    const char* dropModes[] = {"Procedural", "CPU Buffer", "GPU Compute"};
    int dropModeIdx = static_cast<int>(editor.lensDropMode);
    if (ImGui::Combo("Lens Drop Mode", &dropModeIdx, dropModes, IM_ARRAYSIZE(dropModes))) {
        editor.lensDropMode = static_cast<LensDropMode>(dropModeIdx);
        syncEditor();
    }
    if (editor.lensDropMode != LensDropMode::Procedural) {
        if (ImGui::SliderInt("Drop Count", &editor.lensDropCount, 64, 2048)) syncEditor();
    }

    ImGui::Separator();
    ImGui::Text("Snow");
    if (ImGui::SliderFloat("Snow Intensity", &editor.snowIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Snow Speed", &editor.snowSpeed, 0.1f, 2.0f)) syncEditor();
    if (ImGui::SliderFloat("Snow Size", &editor.snowSize, 0.5f, 3.0f)) syncEditor();
    if (ImGui::SliderFloat("Snow Wind", &editor.snowWind, -1.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Snow Turbulence", &editor.snowTurbulence, 0.0f, 1.0f)) syncEditor();

    ImGui::Separator();
    ImGui::Text("Heat");
    if (ImGui::SliderFloat("Heat Shimmer", &editor.heatShimmer, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Heat Speed", &editor.heatSpeed, 0.1f, 3.0f)) syncEditor();
    if (ImGui::SliderFloat("Heat Scale", &editor.heatScale, 0.5f, 3.0f)) syncEditor();
    if (ImGui::Checkbox("Auto Ground Height", &editor.heatWorldHeightAuto)) syncEditor();
    ImGui::SetItemTooltip("Sets heat shimmer origin to the terrain height under the player.");
    if (!editor.heatWorldHeightAuto) {
        if (ImGui::SliderFloat("Heat Ground Height", &editor.heatWorldHeight, -50.0f, 50.0f)) syncEditor();
    } else {
        if (ImGui::SliderFloat("Heat Ground Offset", &editor.heatWorldHeightOffset, -10.0f, 10.0f)) syncEditor();
    }

    ImGui::Separator();
    ImGui::Text("Fog & Storm");
    if (ImGui::SliderFloat("Fog Density", &editor.fogDensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Fog Start", &editor.fogStart, 1.0f, 500.0f)) syncEditor();
    if (ImGui::SliderFloat("Fog End", &editor.fogEnd, 50.0f, 3000.0f)) syncEditor();
    if (ImGui::SliderFloat("Fog Height", &editor.fogHeight, -100.0f, 100.0f)) syncEditor();
    if (ImGui::SliderFloat("Fog Height Falloff", &editor.fogHeightFalloff, 0.0f, 0.1f)) syncEditor();
    if (ImGui::SliderFloat("Storm Tint", &editor.stormTint, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Lightning Chance", &editor.lightningChance, 0.0f, 1.0f)) syncEditor();

    ImGui::Separator();
    ImGui::Text("Clouds");
    if (ImGui::SliderFloat("Cloud Speed", &editor.cloudSpeed, 0.0f, 0.5f)) syncEditor();

    ImGui::Separator();
    ImGui::Text("Atmosphere");
    if (ImGui::SliderFloat("Wind Debris", &editor.windDebrisIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Atmospheric Tint", &editor.atmosphericTint, 0.0f, 1.0f)) syncEditor();

    ImGui::Separator();
    ImGui::Text("Overlay Effects");
    if (ImGui::SliderFloat("Rainbow Intensity", &editor.rainbowIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Mirage Intensity", &editor.mirageIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Shooting Star Intensity", &editor.shootingStarIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Tornado Intensity", &editor.tornadoIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Dust Devil Intensity", &editor.dustDevilIntensity, 0.0f, 1.0f)) syncEditor();
    if (ImGui::SliderFloat("Aurora Intensity", &editor.auroraIntensity, 0.0f, 1.0f)) syncEditor();

    auto& skyCfg = m_skybox.editableConfig();
    if (ImGui::Button("Save Weather to Skybox Config")) {
        skyCfg.procedural.weather = editor;
        skyCfg.procedural.weatherType = editor.weatherType;
    }
}

// Single source of truth for the Lighting controls. Drawn in BOTH the
// F2 (Iterative Effects > Lighting) and F9 (A/B Test > Lighting) tabs so the
// same sliders appear and edit the same members wherever the user looks.
void Engine::updateNightLights() {
    float hour = m_dayNightCycle.timeOfDay() * 24.0f;
    bool isNight = (hour >= 18.0f) || (hour < 5.0f);
    for (auto& pl : m_deferredLighting.getPointLights()) {
        if (pl.nightOnly) pl.enabled = isNight;
    }
}

void Engine::drawLightingControls() {
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "Lighting Configuration");
    ImGui::Separator();
    m_renderEffects[1]->drawUI(); // Global Illumination
    ImGui::SliderFloat("Ambient Intensity", &m_ambientIntensity, 0.0f, 2.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flat hemispheric fill. Constant across the day/night cycle (the old per-time spline was removed; a moonlight fill handles night).");
    ImGui::SliderFloat("Ambient Hemi Floor", &m_ambientHemiFloor, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum hemispheric factor for down-facing surfaces. Higher = flatter ambient; lower = more sky/ground contrast (helps normal-map relief at night).");

    ImGui::Checkbox("SSAO (screen-space AO)", &m_ssaoEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dedicated half-res SSAO pass with bilateral blur. Grounds props, darkens corners/crevices/contact lines.");
    if (m_ssaoEnabled) {
        ImGui::SliderFloat("  AO Strength", &m_ssaoStrength, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("  AO Radius (m)", &m_ssaoRadius, 0.2f, 5.0f, "%.2f");
    }

    ImGui::Checkbox("Environment Specular (IBL)", &m_envSpecEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split-sum environment specular from the sky, roughness-gated. Gives glossy/wet/metal surfaces an environment highlight. Requires PBR.");
    if (m_envSpecEnabled) {
        ImGui::SliderFloat("  Env Spec Intensity", &m_envSpecIntensity, 0.0f, 3.0f, "%.2f");
    }

    ImGui::Checkbox("Sun Bounce (1-bounce GI)", &m_sunBounceEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sunlight bounced off the ground back onto walls/undersides, tinted by this map's real average ground color (computed at load). Occluded by AO.");
    if (m_sunBounceEnabled) {
        ImGui::SliderFloat("  Bounce Strength", &m_sunBounceStrength, 0.0f, 2.0f, "%.2f");
    }

    ImGui::Checkbox("Contact Shadows (screen-space)", &m_contactShadowEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Short screen-space ray march toward the sun: small props and ledges cast crisp near-field shadows the shadow map misses.");
    if (m_contactShadowEnabled) {
        ImGui::SliderFloat("  Contact Length (m)", &m_contactShadowLength, 0.1f, 4.0f, "%.2f");
    }

    ImGui::SliderFloat("Night AO Contrast", &m_nightAoContrast, 1.0f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("AO exponent when the sun is down. 1.0 = off. Higher darkens crevices harder at night so normal-map relief still reads with only ambient/moon light.");

    ImGui::Checkbox("Night Moonlight Fill", &m_moonFillEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the sun sets, drive the directional light with the moon (dir/color/intensity from the Moon Intensity curve). Keeps bump/relief and soft shadows alive at night.");
    if (m_moonFillEnabled) {
        ImGui::SliderFloat("  Moonlight Strength", &m_moonFillStrength, 0.0f, 4.0f, "%.2f");
    }

    ImGui::Checkbox("Auto Exposure (eye adaptation)", &m_autoExposureEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Histogram-based auto exposure measured from the rendered frame, eased over time like the eye adjusting. Falls back to the analytic estimate when the histogram is unavailable.");
    if (m_autoExposureEnabled) {
        ImGui::SliderFloat("  Exposure Key", &m_autoExposureKey, 0.3f, 3.0f, "%.2f");
        ImGui::SliderFloat("  Adaptation Speed", &m_autoExposureSpeed, 0.2f, 8.0f, "%.2f");
        ImGui::Text("  current x%.2f", m_autoExposureCurrent);
    }

    m_renderEffects[4]->drawUI(); // Point Lights

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "PBR Material System");
    bool usePbr = m_deferredLighting.usePbr();
    if (ImGui::Checkbox("Use PBR (Cook-Torrance/GGX)", &usePbr)) {
        m_deferredLighting.setUsePbr(usePbr);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uncheck to switch back to the legacy Blinn-Phong path for A/B comparison.");

    uint32_t dbg = m_deferredLighting.pbrDebugMode();
    bool dR = (dbg & 1u) != 0, dM = (dbg & 2u) != 0, dN = (dbg & 4u) != 0,
         dW = (dbg & 8u) != 0, dS = (dbg & 16u) != 0;
    if (ImGui::Checkbox("Show Roughness (red)", &dR)) dbg ^= 1u;
    ImGui::SameLine();
    if (ImGui::Checkbox("Show Metallic (green)", &dM)) dbg ^= 2u;
    if (ImGui::Checkbox("Show Normal (RGB)", &dN)) dbg ^= 4u;
    ImGui::SameLine();
    if (ImGui::Checkbox("Show Wetness (cyan)", &dW)) dbg ^= 8u;
    if (ImGui::Checkbox("Show Source (magenta)", &dS)) dbg ^= 16u;
    ImGui::SameLine();
    bool dAO = (dbg & 64u) != 0;
    if (ImGui::Checkbox("Show AO (cinza)", &dAO)) dbg ^= 64u;
    m_deferredLighting.setPbrDebugMode(dbg);
    // O composite precisa saber para pular fog/tonemap/bloom: sem isso a
    // visualizacao ainda mudaria com a hora do dia (o fog tinge o longe com a
    // cor do ceu). Ver post_composite.frag.
    m_postSettings.pbrDebugActive = (dbg & 95u) != 0u;

    float pbrScale = m_deferredLighting.pbrLightScale();
    if (ImGui::SliderFloat("PBR Light Scale", &pbrScale, 0.0f, 10.0f, "%.2f")) {
        m_deferredLighting.setPbrLightScale(pbrScale);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Boosts PBR direct-light response so microfacet specular is visible with legacy light intensities.");

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Bump / Normal");
    ImGui::SliderFloat("Normal Map Scale", &m_normalMapScale, 0.0f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Strength of the normal-map detail (0 = flat, 1 = full, >1 = exaggerated).");
    ImGui::SliderFloat("Normal Smoothing", &m_normalSmoothing, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Max normal slope (deg)", &m_normalMaxSlopeDeg, 0.0f, 89.0f, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Caps how far the normal map may tilt the surface. 0 = no cap; past ~70 deg the normal field saturates and the surface reads as crumpled foil instead of relief.");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smooths out JPG artifacts by forcing lower MipMap LODs.");
    // CURVAS POR DISTANCIA DA CAMERA. Mesmo widget de spline do DoF e das
    // sombras (SplineEditorUI): o autor arrasta os pontos, o X e' mostrado em
    // unidades de mundo e o marcador vermelho segue a distancia ATUAL da
    // camera ao alvo, entao da' pra afinar olhando a cena.
    ImGui::Checkbox("Distance curves (relief)", &m_normalDistCurve);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Normal-map strength and mip bias driven by distance to the camera.");
    if (m_normalDistCurve) {
        const float camDist = m_camera.orbitDistance();
        ImGui::DragFloat("Curve max distance (u)", &m_normalDistMax, 5.0f, 50.0f, 20000.0f, "%.0f");
        SplineEditorUI::draw("Normal Map Scale by Distance", m_normalScaleCurve,
                             0.0f, m_normalDistMax, 0.0f, 3.0f, camDist,
                             m_normalScaleCurve.evaluate(camDist / std::max(m_normalDistMax, 1.0f)),
                             false, "Distance (u)");
        SplineEditorUI::draw("Normal Mip Bias by Distance", m_normalSmoothCurve,
                             0.0f, m_normalDistMax, 0.0f, 5.0f, camDist,
                             m_normalSmoothCurve.evaluate(camDist / std::max(m_normalDistMax, 1.0f)),
                             false, "Distance (u)");
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Tessellation (near band)");
    // TESSELACAO DE HARDWARE NO CHAO. O fator sai da mesma familia de curvas:
    // 8 colado na camera, caindo a 1 no fim da banda. Onde vale 1, o patch e'
    // identico ao triangulo original - a transicao com a malha normal e' a
    // propria curva. So' o chao entra, e so' se o driver tiver o estagio.
    ImGui::Checkbox("Tessellate ground up close", &m_tessEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hardware tessellation + height displacement on ground cells near the camera. Zero extra VRAM.");
    if (m_tessEnabled) {
        const float camDist = m_camera.orbitDistance();
        ImGui::SliderFloat("Displacement (u)", &m_tessAmplitude, 0.0f, 6.0f, "%.2f");
        ImGui::SliderFloat("Height contrast", &m_tessHeightGain, 0.1f, 8.0f, "%.2f");
        // TRADE, nao preferencia: um dos dois sempre custa alguma coisa.
        ImGui::Combo("Height from", &m_tessHeightSpace, "Mesh UV (matches texture)\0World (no seams)\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("UV: relief lines up with the texture you see, but opens gaps where UVs break (panel joints).\nWorld: joints always closed, but the relief pattern no longer matches the visible texture.");
        if (m_tessHeightSpace == 1)
            ImGui::SliderFloat("World tile (1/u)", &m_tessWorldScale, 0.01f, 1.0f, "%.3f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gain on the high-passed height. The baked height comes from albedo luminance, so the low band is painted shading - this only amplifies the grain.");
        SplineEditorUI::draw("Tessellation Factor by Distance", m_tessCurve,
                             0.0f, m_normalDistMax, 1.0f, 16.0f, camDist,
                             m_tessCurve.evaluate(camDist / std::max(m_normalDistMax, 1.0f)),
                             false, "Distance (u)");
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Shadow opacity");
    // OPACIDADE DA SOMBRA POR DISTANCIA (pedido do autor 2026-09-06: "as
    // sombras tao muito pretas, deixa uma opacidade ai"). Multiplica o fator
    // de sombra do sol antes de escurecer o pixel: 1 = como era, 0 = sem sombra.
    ImGui::Checkbox("Distance curve (shadow)", &m_shadowOpacityCurve);
    if (m_shadowOpacityCurve) {
        const float camDist = m_camera.orbitDistance();
        SplineEditorUI::draw("Sun Shadow Opacity by Distance", m_shadowOpacitySpline,
                             0.0f, m_normalDistMax, 0.0f, 1.0f, camDist,
                             m_shadowOpacitySpline.evaluate(camDist / std::max(m_normalDistMax, 1.0f)),
                             false, "Distance (u)");
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Flip Normal Map Green Channel", &m_normalMapInvertY)) {}
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip the green channel for normal maps authored with Y pointing down.");
    ImGui::SliderFloat("Fallback Roughness", &m_defaultRoughness, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Fallback Metallic", &m_defaultMetallic, 0.0f, 1.0f, "%.2f");

    ImGui::Text("Legacy = diffuse + Blinn-Phong specular");
    ImGui::Text("PBR    = GGX microfacet with material maps");
    ImGui::Text("Fallback (no maps) matches legacy pixel-perfectly.");
}

void Engine::renderImGui() {
    ERUPTION_LOG_INFO("[Engine] renderImGui playerController=%p", (void*)m_playerController);
    LightingEnvironment env;
    env.sun.direction = m_dayNightCycle.getSunDirection(); env.sun.color = m_dayNightCycle.getSunColor();
    env.sun.intensity = m_dayNightCycle.getSunIntensity(); env.ambientColor = m_dayNightCycle.getAmbientColor();
    env.ambientIntensity = m_ambientIntensity; env.ambientSkyColor = m_dayNightCycle.getSkyTopColor();
    env.ambientGroundColor = m_mapGroundAlbedo;
    env.skyHorizonColor = m_dayNightCycle.getSkyHorizonColor();
    // Cloud shadows: base strength = sun/(sun+ambient), so a dense cloud
    // darkens the ground like map geometry shadows (author feedback 2026-08-10).
    m_cloudLayerRenderer.setSceneAmbientIntensity(env.ambientIntensity);

    float zoomPercent = 1.0f - (m_camera.orbitDistance() - 10.0f) / (1000.0f - 10.0f);

    auto drawStrokeText = [](ImDrawList* dl, ImVec2 pos, const char* text, ImU32 color) {
        dl->AddText(ImVec2(pos.x - 1, pos.y - 1), IM_COL32(0, 0, 0, 255), text);
        dl->AddText(ImVec2(pos.x + 1, pos.y - 1), IM_COL32(0, 0, 0, 255), text);
        dl->AddText(ImVec2(pos.x - 1, pos.y + 1), IM_COL32(0, 0, 0, 255), text);
        dl->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 255), text);
        dl->AddText(pos, color, text);
    };

    if (m_showDebugOverlay) {
        m_modelRenderer.setHighlightedInstance(-1);
        {
            // Demo de video (m_demoHidePanels): painel fora da tela, sem mexer na estrutura.
            ImGui::SetNextWindowPos(m_demoHidePanels ? ImVec2(-10000.0f, -10000.0f) : ImVec2(10, 10), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.45f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
            ImGuiWindowFlags swFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar;

            // Compute minimum width so descriptions aren't clipped
            float minW = ImGui::CalcTextSize("Shortcuts").x + 30.0f;
            const char* keys[]  = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F9", "F12", "Enter", "Space"};
            const char* descs[] = {"Object Manager", "Effects / Post-Process", "Stats / Telemetry", "Toggle Debug Overlay", "Reload Configs", "Sprite Picker", "Tactical View", "A/B Test Menu", "Resource Manager", "Console", "Free Camera"};
            for (int i = 0; i < IM_ARRAYSIZE(keys); ++i) {
                float w = ImGui::CalcTextSize(keys[i]).x + 8.0f + 14.0f + ImGui::CalcTextSize(descs[i]).x;
                if (w > minW) minW = w;
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(minW + 16.0f, 0), ImVec2(FLT_MAX, FLT_MAX));

            if (ImGui::Begin("Shortcuts", nullptr, swFlags)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImU32 white = IM_COL32(255, 255, 255, 220);

                // Header toggle
                const char* arrow = m_shortcutsExpanded ? "v" : ">";
                if (ImGui::Button(arrow)) m_shortcutsExpanded = !m_shortcutsExpanded;
                ImGui::SameLine();
                ImGui::TextUnformatted("Shortcuts");

                if (m_shortcutsExpanded) {
                    auto shortcut = [&](const char* key, const char* desc) {
                        ImVec2 keySize = ImGui::CalcTextSize(key);
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        // key badge
                        dl->AddRectFilled(pos, ImVec2(pos.x + keySize.x + 8, pos.y + keySize.y + 4), IM_COL32(30, 41, 59, 220), 3.0f);
                        drawStrokeText(dl, ImVec2(pos.x + 4, pos.y + 2), key, IM_COL32(250, 204, 21, 255));
                        // description
                        drawStrokeText(dl, ImVec2(pos.x + keySize.x + 14, pos.y + 2), desc, white);
                        ImGui::Dummy(ImVec2(0, keySize.y + 6));
                    };

                    for (int i = 0; i < IM_ARRAYSIZE(keys); ++i) shortcut(keys[i], descs[i]);
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (m_showConsole) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 40), ImGuiCond_Always, ImVec2(0.5f, 0));
            ImGui::SetNextWindowSize(ImVec2(600, 0));
            if (ImGui::Begin("Console", &m_showConsole, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "COMMAND PALETTE");
                ImGui::BulletText("'warp <mapname>' - Warp to map");
                ImGui::BulletText("'exit' / 'quit'   - Exit application");
                if (m_consoleInput[0] == '\0') { m_consoleInput[0] = '@'; m_consoleInput[1] = '\0'; }
                ImGui::Text("CMD>"); ImGui::SameLine(); ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##console_input", m_consoleInput, sizeof(m_consoleInput), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    std::string cmdStr(m_consoleInput); if (cmdStr.find("@") == 0) cmdStr = cmdStr.substr(1);
                    if (cmdStr == "exit" || cmdStr == "quit") m_running = false;
                    else if (cmdStr.find("warp ") == 0) m_pendingMapWarp = cmdStr.substr(5);
                    m_consoleInput[0] = '\0'; m_showConsole = false;
                }
            } ImGui::End();
        }

        if (m_showEffectsMenu) {
            ImGui::SetNextWindowSize(ImVec2(600, 800), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(0.85f);
            if (ImGui::Begin("Iterative Effects (F2)", &m_showEffectsMenu)) {
                if (ImGui::BeginTabBar("F2Tabs")) {
                    if (ImGui::BeginTabItem("Effects")) {
                        auto& pp = m_postSettings;
                        // Keep render-effect toggles synced with systems
                        m_shadowRenderer.settings().enabled = m_renderEffects[5]->isEnabled();
                        pp.enableDoF = m_renderEffects[3]->isEnabled();

                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "All Render Effects");
                        ImGui::Separator();
                        for (auto& effect : m_renderEffects) {
                            effect->drawUI();
                        }
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Post-Process & Effects Configuration");
                        ImGui::Separator();

                        if (pp.enableDoF) {
                            // ---- BLOOM ----
                            // O bloom (Kawase dual-filter) sempre existiu e sempre
                            // rodou, mas nao tinha NENHUM controle exposto e o
                            // limiar padrao era 1.0 - numa cena majoritariamente
                            // LDR quase nada ultrapassa isso, entao o efeito era
                            // invisivel e parecia ausente.
                            // ---- SSAO ----
                            // O raio e' em unidades de MUNDO. O default historico
                            // (1.6, comentado como "metros") nao produzia oclusao
                            // de contato nenhuma numa cena cuja celula de terreno
                            // tem 10 unidades e cuja arvore tem 40 - por isso os
                            // props pareciam adesivo colado no chao.
                            // ---- TONE MAPPING ----
                            // A engine nao tinha operador nenhum: terminava em
                            // pow(color,1/2.2) cru. Medido em parana_field, 1,28%
                            // dos pixels CORTAVAM por canal - o que desloca matiz
                            // e da' a cara de CG. AgX zera esse corte (0,00%).
                            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Tone Mapping");
                            {
                                const char* tmNames[] = { "Legado (so gamma)", "AgX", "ACES", "Khronos Neutral" };
                                ImGui::Combo("Operador", &pp.toneMapMode, tmNames, IM_ARRAYSIZE(tmNames));
                                ImGui::SetItemTooltip("AgX: preserva matiz dessaturando a alta luz (Blender 4.x).\n"
                                                      "ACES: contraste cinematografico, satura mais.\n"
                                                      "Neutral: albedo exato ate' o joelho (Khronos).\n"
                                                      "Legado: corta por canal - foi o padrao historico.");
                                ImGui::SliderFloat("Exposure", &pp.exposure, 0.1f, 4.0f, "%.2f");
                            }
                            ImGui::Separator();

                            ImGui::TextColored(ImVec4(0, 1, 1, 1), "SSAO (oclusao de contato)");
                            ImGui::Checkbox("Enable SSAO", &m_ssaoEnabled);
                            if (m_ssaoEnabled) {
                                ImGui::SliderFloat("SSAO Radius", &m_ssaoRadius, 0.5f, 60.0f, "%.1f u");
                                ImGui::SetItemTooltip("Raio em unidades de MUNDO, nao metros.\n"
                                                      "Celula de terreno = 10u, arvore = 40u.\n"
                                                      "Abaixo de ~5 nao ha' contato visivel.");
                                ImGui::SliderFloat("SSAO Strength", &m_ssaoStrength, 0.0f, 3.0f, "%.2f");
                            }
                            ImGui::Separator();

                            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Bloom");
                            // DUAS caixas mandavam no mesmo bloom e uma engolia
                            // a outra (relatado pelo autor 2026-09-04). Esta
                            // aqui escrevia direto em pp.enableBloom, mas o
                            // render() reatribui esse campo TODO FRAME a partir
                            // de m_renderEffects[2] (ver o bloco "A caixa
                            // 'Bloom' do menu de efeitos era ESCRITA e NUNCA
                            // LIDA"). Resultado: marcar aqui valia um frame e
                            // voltava sozinho, sem ninguem entender por que.
                            // Agora as duas caixas escrevem no MESMO dono - o
                            // RenderEffect - entao concordam sempre.
                            bool bloomOn = (m_renderEffects.size() > 2)
                                         ? m_renderEffects[2]->isEnabled()
                                         : pp.enableBloom;
                            if (ImGui::Checkbox("Enable Bloom", &bloomOn)) {
                                if (m_renderEffects.size() > 2) m_renderEffects[2]->setEnabled(bloomOn);
                                pp.enableBloom = bloomOn;
                            }
                            if (bloomOn) {
                                ImGui::SliderFloat("Bloom Threshold", &pp.bloomThreshold, 0.0f, 3.0f, "%.2f");
                                ImGui::SetItemTooltip("Luminancia a partir da qual o pixel floresce.\n"
                                                      "1.0 = so' o que estoura o branco (quase nada).\n"
                                                      "0.6-0.8 da' halo visivel em lava/emissivo.");
                                ImGui::SliderFloat("Bloom Intensity", &pp.bloomIntensity, 0.0f, 3.0f, "%.2f");
                                ImGui::SetItemTooltip("Quanto do halo e' somado de volta na imagem.");
                            }
                            ImGui::Separator();

                            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Depth of Field Settings");
                            ImGui::Checkbox("Debug Mode", &pp.visualizeCoC);
                            // Os dois RadioButton de modo ("Depth + Foreground"
                            // x "Circle of Confusion") sairam junto com o ramo
                            // legado: existe UM caminho de desfoque agora.
                            if (ImGui::CollapsingHeader("Circle of Confusion Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                                ImGui::Checkbox("Enable Adaptive Focal", &pp.cocEnableAdaptiveFocal);
                                if (pp.cocEnableAdaptiveFocal) {
                                    if (ImGui::CollapsingHeader("Adaptive Focal Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                                        const char* methodItems[] = {"Single Ray", "Multi-Ray Grid", "Screen-Space", "Angular Radius"};
                                        int methodIdx = static_cast<int>(pp.cocAdaptiveFocalMethod);
                                        if (ImGui::Combo("Method", &methodIdx, methodItems, IM_ARRAYSIZE(methodItems))) {
                                            pp.cocAdaptiveFocalMethod = static_cast<PostProcessor::PostSettings::AdaptiveFocalMethod>(methodIdx);
                                        }
                                        ImGui::SliderFloat("Damping", &pp.cocAdaptiveFocalDamping, 0.0f, 1.0f);
                                        float obs = computeObstruction();
                                        ImGui::Text("Obstruction: %.1f%%", obs * 100.0f);
                                    }
                                }
                                ImGui::Checkbox("Enable Zoom-Focal Mapping", &pp.cocEnableZoomMapping);
                                if (pp.cocEnableZoomMapping) {
                                    float adaptiveFocal = pp.cocFocalCurve.evaluate(zoomPercent) + m_playerController->adaptiveFocalOffset();
                                    adaptiveFocal = glm::clamp(adaptiveFocal, 1.0f, 1000.0f);
                                    SplineEditorUI::draw("Focal Mapping", pp.cocFocalCurve, 0.0f, 1000.0f, zoomPercent, adaptiveFocal, false);
                                    SplineEditorUI::draw("Aperture Mapping", pp.cocApertureCurve, 0.0f, 1.0f, zoomPercent, pp.cocAperture, true);
                                    ImGui::Text("Current Zoom: %.1f%%", zoomPercent * 100.0f);
                                } else {
                                    ImGui::SliderFloat("Focal Distance", &pp.cocFocalDistance, 1.0f, 1000.0f);
                                    if (ImGui::SliderFloat("Aperture (Intensity)", &pp.cocAperture, 0.0f, 1.0f, "%.2f")) {}
                                }
                                ImGui::Checkbox("Enable Foreground Blur", &pp.cocEnableForeground);
                            }
                        }
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Motion Blur Settings");
                        ImGui::Checkbox("Enable Camera Motion Blur", &pp.enableMotionBlur);
                        if (pp.enableMotionBlur) {
                            ImGui::SliderFloat("Blur Intensity", &pp.motionBlurAmount, 0.0f, 2.0f, "%.2f");
                        }

                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(1, 0.5f, 1, 1), "Lens Distortion Settings");
                        ImGui::Checkbox("Enable Chromatic Aberration", &pp.enableChromaticAberration);
                        if (pp.enableChromaticAberration) {
                            ImGui::SliderFloat("CA Intensity (Adaptive)", &pp.chromaticAberrationAmount, 0.0f, 5.0f, "%.2f");
                        }

                        ImGui::Separator();
                        if (ImGui::CollapsingHeader("Shadow Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                            m_renderEffects[5]->drawUI(); // Shadows
                            auto& ss = m_shadowRenderer.settings();
                            ImGui::Checkbox("Poisson Disk Sampling", &ss.usePoisson);
                            if (ss.usePoisson) ImGui::SliderInt("Poisson Taps", &ss.poissonTaps, 1, 16);
                            const char* kernelItems[] = {"3x3", "5x5", "7x7", "9x9", "11x11"};
                            int kernelIdx = (ss.pcfKernelSize - 3) / 2;
                            if (ImGui::Combo("PCF Kernel", &kernelIdx, kernelItems, IM_ARRAYSIZE(kernelItems))) ss.pcfKernelSize = 3 + kernelIdx * 2;
                            ImGui::Separator();
                            ImGui::Text("Stability & Performance");
                            ImGui::Checkbox("Enable Positional Snap (Recommended)", &ss.enablePositionalSnap);
                            if (ss.enablePositionalSnap) {
                                ImGui::SliderFloat("Snap Quantize Step", &ss.snapQuantizeStep, 10.0f, 200.0f, "%.0f m");
                                ImGui::TextDisabled("Quantizes C0 size & snaps shadow center to texel grid.");
                            }
                            ImGui::Separator();
                            ImGui::TextDisabled("Legacy (causes artifacts - avoid):");
                            ImGui::Checkbox("Enable Angle Snap (Yaw)", &ss.enableAngleSnapping);
                            if (ss.enableAngleSnapping) ImGui::Checkbox("Enable Temporal Blend", &ss.enableTemporalBlend);
                            else ss.enableTemporalBlend = false;
                            ImGui::Checkbox("Enable Shadow Frustum Culling", &ss.enableShadowCulling);
                            ImGui::Separator();
                            ImGui::Checkbox("Debug Light Rays", &ss.debugLightRays);
                            ImGui::Checkbox("Debug Shadows (Magenta)", &ss.debugMagentaShadow);
                            ImGui::SetItemTooltip("Paints sun-shadowed fragments magenta live — isolate the sun shadow from shading/cloud shadows.");
                            if (ss.enableAdaptiveShadows) {
                                ImGui::Separator();
                                ImGui::Text("Adaptive Shadow Curves");
                                float c0Current = ss.c0SizeCurve.evaluate(zoomPercent);
                                float minSizeCurrent = ss.minModelShadowSizeCurve.evaluate(zoomPercent);
                                ImGui::Text("C0 Size: %.0f | Min Model Size: %.1f", c0Current, minSizeCurrent);
                                SplineEditorUI::draw("C0 Size by Zoom", ss.c0SizeCurve, 10.0f, 10000.0f, zoomPercent, c0Current);
                                SplineEditorUI::draw("Min Model Shadow Size", ss.minModelShadowSizeCurve, 0.0f, 30.0f, zoomPercent, minSizeCurrent);
                                float camPitchDeg = glm::degrees(m_camera.orbitPitch());
                                float tPitch = glm::clamp((camPitchDeg - 10.0f) / (90.0f - 10.0f), 0.0f, 1.0f);
                                float farRangeCurrent = ss.farRangeCurve.evaluate(tPitch);
                                ImGui::Text("Current Pitch: %.1f | Shadow Far Range: %.0f", camPitchDeg, farRangeCurrent);
                                SplineEditorUI::draw("Shadow Far Range by Pitch", ss.farRangeCurve, 5000.0f, 50000.0f, tPitch, farRangeCurrent, false, "Pitch %");
                            }
                        }

                        ImGui::Separator();
                        m_mipmapMenu.drawUI(this);
                        if (m_mipmapMenu.modeChanged) {
                            m_mipmapMenu.modeChanged = false;
                            m_modelRenderer.setMipmapsEnabled(m_mipmapMenu.config.enabled);
                            m_terrainRenderer.setMipmapsEnabled(m_mipmapMenu.config.enabled, m_mipmapMenu.config.mode);
                        }
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Lighting")) {
                        auto& pp = m_postSettings;
                        // Keep render-effect toggles synced with systems
                        pp.enableFog = m_renderEffects[0]->isEnabled();
                        m_shadowRenderer.settings().enabled = m_renderEffects[5]->isEnabled();
                        pp.enableDoF = m_renderEffects[3]->isEnabled();

                        drawLightingControls();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Skybox")) {
                        auto& skyCfg = m_skybox.editableConfig();
                        auto& pp = m_postSettings;

                        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Skybox Configuration");
                        ImGui::Separator();

                        // Render-effect toggles that belong to the skybox/fog subsystem
                        m_renderEffects[6]->drawUI(); // Skybox IBL
                        m_renderEffects[0]->drawUI(); // Fog
                        pp.enableFog = m_renderEffects[0]->isEnabled();
                        ImGui::Separator();

                        auto applySkybox = [&]() {
                            m_skybox.commitConfig();
                            m_water.setSkyTexture(m_skybox.outputView(), m_skybox.outputSampler());
                            // Sync the procedural sky cloud look into the active CloudLayerRenderer.
                            // Manual edits override any automatic transition from weather-type changes.
                            m_cloudAmountInTransition = false;
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            const auto& proc = skyCfg.procedural;
                            cloudCfg.cloudScale = proc.cloudScale;
                            cloudCfg.cloudLightness = proc.cloudLightness;
                            cloudCfg.cloudShade = proc.cloudShade;
                            cloudCfg.cloudSoftness = proc.cloudSoftness;
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                        };

                        const char* backendItems[] = {"Procedural", "CloudLayers", "Volumetric"};
                        int backendIdx = static_cast<int>(skyCfg.backend);
                        if (ImGui::Combo("Backend", &backendIdx, backendItems, IM_ARRAYSIZE(backendItems))) {
                            skyCfg.backend = static_cast<SkyBackendType>(backendIdx);
                            applySkybox();
                            if (skyCfg.backend != SkyBackendType::Procedural) {
                                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Warning: backend '%s' not implemented yet; falls back to procedural.", SkyConfig::backendName(skyCfg.backend));
                            }
                        }
                        ImGui::Text("Current active backend: %s", m_skybox.currentBackendName());
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Procedural Sky Parameters");
                        if (ImGui::SliderFloat("Star Density", &skyCfg.procedural.starDensity, 0.0f, 1.0f)) applySkybox();
                        if (ImGui::SliderFloat("Star Twinkle Speed", &skyCfg.procedural.starTwinkleSpeed, 0.0f, 10.0f)) applySkybox();
                        if (ImGui::Checkbox("Enable Stars", &skyCfg.procedural.enableStars)) applySkybox();
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(0, 1, 1, 1), "Cloud Look");
                        if (ImGui::Checkbox("Enable Clouds", &skyCfg.procedural.enableClouds)) applySkybox();
                        if (ImGui::SliderFloat("Cloud Scale", &skyCfg.procedural.cloudScale, 0.1f, 5.0f)) applySkybox();
                        ImGui::SetItemTooltip("Scales the procedural detail frequency and puff thickness.");
                        if (ImGui::SliderFloat("Cloud Softness", &skyCfg.procedural.cloudSoftness, 0.0f, 1.0f)) applySkybox();
                        ImGui::SetItemTooltip("Softens or hardens the edges of cloud puffs.");
                        if (ImGui::SliderFloat("Cloud Lightness", &skyCfg.procedural.cloudLightness, 0.1f, 2.0f)) applySkybox();
                        ImGui::SetItemTooltip("Brightens the sun-lit parts of clouds.");
                        if (ImGui::SliderFloat("Cloud Shade", &skyCfg.procedural.cloudShade, 0.0f, 1.0f)) applySkybox();
                        ImGui::SetItemTooltip("Darkens the shadowed parts of clouds.");
                        if (ImGui::SliderFloat("Storm Tint", &skyCfg.procedural.stormTint, 0.0f, 1.0f)) applySkybox();
                        if (ImGui::Checkbox("Debug: Show Clouds Only", &skyCfg.procedural.debugShowCloudsOnly)) applySkybox();
                        ImGui::Separator();



                        ImGui::TextColored(ImVec4(0, 1, 1, 1), "Cloud Layer (Single) + Shadow");
                        auto help = [](const char* text) {
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
                        };
                        auto applyCloudLayers = [&]() {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            m_cloudLayersEnabled = cloudCfg.enabled;
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                            m_cloudLayerRenderer.coverageArray().setLayerCount(cloudCfg.layerCount);
                            m_cloudLayerRenderer.coverageArray().setSize(cloudCfg.coverageSize);
                            m_cloudLayerRenderer.coverageArray().setOctaves(cloudCfg.layerCount);
                            // cloudAmount is a runtime shader threshold; the coverage texture
                            // is generated once with the full raw noise range.
                        };
                        CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                        if (ImGui::Checkbox("Enable Cloud Layer", &cloudCfg.enabled)) applyCloudLayers();
                        help("Toggle the single cloud layer, its debug plane and projected shadow.");

                        if (ImGui::SliderFloat("Cloud Bottom", &cloudCfg.cloudBottom, 100.0f, 6000.0f)) {
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                            m_cloudLayerRenderer.coverageArray().setCloudBottom(cloudCfg.cloudBottom);
                            m_postProcessor.weatherRenderer().setCloudBaseHeight(cloudCfg.cloudBottom);
                        }
                        help("Altitude of the single cloud layer base.");
                        if (ImGui::SliderFloat("Cloud Thickness", &cloudCfg.cloudThickness, 200.0f, 2000.0f)) {
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                            m_cloudLayerRenderer.coverageArray().setLayerSpacing(cloudCfg.cloudThickness / glm::max(1u, cloudCfg.layerCount - 1));
                        }
                        help("Vertical thickness of the single cloud layer.");
                        if (ImGui::SliderFloat("Wind Speed", &cloudCfg.windSpeed, 0.0f, 10.0f)) {
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                        }
                        help("How fast cloud patterns drift.");
                        if (ImGui::SliderFloat("Wind Direction X", &cloudCfg.windDirection.x, -1.0f, 1.0f)) {
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                        }
                        if (ImGui::SliderFloat("Wind Direction Z", &cloudCfg.windDirection.z, -1.0f, 1.0f)) {
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                        }
                        help("Direction the cloud patterns drift (X and Z components).");
                        if (ImGui::SliderFloat("Noise Scale", &cloudCfg.noiseScale, 0.001f, 0.02f)) {
                            m_cloudLayerRenderer.setConfig(cloudCfg);
                        }
                        help("Frequency of the procedural detail noise.");
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            if (ImGui::SliderFloat("Coverage Plane Height", &cloudCfg.debugPlaneY, 100.0f, 2000.0f)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("Move the coverage debug plane up/down.");
                        }
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            if (ImGui::SliderFloat("Debug Plane Thickness", &cloudCfg.cloudDebugThickness, 50.0f, 500.0f)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("Slab thickness sampled by the low-pitch ray-march.");
                        }
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            if (ImGui::SliderFloat("Spike Height", &cloudCfg.cloudSpikeHeight, 0.0f, 500.0f)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("How high red (dense) columns spike upward.");
                        }
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            if (ImGui::SliderFloat("Base Depth", &cloudCfg.cloudBaseDepth, 0.0f, 200.0f)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("How deep below the plane the cloud base goes.");
                        }
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            if (ImGui::SliderFloat("Spike Width", &cloudCfg.cloudSpikeWidth, 0.0f, 500.0f)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("How wide the cloud puff spreads sideways.");
                        }
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            if (ImGui::SliderFloat("Cloud Amount", &cloudCfg.cloudAmount, 0.0f, 1.0f)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            if (ImGui::IsItemDeactivatedAfterEdit()) {
                                // Regenerate the thresholded coverage texture when the slider
                                // is released; regenerating every frame stalls the GPU.
                                m_cloudAmountInTransition = false;
                                m_cloudLayerRenderer.coverageArray().setCoverage(cloudCfg.cloudAmount);
                            }
                            help("0 = clear sky, 1 = overcast.");
                        }
                        ImGui::TextColored(ImVec4(0, 1, 1, 1), "Low-pitch Ray-march");
                        {
                            help("Above this pitch angle use POM; below use ray-march.");
                        }
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            int steps = static_cast<int>(cloudCfg.cloudRayMarchSteps);
                            if (ImGui::SliderInt("Ray-march Steps", &steps, 4, 512)) {
                                cloudCfg.cloudRayMarchSteps = static_cast<float>(steps);
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("Step count for the low-pitch ray-march.");
                        }
                        {
                            CloudLayerRenderer::Config cloudCfg = m_cloudLayerRenderer.config();
                            if (ImGui::Checkbox("Show Cloud Shadows", &cloudCfg.showCloudShadows)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("Project cloud coverage as a dark shadow layer on the ground.");
                            if (ImGui::SliderFloat("Cloud Shadow Opacity", &cloudCfg.cloudShadowOpacity, 0.0f, 1.0f)) {
                                m_cloudLayerRenderer.setConfig(cloudCfg);
                            }
                            help("Opacity of the cloud shadow overlay.");
                        }
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(0, 1, 0.5f, 1), "Local Clouds (Debug)");
                        {
                            ImGui::Text("Independent clouds: %u / %u (layers: %zu)",
                                        m_independentCloudCount, LocalCloud::MAX_COUNT,
                                        m_independentCloudLayers.size());

                            if (ImGui::Button("Spawn Local Cloud Over Player")) {
                                // Queued: spawned at the player position, at the
                                // altitude slider value, in the next update tick
                                // (layer creation submits GPU work). Always adds;
                                // never replaces existing clouds.
                                m_pendingCloudSpawns.push_back({m_localCloudAltitude, m_localCloudDensity,
                                                                0.0f, 0.0f, m_localCloudRain});
                            }
                            if (ImGui::Button("Clear Local Clouds")) {
                                clearIndependentClouds();
                            }
                            ImGui::Checkbox("Particles Follow Cloud", &m_localCloudRain);
                            ImGui::SetItemTooltip("The spawned cloud carries its own precipitation (rain/snow) that follows its ground shadow.");
                            ImGui::Checkbox("Debug Particle Occlusion", &m_postSettings.debugRainOcclusion);
                            ImGui::SetItemTooltip("Per-particle occlusion colors: green=visible, cyan=camera above cloud base, red=behind geometry, blue=blocked by cloud, yellow=under surface, magenta=outside cloud shadow mask.");
                            ImGui::Checkbox("Debug Particle Regions", &m_postSettings.debugRainRegions);
                            ImGui::SetItemTooltip("Draw 3D wireframe bounding boxes outlining the active precipitation simulation regions (global and per-cloud followers).");
                            ImGui::Checkbox("Debug Splash Area", &m_postSettings.debugSplashArea);
                            ImGui::SetItemTooltip("Paints every visible rain splash as a pure RGB(255,0,0) quad, through the exact same occlusion gates as normal rendering - red on the ground = splashes really appear there. Works on any tier.");
                            ImGui::Checkbox("Cloud Tint Colors", &m_cloudTestColors);
                            ImGui::SetItemTooltip("Debug: each cloud keeps its normal shaded gradient but multiplied by a distinct saturated color per layer - to track individual clouds and overlaps.");

                            ImGui::SliderFloat("Local Radius X", &m_localCloudRadiusX, 10.0f, 500.0f);
                            ImGui::SliderFloat("Local Radius Z", &m_localCloudRadiusZ, 10.0f, 500.0f);
                            ImGui::SliderFloat("Local Density", &m_localCloudDensity, 0.0f, 1.0f);
                            ImGui::SliderFloat("Local Coverage", &m_localCloudCoverage, 0.0f, 1.0f);
                            ImGui::SliderFloat("Local Falloff", &m_localCloudFalloff, 0.1f, 4.0f);
                            ImGui::SliderFloat("Local Altitude", &m_localCloudAltitude, 0.0f, 1.0f);
                            ImGui::SliderFloat("Local Rotation", &m_localCloudRotation, 0.0f, 6.283f);
                        }
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Sun & Moon");
                        float sunPitchMax = m_dayNightCycle.sunPitchMax();
                        if (ImGui::SliderFloat("Sun Max Altitude", &sunPitchMax, 5.0f, 89.0f, "%.0f deg")) {
                            m_dayNightCycle.setSunPitchMax(sunPitchMax);
                        }
                        float moonPitchMax = m_dayNightCycle.moonPitchMax();
                        if (ImGui::SliderFloat("Moon Max Altitude", &moonPitchMax, 5.0f, 89.0f, "%.0f deg")) {
                            m_dayNightCycle.setMoonPitchMax(moonPitchMax);
                        }
                        if (ImGui::SliderInt("Sun Ray Count", &skyCfg.procedural.sunRayCount, 0, 32)) applySkybox();
                        if (ImGui::SliderFloat("Sun Size", &skyCfg.procedural.sunSize, 0.1f, 2.0f)) applySkybox();
                        if (ImGui::SliderFloat("Sun Limb Darkening", &skyCfg.procedural.sunLimbDarkening, 0.0f, 1.0f)) applySkybox();
                        if (ImGui::SliderFloat("Sun Halo Intensity", &skyCfg.procedural.sunHaloIntensity, 0.0f, 2.0f)) applySkybox();
                        if (ImGui::SliderFloat("Sun Halo Rays", &skyCfg.procedural.sunHaloRays, 0.0f, 16.0f)) applySkybox();
                        if (ImGui::SliderFloat("Sun Halo Size", &skyCfg.procedural.sunHaloSize, 0.1f, 3.0f)) applySkybox();
                        if (ImGui::SliderFloat("Moon Size", &skyCfg.procedural.moonSize, 0.1f, 2.0f)) applySkybox();
                        if (ImGui::Checkbox("Moon Phase Auto", &skyCfg.procedural.moonPhaseAuto)) applySkybox();
                        if (!skyCfg.procedural.moonPhaseAuto) {
                            if (ImGui::SliderFloat("Moon Phase", &skyCfg.procedural.moonPhase, 0.0f, 1.0f)) applySkybox();
                        }
                        if (ImGui::SliderFloat("Moon Phase Offset (legacy)", &skyCfg.procedural.moonPhaseOffset, 0.0f, 0.5f)) applySkybox();
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(1, 0.5f, 1, 1), "Atmosphere / Fog");
                        if (pp.enableFog) {
                            ImGui::SliderFloat("Fog Start", &pp.fogStart, 1.0f, 500.0f);
                            ImGui::SliderFloat("Fog End", &pp.fogEnd, 10.0f, 2000.0f);
                            ImGui::SliderFloat("Fog Opacity", &pp.fogOpacity, 0.0f, 1.0f);
                            ImGui::ColorEdit3("Fog Color", &pp.fogColor.x);
                        }
                        ImGui::Separator();

                        if (ImGui::CollapsingHeader("Day/Night Spline Curves")) {
                            ImGui::Text("Current Time: %.2f", m_dayNightCycle.timeOfDay());
                            SplineEditorUI::draw("Sun Kelvin", m_dayNightCycle.sunKelvinCurve, 1000.0f, 15000.0f, m_dayNightCycle.timeOfDay(), m_dayNightCycle.getSunKelvin());
                            SplineEditorUI::draw("Sun Intensity", m_dayNightCycle.sunIntensityCurve, 0.0f, 5.0f, m_dayNightCycle.timeOfDay(), m_dayNightCycle.getSunIntensity());
                            // "Ambient Intensity" is no longer a time spline - it's a constant
                            // slider in the Lighting panel. Moonlight now handles night fill.
                            SplineEditorUI::draw("Moon Intensity", m_dayNightCycle.moonIntensityCurve, 0.0f, 2.0f, m_dayNightCycle.timeOfDay(), m_dayNightCycle.getMoonIntensity());
                            SplineEditorUI::draw("Fog Density", m_dayNightCycle.fogDensityCurve, 0.0f, 1.0f, m_dayNightCycle.timeOfDay(), m_dayNightCycle.getFogDensity());
                            m_dayNightCycle.syncKeyframesFromCurves();
                        }
                        ImGui::Separator();

                        if (ImGui::Button("Save to Disk")) {
                            std::ofstream ofs("data/skybox_config.json");
                            if (ofs) {
                                ofs << skyCfg.toJson().dump(2);
                                ofs.close();
                                m_skyboxConfig.load("data/skybox_config.json");
                                ERUPTION_LOG_INFO("Skybox config saved to data/skybox_config.json");
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Reload from Disk (F5)")) {
                            if (m_skyboxConfig.reloadIfModified()) {
                                SkyConfig reloaded;
                                reloaded.loadFromJson(m_skyboxConfig.root());
                                m_skybox.reloadConfig(reloaded);
                                m_water.setSkyTexture(m_skybox.outputView(), m_skybox.outputSampler());
                                ERUPTION_LOG_INFO("Skybox config reloaded from disk");
                            }
                        }
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Weather")) {
                        drawWeatherTab();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Liquids")) {
                        {
                            const Mat4 waterView = m_camera.viewMatrix();
                            const Mat4 waterProj = m_camera.projNoJitter();
                            VkExtent2D waterExt = m_vulkan.swapExtent();
                            m_waterMenu.drawUI(waterView, waterProj,
                                               (float)waterExt.width, (float)waterExt.height);
                        }
                        if (m_waterMenu.liquidShapeDirty) {
                            // Nudge de posicao/rotacao mexeu num disco -
                            // reconstroi a malha da lava com os novos
                            // offsets. setupLavaLiquid NAO ressincroniza
                            // m_waterMenu.liquids (so' syncLiquidMenuFromMap
                            // faz isso, e so' roda em map load), entao o
                            // nudge que acabou de ser feito sobrevive.
                            setupLavaLiquid(m_currentMap.get());
                            m_waterMenu.liquidShapeDirty = false;
                        }
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Sprites")) {
                        ImGui::TextColored(ImVec4(0, 1, 1, 1), "Sprite Rendering Settings");
                        ImGui::Separator();
                        
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "diorama Character Tuning");
                        if (ImGui::SliderFloat("Movement Speed", &m_charConfig.moveSpeed, 10.0f, 500.0f)) m_charConfig.save("data/char_config.json");
                        ImGui::TextDisabled("Animation synced: Proportional (100 spd = 65ms).");
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Billboard Tilt Spline");
                        float pitchDeg = glm::degrees(m_camera.orbitPitch());
                        constexpr float kTiltPitchMin = 0.0f;
                        constexpr float kTiltPitchMax = 89.0f;
                        float pitchNorm = (pitchDeg - kTiltPitchMin) / (kTiltPitchMax - kTiltPitchMin);
                        float currentTilt = m_charConfig.billboardTiltSpline.evaluate(pitchNorm);
                        SplineEditorUI::draw("Tilt by Pitch", m_charConfig.billboardTiltSpline,
                                             kTiltPitchMin, kTiltPitchMax, -90.0f, 90.0f,
                                             pitchDeg, currentTilt, false, "Pitch");
                        m_charConfig.save("data/char_config.json");
                        ImGui::Separator();

                        float exposure = m_spriteRenderer.spriteExposure();
                        if (ImGui::SliderFloat("Exposure (Ambient)", &exposure, 0.0f, 5.0f, "%.2f")) {
                            m_spriteRenderer.setSpriteExposure(exposure);
                        }
                        ImGui::TextWrapped("Controls how much the ambient light (Sky/Ground) affects the sprite color. "
                                           "Higher values make sprites react more to world lighting.");
                        ImGui::Separator();
                        const char* shadowTypes[] = { "Planar (Mirror)", "Circle (Blob)" };
                        if (ImGui::Combo("Shadow Type", &m_charConfig.shadowType, shadowTypes, 2)) {
                            m_charConfig.save("data/char_config.json");
                        }

                        if (m_charConfig.shadowType == 0) {
                            if (ImGui::SliderFloat("Planar Softness", &m_charConfig.planarShadowSoftness, 0.0f, 1.0f, "%.2f")) {
                                m_charConfig.save("data/char_config.json");
                            }
                            if (ImGui::SliderFloat("Planar Dilation", &m_charConfig.planarShadowDilation, 0.1f, 3.0f, "%.2f")) {
                                m_charConfig.save("data/char_config.json");
                            }
                        } else {
                            if (ImGui::SliderFloat("Circle Softness", &m_charConfig.circleShadowSoftness, 0.0f, 1.0f, "%.2f")) {
                                m_charConfig.save("data/char_config.json");
                            }
                            if (ImGui::SliderFloat("Circle Dilation", &m_charConfig.circleShadowDilation, 0.1f, 3.0f, "%.2f")) {
                                m_charConfig.save("data/char_config.json");
                            }
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            } ImGui::End();
        }

        if (m_showPlayerLightMenu) {
            ImGui::SetNextWindowSize(ImVec2(360, 230), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(0.9f);
            if (ImGui::Begin("Luz do Jogador (F10)", &m_showPlayerLightMenu)) {
                auto& L = m_playerLight;
                ImGui::TextWrapped("Point light presa ao sprite principal. Serve pra' testar como a "
                                   "luz interage com o mundo (PBR, sombra suave, molhado) sem depender "
                                   "de um prop com luz autoral por perto.");
                ImGui::Separator();
                ImGui::Checkbox("Ligada", &L.enabled);
                ImGui::ColorEdit3("Cor", &L.color.x, ImGuiColorEditFlags_Float);
                ImGui::SliderFloat("Intensidade", &L.intensity, 0.0f, 30.0f, "%.1f");
                ImGui::SliderFloat("Alcance", &L.radius, 10.0f, 800.0f, "%.0f u");
                ImGui::SliderFloat("Altura (fracao do sprite)", &L.heightFrac, 0.0f, 1.5f, "%.2f");
                ImGui::SetItemTooltip("0 = nos pes, 0.6 = peito, 1.0 = topo da cabeca.");
                if (ImGui::Button("Branco quente")) L.color = Vec3(1.0f, 0.85f, 0.6f);
                ImGui::SameLine(); if (ImGui::Button("Lava"))   L.color = Vec3(1.0f, 0.42f, 0.13f);
                ImGui::SameLine(); if (ImGui::Button("Azul"))   L.color = Vec3(0.35f, 0.55f, 1.0f);
                ImGui::SameLine(); if (ImGui::Button("Verde"))  L.color = Vec3(0.4f, 1.0f, 0.45f);
            }
            ImGui::End();
        }
        if (m_showABTestMenu) {
            ImGui::SetNextWindowSize(ImVec2(460, 420), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(0.9f);
            if (ImGui::Begin("A/B Test (F9)", &m_showABTestMenu)) {
                if (ImGui::BeginTabBar("ABTestTabs")) {
                    if (ImGui::BeginTabItem("Water")) {
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Isolate which water feature causes the black glitch");
                        ImGui::Separator();

                        uint32_t& mask = m_waterMenu.abTestMask;
                        int maskInt = static_cast<int>(mask & 0xFFu);
                        if (ImGui::SliderInt("AB Mask", &maskInt, 0, 255)) {
                            mask = static_cast<uint32_t>(maskInt);
                            m_waterMenu.settingsChanged = true;
                            m_waterMenu.saveConfig();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = all OFF, 255 = all ON.\nUse slider or checkboxes below to test combinations.");

                        const char* abLabels[8] = {
                            "Refraction", "Normal Map", "Depth Valid", "Sky Reflect",
                            "Foam", "Gerstner Waves", "Fresnel+Spec", "Pixel Snap"
                        };
                        for (int i = 0; i < 8; i++) {
                            bool bit = (mask >> i) & 1u;
                            if (ImGui::Checkbox(abLabels[i], &bit)) {
                                mask ^= (1u << i);
                                m_waterMenu.settingsChanged = true;
                                m_waterMenu.saveConfig();
                            }
                            if ((i % 2) == 0 && i < 7) ImGui::SameLine();
                        }

                        ImGui::Separator();
                        ImGui::Text("Quick Presets");
                        if (ImGui::Button("All ON (255)")) { mask = 0xFFu; m_waterMenu.settingsChanged = true; m_waterMenu.saveConfig(); }
                        ImGui::SameLine();
                        if (ImGui::Button("All OFF (0)")) { mask = 0x00u; m_waterMenu.settingsChanged = true; m_waterMenu.saveConfig(); }
                        ImGui::SameLine();
                        if (ImGui::Button("Only Normal+Refr (3)")) { mask = 0x03u; m_waterMenu.settingsChanged = true; m_waterMenu.saveConfig(); }
                        ImGui::SameLine();
                        if (ImGui::Button("Only Normal (2)")) { mask = 0x02u; m_waterMenu.settingsChanged = true; m_waterMenu.saveConfig(); }

                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Tip: Start at 0 and increase until glitch appears.");
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Lighting")) {
                        drawLightingControls();

                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "A/B lighting comparison");

                        if (m_abCaptureStep == 0) {
                            if (ImGui::Button("Capture A/B Pair")) {
                                m_abCaptureStep = 1;
                                m_abCaptureFrame = m_framesCount + 2;
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saves /tmp/ab_pbr/ab_pbr_on.png and /tmp/ab_pbr/ab_pbr_off.png for comparison.");
                        } else {
                            ImGui::Text("Capturing... step %d", m_abCaptureStep);
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            } ImGui::End();
        }

        if (m_showObjectMenu) {
            ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(0.85f);
            if (ImGui::Begin("Object Manager (F1)", &m_showObjectMenu)) {
                ImGui::Checkbox("Debug Model Pivots (Red Line)", &m_debugModelPivots);
                
                if (m_debugModelPivots && m_currentMap) {
                    if (ImGui::CollapsingHeader("Height Debug (Nearby)", ImGuiTreeNodeFlags_DefaultOpen)) {
                        const auto& insts = m_modelRenderer.getInstances();
                        Vec3 camPos = m_camera.position();
                        if (ImGui::BeginChild("HeightList", ImVec2(0, 150), true)) {
                            for (const auto& inst : insts) {
                                if (!inst.enabled) continue;
                                float dist = glm::distance(camPos, inst.worldCenter);
                                if (dist > 150.0f) continue;

                                Vec3 pivotPos = Vec3(inst.transform[3]);
                                float groundY = TerrainParser::getTerrainHeightAt(m_currentMap->terrain, pivotPos.x, pivotPos.z);
                                float diff = pivotPos.y - groundY;
                                
                                ImVec4 color = std::abs(diff) > 0.1f ? ImVec4(1, 0.5f, 0, 1) : ImVec4(0, 1, 0, 1);
                                ImGui::TextColored(color, "%.2f | %s", diff, inst.name.c_str());
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("PivotY: %.3f | GndY: %.3f", pivotPos.y, groundY);
                                }
                            }
                            ImGui::EndChild();
                        }
                    }
                }
                ImGui::Separator();

                if (ImGui::CollapsingHeader("Map Selector", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (m_playerController) m_playerController->renderMapSelector();
                }
                auto& pts = m_deferredLighting.getPointLights();
                if (ImGui::CollapsingHeader("Point Lights")) {
                    if (ImGui::BeginChild("LightList", ImVec2(0, 150), true)) {
                        for (size_t i = 0; i < pts.size(); i++) {
                            ImGui::PushID((int)i); ImGui::Checkbox("##enabled", &pts[i].enabled); ImGui::SameLine();
                            char label[64]; sprintf(label, "Light %zu", i);
                            if (ImGui::Selectable(label, m_selectedLightIndex == (int)i)) { m_selectedLightIndex = (int)i; m_camera.setOrbitTarget(pts[i].position); }
                            ImGui::PopID();
                        }
                        ImGui::EndChild();
                    }
                }
                if (ImGui::CollapsingHeader("Textures")) {
                    const auto& tCache = m_modelRenderer.getTextureCache();
                    if (ImGui::BeginChild("TextureList", ImVec2(0, 150), true)) {
                        for (auto const& [path, slot] : tCache) ImGui::Text("[%u] %s", slot, path.c_str());
                        ImGui::EndChild();
                    }
                }
                if (ImGui::CollapsingHeader("Models", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& insts = const_cast<std::vector<ModelInstance>&>(m_modelRenderer.getInstances());
                    if (ImGui::BeginChild("ModelList", ImVec2(0, 150), true)) {
                        for (size_t i = 0; i < insts.size(); i++) {
                            ImGui::PushID((int)i);
                            ImGui::Checkbox("##enabled", &insts[i].enabled);
                            ImGui::SameLine();
                            char mlbl[256]; sprintf(mlbl, "%zu: %s", i, insts[i].name.c_str());
                            if (ImGui::Selectable(mlbl, m_selectedModelInstance == (int)i)) {
                                m_selectedModelInstance = (int)i;
                                m_camera.setOrbitTarget(insts[i].worldCenter);
                            }
                            ImGui::PopID();
                        }
                        // Terreno: aparece na lista pra deixar claro que ele
                        // FAZ PARTE da cena, mas nao tem gizmo - o terreno e'
                        // uma malha estatica em espaco de mundo (sem matriz
                        // de modelo no pipeline, so' viewProj no push
                        // constant), diferente de ModelInstance::transform.
                        // Mover a malha inteira via gizmo exigiria adicionar
                        // uma matriz de modelo ao push constant do terreno
                        // (e ao da sombra tambem) - fora do escopo deste
                        // gizmo de instancias.
                        ImGui::BeginDisabled();
                        ImGui::Selectable("Terrain (static mesh, no gizmo)", false);
                        ImGui::EndDisabled();
                        ImGui::EndChild();
                    }
                    if (m_selectedModelInstance >= 0 && m_selectedModelInstance < (int)insts.size()) {
                        ModelInstance& sel = insts[static_cast<size_t>(m_selectedModelInstance)];
                        ImGui::Text("Selected: %s", sel.name.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Deselect")) m_selectedModelInstance = -1;

                        // Gizmo de viewport (setinhas tipo Unity/Blender/
                        // Unreal), pedido explicito do autor - opera direto
                        // em cima de ModelInstance::transform (a mesma
                        // matriz usada pro draw), entao o efeito e' imediato
                        // e visivel; nao e' salvo no mapa (nudge de sessao,
                        // mesmo espirito do nudge de lava no WaterDebugMenu).
                        static ImGuizmo::OPERATION s_modelGizmoOp = ImGuizmo::TRANSLATE;
                        if (ImGui::RadioButton("Move##modelGizmo", s_modelGizmoOp == ImGuizmo::TRANSLATE)) s_modelGizmoOp = ImGuizmo::TRANSLATE;
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Rotate##modelGizmo", s_modelGizmoOp == ImGuizmo::ROTATE)) s_modelGizmoOp = ImGuizmo::ROTATE;
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Scale##modelGizmo", s_modelGizmoOp == ImGuizmo::SCALE)) s_modelGizmoOp = ImGuizmo::SCALE;

                        const Mat4 view = m_camera.viewMatrix();
                        // Camera::rebuild() faz proj[1][1] *= -1 (NDC do
                        // Vulkan e' Y pra baixo) - o ImGuizmo espera projecao
                        // estilo OpenGL e desenha errado (de um jeito que
                        // muda com o angulo da camera) se receber o flip
                        // direto. Desfazer aqui e' o que faz as setinhas
                        // ficarem fixas no mundo ao orbitar a camera.
                        Mat4 gizmoProj = m_camera.projNoJitter();
                        gizmoProj[1][1] *= -1.0f;
                        VkExtent2D ext = m_vulkan.swapExtent();
                        // SO' DESENHA COM O OBJETO NA FRENTE DA CAMERA. O
                        // ImGuizmo projeta a origem do objeto dividindo por w
                        // sem checar sinal: com o objeto ATRAS (w <= 0) a
                        // posicao de tela vira lixo espelhado e os eixos saem
                        // como retas enormes que o clip da drawlist corta na
                        // borda - le-se como "uma linha cinza no topo e outra
                        // na esquerda", que foi o que o autor viu depois que
                        // os eixos XYZ entraram (2026-09-04). Longe demais do
                        // quadro tambem nao vale a pena desenhar.
                        const Vec4 clipPos = m_camera.viewProjNoJitter() *
                                             Vec4(Vec3(sel.transform[3]), 1.0f);
                        const bool gizmoVisivel =
                            clipPos.w > 1e-4f &&
                            std::abs(clipPos.x / clipPos.w) < 4.0f &&
                            std::abs(clipPos.y / clipPos.w) < 4.0f;
                        if (gizmoVisivel) {
                            ImGuizmo::SetRect(0, 0, (float)ext.width, (float)ext.height);
                            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(gizmoProj),
                                                  s_modelGizmoOp, ImGuizmo::WORLD,
                                                  glm::value_ptr(sel.transform));
                        }
                    }
                }
            } ImGui::End();
        }

        // ERUPTION_TEST_SHOW_STATS=1 (debug): open the F3 telemetry window from boot
        // (and preselect the Weather tab) so headless screenshot runs capture it.
        static const bool s_forceStatsMenu = std::getenv("ERUPTION_TEST_SHOW_STATS") != nullptr;
        if (s_forceStatsMenu) m_showStatsMenu = true;
        if (m_showStatsMenu) {
            if (m_demoPinStats) {
                // Demo de video (ERUPTION_TEST_DEMO stats=1): painel encostado a'
                // direita, altura da tela, sempre visivel - a posicao salva no
                // imgui.ini pode estar fora da janela.
                const ImVec2 ds = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowPos(ImVec2(ds.x - 570.0f, 10.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(560.0f, ds.y - 20.0f), ImGuiCond_Always);
            } else {
                ImGui::SetNextWindowSize(ImVec2(1100, 520), ImGuiCond_FirstUseEver);
            }
            ImGui::SetNextWindowBgAlpha(0.85f);
            if (ImGui::Begin("Telemetry (F3)", &m_showStatsMenu)) {
                {
                    // Weather GPU cost breakdown: what each weather-related
                    // pass costs this frame, in ms — same visual language as
                    // the GPU column.
                    auto drawWeatherGpuCost = []() {
                        static const char* kWeatherPasses[] = {
                            "Rain Heightmap", "Rain TopDown Depth", "Weather Overlay", "Weather Particles",
                            "Cloud Shadow Map", "Cloud Shadows (field)", "Cloud Volumes (field)", "Cloud Layers",
                        };
                        static const ImVec4 kPassColors[] = {
                            ImVec4(0.2f, 0.6f, 1, 1), ImVec4(0.2f, 0.85f, 0.9f, 1), ImVec4(0.6f, 0.4f, 1, 1),
                            ImVec4(0.3f, 0.5f, 1, 1), ImVec4(0.5f, 0.5f, 0.6f, 1), ImVec4(0.35f, 0.35f, 0.45f, 1),
                            ImVec4(0.7f, 0.8f, 1, 1), ImVec4(0.9f, 0.9f, 0.95f, 1),
                        };
                        std::vector<ImGuiEx::PieChartSlice> costSlices;
                        float costTotalMs = 0.0f;
                        const size_t passN = sizeof(kWeatherPasses) / sizeof(kWeatherPasses[0]);
                        // ALL passes are listed unconditionally (even at 0 ms):
                        // hiding idle ones made the legend re-layout and flicker
                        // whenever a pass appeared/disappeared (author feedback).
                        for (size_t i = 0; i < passN; ++i) {
                            float ms = Profiler::getGpuTime(kWeatherPasses[i]);
                            costSlices.push_back({kWeatherPasses[i], ms,
                                                  ImGui::ColorConvertFloat4ToU32(kPassColors[i])});
                            costTotalMs += ms;
                        }
                        static int costHover = -1;
                        ImGuiEx::PieChart("WeatherCostChart", 60.0f, costSlices, costHover);
                        ImGui::SameLine();
                        // Legend in two columns of 4 so the block matches the
                        // pie disc height instead of stacking 9 lines tall.
                        bool anyHovered = false;
                        for (size_t col = 0; col < 2; ++col) {
                            ImGui::BeginGroup();
                            for (size_t i = col * 4; i < col * 4 + 4 && i < passN; ++i) {
                                if (costSlices[i].value > 0.001f) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(costSlices[i].color));
                                    ImGui::Text(" %s: %.2f ms", costSlices[i].label.c_str(), costSlices[i].value);
                                    if (ImGui::IsItemHovered()) { costHover = static_cast<int>(i); anyHovered = true; }
                                    ImGui::PopStyleColor();
                                } else {
                                    ImGui::TextDisabled(" %s: 0.00 ms", costSlices[i].label.c_str());
                                }
                            }
                            if (col == 1) ImGui::Text(" Total: %.2f ms", costTotalMs);
                            ImGui::EndGroup();
                            if (col == 0) ImGui::SameLine();
                        }
                        if (!anyHovered) costHover = -1;
                    };
                    {
                        ImGui::Text("FPS: %.1f (%.2f ms)", m_fps, m_frameTime);
                        static int smoothIdx = 1;
                        const int frameOptions[] = {1, 10, 20, 30, 40, 50, 60};
                        if (ImGui::SliderInt("Average frames", &smoothIdx, 0, 6, "%d")) {
                            Profiler::setSmoothFrames(frameOptions[smoothIdx]);
                        }

                        // ---- CPU frame breakdown -------------------------------
                        // Where the wall-clock frame actually goes. The GPU pass
                        // list below only accounts for GPU work; if the CPU total
                        // is close to the frame time, the bottleneck is the CPU
                        // and no shader tuning will help.
                        //
                        // Every row is ALWAYS drawn, even at 0.00 ms: rows that
                        // appear and disappear make the panel jump around and
                        // hide whatever was flickering.
                        {
                            // AGREGADOS ANINHADOS FICAM DE FORA DA SOMA. "Post
                            // Total" e' o cronometro que envolve o pos-processamento
                            // INTEIRO e os filhos dele (DoF, bloom, overlay de clima,
                            // composite...) tambem estao na lista; somar os dois
                            // contava o post duas vezes e inflava o "GPU total" do
                            // F3. "Lens Drops" idem, aninhado no Weather Overlay.
                            auto ehAgregado = [](const std::string& n) {
                                return n == "Post Total" || n.rfind("Lens Drops", 0) == 0;
                            };
                            float gpuSum = 0.0f;
                            for (const auto& [n, ms] : Profiler::getAllGpuTimes())
                                if (!ehAgregado(n)) gpuSum += ms;
                            const float cpuSum = m_cpuAdvanceMs + m_cpuAppUpdateMs + m_cpuRenderMs + m_cpuTailMs;
                            // Denominador = periodo REAL deste laco, medido no mesmo
                            // frame que as fases. m_frameTime e' media de janela
                            // (fpsElapsed/fpsFrames no PlayerController): dividir
                            // fase instantanea por media dava "render 110%".
                            const float frameWall = (m_cpuFrameWallMs > 0.001f) ? m_cpuFrameWallMs
                                                                                : m_frameTime;
                            const float waitMs = std::max(0.0f, frameWall - cpuSum);

                            ImGui::Separator();
                            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "CPU frame breakdown");
                            // AVISO DE VALIDACAO. Sem ele o painel parece se
                            // contradizer: "render (record)" alto com os passes
                            // de GPU baixos. A causa e' que a camada de validacao
                            // se instala ENTRE a engine e o driver e cobra em
                            // cada chamada gravada - o custo dela cai inteiro em
                            // render(record), que e' tempo de CPU, e nao aparece
                            // em nenhum cronometro de GPU. Medido em
                            // parana_field/rainy: com validacao 17,3 FPS e
                            // render 55,5 ms; sem, 38,1 FPS e render 24,0 ms.
                            if (m_vulkan.validationActive()) {
                                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                                    "  Vulkan Debug ON - timings are NOT representative");
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(
                                        "Vulkan Debug (the validation layer) sits between the engine and the\n"
                                        "driver and charges per recorded call. Its cost lands entirely in\n"
                                        "'render (record)' - CPU time - and shows up in no GPU timer,\n"
                                        "which is why the panel looks self-contradictory.\n\n"
                                        "Measured on parana_field/rainy (the rain pass records 40 draws\n"
                                        "of 393k instances each, so the layer walks a lot of state):\n"
                                        "  Vulkan Debug ON:   17.3 FPS, render 55.5 ms\n"
                                        "  Vulkan Debug OFF:  38.1 FPS, render 24.0 ms\n\n"
                                        "Run with ERUPTION_NO_VALIDATION=1 for real numbers, or use a\n"
                                        "RelWithDebInfo build (Vulkan Debug exists only in Debug builds).");
                                }
                            }

                            struct Row { const char* label; float ms; ImU32 color; const char* help; };
                            const Row rows[] = {
                                { "advanceFrame (sim)", m_cpuAdvanceMs, IM_COL32(120, 200, 255, 255),
                                  "Engine simulation step: weather, cloud spawning/wind, day-night cycle,\n"
                                  "background map streaming and staging uploads. Runs before any rendering." },
                                { "app onUpdate", m_cpuAppUpdateMs, IM_COL32(255, 190, 90, 255),
                                  "Game-side update: player controller, camera, model animation matrices,\n"
                                  "sprite processing, terrain chunk culling and gameplay logic." },
                                { "render (record)", m_cpuRenderMs, IM_COL32(150, 230, 150, 255),
                                  "Recording the Vulkan command buffer for this frame and submitting it.\n"
                                  "This is CPU work only - the GPU cost is the pass list further down." },
                                { "  beginFrame wait", m_cpuBeginFrameMs, IM_COL32(200, 150, 255, 255),
                                  "Blocked inside beginFrame waiting on the in-flight fence and on\n"
                                  "vkAcquireNextImageKHR. High here means the GPU (or the compositor)\n"
                                  "is the limiter, not the CPU. Included in 'render (record)'." },
                                { "tail (input/profiler)", m_cpuTailMs, IM_COL32(190, 190, 190, 255),
                                  "Post-render bookkeeping: input state update and profiler frame commit." },
                                { "wait / vsync", waitMs, IM_COL32(110, 110, 130, 255),
                                  "Wall-clock time not attributed to any CPU phase: present/vsync block,\n"
                                  "driver overhead and GPU back-pressure. Large here with a low CPU total\n"
                                  "means the frame rate is capped by the display or the GPU.\n\n"
                                  "This is the row that closes the books: CPU phases + this = frame.\n"
                                  "If the phases look small but the frame is long, the time is HERE,\n"
                                  "and the next row (vkQueuePresentKHR) says how much of it is present." },
                                // Custo REAL medido de vkQueuePresentKHR (nao o residuo derivado
                                // acima) - sempre calculado (2 leituras de relogio, custo
                                // desprezivel). Achado 2026-09-02: sob Xvfb isto sozinho chegou a
                                // 38-41 ms/frame, FIXO independente de mapa/preset - se este numero
                                // estiver alto rodando por display real, e' driver/vsync de verdade;
                                // sob Xvfb/headless e' so' o present por software, ignorar pra FPS.
                                { "  vkQueuePresentKHR", m_vulkan.lastPresentMs(), IM_COL32(110, 110, 130, 255),
                                  "Medida DIRETA (nao residuo) do tempo dentro de vkQueuePresentKHR.\n"
                                  "Sob Xvfb/headless chega a 38-41 ms fixos (present por software) -\n"
                                  "nao e' custo de render. Rodando num display real, numero alto aqui\n"
                                  "= preso no vsync/driver de verdade." },
                            };

                            // Hovering a row highlights its slice in the pie
                            // below (and vice-versa the legend colour matches).
                            int cpuHover = -1;
                            int sliceIdx = 0;
                            for (const Row& r : rows) {
                                const bool nested = (r.label[0] == ' ');
                                const float frac = (frameWall > 0.001f) ? (r.ms / frameWall) : 0.0f;

                                // Colour swatch = this row's slice colour in the pie.
                                ImGui::Dummy(ImVec2(4.0f, 0.0f));
                                ImGui::SameLine();
                                ImVec2 sp = ImGui::GetCursorScreenPos();
                                const float sw = ImGui::GetTextLineHeight() * 0.72f;
                                if (!nested) {
                                    ImGui::GetWindowDrawList()->AddRectFilled(
                                        ImVec2(sp.x, sp.y + (ImGui::GetTextLineHeight() - sw) * 0.5f),
                                        ImVec2(sp.x + sw, sp.y + (ImGui::GetTextLineHeight() + sw) * 0.5f),
                                        r.color, 2.0f);
                                }
                                ImGui::Dummy(ImVec2(sw + 4.0f, ImGui::GetTextLineHeight()));
                                ImGui::SameLine();

                                ImVec4 col = frac > 0.45f ? ImVec4(1.0f, 0.35f, 0.30f, 1.0f)
                                           : frac > 0.20f ? ImVec4(1.0f, 0.80f, 0.30f, 1.0f)
                                                          : ImVec4(0.75f, 0.80f, 0.85f, 1.0f);
                                ImGui::TextColored(col, "%-22s %6.2f ms  %5.1f%%", r.label, r.ms, frac * 100.0f);
                                if (ImGui::IsItemHovered()) {
                                    if (!nested) cpuHover = sliceIdx;
                                    ImGui::SetTooltip("%s\n\n%s",
                                        r.help,
                                        nested ? "(nested inside 'render (record)' - not a separate pie slice)"
                                               : "Highlighted slice in the frame pie below.");
                                }
                                if (!nested) ++sliceIdx;
                            }

                            // ---- GPU passes, collapsible tree ---------------
                            // Author's rules (2026-09-03): keep EVERY pass listed
                            // even at 0.00 ms, expand everything that can be
                            // expanded, make it collapsible, and keep it in the
                            // main list instead of a separate section further down
                            // (being collapsible it stays out of the way). No
                            // separator above it either: it belongs to the same
                            // breakdown as the CPU rows.
                            //
                            // The table is STATIC, not built from the timing map: a
                            // pass that did not run this frame shows as zero instead
                            // of vanishing - rows that appear and disappear make the
                            // panel jump and hide exactly what was flickering.
                            // Anything registered with Profiler::addGpuTime but
                            // missing here falls into "Other", so nothing is hidden.
                            {
                                struct Pass { const char* name; const char* help; };
                                struct Group { const char* title; const char* help; ImU32 color; std::vector<Pass> passes; };
                                static const std::vector<Group> kGroups = {
                                    { "Geometry", "Scene rasterisation: what fills the G-buffer and the depth maps.",
                                      IM_COL32(120, 200, 255, 255), {
                                        { "GBuffer", "Terrain, models and sprites writing albedo/normal/PBR/material/emissive.\nAlmost entirely a PER-PIXEL cost: foliage uses alpha discard, which\ndisables early-Z, so every layer of leaves runs the whole shader." },
                                        { "Shadows", "Renders the cascades into the shadow atlas (data/shadows.json)." },
                                        { "Cloud Shadow Map", "Shadow map of the global cloud layer." },
                                        { "Rain TopDown Depth", "Top-down depth used to place rain splashes.\nRuns every 8 frames, so it alternates between 0 and its full cost." },
                                    }},
                                    { "Lighting", "Full-screen passes that consume the G-buffer.",
                                      IM_COL32(255, 190, 90, 255), {
                                        { "Deferred Lighting", "Sun, point lights and shadow lookup. Almost entirely a per-pixel cost." },
                                        { "Skybox IBL", "Sky background and image-based reflections." },
                                    }},
                                    { "Clouds & weather", "Volumetrics and world-space weather, before post-processing.",
                                      IM_COL32(150, 230, 150, 255), {
                                        { "Cloud Volumes (field)", "Ray-march of the field clouds. Inside it the density function walks up\nto 100 blobs PER STEP. Step cap comes from cloud_march_steps in the preset." },
                                        { "Cloud Shadows (field)", "Ground shadow cast by each field cloud." },
                                        { "Cloud Layers", "Flat cloud layers." },
                                        { "CloudFluff Render", "Cloud puffs (billboards)." },
                                        { "Cloud Debug Overlay", "Coverage diagnostic overlay (normally 0)." },
                                        { "Map Smoke", "Map smoke plumes (volcano). One ray-march per plume;\nsmoke_steps and smoke_detail in the preset drive the cost." },
                                        { "Water", "Water surface. Skipped when the water is outside the frustum or\nburied under the terrain." },
                                    }},
                                    { "Post-processing", "Everything after the scene is drawn, working on the whole image.\n'Post Total' is the timer wrapping the entire pass; the entries below are\nits parts and should add up to it.",
                                      IM_COL32(200, 150, 255, 255), {
                                        { "Post Total", "AGGREGATE: wraps the entire post-processing pass. Excluded from the GPU\ntotal (its children are counted instead) or it would count twice." },
                                        { "Rain Heightmap", "Height map used to know where rain lands." },
                                        { "Weather Overlay", "Rain, snow and dust drawn over the screen." },
                                        { "Lens Drops (compute, em Weather Overlay)", "NESTED inside Weather Overlay: droplets on the lens. Shown for\ndiagnosis but NOT summed (already inside the overlay)." },
                                        { "Weather Particles", "3D raindrops and snowflakes." },
                                        { "DoF / Tilt-Shift", "Depth of field. Until 2026-09-02 this timer measured the WHOLE\npost-processing pass, which is why it looked expensive in the rain." },
                                        { "Auto Exposure", "Brightness adaptation (luminance histogram)." },
                                        { "Bloom Down", "Bloom downsample pyramid." },
                                        { "Bloom Up", "Bloom upsample." },
                                        { "Composite", "Tonemap, colour grading, chromatic aberration, motion blur and vignette.\nCANNOT be split by timestamp: they are branches of the SAME shader.\nTo isolate each one: perf/test_shader_cost.py." },
                                        { "Post Unattributed", "Residual TIME inside post: Post Total minus the sum of the parts above.\nIf it grows, some stretch of post-processing runs without a timer.\nNot the same as the 'Unclassified' group below, which lists whole\nPASSES missing from this table." },
                                    }},
                                };
                                const auto& times = Profiler::getAllGpuTimes();
                                auto ms_of = [&](const char* n) -> float {
                                    auto it = times.find(n);
                                    return (it == times.end()) ? 0.0f : it->second;
                                };
                                auto isAggregate = [](const std::string& n) {
                                    return n == "Post Total" || n.rfind("Lens Drops", 0) == 0;
                                };
                                const float budget = 1000.0f / 120.0f; // 120 FPS budget
                                auto colorFor = [&](float ms) {
                                    return (ms > budget * 0.34f) ? ImVec4(1.0f, 0.5f, 0.5f, 1.0f)
                                         : (ms > budget * 0.12f) ? ImVec4(1.0f, 0.9f, 0.6f, 1.0f)
                                                                 : ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
                                };

                                std::vector<std::string> listed;
                                for (const Group& g : kGroups)
                                    for (const Pass& p : g.passes) listed.emplace_back(p.name);

                                int gpuHover = -1;
                                int groupIdx = 0;
                                std::vector<ImGuiEx::PieChartSlice> gpuSlices;
                                gpuSlices.reserve(kGroups.size() + 1);

                                for (const Group& g : kGroups) {
                                    float sum = 0.0f;
                                    for (const Pass& p : g.passes)
                                        if (!isAggregate(p.name)) sum += ms_of(p.name);
                                    gpuSlices.push_back({ g.title, std::max(sum, 0.0f), g.color });

                                    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
                                    const bool open = ImGui::TreeNodeEx(
                                        g.title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                        "%-22s %7.3f ms", g.title, sum);
                                    if (ImGui::IsItemHovered()) {
                                        gpuHover = groupIdx;
                                        ImGui::SetTooltip("%s\n\nHighlighted slice in the GPU pie below.", g.help);
                                    }
                                    if (open) {
                                        const bool isPost = std::string(g.title) == "Post-processing";
                                        for (const Pass& p : g.passes) {
                                            const float ms = ms_of(p.name);
                                            const float frac = (gpuSum > 0.001f) ? (ms / gpuSum) : 0.0f;
                                            const bool indent = isPost && std::string(p.name) != "Post Total";
                                            ImGui::TextColored(colorFor(ms), "  %s%-40s %7.3f ms  %5.1f%%",
                                                               indent ? "  " : "", p.name, ms, frac * 100.0f);
                                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.help);
                                        }
                                        if (isPost) {
                                            const float total = ms_of("Post Total");
                                            const float ratio = (total > 0.001f) ? (sum / total) : 1.0f;
                                            ImGui::TextDisabled("    %-40s %7.3f ms  (%.0f%% of Post Total)",
                                                                "sum of the parts", sum, ratio * 100.0f);
                                            if (total > 0.05f && (ratio < 0.9f || ratio > 1.1f))
                                                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                                    "    parts do not add up to the total - some stretch has no timer");
                                        }
                                        ImGui::TreePop();
                                    }
                                    ++groupIdx;
                                }

                                std::vector<std::pair<float, std::string>> other;
                                for (const auto& [n, ms] : times)
                                    if (std::find(listed.begin(), listed.end(), n) == listed.end())
                                        other.emplace_back(ms, n);
                                std::sort(other.rbegin(), other.rend());
                                float otherSum = 0.0f;
                                for (const auto& [ms, n] : other) if (!isAggregate(n)) otherSum += ms;
                                gpuSlices.push_back({ "Unclassified", std::max(otherSum, 0.0f), IM_COL32(190, 190, 190, 255) });
                                ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
                                if (ImGui::TreeNodeEx("Unclassified", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                      "%-22s %7.3f ms", "Unclassified", otherSum)) {
                                    if (ImGui::IsItemHovered()) {
                                        gpuHover = groupIdx;
                                        ImGui::SetTooltip("Whole PASSES that were registered with Profiler::addGpuTime but\nare missing from the table above - so nothing can hide. Empty is\nthe healthy state.\n\nDifferent from 'Post Unattributed', which is residual TIME inside\nthe post pass, not a pass of its own.");
                                    }
                                    if (other.empty()) ImGui::TextDisabled("    (none - every pass is classified)");
                                    for (const auto& [ms, n] : other)
                                        ImGui::TextColored(colorFor(ms), "  %-40s %7.3f ms", n.c_str(), ms);
                                    ImGui::TreePop();
                                }

                                // Same values as a pie, so the split reads at a glance.
                                ImGuiEx::PieChart("##gpuPassPie", 52.0f, gpuSlices, gpuHover);
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("Share of the GPU frame taken by each group of passes.\n"
                                                      "Hover a group above to highlight its slice.");
                                }
                            }

                            ImGui::Text("  CPU total %.2f ms | GPU total %.2f ms | frame %.2f ms (avg %.2f)",
                                        cpuSum, gpuSum, frameWall, m_frameTime);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "CPU total: sum of the phases above (excluding wait/vsync).\n"
                                    "GPU total: sum of the timestamped GPU passes, aggregates excluded.\n"
                                    "frame: THIS loop iteration, measured in the same frame as the\n"
                                    "phases - that is what the percentages divide by, so they can\n"
                                    "never exceed 100%%.\n"
                                    "avg: the windowed average used for the FPS readout. Mixing the\n"
                                    "two is what used to show 'render 110%%'.");
                            }
                            const bool cpuBound = cpuSum > gpuSum && cpuSum > frameWall * 0.5f;
                            ImGui::TextDisabled("  verdict: %s",
                                cpuBound ? "CPU-bound" : (waitMs > m_frameTime * 0.4f ? "vsync / present capped" : "GPU-bound"));
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "CPU-bound: the CPU phases dominate; optimise game/engine code.\n"
                                    "GPU-bound: the GPU passes dominate; optimise shaders or resolution.\n"
                                    "vsync / present capped: neither is saturated, the frame rate is\n"
                                    "limited by the display refresh or the presentation engine.");
                            }

                            // Pie of the same values, so the split is readable at a glance.
                            std::vector<ImGuiEx::PieChartSlice> cpuSlices;
                            cpuSlices.reserve(6);
                            for (const Row& r : rows) {
                                // Skip the nested beginFrame row: it is already
                                // counted inside "render (record)" and would make
                                // the pie sum to more than the frame.
                                if (r.label[0] == ' ') continue;
                                cpuSlices.push_back({ r.label, std::max(r.ms, 0.0f), r.color });
                            }
                            ImGuiEx::PieChart("##cpuFramePie", 52.0f, cpuSlices, cpuHover);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Share of the wall-clock frame taken by each CPU phase\n"
                                                  "plus the unattributed wait/vsync time.\n"
                                                  "Hover a row above to highlight its slice.");
                            }
                        }

                        // Fase detalhada do render record (15 fases nomeadas,
                        // 2026-09-01) - dobravel por default pra nao poluir a
                        // visao rapida. Reusa m_cpuPhaseMs, que so' e' atualizado
                        // quando o gate de cpuMark acima libera (telemetria,
                        // benchmark, OU o F3 aberto - forcado la' em cima).
                        if (ImGui::TreeNode("Render record - 15 fases")) {
                            float phaseSum = 0.0f;
                            for (size_t i = 1; i < m_cpuPhaseMs.size(); ++i) phaseSum += static_cast<float>(m_cpuPhaseMs[i]);
                            for (size_t i = 1; i < m_cpuPhaseMs.size(); ++i) {
                                const float ms = static_cast<float>(m_cpuPhaseMs[i]);
                                const float frac = phaseSum > 0.001f ? ms / phaseSum : 0.0f;
                                ImVec4 col = frac > 0.35f ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                                           : frac > 0.15f ? ImVec4(1.0f, 0.85f, 0.4f, 1.0f)
                                                          : ImVec4(0.8f, 0.8f, 0.85f, 1.0f);
                                ImGui::TextColored(col, "  %-20s %6.2f ms  %5.1f%%",
                                                   kCpuPhaseNames[i], ms, frac * 100.0f);
                            }
                            ImGui::TextDisabled("  soma das fases: %.2f ms (e' o que 'render (record)' mede acima)", phaseSum);
                            ImGui::TreePop();
                        }
                        ImGui::Separator();
                        float gpuVramGb = SystemMonitor::gpuVramBytes() / (1024.0f * 1024.0f * 1024.0f);
                float gpuVramTotalGb = SystemMonitor::gpuVramTotalBytes() / (1024.0f * 1024.0f * 1024.0f);
                float ramGb = SystemMonitor::processRamBytes() / (1024.0f * 1024.0f * 1024.0f);
                float ramTotalGb = SystemMonitor::totalRamBytes() / (1024.0f * 1024.0f * 1024.0f);
                ImGui::Text("GPU: %.1f%% core | %.1f%% VRAM (%.2f / %.2f GB)",
                    SystemMonitor::gpuCorePercent(), SystemMonitor::gpuVramPercent(), gpuVramGb, gpuVramTotalGb);
                ImGui::Text("CPU: %.1f%% core | RAM: %.1f%% (%.2f / %.2f GB)",
                    SystemMonitor::processCpuPercent(), SystemMonitor::processRamPercent(), ramGb, ramTotalGb);

                ImGui::Separator();

                if (ImGui::BeginTable("TelemetryTable", 4, ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableNextRow();
                    
                    // Column 1: GPU VRAM
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "GPU VRAM Usage");
                    std::vector<ImGuiEx::PieChartSlice> vramSlices;
                    size_t totalVram = 0;
                    for (int i = 0; i < static_cast<int>(ProfilerCategory::Count); ++i) {
                        auto cat = static_cast<ProfilerCategory>(i);
                        size_t bytes = Profiler::getVram(cat);
                        if (bytes > 0) {
                            vramSlices.push_back({Profiler::getCategoryName(cat), static_cast<float>(bytes), Profiler::getCategoryColor(cat)});
                            totalVram += bytes;
                        }
                    }
                    static int vramHover = -1;
                    ImGuiEx::PieChart("VramChart", 60.0f, vramSlices, vramHover);
                    ImGui::Dummy(ImVec2(0, 10));
                    ImGui::Text("Total: %.2f MB", totalVram / (1024.0f * 1024.0f));
                    bool vramAnyHovered = false;
                    for (int i = 0; i < static_cast<int>(vramSlices.size()); ++i) {
                        ImGui::PushStyleColor(ImGuiCol_Text, vramSlices[i].color);
                        ImGui::Text(" %s: %.2f MB", vramSlices[i].label.c_str(), vramSlices[i].value / (1024.0f * 1024.0f));
                        if (ImGui::IsItemHovered()) { vramHover = i; vramAnyHovered = true; }
                        ImGui::PopStyleColor();
                    }
                    if (!vramAnyHovered) vramHover = -1;

                    // Column 2: Process RAM (tracked only)
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Process RAM Usage");
                    std::vector<ImGuiEx::PieChartSlice> ramSlices;
                    size_t totalRam = 0;
                    for (int i = 0; i < static_cast<int>(ProfilerCategory::Count); ++i) {
                        auto cat = static_cast<ProfilerCategory>(i);
                        size_t bytes = Profiler::getRam(cat);
                        if (bytes > 0) {
                            ramSlices.push_back({Profiler::getCategoryName(cat), static_cast<float>(bytes), Profiler::getCategoryColor(cat)});
                            totalRam += bytes;
                        }
                    }
                    static int ramHover = -1;
                    ImGuiEx::PieChart("RamChart", 60.0f, ramSlices, ramHover);
                    ImGui::Dummy(ImVec2(0, 10));
                    ImGui::Text("Total Tracked: %.2f MB", totalRam / (1024.0f * 1024.0f));
                    bool ramAnyHovered = false;
                    for (int i = 0; i < static_cast<int>(ramSlices.size()); ++i) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ramSlices[i].color);
                        ImGui::Text(" %s: %.2f MB", ramSlices[i].label.c_str(), ramSlices[i].value / (1024.0f * 1024.0f));
                        if (ImGui::IsItemHovered()) { ramHover = i; ramAnyHovered = true; }
                        ImGui::PopStyleColor();
                    }
                    if (!ramAnyHovered) ramHover = -1;

                    // Column 3: CPU Time
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0, 0.5f, 1, 1), "CPU Frame Time");
                    std::vector<ImGuiEx::PieChartSlice> cpuSlices;
                    float totalCpu = 0.0f;
                    for (int i = 0; i < static_cast<int>(ProfilerCategory::Count); ++i) {
                        auto cat = static_cast<ProfilerCategory>(i);
                        float ms = Profiler::getCpuTime(cat);
                        if (ms > 0.001f) {
                            cpuSlices.push_back({Profiler::getCategoryName(cat), ms, Profiler::getCategoryColor(cat)});
                            totalCpu += ms;
                        }
                    }
                    static int cpuHover = -1;
                    ImGuiEx::PieChart("CpuChart", 60.0f, cpuSlices, cpuHover);
                    ImGui::Dummy(ImVec2(0, 10));
                    ImGui::Text("Total: %.2f ms", totalCpu);
                    bool cpuAnyHovered = false;
                    for (int i = 0; i < static_cast<int>(cpuSlices.size()); ++i) {
                        ImGui::PushStyleColor(ImGuiCol_Text, cpuSlices[i].color);
                        ImGui::Text(" %s: %.2f ms", cpuSlices[i].label.c_str(), cpuSlices[i].value);
                        if (ImGui::IsItemHovered()) { cpuHover = i; cpuAnyHovered = true; }
                        ImGui::PopStyleColor();
                    }
                    if (!cpuAnyHovered) cpuHover = -1;

                    // Column 4: GPU Time
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(1, 0, 0.5f, 1), "GPU Frame Time");
                    std::vector<ImGuiEx::PieChartSlice> gpuSlices;
                    float totalGpu = 0.0f;
                    // Fixed order: render effects that are enabled
                    for (auto& effect : m_renderEffects) {
                        if (effect->isEnabled()) {
                            float ms = Profiler::getGpuTime(effect->getName());
                            gpuSlices.push_back({effect->getName(), ms, Profiler::getEffectColor(effect->getName())});
                            totalGpu += ms;
                        }
                    }
                    static int gpuHover = -1;
                    ImGuiEx::PieChart("GpuChart", 60.0f, gpuSlices, gpuHover);
                    ImGui::Dummy(ImVec2(0, 10));
                    ImGui::Text("Total: %.2f ms", totalGpu);
                    bool gpuAnyHovered = false;
                    for (int i = 0; i < static_cast<int>(gpuSlices.size()); ++i) {
                        ImGui::PushStyleColor(ImGuiCol_Text, gpuSlices[i].color);
                        ImGui::Text(" %s: %.2f ms", gpuSlices[i].label.c_str(), gpuSlices[i].value);
                        if (ImGui::IsItemHovered()) { gpuHover = i; gpuAnyHovered = true; }
                        ImGui::PopStyleColor();
                    }
                    if (!gpuAnyHovered) gpuHover = -1;
                    
                    ImGui::EndTable();
                }
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Weather GPU Cost");
                drawWeatherGpuCost();
                const auto& wr = m_postProcessor.weatherRenderer();
                ImGui::Text("Pool: rain %u  snow %u  dust %u  splash %u  followers %zu",
                            wr.poolRainCount(), wr.poolSnowCount(), wr.poolDustCount(), wr.splashCount(),
                            wr.rainFollowerCount());

            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Scene Statistics");
                ImGui::Text("Instances: %zu", m_modelRenderer.getInstances().size());
                ImGui::Text("Unique Meshes: %zu", m_modelRenderer.getMeshes().size());

                // "Cache" hit/miss (PROXY em software, nao HW perf counter -
                // /proc/sys/kernel/perf_event_paranoid=4 bloqueia contador de
                // verdade sem root nesta maquina, medido 2026-09-01). Hit =
                // instancia que caiu no MESMO bucket (malha,LOD) do vizinho no
                // sort de instancing, ou seja, NAO paga bind/draw novo - a
                // mesma nocao de reuso que motivou a investigacao SoA/AoS.
                // Miss = bucket novo = 1 draw call. Reflete o ULTIMO passe que
                // chamou sortVisibleByMesh neste frame (cena, sombra ou
                // minimapa - normalmente a cena principal, por ser a ultima).
                {
                    uint32_t hits = m_modelRenderer.lastCacheHits();
                    uint32_t misses = m_modelRenderer.lastCacheMisses();
                    uint32_t total = hits + misses;
                    float rate = total > 0 ? 100.0f * float(hits) / float(total) : 0.0f;
                    ImVec4 rateCol = rate >= 70.0f ? ImVec4(0.4f, 1, 0.4f, 1)
                                    : rate >= 40.0f ? ImVec4(1, 0.85f, 0.3f, 1)
                                                     : ImVec4(1, 0.4f, 0.4f, 1);
                    ImGui::Text("Batch hits/misses: %u / %u", hits, misses);
                    ImGui::SameLine();
                    ImGui::TextColored(rateCol, "(%.0f%% hit)", rate);
                    if (ImGui::IsItemHovered()) {
                        // Nao chame isto de "cache": nunca foi cache de
                        // hardware. E' EFICIENCIA DE AGRUPAMENTO DE DRAW CALLS -
                        // quantas instancias vizinhas reusam o mesmo bind. O
                        // tooltip antigo dizia "proxy de cache" e ainda alegava
                        // que contador de HW nao estava disponivel; esta' - o
                        // perf_event_paranoid desta maquina e' -1 e a engine le'
                        // contador de verdade com ERUPTION_HW_COUNTERS=1.
                        ImGui::SetTooltip("Draw-call batching, not a hardware cache.\n"
                                          "hit  = neighbouring instance reuses the same\n"
                                          "       (mesh, LOD) bucket - no new bind.\n"
                                          "miss = new bucket, so one more bind + draw call.\n\n"
                                          "For REAL cache counters (IPC, L1d/LLC, MPKI) run\n"
                                          "with ERUPTION_HW_COUNTERS=1: the engine reads them\n"
                                          "through perf_event_open and writes them per frame\n"
                                          "into the telemetry 'hw' block.");
                    }
                }

                // Triangulos REALMENTE submetidos neste frame (pos-culling e
                // pos-LOD), nao o total carregado: e' o numero que responde
                // "a cena esta pesada de geometria AGORA?". Modelos e terreno
                // desenham no mesmo passe G-Buffer, entao aparecem somados e
                // discriminados. Vermelho acima de 2 M, amarelo acima de 1 M.
                const uint32_t triMod = m_modelRenderer.lastDrawnTriangles();
                const uint32_t triTer = m_terrainRenderer.lastDrawnTriangles();
                const uint32_t triTot = triMod + triTer;
                const ImVec4 triCol = (triTot > 2000000) ? ImVec4(1, 0.3f, 0.3f, 1)
                                    : (triTot > 1000000) ? ImVec4(1, 0.85f, 0.2f, 1)
                                                         : ImVec4(0.4f, 1, 0.4f, 1);
                ImGui::TextColored(triCol, "Triangles on screen: %s",
                                   formatThousands(triTot).c_str());
                ImGui::Text("   models %s  |  terrain %s  |  draws %u",
                            formatThousands(triMod).c_str(),
                            formatThousands(triTer).c_str(),
                            m_modelRenderer.lastDrawCalls());
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Texture Palette Usage (Histogram)");
                std::unordered_map<uint32_t, int> tUsage;
                for (const auto& mesh : m_modelRenderer.getMeshes()) tUsage[mesh.bindlessTexSlot]++;
                
                // Construct histogram data
                std::vector<float> histogramData;
                int maxCount = 0;
                for (auto const& [slot, count] : tUsage) {
                    if (slot >= histogramData.size()) {
                        histogramData.resize(slot + 1, 0.0f);
                    }
                    histogramData[slot] = static_cast<float>(count);
                    if (count > maxCount) maxCount = count;
                }
                
                ImGui::PlotHistogram("##PaletteUsage", histogramData.data(), static_cast<int>(histogramData.size()), 0, "Meshes per Bindless Slot", 0.0f, static_cast<float>(maxCount * 1.2f), ImVec2(0, 150));
                
                ImGui::Text("Active Slots: %zu (Max: %d meshes/slot)", tUsage.size(), maxCount);
                
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "Global Asset Cache");
                auto cacheStats = m_assetCache.getStats();
                ImGui::Text("Model Textures: %zu", cacheStats.modelTextureCount);
                ImGui::Text("Terrain Textures: %zu", cacheStats.terrainTextureCount);
                ImGui::Text("Meshes: %zu", cacheStats.meshCount);
                
            }
            }
        }
    } ImGui::End();
}

        if (m_showSpritePicker) {
            SpritePickerUI::draw(&m_showSpritePicker, m_charConfig, m_packManager);
        }

        if (m_showResourceManager) {
            ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(0.85f);
            if (ImGui::Begin("Resource Manager (F12)", &m_showResourceManager)) {
                if (ImGui::BeginTabBar("F12Tabs")) {
                    if (ImGui::BeginTabItem("Resources")) {
                        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Asset Sources (drag to reorder)");
                        ImGui::Separator();
                        auto& sources = const_cast<std::vector<ResourceEntry>&>(m_resourceConfig.entries);
                        for (int i = 0; i < (int)sources.size(); i++) {
                            ImGui::PushID(i);
                            ImGui::Button("::", ImVec2(30, 0));
                            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                ImGui::SetDragDropPayload("RESOURCE_SOURCE", &i, sizeof(int));
                                ImGui::Text("Moving: %s", sources[i].path.c_str());
                                ImGui::EndDragDropSource();
                            }
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_SOURCE")) {
                                    int srcIdx = *(const int*)payload->Data;
                                    if (srcIdx != i) {
                                        ResourceEntry moved = sources[srcIdx];
                                        sources.erase(sources.begin() + srcIdx);
                                        if (srcIdx < i) i--;
                                        sources.insert(sources.begin() + i, moved);
                                        m_resourceConfig.save("resources.ini");
                                        m_packManager.clear();
                                        for (const auto& entry : sources) { m_packManager.addDirectory(entry.path); }
                                        populateMaps();
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }
                            ImGui::SameLine();
                            const char* icon = sources[i].isDirectory ? "[DIR]" : "[pack]";
                            ImGui::Text("%s %s", icon, sources[i].path.c_str());
                            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 60);
                            if (ImGui::Button("Remove", ImVec2(60, 0))) {
                                sources.erase(sources.begin() + i);
                                m_resourceConfig.save("resources.ini");
                                m_packManager.clear();
                                for (const auto& entry : sources) { m_packManager.addDirectory(entry.path); }
                                populateMaps(); i--;
                            }
                            ImGui::PopID();
                        }
                        ImGui::Separator();
                        static char newPath[512] = ""; static bool newIsDir = false;
                        ImGui::InputText("Path", newPath, sizeof(newPath)); ImGui::Checkbox("Is Directory", &newIsDir); ImGui::SameLine();
                        if (ImGui::Button("Add")) {
                            if (newPath[0] != '\0') {
                                ResourceEntry entry; entry.path = newPath; entry.isDirectory = newIsDir;
                                sources.push_back(entry); m_resourceConfig.save("resources.ini");
                                m_packManager.addDirectory(entry.path);
                                populateMaps(); newPath[0] = '\0';
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::CollapsingHeader("Loaded Source Info")) {
                            for (const auto& src : m_packManager.getSources()) {
                                ImGui::Text("[DIR] %s (%u files)", src.path.c_str(), src.fileCount);
                            }
                        }
                        if (ImGui::CollapsingHeader("Statistics")) {
                            ImGui::Text("Total Directories: %zu", m_packManager.dirCount());
                            ImGui::Text("Total Sources: %zu", m_packManager.sourceCount());
                            auto allFiles = m_packManager.getAllFilenames();
                            ImGui::Text("Total Unique Files: %zu", allFiles.size());
                        }
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Gamepad")) {
                        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Gamepad Configuration (PS2 Style)");
                        ImGui::Separator();
                        int activeGpad = m_inputConfig.gamepad.selectedGamepadIndex;
                        if (ImGui::CollapsingHeader("Controller Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
                            std::vector<std::string> deviceNames; std::vector<int> deviceIndices; int currentSelectionIdx = -1;
                            for (int i = 0; i < 8; i++) {
                                if (glfwJoystickPresent(GLFW_JOYSTICK_1 + i)) {
                                    const char* name = glfwGetJoystickName(GLFW_JOYSTICK_1 + i);
                                    bool isGpad = glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + i);
                                    char buf[256]; sprintf(buf, "[%d] %s %s", i, name, isGpad ? "(Standard)" : "(Generic)");
                                    deviceNames.push_back(buf); deviceIndices.push_back(i);
                                    if (m_inputConfig.gamepad.selectedGamepadIndex == i) currentSelectionIdx = (int)deviceNames.size() - 1;
                                }
                            }
                            if (deviceNames.empty()) ImGui::TextColored(ImVec4(1, 0, 0, 1), "No controllers detected!");
                            else {
                                const char* preview = (currentSelectionIdx >= 0) ? deviceNames[currentSelectionIdx].c_str() : "Select Device...";
                                if (ImGui::BeginCombo("Active Controller", preview)) {
                                    for (int i = 0; i < (int)deviceNames.size(); i++) {
                                        bool isSelected = (currentSelectionIdx == i);
                                        if (ImGui::Selectable(deviceNames[i].c_str(), isSelected)) { m_inputConfig.gamepad.selectedGamepadIndex = deviceIndices[i]; m_inputConfig.save("data/input_config.json"); }
                                        if (isSelected) ImGui::SetItemDefaultFocus();
                                    }
                                    ImGui::EndCombo();
                                }
                            }
                        }
                        ImGui::Spacing();
                        if (!Input::isGamepadPresent(activeGpad)) ImGui::TextColored(ImVec4(1, 0, 0, 1), "Selected Gamepad NOT detected!");
                        ImGui::BeginChild("ControllerLayout", ImVec2(0, 200), true);
                        auto drawBtn = [&](const char* label, int btnIdx) {
                            bool pressed = Input::isGamepadButtonDown(btnIdx, activeGpad);
                            if (pressed) ImGui::TextColored(ImVec4(1, 1, 0, 1), "[%s]", label);
                            else ImGui::Text("[%s]", label);
                        };
                        ImGui::Columns(2, "GpadCols", false); ImGui::SetColumnWidth(0, 200);
                        drawBtn("L1", 4); ImGui::SameLine(); drawBtn("L2", 6); ImGui::Spacing();
                        drawBtn("  UP  ", 12); drawBtn("LFT", 14); ImGui::SameLine(); drawBtn("RGT", 15); drawBtn(" DOWN ", 13);
                        ImGui::NextColumn();
                        drawBtn("R1", 5); ImGui::SameLine(); drawBtn("R2", 7); ImGui::Spacing();
                        ImGui::Indent(30); drawBtn("TRI", 3); ImGui::Unindent(30);
                        drawBtn("SQ", 2); ImGui::SameLine(100); drawBtn("CIR", 1);
                        ImGui::Indent(35); drawBtn(" X ", 0); ImGui::Unindent(35);
                        ImGui::Columns(1); ImGui::Separator();
                        float lx = Input::getGamepadAxis(m_inputConfig.gamepad.cameraAxisX, activeGpad);
                        float ly = Input::getGamepadAxis(m_inputConfig.gamepad.cameraAxisY, activeGpad);
                        float rx = Input::getGamepadAxis(m_inputConfig.gamepad.movementAxisX, activeGpad);
                        float ry = Input::getGamepadAxis(m_inputConfig.gamepad.movementAxisY, activeGpad);
                        ImGui::Text(" L-ANALOG (CAM): [%.2f, %.2f]", lx, ly);
                        if (std::abs(lx) > 0.1f || std::abs(ly) > 0.1f) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0, 1, 0, 1), " <ACTIVE>"); }
                        ImGui::Text(" R-ANALOG (WALK): [%.2f, %.2f]", rx, ry);
                        if (std::abs(rx) > 0.1f || std::abs(ry) > 0.1f) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 1, 0, 1), " <ACTIVE>"); }
                        ImGui::EndChild();
                        ImGui::Separator(); ImGui::Text("Mapping & Sensitivity");
                        bool changed = false; const char* axisItems[] = {"Axis 0", "Axis 1", "Axis 2", "Axis 3", "Axis 4", "Axis 5", "Axis 6", "Axis 7"};
                        if (ImGui::CollapsingHeader("Axes Assignment", ImGuiTreeNodeFlags_DefaultOpen)) {
                            changed |= ImGui::Combo("Camera X (Pan)", &m_inputConfig.gamepad.cameraAxisX, axisItems, IM_ARRAYSIZE(axisItems));
                            changed |= ImGui::Combo("Camera Y (Tilt)", &m_inputConfig.gamepad.cameraAxisY, axisItems, IM_ARRAYSIZE(axisItems));
                            changed |= ImGui::Combo("Walk X (Side)", &m_inputConfig.gamepad.movementAxisX, axisItems, IM_ARRAYSIZE(axisItems));
                            changed |= ImGui::Combo("Walk Y (Forward)", &m_inputConfig.gamepad.movementAxisY, axisItems, IM_ARRAYSIZE(axisItems));
                        }
                        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                            changed |= ImGui::Checkbox("Invert Camera X", &m_inputConfig.gamepad.invertCameraX);
                            changed |= ImGui::Checkbox("Invert Camera Y", &m_inputConfig.gamepad.invertCameraY);
                            changed |= ImGui::SliderFloat("Deadzone", &m_inputConfig.gamepad.deadzone, 0.0f, 0.5f);
                            changed |= ImGui::SliderFloat("Camera Sensitivity", &m_inputConfig.gamepad.cameraSensitivity, 0.1f, 10.0f);
                            changed |= ImGui::SliderFloat("Movement Sensitivity", &m_inputConfig.gamepad.movementSensitivity, 0.1f, 5.0f);
                        }
                        if (changed) m_inputConfig.save("data/input_config.json");
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Keyboard & Mouse")) {
                        ImGui::TextColored(ImVec4(0, 1, 1, 1), "Keyboard & Mouse Configuration");
                        ImGui::Separator();
                        bool changed = false;
                        if (ImGui::CollapsingHeader("Movement (WASD)", ImGuiTreeNodeFlags_DefaultOpen)) {
                            changed |= ImGui::SliderFloat("Base Speed", &m_inputConfig.kbMouse.wasdSpeed, 50.0f, 1000.0f, "%.0f");
                            changed |= ImGui::SliderFloat("Sprint Multiplier (Shift)", &m_inputConfig.kbMouse.sprintMultiplier, 1.0f, 10.0f, "%.1fx");
                        }
                        if (ImGui::CollapsingHeader("Mouse Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                            changed |= ImGui::SliderFloat("Sensitivity", &m_inputConfig.kbMouse.mouseSensitivity, 0.001f, 0.05f, "%.4f");
                            changed |= ImGui::SliderFloat("Zoom Sensitivity", &m_inputConfig.kbMouse.scrollSensitivity, 0.01f, 0.5f, "%.2f");
                            changed |= ImGui::Checkbox("Invert Y Axis", &m_inputConfig.kbMouse.invertMouseY);
                        }
                        if (changed) m_inputConfig.save("data/input_config.json");
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            } ImGui::End();
        }

        // ROTULO DO VIDEO (ERUPTION_TEST_DEMO, acao label=). Faixa discreta no
        // rodape dizendo o que esta' acontecendo na cena - o video precisa
        // explicar sozinho qual clima esta' ligado e o que mudou (pedido do
        // autor 2026-09-06). Aparece so' enquanto o roteiro pediu; fora do
        // demo m_demoLabelUntil fica negativo e nada e' desenhado.
        if (!m_demoLabel.empty() && m_demoStartTime >= 0.0f &&
            (m_timer.elapsed() - m_demoStartTime) < m_demoLabelUntil) {
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            // Largura limitada a' faixa livre a' ESQUERDA do painel F3 (que
            // ocupa 570 px no modo video): sem isto o texto passava por baixo
            // do painel e ficava cortado no meio da palavra.
            const float maxW = std::max(320.0f, ds.x - 620.0f);
            ImGui::SetNextWindowPos(ImVec2(40.0f, ds.y - 56.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(maxW, 200.0f));
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 10.0f));
            if (ImGui::Begin("##demolabel", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
                ImGui::SetWindowFontScale(1.7f);
                ImGui::PushTextWrapPos(maxW - 36.0f);
                ImGui::TextUnformatted(m_demoLabel.c_str());
                ImGui::PopTextWrapPos();
                ImGui::SetWindowFontScale(1.0f);
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        // Control Panel: sempre no canto superior direito
        ImGui::SetNextWindowPos(m_demoHidePanels ? ImVec2(-10000.0f, -10000.0f) : ImVec2(ImGui::GetIO().DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
        if (ImGui::Begin("Control Panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_playerController) m_playerController->renderImGui();
            ImGui::Text("Cam Target: %.1f, %.1f, %.1f", m_camera.target().x, m_camera.target().y, m_camera.target().z);
            ImGui::Text("Yaw: %.1f, Pitch: %.1f", glm::degrees(m_camera.orbitYaw()), glm::degrees(m_camera.orbitPitch()));
            float zpOverlay = 1.0f - (m_camera.orbitDistance() - 10.0f) / (1000.0f - 10.0f);
            ImGui::Text("Zoom: %.1f%%", zpOverlay * 100.0f);
            ImGui::Text("Visible Chunks: %d", m_visibleChunks);
            ImGui::Text("Time Control");
            int curT = (int)(m_dayNightCycle.timeOfDay() * 1440.0f);
            char tStr[32]; sprintf(tStr, "%02d:%02d", curT / 60, curT % 60);
            ImGui::PushItemWidth(150);
            if (ImGui::SliderInt("##Time", &curT, 0, 1439, tStr)) { m_dayNightCycle.setTimeOfDay((float)curT / 1440.0f); m_dayNightCycle.setPaused(true); }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            const char* speedItems[] = {"0.5x", "1x", "2x", "4x"};
            float speedValues[] = {0.5f, 1.0f, 2.0f, 4.0f};
            int speedIdx = 1;
            float curScale = m_dayNightCycle.timeScale();
            for (int i = 0; i < 4; i++) { if (std::abs(curScale - speedValues[i]) < 0.01f) speedIdx = i; }
            ImGui::PushItemWidth(80);
            if (ImGui::Combo("##Speed", &speedIdx, speedItems, IM_ARRAYSIZE(speedItems))) { m_dayNightCycle.setTimeScale(speedValues[speedIdx]); }
            ImGui::PopItemWidth();
            bool paused = m_dayNightCycle.isPaused();
            if (ImGui::Checkbox("Pause Cycle", &paused)) m_dayNightCycle.setPaused(paused);
        }
        ImGui::End();

        // HUD da aplicacao (separada do Control Panel)
        if (m_playerController) m_playerController->renderApplicationHUD();

        // Minimap: faz parte da HUD da aplicacao, entao vai no BackgroundDrawList -
        // as janelas de sistema (F1..F12, Control Panel) ficam sempre por cima.
        // Marcadores de jogador/boss tambÃ©m ficam no ForegroundDrawList.
        // Posição/tamanho do frame são controlados pelo CSS (#minimap) quando disponíveis.
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;

        char mapBuf[256] = "";
        if (!m_demoHidePanels && m_currentMap && m_playerController) {
            int gx = (int)(m_playerController->pos().x / 5.0f);
            int gz = (int)(m_playerController->pos().z / 5.0f);
            sprintf(mapBuf, "%s [%d, %d]", m_currentMapName.c_str(), gx, gz);
        }

        float uiSize = 200.0f;
        ImVec2 startPos(displaySize.x - 10.0f - uiSize, displaySize.y - 10.0f - uiSize);
        if (!m_demoHidePanels && m_minimapBottomRight && m_playerController) {
            eruption::CSSRect cssRect = m_playerController->getMinimapRect(displaySize);
            if (cssRect.w > 0.0f && cssRect.h > 0.0f) {
                // CSS rect now defines the minimap image size/position directly.
                uiSize = std::max(50.0f, std::min(cssRect.w, cssRect.h));
                startPos = ImVec2(cssRect.x, cssRect.y);
            }
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();

        // Fantasy stone/bronze frame around minimap map only
        ImVec2 bgMin(startPos.x - 6, startPos.y - 6);
        ImVec2 bgMax(startPos.x + uiSize + 6, startPos.y + uiSize + 6);
        if (!m_demoHidePanels) dl->AddRectFilled(bgMin, bgMax, IM_COL32(20, 18, 16, 235), 4.0f);
        if (!m_demoHidePanels) dl->AddRect(bgMin, bgMax, IM_COL32(139, 90, 43, 255), 4.0f, 0, 3.0f);
        if (!m_demoHidePanels) dl->AddRect(ImVec2(bgMin.x + 2, bgMin.y + 2), ImVec2(bgMax.x - 2, bgMax.y - 2), IM_COL32(240, 230, 210, 50), 3.0f, 0, 1.0f);
        if (!m_demoHidePanels) dl->AddRect(ImVec2(startPos.x - 2, startPos.y - 2), ImVec2(startPos.x + uiSize + 2, startPos.y + uiSize + 2), IM_COL32(107, 143, 94, 255), 0.0f, 0, 2.0f);

        if (!m_demoHidePanels && m_minimapDescriptorSet != VK_NULL_HANDLE) {
            // Flip UVs both horizontally and vertically to match source map orientation
            dl->AddImage((ImTextureID)m_minimapDescriptorSet, startPos, ImVec2(startPos.x + uiSize, startPos.y + uiSize), ImVec2(1, 1), ImVec2(0, 0));
        }

        // Minimap info panel: nome do mapa + coordenadas + horario (widget separado #minimap-info)
        if (!m_demoHidePanels && m_minimapBottomRight && m_playerController) {
            eruption::CSSRect infoRect = m_playerController->getMinimapInfoRect(displaySize);
            if (infoRect.w > 0.0f && infoRect.h > 0.0f) {
                // Horario do jogo (sem segundos)
                int curT = (int)(m_dayNightCycle.timeOfDay() * 1440.0f);
                char timeBuf[16]; sprintf(timeBuf, "%02d:%02d", curT / 60, curT % 60);

                ImVec2 mapSize = ImGui::CalcTextSize(mapBuf);
                ImVec2 timeSize = ImGui::CalcTextSize(timeBuf);
                float boxW = std::max(mapSize.x, timeSize.x) + 12.0f;
                float boxH = mapSize.y + timeSize.y + 12.0f;

                // Centraliza a caixa de texto horizontalmente com base no minimapa real e verticalmente no retangulo CSS
                float textX = startPos.x + (uiSize - boxW) * 0.5f;
                float textY = infoRect.y + (infoRect.h - boxH) * 0.5f;

                ImVec2 p(textX, textY);
                dl->AddRectFilled(p, ImVec2(p.x + boxW, p.y + boxH), IM_COL32(16, 14, 12, 235), 3.0f);
                dl->AddRect(p, ImVec2(p.x + boxW, p.y + boxH), IM_COL32(139, 90, 43, 255), 3.0f, 0, 1.0f);
                // Centraliza cada linha horizontalmente dentro da caixa
                drawStrokeText(dl, ImVec2(p.x + (boxW - mapSize.x) * 0.5f, p.y + 4.0f), mapBuf, IM_COL32(255, 235, 150, 255));
                drawStrokeText(dl, ImVec2(p.x + (boxW - timeSize.x) * 0.5f, p.y + mapSize.y + 6.0f), timeBuf, IM_COL32(200, 230, 255, 255));
            }
        }

        // Marcadores do minimapa (jogador/boss) sÃ£o desenhados DEPOIS do mapa para ficarem acima dele
        if (m_playerController && !m_demoHidePanels) m_playerController->renderMinimapMarkers();

        // FPS control panel - canto superior esquerdo
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        if (ImGui::Begin("FPSPanel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground)) {
            ImDrawList* fpsDl = ImGui::GetWindowDrawList();
            char fpsBuf[32]; sprintf(fpsBuf, "FPS: %.1f", m_fps);
            ImVec2 fpsSize = ImGui::CalcTextSize(fpsBuf);
            ImVec2 fp = ImGui::GetCursorScreenPos();
            fpsDl->AddRectFilled(fp, ImVec2(fp.x + fpsSize.x + 12.0f, fp.y + fpsSize.y + 8.0f), IM_COL32(0, 0, 0, 180), 3.0f);
            fpsDl->AddRect(fp, ImVec2(fp.x + fpsSize.x + 12.0f, fp.y + fpsSize.y + 8.0f), IM_COL32(100, 255, 100, 255), 3.0f, 0, 1.0f);
            drawStrokeText(fpsDl, ImVec2(fp.x + 6.0f, fp.y + 4.0f), fpsBuf, IM_COL32(100, 255, 100, 255));
        }
        ImGui::End();

        if (m_debugModelPivots && m_currentMap) {
            const auto& insts = m_modelRenderer.getInstances();
            const auto& frustum = m_camera.frustum();
            Vec3 camPos = m_camera.position();
            Mat4 vp = m_camera.viewProjNoJitter();
            float proximityRadius = 5000.0f; // Capture everything

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImGuiIO& io = ImGui::GetIO();

            struct DebugEntry {
                size_t index;
                float dist;
                float offset;
                float groundY;
                Vec3 pivotPos;
                std::string name;
                Vec2 screenPos;
                bool visible;
            };
            static std::vector<DebugEntry> visibleEntries;
            visibleEntries.clear();

            for (size_t i = 0; i < insts.size(); ++i) {
                const auto& inst = insts[i];
                if (!inst.enabled) continue;
                if (!frustum.intersectsAABB(inst.worldAabbMin, inst.worldAabbMax)) continue;

                float dist = glm::distance(camPos, inst.worldCenter);
                if (dist > proximityRadius) continue;

                Vec3 pivotPos = Vec3(inst.transform[3]);
                float groundY = TerrainParser::getTerrainHeightAt(m_currentMap->terrain, pivotPos.x, pivotPos.z);
                
                DebugEntry entry;
                entry.index = i;
                entry.dist = dist;
                entry.pivotPos = pivotPos;
                entry.groundY = groundY;
                entry.offset = pivotPos.y - groundY;
                entry.name = inst.name;
                
                // Project for in-world ID balloon
                Vec3 anchorPos = Vec3(inst.worldCenter.x, inst.worldAabbMax.y + 1.0f, inst.worldCenter.z);
                Vec4 clipPos = vp * Vec4(anchorPos, 1.0f);
                entry.visible = (clipPos.w > 0.0f);
                if (entry.visible) {
                    Vec2 ndc = Vec2(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
                    entry.screenPos = Vec2((ndc.x * 0.5f + 0.5f) * io.DisplaySize.x, (ndc.y * 0.5f + 0.5f) * io.DisplaySize.y);
                }

                visibleEntries.push_back(entry);
            }

            // Left Sidebar
            ImGui::SetNextWindowPos(ImVec2(10, 100), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(250, 400), ImGuiCond_FirstUseEver);
            int hoveredIdx = -1;
            if (ImGui::Begin("Object Debug List", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Visible Objects: %zu", visibleEntries.size());
                ImGui::Separator();
                if (ImGui::BeginChild("ScrollList")) {
                    for (const auto& entry : visibleEntries) {
                        char label[256];
                        sprintf(label, "[ID: %zu] %s", entry.index, entry.name.c_str());
                        
                        ImGui::PushID((int)entry.index);
                        if (ImGui::Selectable(label, (int)entry.index == m_modelRenderer.getHighlightedInstance())) {
                            m_camera.setOrbitTarget(entry.pivotPos);
                        }
                        if (ImGui::IsItemHovered()) {
                            hoveredIdx = (int)entry.index;
                        }
                        
                        if (ImGui::IsItemVisible()) {
                            ImGui::Text("  Off: %.2f | Dist: %.1fm", entry.offset, entry.dist / 10.0f);
                            ImGui::Text("  GroundY: %.3f (Diff: %.3f)", entry.groundY, entry.offset);
                            ImGui::Text("  Pos: %.1f, %.1f, %.1f", entry.pivotPos.x, entry.pivotPos.y, entry.pivotPos.z);
                            ImGui::Separator();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            }
            ImGui::End();

            m_modelRenderer.setHighlightedInstance(hoveredIdx);

            // Draw simplified in-world ID balloons
            for (const auto& entry : visibleEntries) {
                if (!entry.visible) continue;

                char idText[32]; sprintf(idText, "%zu", entry.index);
                ImVec2 textSize = ImGui::CalcTextSize(idText);
                float pad = 2.0f;
                
                bool isHighlighted = ((int)entry.index == hoveredIdx);
                ImU32 bgCol = isHighlighted ? IM_COL32(200, 0, 0, 230) : IM_COL32(0, 0, 0, 150);
                ImU32 textCol = isHighlighted ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 0, 220);

                ImVec2 boxMin = ImVec2(entry.screenPos.x - textSize.x * 0.5f - pad, entry.screenPos.y - textSize.y - pad);
                ImVec2 boxMax = ImVec2(entry.screenPos.x + textSize.x * 0.5f + pad, entry.screenPos.y + pad);

                dl->AddRectFilled(boxMin, boxMax, bgCol, 2.0f);
                dl->AddText(ImVec2(entry.screenPos.x - textSize.x * 0.5f, boxMin.y + pad), textCol, idText);
            }
        }
    }
}

} // namespace eruption
