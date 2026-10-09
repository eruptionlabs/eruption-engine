#include "NnProgram.hpp"

#include "NnConstEval.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace eruption {

namespace {

constexpr size_t kAlignElements = 64; // 256 bytes entre tensores

size_t alignUp(size_t v) { return (v + kAlignElements - 1) / kAlignElements * kAlignElements; }

// Valor de um nome no grafo: constante (CPU) ou tensor na GPU.
struct Value {
    bool dynamic = false;
    OnnxTensor constant;
    NnView view;
};

// Forma alinhada a' direita em 6 dimensoes (as da esquerda viram 1).
NnDims padDims(const std::vector<int64_t>& shape) {
    NnDims d;
    d.fill(1);
    const size_t off = kNnMaxRank - shape.size();
    for (size_t i = 0; i < shape.size(); ++i) d[off + i] = shape[i];
    return d;
}

NnDims padStrides(const std::vector<int64_t>& strides) {
    NnDims s{};
    const size_t off = kNnMaxRank - strides.size();
    for (size_t i = 0; i < strides.size(); ++i) s[off + i] = strides[i];
    return s;
}

// Passos de 'in' dentro da forma 'out' (broadcast = 0), em 6 dimensoes.
NnDims broadcastStrides(const std::vector<int64_t>& in, const std::vector<int64_t>& out) {
    const std::vector<int64_t> own = contiguousStrides(in);
    std::vector<int64_t> s(out.size(), 0);
    const size_t off = out.size() - in.size();
    for (size_t i = 0; i < in.size(); ++i) s[off + i] = in[i] == 1 ? 0 : own[i];
    return padStrides(s);
}

std::vector<int64_t> toInts(const OnnxTensor& t) {
    std::vector<int64_t> v(t.data.size());
    for (size_t i = 0; i < t.data.size(); ++i)
        v[i] = t.data[i] >= 9.2e18 ? INT64_MAX : (t.data[i] <= -9.2e18 ? INT64_MIN : static_cast<int64_t>(t.data[i]));
    return v;
}

bool unaryCodeFor(const std::string& op, NnUnary& code) {
    static const std::map<std::string, NnUnary> table = {
        {"Identity", NnUnary::Identity}, {"Neg", NnUnary::Neg}, {"Sqrt", NnUnary::Sqrt}, {"Sin", NnUnary::Sin},
        {"Cos", NnUnary::Cos}, {"Exp", NnUnary::Exp}, {"Log", NnUnary::Log}, {"Abs", NnUnary::Abs},
        {"Relu", NnUnary::Relu}, {"Sigmoid", NnUnary::Sigmoid}, {"Tanh", NnUnary::Tanh}, {"Erf", NnUnary::Erf},
        {"Selu", NnUnary::Selu}, {"Elu", NnUnary::Elu}, {"LeakyRelu", NnUnary::LeakyRelu},
        {"HardSigmoid", NnUnary::HardSigmoid}, {"Softplus", NnUnary::Softplus},
        {"Reciprocal", NnUnary::Reciprocal}, {"Floor", NnUnary::Floor}, {"Ceil", NnUnary::Ceil},
        {"Clip", NnUnary::Clip}};
    const auto it = table.find(op);
    if (it == table.end()) return false;
    code = it->second;
    return true;
}

bool binaryCodeFor(const std::string& op, NnBinary& code) {
    static const std::map<std::string, NnBinary> table = {
        {"Add", NnBinary::Add}, {"Sub", NnBinary::Sub}, {"Mul", NnBinary::Mul}, {"Div", NnBinary::Div},
        {"Pow", NnBinary::Pow}, {"Min", NnBinary::Min}, {"Max", NnBinary::Max}};
    const auto it = table.find(op);
    if (it == table.end()) return false;
    code = it->second;
    return true;
}

bool reduceCodeFor(const std::string& op, NnReduce& code) {
    static const std::map<std::string, NnReduce> table = {
        {"ReduceSum", NnReduce::Sum}, {"ReduceMean", NnReduce::Mean}, {"ReduceMax", NnReduce::Max},
        {"ReduceMin", NnReduce::Min}, {"ReduceL2", NnReduce::L2}, {"ReduceSumSquare", NnReduce::SumSquare}};
    const auto it = table.find(op);
    if (it == table.end()) return false;
    code = it->second;
    return true;
}

// Monta o programa no por no'. Erros param tudo: o chamador cai pra CPU.
class Planner {
public:
    Planner(const OnnxModel& model, NnProgram& program) : m_model(model), m_prog(program) {}

    bool run(const std::vector<int64_t>& inputShape, std::string& error) {
        if (m_model.inputs.size() != 1 || m_model.outputs.size() != 1) {
            error = "so' redes com uma entrada e uma saida";
            return false;
        }
        Value in;
        in.dynamic = true;
        in.view = newTensor(inputShape);
        m_prog.input = in.view;
        m_values[m_model.inputs[0]] = in;
        for (const auto& kv : m_model.initializers) m_values[kv.first].constant = kv.second;
        for (const OnnxNode& node : m_model.nodes)
            if (!lowerNode(node, error)) {
                error = node.opType + " (" + node.name + "): " + error;
                return false;
            }
        const auto it = m_values.find(m_model.outputs[0]);
        if (it == m_values.end() || !it->second.dynamic) {
            error = "saida da rede nao depende da entrada";
            return false;
        }
        m_prog.output = it->second.view;
        planMemory();
        return true;
    }

private:
    // ---- tensores ----------------------------------------------------------

    NnView newTensor(const std::vector<int64_t>& shape) {
        NnBuffer b;
        b.elements = shapeCount(shape);
        m_prog.buffers.push_back(std::move(b));
        NnView v;
        v.buffer = static_cast<int>(m_prog.buffers.size()) - 1;
        v.shape = shape;
        return v;
    }

    NnView constantTensor(const OnnxTensor& t) {
        NnBuffer b;
        b.constant = true;
        b.elements = t.count();
        b.data.assign(t.data.begin(), t.data.end());
        m_prog.buffers.push_back(std::move(b));
        NnView v;
        v.buffer = static_cast<int>(m_prog.buffers.size()) - 1;
        v.shape = t.shape;
        return v;
    }

    // Entrada de uma op da GPU: tensor dinamico ou constante enviada uma vez.
    bool gpuInput(const std::string& name, NnView& out, std::string& error) {
        const auto it = m_values.find(name);
        if (it == m_values.end()) { error = "valor desconhecido: " + name; return false; }
        if (it->second.dynamic) {
            out = it->second.view;
            return true;
        }
        const auto up = m_uploaded.find(name);
        if (up != m_uploaded.end()) {
            out = up->second;
            return true;
        }
        out = constantTensor(it->second.constant);
        m_uploaded[name] = out;
        return true;
    }

    const OnnxTensor* constInput(const OnnxNode& node, size_t i) const {
        if (i >= node.inputs.size() || node.inputs[i].empty()) return nullptr;
        const auto it = m_values.find(node.inputs[i]);
        if (it == m_values.end() || it->second.dynamic) return nullptr;
        return &it->second.constant;
    }

    bool isDynamic(const std::string& name) const {
        const auto it = m_values.find(name);
        return it != m_values.end() && it->second.dynamic;
    }

    void setDynamic(const std::string& name, const NnView& v) {
        Value val;
        val.dynamic = true;
        val.view = v;
        m_values[name] = val;
    }

    void addOp(NnOp op) {
        const int index = static_cast<int>(m_prog.ops.size());
        for (const NnView& v : op.in) touch(v.buffer, index);
        touch(op.out.buffer, index);
        m_prog.ops.push_back(std::move(op));
    }

    void touch(int buffer, int opIndex) {
        NnBuffer& b = m_prog.buffers[static_cast<size_t>(buffer)];
        b.firstOp = std::min(b.firstOp, opIndex);
        b.lastOp = std::max(b.lastOp, opIndex);
    }

    // ---- despacho por no' ----------------------------------------------------

    bool lowerNode(const OnnxNode& node, std::string& error) {
        bool anyDynamic = false;
        for (const std::string& in : node.inputs)
            if (!in.empty() && isDynamic(in)) anyDynamic = true;
        if (node.opType == "Shape" || node.opType == "Size") return lowerShapeQuery(node, error);
        if (!anyDynamic) return evalOnCpu(node, error);
        return lowerDynamic(node, error);
    }

    bool evalOnCpu(const OnnxNode& node, std::string& error) {
        std::vector<const OnnxTensor*> ins;
        for (size_t i = 0; i < node.inputs.size(); ++i) {
            if (node.inputs[i].empty()) { ins.push_back(nullptr); continue; }
            const auto it = m_values.find(node.inputs[i]);
            if (it == m_values.end()) { error = "valor desconhecido: " + node.inputs[i]; return false; }
            ins.push_back(&it->second.constant);
        }
        std::vector<OnnxTensor> outs;
        if (!evalConstNode(node, ins, outs, error)) return false;
        for (size_t i = 0; i < outs.size() && i < node.outputs.size(); ++i) m_values[node.outputs[i]].constant = std::move(outs[i]);
        return true;
    }

    // Shape/Size so' precisam da forma - que e' conhecida mesmo na GPU.
    bool lowerShapeQuery(const OnnxNode& node, std::string& error) {
        const auto it = m_values.find(node.inputs[0]);
        if (it == m_values.end()) { error = "valor desconhecido"; return false; }
        const std::vector<int64_t> shape = it->second.dynamic ? it->second.view.shape : it->second.constant.shape;
        OnnxTensor t;
        t.type = OnnxType::Int64;
        if (node.opType == "Size") {
            t.data = {static_cast<double>(shapeCount(shape))};
        } else {
            const int64_t rank = static_cast<int64_t>(shape.size());
            int64_t start = node.attrInt("start", 0), end = node.attrInt("end", rank);
            if (start < 0) start += rank;
            if (end < 0) end += rank;
            start = std::clamp<int64_t>(start, 0, rank);
            end = std::clamp<int64_t>(end, start, rank);
            for (int64_t i = start; i < end; ++i) t.data.push_back(static_cast<double>(shape[static_cast<size_t>(i)]));
            t.shape = {end - start};
        }
        m_values[node.outputs[0]].constant = t;
        return true;
    }

    bool lowerDynamic(const OnnxNode& node, std::string& error) {
        const std::string& op = node.opType;
        NnBinary bin;
        NnUnary un;
        NnReduce red;
        if (binaryCodeFor(op, bin)) return lowerBinary(node, bin, error);
        if (unaryCodeFor(op, un)) return lowerUnary(node, un, error);
        if (reduceCodeFor(op, red)) return lowerReduce(node, red, error);
        if (op == "Reshape" || op == "Squeeze" || op == "Unsqueeze" || op == "Flatten") return lowerAlias(node, error);
        if (op == "Slice") return lowerSlice(node, error);
        if (op == "Concat") return lowerConcat(node, error);
        if (op == "Split") return lowerSplit(node, error);
        if (op == "Transpose") return lowerTranspose(node, error);
        if (op == "DepthToSpace") return lowerDepthToSpace(node, error);
        if (op == "Expand") return lowerExpand(node, error);
        if (op == "Gather") return lowerGather(node, error);
        if (op == "Conv") return lowerConv(node, error);
        if (op == "MatMul") return lowerMatMul(node, error);
        if (op == "Gemm") return lowerGemm(node, error);
        if (op == "AveragePool" || op == "MaxPool") return lowerPool(node, error);
        if (op == "GlobalAveragePool") return lowerGlobalAveragePool(node, error);
        if (op == "Resize") return lowerResize(node, error);
        error = "op nao suportada na GPU";
        return false;
    }

    // ---- copias (dados so' mudam de lugar) ---------------------------------

    void addCopy(const NnView& src, const NnDims& srcStrides, const NnView& dst, const NnDims& dstStrides,
                 const std::vector<int64_t>& iterShape) {
        NnOp op;
        op.kind = NnOpKind::Copy;
        op.in = {src};
        op.out = dst;
        op.dims = padDims(iterShape);
        op.strides0 = srcStrides;
        op.stridesOut = dstStrides;
        addOp(std::move(op));
    }

    void copyInto(const NnView& src, const NnView& dst) {
        addCopy(src, padStrides(contiguousStrides(src.shape)), dst, padStrides(contiguousStrides(src.shape)), src.shape);
    }

    bool lowerAlias(const OnnxNode& node, std::string& error) {
        NnView v;
        if (!gpuInput(node.inputs[0], v, error)) return false;
        std::vector<int64_t> shape;
        if (!aliasShape(node, v.shape, shape, error)) return false;
        if (shapeCount(shape) != shapeCount(v.shape)) { error = "contagem muda na mudanca de forma"; return false; }
        v.shape = shape;
        setDynamic(node.outputs[0], v);
        return true;
    }

    bool aliasShape(const OnnxNode& node, const std::vector<int64_t>& in, std::vector<int64_t>& out, std::string& error) {
        const std::string& op = node.opType;
        if (op == "Flatten") {
            const int64_t axis = normalizeAxis(node.attrInt("axis", 1), in.size());
            int64_t a = 1, b = 1;
            for (size_t i = 0; i < in.size(); ++i) (static_cast<int64_t>(i) < axis ? a : b) *= in[i];
            out = {a, b};
            return true;
        }
        std::vector<int64_t> axes;
        if (const OnnxTensor* t = constInput(node, 1)) axes = toInts(*t);
        else axes = node.attrInts("axes");
        if (op == "Reshape") {
            if (axes.empty() && !constInput(node, 1)) { error = "Reshape com forma dinamica"; return false; }
            out = axes;
            const bool allowZero = node.attrInt("allowzero", 0) != 0;
            int64_t known = 1;
            int inferred = -1;
            for (size_t i = 0; i < out.size(); ++i) {
                if (out[i] == 0 && !allowZero) out[i] = i < in.size() ? in[i] : 1;
                if (out[i] == -1) inferred = static_cast<int>(i);
                else known *= out[i];
            }
            if (inferred >= 0) out[static_cast<size_t>(inferred)] = known ? static_cast<int64_t>(shapeCount(in)) / known : 0;
            return true;
        }
        if (op == "Unsqueeze") {
            const size_t rank = in.size() + axes.size();
            for (int64_t& ax : axes) ax = normalizeAxis(ax, rank);
            std::sort(axes.begin(), axes.end());
            out = in;
            for (int64_t ax : axes) out.insert(out.begin() + ax, 1);
            return true;
        }
        for (int64_t& ax : axes) ax = normalizeAxis(ax, in.size()); // Squeeze
        out.clear();
        for (size_t i = 0; i < in.size(); ++i) {
            const bool listed = std::find(axes.begin(), axes.end(), static_cast<int64_t>(i)) != axes.end();
            if (!(axes.empty() ? in[i] == 1 : listed)) out.push_back(in[i]);
        }
        return true;
    }

    bool lowerSlice(const OnnxNode& node, std::string& error) {
        NnView src;
        if (!gpuInput(node.inputs[0], src, error)) return false;
        const OnnxTensor* st = constInput(node, 1);
        const OnnxTensor* en = constInput(node, 2);
        if (!st || !en) { error = "Slice com limites dinamicos"; return false; }
        const size_t rank = src.shape.size();
        std::vector<int64_t> starts = toInts(*st), ends = toInts(*en), axes, steps;
        if (const OnnxTensor* t = constInput(node, 3)) axes = toInts(*t);
        if (const OnnxTensor* t = constInput(node, 4)) steps = toInts(*t);
        if (axes.empty()) { axes.resize(starts.size()); std::iota(axes.begin(), axes.end(), 0); }
        if (steps.empty()) steps.assign(starts.size(), 1);
        const std::vector<int64_t> srcStrides = contiguousStrides(src.shape);
        std::vector<int64_t> shape = src.shape, strides = srcStrides;
        int64_t offset = 0;
        for (size_t i = 0; i < axes.size(); ++i) {
            const size_t ax = static_cast<size_t>(normalizeAxis(axes[i], rank));
            const int64_t dim = src.shape[ax], s = steps[i];
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
            offset += b * srcStrides[ax];
            strides[ax] = srcStrides[ax] * s;
        }
        NnView from = src;
        from.offset = static_cast<size_t>(static_cast<int64_t>(src.offset) + offset);
        const NnView dst = newTensor(shape);
        addCopy(from, padStrides(strides), dst, padStrides(contiguousStrides(shape)), shape);
        setDynamic(node.outputs[0], dst);
        return true;
    }

    bool lowerConcat(const OnnxNode& node, std::string& error) {
        std::vector<NnView> parts;
        for (const std::string& name : node.inputs) {
            NnView v;
            if (!gpuInput(name, v, error)) return false;
            if (shapeCount(v.shape) > 0) parts.push_back(v);
        }
        if (parts.empty()) { error = "Concat vazio"; return false; }
        const size_t rank = parts[0].shape.size();
        const size_t axis = static_cast<size_t>(normalizeAxis(node.attrInt("axis", 0), rank));
        std::vector<int64_t> shape = parts[0].shape;
        shape[axis] = 0;
        for (const NnView& p : parts) {
            if (p.shape.size() != rank) { error = "Concat com ranks diferentes"; return false; }
            shape[axis] += p.shape[axis];
        }
        const NnView dst = newTensor(shape);
        const std::vector<int64_t> dstStrides = contiguousStrides(shape);
        int64_t pos = 0;
        for (const NnView& p : parts) {
            NnView into = dst;
            into.offset = dst.offset + static_cast<size_t>(pos * dstStrides[axis]);
            addCopy(p, padStrides(contiguousStrides(p.shape)), into, padStrides(dstStrides), p.shape);
            pos += p.shape[axis];
        }
        setDynamic(node.outputs[0], dst);
        return true;
    }

    bool lowerSplit(const OnnxNode& node, std::string& error) {
        NnView src;
        if (!gpuInput(node.inputs[0], src, error)) return false;
        const size_t axis = static_cast<size_t>(normalizeAxis(node.attrInt("axis", 0), src.shape.size()));
        std::vector<int64_t> sizes;
        if (const OnnxTensor* t = constInput(node, 1)) sizes = toInts(*t);
        else sizes = node.attrInts("split");
        if (sizes.empty()) sizes.assign(node.outputs.size(), src.shape[axis] / static_cast<int64_t>(node.outputs.size()));
        if (sizes.size() != node.outputs.size()) { error = "Split com tamanhos errados"; return false; }
        const std::vector<int64_t> srcStrides = contiguousStrides(src.shape);
        int64_t pos = 0;
        for (size_t i = 0; i < sizes.size(); ++i) {
            std::vector<int64_t> shape = src.shape;
            shape[axis] = sizes[i];
            NnView from = src;
            from.offset = src.offset + static_cast<size_t>(pos * srcStrides[axis]);
            const NnView dst = newTensor(shape);
            addCopy(from, padStrides(srcStrides), dst, padStrides(contiguousStrides(shape)), shape);
            setDynamic(node.outputs[i], dst);
            pos += sizes[i];
        }
        return true;
    }

    bool lowerTranspose(const OnnxNode& node, std::string& error) {
        NnView src;
        if (!gpuInput(node.inputs[0], src, error)) return false;
        const size_t rank = src.shape.size();
        std::vector<int64_t> perm = node.attrInts("perm");
        if (perm.empty()) for (size_t i = 0; i < rank; ++i) perm.push_back(static_cast<int64_t>(rank - 1 - i));
        const std::vector<int64_t> srcStrides = contiguousStrides(src.shape);
        std::vector<int64_t> shape(rank), strides(rank);
        for (size_t i = 0; i < rank; ++i) {
            shape[i] = src.shape[static_cast<size_t>(perm[i])];
            strides[i] = srcStrides[static_cast<size_t>(perm[i])];
        }
        const NnView dst = newTensor(shape);
        addCopy(src, padStrides(strides), dst, padStrides(contiguousStrides(shape)), shape);
        setDynamic(node.outputs[0], dst);
        return true;
    }

    // out[n, c, h*b+i, w*b+j] <- canal (CRD) c*b*b + i*b + j, ou (DCR)
    // i*b*C + j*C + c. Iteracao em 6D: (N, C, H, b, W, b).
    bool lowerDepthToSpace(const OnnxNode& node, std::string& error) {
        NnView src;
        if (!gpuInput(node.inputs[0], src, error)) return false;
        if (src.shape.size() != 4) { error = "DepthToSpace so' 4D"; return false; }
        const int64_t b = node.attrInt("blocksize", 1);
        const int64_t N = src.shape[0], C = src.shape[1], H = src.shape[2], W = src.shape[3];
        if (C % (b * b)) { error = "DepthToSpace com canais invalidos"; return false; }
        const int64_t Co = C / (b * b), HW = H * W;
        const bool crd = node.attrString("mode", "DCR") == "CRD";
        const std::vector<int64_t> iter = {N, Co, H, b, W, b};
        const std::vector<int64_t> srcStrides = crd ? std::vector<int64_t>{C * HW, b * b * HW, W, b * HW, 1, HW}
                                                    : std::vector<int64_t>{C * HW, HW, W, b * Co * HW, 1, Co * HW};
        const std::vector<int64_t> shape = {N, Co, H * b, W * b};
        const NnView dst = newTensor(shape);
        addCopy(src, padStrides(srcStrides), dst, padStrides(contiguousStrides(iter)), iter);
        setDynamic(node.outputs[0], dst);
        return true;
    }

    bool lowerExpand(const OnnxNode& node, std::string& error) {
        NnView src;
        if (!gpuInput(node.inputs[0], src, error)) return false;
        const OnnxTensor* s = constInput(node, 1);
        std::vector<int64_t> shape;
        if (!s || !broadcastShapes(src.shape, toInts(*s), shape)) { error = "Expand invalido"; return false; }
        const NnView dst = newTensor(shape);
        addCopy(src, broadcastStrides(src.shape, shape), dst, padStrides(contiguousStrides(shape)), shape);
        setDynamic(node.outputs[0], dst);
        return true;
    }

    bool lowerGather(const OnnxNode& node, std::string& error) {
        NnView src;
        if (!gpuInput(node.inputs[0], src, error)) return false;
        const OnnxTensor* idx = constInput(node, 1);
        if (!idx) { error = "Gather com indices dinamicos"; return false; }
        const size_t axis = static_cast<size_t>(normalizeAxis(node.attrInt("axis", 0), src.shape.size()));
        const int64_t dim = src.shape[axis];
        OnnxTensor fixed = *idx;
        fixed.type = OnnxType::Float;
        for (double& v : fixed.data) {
            if (v < 0) v += static_cast<double>(dim);
            if (v < 0 || v >= static_cast<double>(dim)) { error = "Gather com indice fora"; return false; }
        }
        std::vector<int64_t> shape(src.shape.begin(), src.shape.begin() + static_cast<std::ptrdiff_t>(axis));
        shape.insert(shape.end(), idx->shape.begin(), idx->shape.end());
        shape.insert(shape.end(), src.shape.begin() + static_cast<std::ptrdiff_t>(axis) + 1, src.shape.end());
        int64_t outer = 1, inner = 1;
        for (size_t i = 0; i < axis; ++i) outer *= src.shape[i];
        for (size_t i = axis + 1; i < src.shape.size(); ++i) inner *= src.shape[i];
        NnOp op;
        op.kind = NnOpKind::Gather;
        op.in = {src, constantTensor(fixed)};
        op.out = newTensor(shape);
        op.ints = {outer, static_cast<int64_t>(idx->count()), inner, dim};
        setDynamic(node.outputs[0], op.out);
        addOp(std::move(op));
        return true;
    }

    // ---- aritmetica -----------------------------------------------------------

    bool lowerBinary(const OnnxNode& node, NnBinary code, std::string& error) {
        NnView a, b;
        if (node.inputs.size() != 2) { error = "so' 2 operandos"; return false; }
        if (!gpuInput(node.inputs[0], a, error) || !gpuInput(node.inputs[1], b, error)) return false;
        std::vector<int64_t> shape;
        if (!broadcastShapes(a.shape, b.shape, shape) || shape.size() > kNnMaxRank) { error = "broadcast invalido"; return false; }
        NnOp op;
        op.kind = NnOpKind::Binary;
        op.code = static_cast<uint32_t>(code);
        op.in = {a, b};
        op.out = newTensor(shape);
        op.dims = padDims(shape);
        op.strides0 = broadcastStrides(a.shape, shape);
        op.strides1 = broadcastStrides(b.shape, shape);
        setDynamic(node.outputs[0], op.out);
        addOp(std::move(op));
        return true;
    }

    bool lowerUnary(const OnnxNode& node, NnUnary code, std::string& error) {
        NnView a;
        if (!gpuInput(node.inputs[0], a, error)) return false;
        NnOp op;
        op.kind = NnOpKind::Unary;
        op.code = static_cast<uint32_t>(code);
        op.in = {a};
        op.out = newTensor(a.shape);
        switch (code) {
        case NnUnary::Selu:
            op.param0 = node.attrFloat("alpha", 1.67326319217681884765625f);
            op.param1 = node.attrFloat("gamma", 1.05070102214813232421875f);
            break;
        case NnUnary::Elu: op.param0 = node.attrFloat("alpha", 1.0f); break;
        case NnUnary::LeakyRelu: op.param0 = node.attrFloat("alpha", 0.01f); break;
        case NnUnary::HardSigmoid:
            op.param0 = node.attrFloat("alpha", 0.2f);
            op.param1 = node.attrFloat("beta", 0.5f);
            break;
        case NnUnary::Clip: {
            const OnnxTensor* lo = constInput(node, 1);
            const OnnxTensor* hi = constInput(node, 2);
            op.param0 = lo && !lo->data.empty() ? static_cast<float>(lo->data[0]) : node.attrFloat("min", -3.4e38f);
            op.param1 = hi && !hi->data.empty() ? static_cast<float>(hi->data[0]) : node.attrFloat("max", 3.4e38f);
            break;
        }
        default: break;
        }
        setDynamic(node.outputs[0], op.out);
        addOp(std::move(op));
        return true;
    }

    bool lowerReduce(const OnnxNode& node, NnReduce code, std::string& error) {
        NnView a;
        if (!gpuInput(node.inputs[0], a, error)) return false;
        std::vector<int64_t> axes;
        if (const OnnxTensor* t = constInput(node, 1)) axes = toInts(*t);
        else axes = node.attrInts("axes");
        const bool keep = node.attrInt("keepdims", 1) != 0;
        if (axes.empty() && node.attrInt("noop_with_empty_axes", 0)) {
            setDynamic(node.outputs[0], a);
            return true;
        }
        return addReduce(node.outputs[0], a, axes, keep, code);
    }

    bool addReduce(const std::string& outName, const NnView& a, const std::vector<int64_t>& axes, bool keep, NnReduce code) {
        const size_t rank = a.shape.size();
        std::vector<bool> reduced(rank, axes.empty());
        for (int64_t ax : axes) reduced[static_cast<size_t>(normalizeAxis(ax, rank))] = true;
        std::vector<int64_t> shape;
        int64_t mask = 0;
        for (size_t d = 0; d < rank; ++d) {
            if (reduced[d]) mask |= int64_t(1) << (kNnMaxRank - rank + d);
            if (!reduced[d]) shape.push_back(a.shape[d]);
            else if (keep) shape.push_back(1);
        }
        NnOp op;
        op.kind = NnOpKind::Reduce;
        op.code = static_cast<uint32_t>(code);
        op.in = {a};
        op.out = newTensor(shape);
        op.dims = padDims(a.shape);
        op.strides0 = padStrides(contiguousStrides(a.shape));
        op.ints = {mask};
        setDynamic(outName, op.out);
        addOp(std::move(op));
        return true;
    }

    bool lowerGlobalAveragePool(const OnnxNode& node, std::string& error) {
        NnView a;
        if (!gpuInput(node.inputs[0], a, error)) return false;
        if (a.shape.size() != 4) { error = "GlobalAveragePool so' 4D"; return false; }
        return addReduce(node.outputs[0], a, {2, 3}, true, NnReduce::Mean);
    }

    // ---- camadas ----------------------------------------------------------------

    bool lowerConv(const OnnxNode& node, std::string& error) {
        NnView x, w, bias;
        if (!gpuInput(node.inputs[0], x, error) || !gpuInput(node.inputs[1], w, error)) return false;
        const bool hasBias = node.inputs.size() > 2 && !node.inputs[2].empty();
        if (hasBias && !gpuInput(node.inputs[2], bias, error)) return false;
        if (x.shape.size() != 4 || w.shape.size() != 4) { error = "Conv so' 2D"; return false; }
        const int64_t group = node.attrInt("group", 1);
        const int64_t kh = w.shape[2], kw = w.shape[3];
        std::vector<int64_t> strides = node.attrInts("strides"), dil = node.attrInts("dilations"), pads = node.attrInts("pads");
        if (strides.empty()) strides = {1, 1};
        if (dil.empty()) dil = {1, 1};
        if (pads.empty()) pads = {0, 0, 0, 0};
        const std::string autoPad = node.attrString("auto_pad", "NOTSET");
        if (autoPad == "SAME_UPPER" || autoPad == "SAME_LOWER") {
            for (int i = 0; i < 2; ++i) {
                const int64_t in = x.shape[2 + static_cast<size_t>(i)], k = (i ? kw : kh);
                const int64_t outSize = (in + strides[static_cast<size_t>(i)] - 1) / strides[static_cast<size_t>(i)];
                const int64_t total = std::max<int64_t>(0, (outSize - 1) * strides[static_cast<size_t>(i)] + (k - 1) * dil[static_cast<size_t>(i)] + 1 - in);
                const int64_t small = total / 2;
                pads[static_cast<size_t>(i)] = autoPad == "SAME_UPPER" ? small : total - small;
                pads[static_cast<size_t>(i) + 2] = total - pads[static_cast<size_t>(i)];
            }
        } else if (autoPad == "VALID") {
            pads = {0, 0, 0, 0};
        }
        if (x.shape[1] != w.shape[1] * group || w.shape[0] % group) { error = "Conv com canais/grupos invalidos"; return false; }
        const int64_t OH = (x.shape[2] + pads[0] + pads[2] - dil[0] * (kh - 1) - 1) / strides[0] + 1;
        const int64_t OW = (x.shape[3] + pads[1] + pads[3] - dil[1] * (kw - 1) - 1) / strides[1] + 1;
        if (OH <= 0 || OW <= 0) { error = "Conv com saida vazia"; return false; }
        NnOp op;
        op.kind = NnOpKind::Conv2d;
        op.in = {x, w};
        if (hasBias) op.in.push_back(bias);
        op.out = newTensor({x.shape[0], w.shape[0], OH, OW});
        op.ints = {pads[0], pads[1], strides[0], strides[1], dil[0], dil[1], group};
        setDynamic(node.outputs[0], op.out);
        addOp(std::move(op));
        return true;
    }

    bool addMatMul(const NnView& a, const NnView& b, NnView& out, std::string& error) {
        std::vector<int64_t> as = a.shape, bs = b.shape;
        const bool vecA = as.size() == 1, vecB = bs.size() == 1;
        if (vecA) as.insert(as.begin(), 1);
        if (vecB) bs.push_back(1);
        const int64_t M = as[as.size() - 2], K = as.back(), N = bs.back();
        if (bs[bs.size() - 2] != K) { error = "MatMul com K diferente"; return false; }
        std::vector<int64_t> ba(as.begin(), as.end() - 2), bb(bs.begin(), bs.end() - 2), batch;
        if (!broadcastShapes(ba, bb, batch) || batch.size() > 4) { error = "MatMul com lote invalido"; return false; }
        std::vector<int64_t> shape = batch;
        if (!vecA) shape.push_back(M);
        if (!vecB) shape.push_back(N);
        // Passos do lote em matrizes inteiras, alinhados nas 4 primeiras dims.
        NnDims sa{}, sb{}, dims;
        dims.fill(1);
        const std::vector<int64_t> stA = contiguousStrides(ba), stB = contiguousStrides(bb);
        for (size_t i = 0; i < batch.size(); ++i) {
            const size_t d = 4 - batch.size() + i;
            dims[d] = batch[i];
            const size_t ia = i + ba.size() - batch.size(), ib = i + bb.size() - batch.size();
            if (i >= batch.size() - ba.size() && ba[ia] != 1) sa[d] = stA[ia] * M * K;
            if (i >= batch.size() - bb.size() && bb[ib] != 1) sb[d] = stB[ib] * K * N;
        }
        NnOp op;
        op.kind = NnOpKind::MatMul;
        op.in = {a, b};
        op.out = newTensor(shape);
        op.dims = dims;
        op.strides0 = sa;
        op.strides1 = sb;
        op.ints = {M, N, K};
        out = op.out;
        addOp(std::move(op));
        return true;
    }

    bool lowerMatMul(const OnnxNode& node, std::string& error) {
        NnView a, b, out;
        if (!gpuInput(node.inputs[0], a, error) || !gpuInput(node.inputs[1], b, error)) return false;
        if (!addMatMul(a, b, out, error)) return false;
        setDynamic(node.outputs[0], out);
        return true;
    }

    NnView transposed2d(const NnView& v) {
        const std::vector<int64_t> shape = {v.shape[1], v.shape[0]};
        const NnView dst = newTensor(shape);
        addCopy(v, padStrides({1, v.shape[1]}), dst, padStrides(contiguousStrides(shape)), shape);
        return dst;
    }

    // Y = alpha * A' * B' + beta * C. B constante (o caso comum) e' transposto
    // e escalado na CPU; o resto vira copia + MatMul + soma.
    bool lowerGemm(const OnnxNode& node, std::string& error) {
        NnView a, b;
        if (!gpuInput(node.inputs[0], a, error)) return false;
        if (a.shape.size() != 2) { error = "Gemm so' 2D"; return false; }
        if (node.attrInt("transA", 0)) a = transposed2d(a);
        const float alpha = node.attrFloat("alpha", 1.0f), beta = node.attrFloat("beta", 1.0f);
        const bool transB = node.attrInt("transB", 0) != 0;
        if (const OnnxTensor* cb = constInput(node, 1)) {
            OnnxTensor fixed = *cb;
            if (transB) {
                fixed.shape = {cb->shape[1], cb->shape[0]};
                for (int64_t i = 0; i < cb->shape[0]; ++i)
                    for (int64_t j = 0; j < cb->shape[1]; ++j)
                        fixed.data[static_cast<size_t>(j * cb->shape[0] + i)] = cb->data[static_cast<size_t>(i * cb->shape[1] + j)];
            }
            for (double& v : fixed.data) v *= alpha;
            b = constantTensor(fixed);
        } else {
            if (!gpuInput(node.inputs[1], b, error)) return false;
            if (transB) b = transposed2d(b);
            if (alpha != 1.0f) { error = "Gemm com alpha e B dinamico"; return false; }
        }
        NnView y;
        if (!addMatMul(a, b, y, error)) return false;
        if (node.inputs.size() > 2 && !node.inputs[2].empty()) {
            NnView c;
            if (const OnnxTensor* cc = constInput(node, 2)) {
                OnnxTensor fixed = *cc;
                for (double& v : fixed.data) v *= beta;
                c = constantTensor(fixed);
            } else {
                if (!gpuInput(node.inputs[2], c, error)) return false;
                if (beta != 1.0f) { error = "Gemm com beta e C dinamico"; return false; }
            }
            std::vector<int64_t> shape;
            if (!broadcastShapes(y.shape, c.shape, shape) || shape != y.shape) { error = "Gemm com C invalido"; return false; }
            NnOp op;
            op.kind = NnOpKind::Binary;
            op.code = static_cast<uint32_t>(NnBinary::Add);
            op.in = {y, c};
            op.out = newTensor(shape);
            op.dims = padDims(shape);
            op.strides0 = broadcastStrides(y.shape, shape);
            op.strides1 = broadcastStrides(c.shape, shape);
            y = op.out;
            addOp(std::move(op));
        }
        setDynamic(node.outputs[0], y);
        return true;
    }

    bool lowerPool(const OnnxNode& node, std::string& error) {
        NnView x;
        if (!gpuInput(node.inputs[0], x, error)) return false;
        if (x.shape.size() != 4) { error = "Pool so' 2D"; return false; }
        if (node.attrString("auto_pad", "NOTSET") != "NOTSET") { error = "Pool com auto_pad"; return false; }
        const std::vector<int64_t> k = node.attrInts("kernel_shape");
        std::vector<int64_t> s = node.attrInts("strides"), p = node.attrInts("pads"), d = node.attrInts("dilations");
        if (k.size() != 2) { error = "Pool sem kernel 2D"; return false; }
        if (s.empty()) s = {1, 1};
        if (p.empty()) p = {0, 0, 0, 0};
        if (!d.empty() && (d[0] != 1 || d[1] != 1)) { error = "Pool com dilatacao"; return false; }
        const bool ceilMode = node.attrInt("ceil_mode", 0) != 0;
        int64_t outHW[2];
        for (int i = 0; i < 2; ++i) {
            const double span = static_cast<double>(x.shape[2 + static_cast<size_t>(i)] + p[static_cast<size_t>(i)] + p[static_cast<size_t>(i) + 2] - k[static_cast<size_t>(i)]) /
                                static_cast<double>(s[static_cast<size_t>(i)]);
            outHW[i] = static_cast<int64_t>(ceilMode ? std::ceil(span) : std::floor(span)) + 1;
        }
        NnOp op;
        op.kind = NnOpKind::Pool2d;
        op.code = static_cast<uint32_t>(node.opType == "MaxPool" ? NnPool::Max : NnPool::Average);
        op.in = {x};
        op.out = newTensor({x.shape[0], x.shape[1], outHW[0], outHW[1]});
        op.ints = {k[0], k[1], s[0], s[1], p[0], p[1], node.attrInt("count_include_pad", 0)};
        setDynamic(node.outputs[0], op.out);
        addOp(std::move(op));
        return true;
    }

    bool lowerResize(const OnnxNode& node, std::string& error) {
        NnView x;
        if (!gpuInput(node.inputs[0], x, error)) return false;
        if (x.shape.size() != 4) { error = "Resize so' 4D"; return false; }
        const std::string mode = node.attrString("mode", "nearest");
        if (mode != "nearest" && mode != "linear") { error = "Resize modo " + mode; return false; }
        double scale[2] = {1.0, 1.0};
        int64_t outHW[2] = {x.shape[2], x.shape[3]};
        const OnnxTensor* sizes = constInput(node, 3);
        const OnnxTensor* scales = constInput(node, 2);
        if (sizes && sizes->count() == 4) {
            if (sizes->data[0] != static_cast<double>(x.shape[0]) || sizes->data[1] != static_cast<double>(x.shape[1])) { error = "Resize em N/C"; return false; }
            for (int i = 0; i < 2; ++i) {
                outHW[i] = static_cast<int64_t>(sizes->data[2 + static_cast<size_t>(i)]);
                scale[i] = static_cast<double>(outHW[i]) / static_cast<double>(x.shape[2 + static_cast<size_t>(i)]);
            }
        } else if (scales && scales->count() == 4) {
            if (scales->data[0] != 1.0 || scales->data[1] != 1.0) { error = "Resize em N/C"; return false; }
            for (int i = 0; i < 2; ++i) {
                scale[i] = scales->data[2 + static_cast<size_t>(i)];
                outHW[i] = static_cast<int64_t>(std::floor(static_cast<double>(x.shape[2 + static_cast<size_t>(i)]) * scale[i]));
            }
        } else {
            error = "Resize sem escala/tamanho constante";
            return false;
        }
        static const std::map<std::string, NnResizeCoord> coords = {
            {"half_pixel", NnResizeCoord::HalfPixel}, {"pytorch_half_pixel", NnResizeCoord::PytorchHalfPixel},
            {"align_corners", NnResizeCoord::AlignCorners}, {"asymmetric", NnResizeCoord::Asymmetric}};
        static const std::map<std::string, NnResizeNearest> nearests = {
            {"round_prefer_floor", NnResizeNearest::RoundPreferFloor}, {"round_prefer_ceil", NnResizeNearest::RoundPreferCeil},
            {"floor", NnResizeNearest::Floor}, {"ceil", NnResizeNearest::Ceil}};
        const auto c = coords.find(node.attrString("coordinate_transformation_mode", "half_pixel"));
        const auto n = nearests.find(node.attrString("nearest_mode", "round_prefer_floor"));
        if (c == coords.end() || n == nearests.end()) { error = "Resize com modo de coordenada nao suportado"; return false; }
        NnOp op;
        op.kind = NnOpKind::Resize;
        op.code = mode == "linear" ? 1u : 0u;
        op.in = {x};
        op.out = newTensor({x.shape[0], x.shape[1], outHW[0], outHW[1]});
        op.ints = {static_cast<int64_t>(c->second), static_cast<int64_t>(n->second)};
        op.param0 = static_cast<float>(scale[0]);
        op.param1 = static_cast<float>(scale[1]);
        setDynamic(node.outputs[0], op.out);
        addOp(std::move(op));
        return true;
    }

    // ---- memoria ------------------------------------------------------------

    // Constantes no inicio; ativacoes reaproveitam espaco de quem ja' morreu
    // (melhor encaixe numa lista de blocos livres).
    void planMemory() {
        std::vector<NnBuffer>& bufs = m_prog.buffers;
        bufs[static_cast<size_t>(m_prog.input.buffer)].firstOp = -1;
        bufs[static_cast<size_t>(m_prog.output.buffer)].lastOp = INT_MAX;
        size_t top = 0;
        for (NnBuffer& b : bufs)
            if (b.constant) {
                b.offset = top;
                top = alignUp(top + b.elements);
            }
        m_prog.constantElements = top;

        std::vector<std::pair<int, size_t>> order; // (primeiro uso, buffer)
        for (size_t i = 0; i < bufs.size(); ++i)
            if (!bufs[i].constant && bufs[i].lastOp >= bufs[i].firstOp) order.emplace_back(bufs[i].firstOp, i);
        std::sort(order.begin(), order.end());

        FreeList free(top);
        std::multimap<int, size_t> live; // ultimo uso -> buffer
        for (const auto& entry : order) {
            const size_t idx = entry.second;
            NnBuffer& b = bufs[idx];
            while (!live.empty() && live.begin()->first < b.firstOp) {
                const NnBuffer& dead = bufs[live.begin()->second];
                free.release(dead.offset, alignUp(dead.elements));
                live.erase(live.begin());
            }
            b.offset = free.acquire(alignUp(std::max<size_t>(b.elements, 1)));
            live.emplace(b.lastOp, idx);
        }
        m_prog.arenaElements = free.highWater();
    }

    class FreeList {
    public:
        explicit FreeList(size_t base) : m_end(base) {}

        size_t acquire(size_t size) {
            auto best = m_blocks.end();
            for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it)
                if (it->second >= size && (best == m_blocks.end() || it->second < best->second)) best = it;
            if (best == m_blocks.end()) {
                const size_t at = m_end;
                m_end += size;
                return at;
            }
            const size_t at = best->first, left = best->second - size;
            m_blocks.erase(best);
            if (left) m_blocks[at + size] = left;
            return at;
        }

        void release(size_t offset, size_t size) {
            auto it = m_blocks.emplace(offset, size).first;
            auto next = std::next(it);
            if (next != m_blocks.end() && it->first + it->second == next->first) {
                it->second += next->second;
                m_blocks.erase(next);
            }
            if (it != m_blocks.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second == it->first) {
                    prev->second += it->second;
                    m_blocks.erase(it);
                }
            }
        }

        size_t highWater() const { return m_end; }

    private:
        std::map<size_t, size_t> m_blocks; // offset -> tamanho
        size_t m_end;
    };

    const OnnxModel& m_model;
    NnProgram& m_prog;
    std::map<std::string, Value> m_values;
    std::map<std::string, NnView> m_uploaded;
};

} // namespace

size_t viewCount(const NnView& v) { return shapeCount(v.shape); }

bool buildNnProgram(const OnnxModel& model, const std::vector<int64_t>& inputShape, NnProgram& out, std::string& error) {
    out = NnProgram{};
    Planner planner(model, out);
    return planner.run(inputShape, error);
}

} // namespace eruption
