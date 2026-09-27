#include "utils/Profiler.hpp"
#include <cstring>

namespace eruption {

std::atomic<size_t> Profiler::s_gpuVramBytes[static_cast<int>(ProfilerCategory::Count)] = {};
std::atomic<size_t> Profiler::s_sysRamBytes[static_cast<int>(ProfilerCategory::Count)] = {};
std::atomic<float> Profiler::s_cpuTimeMs[static_cast<int>(ProfilerCategory::Count)] = {};
float Profiler::s_gpuTimeMs[static_cast<int>(ProfilerCategory::Count)] = {};
std::unordered_map<std::string, float> Profiler::s_gpuTimeByName;
int Profiler::s_smoothFrames = 10;
std::vector<std::deque<float>> Profiler::s_cpuHistory(static_cast<int>(ProfilerCategory::Count));
std::vector<std::deque<float>> Profiler::s_gpuHistory(static_cast<int>(ProfilerCategory::Count));
std::unordered_map<std::string, std::deque<float>> Profiler::s_gpuNameHistory;

void Profiler::addVram(ProfilerCategory category, size_t bytes) {
    s_gpuVramBytes[static_cast<int>(category)] += bytes;
}

void Profiler::freeVram(ProfilerCategory category, size_t bytes) {
    // size_t: um FREE sem ALLOC correspondente dava underflow e o contador
    // virava lixo de 2^64. Aconteceu de verdade - Meshes e Textures tinham 13
    // FREEs e nenhum ALLOC (pedidos.md #5). Grampeia em zero.
    auto& v = s_gpuVramBytes[static_cast<int>(category)];   // std::atomic<size_t>
    size_t cur = v.load();
    while (!v.compare_exchange_weak(cur, (bytes >= cur) ? 0 : cur - bytes)) {}
}

size_t Profiler::getVram(ProfilerCategory category) {
    return s_gpuVramBytes[static_cast<int>(category)].load();
}

void Profiler::addRam(ProfilerCategory category, size_t bytes) {
    s_sysRamBytes[static_cast<int>(category)] += bytes;
}

void Profiler::freeRam(ProfilerCategory category, size_t bytes) {
    s_sysRamBytes[static_cast<int>(category)] -= bytes;
}

size_t Profiler::getRam(ProfilerCategory category) {
    return s_sysRamBytes[static_cast<int>(category)].load();
}

void Profiler::addCpuTime(ProfilerCategory category, float ms) {
    // Use a lock-free fetch_add on the IEEE-754 bit pattern instead of a CAS loop.
    auto& atomicFloat = s_cpuTimeMs[static_cast<int>(category)];
    auto* atomicBits = reinterpret_cast<std::atomic<uint32_t>*>(&atomicFloat);
    uint32_t addBits;
    static_assert(sizeof(addBits) == sizeof(ms), "float and uint32_t size mismatch");
    std::memcpy(&addBits, &ms, sizeof(ms));
    atomicBits->fetch_add(addBits, std::memory_order_relaxed);
}

float Profiler::getCpuTime(ProfilerCategory category) {
    int idx = static_cast<int>(category);
    if (idx < 0 || idx >= static_cast<int>(ProfilerCategory::Count)) return 0.0f;
    if (s_smoothFrames <= 1) return s_cpuTimeMs[idx].load();
    if (s_cpuHistory[idx].empty()) return s_cpuTimeMs[idx].load();
    return average(s_cpuHistory[idx]);
}

void Profiler::resetCpuTime() {
    for (int i = 0; i < static_cast<int>(ProfilerCategory::Count); ++i) {
        s_cpuTimeMs[i].store(0.0f);
    }
}

void Profiler::addGpuTime(ProfilerCategory category, float ms) {
    int idx = static_cast<int>(category);
    if (idx >= 0 && idx < static_cast<int>(ProfilerCategory::Count))
        s_gpuTimeMs[idx] += ms;
}

float Profiler::getGpuTime(ProfilerCategory category) {
    int idx = static_cast<int>(category);
    if (idx < 0 || idx >= static_cast<int>(ProfilerCategory::Count)) return 0.0f;
    if (s_smoothFrames <= 1) return s_gpuTimeMs[idx];
    if (s_gpuHistory[idx].empty()) return s_gpuTimeMs[idx];
    return average(s_gpuHistory[idx]);
}

void Profiler::resetGpuTime() {
    for (int i = 0; i < static_cast<int>(ProfilerCategory::Count); ++i) {
        s_gpuTimeMs[i] = 0.0f;
    }
}

void Profiler::addGpuTime(const std::string& name, float ms) {
    s_gpuTimeByName[name] += ms;
}

float Profiler::getGpuTime(const std::string& name) {
    auto it = s_gpuTimeByName.find(name);
    if (it == s_gpuTimeByName.end()) return 0.0f;
    if (s_smoothFrames <= 1) return it->second;
    auto h = s_gpuNameHistory.find(name);
    if (h == s_gpuNameHistory.end() || h->second.empty()) return it->second;
    return average(h->second);
}

const std::unordered_map<std::string, float>& Profiler::getAllGpuTimes() {
    return s_gpuTimeByName;
}

void Profiler::resetGpuTimeByName() {
    s_gpuTimeByName.clear();
}

void Profiler::setSmoothFrames(int frames) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    s_smoothFrames = frames;
}

int Profiler::getSmoothFrames() {
    return s_smoothFrames;
}

float Profiler::average(const std::deque<float>& hist) {
    if (hist.empty()) return 0.0f;
    float sum = 0.0f;
    for (float v : hist) sum += v;
    return sum / static_cast<float>(hist.size());
}

void Profiler::commitFrame() {
    // CPU
    for (int i = 0; i < static_cast<int>(ProfilerCategory::Count); ++i) {
        float v = s_cpuTimeMs[i].load();
        s_cpuHistory[i].push_back(v);
        while (static_cast<int>(s_cpuHistory[i].size()) > s_smoothFrames)
            s_cpuHistory[i].pop_front();
    }
    // GPU category
    for (int i = 0; i < static_cast<int>(ProfilerCategory::Count); ++i) {
        float v = s_gpuTimeMs[i];
        s_gpuHistory[i].push_back(v);
        while (static_cast<int>(s_gpuHistory[i].size()) > s_smoothFrames)
            s_gpuHistory[i].pop_front();
    }
    // GPU by name
    for (auto& pair : s_gpuTimeByName) {
        auto& hist = s_gpuNameHistory[pair.first];
        hist.push_back(pair.second);
        while (static_cast<int>(hist.size()) > s_smoothFrames)
            hist.pop_front();
    }
}

const char* Profiler::getCategoryName(ProfilerCategory category) {
    switch (category) {
        case ProfilerCategory::Textures: return "Textures";
        case ProfilerCategory::Meshes: return "Meshes";
        case ProfilerCategory::ShadowMaps: return "Shadow Maps";
        case ProfilerCategory::GBuffer: return "G-Buffer";
        case ProfilerCategory::PostProcessing: return "Post-Processing";
        case ProfilerCategory::Audio: return "Audio";
        case ProfilerCategory::FileCache: return "File Cache";
        case ProfilerCategory::Culling: return "Culling";
        case ProfilerCategory::Animation: return "Animation";
        case ProfilerCategory::RenderDispatch: return "Render Dispatch";
        case ProfilerCategory::LoadingIO: return "Loading I/O";
        default: return "Unknown";
    }
}

uint32_t Profiler::getCategoryColor(ProfilerCategory category) {
    switch (category) {
        case ProfilerCategory::Textures: return 0x4CAF50FF; // Green
        case ProfilerCategory::Meshes: return 0x2196F3FF; // Blue
        case ProfilerCategory::ShadowMaps: return 0x9C27B0FF; // Purple
        case ProfilerCategory::GBuffer: return 0xFFC107FF; // Amber
        case ProfilerCategory::PostProcessing: return 0xE91E63FF; // Pink
        case ProfilerCategory::Audio: return 0x00BCD4FF; // Cyan
        case ProfilerCategory::FileCache: return 0x607D8BFF; // Grey
        case ProfilerCategory::Culling: return 0xCDDC39FF; // Lime
        case ProfilerCategory::Animation: return 0xFF9800FF; // Orange
        case ProfilerCategory::RenderDispatch: return 0xF44336FF; // Red
        case ProfilerCategory::LoadingIO: return 0x795548FF; // Brown
        default: return 0xFFFFFFFF;
    }
}

uint32_t Profiler::getEffectColor(const std::string& name) {
    // Deterministic color hashing for effect names
    uint32_t hash = 0;
    for (char c : name) {
        hash = hash * 31 + static_cast<unsigned char>(c);
    }
    // Generate ARGB color with full alpha
    uint8_t r = static_cast<uint8_t>((hash >> 0) & 0xFF);
    uint8_t g = static_cast<uint8_t>((hash >> 8) & 0xFF);
    uint8_t b = static_cast<uint8_t>((hash >> 16) & 0xFF);
    // Boost saturation slightly by pushing channels away from gray
    uint8_t maxC = std::max(r, std::max(g, b));
    if (maxC < 128) {
        r = std::min(255, r + 64);
        g = std::min(255, g + 64);
        b = std::min(255, b + 64);
    }
    return (0xFF000000u) | (b << 16) | (g << 8) | r;
}

CpuTimerScope::CpuTimerScope(ProfilerCategory category) : m_category(category) {
    m_start = std::chrono::high_resolution_clock::now();
}

CpuTimerScope::~CpuTimerScope() {
    auto end = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(end - m_start).count();
    Profiler::addCpuTime(m_category, ms);
}

RamScope::RamScope(ProfilerCategory category, size_t bytes) : m_category(category), m_bytes(bytes) {
    Profiler::addRam(m_category, m_bytes);
}

RamScope::~RamScope() {
    Profiler::freeRam(m_category, m_bytes);
}

} // namespace eruption
