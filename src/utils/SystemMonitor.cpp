#include "utils/SystemMonitor.hpp"
#include "core/Logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __linux__
#include <dlfcn.h>
#endif

namespace eruption {

SystemMonitor::Sample SystemMonitor::s_prev = {};
float SystemMonitor::s_processCpu = 0.0f;
float SystemMonitor::s_processRam = 0.0f;
size_t SystemMonitor::s_processRamBytes = 0;
size_t SystemMonitor::s_totalRamBytes = 0;
float SystemMonitor::s_gpuCore = 0.0f;
float SystemMonitor::s_gpuVram = 0.0f;
size_t SystemMonitor::s_gpuVramBytes = 0;
size_t SystemMonitor::s_gpuVramTotal = 0;
bool SystemMonitor::s_nvmlReady = false;
void* SystemMonitor::s_nvmlHandle = nullptr;

#ifdef __linux__

void SystemMonitor::readProcSelfStat(Sample& out) {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return;
    unsigned long utime = 0, stime = 0;
    // Format: pid comm state ppid ... utime stime ...
    // We scan all fields up to utime/stime
    char buf[1024];
    if (fgets(buf, sizeof(buf), f)) {
        // Find the last ')' of the comm field to skip it safely
        char* p = strrchr(buf, ')');
        if (p) {
            sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %lu %lu",
                   &utime, &stime);
        }
    }
    fclose(f);
    out.processUtime = utime;
    out.processStime = stime;
}

void SystemMonitor::readProcStat(Sample& out) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        unsigned long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
        sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &system, &idle, &iowait, &irq, &softirq);
        out.totalCpu = user + nice + system + idle + iowait + irq + softirq;
        out.idleCpu = idle + iowait;
    }
    fclose(f);
}

void SystemMonitor::readMemory() {
    FILE* f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                long kb = 0;
                sscanf(line, "VmRSS: %ld", &kb);
                s_processRamBytes = static_cast<size_t>(kb) * 1024;
            }
        }
        fclose(f);
    }

    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                long kb = 0;
                sscanf(line, "MemTotal: %ld", &kb);
                s_totalRamBytes = static_cast<size_t>(kb) * 1024;
                break;
            }
        }
        fclose(f);
    }

    if (s_totalRamBytes > 0) {
        s_processRam = 100.0f * static_cast<float>(s_processRamBytes) / static_cast<float>(s_totalRamBytes);
    }
}

typedef int (*nvmlInit_v2_t)(void);
typedef int (*nvmlShutdown_t)(void);
typedef int (*nvmlDeviceGetHandleByIndex_v2_t)(unsigned int, void**);
typedef int (*nvmlDeviceGetUtilizationRates_t)(void*, void*);
typedef int (*nvmlDeviceGetMemoryInfo_t)(void*, void*);

struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
struct nvmlMemory_t { size_t total; size_t free; size_t used; };

void SystemMonitor::readGpuNvml() {
    if (!s_nvmlHandle) {
        s_nvmlHandle = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
        if (!s_nvmlHandle) {
            s_nvmlHandle = dlopen("libnvidia-ml.so", RTLD_LAZY);
        }
        if (s_nvmlHandle) {
            auto init = (nvmlInit_v2_t)dlsym(s_nvmlHandle, "nvmlInit_v2");
            if (init && init() == 0) {
                s_nvmlReady = true;
            } else {
                dlclose(s_nvmlHandle);
                s_nvmlHandle = nullptr;
            }
        }
    }

    if (!s_nvmlReady || !s_nvmlHandle) {
        readGpuSmiFallback();
        return;
    }

    auto getHandle = (nvmlDeviceGetHandleByIndex_v2_t)dlsym(s_nvmlHandle, "nvmlDeviceGetHandleByIndex_v2");
    auto getUtil = (nvmlDeviceGetUtilizationRates_t)dlsym(s_nvmlHandle, "nvmlDeviceGetUtilizationRates");
    auto getMem = (nvmlDeviceGetMemoryInfo_t)dlsym(s_nvmlHandle, "nvmlDeviceGetMemoryInfo");

    if (!getHandle || !getUtil || !getMem) {
        readGpuSmiFallback();
        return;
    }

    void* device = nullptr;
    if (getHandle(0, &device) != 0) {
        readGpuSmiFallback();
        return;
    }

    nvmlUtilization_t util{};
    nvmlMemory_t mem{};
    if (getUtil(device, &util) == 0) {
        s_gpuCore = static_cast<float>(util.gpu);
    }
    if (getMem(device, &mem) == 0) {
        s_gpuVramBytes = mem.used;
        s_gpuVramTotal = mem.total;
        if (mem.total > 0) {
            s_gpuVram = 100.0f * static_cast<float>(mem.used) / static_cast<float>(mem.total);
        }
    }
}

void SystemMonitor::readGpuSmiFallback() {
    FILE* pipe = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!pipe) return;
    int gpu = 0;
    unsigned long long used = 0, total = 0;
    if (fscanf(pipe, "%d, %llu, %llu", &gpu, &used, &total) == 3) {
        s_gpuCore = static_cast<float>(gpu);
        s_gpuVramBytes = used * 1024ULL * 1024ULL;
        s_gpuVramTotal = total * 1024ULL * 1024ULL;
        if (total > 0) {
            s_gpuVram = 100.0f * static_cast<float>(used) / static_cast<float>(total);
        }
    }
    pclose(pipe);
}

// Generic Linux GPU monitoring via /sys/class/drm (works for AMD, Intel and Mesa/Nouveau).
// NVIDIA proprietary users still get the NVML path above.
static bool readFileULong(const char* path, unsigned long long& out) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char buf[128]{};
    bool ok = fgets(buf, sizeof(buf), f) != nullptr;
    fclose(f);
    if (!ok) return false;
    char* end = nullptr;
    unsigned long long val = strtoull(buf, &end, 10);
    if (end == buf) return false;
    out = val;
    return true;
}

void SystemMonitor::readGpuSysfs() {
    // Try each DRM card and keep the first one that reports VRAM.
    for (int card = 0; card < 8; ++card) {
        char path[256]{};
        unsigned long long vramUsed = 0, vramTotal = 0, busy = 0;

        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/mem_info_vram_used", card);
        bool hasUsed = readFileULong(path, vramUsed);

        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/mem_info_vram_total", card);
        bool hasTotal = readFileULong(path, vramTotal);

        if (!hasUsed && !hasTotal) {
            // Intel/i915 uses a different path for memory usage.
            snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/pp_dpm_sclk", card);
            FILE* probe = fopen(path, "r");
            if (!probe) {
                // Not a GPU we can read from; skip.
                continue;
            }
            fclose(probe);
        }

        // GPU utilization: try AMD's gpu_busy_percent first.
        bool hasBusy = false;
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/gpu_busy_percent", card);
        hasBusy = readFileULong(path, busy);

        if (!hasBusy) {
            // Intel: try gt_cur_freq_mhz / gt_max_freq_mhz as a rough utilization proxy.
            unsigned long long curFreq = 0, maxFreq = 0;
            snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/gt_cur_freq_mhz", card);
            bool hasCur = readFileULong(path, curFreq);
            snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/gt_max_freq_mhz", card);
            bool hasMax = readFileULong(path, maxFreq);
            if (hasCur && hasMax && maxFreq > 0) {
                busy = (curFreq * 100ULL) / maxFreq;
                hasBusy = true;
            }
        }

        if (hasBusy) {
            s_gpuCore = static_cast<float>(busy);
        }
        if (hasUsed) {
            s_gpuVramBytes = vramUsed;
        }
        if (hasTotal) {
            s_gpuVramTotal = vramTotal;
        }
        if (s_gpuVramTotal > 0 && s_gpuVramBytes > 0) {
            s_gpuVram = 100.0f * static_cast<float>(s_gpuVramBytes) / static_cast<float>(s_gpuVramTotal);
        }

        // Stop at the first readable card.
        if (hasUsed || hasTotal || hasBusy) {
            return;
        }
    }
}

void SystemMonitor::update() {
    Sample cur{};
    readProcSelfStat(cur);
    readProcStat(cur);
    readMemory();
    readGpuNvml();
    if (s_gpuVramTotal == 0 && s_gpuCore == 0.0f) {
        readGpuSysfs();
    }

    if (s_prev.totalCpu > 0 && s_prev.processUtime > 0) {
        uint64_t totalDelta = cur.totalCpu - s_prev.totalCpu;
        uint64_t procDelta = (cur.processUtime - s_prev.processUtime) + (cur.processStime - s_prev.processStime);
        if (totalDelta > 0) {
            // utime/stime are in clock ticks; /proc/stat is also in same units
            s_processCpu = 100.0f * static_cast<float>(procDelta) / static_cast<float>(totalDelta);
        }
    }

    s_prev = cur;
}

float SystemMonitor::processCpuPercent() { return s_processCpu; }
float SystemMonitor::processRamPercent() { return s_processRam; }
size_t SystemMonitor::processRamBytes() { return s_processRamBytes; }
size_t SystemMonitor::totalRamBytes() { return s_totalRamBytes; }
float SystemMonitor::gpuCorePercent() { return s_gpuCore; }
float SystemMonitor::gpuVramPercent() { return s_gpuVram; }
size_t SystemMonitor::gpuVramBytes() { return s_gpuVramBytes; }
size_t SystemMonitor::gpuVramTotalBytes() { return s_gpuVramTotal; }

#else // Non-Linux fallback

void SystemMonitor::update() {}
float SystemMonitor::processCpuPercent() { return 0.0f; }
float SystemMonitor::processRamPercent() { return 0.0f; }
size_t SystemMonitor::processRamBytes() { return 0; }
size_t SystemMonitor::totalRamBytes() { return 0; }
float SystemMonitor::gpuCorePercent() { return 0.0f; }
float SystemMonitor::gpuVramPercent() { return 0.0f; }
size_t SystemMonitor::gpuVramBytes() { return 0; }
size_t SystemMonitor::gpuVramTotalBytes() { return 0; }

#endif

} // namespace eruption
