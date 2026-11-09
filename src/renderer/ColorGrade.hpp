#pragma once

#include "math/Types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

struct LookConfig;

// LUT 3D de color grading (post_composite.frag, binding 14).
//
// O grading analitico do composite (saturacao, contraste, split-toning) roda
// por pixel; balanco de branco, lift/gamma/gain, curvas, vibrance e uma LUT
// .cube de colorista nao cabem ali sem pesar o shader de todo preset. A LUT
// assa a cadeia INTEIRA uma vez (quando o look muda) num cubo 32^3, e o
// composite troca a conta por uma leitura trilinear.
//
// Dominio: a cor depois do tonemap (linear, 0..1), indexada em gamma 1/2.2
// (mais nos do cubo nos escuros). Saida: linear/kGradeLutRange codificada em
// gamma 1/2.2 num A2B10G10R10 (o contraste passa de 1 nas luzes, e a vinheta
// ainda multiplica depois).
constexpr float kGradeLutRange = 2.0f;
constexpr int kGradeLutSize = 32;

// LUT .cube (Adobe/Resolve, LUT_3D_SIZE, dominio 0..1 em gamma de tela).
struct CubeLut {
    int size = 0;
    std::vector<Vec3> data; // r mais rapido, depois g, depois b
    bool load(const std::string& path);
    Vec3 sample(const Vec3& c) const; // trilinear, c em 0..1
};

// A cadeia de grading inteira para uma cor (linear pos-tonemap). Com os
// campos novos do LookConfig no padrao e sem .cube, e' exatamente o
// applyGrading + applySplitToning do composite.
Vec3 gradeColor(const LookConfig& look, Vec3 c, const CubeLut* cube);

// Cubo n^3 em A2B10G10R10 (r nos bits baixos), r mais rapido.
std::vector<uint32_t> bakeGradeLut(const LookConfig& look, const CubeLut* cube, int n = kGradeLutSize);

// O look pede a LUT? (algum campo novo fora do padrao, .cube ou color_lut).
bool lookWantsGradeLut(const LookConfig& look);

// Assinatura dos campos que entram na LUT: muda -> reassar e reenviar.
uint64_t gradeLutHash(const LookConfig& look);

} // namespace eruption
