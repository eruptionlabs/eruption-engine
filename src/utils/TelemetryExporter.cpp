#include "TelemetryExporter.hpp"

#include "utils/CacheStats.hpp"
#include "utils/Profiler.hpp"
#include "utils/SystemMonitor.hpp"
#include "core/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace eruption {

namespace {

using Clock = std::chrono::steady_clock;

struct State {
    bool initialized = false;
    bool enabled = false;
    bool finished = false;
    std::string outPath;
    std::ofstream out;
    int every = 1;
    int maxFrames = 20000;
    int recorded = 0;
    Clock::time_point start;
    Clock::time_point loadStart;
    bool loadActive = false;

    // Summary accumulators (frames only; load events are stored verbatim).
    double fpsSum = 0.0;
    float fpsMin = 0.0f;
    float maxFrameMs = 0.0f;
    int framesBelow60 = 0;
    int framesBelow120 = 0;
    size_t peakRamBytes = 0;
    size_t peakVramBytes = 0;
    std::vector<float> fpsValues;
    // Delta REAL por frame (nao suavizado) - base dos percentis de
    // frame time e da contagem de engasgo.
    std::vector<float> frameDtMs;
    float maxFrameDtMs = 0.0f;
    std::unordered_map<std::string, double> gpuSumMs;
    std::unordered_map<std::string, int> gpuCount;
    nlohmann::json loadEvents = nlohmann::json::array();

    std::mutex mtx;
};

State& state() {
    static State s;
    return s;
}

int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    int parsed = std::atoi(v);
    return parsed > 0 ? parsed : fallback;
}

// Caller holds s.mtx.
void ensureInit(State& s) {
    if (s.initialized) return;
    s.initialized = true;
    const char* path = std::getenv("ERUPTION_TELEMETRY_OUT");
    if (!path || !*path) return;
    s.out.open(path, std::ios::out | std::ios::trunc);
    if (!s.out.is_open()) {
        ERUPTION_LOG_WARN("[TELEMETRY] cannot open '%s', telemetry disabled", path);
        return;
    }
    s.outPath = path;
    s.every = envInt("ERUPTION_TELEMETRY_EVERY", 1);
    s.maxFrames = envInt("ERUPTION_TELEMETRY_MAX_FRAMES", 20000);
    s.start = Clock::now();
    s.enabled = true;
    s.fpsValues.reserve(std::min(s.maxFrames, 20000));
    ERUPTION_LOG_INFO("[TELEMETRY] recording to '%s' (every=%d, max_frames=%d)",
                      path, s.every, s.maxFrames);
    ERUPTION_LOG_INFO("[TELEMETRY] contadores de hardware (bloco \"hw\"): %s",
                      HwPerf::status());
}

double elapsedSec(const State& s) {
    return std::chrono::duration<double>(Clock::now() - s.start).count();
}

// Caller holds s.mtx.
void writeEvent(State& s, nlohmann::json&& j) {
    if (!s.enabled || s.finished) return;
    j["t"] = elapsedSec(s);
    s.loadEvents.push_back(j);
    s.out << j.dump() << '\n';
    s.out.flush();
}

void writeSummary(State& s) {
    if (s.outPath.empty()) return;
    nlohmann::json sum;
    const int n = static_cast<int>(s.fpsValues.size());
    sum["schema"] = 1;
    sum["frames"] = n;
    sum["duration_s"] = elapsedSec(s);
    if (n > 0) {
        std::vector<float> sorted = s.fpsValues;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) {
            int idx = std::clamp(static_cast<int>(p * (n - 1)), 0, n - 1);
            return sorted[idx];
        };
        sum["fps"] = {
            {"avg", s.fpsSum / n}, {"min", s.fpsMin},
            {"p1", pct(0.01)}, {"p50", pct(0.50)}, {"p99", pct(0.99)},
        };
        sum["max_frame_ms"] = s.maxFrameMs;
        sum["frames_below_60"] = s.framesBelow60;
        sum["frames_below_120"] = s.framesBelow120;

        // FRAME TIME DE VERDADE (delta real, sem suavizacao). Percentil se
        // calcula AQUI, nao no FPS: FPS e' reciproco, e percentil de reciproco
        // distorce. p99/p99.9 e a contagem de engasgo sao o que diz se o jogo
        // ROLA LISO - a media pode estar otima com o frame 1-em-200 travando.
        const int nd = static_cast<int>(s.frameDtMs.size());
        if (nd > 0) {
            std::vector<float> ds = s.frameDtMs;
            std::sort(ds.begin(), ds.end());
            auto dpct = [&](double p) {
                int idx = std::clamp(static_cast<int>(p * (nd - 1)), 0, nd - 1);
                return ds[idx];
            };
            double dsum = 0.0;
            for (float v : ds) dsum += v;
            const float median = dpct(0.50);
            // Engasgo = frame que passa do dobro da mediana E de 8 ms de folga
            // absoluta. O criterio relativo sozinho acusaria ruido numa cena
            // que roda a 900 FPS; o absoluto sozinho nunca dispararia numa
            // cena pesada que ja' roda a 30.
            int hitches = 0, hitchesBad = 0;
            for (float v : ds) {
                if (v > median * 2.0f && v > median + 8.0f) ++hitches;
                if (v > 50.0f) ++hitchesBad;   // >50 ms: some visivel a olho nu
            }
            sum["frame_dt_ms"] = {
                {"avg", dsum / nd}, {"median", median},
                {"p95", dpct(0.95)}, {"p99", dpct(0.99)}, {"p999", dpct(0.999)},
                {"max", s.maxFrameDtMs},
                {"hitches", hitches}, {"hitches_over_50ms", hitchesBad},
            };
        }
        nlohmann::json gpu;
        for (const auto& [name, total] : s.gpuSumMs) {
            int c = s.gpuCount[name];
            if (c > 0) gpu[name] = total / c;
        }
        sum["gpu_avg_ms"] = gpu;
        sum["peak_ram_mb"] = static_cast<double>(s.peakRamBytes) / (1024.0 * 1024.0);
        sum["peak_vram_mb"] = static_cast<double>(s.peakVramBytes) / (1024.0 * 1024.0);
    }
    sum["load_events"] = s.loadEvents;
    std::ofstream f(s.outPath + ".summary.json", std::ios::out | std::ios::trunc);
    if (f.is_open()) f << sum.dump(2) << '\n';
}

} // namespace

bool TelemetryExporter::enabled() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    ensureInit(s);
    return s.enabled && !s.finished;
}

void TelemetryExporter::recordFrame(const FrameStats& st) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    ensureInit(s);
    if (!s.enabled || s.finished) return;
    if (s.every > 1 && (st.frame % s.every) != 0) return;
    if (s.recorded >= s.maxFrames) {
        writeEvent(s, nlohmann::json{{"event", "max_frames_reached"}});
        s.finished = true;
        s.out.flush();
        return;
    }
    s.recorded++;

    nlohmann::json j;
    j["schema"] = 1;
    j["t"] = elapsedSec(s);
    j["frame"] = st.frame;
    j["fps"] = st.fps;
    j["frame_ms"] = st.frameMs;
    j["frame_dt_ms"] = st.frameDtMs;
    if (st.pipeInputPrimitives > 0 || st.pipeFragInvocations > 0) {
        j["pipe"] = {
            {"input_primitives", st.pipeInputPrimitives},
            {"clipped_primitives", st.pipeClippedPrimitives},
            {"vertex_invocations", st.pipeVertexInvocations},
            {"frag_invocations", st.pipeFragInvocations},
        };
    }
    j["present_ms"] = st.presentMs;
    j["cpu"] = {
        {"advance_ms", st.cpuAdvanceMs}, {"app_update_ms", st.cpuAppUpdateMs},
        {"render_ms", st.cpuRenderMs}, {"begin_wait_ms", st.cpuBeginWaitMs},
        {"tail_ms", st.cpuTailMs},
    };
    nlohmann::json cpuCat;
    for (uint32_t c = 0; c < static_cast<uint32_t>(ProfilerCategory::Count); ++c) {
        const auto cat = static_cast<ProfilerCategory>(c);
        const float ms = Profiler::getCpuTime(cat);
        if (ms > 0.0f) cpuCat[Profiler::getCategoryName(cat)] = ms;
    }
    j["cpu_cat"] = cpuCat;

    // Quebra do render_ms por fase. E' o dado que faltava: render_ms sozinho nao
    // diz se o custo esta em sombra, G-buffer, nuvem ou post.
    if (!st.cpuPhases.empty()) {
        nlohmann::json ph;
        for (const auto& [name, ms] : st.cpuPhases)
            if (ms > 0.0f) ph[name] = ms;
        j["cpu_phase"] = ph;
    }
    // VRAM RASTREADA por nós (PROFILE_VRAM_ALLOC), separada do total do
    // dispositivo que o NVML reporta em sys.vram_mb (que inclui desktop,
    // browser, etc. e não serve para atribuir custo à engine).
    nlohmann::json vramCat;
    double vramTrackedMb = 0.0;
    for (uint32_t c = 0; c < static_cast<uint32_t>(ProfilerCategory::Count); ++c) {
        const auto cat = static_cast<ProfilerCategory>(c);
        const double mb = static_cast<double>(Profiler::getVram(cat)) / (1024.0 * 1024.0);
        if (mb > 0.0) {
            vramCat[Profiler::getCategoryName(cat)] = mb;
            vramTrackedMb += mb;
        }
    }
    j["vram_cat"] = vramCat;
    j["vram_tracked_mb"] = vramTrackedMb;
    nlohmann::json gpu;
    float gpuTotal = 0.0f;
    for (const auto& [name, ms] : Profiler::getAllGpuTimes()) {
        gpu[name] = ms;
        gpuTotal += ms;
        s.gpuSumMs[name] += ms;
        s.gpuCount[name] += 1;
    }
    j["gpu"] = gpu;
    j["gpu_total_ms"] = gpuTotal;
    const size_t ramBytes = SystemMonitor::processRamBytes();
    const size_t vramBytes = SystemMonitor::gpuVramBytes();
    j["sys"] = {
        {"proc_cpu_pct", SystemMonitor::processCpuPercent()},
        {"proc_ram_mb", static_cast<double>(ramBytes) / (1024.0 * 1024.0)},
        {"gpu_core_pct", SystemMonitor::gpuCorePercent()},
        {"vram_mb", static_cast<double>(vramBytes) / (1024.0 * 1024.0)},
        {"vram_total_mb", static_cast<double>(SystemMonitor::gpuVramTotalBytes()) / (1024.0 * 1024.0)},
        {"vma_device_mb", st.vmaDeviceMb},
    };
    j["scene"] = {
        {"instances", st.instances}, {"meshes", st.meshes},
        {"visible_chunks", st.visibleChunks},
        // Triangulos na tela: modelos + terreno (mesmo passe G-Buffer), e o
        // total, que e' o numero que se quer olhar de relance.
        {"tri_models", st.triModels}, {"tri_terrain", st.triTerrain},
        {"triangles", st.triModels + st.triTerrain},
        {"draw_calls", st.drawCalls},
        // AGRUPAMENTO de draw calls (nao e' cache - ver FrameStats).
        // batch_runs = baldes (malha,LOD) = binds/draws; batch_reuse =
        // instancias absorvidas por um balde ja' aberto.
        {"batch_runs", st.batchRuns},
        {"batch_reuse", st.batchReuse},
        {"batch_reuse_pct", (st.batchReuse + st.batchRuns) > 0
            ? 100.0 * static_cast<double>(st.batchReuse) / (st.batchReuse + st.batchRuns)
            : 0.0},
        {"instances_per_batch", st.batchRuns > 0
            ? static_cast<double>(st.batchReuse + st.batchRuns) / st.batchRuns
            : 0.0},
        // DEPRECADO: mesmos numeros com o nome errado. Mantidos so' para nao
        // quebrar perf/test_cache_stats.py, que ja' existia. Some quando
        // aquele script migrar para batch_*.
        {"cache_hits", st.batchReuse},
        {"cache_misses", st.batchRuns},
        {"cache_hit_rate", (st.batchReuse + st.batchRuns) > 0
            ? static_cast<double>(st.batchReuse) / (st.batchReuse + st.batchRuns)
            : 0.0},
    };
    // CACHE DE HARDWARE DE VERDADE (perf_event_open na thread que grava a
    // telemetria = a thread de render). Opt-in: ERUPTION_HW_COUNTERS=1.
    // running_pct < 100 => o contador ficou parado parte do tempo (thread
    // migrou para E-core ou houve multiplexacao) e o valor foi extrapolado.
    if (HwPerf::enabled()) {
        const HwPerf::Counters hw = HwPerf::readThreadDelta();
        if (hw.valid) {
            nlohmann::json h;
            h["cycles"] = hw.cycles;
            h["instructions"] = hw.instructions;
            h["l1d_loads"] = hw.l1dLoads;
            h["l1d_misses"] = hw.l1dMisses;
            h["llc_loads"] = hw.llcLoads;
            h["llc_misses"] = hw.llcMisses;
            h["branch_misses"] = hw.branchMisses;
            h["running_pct"] = hw.runningPct;
            if (hw.cycles > 0)
                h["ipc"] = static_cast<double>(hw.instructions) / hw.cycles;
            if (hw.instructions > 0) {
                // MPKI: misses por 1000 instrucoes - a unidade padrao para
                // comparar layouts de dados entre builds e cenas.
                h["l1d_mpki"] = 1000.0 * hw.l1dMisses / hw.instructions;
                h["llc_mpki"] = 1000.0 * hw.llcMisses / hw.instructions;
            }
            if (hw.l1dLoads > 0)
                h["l1d_miss_rate"] = static_cast<double>(hw.l1dMisses) / hw.l1dLoads;
            if (hw.llcLoads > 0)
                h["llc_miss_rate"] = static_cast<double>(hw.llcMisses) / hw.llcLoads;
            j["hw"] = h;
        }
    }
    j["load"] = {
        {"loading", st.loading}, {"progress", st.loadProgress},
        {"frames_after_swap", st.framesAfterSwap},
    };
    j["weather"] = {
        {"type", st.weatherType ? st.weatherType : ""},
        {"rain", st.rainIntensity}, {"snow", st.snowIntensity},
    };
    s.out << j.dump() << '\n';
    if ((s.recorded % 60) == 0) s.out.flush();

    if (st.fps > 0.0f) {
        s.fpsSum += st.fps;
        s.fpsMin = (s.fpsValues.empty()) ? st.fps : std::min(s.fpsMin, st.fps);
        s.fpsValues.push_back(st.fps);
        if (st.fps < 60.0f) s.framesBelow60++;
        if (st.fps < 120.0f) s.framesBelow120++;
    }
    s.maxFrameMs = std::max(s.maxFrameMs, st.frameMs);
    // Fora do `if (fps > 0)` de proposito: o contador em janela leva meio
    // segundo pra ter valor, o delta real existe desde o primeiro frame.
    if (st.frameDtMs > 0.0f) {
        s.frameDtMs.push_back(st.frameDtMs);
        s.maxFrameDtMs = std::max(s.maxFrameDtMs, st.frameDtMs);
    }
    s.peakRamBytes = std::max(s.peakRamBytes, ramBytes);
    s.peakVramBytes = std::max(s.peakVramBytes, vramBytes);
}

void TelemetryExporter::recordLoadStart(const std::string& mapName) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    ensureInit(s);
    if (!s.enabled || s.finished) return;
    s.loadStart = Clock::now();
    s.loadActive = true;
    writeEvent(s, nlohmann::json{{"event", "load_start"}, {"map", mapName}});
}

void TelemetryExporter::recordMapSwap(const std::string& mapName, double freezeMs) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    ensureInit(s);
    if (!s.enabled || s.finished) return;
    writeEvent(s, nlohmann::json{{"event", "map_swap"}, {"map", mapName},
                                 {"freeze_ms", freezeMs}});
}

void TelemetryExporter::recordLoadEnd(const std::string& mapName) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    ensureInit(s);
    if (!s.enabled || s.finished) return;
    double wallS = s.loadActive
        ? std::chrono::duration<double>(Clock::now() - s.loadStart).count()
        : 0.0;
    s.loadActive = false;
    writeEvent(s, nlohmann::json{{"event", "load_end"}, {"map", mapName},
                                 {"wall_s", wallS}});
}

void TelemetryExporter::shutdown() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.enabled) return;
    s.out.flush();
    writeSummary(s);
    s.out.close();
    s.enabled = false;
    s.finished = true;
    ERUPTION_LOG_INFO("[TELEMETRY] wrote %d frames + summary to '%s'",
                      s.recorded, s.outPath.c_str());
}

} // namespace eruption
