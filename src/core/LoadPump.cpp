#include "core/LoadPump.hpp"
#include <algorithm>
#include <chrono>
#include <thread>

namespace eruption {

namespace {
std::function<void()> g_pump;
std::thread::id g_owner;
std::chrono::steady_clock::time_point g_next;
bool g_inside = false;
} // namespace

void setLoadPump(std::function<void()> pump) {
    g_pump = std::move(pump);
    g_owner = std::this_thread::get_id();
}

void loadPump() {
    if (!g_pump || g_inside || std::this_thread::get_id() != g_owner) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next) return;
    g_inside = true;
    g_pump();
    g_inside = false;
    // O redesenho fica com no maximo ~10% do tempo: sem GPU de verdade (Xvfb) a
    // apresentacao copia o quadro inteiro pelo socket do X e leva dezenas de ms,
    // e redesenhar a cada 16 ms fazia a carga do mapa ficar 13x mais lenta.
    const auto fim = std::chrono::steady_clock::now();
    g_next = fim + std::max<std::chrono::steady_clock::duration>(std::chrono::milliseconds(16), (fim - now) * 9);
}

} // namespace eruption
