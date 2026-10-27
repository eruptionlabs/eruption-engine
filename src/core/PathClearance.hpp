// Folga de parede para o caminho: um ponto so' serve se nenhuma celula
// bloqueada da grade estiver a menos de `clearance` dele. A celula (cx, cz) e'
// o quadrado de lado `step` centrado em (cx*step, cz*step), como no
// Engine::findPath. Sem dependencia do motor, para o teste rodar numa grade
// sintetica.
#pragma once

#include <algorithm>
#include <cmath>

namespace eruption {

// Passo da amostragem dos trechos com folga, em fracao da celula: um quarto
// de celula pega a quina que o meio-passo do alisamento antigo atravessava.
constexpr float kClearanceSample = 0.25f;

// `walk(cx, cz)` diz se a celula anda (sem folga). Testa toda celula cujo
// quadrado chega a menos de `clearance` do ponto (distancia ponto-quadrado).
template <class Walk>
bool pointHasClearance(float px, float pz, float step, float clearance, Walk&& walk) {
    const int cx0 = int(std::floor((px - clearance) / step + 0.5f));
    const int cx1 = int(std::floor((px + clearance) / step + 0.5f));
    const int cz0 = int(std::floor((pz - clearance) / step + 0.5f));
    const int cz1 = int(std::floor((pz + clearance) / step + 0.5f));
    const float half = step * 0.5f, c2 = clearance * clearance;
    for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx) {
            const float dx = std::max(0.0f, std::abs(px - cx * step) - half);
            const float dz = std::max(0.0f, std::abs(pz - cz * step) - half);
            if (dx * dx + dz * dz >= c2 && clearance > 0.0f) continue;
            if (!walk(cx, cz)) return false;
        }
    return true;
}

// Trecho reto de a a b amostrado a cada kClearanceSample de celula (pontas
// fora: quem chama ja' as conferiu).
template <class Walk>
bool segmentHasClearance(float ax, float az, float bx, float bz, float step, float clearance, Walk&& walk) {
    const float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    const int n = std::max(1, int(std::ceil(len / (step * kClearanceSample))));
    for (int i = 1; i < n; ++i) {
        const float t = float(i) / float(n);
        if (!pointHasClearance(ax + (bx - ax) * t, az + (bz - az) * t, step, clearance, walk)) return false;
    }
    return true;
}

} // namespace eruption
