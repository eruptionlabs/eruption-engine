#pragma once

#include <vector>
#include <utility>

#include <cstdint>
#include <string>

namespace eruption {

// Per-frame JSONL telemetry sink consumed by the perf/ Python harness.
//
// Enabled by ERUPTION_TELEMETRY_OUT=<path.jsonl> (the --telemetry CLI flag
// sets the same env var). Disabled (all calls no-op) when the var is unset.
// One JSON object per sampled frame plus load-lifecycle events, and a
// <path>.summary.json aggregate written on shutdown.
//
// Tuning:
//   ERUPTION_TELEMETRY_EVERY=N       sample every Nth frame (default 1)
//   ERUPTION_TELEMETRY_MAX_FRAMES=N  stop recording after N frames (default
//                                    20000 - keeps the file bounded)
class TelemetryExporter {
public:
    struct FrameStats {
        int frame = 0;
        // ATENCAO: fps e frameMs vem do contador em JANELA de 0,5 s da engine,
        // ou seja, ja' chegam SUAVIZADOS - toda linha do JSONL dentro da mesma
        // janela repete o mesmo valor. Servem pra leitura humana e pra media,
        // NAO pra achar engasgo: um hitch de 40 ms no meio de meio segundo de
        // frames de 8 ms vira ~8,6 ms aqui e SOME. Pra qualquer metrica de
        // suavidade (p99/p99.9 de frame time, contagem de hitch) use frameDtMs.
        float fps = 0.0f;
        float frameMs = 0.0f;
        // Delta de RELOGIO DE PAREDE deste frame, em ms (Timer::realDeltaTime).
        // E' o unico campo por-frame de verdade: imune ao ERUPTION_TEST_FIXED_DT
        // e sem suavizacao nenhuma. Foi o que faltava pra suite conseguir ver
        // engasgo - antes os limiares de stutter (max_frame_ms, p1_fps,
        // max_frame_ms_during_load) media todos janela suavizada e por
        // construcao nao disparavam pra hitch de um frame so'.
        float frameDtMs = 0.0f;
        float cpuAdvanceMs = 0.0f;
        float cpuAppUpdateMs = 0.0f;
        float cpuRenderMs = 0.0f;
        float cpuBeginWaitMs = 0.0f;
        float cpuTailMs = 0.0f;
        // Custo do vkQueuePresentKHR. Sob xvfb domina o frame (~7,4 ms) e cria
        // um teto de ~135 FPS que nao tem nada a ver com a engine.
        float presentMs = 0.0f;
        uint32_t instances = 0;
        uint32_t meshes = 0;
        int visibleChunks = 0;
        // Triangulos REALMENTE submetidos neste frame (pos-culling, pos-LOD).
        uint32_t triModels = 0;   // passe principal de modelos
        uint32_t triTerrain = 0;  // terreno, no mesmo passe G-Buffer
        uint32_t drawCalls = 0;
        // PIPELINE STATISTICS do passe de geometria (contagem do DRIVER, nao
        // da CPU). triModels/triTerrain acima sao o que a engine SUBMETE; isto
        // e' o que o pipeline processou de fato. A diferenca importa pra
        // publicar numero de cena de referencia: o LOD de malha roda no load,
        // entao a contagem do ARQUIVO nunca e' a que se rasteriza.
        // fragInvocations e' sobredesenho medido (nao estimado).
        uint64_t pipeInputPrimitives = 0;
        uint64_t pipeClippedPrimitives = 0;
        uint64_t pipeVertexInvocations = 0;
        uint64_t pipeFragInvocations = 0;
        // AGRUPAMENTO DE DRAW CALLS - NAO E' CACHE DE HARDWARE.
        //
        // Isto vem do counting sort de ModelRenderer::sortVisibleByMesh e conta
        // BALDES (malha,LOD):
        //   batchRuns   = baldes nao-vazios = 1 bind + 1 draw call cada
        //   batchReuse  = instancias que cairam num balde ja' aberto (nao
        //                 pagam bind/draw novo)
        // Ou seja: eficiencia de AGRUPAMENTO, uma metrica de CPU/driver. Foi
        // batizado de "cache hit/miss" por analogia e o nome enganou - nao ha'
        // nenhuma linha de cache envolvida. Cache de verdade agora existe e vem
        // de HwPerf (perf_event_open), no bloco "hw" do JSONL.
        //
        // Os nomes antigos ficam como ALIAS DO MESMO ARMAZENAMENTO so' porque
        // Engine.cpp ainda escreve por eles (`st.cacheHits = ...`). Ao migrar
        // Engine.cpp para batchReuse/batchRuns, apague as duas linhas de alias.
        // Eficiencia de AGRUPAMENTO DE DRAW CALLS (nao e' cache de hardware -
        // ver o bloco "hw" para contador de verdade). O alias em union com os
        // nomes antigos existia so' enquanto Engine.cpp ainda escrevia
        // cacheHits/cacheMisses; ja' nao escreve, entao os nomes ficaram limpos.
        uint32_t batchReuse = 0;   // instancias que reusaram o bucket do vizinho
        uint32_t batchRuns  = 0;   // buckets distintos = binds + draw calls
        // ms por fase dentro de render(), deste frame
        std::vector<std::pair<const char*, float>> cpuPhases;
        bool loading = false;
        float loadProgress = 0.0f;
        int framesAfterSwap = -1;
        const char* weatherType = "";
        float rainIntensity = 0.0f;
        float snowIntensity = 0.0f;
        // VRAM alocada PELO PROCESSO via VMA (blockBytes das heaps device-local).
        // sys.vram_mb vem do NVML e é do dispositivo inteiro (desktop, outros
        // processos); só esta serve para atribuir custo à engine.
        double vmaDeviceMb = 0.0;
    };

    static bool enabled();

    // GPU pass times (Profiler::getAllGpuTimes) and system stats
    // (SystemMonitor) are appended internally.
    static void recordFrame(const FrameStats& stats);

    static void recordLoadStart(const std::string& mapName);
    static void recordMapSwap(const std::string& mapName, double freezeMs);
    static void recordLoadEnd(const std::string& mapName);

    // Flushes the JSONL stream and writes <path>.summary.json. Safe to call
    // more than once; recording stays disabled afterwards.
    static void shutdown();
};

} // namespace eruption
