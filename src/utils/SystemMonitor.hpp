#pragma once

#include <cstddef>
#include <cstdint>

namespace eruption {

class SystemMonitor {
public:
    static void update();

    static float processCpuPercent();
    static float processRamPercent();
    static size_t processRamBytes();
    static size_t totalRamBytes();

    // Detalhe da memoria, lido junto com o resto (1 Hz): onde o RSS do processo
    // esta (anonimo = heap, arquivo = mapeado, shmem) e quanto foi para o swap,
    // quanto o sistema ainda tem livre e a pressao de memoria (PSI, janela de
    // 10 s: fracao do tempo em que alguma tarefa esperou por memoria). Zeros
    // onde o arquivo nao existe; psiSomeAvg10 < 0 = sem PSI.
    struct MemoryDetail {
        size_t rssAnonBytes = 0, rssFileBytes = 0, rssShmemBytes = 0, swapBytes = 0;
        size_t systemAvailableBytes = 0, systemSwapFreeBytes = 0;
        float psiSomeAvg10 = -1.0f, psiFullAvg10 = -1.0f;
    };
    static const MemoryDetail& memoryDetail();

    static float gpuCorePercent();
    static float gpuVramPercent();
    static size_t gpuVramBytes();
    static size_t gpuVramTotalBytes();

private:
    struct Sample {
        uint64_t processUtime = 0;
        uint64_t processStime = 0;
        uint64_t totalCpu = 0;
        uint64_t idleCpu = 0;
    };

    static Sample s_prev;
    static float s_processCpu;
    static float s_processRam;
    static size_t s_processRamBytes;
    static size_t s_totalRamBytes;
    static MemoryDetail s_memoryDetail;

    static float s_gpuCore;
    static float s_gpuVram;
    static size_t s_gpuVramBytes;
    static size_t s_gpuVramTotal;

    static bool s_nvmlReady;
    static void* s_nvmlHandle;

    static void readProcStat(Sample& out);
    static void readProcSelfStat(Sample& out);
    static void readMemory();
    static void readGpuNvml();
    static void readGpuSmiFallback();
    static void readGpuSysfs();
};

} // namespace eruption
