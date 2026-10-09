#pragma once

#include "OnnxModel.hpp"

#include <array>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

// Programa de GPU de uma rede ONNX para UMA forma de entrada: a lista de
// operacoes que dependem da imagem, com formas resolvidas, e a reparticao da
// memoria. Tudo que so' depende de constantes ou da forma ja' foi avaliado
// na CPU (NnConstEval) e entra como buffer constante.
//
// Tensores sao float32 contiguos dentro de UMA area de memoria (arena);
// offsets e tamanhos sao em elementos.

constexpr int kNnMaxRank = 6;
using NnDims = std::array<int64_t, kNnMaxRank>;

enum class NnOpKind { Copy, Binary, Unary, Conv2d, MatMul, Gather, Reduce, Pool2d, Resize };

// Codigos compartilhados com os shaders (shaders/ml/*.comp).
enum class NnBinary : uint32_t { Add = 0, Sub, Mul, Div, Pow, Min, Max };
enum class NnUnary : uint32_t {
    Identity = 0, Neg, Sqrt, Sin, Cos, Exp, Log, Abs, Relu, Sigmoid, Tanh, Erf, Selu, Elu, LeakyRelu,
    HardSigmoid, Softplus, Reciprocal, Floor, Ceil, Clip
};
enum class NnReduce : uint32_t { Sum = 0, Mean, Max, Min, L2, SumSquare };
enum class NnPool : uint32_t { Average = 0, Max };
enum class NnResizeCoord : uint32_t { HalfPixel = 0, PytorchHalfPixel, AlignCorners, Asymmetric };
enum class NnResizeNearest : uint32_t { RoundPreferFloor = 0, RoundPreferCeil, Floor, Ceil };

struct NnBuffer {
    size_t elements = 0;
    bool constant = false;
    std::vector<float> data; // so' constantes
    int firstOp = INT_MAX, lastOp = -1;
    size_t offset = 0;       // preenchido pelo planejamento de memoria
};

struct NnView {
    int buffer = -1;
    size_t offset = 0; // dentro do buffer
    std::vector<int64_t> shape;
};

// Parametros por tipo de operacao:
//  Copy    in[0] -> out; 'dims' e' o espaco de iteracao, 'strides0' os passos
//          na origem e 'stridesOut' no destino (alinhados a' direita).
//  Binary  in[0] op in[1] -> out contiguo; 'dims' = forma da saida,
//          'strides0'/'strides1' com 0 onde ha' broadcast.
//  Unary   in[0] -> out; code, param0, param1.
//  Conv2d  in = {x, w, bias?}; ints = {padTop, padLeft, strideH, strideW,
//          dilH, dilW, group}.
//  MatMul  in = {A, B}; dims[0..3] = lote, strides0/strides1 = passo do lote
//          em A/B; ints = {M, N, K}.
//  Gather  in = {dados, indices}; ints = {outer, K, inner, tamanho do eixo}.
//  Reduce  in[0] -> out; dims = forma da entrada, strides0 = seus passos,
//          ints = {mascara dos eixos reduzidos}; code = NnReduce.
//  Pool2d  ints = {kh, kw, sh, sw, padTop, padLeft, countIncludePad}.
//  Resize  code = 0 vizinho / 1 linear; ints = {NnResizeCoord,
//          NnResizeNearest}; param0/param1 = escala em H/W.
struct NnOp {
    NnOpKind kind = NnOpKind::Copy;
    std::vector<NnView> in;
    NnView out;
    uint32_t code = 0;
    float param0 = 0.0f, param1 = 0.0f;
    NnDims dims{}, strides0{}, strides1{}, stridesOut{};
    std::vector<int64_t> ints;
};

struct NnProgram {
    std::vector<NnBuffer> buffers;
    std::vector<NnOp> ops;
    NnView input, output;
    size_t arenaElements = 0;
    size_t constantElements = 0; // constantes ficam no inicio da arena
};

bool buildNnProgram(const OnnxModel& model, const std::vector<int64_t>& inputShape, NnProgram& out,
                    std::string& error);

size_t viewCount(const NnView& v);

} // namespace eruption
