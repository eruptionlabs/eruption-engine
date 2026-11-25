#pragma once

#include "math/Types.hpp"
#include "renderer/ImmediateDraw.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace eruption {

// Precipitacao de uma nuvem de script (weather.cloud_precip): chuva ou neve num
// volume (cilindro) da base da nuvem ate' o chao, so' na area `radius`. Simulada
// na CPU (poucas centenas de particulas por nuvem) e desenhada pelo
// ImmediateDraw: gotas como fitas viradas para a camera e flocos como pontos,
// aditivos, com teste de profundidade (o cenario tapa o que esta' atras).
// As particulas guardam o deslocamento em relacao ao centro da nuvem, entao
// acompanham o cloud_move; uma nuvem que deixa de pedir sai do mapa de estados.
class CloudPrecip {
public:
    struct Params {
        uint32_t id = 0;       // id da nuvem de script (chave do emissor)
        Vec2 center;           // x, z no mundo
        float baseY = 0.0f;    // topo da coluna (base da nuvem)
        float radius = 40.0f;  // raio da area (m)
        bool snow = true;
        float rate = 1.0f;     // multiplicador da quantidade (0..3)
        float alpha = 1.0f;    // 0..1: some com a nuvem
    };
    // Altura do chao em (x, z).
    using GroundFn = std::function<float(float, float)>;

    static constexpr uint32_t kMaxPerEmitter = 3000;
    static constexpr uint32_t kMaxTotal = 6000;

    bool init(ImmediateDraw& draw);
    // Avanca as particulas dos emissores desta lista; os que nao vierem somem.
    void update(float dt, const std::vector<Params>& emitters, const GroundFn& ground);
    // snowLit: cor do floco ja' iluminado pela cena (albedo x luz); o floco e' opaco, nao emite.
    void submit(ImmediateDraw& draw, const Vec3& camPos, const Vec3& camRight, const Vec3& camUp,
                const Vec3& snowLit);
    void clear() { m_states.clear(); m_active.clear(); }
    uint32_t particleCount() const;
    uint32_t lastVertices() const { return m_lastVertices; }

private:
    struct Particle {
        float ox, oz;   // deslocamento em relacao ao centro
        float y;
        float phase;
        float size;
    };
    struct State {
        std::vector<Particle> parts;
        uint32_t rng = 1;
        float time = 0.0f;
        Params last;
        float groundY = 0.0f;
    };
    int m_texDot = -1, m_texStreak = -1, m_texFlake = -1;
    uint32_t m_lastVertices = 0;
    std::unordered_map<uint32_t, State> m_states;
    std::vector<uint32_t> m_active; // ids vistos no ultimo update, em ordem
};

} // namespace eruption
