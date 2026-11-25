// Tornado (vfx.tornado): estado por quadro, rajada de vento e rastro no chao. O desenho
// do funil fica em CloudLayerRenderer::renderTornado e o dos destrocos em
// TornadoField::submitDebris; aqui so' o que depende do motor (vento, estado do chao).
#include "core/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

uint32_t Engine::startTornado(const TornadoSpec& spec) {
    if (!m_tornadoEnabled || m_tornadoMax <= 0) return 0;
    return m_tornados.start(spec, size_t(m_tornadoMax));
}

void Engine::updateTornados(float dt) {
    if (!m_tornados.active()) return;
    m_tornados.update(dt);
    for (TornadoField::Tornado& t : m_tornados.all()) {
        const float strength = TornadoField::intensity(t) * TornadoField::touchdown(t);
        if (strength <= 0.05f) continue;
        const TornadoSpec& s = t.spec;
        // Rajada que acompanha o tornado: a vegetacao em volta se curva para fora e as particulas
        // sao levadas. Renovada antes de acabar, entao o vento nao falha entre uma e outra.
        t.burstTimer -= dt;
        if (t.burstTimer <= 0.0f) {
            t.burstTimer = 0.4f;
            if (s.wind > 0.0f) windBurst(s.pos.x, s.pos.z, s.radiusTop * 1.7f, 0.40f * s.wind * strength, 0.9f);
        }
        // Rastro: terra revolvida e escurecida onde o pe passou (um ponto a cada ~1,5 raios).
        const float spacing = std::max(s.radiusBottom * 1.5f, 0.3f);
        const float dx = s.pos.x - t.lastTrail.x, dz = s.pos.z - t.lastTrail.z;
        if (!s.trailKind.empty() && (dx * dx + dz * dz >= spacing * spacing || t.trailTimer == 0.0f)) {
            GroundStateRequest r;
            r.kind = s.trailKind; // padrao "scorch": terra revolvida e escura (o "wet" brilha como poca no campo aberto)
            r.x = s.pos.x;
            r.z = s.pos.z;
            r.radius = std::max(s.radiusBottom * 2.4f, 0.6f);
            r.strength = s.trailStrength * strength;
            r.duration = 45.0f;
            r.fade = 30.0f;
            groundState(r);
            t.lastTrail = s.pos;
            t.trailTimer = 1.0f;
        }
    }
}

} // namespace eruption
