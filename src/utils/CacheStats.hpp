#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace eruption {

// Amostrador estatistico de DISTANCIA DE REUSO (o fator comum de todo cache
// miss: distancia > capacidade => miss). Estilo SHARDS (Waldspurger et al.,
// FAST'15): em vez de rastrear todo acesso, amostra linhas de cache por hash
// espacial do endereco a uma taxa fixa R; as distancias medidas no trace
// amostrado escalam por 1/R e reconstroem a distribuicao real com erro
// pequeno. Motivo original de existir em software: perf_event_paranoid=4
// bloqueava contador de hardware sem root (medido 2026-09-01).
//
// ISSO MUDOU (2026-09-02): a maquina esta' em paranoid=-1 e o contador de
// hardware REAL agora existe, em HwPerf (mais abaixo neste arquivo). Este
// amostrador continua util para o que o PMU nao da': miss atribuido a UM LACO
// nomeado, sem precisar de perf record. Para "quanto de cache a thread de
// render gastou neste frame", use HwPerf - e' medida, nao estimativa.
//
// Uso: CacheStats::sampler("nome-do-loop").touch(&dado[idx]) dentro do hot
// loop. Custo quando desligado: um load+branch. Quando ligado: um hash+compare
// por acesso; o trabalho de LRU so' acontece nos ~6% amostrados.
class ReuseDistSampler {
public:
    explicit ReuseDistSampler(std::string name);

    void touch(const void* addr);
    void beginFrame(); // fecha a janela do frame anterior e zera acumuladores

    const std::string& name() const { return m_name; }
    // Estimativas da ULTIMA janela fechada (fracao 0..1 de acessos cuja
    // distancia de reuso estimada excede a capacidade do nivel).
    float l1MissRatio() const { return m_lastL1Miss; }
    float l2MissRatio() const { return m_lastL2Miss; }
    uint32_t lastTouches() const { return m_lastTouches; }

private:
    // R = 1/16: com dezenas de milhares de toques/frame nos loops de
    // instancia, resolve distancias ate' ~16 linhas - suficiente para os
    // limiares L1 (~512 linhas / 32KB) e L2 (~16k linhas / 1MB).
    static constexpr uint32_t kSampleShift = 4; // 1/16
    static constexpr size_t kMaxTracked = 4096;

    std::string m_name;
    // LRU dos enderecos de linha amostrados: posicao na lista = distancia
    // amostrada. Vetor simples: kMaxTracked e' pequeno e o hit e' raro o
    // bastante pra busca linear nao doer (so' ~6% dos toques entram aqui).
    std::vector<uint64_t> m_lru;
    uint64_t m_touches = 0;
    uint64_t m_missL1 = 0;
    uint64_t m_missL2 = 0;
    uint64_t m_sampled = 0;
    float m_lastL1Miss = 0.0f;
    float m_lastL2Miss = 0.0f;
    uint32_t m_lastTouches = 0;
};

// ---------------------------------------------------------------------------
// CONTADOR DE CACHE DE VERDADE (nao proxy, nao amostragem estatistica).
//
// O comentario do ReuseDistSampler acima diz que perf_event_paranoid=4
// bloqueava contador de HW sem root. Isso MUDOU: a maquina esta' em
// paranoid=-1 (verificado 2026-09-02, tools/setup_telemetry.sh --apply),
// entao perf_event_open funciona no proprio processo, sem root e sem `perf`.
//
// HwPerf abre os contadores do PMU para a THREAD QUE CHAMA (pid=0, cpu=-1:
// o contador segue a thread entre cores) e devolve o DELTA desde a leitura
// anterior daquela thread. Chamado uma vez por frame pelo TelemetryExporter,
// da' cycles/instructions/L1d/LLC/branch REAIS por frame da thread de render.
//
// ARMADILHA DE CPU HIBRIDA (esta maquina e' i7-13620H: 6 P-cores + 4 E-cores):
// os eventos genericos abrem no PMU cpu_core. Quando o scheduler move a
// thread para um E-core o contador NAO conta - por isso cada evento carrega
// time_enabled/time_running e o valor e' escalado por enabled/running, com a
// fracao exposta em running_pct. running_pct << 100 = o numero foi
// extrapolado; para medida limpa, fixe a thread nos P-cores
//     taskset -c "$(cat /sys/devices/cpu_core/cpus)" ./eruption-engine ...
//
// Ligado por ERUPTION_HW_COUNTERS=1 (opt-in: sao 7 syscalls read() por frame,
// ~2-3 us, mas nao ha' motivo para pagar isso quando ninguem esta medindo).
namespace HwPerf {

struct Counters {
    bool valid = false;
    uint64_t cycles = 0;
    uint64_t instructions = 0;
    uint64_t l1dLoads = 0;
    uint64_t l1dMisses = 0;
    uint64_t llcLoads = 0;
    uint64_t llcMisses = 0;
    uint64_t branchMisses = 0;
    // Menor fracao enabled/running entre os eventos, em % (100 = sem
    // multiplexacao e sempre num core que conta).
    double runningPct = 0.0;
};

// true se ERUPTION_HW_COUNTERS=1 e perf_event_open funcionou nesta thread.
bool enabled();
// Motivo legivel quando enabled() e' false ("desligado", "EACCES: paranoid=N",
// ...). Nunca nullptr.
const char* status();
// Delta desde a chamada anterior NESTA thread (a primeira chamada abre os
// contadores e devolve valid=false). Nao aloca.
Counters readThreadDelta();

} // namespace HwPerf

namespace CacheStats {
    // Liga/desliga globalmente (Engine liga quando o F3 esta' aberto).
    void setEnabled(bool on);
    bool enabled();
    // Registro por nome (cria na primeira chamada; ponteiro estavel).
    ReuseDistSampler& sampler(const char* name);
    // Snapshot de todos os amostradores registrados (para o F3).
    std::vector<ReuseDistSampler*> all();
    // Chamar 1x por frame (Engine) - fecha a janela de cada amostrador.
    void beginFrame();
}

} // namespace eruption
