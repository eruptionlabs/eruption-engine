// Painel de telemetria do editor: FPS, gráfico de tempo de frame, divisão da
// CPU, custo por passe de GPU, memória de vídeo e números da cena.
#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"

#include "core/Engine.hpp"
#include "script/ScriptHost.hpp"
#include "utils/Profiler.hpp"

#include <imgui.h>

#include <algorithm>
#include <vector>

namespace eruption {

void Editor::recordFrameTime(float dt) {
    m_frameTimes[m_frameTimeHead] = dt * 1000.0f;
    m_frameTimeHead = (m_frameTimeHead + 1) % kFrameHistory;
}

void Editor::drawTelemetry() {
    if (!ImGui::Begin("Telemetry###Telemetry", &m_showTelemetry)) { ImGui::End(); return; }
    Engine& e = *m_engine;

    // Cabeçalho: FPS grande, tempo e hardware.
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::Text("%.0f FPS", e.fps());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%.2f ms per frame", e.frameTime());
    ImGui::TextDisabled("%s", m_hardware.c_str());
    ImGui::EndGroup();
    const VkExtent2D re = e.renderExtent(), de = e.displayExtent();
    ImGui::TextDisabled("Render %ux%u -> view %ux%u", re.width, re.height, de.width, de.height);

    // Tempo de frame (últimos frames), com as linhas de 60 e 120 FPS.
    std::vector<float> ordered(kFrameHistory);
    for (int i = 0; i < kFrameHistory; ++i) ordered[static_cast<size_t>(i)] = m_frameTimes[(m_frameTimeHead + i) % kFrameHistory];
    const float maxMs = std::max(20.0f, *std::max_element(ordered.begin(), ordered.end()) * 1.15f);
    const ImVec2 gpos = ImGui::GetCursorScreenPos();
    const ImVec2 gsize(ImGui::GetContentRegionAvail().x, 90.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, kAccent);
    ImGui::PlotLines("##frametime", ordered.data(), kFrameHistory, 0, nullptr, 0.0f, maxMs, gsize);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& [ms, label] : {std::pair<float, const char*>{16.67f, "60"}, {8.33f, "120"}}) {
        if (ms > maxMs) continue;
        const float y = gpos.y + gsize.y * (1.0f - ms / maxMs);
        dl->AddLine(ImVec2(gpos.x, y), ImVec2(gpos.x + gsize.x, y), IM_COL32(255, 255, 255, 50));
        dl->AddText(ImVec2(gpos.x + 4, y - 14), IM_COL32(255, 255, 255, 110), label);
    }

    auto bar = [](const char* label, float ms, float scale, ImU32 color) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(150);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = std::max(2.0f, std::min(1.0f, ms / scale) * (ImGui::GetContentRegionAvail().x - 70.0f));
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + ImGui::GetTextLineHeight()), color, 2.0f);
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::Text("%.2f ms", ms);
    };

    if (ImGui::BeginTable("##cols", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::TextColored(kAccent, "CPU");
        const float cpuScale = std::max(1.0f, e.m_cpuFrameWallMs);
        bar("Simulation", e.m_cpuAdvanceMs, cpuScale, IM_COL32(120, 175, 245, 255));
        bar("Game logic", e.m_cpuAppUpdateMs, cpuScale, IM_COL32(110, 190, 90, 255));
        bar("Render (record)", e.m_cpuRenderMs, cpuScale, IM_COL32(230, 200, 120, 255));
        bar("Present + tail", e.m_cpuTailMs, cpuScale, IM_COL32(160, 160, 165, 255));
        if (m_scripts) bar("Scripts (Luau)", m_scripts->lastUpdateMs(), cpuScale, IM_COL32(255, 120, 90, 255));

        ImGui::Spacing();
        ImGui::TextColored(kAccent, "Memory and scene");
        if (e.m_vulkan.allocator()) {
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(e.m_vulkan.physicalDevice(), &mp);
            std::vector<VmaBudget> budgets(VK_MAX_MEMORY_HEAPS);
            vmaGetHeapBudgets(e.m_vulkan.allocator(), budgets.data());
            uint64_t used = 0, budget = 0;
            for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
                if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    used += budgets[h].statistics.allocationBytes;
                    budget += budgets[h].budget;
                }
            ImGui::Text("VRAM used by the engine: %.0f MB of %.0f MB", used / 1048576.0, budget / 1048576.0);
        }
        ImGui::Text("Models in the map: %zu", e.modelRenderer().getInstances().size());
        ImGui::Text("Point lights: %zu", e.m_deferredLighting.getPointLights().size());
        ImGui::Text("Visible terrain chunks: %d", e.visibleChunks());

        ImGui::TableNextColumn();
        ImGui::TextColored(kAccent, "GPU passes");
        std::vector<std::pair<std::string, float>> passes(Profiler::getAllGpuTimes().begin(), Profiler::getAllGpuTimes().end());
        float total = 0.0f;
        for (const auto& p : passes) if (p.first != "Post Total") total += p.second;
        std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        const float gpuScale = passes.empty() ? 1.0f : std::max(0.5f, passes.front().second);
        int shown = 0;
        for (const auto& [name, ms] : passes) {
            if (ms < 0.005f) continue;
            bar(name.c_str(), ms, gpuScale, IM_COL32(255, 60, 40, 200));
            if (++shown >= 12) break;
        }
        if (passes.empty()) ImGui::TextDisabled("No GPU timings yet.");
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace eruption
