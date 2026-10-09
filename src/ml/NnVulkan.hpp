#pragma once

#include "OnnxModel.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eruption {

// Inferencia de uma rede ONNX na GPU por compute shaders Vulkan
// (shaders/ml/*.comp). Contexto Vulkan proprio, separado do renderer: pode
// ser usado de qualquer thread (serializado internamente) e fora do motor.
//
// Um programa por forma de entrada (NnProgram) com memoria, descritores e
// command buffer gravados uma vez; os ultimos usados ficam em cache.
//
// create() falha (nullptr + erro) sem GPU Vulkan utilizavel, sem os shaders
// compilados ou com op que o planejador nao suporta - quem chama cai pra CPU.
// ERUPTION_NN_GPU=0 desliga; ERUPTION_NN_GPU_INDEX escolhe o dispositivo.
class NnVulkanExecutor {
public:
    static std::unique_ptr<NnVulkanExecutor> create(std::shared_ptr<const OnnxModel> model, std::string& error);
    ~NnVulkanExecutor();
    NnVulkanExecutor(const NnVulkanExecutor&) = delete;
    NnVulkanExecutor& operator=(const NnVulkanExecutor&) = delete;

    // Thread-safe. 'output' recebe a saida float contigua e 'outputShape'
    // a forma dela.
    bool run(const std::vector<int64_t>& inputShape, const std::vector<float>& input, std::vector<float>& output,
             std::vector<int64_t>& outputShape, std::string& error);

    const std::string& deviceName() const;

    // Tempo de GPU da ultima inferencia (ms), medido por timestamp.
    double lastGpuMs() const;

private:
    NnVulkanExecutor() = default;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace eruption
