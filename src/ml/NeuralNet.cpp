#include "NeuralNet.hpp"

#include "NnVulkan.hpp"
#include "OnnxModel.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>

#ifdef ERUPTION_HAVE_ORT
#include <onnxruntime_cxx_api.h>
#endif

namespace eruption {

namespace {

uint32_t hashBytes(const std::vector<char>& bytes) {
    uint32_t h = 2166136261u; // FNV-1a
    for (char c : bytes) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool envFlag(const char* name, bool def) {
    const char* e = std::getenv(name);
    return (e && *e) ? std::atoi(e) != 0 : def;
}

// Limite de inferencias simultaneas na CPU. Cada uma segura a memoria de
// ativacao inteira da rede; sem isso um pool de N threads estoura a RAM.
class ConcurrencyLimit {
public:
    explicit ConcurrencyLimit(int slots) : m_free(std::max(1, slots)) {}

    void acquire() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_free == 0) m_cv.wait(lock);
        --m_free;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_free;
        }
        m_cv.notify_one();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    int m_free;
};

class ScopedSlot {
public:
    explicit ScopedSlot(ConcurrencyLimit& limit) : m_limit(limit) { m_limit.acquire(); }
    ~ScopedSlot() { m_limit.release(); }
    ScopedSlot(const ScopedSlot&) = delete;
    ScopedSlot& operator=(const ScopedSlot&) = delete;

private:
    ConcurrencyLimit& m_limit;
};

int defaultConcurrency() { return static_cast<int>(std::max(1u, std::thread::hardware_concurrency())); }

// GPU confere com a CPU se a maior diferenca fica abaixo de 0,1% da maior
// magnitude da saida (as duas em float32; so' a ordem das somas muda).
bool outputsAgree(const std::vector<float>& ref, const std::vector<float>& got, double& maxDiff) {
    if (ref.size() != got.size()) return false;
    double maxRef = 0.0;
    maxDiff = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        if (!std::isfinite(got[i])) return false;
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(ref[i] - got[i])));
        maxRef = std::max(maxRef, static_cast<double>(std::fabs(ref[i])));
    }
    return maxDiff <= 1e-3 * std::max(maxRef, 1.0);
}

#ifdef ERUPTION_HAVE_ORT
Ort::Env& ortEnv() {
    static Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "eruption"};
    return env;
}

Ort::SessionOptions sessionOptions(const NeuralNetOptions& opts) {
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(std::max(1, opts.threadsPerRun));
    so.SetInterOpNumThreads(1);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // Sem arena: a memoria de ativacao volta pro sistema ao fim de cada
    // inferencia (a CPU aqui e' conferencia e reserva, nao o caminho quente).
    so.DisableCpuMemArena();
    return so;
}

NnTensor toTensor(const Ort::Value& v) {
    const Ort::TensorTypeAndShapeInfo info = v.GetTensorTypeAndShapeInfo();
    const float* p = v.GetTensorData<float>();
    NnTensor t;
    t.shape = info.GetShape();
    t.data.assign(p, p + info.GetElementCount());
    return t;
}
#endif

} // namespace

const char* backendName(NnBackend b) {
    switch (b) {
    case NnBackend::Gpu: return "GPU";
    case NnBackend::Cpu: return "CPU";
    case NnBackend::None: break;
    }
    return "nenhum";
}

struct NeuralNet::Impl {
    std::shared_ptr<OnnxModel> model;
    std::vector<char> bytes;      // ate' os niveis subirem
    NeuralNetOptions options;
    mutable std::once_flag backendsOnce;
    std::unique_ptr<NnVulkanExecutor> gpu;
    mutable std::atomic<bool> gpuEnabled{false};
    bool validateGpu = true;
    mutable std::mutex validatedMutex;
    mutable std::set<std::vector<int64_t>> validatedShapes;
    // Uma conferencia por vez: varias threads pegando o mesmo tamanho novo
    // juntas rodavam uma CPU cada (0,8 GB a mais de pico no bake).
    mutable std::mutex validationRunMutex;
#ifdef ERUPTION_HAVE_ORT
    std::unique_ptr<Ort::Session> session;
#endif
    std::unique_ptr<ConcurrencyLimit> cpuLimit;

    // GPU e CPU sobem na PRIMEIRA inferencia, nao no load: com o cache de
    // texturas quente a rede nem roda, e ai' nao se paga contexto Vulkan nem
    // sessao do ONNX Runtime.
    void ensureBackends(const std::string& path) const {
        std::call_once(backendsOnce, &Impl::createBackends, const_cast<Impl*>(this), path);
    }

    void createBackends(const std::string& path) {
        std::string error;
        gpu = NnVulkanExecutor::create(model, error);
        gpuEnabled = gpu != nullptr;
        if (!gpu) std::printf("[NN] %s sem GPU: %s\n", path.c_str(), error.c_str());
#ifdef ERUPTION_HAVE_ORT
        try {
            session = std::make_unique<Ort::Session>(ortEnv(), bytes.data(), bytes.size(), sessionOptions(options));
        } catch (const std::exception& e) {
            std::printf("[NN] %s sem CPU: %s\n", path.c_str(), e.what());
        }
#endif
        bytes.clear();
        bytes.shrink_to_fit();
        std::printf("[NN] %s: GPU %s, CPU %s\n", path.c_str(), gpu ? gpu->deviceName().c_str() : "indisponivel",
                    cpuAvailable() ? "ONNX Runtime" : "indisponivel");
    }

    bool cpuAvailable() const {
#ifdef ERUPTION_HAVE_ORT
        return session != nullptr;
#else
        return false;
#endif
    }

    bool runCpu(const std::vector<std::string>& inNames, const std::vector<const NnTensor*>& inputs,
                const std::vector<std::string>& outNames, std::vector<NnTensor>& outputs, const std::string& path) const {
#ifdef ERUPTION_HAVE_ORT
        if (!session) return false;
        const ScopedSlot slot(*cpuLimit);
        try {
            const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::vector<Ort::Value> in;
            std::vector<const char*> inPtrs, outPtrs;
            for (size_t i = 0; i < inputs.size(); ++i) {
                // ORT pede ponteiro nao-const mas nao escreve na entrada.
                float* data = const_cast<float*>(inputs[i]->data.data());
                in.push_back(Ort::Value::CreateTensor<float>(mem, data, inputs[i]->data.size(), inputs[i]->shape.data(),
                                                             inputs[i]->shape.size()));
                inPtrs.push_back(inNames[i].c_str());
            }
            for (const std::string& n : outNames) outPtrs.push_back(n.c_str());
            const std::vector<Ort::Value> res = session->Run(Ort::RunOptions{nullptr}, inPtrs.data(), in.data(), in.size(),
                                                             outPtrs.data(), outPtrs.size());
            outputs.clear();
            for (const Ort::Value& v : res) {
                if (!v.IsTensor()) return false;
                outputs.push_back(toTensor(v));
            }
            return true;
        } catch (const std::exception& e) {
            std::printf("[NN] inferencia CPU falhou (%s): %s\n", path.c_str(), e.what());
            return false;
        }
#else
        (void)inNames; (void)inputs; (void)outNames; (void)outputs; (void)path;
        return false;
#endif
    }

    // A GPU executa redes de uma entrada e uma saida (as do proprio grafo).
    bool gpuCanRun(const std::vector<std::string>& inNames, const std::vector<std::string>& outNames) const {
        return gpuEnabled.load() && inNames.size() == 1 && outNames.size() == 1 && model->inputs.size() == 1 &&
               model->outputs.size() == 1 && inNames[0] == model->inputs[0] && outNames[0] == model->outputs[0];
    }

    void disableGpu(const std::string& why, const std::string& path) const {
        if (gpuEnabled.exchange(false)) std::printf("[NN] GPU desligada para %s: %s - segue na CPU\n", path.c_str(), why.c_str());
    }

    bool needsValidation(const std::vector<int64_t>& shape) const {
        if (!validateGpu || !cpuAvailable()) return false;
        std::lock_guard<std::mutex> lock(validatedMutex);
        return !validatedShapes.count(shape);
    }

    void markValidated(const std::vector<int64_t>& shape) const {
        std::lock_guard<std::mutex> lock(validatedMutex);
        validatedShapes.insert(shape);
    }

    // Primeira inferencia de cada forma: a GPU tem que bater com a CPU.
    // Divergencia desliga a GPU pra esta rede. Sem CPU pra comparar (ou se a
    // CPU falhar), a saida da GPU e' aceita.
    bool validateAgainstCpu(const std::vector<std::string>& inNames, const std::vector<const NnTensor*>& inputs,
                            const std::vector<std::string>& outNames, const NnTensor& gpuOut, const std::string& path) const {
        const std::lock_guard<std::mutex> lock(validationRunMutex);
        if (!needsValidation(inputs[0]->shape)) return gpuEnabled.load();
        std::vector<NnTensor> cpuOut;
        if (!runCpu(inNames, inputs, outNames, cpuOut, path)) return true;
        double diff = 0.0;
        if (outputsAgree(cpuOut[0].data, gpuOut.data, diff)) {
            markValidated(inputs[0]->shape);
            return true;
        }
        disableGpu("diverge da CPU (diferenca " + std::to_string(diff) + ")", path);
        return false;
    }
};

std::shared_ptr<NeuralNet> NeuralNet::load(const std::string& path, const NeuralNetOptions& opts) {
    const std::vector<char> bytes = readFile(path);
    if (bytes.empty()) return nullptr;
    auto model = std::make_shared<OnnxModel>();
    std::string error;
    if (!parseOnnxModel(bytes, *model, error)) {
        std::printf("[NN] %s invalido: %s\n", path.c_str(), error.c_str());
        return nullptr;
    }
    std::shared_ptr<NeuralNet> net(new NeuralNet());
    net->m_impl = std::make_unique<Impl>();
    Impl& im = *net->m_impl;
    im.model = model;
    im.validateGpu = envFlag("ERUPTION_NN_GPU_VALIDATE", true);
    im.cpuLimit = std::make_unique<ConcurrencyLimit>(opts.maxConcurrent > 0 ? opts.maxConcurrent : defaultConcurrency());
    net->m_key = hashBytes(bytes);
    net->m_path = path;

    im.bytes = bytes;
    im.options = opts;
    return net;
}

NeuralNet::~NeuralNet() = default;

std::vector<std::string> NeuralNet::inputNames() const { return m_impl->model->inputs; }

std::vector<std::string> NeuralNet::outputNames() const { return m_impl->model->outputs; }

std::string NeuralNet::metadata(const std::string& key) const {
    const auto it = m_impl->model->metadata.find(key);
    return it == m_impl->model->metadata.end() ? std::string() : it->second;
}

bool NeuralNet::hasGpu() const {
    m_impl->ensureBackends(m_path);
    return m_impl->gpuEnabled.load();
}

bool NeuralNet::hasCpu() const {
    m_impl->ensureBackends(m_path);
    return m_impl->cpuAvailable();
}

std::string NeuralNet::gpuName() const {
    m_impl->ensureBackends(m_path);
    return m_impl->gpu ? m_impl->gpu->deviceName() : std::string();
}

NnBackend NeuralNet::run(const std::vector<std::string>& inputNames, const std::vector<const NnTensor*>& inputs,
                         const std::vector<std::string>& outputNames, std::vector<NnTensor>& outputs) const {
    const Impl& im = *m_impl;
    if (inputNames.size() != inputs.size() || outputNames.empty()) return NnBackend::None;
    im.ensureBackends(m_path);

    if (im.gpuCanRun(inputNames, outputNames)) {
        NnTensor gpuOut;
        std::string error;
        if (im.gpu->run(inputs[0]->shape, inputs[0]->data, gpuOut.data, gpuOut.shape, error)) {
            if (!im.needsValidation(inputs[0]->shape) ||
                im.validateAgainstCpu(inputNames, inputs, outputNames, gpuOut, m_path)) {
                outputs.assign(1, std::move(gpuOut));
                return NnBackend::Gpu;
            }
        }
        im.disableGpu(error, m_path);
    }
    if (im.runCpu(inputNames, inputs, outputNames, outputs, m_path)) return NnBackend::Cpu;
    return NnBackend::None;
}

} // namespace eruption
