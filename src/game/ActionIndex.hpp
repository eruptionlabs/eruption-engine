#pragma once
#include <cstddef>

namespace eruption {

// Indice da acao de um sprite para uma direcao (0..7) ou a visao tatica (4*8 + direcao).
// O vetor de acoes pode ter menos entradas do que isso: um sprite de PNG tem UMA so'
// (loadSpriteFromPng) e indexar com a direcao lia memoria solta (SIGSEGV intermitente
// no HUD sobre a cabeca, conforme o angulo da camera). Devolve -1 se nao ha acao.
inline int safeActionIndex(int wanted, std::size_t count) {
    if (count == 0 || wanted < 0) return -1;
    std::size_t idx = static_cast<std::size_t>(wanted);
    if (idx >= count) idx %= 8;
    if (idx >= count) idx = 0;
    return static_cast<int>(idx);
}

} // namespace eruption
