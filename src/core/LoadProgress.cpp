// loading.progress: quanto da carga em curso ja' acabou, contado no trabalho
// que o motor deixa pendente depois do mapa (folhas de sprite em preparo,
// vizinhos do mundo continuo lendo, texturas na fila de envio, sons
// decodificando). A mesma fracao move a barra da espera do loading.hold, para
// o jogo e a barra nunca discordarem.
#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "core/LoadingScreen.hpp"
#include "utils/PbrTextureLoader.hpp"

#include <algorithm>
#include <chrono>

namespace eruption {

namespace {
// Barra da troca em segundo plano enquanto o mapa novo le' (renderLoadingProgress).
constexpr float kWarpProgress = 0.4f;
// "Finalizing" da carga de mapa: dai' ate' 1 e' o trabalho pendente.
constexpr float kMapDone = 0.95f;
} // namespace

int Engine::pendingLoadWork() {
    int n = 0;
    for (const auto& [key, job] : m_spritePreloads)
        if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) ++n;
    if (m_warpJobActive) ++n;
    for (const WorldRegion& r : m_regions)
        if (r.state == WorldRegion::State::Loading) ++n;
    n += static_cast<int>(s_pendingUploads.size() + pendingPbrUploadCount());
    n += static_cast<int>(m_audio.pendingLoads());
    return n;
}

// Uma carga comeca num mapa novo (loadMap) ou no inicio de uma troca em
// segundo plano; a troca que termina no mapa dela continua a mesma carga.
// Os contadores so' crescem: total soma o que apareceu pendente desde o
// inicio, feito = total - pendente. A fracao guarda o maximo. Pronta, a carga
// fica pronta ate' a proxima: o que o jogo pede depois (um vizinho que entra
// andando, um som novo) ja' e' o jogo rodando, e a barra nao pode voltar.
void Engine::loadProgress(float& done, float& total, bool& ready) {
    auto begin = [&] {
        m_lpSeen = m_lpPendingLast = 0;
        m_lpRaw = 0.0f;
        m_lpReady = false;
        m_lpMapped = m_lpSessions == 0 && !m_firstLoadOver && projectLoadingStyle() != nullptr;
        ++m_lpSessions;
    };
    if (m_lpReady && !m_warpJobActive && m_currentMapName == m_lpMap) {
        done = total = float(m_lpSeen + 1);
        ready = true;
        return;
    }
    if (m_warpJobActive && !m_lpWarp) {
        begin();
        m_lpWarp = true;
    } else if (!m_warpJobActive && m_lpWarp) {
        m_lpWarp = false;
        m_lpMap = m_currentMapName;
    } else if (!m_warpJobActive && m_currentMapName != m_lpMap) {
        begin();
        m_lpMap = m_currentMapName;
    }
    const int pending = pendingLoadWork();
    if (pending > m_lpPendingLast) m_lpSeen += pending - m_lpPendingLast;
    m_lpPendingLast = pending;
    ready = pending == 0 && !m_warpJobActive;
    m_lpReady = ready;
    float raw = 1.0f;
    if (m_warpJobActive)
        raw = kWarpProgress;
    else if (!ready)
        raw = kMapDone + (1.0f - kMapDone) * float(m_lpSeen - pending) / float(std::max(m_lpSeen, 1));
    m_lpRaw = std::max(m_lpRaw, std::min(raw, ready ? 1.0f : 0.999f));
    total = float(m_lpSeen + 1); // o mapa conta como um
    const float shown = m_lpMapped ? 0.3f + 0.7f * m_lpRaw : m_lpRaw;
    done = shown * total;
}

} // namespace eruption
