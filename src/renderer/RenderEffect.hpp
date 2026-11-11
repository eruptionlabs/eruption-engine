#pragma once

#include "core/FxFilter.hpp"
#include <string>
#include <vector>
#include <imgui.h>

namespace eruption {

/**
 * @brief Base class for iterative render effects.
 * FUTURE IMPLEMENTATIONS: Inherit from this class and add to Engine's m_renderEffects list.
 */
class RenderEffect {
public:
    RenderEffect(const std::string& name, bool enabled = true) 
        : m_name(name), m_enabled(enabled) {}
    virtual ~RenderEffect() = default;

    const std::string& getName() const { return m_name; }
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }
    const std::string& tooltip() const { return m_tooltip; }
    void setTooltip(const std::string& tooltip) { m_tooltip = tooltip; }

    /**
     * @brief Draw effect-specific controls in the F2 menu.
     */
    virtual void drawUI() {
        ImGui::PushID(m_name.c_str());
        ::eruption::fx::describe(m_name.c_str(), m_tooltip.c_str());
        ::eruption::fx::Checkbox(m_name.c_str(), &m_enabled);
        if (::eruption::fx::IsItemHovered() && !m_tooltip.empty()) {
            ::eruption::fx::SetTooltip("%s", m_tooltip.c_str());
        }
        if (m_enabled || ::eruption::fx::active()) {
            for (auto& param : m_parameters) {
                ::eruption::fx::describe(param.name.c_str(), param.help.c_str());
                ::eruption::fx::SliderFloat(param.name.c_str(), &param.value, param.min, param.max);
                if (::eruption::fx::IsItemHovered() && !param.help.empty()) {
                    ::eruption::fx::SetTooltip("%s", param.help.c_str());
                }
            }
        }
        ImGui::PopID();
    }

    struct Parameter {
        std::string name;
        float value;
        float min;
        float max;
        std::string help;
    };

    float getParam(size_t index) const {
        return (index < m_parameters.size()) ? m_parameters[index].value : 0.0f;
    }

    void setParamHelp(size_t index, const std::string& help) {
        if (index < m_parameters.size()) m_parameters[index].help = help;
    }

protected:
    std::string m_name;
    bool m_enabled;
    std::string m_tooltip;
    std::vector<Parameter> m_parameters;
};

class GlobalIlluminationEffect : public RenderEffect {
public:
    GlobalIlluminationEffect(float initialIntensity)
        : RenderEffect("Global Illumination", true) {
        m_parameters.push_back({"GI Intensity", initialIntensity, 0.0f, 1.0f, ""});
        setTooltip("Global ambient light intensity. 0 = disabled, 1 = full map ambient.");
        setParamHelp(0, "Scales the hemispheric ambient light contribution.");
    }
};

} // namespace eruption
