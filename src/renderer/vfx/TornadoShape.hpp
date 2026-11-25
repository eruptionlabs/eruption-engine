#pragma once

#include "math/Types.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {
namespace tornado_shape {

// Forma do tornado em funcao da altura relativa h (0 = chao, 1 = base da nuvem-mae).
// O shader do funil (shaders/clouds/tornado.frag) repete EXATAMENTE estas contas:
// os destrocos, a caixa do ray-march e os testes usam a mesma curva que a imagem.
// `age` e' o relogio do proprio tornado (s); `seed` em radianos.

struct Shape {
    float height = 200.0f;
    float radiusTop = 36.0f;
    float radiusBottom = 6.0f;
    float seed = 0.0f;
};

inline float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Deslocamento horizontal do eixo (a corda que balanca): inclinacao fixa em curva,
// balanco lento e uma onda de dobra que sobe. Zero no chao.
inline Vec2 axisOffset(const Shape& s, float h, float age) {
    const float hh = std::clamp(h, 0.0f, 1.3f);
    const Vec2 lean = Vec2(std::cos(s.seed), std::sin(s.seed)) * (0.14f * s.height * hh * hh);
    const float sw = 0.055f * s.height * hh;
    const Vec2 swing = sw * Vec2(std::sin(age * 0.45f + hh * 2.7f + s.seed * 1.7f),
                                 std::cos(age * 0.36f + hh * 3.3f + s.seed * 0.9f));
    const Vec2 kink = (0.010f * s.height * hh) * Vec2(std::sin(age * 1.3f - hh * 8.5f + s.seed),
                                                      std::cos(age * 1.1f - hh * 7.5f + s.seed * 2.3f));
    return lean + swing + kink;
}

// Raio da parede: corda fina embaixo, abre em curva e alarga ao encontrar a nuvem.
inline float radiusAt(const Shape& s, float h, float age) {
    const float t = std::clamp(h, 0.0f, 1.3f);
    float r = s.radiusBottom + (s.radiusTop - s.radiusBottom) * std::pow(std::min(t, 1.0f), 1.6f);
    r *= 1.0f + 0.9f * smoothstep(0.80f, 1.05f, t);
    r *= 1.0f + 0.10f * std::sin(t * 9.0f - age * 0.7f + s.seed * 2.0f) * (0.3f + t);
    return r;
}

// Velocidade angular (rad/s) da parede na altura h e raio relativo rr: mais rapida
// perto do eixo e embaixo (rotacao diferencial).
inline float angularSpeed(float spin, float h, float rr) {
    return 0.6f * spin * (0.55f + 1.6f * (1.0f - std::clamp(h, 0.0f, 1.0f))) / (0.45f + rr);
}

// Raio da nuvem-mae e do anel de poeira a partir das dimensoes do funil.
inline float motherRadius(const Shape& s) { return std::max(s.radiusTop * 4.5f, s.height * 0.55f); }
inline float skirtRadius(const Shape& s) { return std::max(s.radiusBottom * 7.0f, s.height * 0.17f); }

} // namespace tornado_shape
} // namespace eruption
