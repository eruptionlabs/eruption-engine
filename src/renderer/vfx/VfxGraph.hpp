#pragma once

// GRAFO DE EFEITO: expressoes por particula montadas em nos (editor) e
// compiladas na CPU para um bytecode curto que um interpretador pequeno roda
// na GPU (nascimento, simulacao e desenho). Nada de compilar shader: o
// grafo vale no mesmo frame em que e' editado, no editor e no jogo
// exportado, em qualquer GPU, e todas as particulas continuam num dispatch.
//
// Todo valor e' vec4 dentro da maquina (escalar = os 4 iguais; vetor usa
// xyz). Cada saida do grafo vira um programa separado, so' com o que ela usa:
//   velocidade inicial (nascimento), forca (simulacao),
//   cor e tamanho (desenho, multiplicam o que as curvas da vida dao).

#include "math/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

struct VfxGraphNode {
    int id = 0;
    std::string op;     // tipo do no (ver vfxGraphCatalog)
    Vec4 value{0.0f};   // constante / parametro do no
    Vec2 pos{0.0f};     // posicao no editor
};

struct VfxGraphLink {
    int fromNode = 0, fromPin = 0; // saida
    int toNode = 0, toPin = 0;     // entrada
};

struct VfxGraph {
    std::vector<VfxGraphNode> nodes;
    std::vector<VfxGraphLink> links;
    int nextId = 1;
    bool empty() const { return nodes.empty(); }
    const VfxGraphNode* node(int id) const;
};

// Tipos de no (catalogo unico: compilador, editor e arquivo).
struct VfxGraphNodeType {
    const char* op;           // chave no arquivo
    const char* label;        // nome na interface (ingles; traduzido no editor)
    const char* category;     // Entrada, Constante, Matematica, Vetor, Saida
    int inputs;
    int outputs;
    const char* inputNames[4];
    const char* outputNames[4];
    uint8_t code;             // opcode da maquina (0 = nao gera instrucao direta)
};
const std::vector<VfxGraphNodeType>& vfxGraphCatalog();
const VfxGraphNodeType* vfxGraphType(const std::string& op);

// Programa compilado: faixas no codigo e constantes absolutas no pool.
struct VfxGraphProgram {
    uint32_t offset = 0, length = 0; // em instrucoes (uint)
};
struct VfxGraphCompiled {
    std::vector<uint32_t> code;
    std::vector<Vec4> constants;
    VfxGraphProgram spawnVelocity, force, color, size;
    std::string error; // vazio = ok
};

// Compila o grafo. codeBase/constBase: onde o resultado vai morar nos
// buffers globais (as instrucoes ja' saem com enderecos absolutos).
VfxGraphCompiled compileVfxGraph(const VfxGraph& g, uint32_t codeBase, uint32_t constBase);

// Opcodes (iguais aos de shaders/vfx/vfx_graph.glsl).
namespace vfxop {
enum : uint8_t {
    End = 0, Const, InAge, InLife, InPos, InVel, InRandom, InTime, InEmitterPos, InSize,
    Add, Sub, Mul, Div, Min, Max, Pow, Sin, Cos, Abs, Fract, Floor, Sqrt, Negate, OneMinus, Saturate,
    Lerp, Clamp, Smoothstep, Step, Length, Normalize, Dot, Cross, SplitX, SplitY, SplitZ, SplitW,
    Combine3, Noise, Mod, Remap,
    Count
};
}

} // namespace eruption
