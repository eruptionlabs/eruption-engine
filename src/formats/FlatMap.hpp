#pragma once

// MAPA VAZIO: chão plano + grade de altura + ambiente. É o ponto de partida
// de todo mapa autoral — o File > New Map do editor e o --new-map da linha
// de comando chamam ESTA peça, para existir um jeito só de nascer um mapa.
//
// Escreve, dentro de assets/external/<nome>/ do projeto aberto:
//   <nome>.glb  chão de `size` x `size` unidades, de (0,0) a (size,size)
//   <nome>.ter  um ponto de altura por vértice do chão (passo de 8 u)
//   <nome>.env  direção do sol; o resto do ambiente vem do motor
//
// Não toca em GPU nem em cena: serve a quem está com o motor aberto e a quem
// está gerando mapa por programa.

#include "math/Types.hpp"

#include <filesystem>
#include <string>

namespace eruption {

class FlatMap {
public:
    struct Options {
        float size = 1000.0f;                       // lado do chão, em unidades
        Vec3 groundColor{0.30f, 0.34f, 0.27f};      // cor do material do chão
        Vec3 sunDirection{0.47f, -0.82f, 0.33f};    // sol a ~55 graus
        bool overwrite = false;                     // true: refaz um mapa existente
    };

    // `dir` é a pasta do mapa (assets/external/<nome>). Erro em *error.
    static bool create(const std::filesystem::path& dir, const std::string& name, const Options& o,
                       std::string& error);

    // Passo da grade do chão. O .ter tem um ponto por vértice (o pincel de
    // terreno edita os dois juntos).
    static constexpr float kGridStep = 8.0f;
};

} // namespace eruption
