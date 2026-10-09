#include "NnConstEval.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>

namespace eruption {

namespace {

using Inputs = std::vector<const OnnxTensor*>;
using Outputs = std::vector<OnnxTensor>;

const OnnxTensor* input(const Inputs& in, size_t i) { return i < in.size() ? in[i] : nullptr; }

int64_t toInt(double v) {
    if (v >= 9.2e18) return INT64_MAX;
    if (v <= -9.2e18) return INT64_MIN;
    return static_cast<int64_t>(v);
}

std::vector<int64_t> toInts(const OnnxTensor& t) {
    std::vector<int64_t> out(t.data.size());
    for (size_t i = 0; i < t.data.size(); ++i) out[i] = toInt(t.data[i]);
    return out;
}

OnnxTensor makeTensor(OnnxType type, std::vector<int64_t> shape) {
    OnnxTensor t;
    t.type = type;
    t.shape = std::move(shape);
    t.data.assign(t.count(), 0.0);
    return t;
}

// Passo de cada eixo de 'in' alinhado a' direita com 'outShape', 0 onde o
// eixo e' difundido (broadcast).
std::vector<int64_t> alignedStrides(const std::vector<int64_t>& in, const std::vector<int64_t>& outShape) {
    const std::vector<int64_t> own = contiguousStrides(in);
    std::vector<int64_t> s(outShape.size(), 0);
    const size_t offset = outShape.size() - in.size();
    for (size_t i = 0; i < in.size(); ++i) s[offset + i] = in[i] == 1 ? 0 : own[i];
    return s;
}

// Indice no tensor de origem para o elemento linear 'idx' da saida.
int64_t sourceIndex(size_t idx, const std::vector<int64_t>& outShape, const std::vector<int64_t>& srcStrides) {
    int64_t src = 0;
    for (size_t d = outShape.size(); d-- > 0;) {
        const int64_t dim = outShape[d];
        const int64_t c = static_cast<int64_t>(idx % static_cast<size_t>(dim));
        idx /= static_cast<size_t>(dim);
        src += c * srcStrides[d];
    }
    return src;
}

// ---- elemento a elemento ---------------------------------------------------

enum class BinaryOp { Add, Sub, Mul, Div, Pow, Mod, FMod, Min, Max, Less, LessEq, Greater, GreaterEq, Equal, And, Or, Xor };

bool binaryOpFor(const std::string& op, const OnnxNode& node, BinaryOp& out) {
    static const std::map<std::string, BinaryOp> table = {
        {"Add", BinaryOp::Add}, {"Sub", BinaryOp::Sub}, {"Mul", BinaryOp::Mul}, {"Div", BinaryOp::Div},
        {"Pow", BinaryOp::Pow}, {"Min", BinaryOp::Min}, {"Max", BinaryOp::Max}, {"Less", BinaryOp::Less},
        {"LessOrEqual", BinaryOp::LessEq}, {"Greater", BinaryOp::Greater}, {"GreaterOrEqual", BinaryOp::GreaterEq},
        {"Equal", BinaryOp::Equal}, {"And", BinaryOp::And}, {"Or", BinaryOp::Or}, {"Xor", BinaryOp::Xor}};
    if (op == "Mod") {
        out = node.attrInt("fmod", 0) ? BinaryOp::FMod : BinaryOp::Mod;
        return true;
    }
    const auto it = table.find(op);
    if (it == table.end()) return false;
    out = it->second;
    return true;
}

bool isComparison(BinaryOp op) {
    return op == BinaryOp::Less || op == BinaryOp::LessEq || op == BinaryOp::Greater || op == BinaryOp::GreaterEq ||
           op == BinaryOp::Equal || op == BinaryOp::And || op == BinaryOp::Or || op == BinaryOp::Xor;
}

double applyBinary(BinaryOp op, double a, double b, bool integer) {
    switch (op) {
    case BinaryOp::Add: return a + b;
    case BinaryOp::Sub: return a - b;
    case BinaryOp::Mul: return a * b;
    case BinaryOp::Div:
        if (!integer) return a / b;
        return b == 0.0 ? 0.0 : std::trunc(a / b); // divisao inteira trunca
    case BinaryOp::Pow: return std::pow(a, b);
    case BinaryOp::Mod: {
        if (b == 0.0) return 0.0;
        const double r = std::fmod(a, b); // sinal do divisor (como Python)
        return (r != 0.0 && ((r < 0) != (b < 0))) ? r + b : r;
    }
    case BinaryOp::FMod: return b == 0.0 ? 0.0 : std::fmod(a, b);
    case BinaryOp::Min: return std::min(a, b);
    case BinaryOp::Max: return std::max(a, b);
    case BinaryOp::Less: return a < b;
    case BinaryOp::LessEq: return a <= b;
    case BinaryOp::Greater: return a > b;
    case BinaryOp::GreaterEq: return a >= b;
    case BinaryOp::Equal: return a == b;
    case BinaryOp::And: return (a != 0.0) && (b != 0.0);
    case BinaryOp::Or: return (a != 0.0) || (b != 0.0);
    case BinaryOp::Xor: return (a != 0.0) != (b != 0.0);
    }
    return 0.0;
}

bool evalBinary(BinaryOp op, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    const OnnxTensor* b = input(in, 1);
    std::vector<int64_t> shape;
    if (!a || !b || !broadcastShapes(a->shape, b->shape, shape)) {
        error = "formas incompativeis no broadcast";
        return false;
    }
    const bool integer = a->type != OnnxType::Float;
    OnnxTensor r = makeTensor(isComparison(op) ? OnnxType::Bool : a->type, shape);
    const std::vector<int64_t> sa = alignedStrides(a->shape, shape), sb = alignedStrides(b->shape, shape);
    for (size_t i = 0; i < r.data.size(); ++i) {
        const double va = a->data[static_cast<size_t>(sourceIndex(i, shape, sa))];
        const double vb = b->data[static_cast<size_t>(sourceIndex(i, shape, sb))];
        double v = applyBinary(op, va, vb, integer);
        if (r.type == OnnxType::Float) v = static_cast<float>(v);
        r.data[i] = v;
    }
    out.push_back(std::move(r));
    return true;
}

bool applyUnary(const std::string& op, double x, double& y) {
    if (op == "Neg") y = -x;
    else if (op == "Sqrt") y = std::sqrt(x);
    else if (op == "Sin") y = std::sin(x);
    else if (op == "Cos") y = std::cos(x);
    else if (op == "Floor") y = std::floor(x);
    else if (op == "Ceil") y = std::ceil(x);
    else if (op == "Round") y = std::nearbyint(x);
    else if (op == "Abs") y = std::fabs(x);
    else if (op == "Exp") y = std::exp(x);
    else if (op == "Log") y = std::log(x);
    else if (op == "Not") y = x == 0.0;
    else if (op == "Sigmoid") y = 1.0 / (1.0 + std::exp(-x));
    else if (op == "Erf") y = std::erf(x);
    else if (op == "Reciprocal") y = 1.0 / x;
    else if (op == "Relu") y = std::max(x, 0.0);
    else if (op == "Tanh") y = std::tanh(x);
    else if (op == "Identity") y = x;
    else return false;
    return true;
}

bool evalUnary(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    OnnxTensor r = *a;
    if (node.opType == "Not") r.type = OnnxType::Bool;
    for (double& v : r.data) {
        double y = 0.0;
        if (!applyUnary(node.opType, v, y)) { error = "op unaria desconhecida"; return false; }
        v = r.type == OnnxType::Float ? static_cast<float>(y) : y;
    }
    out.push_back(std::move(r));
    return true;
}

bool evalCast(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    OnnxTensor r = *a;
    const int64_t to = node.attrInt("to", 1);
    if (to == 1 || to == 11) {
        r.type = OnnxType::Float;
        for (double& v : r.data) v = static_cast<float>(v);
    } else if (to == 9) {
        r.type = OnnxType::Bool;
        for (double& v : r.data) v = v != 0.0;
    } else if (to == 6 || to == 7 || to == 2 || to == 3) {
        r.type = OnnxType::Int64;
        for (double& v : r.data) v = std::trunc(v);
    } else {
        error = "Cast para tipo nao suportado";
        return false;
    }
    out.push_back(std::move(r));
    return true;
}

bool evalWhere(const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* c = input(in, 0);
    const OnnxTensor* x = input(in, 1);
    const OnnxTensor* y = input(in, 2);
    std::vector<int64_t> s1, shape;
    if (!c || !x || !y || !broadcastShapes(c->shape, x->shape, s1) || !broadcastShapes(s1, y->shape, shape)) {
        error = "Where com formas incompativeis";
        return false;
    }
    OnnxTensor r = makeTensor(x->type, shape);
    const auto sc = alignedStrides(c->shape, shape), sx = alignedStrides(x->shape, shape), sy = alignedStrides(y->shape, shape);
    for (size_t i = 0; i < r.data.size(); ++i) {
        const bool cond = c->data[static_cast<size_t>(sourceIndex(i, shape, sc))] != 0.0;
        r.data[i] = cond ? x->data[static_cast<size_t>(sourceIndex(i, shape, sx))]
                         : y->data[static_cast<size_t>(sourceIndex(i, shape, sy))];
    }
    out.push_back(std::move(r));
    return true;
}

// ---- forma e construcao ----------------------------------------------------

bool evalShape(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    const int64_t rank = static_cast<int64_t>(a->shape.size());
    int64_t start = node.attrInt("start", 0), end = node.attrInt("end", rank);
    if (start < 0) start += rank;
    if (end < 0) end += rank;
    start = std::clamp<int64_t>(start, 0, rank);
    end = std::clamp<int64_t>(end, start, rank);
    OnnxTensor r = makeTensor(OnnxType::Int64, {end - start});
    for (int64_t i = start; i < end; ++i) r.data[static_cast<size_t>(i - start)] = static_cast<double>(a->shape[static_cast<size_t>(i)]);
    out.push_back(std::move(r));
    return true;
}

bool evalConstant(const OnnxNode& node, Outputs& out, std::string& error) {
    if (const OnnxAttribute* v = node.attribute("value")) {
        out.push_back(v->t);
        return true;
    }
    if (const OnnxAttribute* v = node.attribute("value_float")) {
        OnnxTensor t = makeTensor(OnnxType::Float, {});
        t.data[0] = v->f;
        out.push_back(std::move(t));
        return true;
    }
    if (const OnnxAttribute* v = node.attribute("value_int")) {
        OnnxTensor t = makeTensor(OnnxType::Int64, {});
        t.data[0] = static_cast<double>(v->i);
        out.push_back(std::move(t));
        return true;
    }
    if (const OnnxAttribute* v = node.attribute("value_ints")) {
        OnnxTensor t = makeTensor(OnnxType::Int64, {static_cast<int64_t>(v->ints.size())});
        for (size_t i = 0; i < v->ints.size(); ++i) t.data[i] = static_cast<double>(v->ints[i]);
        out.push_back(std::move(t));
        return true;
    }
    if (const OnnxAttribute* v = node.attribute("value_floats")) {
        OnnxTensor t = makeTensor(OnnxType::Float, {static_cast<int64_t>(v->floats.size())});
        for (size_t i = 0; i < v->floats.size(); ++i) t.data[i] = v->floats[i];
        out.push_back(std::move(t));
        return true;
    }
    error = "Constant sem valor suportado";
    return false;
}

bool evalConstantOfShape(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* s = input(in, 0);
    if (!s) { error = "entrada ausente"; return false; }
    OnnxType type = OnnxType::Float;
    double value = 0.0;
    if (const OnnxAttribute* v = node.attribute("value")) {
        type = v->t.type;
        value = v->t.data.empty() ? 0.0 : v->t.data[0];
    }
    OnnxTensor r = makeTensor(type, toInts(*s));
    std::fill(r.data.begin(), r.data.end(), value);
    out.push_back(std::move(r));
    return true;
}

bool evalRange(const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* s = input(in, 0);
    const OnnxTensor* l = input(in, 1);
    const OnnxTensor* d = input(in, 2);
    if (!s || !l || !d || s->data.empty() || l->data.empty() || d->data.empty() || d->data[0] == 0.0) {
        error = "Range invalido";
        return false;
    }
    const double start = s->data[0], limit = l->data[0], delta = d->data[0];
    const int64_t n = std::max<int64_t>(0, static_cast<int64_t>(std::ceil((limit - start) / delta)));
    OnnxTensor r = makeTensor(s->type, {n});
    for (int64_t i = 0; i < n; ++i) r.data[static_cast<size_t>(i)] = start + static_cast<double>(i) * delta;
    out.push_back(std::move(r));
    return true;
}

bool evalReshape(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    const OnnxTensor* s = input(in, 1);
    if (!a || !s) { error = "entrada ausente"; return false; }
    std::vector<int64_t> shape = toInts(*s);
    const bool allowZero = node.attrInt("allowzero", 0) != 0;
    int64_t known = 1;
    int inferred = -1;
    for (size_t i = 0; i < shape.size(); ++i) {
        if (shape[i] == 0 && !allowZero) shape[i] = i < a->shape.size() ? a->shape[i] : 1;
        if (shape[i] == -1) inferred = static_cast<int>(i);
        else known *= shape[i];
    }
    if (inferred >= 0) shape[static_cast<size_t>(inferred)] = known ? static_cast<int64_t>(a->count()) / known : 0;
    OnnxTensor r = *a;
    r.shape = shape;
    if (r.count() != a->count()) { error = "Reshape com contagem diferente"; return false; }
    out.push_back(std::move(r));
    return true;
}

std::vector<int64_t> axesFrom(const OnnxNode& node, const Inputs& in, size_t inputIndex) {
    if (const OnnxTensor* t = input(in, inputIndex)) return toInts(*t);
    return node.attrInts("axes");
}

bool evalUnsqueeze(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    std::vector<int64_t> axes = axesFrom(node, in, 1);
    const size_t rank = a->shape.size() + axes.size();
    for (int64_t& ax : axes) ax = normalizeAxis(ax, rank);
    std::sort(axes.begin(), axes.end());
    std::vector<int64_t> shape = a->shape;
    for (int64_t ax : axes) shape.insert(shape.begin() + ax, 1);
    OnnxTensor r = *a;
    r.shape = shape;
    out.push_back(std::move(r));
    return true;
}

bool evalSqueeze(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    std::vector<int64_t> axes = axesFrom(node, in, 1);
    for (int64_t& ax : axes) ax = normalizeAxis(ax, a->shape.size());
    std::vector<int64_t> shape;
    for (size_t i = 0; i < a->shape.size(); ++i) {
        const bool listed = std::find(axes.begin(), axes.end(), static_cast<int64_t>(i)) != axes.end();
        const bool drop = axes.empty() ? a->shape[i] == 1 : listed;
        if (!drop) shape.push_back(a->shape[i]);
    }
    OnnxTensor r = *a;
    r.shape = shape;
    out.push_back(std::move(r));
    return true;
}

bool evalConcat(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    std::vector<const OnnxTensor*> parts;
    for (const OnnxTensor* t : in)
        if (t && t->count() > 0) parts.push_back(t);
    if (parts.empty()) {
        out.push_back(in.empty() || !in[0] ? OnnxTensor{} : *in[0]);
        return true;
    }
    const size_t rank = parts[0]->shape.size();
    const int64_t axis = normalizeAxis(node.attrInt("axis", 0), rank);
    std::vector<int64_t> shape = parts[0]->shape;
    shape[static_cast<size_t>(axis)] = 0;
    for (const OnnxTensor* t : parts) {
        if (t->shape.size() != rank) { error = "Concat com ranks diferentes"; return false; }
        shape[static_cast<size_t>(axis)] += t->shape[static_cast<size_t>(axis)];
    }
    OnnxTensor r = makeTensor(parts[0]->type, shape);
    size_t outer = 1;
    for (int64_t i = 0; i < axis; ++i) outer *= static_cast<size_t>(shape[static_cast<size_t>(i)]);
    size_t pos = 0;
    const size_t outRow = r.data.size() / std::max<size_t>(outer, 1);
    for (const OnnxTensor* t : parts) {
        const size_t row = t->count() / std::max<size_t>(outer, 1);
        for (size_t o = 0; o < outer; ++o)
            std::copy_n(t->data.begin() + static_cast<std::ptrdiff_t>(o * row), row,
                        r.data.begin() + static_cast<std::ptrdiff_t>(o * outRow + pos));
        pos += row;
    }
    out.push_back(std::move(r));
    return true;
}

bool evalGather(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    const OnnxTensor* idx = input(in, 1);
    if (!a || !idx || a->shape.empty()) { error = "Gather invalido"; return false; }
    const size_t axis = static_cast<size_t>(normalizeAxis(node.attrInt("axis", 0), a->shape.size()));
    std::vector<int64_t> shape(a->shape.begin(), a->shape.begin() + static_cast<std::ptrdiff_t>(axis));
    shape.insert(shape.end(), idx->shape.begin(), idx->shape.end());
    shape.insert(shape.end(), a->shape.begin() + static_cast<std::ptrdiff_t>(axis) + 1, a->shape.end());
    const int64_t dim = a->shape[axis];
    size_t outer = 1, inner = 1;
    for (size_t i = 0; i < axis; ++i) outer *= static_cast<size_t>(a->shape[i]);
    for (size_t i = axis + 1; i < a->shape.size(); ++i) inner *= static_cast<size_t>(a->shape[i]);
    OnnxTensor r = makeTensor(a->type, shape);
    const size_t k = idx->count();
    for (size_t o = 0; o < outer; ++o)
        for (size_t j = 0; j < k; ++j) {
            int64_t ix = toInt(idx->data[j]);
            if (ix < 0) ix += dim;
            if (ix < 0 || ix >= dim) { error = "Gather com indice fora"; return false; }
            for (size_t i = 0; i < inner; ++i)
                r.data[(o * k + j) * inner + i] = a->data[(o * static_cast<size_t>(dim) + static_cast<size_t>(ix)) * inner + i];
        }
    out.push_back(std::move(r));
    return true;
}

bool evalSlice(const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    const OnnxTensor* st = input(in, 1);
    const OnnxTensor* en = input(in, 2);
    if (!a || !st || !en) { error = "Slice invalido"; return false; }
    const size_t rank = a->shape.size();
    std::vector<int64_t> starts = toInts(*st), ends = toInts(*en), axes, steps;
    if (const OnnxTensor* t = input(in, 3)) axes = toInts(*t);
    if (const OnnxTensor* t = input(in, 4)) steps = toInts(*t);
    if (axes.empty()) { axes.resize(starts.size()); std::iota(axes.begin(), axes.end(), 0); }
    if (steps.empty()) steps.assign(starts.size(), 1);
    std::vector<int64_t> begin(rank, 0), step(rank, 1), shape = a->shape;
    for (size_t i = 0; i < axes.size(); ++i) {
        const size_t ax = static_cast<size_t>(normalizeAxis(axes[i], rank));
        const int64_t dim = a->shape[ax], s = steps[i];
        if (s == 0) { error = "Slice com passo 0"; return false; }
        int64_t b = starts[i], e = ends[i];
        if (b < 0) b += dim;
        if (e < 0) e += dim;
        if (s > 0) {
            b = std::clamp<int64_t>(b, 0, dim);
            e = std::clamp<int64_t>(e, 0, dim);
            shape[ax] = e > b ? (e - b + s - 1) / s : 0;
        } else {
            b = std::clamp<int64_t>(b, 0, dim - 1);
            e = std::clamp<int64_t>(e, -1, dim - 1);
            shape[ax] = b > e ? (b - e - s - 1) / (-s) : 0;
        }
        begin[ax] = b;
        step[ax] = s;
    }
    OnnxTensor r = makeTensor(a->type, shape);
    const std::vector<int64_t> src = contiguousStrides(a->shape);
    for (size_t i = 0; i < r.data.size(); ++i) {
        size_t rem = i;
        int64_t off = 0;
        for (size_t d = rank; d-- > 0;) {
            const int64_t c = static_cast<int64_t>(rem % static_cast<size_t>(shape[d]));
            rem /= static_cast<size_t>(shape[d]);
            off += (begin[d] + c * step[d]) * src[d];
        }
        r.data[i] = a->data[static_cast<size_t>(off)];
    }
    out.push_back(std::move(r));
    return true;
}

bool evalTranspose(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    const size_t rank = a->shape.size();
    std::vector<int64_t> perm = node.attrInts("perm");
    if (perm.empty()) for (size_t i = 0; i < rank; ++i) perm.push_back(static_cast<int64_t>(rank - 1 - i));
    std::vector<int64_t> shape(rank), strides(rank);
    const std::vector<int64_t> src = contiguousStrides(a->shape);
    for (size_t i = 0; i < rank; ++i) {
        shape[i] = a->shape[static_cast<size_t>(perm[i])];
        strides[i] = src[static_cast<size_t>(perm[i])];
    }
    OnnxTensor r = makeTensor(a->type, shape);
    for (size_t i = 0; i < r.data.size(); ++i) r.data[i] = a->data[static_cast<size_t>(sourceIndex(i, shape, strides))];
    out.push_back(std::move(r));
    return true;
}

bool evalExpand(const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    const OnnxTensor* s = input(in, 1);
    std::vector<int64_t> shape;
    if (!a || !s || !broadcastShapes(a->shape, toInts(*s), shape)) { error = "Expand invalido"; return false; }
    OnnxTensor r = makeTensor(a->type, shape);
    const auto sa = alignedStrides(a->shape, shape);
    for (size_t i = 0; i < r.data.size(); ++i) r.data[i] = a->data[static_cast<size_t>(sourceIndex(i, shape, sa))];
    out.push_back(std::move(r));
    return true;
}

bool evalSplit(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    const size_t axis = static_cast<size_t>(normalizeAxis(node.attrInt("axis", 0), a->shape.size()));
    std::vector<int64_t> sizes;
    if (const OnnxTensor* t = input(in, 1)) sizes = toInts(*t);
    else sizes = node.attrInts("split");
    const size_t parts = std::max<size_t>(node.outputs.size(), 1);
    if (sizes.empty()) sizes.assign(parts, a->shape[axis] / static_cast<int64_t>(parts));
    int64_t start = 0;
    for (int64_t size : sizes) {
        OnnxTensor st = makeTensor(OnnxType::Int64, {1}), en = makeTensor(OnnxType::Int64, {1}), ax = makeTensor(OnnxType::Int64, {1});
        st.data[0] = static_cast<double>(start);
        en.data[0] = static_cast<double>(start + size);
        ax.data[0] = static_cast<double>(axis);
        if (!evalSlice({a, &st, &en, &ax}, out, error)) return false;
        start += size;
    }
    return true;
}

// ---- reducoes e algebra ----------------------------------------------------

bool evalReduce(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    if (!a) { error = "entrada ausente"; return false; }
    const size_t rank = a->shape.size();
    std::vector<int64_t> axes = axesFrom(node, in, 1);
    const bool keep = node.attrInt("keepdims", 1) != 0;
    if (axes.empty() && node.attrInt("noop_with_empty_axes", 0)) {
        out.push_back(*a);
        return true;
    }
    std::vector<bool> reduced(rank, axes.empty());
    for (int64_t ax : axes) reduced[static_cast<size_t>(normalizeAxis(ax, rank))] = true;
    std::vector<int64_t> keepShape(rank), outShape;
    for (size_t d = 0; d < rank; ++d) {
        keepShape[d] = reduced[d] ? 1 : a->shape[d];
        if (!reduced[d] || keep) outShape.push_back(keepShape[d]);
    }
    const std::string& op = node.opType;
    const bool isMax = op == "ReduceMax", isMin = op == "ReduceMin", isProd = op == "ReduceProd";
    OnnxTensor r = makeTensor(a->type, keepShape);
    const double init = isMax ? -INFINITY : (isMin ? INFINITY : (isProd ? 1.0 : 0.0));
    std::fill(r.data.begin(), r.data.end(), init);
    const std::vector<int64_t> dst = alignedStrides(keepShape, a->shape);
    for (size_t i = 0; i < a->data.size(); ++i) {
        double& acc = r.data[static_cast<size_t>(sourceIndex(i, a->shape, dst))];
        const double v = a->data[i];
        if (isMax) acc = std::max(acc, v);
        else if (isMin) acc = std::min(acc, v);
        else if (isProd) acc *= v;
        else if (op == "ReduceL2" || op == "ReduceSumSquare") acc += v * v;
        else acc += v;
    }
    const double n = static_cast<double>(a->count()) / static_cast<double>(std::max<size_t>(r.count(), 1));
    for (double& v : r.data) {
        if (op == "ReduceMean") v /= n;
        else if (op == "ReduceL2") v = std::sqrt(v);
        if (r.type == OnnxType::Float) v = static_cast<float>(v);
    }
    r.shape = outShape;
    out.push_back(std::move(r));
    return true;
}

bool evalMatMul(const Inputs& in, Outputs& out, std::string& error) {
    const OnnxTensor* a = input(in, 0);
    const OnnxTensor* b = input(in, 1);
    if (!a || !b || a->shape.size() < 2 || b->shape.size() < 2) { error = "MatMul so' com rank >= 2 na CPU"; return false; }
    const int64_t M = a->shape[a->shape.size() - 2], K = a->shape.back(), N = b->shape.back();
    if (b->shape[b->shape.size() - 2] != K) { error = "MatMul com K diferente"; return false; }
    std::vector<int64_t> ba(a->shape.begin(), a->shape.end() - 2), bb(b->shape.begin(), b->shape.end() - 2), batch;
    if (!broadcastShapes(ba, bb, batch)) { error = "MatMul com lotes incompativeis"; return false; }
    std::vector<int64_t> shape = batch;
    shape.push_back(M);
    shape.push_back(N);
    OnnxTensor r = makeTensor(a->type, shape);
    const auto sa = alignedStrides(ba, batch), sb = alignedStrides(bb, batch);
    const size_t nb = shapeCount(batch);
    for (size_t bi = 0; bi < nb; ++bi) {
        const size_t oa = static_cast<size_t>(sourceIndex(bi, batch, sa)) * static_cast<size_t>(M * K);
        const size_t ob = static_cast<size_t>(sourceIndex(bi, batch, sb)) * static_cast<size_t>(K * N);
        for (int64_t i = 0; i < M; ++i)
            for (int64_t j = 0; j < N; ++j) {
                double acc = 0.0;
                for (int64_t k = 0; k < K; ++k)
                    acc += a->data[oa + static_cast<size_t>(i * K + k)] * b->data[ob + static_cast<size_t>(k * N + j)];
                r.data[bi * static_cast<size_t>(M * N) + static_cast<size_t>(i * N + j)] = acc;
            }
    }
    out.push_back(std::move(r));
    return true;
}

// Einsum generico (laco ingenuo sobre todas as letras): pensado pra tabelas
// pequenas montadas a partir da forma, nao pra ativacao.
bool evalEinsum(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    std::string eq = node.attrString("equation", "");
    eq.erase(std::remove(eq.begin(), eq.end(), ' '), eq.end());
    const size_t arrow = eq.find("->");
    if (arrow == std::string::npos || eq.find("...") != std::string::npos) { error = "Einsum sem saida explicita"; return false; }
    std::vector<std::string> terms;
    size_t p = 0;
    const std::string lhs = eq.substr(0, arrow), rhs = eq.substr(arrow + 2);
    while (p <= lhs.size()) {
        const size_t c = lhs.find(',', p);
        terms.push_back(lhs.substr(p, c == std::string::npos ? std::string::npos : c - p));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    if (terms.size() != in.size()) { error = "Einsum com numero de operandos errado"; return false; }
    std::map<char, int64_t> dims;
    for (size_t t = 0; t < terms.size(); ++t) {
        if (!in[t] || in[t]->shape.size() != terms[t].size()) { error = "Einsum com rank errado"; return false; }
        for (size_t i = 0; i < terms[t].size(); ++i) dims[terms[t][i]] = in[t]->shape[i];
    }
    std::vector<char> letters;
    for (const auto& kv : dims) letters.push_back(kv.first);
    std::vector<int64_t> outShape;
    for (char c : rhs) outShape.push_back(dims[c]);
    OnnxTensor r = makeTensor(in[0]->type, outShape);
    std::vector<int64_t> loopShape;
    for (char c : letters) loopShape.push_back(dims[c]);
    const size_t total = shapeCount(loopShape);
    std::vector<int64_t> coord(letters.size());
    for (size_t idx = 0; idx < total; ++idx) {
        size_t rem = idx;
        for (size_t d = letters.size(); d-- > 0;) {
            coord[d] = static_cast<int64_t>(rem % static_cast<size_t>(loopShape[d]));
            rem /= static_cast<size_t>(loopShape[d]);
        }
        double prod = 1.0;
        for (size_t t = 0; t < terms.size(); ++t) {
            int64_t off = 0;
            for (char c : terms[t]) {
                const size_t li = static_cast<size_t>(std::find(letters.begin(), letters.end(), c) - letters.begin());
                off = off * dims[c] + coord[li];
            }
            prod *= in[t]->data[static_cast<size_t>(off)];
        }
        int64_t o = 0;
        for (char c : rhs) {
            const size_t li = static_cast<size_t>(std::find(letters.begin(), letters.end(), c) - letters.begin());
            o = o * dims[c] + coord[li];
        }
        r.data[static_cast<size_t>(o)] += prod;
    }
    if (r.type == OnnxType::Float)
        for (double& v : r.data) v = static_cast<float>(v);
    out.push_back(std::move(r));
    return true;
}

bool isUnaryOp(const std::string& op) {
    static const char* kOps[] = {"Neg", "Sqrt", "Sin", "Cos", "Floor", "Ceil", "Round", "Abs", "Exp", "Log", "Not",
                                 "Sigmoid", "Erf", "Reciprocal", "Relu", "Tanh", "Identity"};
    return std::find(std::begin(kOps), std::end(kOps), op) != std::end(kOps);
}

bool isReduceOp(const std::string& op) {
    return op == "ReduceSum" || op == "ReduceMean" || op == "ReduceMax" || op == "ReduceMin" || op == "ReduceProd" ||
           op == "ReduceL2" || op == "ReduceSumSquare";
}

} // namespace

std::vector<int64_t> contiguousStrides(const std::vector<int64_t>& shape) {
    std::vector<int64_t> s(shape.size(), 1);
    for (size_t i = shape.size(); i-- > 1;) s[i - 1] = s[i] * shape[i];
    return s;
}

bool broadcastShapes(const std::vector<int64_t>& a, const std::vector<int64_t>& b, std::vector<int64_t>& out) {
    const size_t rank = std::max(a.size(), b.size());
    out.assign(rank, 1);
    for (size_t i = 0; i < rank; ++i) {
        const int64_t da = i < rank - a.size() ? 1 : a[i - (rank - a.size())];
        const int64_t db = i < rank - b.size() ? 1 : b[i - (rank - b.size())];
        if (da != db && da != 1 && db != 1) return false;
        out[i] = da == 1 ? db : da;
    }
    return true;
}

int64_t normalizeAxis(int64_t axis, size_t rank) { return axis < 0 ? axis + static_cast<int64_t>(rank) : axis; }

size_t shapeCount(const std::vector<int64_t>& shape) {
    size_t n = 1;
    for (int64_t d : shape) n *= static_cast<size_t>(std::max<int64_t>(d, 0));
    return n;
}

bool evalConstNode(const OnnxNode& node, const Inputs& in, Outputs& out, std::string& error) {
    out.clear();
    const std::string& op = node.opType;
    BinaryOp bop;
    if (binaryOpFor(op, node, bop)) return evalBinary(bop, in, out, error);
    if (isUnaryOp(op)) return evalUnary(node, in, out, error);
    if (isReduceOp(op)) return evalReduce(node, in, out, error);
    if (op == "Cast") return evalCast(node, in, out, error);
    if (op == "Where") return evalWhere(in, out, error);
    if (op == "Shape") return evalShape(node, in, out, error);
    if (op == "Constant") return evalConstant(node, out, error);
    if (op == "ConstantOfShape") return evalConstantOfShape(node, in, out, error);
    if (op == "Range") return evalRange(in, out, error);
    if (op == "Reshape") return evalReshape(node, in, out, error);
    if (op == "Unsqueeze") return evalUnsqueeze(node, in, out, error);
    if (op == "Squeeze") return evalSqueeze(node, in, out, error);
    if (op == "Concat") return evalConcat(node, in, out, error);
    if (op == "Gather") return evalGather(node, in, out, error);
    if (op == "Slice") return evalSlice(in, out, error);
    if (op == "Transpose") return evalTranspose(node, in, out, error);
    if (op == "Expand") return evalExpand(in, out, error);
    if (op == "Split") return evalSplit(node, in, out, error);
    if (op == "MatMul") return evalMatMul(in, out, error);
    if (op == "Einsum") return evalEinsum(node, in, out, error);
    error = "op " + op + " nao suportada na avaliacao de constantes";
    return false;
}

} // namespace eruption
