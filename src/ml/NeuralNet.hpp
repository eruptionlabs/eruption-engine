#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eruption {

// Rede neural generica (ONNX). Nao sabe nada do que a rede faz: entra tensor
// float nomeado, sai tensor float nomeado. Quem da' significado aos canais e'
// o chamador (e a configuracao dele), nao este modulo.
//
// Dois niveis de execucao, nessa ordem:
//   GPU - compute shaders Vulkan proprios (NnVulkan.hpp), qualquer placa
//         Vulkan; a primeira inferencia de cada forma e' conferida contra a
//         CPU quando ela existe, e divergencia desliga a GPU pra essa rede.
//   CPU - ONNX Runtime (quando compilado com ERUPTION_HAVE_ORT).
// Nivel que falha cai pro seguinte sozinho. Sem nenhum dos dois, load()
// devolve nullptr e o chamador usa o caminho dele sem rede.
struct NnTensor {
    std::vector<int64_t> shape;
    std::vector<float> data;
};

enum class NnBackend { None, Gpu, Cpu };

const char* backendName(NnBackend b);

struct NeuralNetOptions {
    int threadsPerRun = 1;  // CPU: threads internas de UMA inferencia
    int maxConcurrent = 0;  // CPU: inferencias simultaneas (0 = nucleos)
};

class NeuralNet {
public:
    static std::shared_ptr<NeuralNet> load(const std::string& path, const NeuralNetOptions& opts = {});
    ~NeuralNet();

    // Thread-safe. outputs recebe um tensor por nome pedido, na mesma ordem.
    // Devolve o nivel que rodou (None = falhou em todos).
    NnBackend run(const std::vector<std::string>& inputNames, const std::vector<const NnTensor*>& inputs,
                  const std::vector<std::string>& outputNames, std::vector<NnTensor>& outputs) const;

    const std::string& path() const { return m_path; }
    uint32_t key() const { return m_key; } // hash dos bytes do arquivo
    std::vector<std::string> inputNames() const;
    std::vector<std::string> outputNames() const;
    // Metadado customizado gravado no proprio .onnx ("" se nao existe). E'
    // por ele que um modelo novo se descreve sozinho (plug and play).
    std::string metadata(const std::string& key) const;
    // Niveis disponiveis neste momento (a GPU pode cair durante o uso).
    bool hasGpu() const;
    bool hasCpu() const;
    std::string gpuName() const;

private:
    NeuralNet() = default;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_path;
    uint32_t m_key = 0;
};

} // namespace eruption
