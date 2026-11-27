#pragma once
// Marcas de tempo do arranque (Engine::init e os init dos renderers). So' em ERUPTION_DEV; no build final
// bootMark() e' uma funcao vazia e some. Serve para achar onde o arranque gasta tempo no celular: cada marca
// loga o tempo desde a anterior, o total e quantas pipelines graficas nasceram (e quanto o driver gastou nelas).
#include "core/Logger.hpp"
#if ERUPTION_DEV
#include "renderer/VkCompat.hpp"
#include "utils/VkTrace.hpp"
#include <string>
#include <chrono>
#include <cstdio>
#include <ctime>
#endif

namespace eruption {

#if ERUPTION_DEV
inline void bootMark(const char* phase) {
    static const auto t0 = std::chrono::steady_clock::now();
    static auto last = t0;
    static uint32_t lastPipes = 0;
    static double lastPipeSec = 0.0;
    const auto now = std::chrono::steady_clock::now();
    uint32_t pipes = 0;
    double pipeSec = 0.0;
    vkc::pipelineStats(pipes, pipeSec);
    // O que as chamadas Vulkan, a leitura de .spv e o redesenho da tela de carga gastaram desde a marca anterior.
    static uint64_t lastCount[vktrace::kOpCount] = {};
    static int64_t lastNs[vktrace::kOpCount] = {};
    uint64_t count[vktrace::kOpCount];
    int64_t ns[vktrace::kOpCount];
    vktrace::snapshot(count, ns);
    std::string detail;
    for (int i = 0; i < vktrace::kOpCount; ++i) {
        if (count[i] == lastCount[i]) continue;
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %s %llux%.1f", vktrace::name(static_cast<vktrace::Op>(i)),
                      static_cast<unsigned long long>(count[i] - lastCount[i]), static_cast<double>(ns[i] - lastNs[i]) / 1e6);
        detail += buf;
        lastCount[i] = count[i];
        lastNs[i] = ns[i];
    }
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ERUPTION_LOG_WARN("[BOOT] %-26s +%7.1f ms (total %7.1f ms) | pipelines +%u em %.1f ms | t=%.1f |%s", phase,
                      std::chrono::duration<double, std::milli>(now - last).count(),
                      std::chrono::duration<double, std::milli>(now - t0).count(), pipes - lastPipes,
                      (pipeSec - lastPipeSec) * 1000.0, ts.tv_sec * 1e3 + ts.tv_nsec / 1e6, detail.c_str());
    last = now;
    lastPipes = pipes;
    lastPipeSec = pipeSec;
}
#else
inline void bootMark(const char*) {}
#endif

} // namespace eruption
