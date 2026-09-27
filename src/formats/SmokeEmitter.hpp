#pragma once

#include "math/Types.hpp"

namespace eruption {

// Emissor de fumaça declarado no companion .env do mapa (chave opcional
// "smoke"). É só DADO: a engine é quem implementa a coluna de fumaça
// (CloudLayerRenderer::renderSmokePlume). Ferramentas externas
// (eruption-tools/smoke_add.py) apenas escrevem o array.
//
// Modelo físico da pluma (todos os campos em unidades de mundo / segundos):
//   - a parcela de fumaça nasce em `position` com raio `radius`
//   - sobe a `rise` m/s, então a idade da parcela na altura h é t = h / rise
//   - é advectada pelo vento: centro(h) = position.xz + vento * t * windScale
//   - o raio cresce com a altura: r(h) = radius * (1 + spread * h/height)
//   - dissipa com a distância da fonte (densidade cai com h)
struct SmokeEmitter {
    Vec3 position = Vec3(0.0f);      // "pos": boca do vulcão, em coordenadas de mundo
    float radius = 25.0f;            // "radius": raio da base da coluna (m)
    float height = 60.0f;            // "thickness": altura total da coluna (m)
    float rise = 12.0f;              // "rise": velocidade de subida (m/s)
    float rate = 1.0f;               // "rate": ritmo do borbulhar (multiplica a animação do ruído)
    float density = 0.7f;            // "density": opacidade máxima (0..1)
    Vec3 color = Vec3(0.25f, 0.24f, 0.23f); // "color": cor base (cinza escuro vulcânico)
    float spread = 1.8f;             // "spread": quanto o raio cresce até o topo (r_topo = radius*(1+spread))
    float windScale = 1.0f;          // "wind": multiplicador da advecção pelo vento do clima
    float turbulence = 0.03f;        // "turbulence": frequência do ruído 3D (1/m)
    float glow = 0.0f;               // "glow": brasa na base (0 = sem emissivo)
    float fire = 0.0f;               // "fire": 0 = fumaça, 1 = chama procedural
    bool enabled = true;             // "enabled": permite desligar sem apagar a entrada
};

} // namespace eruption
