#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace eruption {

// Registro de eventos do JOGO em JSONL (telemetry.emit no Luau): dano, cura,
// morte, uso de habilidade - o que o jogo quiser contar para balancear.
// Nao confundir com o TelemetryExporter (ERUPTION_TELEMETRY_OUT), que grava
// o custo de cada quadro do motor.
//
// Uma linha por evento, chaves curtas e fixas:
//   {"ch":"_head","v":1,"project":..,"map":..,"build":..,"date":..,"part":0}
//   {"t":1234,"f":74,"ch":"dano","valor":12,"alvo":"lobo"}
//   {"ch":"_end","t":..,"f":..,"events":N,"dropped":D}
// t = tempo de jogo em ms (o dt dos scripts: pausa e passo fixo valem),
// f = quadro de script desde a abertura. Canal com "_" e' do motor.
//
// Custo: a linha e' montada na thread do jogo num buffer reservado na
// abertura e copiada para um anel de tamanho fixo (kRingBytes); uma thread
// escreve no disco a cada segundo ou quando o anel passa da metade. Anel
// cheio: o evento cai e entra na conta "dropped" - o quadro nunca espera o
// disco. Fechado, nenhuma chamada aloca nem trava.
//
// Arquivo maior que o teto (maxFileBytes, mais ate' um lote): vira
// <arquivo>.1 (o .1 anterior se perde) e o atual recomeca com cabecalho de
// "part" seguinte. No disco fica no maximo ~2 x o teto.
class EventLog {
public:
    struct Header {
        std::string project;
        std::string map;
    };

    static constexpr size_t kRingBytes = 1u << 20;
    static constexpr size_t kMaxLineBytes = 16u << 10;
    static constexpr size_t kMaxStringBytes = 512;

    static EventLog& instance();
    EventLog() = default;
    ~EventLog();
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    // Abre (trunca) o arquivo e sobe a thread de escrita. Reabrir fecha o anterior.
    bool open(const std::filesystem::path& path, const Header& header, uint64_t maxFileBytes = 64ull << 20);
    // Escreve o que restou, a linha "_end" e fecha.
    void close();
    bool enabled() const { return m_open; }
    const std::filesystem::path& path() const { return m_path; }

    // Relogio de jogo (uma vez por quadro de script).
    void advance(float dt);

    // Uma linha: begin, campos, commit. Chave repetida nao e' conferida aqui.
    void begin(std::string_view channel);
    void field(std::string_view key, double value);
    void field(std::string_view key, bool value);
    void field(std::string_view key, std::string_view value);
    void field(std::string_view key, const float* values, int count);
    void commit();

    uint64_t events() const { return m_events; }
    uint64_t dropped() const;

private:
    void push(const char* data, size_t n);
    void writerLoop();
    void drain(std::vector<char>& out);
    void writeChunk(const char* data, size_t n);
    void writeHeader();

    bool m_open = false;
    std::filesystem::path m_path;
    Header m_header;
    uint64_t m_maxFileBytes = 0;
    double m_timeMs = 0.0;
    uint64_t m_frame = 0;
    uint64_t m_events = 0;
    std::string m_line;

    // Anel: escrito pela thread do jogo, esvaziado pela de escrita.
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<char> m_ring;
    size_t m_ringHead = 0; // proximo byte a ler
    size_t m_ringSize = 0;
    uint64_t m_dropped = 0;
    bool m_stop = false;

    // So' a thread de escrita mexe daqui para baixo (depois do open).
    std::thread m_writer;
    std::vector<char> m_out;
    std::FILE* m_file = nullptr;
    uint64_t m_fileBytes = 0;
    int m_part = 0;
};

} // namespace eruption
