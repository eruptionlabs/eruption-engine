#pragma once

#include "math/Types.hpp"
#include "renderer/DeferredLighting.hpp"
#include "renderer/ImmediateDraw.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace eruption {

// Raio 3D entre dois pontos (vfx.bolt do Luau). O trajeto e' gerado em 3D por
// deslocamento do ponto medio (nao e' um quad so'): tronco principal que afina
// ate' a ponta e galhos que afinam e somem. A cada flicker_ms o trajeto e'
// sorteado de novo (o raio treme). O desenho e' uma fita virada para a camera
// POR SEGMENTO (a juncao usa o lado medio, sem frestas), em duas camadas
// aditivas: halo largo na cor do raio e nucleo estreito e quente. Sem escrita de
// profundidade (passa pelo ImmediateDraw, com teste de profundidade so' de
// leitura). O trecho dentro de uma nuvem (acima da base dela) fica largo,
// difuso e fraco, e cada sorteio acende a nuvem por dentro (onFlash).
struct BoltSpec {
    Vec3 from = Vec3(0.0f);
    Vec3 to = Vec3(0.0f, -50.0f, 0.0f);
    float width = 1.2f;          // metros na raiz do tronco
    int branches = 3;            // galhos (0..12)
    float jitter = 0.35f;        // 0..1, desvio relativo do trajeto
    float flickerMs = 70.0f;     // intervalo do novo sorteio
    float life = 0.45f;          // s; <= 0: ate' bolt_stop
    Vec3 color = Vec3(0.55f, 0.68f, 1.0f);      // halo
    Vec3 coreColor = Vec3(1.0f, 0.98f, 0.95f);  // nucleo
    float intensity = 1.0f;
    float lightRadius = 60.0f;   // 0 = sem luz
    float lightIntensity = 8.0f;
    uint32_t cloudId = 0;        // nuvem de script em que o raio nasce (0 = nenhuma)
    float cloudBaseY = -1e30f;   // altura da base dessa nuvem (acima dela e' "dentro")
};

class VfxBolt {
public:
    static constexpr size_t kMaxBolts = 16;

    bool init(ImmediateDraw& draw);
    // Devolve o id (> 0) ou 0 se o teto de raios simultaneos foi atingido.
    uint32_t start(const BoltSpec& spec);
    bool stop(uint32_t id);
    void clear() { m_bolts.clear(); }
    void update(float dt);
    // Entrega a geometria do quadro ao ImmediateDraw.
    void submit(ImmediateDraw& draw, const Vec3& camPos);
    // Luzes pontuais curtas (saida e impacto) dos raios vivos, no maximo `maxLights`.
    void collectLights(std::vector<PointLight>& out, size_t maxLights) const;
    size_t activeCount() const { return m_bolts.size(); }
    uint32_t lastVertices() const { return m_lastVertices; }

    // A nuvem acende a cada sorteio: (id da nuvem, intensidade, duracao em s).
    std::function<void(uint32_t, float, float)> onFlash;

private:
    struct Pt {
        Vec3 p;
        float w;        // largura (m)
        float diffuse;  // 0 fora da nuvem .. 1 dentro
    };
    struct Poly {
        std::vector<Pt> pts;
        float inten = 1.0f;
    };
    struct Bolt {
        uint32_t id = 0;
        BoltSpec spec;
        float age = 0.0f;
        float nextRoll = 0.0f;
        uint32_t rolls = 0;
        float gain = 1.0f;       // brilho do sorteio atual
        std::vector<Poly> polys; // tronco primeiro
    };
    void roll(Bolt& b);
    float envelope(const Bolt& b) const;

    int m_texHalo = -1, m_texCore = -1;
    uint32_t m_nextId = 1;
    uint32_t m_lastVertices = 0;
    std::vector<Bolt> m_bolts;
};

} // namespace eruption
