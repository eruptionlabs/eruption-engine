#include "utils/CacheStats.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

#ifdef __linux__
#include <asm/unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace eruption {

namespace {
std::atomic<bool> g_enabled{false};
std::mutex g_registryMutex;
std::vector<std::unique_ptr<ReuseDistSampler>> g_samplers;

// Fibonacci hash do endereco da LINHA (addr>>6): decide se a linha entra na
// amostra. Determinisco por linha (SHARDS): a MESMA linha e' sempre amostrada
// ou nunca - e' isso que preserva as distancias de reuso na sub-amostra.
inline bool sampled(uint64_t line, uint32_t shift) {
    const uint64_t h = line * 0x9E3779B97F4A7C15ull;
    return (h >> (64 - shift)) == 0; // top bits zero => 1 em 2^shift
}
} // namespace

ReuseDistSampler::ReuseDistSampler(std::string name) : m_name(std::move(name)) {
    m_lru.reserve(kMaxTracked);
}

void ReuseDistSampler::touch(const void* addr) {
    ++m_touches;
    const uint64_t line = reinterpret_cast<uint64_t>(addr) >> 6;
    if (!sampled(line, kSampleShift)) return;
    ++m_sampled;

    // Busca linear na LRU: posicao = distancia de reuso AMOSTRADA.
    // Distancia real estimada = posicao * 2^kSampleShift.
    auto it = std::find(m_lru.begin(), m_lru.end(), line);
    uint64_t estDist;
    if (it == m_lru.end()) {
        estDist = UINT64_MAX; // frio (compulsory na janela)
        if (m_lru.size() >= kMaxTracked) m_lru.pop_back();
    } else {
        estDist = static_cast<uint64_t>(it - m_lru.begin()) << kSampleShift;
        m_lru.erase(it);
    }
    m_lru.insert(m_lru.begin(), line);

    // Limiares em LINHAS de 64B: L1d 32KB = 512, L2 1MB = 16384. (Valores de
    // uma CPU mobile tipica; a 930M anda pareada com i5/i7 dessa classe. Sao
    // limiares de LEITURA da distribuicao, nao afetam a medicao em si.)
    if (estDist > 512) ++m_missL1;
    if (estDist > 16384) ++m_missL2;
}

void ReuseDistSampler::beginFrame() {
    if (m_sampled > 0) {
        m_lastL1Miss = static_cast<float>(m_missL1) / static_cast<float>(m_sampled);
        m_lastL2Miss = static_cast<float>(m_missL2) / static_cast<float>(m_sampled);
        m_lastTouches = static_cast<uint32_t>(m_touches);
    }
    m_touches = m_missL1 = m_missL2 = m_sampled = 0;
    // A LRU NAO zera entre frames: reuso entre frames e' reuso real (a
    // instancia tocada no frame N-1 ainda pode estar quente no N).
}

namespace CacheStats {

void setEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

ReuseDistSampler& sampler(const char* name) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (auto& s : g_samplers) {
        if (s->name() == name) return *s;
    }
    g_samplers.push_back(std::make_unique<ReuseDistSampler>(name));
    return *g_samplers.back();
}

std::vector<ReuseDistSampler*> all() {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    std::vector<ReuseDistSampler*> out;
    out.reserve(g_samplers.size());
    for (auto& s : g_samplers) out.push_back(s.get());
    return out;
}

void beginFrame() {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (auto& s : g_samplers) s->beginFrame();
}

} // namespace CacheStats

// ===========================================================================
// HwPerf - PMU do processador lido de dentro do processo (perf_event_open).
// ===========================================================================
namespace HwPerf {

#ifdef __linux__
namespace {

// Sete eventos INDEPENDENTES (nao um grupo). Grupo e' tudo-ou-nada: se o
// conjunto nao couber nos contadores fisicos disponiveis (o nmi_watchdog
// costuma ocupar um), o grupo inteiro nunca e' agendado e o resultado e' ZERO
// silencioso. Independentes, cada evento e' multiplexado sozinho e a fracao
// de tempo em que rodou vem no proprio read - por isso o valor e' escalavel e
// o furo e' visivel em runningPct em vez de virar numero errado.
struct EventDef { uint32_t type; uint64_t config; const char* name; };

constexpr uint64_t hwCache(uint64_t id, uint64_t op, uint64_t result) {
    return id | (op << 8) | (result << 16);
}

const EventDef kEvents[] = {
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,    "cycles"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,  "instructions"},
    {PERF_TYPE_HW_CACHE, hwCache(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                                 PERF_COUNT_HW_CACHE_RESULT_ACCESS), "L1-dcache-loads"},
    {PERF_TYPE_HW_CACHE, hwCache(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                                 PERF_COUNT_HW_CACHE_RESULT_MISS),   "L1-dcache-load-misses"},
    {PERF_TYPE_HW_CACHE, hwCache(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ,
                                 PERF_COUNT_HW_CACHE_RESULT_ACCESS), "LLC-loads"},
    {PERF_TYPE_HW_CACHE, hwCache(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ,
                                 PERF_COUNT_HW_CACHE_RESULT_MISS),   "LLC-load-misses"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, "branch-misses"},
};
constexpr int kNumEvents = static_cast<int>(sizeof(kEvents) / sizeof(kEvents[0]));

// read_format sem GROUP: value, time_enabled, time_running.
struct ReadOut { uint64_t value, timeEnabled, timeRunning; };

long perfEventOpen(perf_event_attr* attr, pid_t pid, int cpu, int groupFd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, groupFd, flags);
}

std::atomic<bool> g_wanted{false};
std::atomic<bool> g_wantedInit{false};
// Uma mensagem de status global (a primeira thread que tentar abrir decide).
char g_status[128] = "nao inicializado";
std::mutex g_statusMutex;

void setStatus(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

bool wanted() {
    if (!g_wantedInit.load(std::memory_order_acquire)) {
        const char* e = std::getenv("ERUPTION_HW_COUNTERS");
        const bool on = e && *e && std::strcmp(e, "0") != 0;
        g_wanted.store(on, std::memory_order_relaxed);
        if (!on) setStatus("desligado (defina ERUPTION_HW_COUNTERS=1)");
        g_wantedInit.store(true, std::memory_order_release);
    }
    return g_wanted.load(std::memory_order_relaxed);
}

int paranoidLevel() {
    std::ifstream f("/proc/sys/kernel/perf_event_paranoid");
    int v = 99;
    if (f) f >> v;
    return v;
}

// Estado por thread: um fd por evento + o valor acumulado da leitura anterior.
struct ThreadState {
    bool tried = false;
    bool ok = false;
    int fds[kNumEvents];
    uint64_t prevScaled[kNumEvents];

    ThreadState() {
        for (int i = 0; i < kNumEvents; ++i) { fds[i] = -1; prevScaled[i] = 0; }
    }
    ~ThreadState() {
        for (int i = 0; i < kNumEvents; ++i) if (fds[i] >= 0) close(fds[i]);
    }

    void open() {
        tried = true;
        if (!wanted()) return;
        const int par = paranoidLevel();
        int opened = 0;
        for (int i = 0; i < kNumEvents; ++i) {
            perf_event_attr attr{};
            attr.size = sizeof(attr);
            attr.type = kEvents[i].type;
            attr.config = kEvents[i].config;
            attr.disabled = 0;
            // Conta so' esta thread, em qualquer core (cpu=-1): o contador
            // migra junto com a thread.
            attr.inherit = 0;          // filhos nao herdam (queremos ESTA thread)
            attr.exclude_kernel = (par > 1) ? 1 : 0;
            attr.exclude_hv = 1;
            attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            const long fd = perfEventOpen(&attr, 0 /*esta thread*/, -1 /*qualquer cpu*/, -1, 0);
            if (fd < 0) {
                if (opened == 0) {
                    const int err = errno;
                    if (err == EACCES || err == EPERM)
                        setStatus("perf_event_open negado (errno=%d, perf_event_paranoid=%d): "
                                  "rode tools/setup_telemetry.sh --apply", err, par);
                    else
                        setStatus("perf_event_open falhou para '%s': %s (paranoid=%d)",
                                  kEvents[i].name, std::strerror(err), par);
                }
                continue;
            }
            fds[i] = static_cast<int>(fd);
            ++opened;
        }
        ok = opened > 0;
        if (ok) setStatus("ok: %d/%d eventos abertos (paranoid=%d)", opened, kNumEvents, par);
    }
};

thread_local ThreadState t_state;

// Le um evento e devolve o valor JA ESCALADO por enabled/running (a conta que
// o proprio `perf stat` faz quando ha' multiplexacao).
bool readScaled(int fd, uint64_t& outScaled, double& outRunPct) {
    if (fd < 0) return false;
    ReadOut r{};
    const ssize_t n = read(fd, &r, sizeof(r));
    if (n != static_cast<ssize_t>(sizeof(r))) return false;
    if (r.timeRunning == 0) { outScaled = 0; outRunPct = 0.0; return true; }
    outRunPct = 100.0 * static_cast<double>(r.timeRunning) / static_cast<double>(r.timeEnabled);
    outScaled = static_cast<uint64_t>(static_cast<double>(r.value) *
                                      (static_cast<double>(r.timeEnabled) /
                                       static_cast<double>(r.timeRunning)));
    return true;
}

} // namespace

bool enabled() {
    if (!wanted()) return false;
    if (!t_state.tried) t_state.open();
    return t_state.ok;
}

const char* status() {
    wanted();
    return g_status;
}

Counters readThreadDelta() {
    Counters c;
    if (!enabled()) return c;

    uint64_t now[kNumEvents];
    double runPct[kNumEvents];
    double worstRun = 100.0;
    bool anyPrev = false;
    for (int i = 0; i < kNumEvents; ++i) {
        now[i] = 0; runPct[i] = 0.0;
        if (!readScaled(t_state.fds[i], now[i], runPct[i])) { now[i] = t_state.prevScaled[i]; continue; }
        if (t_state.fds[i] >= 0) worstRun = std::min(worstRun, runPct[i]);
        if (t_state.prevScaled[i] != 0) anyPrev = true;
    }
    auto delta = [&](int i) -> uint64_t {
        const uint64_t d = (now[i] >= t_state.prevScaled[i]) ? now[i] - t_state.prevScaled[i] : 0;
        return d;
    };
    c.cycles       = delta(0);
    c.instructions = delta(1);
    c.l1dLoads     = delta(2);
    c.l1dMisses    = delta(3);
    c.llcLoads     = delta(4);
    c.llcMisses    = delta(5);
    c.branchMisses = delta(6);
    c.runningPct   = worstRun;
    for (int i = 0; i < kNumEvents; ++i) t_state.prevScaled[i] = now[i];
    // A primeira leitura da thread nao tem base de comparacao.
    c.valid = anyPrev;
    return c;
}

#else // !__linux__

bool enabled() { return false; }
const char* status() { return "perf_event_open so' existe no Linux"; }
Counters readThreadDelta() { return Counters{}; }

#endif

} // namespace HwPerf

} // namespace eruption
