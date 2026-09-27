#pragma once

#include <cstdint>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>

namespace eruption {

enum class ProfilerCategory {
    // GPU / RAM
    Textures = 0,
    Meshes,
    ShadowMaps,
    GBuffer,
    PostProcessing,
    Audio,
    FileCache,
    
    // CPU
    Culling,
    Animation,
    RenderDispatch,
    LoadingIO,
    
    Count
};

class Profiler {
public:
    static void addVram(ProfilerCategory category, size_t bytes);
    static void freeVram(ProfilerCategory category, size_t bytes);
    static size_t getVram(ProfilerCategory category);

    static void addRam(ProfilerCategory category, size_t bytes);
    static void freeRam(ProfilerCategory category, size_t bytes);
    static size_t getRam(ProfilerCategory category);

    static void addCpuTime(ProfilerCategory category, float ms);
    static float getCpuTime(ProfilerCategory category);
    static void resetCpuTime();

    static void addGpuTime(ProfilerCategory category, float ms);
    static float getGpuTime(ProfilerCategory category);
    static void resetGpuTime();

    static void addGpuTime(const std::string& name, float ms);
    static float getGpuTime(const std::string& name);
    static const std::unordered_map<std::string, float>& getAllGpuTimes();
    static void resetGpuTimeByName();

    static void setSmoothFrames(int frames);
    static int getSmoothFrames();
    static void commitFrame(); // call once per frame to push current values into rolling average

    static const char* getCategoryName(ProfilerCategory category);
    static uint32_t getCategoryColor(ProfilerCategory category);
    static uint32_t getEffectColor(const std::string& name);

private:
    static std::atomic<size_t> s_gpuVramBytes[static_cast<int>(ProfilerCategory::Count)];
    static std::atomic<size_t> s_sysRamBytes[static_cast<int>(ProfilerCategory::Count)];
    static std::atomic<float> s_cpuTimeMs[static_cast<int>(ProfilerCategory::Count)];
    static float s_gpuTimeMs[static_cast<int>(ProfilerCategory::Count)];

    static std::unordered_map<std::string, float> s_gpuTimeByName;

    // Rolling history for CPU/GPU times
    static int s_smoothFrames;
    static std::vector<std::deque<float>> s_cpuHistory;
    static std::vector<std::deque<float>> s_gpuHistory;
    static std::unordered_map<std::string, std::deque<float>> s_gpuNameHistory;

    static float average(const std::deque<float>& hist);
};

class CpuTimerScope {
public:
    CpuTimerScope(ProfilerCategory category);
    ~CpuTimerScope();

private:
    ProfilerCategory m_category;
    std::chrono::high_resolution_clock::time_point m_start;
};

class RamScope {
public:
    RamScope(ProfilerCategory category, size_t bytes);
    ~RamScope();

private:
    ProfilerCategory m_category;
    size_t m_bytes;
};

#define PROFILE_VRAM_ALLOC(category, bytes) eruption::Profiler::addVram(category, bytes)
#define PROFILE_VRAM_FREE(category, bytes)  eruption::Profiler::freeVram(category, bytes)
#define PROFILE_RAM_ALLOC(category, bytes)  eruption::Profiler::addRam(category, bytes)
#define PROFILE_RAM_FREE(category, bytes)   eruption::Profiler::freeRam(category, bytes)
#define PROFILE_CPU_SCOPE(category)         eruption::CpuTimerScope _timer_##__LINE__(category)
#define PROFILE_RAM_SCOPE(category, bytes)  eruption::RamScope _ramscope_##__LINE__(category, bytes)

} // namespace eruption
