// Registro de eventos do jogo (utils/EventLog, telemetry.emit no Luau).
//
//   1. fechado nao aloca                    — jogo sem telemetria nao paga nada
//   2. ritmo de jogo: 100 mil eventos em    — o pedido de balanceamento: nada
//      3600 quadros, nenhum descartado,       cai, e o custo por evento e' medido
//      todas as linhas JSON validas
//   3. rajada maior que o anel: escritos +  — anel cheio descarta e conta, sem
//      descartados = emitidos                 travar e sem linha partida
//   4. teto do arquivo: vira .1 e a parte   — o disco nao cresce sem fim
//      nova recomeca com cabecalho
//   5. texto com aspas e quebra de linha    — uma linha por evento, sempre
//
//   g++ -std=c++17 -O2 -I src tests/test_event_log.cpp src/utils/EventLog.cpp src/core/Logger.cpp -lpthread
#include "utils/EventLog.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace eruption;
namespace fs = std::filesystem;

static std::atomic<long> g_allocs{0};
void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int g_falhas = 0;

static void checa(bool ok, const char* titulo) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", titulo);
    if (!ok) ++g_falhas;
}

static std::vector<std::string> linhas(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) out.push_back(l);
    return out;
}

// JSON raso o bastante para este formato: objeto, aspas balanceadas fora de
// escape, sem controle cru.
static bool linhaValida(const std::string& l) {
    if (l.size() < 2 || l.front() != '{' || l.back() != '}') return false;
    bool dentro = false;
    for (size_t i = 0; i < l.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(l[i]);
        if (c < 0x20) return false;
        if (dentro && c == '\\') { ++i; continue; }
        if (c == '"') dentro = !dentro;
    }
    return !dentro;
}

static void quadroDeJogo(EventLog& log, int q) {
    static const char* nomes[] = {"dano", "cura", "kill", "skill"};
    const float pos[3] = {float(q), 1.5f, -2.25f};
    for (int k = 0; k < 28; ++k) {
        log.begin(nomes[k % 4]);
        log.field("valor", double(k * 7 + q % 13));
        log.field("crit", (k % 5) == 0);
        log.field("alvo", std::string_view("lobo_cinza"));
        log.field("pos", pos, 3);
        log.commit();
    }
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("event_log_test_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    EventLog log;

    std::printf("1. fechado\n");
    {
        const long antes = g_allocs.load();
        for (int q = 0; q < 1000; ++q) {
            log.advance(1.0f / 60.0f);
            quadroDeJogo(log, q);
        }
        checa(g_allocs.load() == antes, "28 mil eventos com o registro fechado: 0 alocacao");
    }

    std::printf("2. ritmo de jogo\n");
    {
        const fs::path p = dir / "jogo.jsonl";
        checa(log.open(p, {"teste", "mapa_x"}), "abre");
        double emitMs = 0.0, piorQuadroMs = 0.0;
        for (int q = 0; q < 3600; ++q) {
            log.advance(1.0f / 60.0f);
            const auto t0 = std::chrono::steady_clock::now();
            quadroDeJogo(log, q);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            emitMs += ms;
            piorQuadroMs = std::max(piorQuadroMs, ms);
            // Um quadro de jogo leva tempo; sem isto a escrita nunca alcanca.
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        const uint64_t n = log.events(), d = log.dropped();
        log.close();
        std::printf("     %llu eventos, %llu descartados; custo medio %.4f ms/quadro (%.0f ns/evento), pior quadro %.3f ms\n",
                    (unsigned long long)n, (unsigned long long)d, emitMs / 3600.0, emitMs * 1e6 / double(n),
                    piorQuadroMs);
        checa(n == 3600u * 28u && d == 0, "100800 eventos, nenhum descartado");
        checa(emitMs / 3600.0 < 0.05, "custo medio < 0,05 ms por quadro");
        const auto l = linhas(p);
        bool ok = l.size() == n + 2;
        for (const auto& s : l) ok = ok && linhaValida(s);
        checa(ok, "cabecalho + um evento por linha + fim, todas JSON");
        checa(l.front().find("\"ch\":\"_head\"") != std::string::npos && l.front().find("\"map\":\"mapa_x\"") != std::string::npos,
              "cabecalho com o mapa");
        checa(l[1].rfind("{\"t\":17,\"f\":1,\"ch\":\"dano\",\"valor\":0,\"crit\":true", 0) == 0, "evento: t em ms e f do quadro");
        checa(l.back().find("\"events\":100800") != std::string::npos && l.back().find("\"t\":60000") != std::string::npos,
              "fim com a contagem e 60 s de jogo");
        checa(EventLog::kRingBytes * 2 + EventLog::kMaxLineBytes <= (4u << 20), "memoria fixa <= 4 MB (anel + lote + linha)");
    }

    std::printf("3. rajada\n");
    {
        const fs::path p = dir / "rajada.jsonl";
        log.open(p, {"teste", "mapa_x"});
        for (int q = 0; q < 8000; ++q) quadroDeJogo(log, q);
        const uint64_t n = log.events(), d = log.dropped();
        log.close();
        std::printf("     %llu escritos, %llu descartados\n", (unsigned long long)n, (unsigned long long)d);
        const auto l = linhas(p);
        bool ok = true;
        for (const auto& s : l) ok = ok && linhaValida(s);
        checa(n + d == 8000u * 28u, "escritos + descartados = emitidos");
        checa(l.size() == n + 2 && ok, "so' linhas inteiras no arquivo");
        checa(l.back().find("\"dropped\":" + std::to_string(d)) != std::string::npos, "fim diz quantos cairam");
    }

    std::printf("4. teto do arquivo\n");
    {
        const fs::path p = dir / "teto.jsonl";
        log.open(p, {"teste", "mapa_x"}, 64u << 10);
        for (int q = 0; q < 400; ++q) {
            log.advance(1.0f / 60.0f);
            quadroDeJogo(log, q);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        log.close();
        fs::path antigo = p;
        antigo += ".1";
        const auto a = linhas(antigo), b = linhas(p);
        checa(fs::exists(antigo) && !a.empty() && a.front().find("\"_head\"") != std::string::npos, "parte antiga em .1");
        checa(!b.empty() && b.front().find("\"_head\"") != std::string::npos && b.front().find("\"part\":0") == std::string::npos,
              "parte nova com cabecalho proprio");
        checa(fs::file_size(p) < (64u << 10) + EventLog::kRingBytes, "arquivo atual dentro do teto + um lote");
    }

    std::printf("5. texto\n");
    {
        const fs::path p = dir / "texto.jsonl";
        log.open(p, {"proj \"x\"", "m"});
        log.begin("fala");
        log.field("texto", std::string_view("ele disse \"oi\"\nlinha 2\\fim"));
        log.commit();
        log.close();
        const auto l = linhas(p);
        checa(l.size() == 3 && linhaValida(l[0]) && linhaValida(l[1]), "aspas, barra e quebra escapadas");
        checa(l[1].find("\\\"oi\\\"\\u000alinha 2\\\\fim") != std::string::npos, "texto escapado como JSON");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("%s\n", g_falhas ? "FALHOU" : "TUDO CERTO");
    return g_falhas ? 1 : 0;
}
