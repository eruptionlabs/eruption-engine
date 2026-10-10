#pragma once

// Texturas de particula que vem com o motor, geradas na hora (sem arquivo):
// "builtin:puff" (fumaca, 4x4 variacoes), "builtin:flame" (chama, 4x4
// quadros), "builtin:spark", "builtin:ring" e "builtin:star". Servem de
// ponto de partida; o jogo usa as proprias folhas de quadros em PNG.

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

struct VfxImage {
    uint32_t width = 0, height = 0;
    uint32_t cols = 1, rows = 1; // folha de quadros
    std::vector<uint8_t> rgba;   // alfa reto (nao premultiplicado)
};

bool makeVfxBuiltin(const std::string& name, VfxImage& out);
const std::vector<std::string>& vfxBuiltinNames();

} // namespace eruption
