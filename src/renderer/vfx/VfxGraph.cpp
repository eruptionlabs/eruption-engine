#include "renderer/vfx/VfxGraph.hpp"

#include <functional>
#include <map>
#include <set>

namespace eruption {

const VfxGraphNode* VfxGraph::node(int id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const std::vector<VfxGraphNodeType>& vfxGraphCatalog() {
    using namespace vfxop;
    static const std::vector<VfxGraphNodeType> k = {
        // Entradas (dados da particula / do efeito)
        {"in_age", "Age (s)", "Input", 0, 1, {}, {"s"}, InAge},
        {"in_life", "Life fraction", "Input", 0, 1, {}, {"0..1"}, InLife},
        {"in_position", "Position", "Input", 0, 1, {}, {"xyz"}, InPos},
        {"in_velocity", "Velocity", "Input", 0, 1, {}, {"xyz"}, InVel},
        {"in_random", "Random", "Input", 0, 1, {}, {"0..1"}, InRandom},
        {"in_time", "Effect time", "Input", 0, 1, {}, {"s"}, InTime},
        {"in_emitter", "Effect position", "Input", 0, 1, {}, {"xyz"}, InEmitterPos},
        {"in_size", "Start size", "Input", 0, 1, {}, {"m"}, InSize},
        // Constantes (o valor fica no proprio no)
        {"const_float", "Number", "Constant", 0, 1, {}, {"x"}, Const},
        {"const_vec3", "Vector", "Constant", 0, 1, {}, {"xyz"}, Const},
        {"const_color", "Color", "Constant", 0, 1, {}, {"rgba"}, Const},
        // Matematica (componente a componente)
        {"add", "Add", "Math", 2, 1, {"a", "b"}, {"a+b"}, Add},
        {"sub", "Subtract", "Math", 2, 1, {"a", "b"}, {"a-b"}, Sub},
        {"mul", "Multiply", "Math", 2, 1, {"a", "b"}, {"a*b"}, Mul},
        {"div", "Divide", "Math", 2, 1, {"a", "b"}, {"a/b"}, Div},
        {"min", "Min", "Math", 2, 1, {"a", "b"}, {"min"}, Min},
        {"max", "Max", "Math", 2, 1, {"a", "b"}, {"max"}, Max},
        {"pow", "Power", "Math", 2, 1, {"base", "exp"}, {"pow"}, Pow},
        {"mod", "Modulo", "Math", 2, 1, {"a", "b"}, {"mod"}, Mod},
        {"sin", "Sine", "Math", 1, 1, {"x"}, {"sin"}, Sin},
        {"cos", "Cosine", "Math", 1, 1, {"x"}, {"cos"}, Cos},
        {"abs", "Absolute", "Math", 1, 1, {"x"}, {"|x|"}, Abs},
        {"fract", "Fraction", "Math", 1, 1, {"x"}, {"fract"}, Fract},
        {"floor", "Floor", "Math", 1, 1, {"x"}, {"floor"}, Floor},
        {"sqrt", "Square root", "Math", 1, 1, {"x"}, {"sqrt"}, Sqrt},
        {"negate", "Negate", "Math", 1, 1, {"x"}, {"-x"}, Negate},
        {"one_minus", "One minus", "Math", 1, 1, {"x"}, {"1-x"}, OneMinus},
        {"saturate", "Saturate", "Math", 1, 1, {"x"}, {"0..1"}, Saturate},
        {"lerp", "Lerp", "Math", 3, 1, {"a", "b", "t"}, {"mix"}, Lerp},
        {"clamp", "Clamp", "Math", 3, 1, {"x", "min", "max"}, {"clamp"}, Clamp},
        {"smoothstep", "Smoothstep", "Math", 3, 1, {"edge0", "edge1", "x"}, {"s"}, Smoothstep},
        {"step", "Step", "Math", 2, 1, {"edge", "x"}, {"0/1"}, Step},
        // Vetor
        {"length", "Length", "Vector", 1, 1, {"v"}, {"|v|"}, Length},
        {"normalize", "Normalize", "Vector", 1, 1, {"v"}, {"v/|v|"}, Normalize},
        {"dot", "Dot", "Vector", 2, 1, {"a", "b"}, {"a.b"}, Dot},
        {"cross", "Cross", "Vector", 2, 1, {"a", "b"}, {"axb"}, Cross},
        {"split", "Split", "Vector", 1, 4, {"v"}, {"x", "y", "z", "w"}, 0},
        {"combine", "Combine", "Vector", 3, 1, {"x", "y", "z"}, {"xyz"}, Combine3},
        {"noise", "Curl noise", "Vector", 2, 1, {"position", "time"}, {"xyz"}, Noise},
        // Saidas (uma de cada, no maximo)
        {"out_spawn_velocity", "Spawn: add velocity", "Output", 1, 0, {"m/s"}, {}, 0},
        {"out_force", "Update: add force", "Output", 1, 0, {"m/s2"}, {}, 0},
        {"out_color", "Render: multiply color", "Output", 1, 0, {"rgba"}, {}, 0},
        {"out_size", "Render: multiply size", "Output", 1, 0, {"x"}, {}, 0},
    };
    return k;
}

const VfxGraphNodeType* vfxGraphType(const std::string& op) {
    for (const auto& t : vfxGraphCatalog())
        if (op == t.op) return &t;
    return nullptr;
}

namespace {

constexpr int kRegisters = 16;

// Instrucao: op(6) | dst(4) | a(4) | b(4) | c(4) | imm(10). Const ocupa duas
// palavras: a segunda e' o indice absoluto da constante.
uint32_t ins(uint8_t op, int dst, int a = 0, int b = 0, int c = 0, uint32_t imm = 0) {
    return uint32_t(op & 63) | (uint32_t(dst & 15) << 6) | (uint32_t(a & 15) << 10) | (uint32_t(b & 15) << 14) |
           (uint32_t(c & 15) << 18) | ((imm & 1023u) << 22);
}

struct Compiler {
    const VfxGraph& g;
    VfxGraphCompiled& out;
    uint32_t codeBase, constBase;
    std::map<std::pair<int, int>, int> memo; // (no, pino de saida) -> registrador
    std::set<int> visiting;
    int nextReg = 0;
    bool failed = false;

    int fail(const std::string& msg) {
        if (!failed) out.error = msg;
        failed = true;
        return 0;
    }
    int alloc() {
        if (nextReg >= kRegisters) return fail("grafo grande demais para uma saida (mais de 16 valores vivos)");
        return nextReg++;
    }
    int constant(const Vec4& v) {
        const int r = alloc();
        out.constants.push_back(v);
        out.code.push_back(ins(vfxop::Const, r));
        out.code.push_back(constBase + uint32_t(out.constants.size() - 1));
        return r;
    }
    // Valor que chega na entrada `pin` do no: o ligado, ou o valor do proprio
    // no naquele componente (entrada solta tem valor editavel).
    int input(const VfxGraphNode& n, int pin) {
        for (const auto& l : g.links)
            if (l.toNode == n.id && l.toPin == pin) return eval(l.fromNode, l.fromPin);
        return constant(Vec4(n.value[pin & 3]));
    }
    int eval(int nodeId, int pin) {
        if (failed) return 0;
        auto key = std::make_pair(nodeId, pin);
        auto it = memo.find(key);
        if (it != memo.end()) return it->second;
        const VfxGraphNode* n = g.node(nodeId);
        if (!n) return fail("ligacao para um no que nao existe");
        const VfxGraphNodeType* t = vfxGraphType(n->op);
        if (!t) return fail("no desconhecido: " + n->op);
        if (visiting.count(nodeId)) return fail("o grafo tem um ciclo (" + std::string(t->label) + ")");
        visiting.insert(nodeId);
        int r = 0;
        using namespace vfxop;
        if (n->op == "split") {
            const int a = input(*n, 0);
            r = alloc();
            out.code.push_back(ins(uint8_t(SplitX + pin), r, a));
        } else if (t->code == Const) {
            r = constant(n->value);
        } else if (t->inputs == 0) {
            r = alloc();
            out.code.push_back(ins(t->code, r, 0, 0, 0, uint32_t(n->value.x) & 1023u)); // imm: semente do aleatorio
        } else {
            int a = 0, b = 0, c = 0;
            a = input(*n, 0);
            if (t->inputs > 1) b = input(*n, 1);
            if (t->inputs > 2) c = input(*n, 2);
            r = alloc();
            out.code.push_back(ins(t->code, r, a, b, c));
        }
        visiting.erase(nodeId);
        memo[key] = r;
        return r;
    }
    VfxGraphProgram program(const char* outputOp) {
        VfxGraphProgram p;
        const VfxGraphNode* outNode = nullptr;
        for (const auto& n : g.nodes)
            if (n.op == outputOp) outNode = &n;
        if (!outNode) return p; // saida nao usada: sem programa
        memo.clear();
        visiting.clear();
        nextReg = 0;
        const uint32_t start = uint32_t(out.code.size());
        const int r = input(*outNode, 0);
        out.code.push_back(ins(vfxop::End, 0, r));
        if (failed) return VfxGraphProgram{};
        p.offset = codeBase + start;
        p.length = uint32_t(out.code.size()) - start;
        return p;
    }
};

} // namespace

VfxGraphCompiled compileVfxGraph(const VfxGraph& g, uint32_t codeBase, uint32_t constBase) {
    VfxGraphCompiled out;
    Compiler c{g, out, codeBase, constBase, {}, {}, 0, false};
    out.spawnVelocity = c.program("out_spawn_velocity");
    out.force = c.program("out_force");
    out.color = c.program("out_color");
    out.size = c.program("out_size");
    if (c.failed) {
        out.spawnVelocity = out.force = out.color = out.size = VfxGraphProgram{};
        out.code.clear();
        out.constants.clear();
    }
    return out;
}

} // namespace eruption
