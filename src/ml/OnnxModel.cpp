#include "OnnxModel.hpp"

#include <cstdint>
#include <cstring>

namespace eruption {

namespace {

// Tipos de elemento do ONNX (TensorProto.DataType) que o leitor aceita.
constexpr int kTypeFloat = 1, kTypeUint8 = 2, kTypeInt8 = 3, kTypeInt32 = 6, kTypeInt64 = 7, kTypeBool = 9,
              kTypeDouble = 11;

enum WireType { kVarint = 0, kFixed64 = 1, kLengthDelimited = 2, kFixed32 = 5 };

float asFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

double asDouble(uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

// INT64_MAX (comum em Slice) nao cabe exato em double; voltar sem clamp e'
// comportamento indefinido.
int64_t clampToInt64(double v) {
    if (v >= 9.2e18) return INT64_MAX;
    if (v <= -9.2e18) return INT64_MIN;
    return static_cast<int64_t>(v);
}

// Leitura sequencial de uma mensagem protobuf. Erro de formato vira ok=false
// e todas as leituras seguintes devolvem zero.
class ProtoReader {
public:
    ProtoReader(const uint8_t* data, size_t size) : m_p(data), m_end(data + size) {}

    bool ok() const { return m_ok; }
    bool atEnd() const { return !m_ok || m_p >= m_end; }

    bool nextField(uint32_t& field, uint32_t& wire) {
        if (atEnd()) return false;
        const uint64_t key = varint();
        field = static_cast<uint32_t>(key >> 3);
        wire = static_cast<uint32_t>(key & 7);
        return m_ok;
    }

    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (m_p >= m_end) return fail();
            const uint8_t b = *m_p++;
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        return fail();
    }

    uint32_t fixed32() {
        uint32_t v = 0;
        if (!take(&v, 4)) return 0;
        return v;
    }

    uint64_t fixed64() {
        uint64_t v = 0;
        if (!take(&v, 8)) return 0;
        return v;
    }

    ProtoReader message() {
        const uint64_t len = varint();
        if (!m_ok || len > static_cast<uint64_t>(m_end - m_p)) {
            fail();
            return ProtoReader(m_end, 0);
        }
        ProtoReader sub(m_p, static_cast<size_t>(len));
        m_p += len;
        return sub;
    }

    std::string bytes() {
        const ProtoReader sub = message();
        return std::string(reinterpret_cast<const char*>(sub.m_p), static_cast<size_t>(sub.m_end - sub.m_p));
    }

    void skip(uint32_t wire) {
        if (wire == kVarint) varint();
        else if (wire == kFixed64) fixed64();
        else if (wire == kFixed32) fixed32();
        else if (wire == kLengthDelimited) message();
        else fail();
    }

    // Campos escalares repetidos: aceitam a forma "packed" (um bloco so') e a
    // solta (um valor por ocorrencia do campo).
    void repeatedVarint(uint32_t wire, std::vector<double>& out) {
        if (wire != kLengthDelimited) {
            out.push_back(static_cast<double>(static_cast<int64_t>(varint())));
            return;
        }
        ProtoReader packed = message();
        while (!packed.atEnd()) out.push_back(static_cast<double>(static_cast<int64_t>(packed.varint())));
        if (!packed.ok()) fail();
    }

    void repeatedFloat(uint32_t wire, std::vector<double>& out) {
        if (wire != kLengthDelimited) {
            out.push_back(asFloat(fixed32()));
            return;
        }
        ProtoReader packed = message();
        while (!packed.atEnd()) out.push_back(asFloat(packed.fixed32()));
        if (!packed.ok()) fail();
    }

    void repeatedDouble(uint32_t wire, std::vector<double>& out) {
        if (wire != kLengthDelimited) {
            out.push_back(asDouble(fixed64()));
            return;
        }
        ProtoReader packed = message();
        while (!packed.atEnd()) out.push_back(asDouble(packed.fixed64()));
        if (!packed.ok()) fail();
    }

private:
    uint64_t fail() {
        m_ok = false;
        m_p = m_end;
        return 0;
    }

    bool take(void* dst, size_t n) {
        if (static_cast<size_t>(m_end - m_p) < n) {
            fail();
            return false;
        }
        std::memcpy(dst, m_p, n);
        m_p += n;
        return true;
    }

    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_ok = true;
};

bool decodeRaw(const std::string& raw, int dataType, std::vector<double>& out) {
    const size_t elemSize = dataType == kTypeFloat || dataType == kTypeInt32 ? 4
                          : dataType == kTypeInt64 || dataType == kTypeDouble ? 8
                          : 1;
    if (raw.size() % elemSize) return false;
    const size_t n = raw.size() / elemSize;
    out.resize(n);
    const char* p = raw.data();
    for (size_t i = 0; i < n; ++i, p += elemSize) {
        switch (dataType) {
        case kTypeFloat: { float v; std::memcpy(&v, p, 4); out[i] = v; break; }
        case kTypeInt32: { int32_t v; std::memcpy(&v, p, 4); out[i] = v; break; }
        case kTypeInt64: { int64_t v; std::memcpy(&v, p, 8); out[i] = static_cast<double>(v); break; }
        case kTypeDouble: { double v; std::memcpy(&v, p, 8); out[i] = v; break; }
        case kTypeInt8: out[i] = static_cast<int8_t>(*p); break;
        default: out[i] = static_cast<uint8_t>(*p); break; // uint8, bool
        }
    }
    return true;
}

bool mapType(int dataType, OnnxType& out) {
    switch (dataType) {
    case kTypeFloat: case kTypeDouble: out = OnnxType::Float; return true;
    case kTypeInt64: case kTypeInt32: case kTypeInt8: case kTypeUint8: out = OnnxType::Int64; return true;
    case kTypeBool: out = OnnxType::Bool; return true;
    default: return false;
    }
}

bool parseTensor(ProtoReader r, OnnxTensor& t, std::string& name, std::string& error) {
    int dataType = 0;
    std::string raw;
    bool external = false;
    std::vector<double> dims;
    uint32_t field, wire;
    while (r.nextField(field, wire)) {
        switch (field) {
        case 1: r.repeatedVarint(wire, dims); break;
        case 2: dataType = static_cast<int>(r.varint()); break;
        case 4: r.repeatedFloat(wire, t.data); break;
        case 5: case 7: r.repeatedVarint(wire, t.data); break;
        case 8: name = r.bytes(); break;
        case 9: raw = r.bytes(); break;
        case 10: r.repeatedDouble(wire, t.data); break;
        case 13: external = true; r.skip(wire); break;
        default: r.skip(wire); break;
        }
    }
    if (!r.ok()) { error = "tensor malformado"; return false; }
    if (external) { error = "tensor com dados externos nao suportado: " + name; return false; }
    if (!mapType(dataType, t.type)) { error = "tipo de tensor nao suportado (" + std::to_string(dataType) + "): " + name; return false; }
    if (!raw.empty() && !decodeRaw(raw, dataType, t.data)) { error = "raw_data com tamanho invalido: " + name; return false; }
    t.shape.assign(dims.begin(), dims.end());
    if (t.data.size() != t.count()) { error = "tensor com numero de elementos errado: " + name; return false; }
    return true;
}

bool parseAttribute(ProtoReader r, std::string& name, OnnxAttribute& a, std::string& error) {
    uint32_t field, wire;
    int type = 0;
    std::string tensorName;
    std::vector<double> floats, ints;
    while (r.nextField(field, wire)) {
        switch (field) {
        case 1: name = r.bytes(); break;
        case 2: a.f = asFloat(r.fixed32()); break;
        case 3: a.i = static_cast<int64_t>(r.varint()); break;
        case 4: a.s = r.bytes(); break;
        case 5: if (!parseTensor(r.message(), a.t, tensorName, error)) return false; break;
        case 6: error = "grafo aninhado (If/Loop) nao suportado"; return false;
        case 7: r.repeatedFloat(wire, floats); break;
        case 8: r.repeatedVarint(wire, ints); break;
        case 9: a.strings.push_back(r.bytes()); break;
        case 20: type = static_cast<int>(r.varint()); break;
        default: r.skip(wire); break;
        }
    }
    a.floats.assign(floats.begin(), floats.end());
    for (double v : ints) a.ints.push_back(clampToInt64(v));
    // AttributeProto.AttributeType: 1 float, 2 int, 3 string, 4 tensor,
    // 6 floats, 7 ints, 8 strings.
    switch (type) {
    case 1: a.kind = OnnxAttribute::Kind::Float; break;
    case 2: a.kind = OnnxAttribute::Kind::Int; break;
    case 3: a.kind = OnnxAttribute::Kind::String; break;
    case 4: a.kind = OnnxAttribute::Kind::Tensor; break;
    case 6: a.kind = OnnxAttribute::Kind::Floats; break;
    case 7: a.kind = OnnxAttribute::Kind::Ints; break;
    case 8: a.kind = OnnxAttribute::Kind::Strings; break;
    default: a.kind = OnnxAttribute::Kind::None; break;
    }
    if (!r.ok()) { error = "atributo malformado"; return false; }
    return true;
}

bool parseNode(ProtoReader r, OnnxNode& n, std::string& error) {
    uint32_t field, wire;
    while (r.nextField(field, wire)) {
        switch (field) {
        case 1: n.inputs.push_back(r.bytes()); break;
        case 2: n.outputs.push_back(r.bytes()); break;
        case 3: n.name = r.bytes(); break;
        case 4: n.opType = r.bytes(); break;
        case 5: {
            std::string key;
            OnnxAttribute a;
            if (!parseAttribute(r.message(), key, a, error)) return false;
            n.attributes[key] = std::move(a);
            break;
        }
        case 7: n.domain = r.bytes(); break;
        default: r.skip(wire); break;
        }
    }
    if (!r.ok()) { error = "no malformado"; return false; }
    return true;
}

std::string valueInfoName(ProtoReader r) {
    uint32_t field, wire;
    std::string name;
    while (r.nextField(field, wire)) {
        if (field == 1) name = r.bytes();
        else r.skip(wire);
    }
    return name;
}

bool parseGraph(ProtoReader r, OnnxModel& m, std::string& error) {
    std::vector<std::string> declaredInputs;
    uint32_t field, wire;
    while (r.nextField(field, wire)) {
        switch (field) {
        case 1: {
            OnnxNode n;
            if (!parseNode(r.message(), n, error)) return false;
            m.nodes.push_back(std::move(n));
            break;
        }
        case 5: {
            OnnxTensor t;
            std::string name;
            if (!parseTensor(r.message(), t, name, error)) return false;
            m.initializers[name] = std::move(t);
            break;
        }
        case 11: declaredInputs.push_back(valueInfoName(r.message())); break;
        case 12: m.outputs.push_back(valueInfoName(r.message())); break;
        default: r.skip(wire); break;
        }
    }
    if (!r.ok()) { error = "grafo malformado"; return false; }
    // IR antigo lista as constantes tambem como entrada do grafo.
    for (const std::string& in : declaredInputs)
        if (!m.initializers.count(in)) m.inputs.push_back(in);
    return true;
}

void parseMetadataEntry(ProtoReader r, OnnxModel& m) {
    uint32_t field, wire;
    std::string key, value;
    while (r.nextField(field, wire)) {
        if (field == 1) key = r.bytes();
        else if (field == 2) value = r.bytes();
        else r.skip(wire);
    }
    m.metadata[key] = value;
}

void parseOpset(ProtoReader r, OnnxModel& m) {
    uint32_t field, wire;
    std::string domain;
    int64_t version = 0;
    while (r.nextField(field, wire)) {
        if (field == 1) domain = r.bytes();
        else if (field == 2) version = static_cast<int64_t>(r.varint());
        else r.skip(wire);
    }
    if (domain.empty() || domain == "ai.onnx") m.opset = version;
}

} // namespace

size_t OnnxTensor::count() const {
    size_t n = 1;
    for (int64_t d : shape) n *= static_cast<size_t>(d < 0 ? 0 : d);
    return n;
}

const OnnxAttribute* OnnxNode::attribute(const std::string& key) const {
    const auto it = attributes.find(key);
    return it == attributes.end() ? nullptr : &it->second;
}

int64_t OnnxNode::attrInt(const std::string& key, int64_t def) const {
    const OnnxAttribute* a = attribute(key);
    return a ? a->i : def;
}

float OnnxNode::attrFloat(const std::string& key, float def) const {
    const OnnxAttribute* a = attribute(key);
    return a ? a->f : def;
}

std::string OnnxNode::attrString(const std::string& key, const std::string& def) const {
    const OnnxAttribute* a = attribute(key);
    return a ? a->s : def;
}

std::vector<int64_t> OnnxNode::attrInts(const std::string& key) const {
    const OnnxAttribute* a = attribute(key);
    return a ? a->ints : std::vector<int64_t>{};
}

bool parseOnnxModel(const std::vector<char>& bytes, OnnxModel& out, std::string& error) {
    out = OnnxModel{};
    ProtoReader r(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    bool haveGraph = false;
    uint32_t field, wire;
    while (r.nextField(field, wire)) {
        switch (field) {
        case 7:
            if (!parseGraph(r.message(), out, error)) return false;
            haveGraph = true;
            break;
        case 8: parseOpset(r.message(), out); break;
        case 14: parseMetadataEntry(r.message(), out); break;
        default: r.skip(wire); break;
        }
    }
    if (!r.ok()) { error = "arquivo ONNX malformado"; return false; }
    if (!haveGraph) { error = "arquivo sem grafo"; return false; }
    return true;
}

} // namespace eruption
