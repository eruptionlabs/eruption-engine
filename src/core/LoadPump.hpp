#pragma once

#include <functional>

namespace eruption {

// Tela de carregamento viva durante trabalho longo da thread principal: quem
// espera uma tarefa (leitura do mapa, LOD) chama loadPump() no laço de espera
// e a tela é redesenhada (no máximo a cada ~16 ms, e nunca mais que ~10% do
// tempo: com apresentação lenta, o intervalo cresce). Fora da thread que
// registrou a bomba, ou sem bomba, não faz nada.
void setLoadPump(std::function<void()> pump);
void loadPump();

} // namespace eruption
