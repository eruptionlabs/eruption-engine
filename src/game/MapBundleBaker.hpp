#pragma once
// BUILD DO MAPA: --bake-map <mapa> [--bake-raw] [--bake-level <1..19>].
//
// Abre o mapa como o jogo abre (cena, scripts, modelos, relevo, sondas), com
// tudo o que faltar sendo gerado e gravado no caminho (BC, PBR, relevo), e
// anota cada arquivo que a carga pede (utils/AssetVfs). Quando a carga
// assenta - relevo pronto e nenhum arquivo novo pedido por alguns segundos -
// grava <pasta do mapa>/<mapa>.emap com eles na ordem da carga e fecha.
//
// Roda num processo proprio (o editor dispara um, File > Build Map): numa
// sessao que ja' abriu outros mapas, texturas ja' na GPU nao seriam lidas de
// novo e ficariam de fora do pacote.

#include <chrono>
#include <cstddef>
#include <string>

namespace eruption {

class Engine;

class MapBundleBaker {
public:
    struct Options {
        std::string map;
        bool compress = true; // zstd por entrada (sem zstd no build, cru)
        int level = 9;
    };
    // Liga a anotacao e desliga os pacotes existentes: chamar ANTES da carga.
    MapBundleBaker(Engine& engine, Options options);
    // Uma vez por quadro. Fecha o motor quando o pacote foi gravado.
    void update();
    bool done() const { return m_done; }
    // Codigo de saida do processo: 0, ou 1 se o pacote nao foi gravado.
    static int exitCode();

private:
    void finish(bool timedOut);

    Engine& m_engine;
    Options m_opt;
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_start, m_lastChange, m_lastLog;
    size_t m_lastCount = 0;
    int m_frames = 0;
    bool m_done = false;
};

} // namespace eruption
