#pragma once

#include "math/Types.hpp"
#include <string>
#include <vector>
#include <chrono>

namespace eruption {

struct BenchmarkFrame {
    float fps = 0.0f;
    float gpuTimeWaterMs = 0.0f;
    float gpuTimeTotalMs = 0.0f;
    float timeOfDay = 0.0f;
    uint32_t triCount = 0;
    uint32_t planeCount = 0;
};

struct BenchmarkResult {
    std::string mapName;
    Vec3 cameraPos;
    float durationSec = 0.0f;
    float avgFps = 0.0f;
    float minFps = 9999.0f;
    float maxFps = 0.0f;
    float avgGpuWaterMs = 0.0f;
    float avgGpuTotalMs = 0.0f;
    float pctFramesBelow60 = 0.0f;
    std::vector<BenchmarkFrame> frames;
};

class WaterBenchmark {
public:
    void start(const std::string& mapName, const Vec3& cameraPos, float durationSec);
    void stop();
    bool isRunning() const { return m_running; }
    bool isWarmUp() const { return m_warmUp; }

    void recordFrame(float fps, float gpuWaterMs, float gpuTotalMs,
                     float timeOfDay, uint32_t triCount, uint32_t planeCount);

    const BenchmarkResult& result() const { return m_result; }

    void exportReport(const std::string& path) const;
    void drawUI(); // ImGui overlay during benchmark

    static std::string generateDefaultPath(const std::string& mapName);

private:
    bool m_running = false;
    bool m_warmUp = false;
    BenchmarkResult m_result;
    std::chrono::steady_clock::time_point m_startTime;
};

} // namespace eruption
