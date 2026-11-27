#pragma once
// Medidor do arranque (so' ERUPTION_DEV): quanto tempo e quantas chamadas Vulkan caras (modulo de shader,
// pipeline de compute, submit, esperas, present) e outras partes do init (leitura de .spv, redesenho da tela
// de carga) cada etapa do Engine::init gastou. O log [BOOT] imprime a diferenca por etapa. No celular o
// tempo de init nao e' o do PC (la' o driver e a flash e' que pesam), entao a medida e' feita no proprio
// aparelho. No build final nada disto existe.
#if ERUPTION_DEV
#include <chrono>
#include <cstdint>
#include <string>

namespace eruption::vktrace {

enum Op : int {
    ShaderModule, ComputePipeline, Submit, QueueWaitIdle, DeviceWaitIdle, WaitFences, Present, Acquire,
    CreateImage, AllocMemory, CreateBuffer, SpvFile, Redraw, Upload, Other, kOpCount
};

void add(Op op, int64_t ns);
void snapshot(uint64_t count[kOpCount], int64_t ns[kOpCount]);
const char* name(Op op);

// Mede um trecho e soma em `op` ao sair do escopo.
struct Scope {
    explicit Scope(Op o) : op(o), t0(std::chrono::steady_clock::now()) {}
    ~Scope() { add(op, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
    Op op;
    std::chrono::steady_clock::time_point t0;
};

} // namespace eruption::vktrace
#define ERUPTION_BOOT_SCOPE(op) ::eruption::vktrace::Scope eruptionBootScope_(::eruption::vktrace::op)
#else
#define ERUPTION_BOOT_SCOPE(op) ((void)0)
#endif
