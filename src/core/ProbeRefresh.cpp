// Recozimento da grade de sondas de irradiancia quando a cena muda.

#include "core/Engine.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cstdlib>

namespace eruption {

// A grade e' cozida no fim do loadMap, so' com a malha do mapa. As instancias
// que a cena e os vizinhos do mundo poem depois (addModelFromFile) ficavam de
// fora: mundo montado por instancias saia como ceu aberto em tudo (vis 0,999,
// contra 0,008 de minimo com a mesma geometria dentro do .glb), e a sombra
// ambiente debaixo de telhados sumia. Quando o numero de instancias muda e
// fica parado kSettleFrames quadros (a cena entra em rajada), coze de novo
// com tudo o que esta' na cena. O bake mede ~10 ms e espera a GPU, uma vez
// por mudanca. ERUPTION_TEST_NO_PROBE_REFRESH=1 desliga (A/B).
void Engine::refreshIrradianceProbes() {
    constexpr int kSettleFrames = 30;
    static const bool kOff = std::getenv("ERUPTION_TEST_NO_PROBE_REFRESH") != nullptr;
    if (kOff || m_probeBakedInstances == SIZE_MAX || !m_irradianceProbesEnabled) return;
    const size_t n = m_modelRenderer.instanceCount();
    if (n == m_probeBakedInstances) {
        m_probeStableFrames = 0;
        return;
    }
    if (n != m_probeSeenInstances) {
        m_probeSeenInstances = n;
        m_probeStableFrames = 0;
        return;
    }
    if (++m_probeStableFrames < kSettleFrames) return;
    m_probeBakedInstances = n;
    m_probeStableFrames = 0;

    // Mesmo AABB do loadMap: terreno + instancias, com folga para copas.
    Vec3 mn(1e30f), mx(-1e30f);
    for (const auto& ch : m_terrainRenderer.chunks()) {
        mn = glm::min(mn, ch.aabbMin);
        mx = glm::max(mx, ch.aabbMax);
    }
    Vec3 imn, imx;
    if (m_modelRenderer.sceneBounds(imn, imx)) {
        mn = glm::min(mn, imn);
        mx = glm::max(mx, imx);
    }
    if (!(mx.x > mn.x && mx.z > mn.z)) return;
    mx.y += 40.0f;
    mn.y -= 20.0f;
    const bool ok = m_irradianceProbes.bake(m_terrainRenderer, m_modelRenderer,
                                            m_shadowRenderer.shadowPipeline(), m_shadowRenderer.shadowLayout(),
                                            m_shadowRenderer.shadowInstPipeline(), m_shadowRenderer.shadowInstLayout(),
                                            &m_bindless, mn, mx);
    if (ok) {
        m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                               m_irradianceProbes.gridMin(), m_irradianceProbes.gridInvExtent(), true);
    }
    ERUPTION_LOG_WARN("[PROBES] recozida com %zu instancias (cena/vizinhos): %s", n, ok ? "ok" : "falhou");
}

} // namespace eruption
