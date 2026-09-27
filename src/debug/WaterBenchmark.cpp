#include "debug/WaterBenchmark.hpp"
#include "core/Logger.hpp"
#include <imgui.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <numeric>

namespace eruption {

void WaterBenchmark::start(const std::string& mapName, const Vec3& cameraPos, float durationSec) {
    m_result = {};
    m_result.mapName = mapName;
    m_result.cameraPos = cameraPos;
    m_result.durationSec = durationSec;
    m_result.frames.reserve(static_cast<size_t>(durationSec * 60.0f));
    m_running = true;
    m_warmUp = true;
    m_startTime = std::chrono::steady_clock::now();
    ERUPTION_LOG_INFO("Water benchmark started: %s for %.0fs (2s warm-up)", mapName.c_str(), durationSec);
}

void WaterBenchmark::stop() {
    if (!m_running) return;
    m_running = false;

    if (m_result.frames.empty()) return;

    float sumFps = 0.0f;
    float sumGpuWater = 0.0f;
    float sumGpuTotal = 0.0f;
    int below60 = 0;

    for (const auto& f : m_result.frames) {
        sumFps += f.fps;
        sumGpuWater += f.gpuTimeWaterMs;
        sumGpuTotal += f.gpuTimeTotalMs;
        m_result.minFps = glm::min(m_result.minFps, f.fps);
        m_result.maxFps = glm::max(m_result.maxFps, f.fps);
        if (f.fps < 60.0f) below60++;
    }

    size_t n = m_result.frames.size();
    m_result.avgFps = sumFps / static_cast<float>(n);
    m_result.avgGpuWaterMs = sumGpuWater / static_cast<float>(n);
    m_result.avgGpuTotalMs = sumGpuTotal / static_cast<float>(n);
    m_result.pctFramesBelow60 = (static_cast<float>(below60) / static_cast<float>(n)) * 100.0f;

    ERUPTION_LOG_INFO("Benchmark complete: avg=%.1f fps, min=%.1f, max=%.1f, below60=%.1f%%",
        m_result.avgFps, m_result.minFps, m_result.maxFps, m_result.pctFramesBelow60);

    std::string path = generateDefaultPath(m_result.mapName);
    exportReport(path);
}

void WaterBenchmark::recordFrame(float fps, float gpuWaterMs, float gpuTotalMs,
                                  float timeOfDay, uint32_t triCount, uint32_t planeCount) {
    if (!m_running) return;

    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - m_startTime).count();
    if (elapsed >= m_result.durationSec + 2.0f) { // +2s warm-up
        stop();
        return;
    }

    if (elapsed < 2.0f) {
        m_warmUp = true;
        return; // Skip warm-up frames
    }
    m_warmUp = false;

    BenchmarkFrame f;
    f.fps = fps;
    f.gpuTimeWaterMs = gpuWaterMs;
    f.gpuTimeTotalMs = gpuTotalMs;
    f.timeOfDay = timeOfDay;
    f.triCount = triCount;
    f.planeCount = planeCount;
    m_result.frames.push_back(f);
}

void WaterBenchmark::exportReport(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        ERUPTION_LOG_WARN("Failed to write benchmark report: %s", path.c_str());
        return;
    }

    file << "# Water Benchmark Report\n";
    file << "# ======================\n\n";
    file << "Map: " << m_result.mapName << "\n";
    file << "Camera: " << std::fixed << std::setprecision(1)
         << m_result.cameraPos.x << ", "
         << m_result.cameraPos.y << ", "
         << m_result.cameraPos.z << "\n";
    file << "Duration: " << m_result.durationSec << "s\n";
    file << "Frames: " << m_result.frames.size() << "\n\n";

    file << "## Summary\n";
    file << "- Average FPS: " << std::setprecision(1) << m_result.avgFps << "\n";
    file << "- Min FPS: " << m_result.minFps << "\n";
    file << "- Max FPS: " << m_result.maxFps << "\n";
    file << "- % Frames < 60 FPS: " << std::setprecision(1) << m_result.pctFramesBelow60 << "%\n";
    file << "- Avg GPU Water (ms): " << std::setprecision(3) << m_result.avgGpuWaterMs << "\n";
    file << "- Avg GPU Total (ms): " << std::setprecision(3) << m_result.avgGpuTotalMs << "\n";
    file << "- PASS: " << (m_result.minFps >= 60.0f ? "YES" : "NO") << "\n\n";

    file << "## Per-Frame Data\n";
    file << "time_of_day,fps,gpu_water_ms,gpu_total_ms,tri_count,plane_count\n";
    for (const auto& f : m_result.frames) {
        file << std::setprecision(3)
             << f.timeOfDay << ","
             << std::setprecision(1) << f.fps << ","
             << std::setprecision(3) << f.gpuTimeWaterMs << ","
             << f.gpuTimeTotalMs << ","
             << f.triCount << ","
             << f.planeCount << "\n";
    }

    ERUPTION_LOG_INFO("Benchmark report saved to: %s", path.c_str());
}

void WaterBenchmark::drawUI() {
    if (!m_running) return;

    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - m_startTime).count();
    float progress = glm::clamp(elapsed / m_result.durationSec, 0.0f, 1.0f);

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, 30), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::Begin("Water Benchmark", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

    if (m_warmUp) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "WARM-UP...");
    } else {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "BENCHMARKING WATER");
    }
    ImGui::Text("Map: %s", m_result.mapName.c_str());
    ImGui::Text("Progress: %.1f / %.0fs", elapsed, m_result.durationSec);
    ImGui::ProgressBar(progress);
    if (!m_result.frames.empty()) {
        ImGui::Text("Current FPS: %.1f", m_result.frames.back().fps);
    }
    ImGui::End();
}

std::string WaterBenchmark::generateDefaultPath(const std::string& mapName) {
    std::string safeName = mapName;
    for (auto& c : safeName) {
        if (c == '/' || c == '\\' || c == ' ') c = '_';
    }
    return "logs/water_benchmark_" + safeName + ".csv";
}

} // namespace eruption
