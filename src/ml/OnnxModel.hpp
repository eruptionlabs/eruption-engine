#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace eruption {

// Leitor minimo do formato ONNX (protobuf), sem dependencia externa. Le so'
// o que a inferencia precisa: nos, atributos, constantes, entradas/saidas e
// metadados. Grafos aninhados (If/Loop) e dados externos nao sao suportados.

enum class OnnxType { Float, Int64, Bool };

// Valores guardados em double: cobre float32 e os inteiros de forma/indice
// (exatos ate' 2^53) com um tipo so'.
struct OnnxTensor {
    OnnxType type = OnnxType::Float;
    std::vector<int64_t> shape;
    std::vector<double> data;

    size_t count() const;
};

struct OnnxAttribute {
    enum class Kind { None, Float, Int, String, Tensor, Floats, Ints, Strings };
    Kind kind = Kind::None;
    float f = 0.0f;
    int64_t i = 0;
    std::string s;
    OnnxTensor t;
    std::vector<float> floats;
    std::vector<int64_t> ints;
    std::vector<std::string> strings;
};

struct OnnxNode {
    std::string opType, domain, name;
    std::vector<std::string> inputs, outputs; // nome vazio = entrada opcional ausente
    std::map<std::string, OnnxAttribute> attributes;

    const OnnxAttribute* attribute(const std::string& key) const;
    int64_t attrInt(const std::string& key, int64_t def) const;
    float attrFloat(const std::string& key, float def) const;
    std::string attrString(const std::string& key, const std::string& def) const;
    std::vector<int64_t> attrInts(const std::string& key) const;
};

struct OnnxModel {
    std::vector<OnnxNode> nodes;
    std::map<std::string, OnnxTensor> initializers;
    std::vector<std::string> inputs;  // entradas reais (sem as constantes)
    std::vector<std::string> outputs;
    std::map<std::string, std::string> metadata;
    int64_t opset = 0;
};

// false = arquivo invalido ou com recurso nao suportado (erro em 'error').
bool parseOnnxModel(const std::vector<char>& bytes, OnnxModel& out, std::string& error);

} // namespace eruption
